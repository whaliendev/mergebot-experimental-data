#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/extra_type_info.hpp"
#include "duckdb/parser/common_table_expression_info.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/parser/parsed_data/sample_options.hpp"
#include "duckdb/execution/reservoir_sample.hpp"
#include "duckdb/common/queue.hpp"
#include "duckdb/parser/tableref/pivotref.hpp"
#include "duckdb/planner/tableref/bound_pivotref.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression/bound_parameter_data.hpp"
#include "duckdb/planner/joinside.hpp"
#include "duckdb/parser/parsed_data/vacuum_info.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/common/multi_file_reader_options.hpp"
#include "duckdb/common/multi_file_reader.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_option.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"
#include "duckdb/function/scalar/strftime_format.hpp"
#include "duckdb/function/table/read_csv.hpp"
#include "duckdb/common/types/interval.hpp"
<<<<<<< HEAD
#include "duckdb/parser/qualified_name.hpp"
||||||| c220f7bc2f
=======
#include "duckdb/parser/parsed_data/exported_table_data.hpp"
>>>>>>> 18132018
namespace duckdb {
void BlockingSample::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<unique_ptr<BaseReservoirSampling>>(100, "base_reservoir_sample", base_reservoir_sample);
 serializer.WriteProperty<SampleType>(101, "type", type);
 serializer.WritePropertyWithDefault<bool>(102, "destroyed", destroyed);
}
unique_ptr<BlockingSample> BlockingSample::Deserialize(Deserializer &deserializer) {
 auto base_reservoir_sample = deserializer.ReadPropertyWithDefault<unique_ptr<BaseReservoirSampling>>(100, "base_reservoir_sample");
 auto type = deserializer.ReadProperty<SampleType>(101, "type");
 auto destroyed = deserializer.ReadPropertyWithDefault<bool>(102, "destroyed");
 unique_ptr<BlockingSample> result;
 switch (type) {
 case SampleType::RESERVOIR_PERCENTAGE_SAMPLE:
  result = ReservoirSamplePercentage::Deserialize(deserializer);
  break;
 case SampleType::RESERVOIR_SAMPLE:
  result = ReservoirSample::Deserialize(deserializer);
  break;
 default:
  throw SerializationException("Unsupported type for deserialization of BlockingSample!");
 }
 result->base_reservoir_sample = std::move(base_reservoir_sample);
 result->destroyed = destroyed;
 return result;
}
void BaseReservoirSampling::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<idx_t>(100, "next_index_to_sample", next_index_to_sample);
 serializer.WriteProperty<double>(101, "min_weight_threshold", min_weight_threshold);
 serializer.WritePropertyWithDefault<idx_t>(102, "min_weighted_entry_index", min_weighted_entry_index);
 serializer.WritePropertyWithDefault<idx_t>(103, "num_entries_to_skip_b4_next_sample", num_entries_to_skip_b4_next_sample);
 serializer.WritePropertyWithDefault<idx_t>(104, "num_entries_seen_total", num_entries_seen_total);
 serializer.WritePropertyWithDefault<std::priority_queue<std::pair<double, idx_t>>>(105, "reservoir_weights", reservoir_weights);
}
unique_ptr<BaseReservoirSampling> BaseReservoirSampling::Deserialize(Deserializer &deserializer) {
 auto result = duckdb::unique_ptr<BaseReservoirSampling>(new BaseReservoirSampling());
 deserializer.ReadPropertyWithDefault<idx_t>(100, "next_index_to_sample", result->next_index_to_sample);
 deserializer.ReadProperty<double>(101, "min_weight_threshold", result->min_weight_threshold);
 deserializer.ReadPropertyWithDefault<idx_t>(102, "min_weighted_entry_index", result->min_weighted_entry_index);
 deserializer.ReadPropertyWithDefault<idx_t>(103, "num_entries_to_skip_b4_next_sample", result->num_entries_to_skip_b4_next_sample);
 deserializer.ReadPropertyWithDefault<idx_t>(104, "num_entries_seen_total", result->num_entries_seen_total);
 deserializer.ReadPropertyWithDefault<std::priority_queue<std::pair<double, idx_t>>>(105, "reservoir_weights", result->reservoir_weights);
 return result;
}
void BoundCaseCheck::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<unique_ptr<Expression>>(100, "when_expr", when_expr);
 serializer.WritePropertyWithDefault<unique_ptr<Expression>>(101, "then_expr", then_expr);
}
BoundCaseCheck BoundCaseCheck::Deserialize(Deserializer &deserializer) {
 BoundCaseCheck result;
 deserializer.ReadPropertyWithDefault<unique_ptr<Expression>>(100, "when_expr", result.when_expr);
 deserializer.ReadPropertyWithDefault<unique_ptr<Expression>>(101, "then_expr", result.then_expr);
 return result;
}
void BoundLimitNode::Serialize(Serializer &serializer) const {
 serializer.WriteProperty<LimitNodeType>(100, "type", type);
 serializer.WritePropertyWithDefault<idx_t>(101, "constant_integer", constant_integer);
 serializer.WriteProperty<double>(102, "constant_percentage", constant_percentage);
 serializer.WritePropertyWithDefault<unique_ptr<Expression>>(103, "expression", expression);
}
BoundLimitNode BoundLimitNode::Deserialize(Deserializer &deserializer) {
 auto type = deserializer.ReadProperty<LimitNodeType>(100, "type");
 auto constant_integer = deserializer.ReadPropertyWithDefault<idx_t>(101, "constant_integer");
 auto constant_percentage = deserializer.ReadProperty<double>(102, "constant_percentage");
 auto expression = deserializer.ReadPropertyWithDefault<unique_ptr<Expression>>(103, "expression");
 BoundLimitNode result(type, constant_integer, constant_percentage, std::move(expression));
 return result;
}
void BoundOrderByNode::Serialize(Serializer &serializer) const {
 serializer.WriteProperty<OrderType>(100, "type", type);
 serializer.WriteProperty<OrderByNullType>(101, "null_order", null_order);
 serializer.WritePropertyWithDefault<unique_ptr<Expression>>(102, "expression", expression);
}
BoundOrderByNode BoundOrderByNode::Deserialize(Deserializer &deserializer) {
 auto type = deserializer.ReadProperty<OrderType>(100, "type");
 auto null_order = deserializer.ReadProperty<OrderByNullType>(101, "null_order");
 auto expression = deserializer.ReadPropertyWithDefault<unique_ptr<Expression>>(102, "expression");
 BoundOrderByNode result(type, null_order, std::move(expression));
 return result;
}
void BoundParameterData::Serialize(Serializer &serializer) const {
 serializer.WriteProperty<Value>(100, "value", value);
 serializer.WriteProperty<LogicalType>(101, "return_type", return_type);
}
shared_ptr<BoundParameterData> BoundParameterData::Deserialize(Deserializer &deserializer) {
 auto value = deserializer.ReadProperty<Value>(100, "value");
 auto result = duckdb::shared_ptr<BoundParameterData>(new BoundParameterData(value));
 deserializer.ReadProperty<LogicalType>(101, "return_type", result->return_type);
 return result;
}
void BoundPivotInfo::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<idx_t>(100, "group_count", group_count);
 serializer.WritePropertyWithDefault<vector<LogicalType>>(101, "types", types);
 serializer.WritePropertyWithDefault<vector<string>>(102, "pivot_values", pivot_values);
 serializer.WritePropertyWithDefault<vector<unique_ptr<Expression>>>(103, "aggregates", aggregates);
}
BoundPivotInfo BoundPivotInfo::Deserialize(Deserializer &deserializer) {
 BoundPivotInfo result;
 deserializer.ReadPropertyWithDefault<idx_t>(100, "group_count", result.group_count);
 deserializer.ReadPropertyWithDefault<vector<LogicalType>>(101, "types", result.types);
 deserializer.ReadPropertyWithDefault<vector<string>>(102, "pivot_values", result.pivot_values);
 deserializer.ReadPropertyWithDefault<vector<unique_ptr<Expression>>>(103, "aggregates", result.aggregates);
 return result;
}
template <typename T>
void CSVOption<T>::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<bool>(100, "set_by_user", set_by_user);
 serializer.WriteProperty<T>(101, "value", value);
}
template <typename T>
CSVOption<T> CSVOption<T>::Deserialize(Deserializer &deserializer) {
 CSVOption<T> result;
 deserializer.ReadPropertyWithDefault<bool>(100, "set_by_user", result.set_by_user);
 deserializer.ReadProperty<T>(101, "value", result.value);
 return result;
}
void CSVReaderOptions::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<bool>(100, "ignore_errors", ignore_errors, false);
 serializer.WritePropertyWithDefault<idx_t>(101, "buffer_sample_size", buffer_sample_size);
 serializer.WritePropertyWithDefault<vector<string>>(102, "null_str", null_str);
 serializer.WriteProperty<FileCompressionType>(103, "compression", compression);
 serializer.WritePropertyWithDefault<bool>(104, "allow_quoted_nulls", allow_quoted_nulls);
 serializer.WritePropertyWithDefault<idx_t>(105, "maximum_line_size", maximum_line_size);
 serializer.WritePropertyWithDefault<bool>(106, "normalize_names", normalize_names);
 serializer.WritePropertyWithDefault<vector<bool>>(107, "force_not_null", force_not_null);
 serializer.WritePropertyWithDefault<bool>(108, "all_varchar", all_varchar);
 serializer.WritePropertyWithDefault<idx_t>(109, "sample_size_chunks", sample_size_chunks);
 serializer.WritePropertyWithDefault<bool>(110, "auto_detect", auto_detect);
 serializer.WritePropertyWithDefault<string>(111, "file_path", file_path);
 serializer.WritePropertyWithDefault<string>(112, "decimal_separator", decimal_separator);
 serializer.WritePropertyWithDefault<bool>(113, "null_padding", null_padding);
 serializer.WritePropertyWithDefault<idx_t>(114, "buffer_size", buffer_size);
 serializer.WriteProperty<MultiFileReaderOptions>(115, "file_options", file_options);
 serializer.WritePropertyWithDefault<vector<bool>>(116, "force_quote", force_quote);
 serializer.WritePropertyWithDefault<string>(117, "rejects_table_name", rejects_table_name, "reject_errors");
 serializer.WritePropertyWithDefault<idx_t>(118, "rejects_limit", rejects_limit);
 serializer.WriteProperty<CSVOption<char>>(121, "dialect_options.state_machine_options.delimiter", dialect_options.state_machine_options.delimiter);
 serializer.WriteProperty<CSVOption<char>>(122, "dialect_options.state_machine_options.quote", dialect_options.state_machine_options.quote);
 serializer.WriteProperty<CSVOption<char>>(123, "dialect_options.state_machine_options.escape", dialect_options.state_machine_options.escape);
 serializer.WriteProperty<CSVOption<bool>>(124, "dialect_options.header", dialect_options.header);
 serializer.WritePropertyWithDefault<idx_t>(125, "dialect_options.num_cols", dialect_options.num_cols);
 serializer.WriteProperty<CSVOption<NewLineIdentifier>>(126, "dialect_options.state_machine_options.new_line", dialect_options.state_machine_options.new_line);
 serializer.WriteProperty<CSVOption<idx_t>>(127, "dialect_options.skip_rows", dialect_options.skip_rows);
 serializer.WriteProperty<map<LogicalTypeId, CSVOption<StrpTimeFormat>>>(128, "dialect_options.date_format", dialect_options.date_format);
 serializer.WritePropertyWithDefault<string>(129, "sniffer_user_mismatch_error", sniffer_user_mismatch_error);
 serializer.WritePropertyWithDefault<bool>(130, "parallel", parallel);
 serializer.WritePropertyWithDefault<vector<bool>>(131, "was_type_manually_set", was_type_manually_set);
 serializer.WritePropertyWithDefault<CSVOption<string>>(132, "rejects_scan_name", rejects_scan_name, {"reject_scans"});
 serializer.WritePropertyWithDefault<vector<string>>(133, "name_list", name_list);
 serializer.WritePropertyWithDefault<vector<LogicalType>>(134, "sql_type_list", sql_type_list);
 serializer.WritePropertyWithDefault<case_insensitive_map_t<idx_t>>(135, "sql_types_per_column", sql_types_per_column);
 serializer.WritePropertyWithDefault<bool>(136, "columns_set", columns_set, false);
 serializer.WritePropertyWithDefault<CSVOption<char>>(137, "dialect_options.state_machine_options.comment", dialect_options.state_machine_options.comment, CSVOption<char>('\0'));
 serializer.WritePropertyWithDefault<idx_t>(138, "dialect_options.rows_until_header", dialect_options.rows_until_header);
}
CSVReaderOptions CSVReaderOptions::Deserialize(Deserializer &deserializer) {
 CSVReaderOptions result;
 deserializer.ReadPropertyWithExplicitDefault<bool>(100, "ignore_errors", result.ignore_errors, false);
 deserializer.ReadPropertyWithDefault<idx_t>(101, "buffer_sample_size", result.buffer_sample_size);
 deserializer.ReadPropertyWithDefault<vector<string>>(102, "null_str", result.null_str);
 deserializer.ReadProperty<FileCompressionType>(103, "compression", result.compression);
 deserializer.ReadPropertyWithDefault<bool>(104, "allow_quoted_nulls", result.allow_quoted_nulls);
 deserializer.ReadPropertyWithDefault<idx_t>(105, "maximum_line_size", result.maximum_line_size);
 deserializer.ReadPropertyWithDefault<bool>(106, "normalize_names", result.normalize_names);
 deserializer.ReadPropertyWithDefault<vector<bool>>(107, "force_not_null", result.force_not_null);
 deserializer.ReadPropertyWithDefault<bool>(108, "all_varchar", result.all_varchar);
 deserializer.ReadPropertyWithDefault<idx_t>(109, "sample_size_chunks", result.sample_size_chunks);
 deserializer.ReadPropertyWithDefault<bool>(110, "auto_detect", result.auto_detect);
 deserializer.ReadPropertyWithDefault<string>(111, "file_path", result.file_path);
 deserializer.ReadPropertyWithDefault<string>(112, "decimal_separator", result.decimal_separator);
 deserializer.ReadPropertyWithDefault<bool>(113, "null_padding", result.null_padding);
 deserializer.ReadPropertyWithDefault<idx_t>(114, "buffer_size", result.buffer_size);
 deserializer.ReadProperty<MultiFileReaderOptions>(115, "file_options", result.file_options);
 deserializer.ReadPropertyWithDefault<vector<bool>>(116, "force_quote", result.force_quote);
 deserializer.ReadPropertyWithExplicitDefault<string>(117, "rejects_table_name", result.rejects_table_name, "reject_errors");
 deserializer.ReadPropertyWithDefault<idx_t>(118, "rejects_limit", result.rejects_limit);
 deserializer.ReadDeletedProperty<vector<string>>(119, "rejects_recovery_columns");
 deserializer.ReadDeletedProperty<vector<idx_t>>(120, "rejects_recovery_column_ids");
 deserializer.ReadProperty<CSVOption<char>>(121, "dialect_options.state_machine_options.delimiter", result.dialect_options.state_machine_options.delimiter);
 deserializer.ReadProperty<CSVOption<char>>(122, "dialect_options.state_machine_options.quote", result.dialect_options.state_machine_options.quote);
 deserializer.ReadProperty<CSVOption<char>>(123, "dialect_options.state_machine_options.escape", result.dialect_options.state_machine_options.escape);
 deserializer.ReadProperty<CSVOption<bool>>(124, "dialect_options.header", result.dialect_options.header);
 deserializer.ReadPropertyWithDefault<idx_t>(125, "dialect_options.num_cols", result.dialect_options.num_cols);
 deserializer.ReadProperty<CSVOption<NewLineIdentifier>>(126, "dialect_options.state_machine_options.new_line", result.dialect_options.state_machine_options.new_line);
 deserializer.ReadProperty<CSVOption<idx_t>>(127, "dialect_options.skip_rows", result.dialect_options.skip_rows);
 deserializer.ReadProperty<map<LogicalTypeId, CSVOption<StrpTimeFormat>>>(128, "dialect_options.date_format", result.dialect_options.date_format);
 deserializer.ReadPropertyWithDefault<string>(129, "sniffer_user_mismatch_error", result.sniffer_user_mismatch_error);
 deserializer.ReadPropertyWithDefault<bool>(130, "parallel", result.parallel);
 deserializer.ReadPropertyWithDefault<vector<bool>>(131, "was_type_manually_set", result.was_type_manually_set);
 deserializer.ReadPropertyWithExplicitDefault<CSVOption<string>>(132, "rejects_scan_name", result.rejects_scan_name, {"reject_scans"});
 deserializer.ReadPropertyWithDefault<vector<string>>(133, "name_list", result.name_list);
 deserializer.ReadPropertyWithDefault<vector<LogicalType>>(134, "sql_type_list", result.sql_type_list);
 deserializer.ReadPropertyWithDefault<case_insensitive_map_t<idx_t>>(135, "sql_types_per_column", result.sql_types_per_column);
 deserializer.ReadPropertyWithExplicitDefault<bool>(136, "columns_set", result.columns_set, false);
 deserializer.ReadPropertyWithExplicitDefault<CSVOption<char>>(137, "dialect_options.state_machine_options.comment", result.dialect_options.state_machine_options.comment, CSVOption<char>('\0'));
 deserializer.ReadPropertyWithDefault<idx_t>(138, "dialect_options.rows_until_header", result.dialect_options.rows_until_header);
 return result;
}
void CaseCheck::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<unique_ptr<ParsedExpression>>(100, "when_expr", when_expr);
 serializer.WritePropertyWithDefault<unique_ptr<ParsedExpression>>(101, "then_expr", then_expr);
}
CaseCheck CaseCheck::Deserialize(Deserializer &deserializer) {
 CaseCheck result;
 deserializer.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(100, "when_expr", result.when_expr);
 deserializer.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(101, "then_expr", result.then_expr);
 return result;
}
void ColumnBinding::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<idx_t>(100, "table_index", table_index);
 serializer.WritePropertyWithDefault<idx_t>(101, "column_index", column_index);
}
ColumnBinding ColumnBinding::Deserialize(Deserializer &deserializer) {
 ColumnBinding result;
 deserializer.ReadPropertyWithDefault<idx_t>(100, "table_index", result.table_index);
 deserializer.ReadPropertyWithDefault<idx_t>(101, "column_index", result.column_index);
 return result;
}
void ColumnDefinition::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<string>(100, "name", name);
 serializer.WriteProperty<LogicalType>(101, "type", type);
 serializer.WritePropertyWithDefault<unique_ptr<ParsedExpression>>(102, "expression", expression);
 serializer.WriteProperty<TableColumnType>(103, "category", category);
 serializer.WriteProperty<duckdb::CompressionType>(104, "compression_type", compression_type);
 serializer.WritePropertyWithDefault<Value>(105, "comment", comment, Value());
 serializer.WritePropertyWithDefault<unordered_map<string, string>>(106, "tags", tags, unordered_map<string, string>());
}
ColumnDefinition ColumnDefinition::Deserialize(Deserializer &deserializer) {
 auto name = deserializer.ReadPropertyWithDefault<string>(100, "name");
 auto type = deserializer.ReadProperty<LogicalType>(101, "type");
 auto expression = deserializer.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(102, "expression");
 auto category = deserializer.ReadProperty<TableColumnType>(103, "category");
 ColumnDefinition result(std::move(name), std::move(type), std::move(expression), category);
 deserializer.ReadProperty<duckdb::CompressionType>(104, "compression_type", result.compression_type);
 deserializer.ReadPropertyWithExplicitDefault<Value>(105, "comment", result.comment, Value());
 deserializer.ReadPropertyWithExplicitDefault<unordered_map<string, string>>(106, "tags", result.tags, unordered_map<string, string>());
 return result;
}
void ColumnInfo::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<vector<string>>(100, "names", names);
 serializer.WritePropertyWithDefault<vector<LogicalType>>(101, "types", types);
}
ColumnInfo ColumnInfo::Deserialize(Deserializer &deserializer) {
 ColumnInfo result;
 deserializer.ReadPropertyWithDefault<vector<string>>(100, "names", result.names);
 deserializer.ReadPropertyWithDefault<vector<LogicalType>>(101, "types", result.types);
 return result;
}
void ColumnList::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<vector<ColumnDefinition>>(100, "columns", columns);
}
ColumnList ColumnList::Deserialize(Deserializer &deserializer) {
 auto columns = deserializer.ReadPropertyWithDefault<vector<ColumnDefinition>>(100, "columns");
 ColumnList result(std::move(columns));
 return result;
}
void CommonTableExpressionInfo::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<vector<string>>(100, "aliases", aliases);
 serializer.WritePropertyWithDefault<unique_ptr<SelectStatement>>(101, "query", query);
 serializer.WriteProperty<CTEMaterialize>(102, "materialized", materialized);
}
unique_ptr<CommonTableExpressionInfo> CommonTableExpressionInfo::Deserialize(Deserializer &deserializer) {
 auto result = duckdb::unique_ptr<CommonTableExpressionInfo>(new CommonTableExpressionInfo());
 deserializer.ReadPropertyWithDefault<vector<string>>(100, "aliases", result->aliases);
 deserializer.ReadPropertyWithDefault<unique_ptr<SelectStatement>>(101, "query", result->query);
 deserializer.ReadProperty<CTEMaterialize>(102, "materialized", result->materialized);
 return result;
}
void CommonTableExpressionMap::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<InsertionOrderPreservingMap<unique_ptr<CommonTableExpressionInfo>>>(100, "map", map);
}
CommonTableExpressionMap CommonTableExpressionMap::Deserialize(Deserializer &deserializer) {
 CommonTableExpressionMap result;
 deserializer.ReadPropertyWithDefault<InsertionOrderPreservingMap<unique_ptr<CommonTableExpressionInfo>>>(100, "map", result.map);
 return result;
}
void ExportedTableData::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<string>(1, "table_name", table_name);
 serializer.WritePropertyWithDefault<string>(2, "schema_name", schema_name);
 serializer.WritePropertyWithDefault<string>(3, "database_name", database_name);
 serializer.WritePropertyWithDefault<string>(4, "file_path", file_path);
 serializer.WritePropertyWithDefault<vector<string>>(5, "not_null_columns", not_null_columns);
}
ExportedTableData ExportedTableData::Deserialize(Deserializer &deserializer) {
 ExportedTableData result;
 deserializer.ReadPropertyWithDefault<string>(1, "table_name", result.table_name);
 deserializer.ReadPropertyWithDefault<string>(2, "schema_name", result.schema_name);
 deserializer.ReadPropertyWithDefault<string>(3, "database_name", result.database_name);
 deserializer.ReadPropertyWithDefault<string>(4, "file_path", result.file_path);
 deserializer.ReadPropertyWithDefault<vector<string>>(5, "not_null_columns", result.not_null_columns);
 return result;
}
void ExportedTableInfo::Serialize(Serializer &serializer) const {
 serializer.WriteProperty<ExportedTableData>(1, "table_data", table_data);
}
ExportedTableInfo ExportedTableInfo::Deserialize(Deserializer &deserializer) {
 auto table_data = deserializer.ReadProperty<ExportedTableData>(1, "table_data");
 ExportedTableInfo result(deserializer.Get<ClientContext &>(), table_data);
 return result;
}
void HivePartitioningIndex::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<string>(100, "value", value);
 serializer.WritePropertyWithDefault<idx_t>(101, "index", index);
}
HivePartitioningIndex HivePartitioningIndex::Deserialize(Deserializer &deserializer) {
 auto value = deserializer.ReadPropertyWithDefault<string>(100, "value");
 auto index = deserializer.ReadPropertyWithDefault<idx_t>(101, "index");
 HivePartitioningIndex result(std::move(value), index);
 return result;
}
void JoinCondition::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<unique_ptr<Expression>>(100, "left", left);
 serializer.WritePropertyWithDefault<unique_ptr<Expression>>(101, "right", right);
 serializer.WriteProperty<ExpressionType>(102, "comparison", comparison);
}
JoinCondition JoinCondition::Deserialize(Deserializer &deserializer) {
 JoinCondition result;
 deserializer.ReadPropertyWithDefault<unique_ptr<Expression>>(100, "left", result.left);
 deserializer.ReadPropertyWithDefault<unique_ptr<Expression>>(101, "right", result.right);
 deserializer.ReadProperty<ExpressionType>(102, "comparison", result.comparison);
 return result;
}
void LogicalType::Serialize(Serializer &serializer) const {
 serializer.WriteProperty<LogicalTypeId>(100, "id", id_);
 serializer.WritePropertyWithDefault<shared_ptr<ExtraTypeInfo>>(101, "type_info", type_info_);
}
LogicalType LogicalType::Deserialize(Deserializer &deserializer) {
 auto id = deserializer.ReadProperty<LogicalTypeId>(100, "id");
 auto type_info = deserializer.ReadPropertyWithDefault<shared_ptr<ExtraTypeInfo>>(101, "type_info");
 LogicalType result(id, std::move(type_info));
 return result;
}
void MultiFileReaderBindData::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<idx_t>(100, "filename_idx", filename_idx);
 serializer.WritePropertyWithDefault<vector<HivePartitioningIndex>>(101, "hive_partitioning_indexes", hive_partitioning_indexes);
}
MultiFileReaderBindData MultiFileReaderBindData::Deserialize(Deserializer &deserializer) {
 MultiFileReaderBindData result;
 deserializer.ReadPropertyWithDefault<idx_t>(100, "filename_idx", result.filename_idx);
 deserializer.ReadPropertyWithDefault<vector<HivePartitioningIndex>>(101, "hive_partitioning_indexes", result.hive_partitioning_indexes);
 return result;
}
void MultiFileReaderOptions::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<bool>(100, "filename", filename);
 serializer.WritePropertyWithDefault<bool>(101, "hive_partitioning", hive_partitioning);
 serializer.WritePropertyWithDefault<bool>(102, "auto_detect_hive_partitioning", auto_detect_hive_partitioning);
 serializer.WritePropertyWithDefault<bool>(103, "union_by_name", union_by_name);
 serializer.WritePropertyWithDefault<bool>(104, "hive_types_autocast", hive_types_autocast);
 serializer.WritePropertyWithDefault<case_insensitive_map_t<LogicalType>>(105, "hive_types_schema", hive_types_schema);
 serializer.WritePropertyWithDefault<string>(106, "filename_column", filename_column, MultiFileReaderOptions::DEFAULT_FILENAME_COLUMN);
}
MultiFileReaderOptions MultiFileReaderOptions::Deserialize(Deserializer &deserializer) {
 MultiFileReaderOptions result;
 deserializer.ReadPropertyWithDefault<bool>(100, "filename", result.filename);
 deserializer.ReadPropertyWithDefault<bool>(101, "hive_partitioning", result.hive_partitioning);
 deserializer.ReadPropertyWithDefault<bool>(102, "auto_detect_hive_partitioning", result.auto_detect_hive_partitioning);
 deserializer.ReadPropertyWithDefault<bool>(103, "union_by_name", result.union_by_name);
 deserializer.ReadPropertyWithDefault<bool>(104, "hive_types_autocast", result.hive_types_autocast);
 deserializer.ReadPropertyWithDefault<case_insensitive_map_t<LogicalType>>(105, "hive_types_schema", result.hive_types_schema);
 deserializer.ReadPropertyWithExplicitDefault<string>(106, "filename_column", result.filename_column, MultiFileReaderOptions::DEFAULT_FILENAME_COLUMN);
 return result;
}
void OrderByNode::Serialize(Serializer &serializer) const {
 serializer.WriteProperty<OrderType>(100, "type", type);
 serializer.WriteProperty<OrderByNullType>(101, "null_order", null_order);
 serializer.WritePropertyWithDefault<unique_ptr<ParsedExpression>>(102, "expression", expression);
}
OrderByNode OrderByNode::Deserialize(Deserializer &deserializer) {
 auto type = deserializer.ReadProperty<OrderType>(100, "type");
 auto null_order = deserializer.ReadProperty<OrderByNullType>(101, "null_order");
 auto expression = deserializer.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(102, "expression");
 OrderByNode result(type, null_order, std::move(expression));
 return result;
}
void PivotColumn::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<vector<unique_ptr<ParsedExpression>>>(100, "pivot_expressions", pivot_expressions);
 serializer.WritePropertyWithDefault<vector<string>>(101, "unpivot_names", unpivot_names);
 serializer.WritePropertyWithDefault<vector<PivotColumnEntry>>(102, "entries", entries);
 serializer.WritePropertyWithDefault<string>(103, "pivot_enum", pivot_enum);
}
PivotColumn PivotColumn::Deserialize(Deserializer &deserializer) {
 PivotColumn result;
 deserializer.ReadPropertyWithDefault<vector<unique_ptr<ParsedExpression>>>(100, "pivot_expressions", result.pivot_expressions);
 deserializer.ReadPropertyWithDefault<vector<string>>(101, "unpivot_names", result.unpivot_names);
 deserializer.ReadPropertyWithDefault<vector<PivotColumnEntry>>(102, "entries", result.entries);
 deserializer.ReadPropertyWithDefault<string>(103, "pivot_enum", result.pivot_enum);
 return result;
}
void PivotColumnEntry::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<vector<Value>>(100, "values", values);
 serializer.WritePropertyWithDefault<unique_ptr<ParsedExpression>>(101, "star_expr", expr);
 serializer.WritePropertyWithDefault<string>(102, "alias", alias);
}
PivotColumnEntry PivotColumnEntry::Deserialize(Deserializer &deserializer) {
 PivotColumnEntry result;
 deserializer.ReadPropertyWithDefault<vector<Value>>(100, "values", result.values);
 deserializer.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(101, "star_expr", result.expr);
 deserializer.ReadPropertyWithDefault<string>(102, "alias", result.alias);
 return result;
}
void QualifiedColumnName::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<string>(100, "catalog", catalog);
 serializer.WritePropertyWithDefault<string>(101, "schema", schema);
 serializer.WritePropertyWithDefault<string>(102, "table", table);
 serializer.WritePropertyWithDefault<string>(103, "column", column);
}
QualifiedColumnName QualifiedColumnName::Deserialize(Deserializer &deserializer) {
 QualifiedColumnName result;
 deserializer.ReadPropertyWithDefault<string>(100, "catalog", result.catalog);
 deserializer.ReadPropertyWithDefault<string>(101, "schema", result.schema);
 deserializer.ReadPropertyWithDefault<string>(102, "table", result.table);
 deserializer.ReadPropertyWithDefault<string>(103, "column", result.column);
 return result;
}
void ReadCSVData::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<vector<string>>(100, "files", files);
 serializer.WritePropertyWithDefault<vector<LogicalType>>(101, "csv_types", csv_types);
 serializer.WritePropertyWithDefault<vector<string>>(102, "csv_names", csv_names);
 serializer.WritePropertyWithDefault<vector<LogicalType>>(103, "return_types", return_types);
 serializer.WritePropertyWithDefault<vector<string>>(104, "return_names", return_names);
 serializer.WritePropertyWithDefault<idx_t>(105, "filename_col_idx", filename_col_idx);
 serializer.WriteProperty<CSVReaderOptions>(106, "options", options);
 serializer.WriteProperty<MultiFileReaderBindData>(107, "reader_bind", reader_bind);
 serializer.WritePropertyWithDefault<vector<ColumnInfo>>(108, "column_info", column_info);
}
unique_ptr<ReadCSVData> ReadCSVData::Deserialize(Deserializer &deserializer) {
 auto result = duckdb::unique_ptr<ReadCSVData>(new ReadCSVData());
 deserializer.ReadPropertyWithDefault<vector<string>>(100, "files", result->files);
 deserializer.ReadPropertyWithDefault<vector<LogicalType>>(101, "csv_types", result->csv_types);
 deserializer.ReadPropertyWithDefault<vector<string>>(102, "csv_names", result->csv_names);
 deserializer.ReadPropertyWithDefault<vector<LogicalType>>(103, "return_types", result->return_types);
 deserializer.ReadPropertyWithDefault<vector<string>>(104, "return_names", result->return_names);
 deserializer.ReadPropertyWithDefault<idx_t>(105, "filename_col_idx", result->filename_col_idx);
 deserializer.ReadProperty<CSVReaderOptions>(106, "options", result->options);
 deserializer.ReadProperty<MultiFileReaderBindData>(107, "reader_bind", result->reader_bind);
 deserializer.ReadPropertyWithDefault<vector<ColumnInfo>>(108, "column_info", result->column_info);
 return result;
}
void ReservoirSample::Serialize(Serializer &serializer) const {
 BlockingSample::Serialize(serializer);
 serializer.WritePropertyWithDefault<idx_t>(200, "sample_count", sample_count);
 serializer.WritePropertyWithDefault<unique_ptr<ReservoirChunk>>(201, "reservoir_chunk", reservoir_chunk);
}
unique_ptr<BlockingSample> ReservoirSample::Deserialize(Deserializer &deserializer) {
 auto sample_count = deserializer.ReadPropertyWithDefault<idx_t>(200, "sample_count");
 auto result = duckdb::unique_ptr<ReservoirSample>(new ReservoirSample(sample_count));
 deserializer.ReadPropertyWithDefault<unique_ptr<ReservoirChunk>>(201, "reservoir_chunk", result->reservoir_chunk);
 return std::move(result);
}
void ReservoirSamplePercentage::Serialize(Serializer &serializer) const {
 BlockingSample::Serialize(serializer);
 serializer.WriteProperty<double>(200, "sample_percentage", sample_percentage);
 serializer.WritePropertyWithDefault<idx_t>(201, "reservoir_sample_size", reservoir_sample_size);
}
unique_ptr<BlockingSample> ReservoirSamplePercentage::Deserialize(Deserializer &deserializer) {
 auto sample_percentage = deserializer.ReadProperty<double>(200, "sample_percentage");
 auto result = duckdb::unique_ptr<ReservoirSamplePercentage>(new ReservoirSamplePercentage(sample_percentage));
 deserializer.ReadPropertyWithDefault<idx_t>(201, "reservoir_sample_size", result->reservoir_sample_size);
 return std::move(result);
}
void SampleOptions::Serialize(Serializer &serializer) const {
 serializer.WriteProperty<Value>(100, "sample_size", sample_size);
 serializer.WritePropertyWithDefault<bool>(101, "is_percentage", is_percentage);
 serializer.WriteProperty<SampleMethod>(102, "method", method);
 serializer.WritePropertyWithDefault<int64_t>(103, "seed", seed);
}
unique_ptr<SampleOptions> SampleOptions::Deserialize(Deserializer &deserializer) {
 auto result = duckdb::unique_ptr<SampleOptions>(new SampleOptions());
 deserializer.ReadProperty<Value>(100, "sample_size", result->sample_size);
 deserializer.ReadPropertyWithDefault<bool>(101, "is_percentage", result->is_percentage);
 deserializer.ReadProperty<SampleMethod>(102, "method", result->method);
 deserializer.ReadPropertyWithDefault<int64_t>(103, "seed", result->seed);
 return result;
}
void StrpTimeFormat::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<string>(100, "format_specifier", format_specifier);
}
StrpTimeFormat StrpTimeFormat::Deserialize(Deserializer &deserializer) {
 auto format_specifier = deserializer.ReadPropertyWithDefault<string>(100, "format_specifier");
 StrpTimeFormat result(format_specifier);
 return result;
}
void TableFilterSet::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<unordered_map<idx_t, unique_ptr<TableFilter>>>(100, "filters", filters);
}
TableFilterSet TableFilterSet::Deserialize(Deserializer &deserializer) {
 TableFilterSet result;
 deserializer.ReadPropertyWithDefault<unordered_map<idx_t, unique_ptr<TableFilter>>>(100, "filters", result.filters);
 return result;
}
void VacuumOptions::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<bool>(100, "vacuum", vacuum);
 serializer.WritePropertyWithDefault<bool>(101, "analyze", analyze);
}
VacuumOptions VacuumOptions::Deserialize(Deserializer &deserializer) {
 VacuumOptions result;
 deserializer.ReadPropertyWithDefault<bool>(100, "vacuum", result.vacuum);
 deserializer.ReadPropertyWithDefault<bool>(101, "analyze", result.analyze);
 return result;
}
void interval_t::Serialize(Serializer &serializer) const {
 serializer.WritePropertyWithDefault<int32_t>(1, "months", months);
 serializer.WritePropertyWithDefault<int32_t>(2, "days", days);
 serializer.WritePropertyWithDefault<int64_t>(3, "micros", micros);
}
interval_t interval_t::Deserialize(Deserializer &deserializer) {
 interval_t result;
 deserializer.ReadPropertyWithDefault<int32_t>(1, "months", result.months);
 deserializer.ReadPropertyWithDefault<int32_t>(2, "days", result.days);
 deserializer.ReadPropertyWithDefault<int64_t>(3, "micros", result.micros);
 return result;
}
}
