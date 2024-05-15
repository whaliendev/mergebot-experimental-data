#include "duckdb_python/pyrelation.hpp"
#include "duckdb_python/pytype.hpp"
#include "duckdb_python/pyconnection.hpp"
#include "duckdb_python/pyconnection/pyconnection.hpp"
#include "duckdb_python/vector_conversion.hpp"
#include "duckdb_python/pandas_type.hpp"
#include "duckdb_python/pyresult.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb_python/numpy/numpy_type.hpp"
#include "duckdb/main/relation/query_relation.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/main/relation/view_relation.hpp"
#include "duckdb/function/pragma/pragma_functions.hpp"
#include "duckdb/parser/statement/pragma_statement.hpp"
#include "duckdb/common/box_renderer.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/catalog/default/default_types.hpp"
namespace duckdb {
DuckDBPyRelation::DuckDBPyRelation(unique_ptr<DuckDBPyResult> result_p) : rel(nullptr), result(std::move(result_p)) {
 if (!result) {
  throw InternalException("DuckDBPyRelation created without a result");
 }
 this->types = result->GetTypes();
 this->names = result->GetNames();
}
DuckDBPyRelation::DuckDBPyRelation(unique_ptr<DuckDBPyResult> result_p) : rel(nullptr), result(std::move(result_p)) {
 if (!result) {
  throw InternalException("DuckDBPyRelation created without a result");
 }
 this->types = result->GetTypes();
 this->names = result->GetNames();
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
struct DescribeAggregateInfo {
 explicit DescribeAggregateInfo(string name_p, bool numeric_only = false)
     : name(std::move(name_p)), numeric_only(numeric_only) {
 }
 string name;
 bool numeric_only;
};
vector<string> CreateExpressionList(const vector<ColumnDefinition> &columns,
                                    const vector<DescribeAggregateInfo> &aggregates) {
 vector<string> expressions;
 expressions.reserve(columns.size());
 string aggr_names = "UNNEST([";
 for (idx_t i = 0; i < aggregates.size(); i++) {
  if (i > 0) {
   aggr_names += ", ";
  }
  aggr_names += "'";
  aggr_names += aggregates[i].name;
  aggr_names += "'";
 }
 aggr_names += "])";
 aggr_names += " AS aggr";
 expressions.push_back(aggr_names);
 for (idx_t c = 0; c < columns.size(); c++) {
  auto &col = columns[c];
  string expr = "UNNEST([";
  for (idx_t i = 0; i < aggregates.size(); i++) {
   if (i > 0) {
    expr += ", ";
   }
   if (aggregates[i].numeric_only && !col.GetType().IsNumeric()) {
    expr += "NULL";
    continue;
   }
   expr += aggregates[i].name;
   expr += "(";
   expr += KeywordHelper::WriteOptionallyQuoted(col.GetName());
   expr += ")";
   if (col.GetType().IsNumeric()) {
    expr += "::DOUBLE";
   } else {
    expr += "::VARCHAR";
   }
  }
  expr += "])";
  expr += " AS " + KeywordHelper::WriteOptionallyQuoted(col.GetName());
  expressions.push_back(expr);
 }
 return expressions;
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
static unique_ptr<QueryResult> PyExecuteRelation(const shared_ptr<Relation> &rel, bool stream_result = false) {
 if (!rel) {
  return nullptr;
 }
 auto context = rel->context.GetContext();
 py::gil_scoped_release release;
 auto pending_query = context->PendingQuery(rel, stream_result);
 return DuckDBPyConnection::CompletePendingQuery(*pending_query);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
static bool ContainsStructFieldByName(LogicalType &type, const string &name) {
 if (type.id() != LogicalTypeId::STRUCT) {
  return false;
 }
 auto count = StructType::GetChildCount(type);
 for (idx_t i = 0; i < count; i++) {
  auto &field_name = StructType::GetChildName(type, i);
  if (StringUtil::CIEquals(name, field_name)) {
   return true;
  }
 }
 return false;
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
static bool IsDescribeStatement(SQLStatement &statement) {
 if (statement.type != StatementType::PRAGMA_STATEMENT) {
  return false;
 }
 auto &pragma_statement = statement.Cast<PragmaStatement>();
 if (pragma_statement.info->name != "show") {
  return false;
 }
 return true;
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
static bool IsAcceptedInsertRelationType(const Relation &relation) {
 return relation.type == RelationType::TABLE_RELATION;
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
bool DuckDBPyRelation::IsRelation(const py::object &object) {
 return py::isinstance<DuckDBPyRelation>(object);
}
}
