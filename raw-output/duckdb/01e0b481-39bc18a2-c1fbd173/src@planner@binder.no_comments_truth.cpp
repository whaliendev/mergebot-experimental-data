#include "planner/binder.hpp"
#include "parser/constraints/list.hpp"
#include "parser/expression/list.hpp"
#include "parser/statement/list.hpp"
#include "parser/tableref/list.hpp"
#include "main/client_context.hpp"
#include "main/database.hpp"
using namespace duckdb;
using namespace std;
unique_ptr<SQLStatement> Binder::Visit(SelectStatement &statement) {
 for (auto &cte_it : statement.cte_map) {
  AddCTE(cte_it.first, cte_it.second.get());
 }
 if (statement.from_table) {
  AcceptChild(&statement.from_table);
 }
 vector<unique_ptr<Expression>> new_select_list;
 for (auto &select_element : statement.select_list) {
  if (select_element->GetExpressionType() == ExpressionType::STAR) {
   bind_context->GenerateAllColumnExpressions(new_select_list);
   continue;
  } else {
   new_select_list.push_back(move(select_element));
  }
 }
 statement.result_column_count = new_select_list.size();
 for (size_t i = 0; i < new_select_list.size(); i++) {
  auto &select_element = new_select_list[i];
  AcceptChild(&select_element);
  select_element->ResolveType();
  if (select_element->return_type == TypeId::INVALID) {
   throw BinderException(
       "Could not resolve type of projection element!");
  }
  if (!select_element->alias.empty()) {
   bind_context->AddExpression(select_element->alias,
                               select_element.get(), i);
  }
 }
 for (auto &order : statement.orderby.orders) {
  AcceptChild(&order.expression);
  if (order.expression->type == ExpressionType::COLUMN_REF) {
   auto selection_ref =
       reinterpret_cast<ColumnRefExpression *>(order.expression.get());
   if (selection_ref->column_name.empty()) {
    if (selection_ref->index < 1 ||
        selection_ref->index > new_select_list.size()) {
     throw BinderException(
         "ORDER term out of range - should be between 1 and %d",
         (int)new_select_list.size());
    }
    selection_ref->return_type =
        new_select_list[selection_ref->index - 1]->return_type;
    selection_ref->reference =
        new_select_list[selection_ref->index - 1].get();
   }
  }
  order.expression->ResolveType();
 }
 for (size_t i = 0; i < statement.orderby.orders.size(); i++) {
  size_t j = 0;
  TypeId type = TypeId::INVALID;
  for (; j < new_select_list.size(); j++) {
   if (statement.orderby.orders[i].expression->Equals(
           new_select_list[j].get())) {
    break;
   }
   if (statement.orderby.orders[i].expression->type ==
           ExpressionType::COLUMN_REF &&
       reinterpret_cast<ColumnRefExpression *>(
           statement.orderby.orders[i].expression.get())
               ->reference == new_select_list[j].get()) {
    break;
   }
  }
  if (j == new_select_list.size()) {
   new_select_list.push_back(
       move(statement.orderby.orders[i].expression));
  }
  type = new_select_list[j]->return_type;
  if (type == TypeId::INVALID) {
   throw Exception(
       "Could not deduce return type of ORDER BY expression");
  }
  statement.orderby.orders[i].expression =
      make_unique<ColumnRefExpression>(type, j);
 }
 statement.select_list = move(new_select_list);
 if (statement.where_clause) {
  AcceptChild(&statement.where_clause);
  statement.where_clause->ResolveType();
 }
 if (statement.HasGroup()) {
  for (auto &group : statement.groupby.groups) {
   AcceptChild(&group);
  }
  for (size_t i = 0; i < statement.groupby.groups.size(); i++) {
   auto &group = statement.groupby.groups[i];
   if (group->type != ExpressionType::COLUMN_REF) {
    throw BinderException(
        "GROUP BY clause needs to be a column or alias reference.");
   }
   auto group_column =
       reinterpret_cast<ColumnRefExpression *>(group.get());
   if (group_column->reference) {
    size_t select_index = group_column->index;
    auto group_ref = make_unique<GroupRefExpression>(
        statement.groupby.groups[i]->return_type, i);
    group_ref->alias = string(group_column->column_name);
    statement.groupby.groups[i] =
        move(statement.select_list[select_index]);
    statement.select_list[select_index] = move(group_ref);
   }
  }
  for (size_t i = 0; i < statement.select_list.size(); i++) {
   auto &select = statement.select_list[i];
   if (select->type == ExpressionType::GROUP_REF)
    continue;
   if (select->IsAggregate())
    continue;
   if (select->type == ExpressionType::COLUMN_REF) {
    auto select_column =
        reinterpret_cast<ColumnRefExpression *>(select.get());
    bool found_matching = false;
    for (size_t j = 0; j < statement.groupby.groups.size(); j++) {
     auto &group = statement.groupby.groups[j];
     if (select_column->reference) {
      if (select_column->reference == group.get()) {
       auto group_ref = make_unique<GroupRefExpression>(
           statement.select_list[i]->return_type, j);
       group_ref->alias =
           string(select_column->column_name);
       statement.select_list[i] = move(group_ref);
       found_matching = true;
       break;
      }
     } else {
      if (group->type == ExpressionType::COLUMN_REF) {
       auto group_column =
           reinterpret_cast<ColumnRefExpression *>(
               group.get());
       if (group_column->binding ==
           select_column->binding) {
        auto group_ref =
            make_unique<GroupRefExpression>(
                statement.select_list[i]->return_type,
                j);
        group_ref->alias =
            statement.select_list[i]->alias.empty()
                ? select_column->column_name
                : statement.select_list[i]->alias;
        statement.select_list[i] = move(group_ref);
        found_matching = true;
        break;
       }
      }
     }
    }
    if (found_matching) {
     continue;
    }
   }
   statement.select_list[i] = make_unique<AggregateExpression>(
       ExpressionType::AGGREGATE_FIRST,
       move(statement.select_list[i]));
   statement.select_list[i]->ResolveType();
  }
 }
 if (statement.groupby.having) {
  AcceptChild(&statement.groupby.having);
  statement.groupby.having->ResolveType();
 }
 if (statement.union_select) {
  Binder binder(context, this);
  statement.union_select->Accept(&binder);
  statement.setop_binder = move(binder.bind_context);
 }
 return nullptr;
}
unique_ptr<SQLStatement> Binder::Visit(InsertStatement &statement) {
 if (statement.select_statement) {
  AcceptChild(&statement.select_statement);
 }
 for (auto &expression_list : statement.values) {
  for (auto &expression : expression_list) {
   AcceptChild(&expression);
  }
 }
 return nullptr;
}
unique_ptr<SQLStatement> Binder::Visit(CopyStatement &stmt) {
 if (stmt.select_statement) {
  AcceptChild(&stmt.select_statement);
 }
 return nullptr;
}
unique_ptr<SQLStatement> Binder::Visit(DeleteStatement &stmt) {
 AcceptChild(&stmt.table);
 AcceptChild(&stmt.condition);
 return nullptr;
}
unique_ptr<SQLStatement> Binder::Visit(AlterTableStatement &stmt) {
 AcceptChild(&stmt.table);
 return nullptr;
}
unique_ptr<SQLStatement> Binder::Visit(UpdateStatement &stmt) {
 AcceptChild(&stmt.table);
 if (stmt.condition) {
  AcceptChild(&stmt.condition);
 }
 for (auto &expression : stmt.expressions) {
  AcceptChild(&expression);
  if (expression->type == ExpressionType::VALUE_DEFAULT) {
   continue;
  }
  expression->ResolveType();
  if (expression->return_type == TypeId::INVALID) {
   throw BinderException(
       "Could not resolve type of projection element!");
  }
 }
 return nullptr;
}
unique_ptr<SQLStatement> Binder::Visit(CreateTableStatement &stmt) {
 bind_context->AddDummyTable(stmt.info->table, stmt.info->columns);
 for (auto &it : stmt.info->constraints) {
  AcceptChild(&it);
 }
 return nullptr;
}
unique_ptr<Constraint> Binder::Visit(CheckConstraint &constraint) {
 SQLNodeVisitor::Visit(constraint);
 constraint.expression->ResolveType();
 if (constraint.expression->return_type == TypeId::INVALID) {
  throw BinderException("Could not resolve type of constraint!");
 }
 if (constraint.expression->return_type != TypeId::INTEGER) {
  constraint.expression = make_unique<CastExpression>(
      TypeId::INTEGER, move(constraint.expression));
 }
 return nullptr;
}
unique_ptr<Expression> Binder::Visit(ColumnRefExpression &expr) {
 if (expr.column_name.empty()) {
  return nullptr;
 }
 if (expr.table_name.empty()) {
  expr.table_name = bind_context->GetMatchingBinding(expr.column_name);
 }
 bind_context->BindColumn(expr);
 return nullptr;
}
unique_ptr<Expression> Binder::Visit(FunctionExpression &expr) {
 SQLNodeVisitor::Visit(expr);
 expr.bound_function = context.db.catalog.GetScalarFunction(
     context.ActiveTransaction(), expr.schema, expr.function_name);
 return nullptr;
}
unique_ptr<Expression> Binder::Visit(SubqueryExpression &expr) {
 assert(bind_context);
 Binder binder(context);
 binder.bind_context->parent = bind_context.get();
 binder.CTE_bindings = CTE_bindings;
 expr.subquery->Accept(&binder);
 if (expr.subquery->select_list.size() < 1) {
  throw BinderException("Subquery has no projections");
 }
 if (expr.subquery->select_list[0]->return_type == TypeId::INVALID) {
  throw BinderException("Subquery has no type");
 }
 if (expr.subquery_type == SubqueryType::IN &&
     expr.subquery->select_list.size() != 1) {
  throw BinderException("Subquery returns %zu columns - expected 1",
                        expr.subquery->select_list.size());
 }
 expr.return_type = expr.subquery_type == SubqueryType::EXISTS
                        ? TypeId::BOOLEAN
                        : expr.subquery->select_list[0]->return_type;
 expr.context = move(binder.bind_context);
 expr.is_correlated = expr.context->GetMaxDepth() > 0;
 return nullptr;
}
unique_ptr<TableRef> Binder::Visit(BaseTableRef &expr) {
 auto cte = FindCTE(expr.table_name);
 if (cte) {
  auto subquery = make_unique<SubqueryRef>(move(cte));
  subquery->alias = expr.alias.empty() ? expr.table_name : expr.alias;
  AcceptChild(&subquery);
  return move(subquery);
 }
 auto table = context.db.catalog.GetTable(context.ActiveTransaction(),
                                          expr.schema_name, expr.table_name);
 bind_context->AddBaseTable(
     expr.alias.empty() ? expr.table_name : expr.alias, table);
 return nullptr;
}
unique_ptr<TableRef> Binder::Visit(CrossProductRef &expr) {
 AcceptChild(&expr.left);
 AcceptChild(&expr.right);
 return nullptr;
}
unique_ptr<TableRef> Binder::Visit(JoinRef &expr) {
 AcceptChild(&expr.left);
 AcceptChild(&expr.right);
 AcceptChild(&expr.condition);
 return nullptr;
}
unique_ptr<TableRef> Binder::Visit(SubqueryRef &expr) {
 Binder binder(context, this);
 expr.subquery->Accept(&binder);
 expr.context = move(binder.bind_context);
 bind_context->AddSubquery(expr.alias, expr);
 return nullptr;
}
unique_ptr<TableRef> Binder::Visit(TableFunction &expr) {
 auto function_definition = (FunctionExpression *)expr.function.get();
 auto function = context.db.catalog.GetTableFunction(
     context.ActiveTransaction(), function_definition);
 bind_context->AddTableFunction(
     expr.alias.empty() ? function_definition->function_name : expr.alias,
     function);
 return nullptr;
}
void Binder::AddCTE(const std::string &name, SelectStatement *cte) {
 assert(cte);
 assert(!name.empty());
 auto entry = CTE_bindings.find(name);
 if (entry != CTE_bindings.end()) {
  throw BinderException("Duplicate CTE \"%s\" in query!", name.c_str());
 }
 CTE_bindings[name] = cte;
}
unique_ptr<SelectStatement> Binder::FindCTE(const std::string &name) {
 auto entry = CTE_bindings.find(name);
 if (entry == CTE_bindings.end()) {
  if (parent) {
   return parent->FindCTE(name);
  }
  return nullptr;
 }
 return entry->second->Copy();
}
