#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/tableref/pivotref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/common/types/value_map.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/planner/tableref/bound_subqueryref.hpp"
#include "duckdb/planner/tableref/bound_pivotref.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/main/client_config.hpp"
namespace duckdb {
static void ConstructPivots(PivotRef &ref, idx_t pivot_idx, vector<unique_ptr<ParsedExpression>> &pivot_expressions, unique_ptr<ParsedExpression> current_expr = nullptr, const string &current_name = string()) {
 auto &pivot = ref.pivots[pivot_idx];
 bool last_pivot = pivot_idx + 1 == ref.pivots.size();
 for (auto &entry : pivot.entries) {
  unique_ptr<ParsedExpression> expr = current_expr ? current_expr->Copy() : nullptr;
  string name = entry.alias;
  D_ASSERT(entry.values.size() == pivot.pivot_expressions.size());
  for (idx_t v = 0; v < entry.values.size(); v++) {
   auto &value = entry.values[v];
   auto column_ref = pivot.pivot_expressions[v]->Copy();
   auto constant_value = make_uniq<ConstantExpression>(value);
   auto comp_expr = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_NOT_DISTINCT_FROM,
                                                    std::move(column_ref), std::move(constant_value));
   if (expr) {
    expr = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(expr),
                                            std::move(comp_expr));
   } else {
    expr = std::move(comp_expr);
   }
   if (entry.alias.empty()) {
    if (name.empty()) {
     name = value.ToString();
    } else {
     name += "_" + value.ToString();
    }
   }
  }
  if (!current_name.empty()) {
   name = current_name + "_" + name;
  }
  if (last_pivot) {
   for (auto &aggr : ref.aggregates) {
    auto copy = aggr->Copy();
    auto &function = (FunctionExpression &)*copy;
    function.filter = expr->Copy();
    if (ref.aggregates.size() > 1 || !function.alias.empty()) {
     function.alias = name + "_" + function.GetName();
    } else {
     function.alias = name;
    }
    pivot_expressions.push_back(std::move(copy));
   }
  } else {
   ConstructPivots(ref, pivot_idx + 1, pivot_expressions, std::move(expr), name);
  }
 }
}
static void ConstructPivots(PivotRef &ref, vector<PivotValueElement> &pivot_values, idx_t pivot_idx = 0, const PivotValueElement &current_value = PivotValueElement()) {
 auto &pivot = ref.pivots[pivot_idx];
 bool last_pivot = pivot_idx + 1 == ref.pivots.size();
 for (auto &entry : pivot.entries) {
  PivotValueElement new_value = current_value;
  string name = entry.alias;
  D_ASSERT(entry.values.size() == pivot.pivot_expressions.size());
  for (idx_t v = 0; v < entry.values.size(); v++) {
   auto &value = entry.values[v];
   new_value.values.push_back(value);
   if (entry.alias.empty()) {
    if (name.empty()) {
     name = value.ToString();
    } else {
     name += "_" + value.ToString();
    }
   }
  }
  if (!current_value.name.empty()) {
   new_value.name = current_value.name + "_" + name;
  } else {
   new_value.name = std::move(name);
  }
  if (last_pivot) {
   pivot_values.push_back(std::move(new_value));
  } else {
   ConstructPivots(ref, pivot_values, pivot_idx + 1, std::move(new_value));
  }
 }
}
static void ExtractPivotExpressions(ParsedExpression &expr, case_insensitive_set_t &handled_columns) {
 if (expr.type == ExpressionType::COLUMN_REF) {
  auto &child_colref = (ColumnRefExpression &)expr;
  if (child_colref.IsQualified()) {
   throw BinderException("PIVOT expression cannot contain qualified columns");
  }
  handled_columns.insert(child_colref.GetColumnName());
 }
 ParsedExpressionIterator::EnumerateChildren(
     expr, [&](ParsedExpression &child) { ExtractPivotExpressions(child, handled_columns); });
}
struct PivotBindState {
 vector<string> internal_group_names;
 vector<string> group_names;
 vector<string> aggregate_names;
 vector<string> internal_aggregate_names;
};
static unique_ptr<SelectNode> PivotInitialAggregate(PivotBindState &bind_state, PivotRef &ref, vector<unique_ptr<ParsedExpression>> all_columns, const case_insensitive_set_t &handled_columns) {
 auto subquery_stage1 = make_unique<SelectNode>();
 subquery_stage1->from_table = std::move(ref.source);
 if (ref.groups.empty()) {
  for (auto &entry : all_columns) {
   if (entry->type != ExpressionType::COLUMN_REF) {
    throw InternalException("Unexpected child of pivot source - not a ColumnRef");
   }
   auto &columnref = (ColumnRefExpression &)*entry;
   if (handled_columns.find(columnref.GetColumnName()) == handled_columns.end()) {
    subquery_stage1->groups.group_expressions.push_back(
        make_unique<ConstantExpression>(Value::INTEGER(subquery_stage1->select_list.size() + 1)));
    subquery_stage1->select_list.push_back(make_unique<ColumnRefExpression>(columnref.GetColumnName()));
   }
  }
 } else {
  for (auto &row : ref.groups) {
   subquery_stage1->groups.group_expressions.push_back(
       make_unique<ConstantExpression>(Value::INTEGER(subquery_stage1->select_list.size() + 1)));
   subquery_stage1->select_list.push_back(make_unique<ColumnRefExpression>(row));
  }
 }
 idx_t group_count = 0;
 for (auto &expr : subquery_stage1->select_list) {
  bind_state.group_names.push_back(expr->GetName());
  if (expr->alias.empty()) {
   expr->alias = "__internal_pivot_group" + std::to_string(++group_count);
  }
  bind_state.internal_group_names.push_back(expr->alias);
 }
 idx_t pivot_count = 0;
 for (auto &pivot_column : ref.pivots) {
  for (auto &pivot_expr : pivot_column.pivot_expressions) {
   if (pivot_expr->alias.empty()) {
    pivot_expr->alias = "__internal_pivot_ref" + std::to_string(++pivot_count);
   }
   auto pivot_alias = pivot_expr->alias;
   subquery_stage1->groups.group_expressions.push_back(
       make_unique<ConstantExpression>(Value::INTEGER(subquery_stage1->select_list.size() + 1)));
   subquery_stage1->select_list.push_back(std::move(pivot_expr));
   pivot_expr = make_unique<ColumnRefExpression>(std::move(pivot_alias));
  }
 }
 idx_t aggregate_count = 0;
 for (auto &aggregate : ref.aggregates) {
  auto aggregate_alias = "__internal_pivot_aggregate" + std::to_string(++aggregate_count);
  bind_state.aggregate_names.push_back(aggregate->alias);
  bind_state.internal_aggregate_names.push_back(aggregate_alias);
  aggregate->alias = std::move(aggregate_alias);
  subquery_stage1->select_list.push_back(std::move(aggregate));
 }
 return subquery_stage1;
}
static unique_ptr<SelectNode> PivotListAggregate(PivotBindState &bind_state, PivotRef &ref, unique_ptr<SelectNode> subquery_stage1) {
 auto subquery_stage2 = make_unique<SelectNode>();
 auto subquery_select = make_unique<SelectStatement>();
 subquery_select->node = std::move(subquery_stage1);
 auto subquery_ref = make_unique<SubqueryRef>(std::move(subquery_select));
 for (idx_t gr = 0; gr < bind_state.internal_group_names.size(); gr++) {
  subquery_stage2->groups.group_expressions.push_back(
      make_unique<ConstantExpression>(Value::INTEGER(subquery_stage2->select_list.size() + 1)));
  auto group_reference = make_unique<ColumnRefExpression>(bind_state.internal_group_names[gr]);
  group_reference->alias = bind_state.internal_group_names[gr];
  subquery_stage2->select_list.push_back(std::move(group_reference));
 }
 for (idx_t aggr = 0; aggr < bind_state.internal_aggregate_names.size(); aggr++) {
  auto colref = make_unique<ColumnRefExpression>(bind_state.internal_aggregate_names[aggr]);
  vector<unique_ptr<ParsedExpression>> list_children;
  list_children.push_back(std::move(colref));
  auto aggregate = make_unique<FunctionExpression>("list", std::move(list_children));
  aggregate->alias = bind_state.internal_aggregate_names[aggr];
  subquery_stage2->select_list.push_back(std::move(aggregate));
 }
 auto pivot_name = "__internal_pivot_name";
 unique_ptr<ParsedExpression> expr;
 for (auto &pivot : ref.pivots) {
  for (auto &pivot_expr : pivot.pivot_expressions) {
   auto cast = make_unique<CastExpression>(LogicalType::VARCHAR, std::move(pivot_expr));
   vector<unique_ptr<ParsedExpression>> coalesce_children;
   coalesce_children.push_back(std::move(cast));
   coalesce_children.push_back(make_unique<ConstantExpression>(Value("NULL")));
   auto coalesce =
       make_unique<OperatorExpression>(ExpressionType::OPERATOR_COALESCE, std::move(coalesce_children));
   if (!expr) {
    expr = std::move(coalesce);
   } else {
    vector<unique_ptr<ParsedExpression>> concat_children;
    concat_children.push_back(std::move(expr));
    concat_children.push_back(make_unique<ConstantExpression>(Value("_")));
    concat_children.push_back(std::move(coalesce));
    auto concat = make_unique<FunctionExpression>("concat", std::move(concat_children));
    expr = std::move(concat);
   }
  }
 }
 vector<unique_ptr<ParsedExpression>> list_children;
 list_children.push_back(std::move(expr));
 auto aggregate = make_unique<FunctionExpression>("list", std::move(list_children));
 aggregate->alias = pivot_name;
 subquery_stage2->select_list.push_back(std::move(aggregate));
 subquery_stage2->from_table = std::move(subquery_ref);
 return subquery_stage2;
}
static unique_ptr<SelectNode> PivotFinalOperator(PivotBindState &bind_state, PivotRef &ref, unique_ptr<SelectNode> subquery, vector<PivotValueElement> pivot_values) {
 auto final_pivot_operator = make_unique<SelectNode>();
 auto subquery_select = make_unique<SelectStatement>();
 subquery_select->node = std::move(subquery);
 auto subquery_ref = make_unique<SubqueryRef>(std::move(subquery_select));
 auto bound_pivot = make_unique<PivotRef>();
 bound_pivot->bound_pivot_values = std::move(pivot_values);
 bound_pivot->bound_group_names = std::move(bind_state.group_names);
 bound_pivot->bound_aggregate_names = std::move(bind_state.aggregate_names);
 bound_pivot->source = std::move(subquery_ref);
 final_pivot_operator->select_list.push_back(make_unique<StarExpression>());
 final_pivot_operator->from_table = std::move(bound_pivot);
 return final_pivot_operator;
}
void ExtractPivotAggregates(BoundTableRef &node, vector<unique_ptr<Expression>> &aggregates) {
 if (node.type != TableReferenceType::SUBQUERY) {
  throw InternalException("Pivot - Expected a subquery");
 }
 auto &subq = (BoundSubqueryRef &)node;
 if (subq.subquery->type != QueryNodeType::SELECT_NODE) {
  throw InternalException("Pivot - Expected a select node");
 }
 auto &select = (BoundSelectNode &)*subq.subquery;
 if (select.from_table->type != TableReferenceType::SUBQUERY) {
  throw InternalException("Pivot - Expected another subquery");
 }
 auto &subq2 = (BoundSubqueryRef &)*select.from_table;
 if (subq2.subquery->type != QueryNodeType::SELECT_NODE) {
  throw InternalException("Pivot - Expected another select node");
 }
 auto &select2 = (BoundSelectNode &)*subq2.subquery;
 for (auto &aggr : select2.aggregates) {
  aggregates.push_back(aggr->Copy());
 }
}
unique_ptr<BoundTableRef> Binder::BindBoundPivot(PivotRef &ref) {
 auto result = make_unique<BoundPivotRef>();
 result->bind_index = GenerateTableIndex();
 result->child_binder = Binder::CreateBinder(context, this);
 result->child = result->child_binder->Bind(*ref.source);
 auto &aggregates = result->bound_pivot.aggregates;
 ExtractPivotAggregates(*result->child, aggregates);
 if (aggregates.size() != ref.bound_aggregate_names.size()) {
  throw InternalException("Pivot - aggregate count mismatch");
 }
 vector<string> child_names;
 vector<LogicalType> child_types;
 result->child_binder->bind_context.GetTypesAndNames(child_names, child_types);
 vector<string> names;
 vector<LogicalType> types;
 for (idx_t i = 0; i < ref.bound_group_names.size(); i++) {
  names.push_back(ref.bound_group_names[i]);
  types.push_back(child_types[i]);
 }
 for (auto &pivot_value : ref.bound_pivot_values) {
  for (idx_t aggr_idx = 0; aggr_idx < ref.bound_aggregate_names.size(); aggr_idx++) {
   auto &aggr = aggregates[aggr_idx];
   auto &aggr_name = ref.bound_aggregate_names[aggr_idx];
   auto name = pivot_value.name;
   if (aggregates.size() > 1 || !aggr_name.empty()) {
    name += "_" + (aggr_name.empty() ? aggr->GetName() : aggr_name);
   }
   string pivot_str;
   for (auto &value : pivot_value.values) {
    auto str = value.ToString();
    if (pivot_str.empty()) {
     pivot_str = std::move(str);
    } else {
     pivot_str += "_" + str;
    }
   }
   result->bound_pivot.pivot_values.push_back(std::move(pivot_str));
   names.push_back(std::move(name));
   types.push_back(aggr->return_type);
  }
 }
 result->bound_pivot.group_count = ref.bound_group_names.size();
 result->bound_pivot.types = types;
 auto subquery_alias = ref.alias.empty() ? "__unnamed_pivot" : ref.alias;
 bind_context.AddGenericBinding(result->bind_index, subquery_alias, names, types);
 MoveCorrelatedExpressions(*result->child_binder);
 return result;
}
unique_ptr<SelectNode> Binder::BindPivot(PivotRef &ref, vector<unique_ptr<ParsedExpression>> all_columns) {
 case_insensitive_set_t handled_columns;
 for (auto &aggr : ref.aggregates) {
  if (aggr->type != ExpressionType::FUNCTION) {
   throw BinderException(FormatError(*aggr, "Pivot expression must be an aggregate"));
  }
  if (aggr->HasSubquery()) {
   throw BinderException(FormatError(*aggr, "Pivot expression cannot contain subqueries"));
  }
  if (aggr->IsWindow()) {
   throw BinderException(FormatError(*aggr, "Pivot expression cannot contain window functions"));
  }
  ExtractPivotExpressions(*aggr, handled_columns);
 }
 value_set_t pivots;
 idx_t total_pivots = 1;
 for (auto &pivot : ref.pivots) {
  if (!pivot.pivot_enum.empty()) {
   auto type = Catalog::GetType(context, INVALID_CATALOG, INVALID_SCHEMA, pivot.pivot_enum);
   if (type.id() != LogicalTypeId::ENUM) {
    throw BinderException(
        FormatError(ref, StringUtil::Format("Pivot must reference an ENUM type: \"%s\" is of type \"%s\"",
                                            pivot.pivot_enum, type.ToString())));
   }
   auto enum_size = EnumType::GetSize(type);
   for (idx_t i = 0; i < enum_size; i++) {
    auto enum_value = EnumType::GetValue(Value::ENUM(i, type));
    PivotColumnEntry entry;
    entry.values.emplace_back(enum_value);
    entry.alias = std::move(enum_value);
    pivot.entries.push_back(std::move(entry));
   }
  }
  total_pivots *= pivot.entries.size();
  for (auto &pivot_name : pivot.pivot_expressions) {
   ExtractPivotExpressions(*pivot_name, handled_columns);
  }
  value_set_t pivots;
  for (auto &entry : pivot.entries) {
   D_ASSERT(!entry.star_expr);
   Value val;
   if (entry.values.size() == 1) {
    val = entry.values[0];
   } else {
    val = Value::LIST(LogicalType::VARCHAR, entry.values);
   }
   if (pivots.find(val) != pivots.end()) {
    throw BinderException(FormatError(
        ref, StringUtil::Format("The value \"%s\" was specified multiple times in the IN clause",
                                val.ToString())));
   }
   if (entry.values.size() != pivot.pivot_expressions.size()) {
    throw ParserException("PIVOT IN list - inconsistent amount of rows - expected %d but got %d",
                          pivot.pivot_expressions.size(), entry.values.size());
   }
   pivots.insert(val);
  }
 }
 auto pivot_limit = ClientConfig::GetConfig(context).pivot_limit;
 if (total_pivots >= pivot_limit) {
  throw BinderException("Pivot column limit of %llu exceeded. Use SET pivot_limit=X to increase the limit.",
                        ClientConfig::GetConfig(context).pivot_limit);
 }
 vector<PivotValueElement> pivot_values;
 ConstructPivots(ref, pivot_values);
 PivotBindState bind_state;
 auto subquery_stage1 = PivotInitialAggregate(bind_state, ref, std::move(all_columns), handled_columns);
 auto subquery_stage2 = PivotListAggregate(bind_state, ref, std::move(subquery_stage1));
 auto pivot_node = PivotFinalOperator(bind_state, ref, std::move(subquery_stage2), std::move(pivot_values));
 return pivot_node;
}
unique_ptr<SelectNode> Binder::BindPivot(PivotRef &ref, vector<unique_ptr<ParsedExpression>> all_columns) {
 const static idx_t PIVOT_EXPRESSION_LIMIT = 10000;
 case_insensitive_set_t handled_columns;
 for (auto &aggr : ref.aggregates) {
  if (aggr->type != ExpressionType::FUNCTION) {
   throw BinderException(FormatError(*aggr, "Pivot expression must be an aggregate"));
  }
  if (aggr->HasSubquery()) {
   throw BinderException(FormatError(*aggr, "Pivot expression cannot contain subqueries"));
  }
  if (aggr->IsWindow()) {
   throw BinderException(FormatError(*aggr, "Pivot expression cannot contain window functions"));
  }
  ExtractPivotExpressions(*aggr, handled_columns);
 }
 value_set_t pivots;
 auto select_node = make_uniq<SelectNode>();
 idx_t total_pivots = 1;
 for (auto &pivot : ref.pivots) {
  if (!pivot.pivot_enum.empty()) {
   auto type = Catalog::GetType(context, INVALID_CATALOG, INVALID_SCHEMA, pivot.pivot_enum);
   if (type.id() != LogicalTypeId::ENUM) {
    throw BinderException(
        FormatError(ref, StringUtil::Format("Pivot must reference an ENUM type: \"%s\" is of type \"%s\"",
                                            pivot.pivot_enum, type.ToString())));
   }
   auto enum_size = EnumType::GetSize(type);
   for (idx_t i = 0; i < enum_size; i++) {
    auto enum_value = EnumType::GetValue(Value::ENUM(i, type));
    PivotColumnEntry entry;
    entry.values.emplace_back(enum_value);
    entry.alias = std::move(enum_value);
    pivot.entries.push_back(std::move(entry));
   }
  }
  total_pivots *= pivot.entries.size();
  for (auto &pivot_name : pivot.pivot_expressions) {
   ExtractPivotExpressions(*pivot_name, handled_columns);
  }
  value_set_t pivots;
  for (auto &entry : pivot.entries) {
   D_ASSERT(!entry.star_expr);
   Value val;
   if (entry.values.size() == 1) {
    val = entry.values[0];
   } else {
    val = Value::LIST(LogicalType::VARCHAR, entry.values);
   }
   if (pivots.find(val) != pivots.end()) {
    throw BinderException(FormatError(
        ref, StringUtil::Format("The value \"%s\" was specified multiple times in the IN clause",
                                val.ToString())));
   }
   if (entry.values.size() != pivot.pivot_expressions.size()) {
    throw ParserException("PIVOT IN list - inconsistent amount of rows - expected %d but got %d",
                          pivot.pivot_expressions.size(), entry.values.size());
   }
   pivots.insert(val);
  }
 }
 if (total_pivots >= PIVOT_EXPRESSION_LIMIT) {
  throw BinderException("Pivot column limit of %llu exceeded", PIVOT_EXPRESSION_LIMIT);
 }
 vector<unique_ptr<ParsedExpression>> pivot_expressions;
 ConstructPivots(ref, 0, pivot_expressions);
 if (ref.groups.empty()) {
  for (auto &entry : all_columns) {
   if (entry->type != ExpressionType::COLUMN_REF) {
    throw InternalException("Unexpected child of pivot source - not a ColumnRef");
   }
   auto &columnref = (ColumnRefExpression &)*entry;
   if (handled_columns.find(columnref.GetColumnName()) == handled_columns.end()) {
    select_node->groups.group_expressions.push_back(
        make_uniq<ConstantExpression>(Value::INTEGER(select_node->select_list.size() + 1)));
    select_node->select_list.push_back(std::move(entry));
   }
  }
 } else {
  for (auto &row : ref.groups) {
   select_node->groups.group_expressions.push_back(
       make_uniq<ConstantExpression>(Value::INTEGER(select_node->select_list.size() + 1)));
   select_node->select_list.push_back(make_uniq<ColumnRefExpression>(row));
  }
 }
 for (auto &pivot_expr : pivot_expressions) {
  select_node->select_list.push_back(std::move(pivot_expr));
 }
 return select_node;
}
unique_ptr<SelectNode> Binder::BindUnpivot(Binder &child_binder, PivotRef &ref, vector<unique_ptr<ParsedExpression>> all_columns, unique_ptr<ParsedExpression> &where_clause) {
 D_ASSERT(ref.groups.empty());
 D_ASSERT(ref.pivots.size() == 1);
 unique_ptr<ParsedExpression> expr;
 auto select_node = make_uniq<SelectNode>();
 auto &unpivot = ref.pivots[0];
 vector<PivotColumnEntry> new_entries;
 for (auto &entry : unpivot.entries) {
  if (entry.star_expr) {
   D_ASSERT(entry.values.empty());
   vector<unique_ptr<ParsedExpression>> star_columns;
   child_binder.ExpandStarExpression(std::move(entry.star_expr), star_columns);
   for (auto &col : star_columns) {
    if (col->type != ExpressionType::COLUMN_REF) {
     throw InternalException("Unexpected child of unpivot star - not a ColumnRef");
    }
    auto &columnref = (ColumnRefExpression &)*col;
    PivotColumnEntry new_entry;
    new_entry.values.emplace_back(columnref.GetColumnName());
    new_entry.alias = columnref.GetColumnName();
    new_entries.push_back(std::move(new_entry));
   }
  } else {
   new_entries.push_back(std::move(entry));
  }
 }
 unpivot.entries = std::move(new_entries);
 case_insensitive_set_t handled_columns;
 case_insensitive_map_t<string> name_map;
 for (auto &entry : unpivot.entries) {
  for (auto &value : entry.values) {
   handled_columns.insert(value.ToString());
  }
 }
 for (auto &col_expr : all_columns) {
  if (col_expr->type != ExpressionType::COLUMN_REF) {
   throw InternalException("Unexpected child of pivot source - not a ColumnRef");
  }
  auto &columnref = (ColumnRefExpression &)*col_expr;
  auto &column_name = columnref.GetColumnName();
  auto entry = handled_columns.find(column_name);
  if (entry == handled_columns.end()) {
   select_node->select_list.push_back(std::move(col_expr));
  } else {
   name_map[column_name] = column_name;
   handled_columns.erase(entry);
  }
 }
 if (!handled_columns.empty()) {
  for (auto &entry : handled_columns) {
   throw BinderException("Column \"%s\" referenced in UNPIVOT but no matching entry was found in the table",
                         entry);
  }
 }
 vector<Value> unpivot_names;
 for (auto &entry : unpivot.entries) {
  string generated_name;
  for (auto &val : entry.values) {
   auto name_entry = name_map.find(val.ToString());
   if (name_entry == name_map.end()) {
    throw InternalException("Unpivot - could not find column name in name map");
   }
   if (!generated_name.empty()) {
    generated_name += "_";
   }
   generated_name += name_entry->second;
  }
  unpivot_names.emplace_back(!entry.alias.empty() ? entry.alias : generated_name);
 }
 vector<vector<unique_ptr<ParsedExpression>>> unpivot_expressions;
 for (idx_t v_idx = 0; v_idx < unpivot.entries[0].values.size(); v_idx++) {
  vector<unique_ptr<ParsedExpression>> expressions;
  expressions.reserve(unpivot.entries.size());
  for (auto &entry : unpivot.entries) {
   expressions.push_back(make_uniq<ColumnRefExpression>(entry.values[v_idx].ToString()));
  }
  unpivot_expressions.push_back(std::move(expressions));
 }
 auto unpivot_list = Value::LIST(LogicalType::VARCHAR, std::move(unpivot_names));
 auto unpivot_name_expr = make_uniq<ConstantExpression>(std::move(unpivot_list));
 vector<unique_ptr<ParsedExpression>> unnest_name_children;
 unnest_name_children.push_back(std::move(unpivot_name_expr));
 auto unnest_name_expr = make_uniq<FunctionExpression>("unnest", std::move(unnest_name_children));
 unnest_name_expr->alias = unpivot.unpivot_names[0];
 select_node->select_list.push_back(std::move(unnest_name_expr));
 if (ref.unpivot_names.size() != unpivot_expressions.size()) {
  throw BinderException("UNPIVOT name count mismatch - got %d names but %d expressions", ref.unpivot_names.size(),
                        unpivot_expressions.size());
 }
 for (idx_t i = 0; i < unpivot_expressions.size(); i++) {
  auto list_expr = make_uniq<FunctionExpression>("list_value", std::move(unpivot_expressions[i]));
  vector<unique_ptr<ParsedExpression>> unnest_val_children;
  unnest_val_children.push_back(std::move(list_expr));
  auto unnest_val_expr = make_uniq<FunctionExpression>("unnest", std::move(unnest_val_children));
  auto unnest_name = i < ref.column_name_alias.size() ? ref.column_name_alias[i] : ref.unpivot_names[i];
  unnest_val_expr->alias = unnest_name;
  select_node->select_list.push_back(std::move(unnest_val_expr));
  if (!ref.include_nulls) {
   auto colref = make_uniq<ColumnRefExpression>(unnest_name);
   auto filter = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, std::move(colref));
   if (where_clause) {
    where_clause = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
                                                    std::move(where_clause), std::move(filter));
   } else {
    where_clause = std::move(filter);
   }
  }
 }
 return select_node;
}
unique_ptr<BoundTableRef> Binder::Bind(PivotRef &ref) {
 if (!ref.source) {
  throw InternalException("Pivot without a source!?");
 }
 if (!ref.bound_pivot_values.empty() || !ref.bound_group_names.empty() || !ref.bound_aggregate_names.empty()) {
  return BindBoundPivot(ref);
 }
 auto copied_source = ref.source->Copy();
 auto star_binder = Binder::CreateBinder(context, this);
 star_binder->Bind(*copied_source);
 vector<unique_ptr<ParsedExpression>> all_columns;
<<<<<<< HEAD
 star_binder->ExpandStarExpression(make_unique<StarExpression>(), all_columns);
||||||| 4161f39ca1
 child_binder->ExpandStarExpression(make_unique<StarExpression>(), all_columns);
=======
 child_binder->ExpandStarExpression(make_uniq<StarExpression>(), all_columns);
>>>>>>> d84e329b
 unique_ptr<SelectNode> select_node;
 unique_ptr<ParsedExpression> where_clause;
 if (!ref.aggregates.empty()) {
  select_node = BindPivot(ref, std::move(all_columns));
 } else {
  select_node = BindUnpivot(*star_binder, ref, std::move(all_columns), where_clause);
 }
 auto child_binder = Binder::CreateBinder(context, this);
 auto bound_select_node = child_binder->BindNode(*select_node);
 auto root_index = bound_select_node->GetRootIndex();
 BoundQueryNode *bound_select_ptr = bound_select_node.get();
 unique_ptr<BoundTableRef> result;
 MoveCorrelatedExpressions(*child_binder);
<<<<<<< HEAD
 result = make_unique<BoundSubqueryRef>(std::move(child_binder), std::move(bound_select_node));
 auto subquery_alias = ref.alias.empty() ? "__unnamed_pivot" : ref.alias;
 SubqueryRef subquery_ref(nullptr, subquery_alias);
||||||| 4161f39ca1
 result = make_unique<BoundSubqueryRef>(std::move(child_binder), std::move(bound_select_node));
 auto alias = ref.alias.empty() ? "__unnamed_pivot" : ref.alias;
 SubqueryRef subquery_ref(nullptr, alias);
=======
 result = make_uniq<BoundSubqueryRef>(std::move(child_binder), std::move(bound_select_node));
 auto alias = ref.alias.empty() ? "__unnamed_pivot" : ref.alias;
 SubqueryRef subquery_ref(nullptr, alias);
>>>>>>> d84e329b
 subquery_ref.column_name_alias = std::move(ref.column_name_alias);
 if (where_clause) {
  child_binder = Binder::CreateBinder(context, this);
  child_binder->bind_context.AddSubquery(root_index, subquery_ref.alias, subquery_ref, *bound_select_ptr);
  auto where_query = make_uniq<SelectNode>();
  where_query->select_list.push_back(make_uniq<StarExpression>());
  where_query->where_clause = std::move(where_clause);
  bound_select_node = child_binder->BindSelectNode(*where_query, std::move(result));
  bound_select_ptr = bound_select_node.get();
  root_index = bound_select_node->GetRootIndex();
  result = make_uniq<BoundSubqueryRef>(std::move(child_binder), std::move(bound_select_node));
 }
 bind_context.AddSubquery(root_index, subquery_ref.alias, subquery_ref, *bound_select_ptr);
 return result;
}
}
