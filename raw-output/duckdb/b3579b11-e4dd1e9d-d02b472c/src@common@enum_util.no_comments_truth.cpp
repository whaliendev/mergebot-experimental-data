#include "duckdb/common/enum_util.hpp"
#include "duckdb/catalog/catalog_entry/dependency/dependency_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_column_type.hpp"
#include "duckdb/common/box_renderer.hpp"
#include "duckdb/common/enums/access_mode.hpp"
#include "duckdb/common/enums/aggregate_handling.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/common/enums/compression_type.hpp"
#include "duckdb/common/enums/cte_materialize.hpp"
#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/enums/debug_initialize.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/enums/file_compression_type.hpp"
#include "duckdb/common/enums/file_glob_options.hpp"
#include "duckdb/common/enums/filter_propagate_result.hpp"
#include "duckdb/common/enums/index_constraint_type.hpp"
#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/common/enums/joinref_type.hpp"
#include "duckdb/common/enums/logical_operator_type.hpp"
#include "duckdb/common/enums/on_create_conflict.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"
#include "duckdb/common/enums/operator_result_type.hpp"
#include "duckdb/common/enums/optimizer_type.hpp"
#include "duckdb/common/enums/order_preservation_type.hpp"
#include "duckdb/common/enums/order_type.hpp"
#include "duckdb/common/enums/output_type.hpp"
#include "duckdb/common/enums/pending_execution_result.hpp"
#include "duckdb/common/enums/physical_operator_type.hpp"
#include "duckdb/common/enums/profiler_format.hpp"
#include "duckdb/common/enums/relation_type.hpp"
#include "duckdb/common/enums/scan_options.hpp"
#include "duckdb/common/enums/set_operation_type.hpp"
#include "duckdb/common/enums/set_scope.hpp"
#include "duckdb/common/enums/set_type.hpp"
#include "duckdb/common/enums/statement_type.hpp"
#include "duckdb/common/enums/subquery_type.hpp"
#include "duckdb/common/enums/tableref_type.hpp"
#include "duckdb/common/enums/undo_flags.hpp"
#include "duckdb/common/enums/vector_type.hpp"
#include "duckdb/common/enums/wal_type.hpp"
#include "duckdb/common/enums/window_aggregation_mode.hpp"
#include "duckdb/common/exception_format_value.hpp"
#include "duckdb/common/extra_type_info.hpp"
#include "duckdb/common/file_buffer.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/sort/partition_state.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/column/column_data_scan_states.hpp"
#include "duckdb/common/types/column/partitioned_column_data.hpp"
#include "duckdb/common/types/conflict_manager.hpp"
#include "duckdb/common/types/hyperloglog.hpp"
#include "duckdb/common/types/row/partitioned_tuple_data.hpp"
#include "duckdb/common/types/row/tuple_data_collection.hpp"
#include "duckdb/common/types/row/tuple_data_states.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/vector_buffer.hpp"
#include "duckdb/core_functions/aggregate/quantile_enum.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/node.hpp"
#include "duckdb/execution/operator/scan/csv/base_csv_reader.hpp"
#include "duckdb/execution/operator/scan/csv/csv_option.hpp"
#include "duckdb/execution/operator/scan/csv/csv_state.hpp"
#include "duckdb/execution/operator/scan/csv/quote_rules.hpp"
#include "duckdb/function/aggregate_state.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/function/macro_function.hpp"
#include "duckdb/function/scalar/compressed_materialization_functions.hpp"
#include "duckdb/function/scalar/strftime_format.hpp"
#include "duckdb/function/table/arrow/arrow_duck_schema.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/error_manager.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/parallel/interrupt.hpp"
#include "duckdb/parallel/task.hpp"
#include "duckdb/parser/constraint.hpp"
#include "duckdb/parser/expression/parameter_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/parser/parsed_data/alter_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/alter_table_function_info.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_sequence_info.hpp"
#include "duckdb/parser/parsed_data/load_info.hpp"
#include "duckdb/parser/parsed_data/parse_info.hpp"
#include "duckdb/parser/parsed_data/pragma_info.hpp"
#include "duckdb/parser/parsed_data/sample_options.hpp"
#include "duckdb/parser/parsed_data/transaction_info.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/simplified_token.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/compression/bitpacking.hpp"
#include "duckdb/storage/magic_bytes.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/table/chunk_info.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/verification/statement_verifier.hpp"
namespace duckdb {
template<>
const char* EnumUtil::ToChars<AccessMode>(AccessMode value) {
 switch(value) {
 case AccessMode::UNDEFINED:
  return "UNDEFINED";
 case AccessMode::AUTOMATIC:
  return "AUTOMATIC";
 case AccessMode::READ_ONLY:
  return "READ_ONLY";
 case AccessMode::READ_WRITE:
  return "READ_WRITE";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
AccessMode EnumUtil::FromString<AccessMode>(const char *value) {
 if (StringUtil::Equals(value, "UNDEFINED")) {
  return AccessMode::UNDEFINED;
 }
 if (StringUtil::Equals(value, "AUTOMATIC")) {
  return AccessMode::AUTOMATIC;
 }
 if (StringUtil::Equals(value, "READ_ONLY")) {
  return AccessMode::READ_ONLY;
 }
 if (StringUtil::Equals(value, "READ_WRITE")) {
  return AccessMode::READ_WRITE;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<AggregateHandling>(AggregateHandling value) {
 switch(value) {
 case AggregateHandling::STANDARD_HANDLING:
  return "STANDARD_HANDLING";
 case AggregateHandling::NO_AGGREGATES_ALLOWED:
  return "NO_AGGREGATES_ALLOWED";
 case AggregateHandling::FORCE_AGGREGATES:
  return "FORCE_AGGREGATES";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
AggregateHandling EnumUtil::FromString<AggregateHandling>(const char *value) {
 if (StringUtil::Equals(value, "STANDARD_HANDLING")) {
  return AggregateHandling::STANDARD_HANDLING;
 }
 if (StringUtil::Equals(value, "NO_AGGREGATES_ALLOWED")) {
  return AggregateHandling::NO_AGGREGATES_ALLOWED;
 }
 if (StringUtil::Equals(value, "FORCE_AGGREGATES")) {
  return AggregateHandling::FORCE_AGGREGATES;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<AggregateOrderDependent>(AggregateOrderDependent value) {
 switch(value) {
 case AggregateOrderDependent::ORDER_DEPENDENT:
  return "ORDER_DEPENDENT";
 case AggregateOrderDependent::NOT_ORDER_DEPENDENT:
  return "NOT_ORDER_DEPENDENT";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
AggregateOrderDependent EnumUtil::FromString<AggregateOrderDependent>(const char *value) {
 if (StringUtil::Equals(value, "ORDER_DEPENDENT")) {
  return AggregateOrderDependent::ORDER_DEPENDENT;
 }
 if (StringUtil::Equals(value, "NOT_ORDER_DEPENDENT")) {
  return AggregateOrderDependent::NOT_ORDER_DEPENDENT;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<AggregateType>(AggregateType value) {
 switch(value) {
 case AggregateType::NON_DISTINCT:
  return "NON_DISTINCT";
 case AggregateType::DISTINCT:
  return "DISTINCT";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
AggregateType EnumUtil::FromString<AggregateType>(const char *value) {
 if (StringUtil::Equals(value, "NON_DISTINCT")) {
  return AggregateType::NON_DISTINCT;
 }
 if (StringUtil::Equals(value, "DISTINCT")) {
  return AggregateType::DISTINCT;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<AlterForeignKeyType>(AlterForeignKeyType value) {
 switch(value) {
 case AlterForeignKeyType::AFT_ADD:
  return "AFT_ADD";
 case AlterForeignKeyType::AFT_DELETE:
  return "AFT_DELETE";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
AlterForeignKeyType EnumUtil::FromString<AlterForeignKeyType>(const char *value) {
 if (StringUtil::Equals(value, "AFT_ADD")) {
  return AlterForeignKeyType::AFT_ADD;
 }
 if (StringUtil::Equals(value, "AFT_DELETE")) {
  return AlterForeignKeyType::AFT_DELETE;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<AlterScalarFunctionType>(AlterScalarFunctionType value) {
 switch(value) {
 case AlterScalarFunctionType::INVALID:
  return "INVALID";
 case AlterScalarFunctionType::ADD_FUNCTION_OVERLOADS:
  return "ADD_FUNCTION_OVERLOADS";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
AlterScalarFunctionType EnumUtil::FromString<AlterScalarFunctionType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return AlterScalarFunctionType::INVALID;
 }
 if (StringUtil::Equals(value, "ADD_FUNCTION_OVERLOADS")) {
  return AlterScalarFunctionType::ADD_FUNCTION_OVERLOADS;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<AlterTableFunctionType>(AlterTableFunctionType value) {
 switch(value) {
 case AlterTableFunctionType::INVALID:
  return "INVALID";
 case AlterTableFunctionType::ADD_FUNCTION_OVERLOADS:
  return "ADD_FUNCTION_OVERLOADS";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
AlterTableFunctionType EnumUtil::FromString<AlterTableFunctionType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return AlterTableFunctionType::INVALID;
 }
 if (StringUtil::Equals(value, "ADD_FUNCTION_OVERLOADS")) {
  return AlterTableFunctionType::ADD_FUNCTION_OVERLOADS;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<AlterTableType>(AlterTableType value) {
 switch(value) {
 case AlterTableType::INVALID:
  return "INVALID";
 case AlterTableType::RENAME_COLUMN:
  return "RENAME_COLUMN";
 case AlterTableType::RENAME_TABLE:
  return "RENAME_TABLE";
 case AlterTableType::ADD_COLUMN:
  return "ADD_COLUMN";
 case AlterTableType::REMOVE_COLUMN:
  return "REMOVE_COLUMN";
 case AlterTableType::ALTER_COLUMN_TYPE:
  return "ALTER_COLUMN_TYPE";
 case AlterTableType::SET_DEFAULT:
  return "SET_DEFAULT";
 case AlterTableType::FOREIGN_KEY_CONSTRAINT:
  return "FOREIGN_KEY_CONSTRAINT";
 case AlterTableType::SET_NOT_NULL:
  return "SET_NOT_NULL";
 case AlterTableType::DROP_NOT_NULL:
  return "DROP_NOT_NULL";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
AlterTableType EnumUtil::FromString<AlterTableType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return AlterTableType::INVALID;
 }
 if (StringUtil::Equals(value, "RENAME_COLUMN")) {
  return AlterTableType::RENAME_COLUMN;
 }
 if (StringUtil::Equals(value, "RENAME_TABLE")) {
  return AlterTableType::RENAME_TABLE;
 }
 if (StringUtil::Equals(value, "ADD_COLUMN")) {
  return AlterTableType::ADD_COLUMN;
 }
 if (StringUtil::Equals(value, "REMOVE_COLUMN")) {
  return AlterTableType::REMOVE_COLUMN;
 }
 if (StringUtil::Equals(value, "ALTER_COLUMN_TYPE")) {
  return AlterTableType::ALTER_COLUMN_TYPE;
 }
 if (StringUtil::Equals(value, "SET_DEFAULT")) {
  return AlterTableType::SET_DEFAULT;
 }
 if (StringUtil::Equals(value, "FOREIGN_KEY_CONSTRAINT")) {
  return AlterTableType::FOREIGN_KEY_CONSTRAINT;
 }
 if (StringUtil::Equals(value, "SET_NOT_NULL")) {
  return AlterTableType::SET_NOT_NULL;
 }
 if (StringUtil::Equals(value, "DROP_NOT_NULL")) {
  return AlterTableType::DROP_NOT_NULL;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<AlterType>(AlterType value) {
 switch(value) {
 case AlterType::INVALID:
  return "INVALID";
 case AlterType::ALTER_TABLE:
  return "ALTER_TABLE";
 case AlterType::ALTER_VIEW:
  return "ALTER_VIEW";
 case AlterType::ALTER_SEQUENCE:
  return "ALTER_SEQUENCE";
 case AlterType::CHANGE_OWNERSHIP:
  return "CHANGE_OWNERSHIP";
 case AlterType::ALTER_SCALAR_FUNCTION:
  return "ALTER_SCALAR_FUNCTION";
 case AlterType::ALTER_TABLE_FUNCTION:
  return "ALTER_TABLE_FUNCTION";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
AlterType EnumUtil::FromString<AlterType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return AlterType::INVALID;
 }
 if (StringUtil::Equals(value, "ALTER_TABLE")) {
  return AlterType::ALTER_TABLE;
 }
 if (StringUtil::Equals(value, "ALTER_VIEW")) {
  return AlterType::ALTER_VIEW;
 }
 if (StringUtil::Equals(value, "ALTER_SEQUENCE")) {
  return AlterType::ALTER_SEQUENCE;
 }
 if (StringUtil::Equals(value, "CHANGE_OWNERSHIP")) {
  return AlterType::CHANGE_OWNERSHIP;
 }
 if (StringUtil::Equals(value, "ALTER_SCALAR_FUNCTION")) {
  return AlterType::ALTER_SCALAR_FUNCTION;
 }
 if (StringUtil::Equals(value, "ALTER_TABLE_FUNCTION")) {
  return AlterType::ALTER_TABLE_FUNCTION;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<AlterViewType>(AlterViewType value) {
 switch(value) {
 case AlterViewType::INVALID:
  return "INVALID";
 case AlterViewType::RENAME_VIEW:
  return "RENAME_VIEW";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
AlterViewType EnumUtil::FromString<AlterViewType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return AlterViewType::INVALID;
 }
 if (StringUtil::Equals(value, "RENAME_VIEW")) {
  return AlterViewType::RENAME_VIEW;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<AppenderType>(AppenderType value) {
 switch(value) {
 case AppenderType::LOGICAL:
  return "LOGICAL";
 case AppenderType::PHYSICAL:
  return "PHYSICAL";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
AppenderType EnumUtil::FromString<AppenderType>(const char *value) {
 if (StringUtil::Equals(value, "LOGICAL")) {
  return AppenderType::LOGICAL;
 }
 if (StringUtil::Equals(value, "PHYSICAL")) {
  return AppenderType::PHYSICAL;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ArrowDateTimeType>(ArrowDateTimeType value) {
 switch(value) {
 case ArrowDateTimeType::MILLISECONDS:
  return "MILLISECONDS";
 case ArrowDateTimeType::MICROSECONDS:
  return "MICROSECONDS";
 case ArrowDateTimeType::NANOSECONDS:
  return "NANOSECONDS";
 case ArrowDateTimeType::SECONDS:
  return "SECONDS";
 case ArrowDateTimeType::DAYS:
  return "DAYS";
 case ArrowDateTimeType::MONTHS:
  return "MONTHS";
 case ArrowDateTimeType::MONTH_DAY_NANO:
  return "MONTH_DAY_NANO";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ArrowDateTimeType EnumUtil::FromString<ArrowDateTimeType>(const char *value) {
 if (StringUtil::Equals(value, "MILLISECONDS")) {
  return ArrowDateTimeType::MILLISECONDS;
 }
 if (StringUtil::Equals(value, "MICROSECONDS")) {
  return ArrowDateTimeType::MICROSECONDS;
 }
 if (StringUtil::Equals(value, "NANOSECONDS")) {
  return ArrowDateTimeType::NANOSECONDS;
 }
 if (StringUtil::Equals(value, "SECONDS")) {
  return ArrowDateTimeType::SECONDS;
 }
 if (StringUtil::Equals(value, "DAYS")) {
  return ArrowDateTimeType::DAYS;
 }
 if (StringUtil::Equals(value, "MONTHS")) {
  return ArrowDateTimeType::MONTHS;
 }
 if (StringUtil::Equals(value, "MONTH_DAY_NANO")) {
  return ArrowDateTimeType::MONTH_DAY_NANO;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ArrowVariableSizeType>(ArrowVariableSizeType value) {
 switch(value) {
 case ArrowVariableSizeType::FIXED_SIZE:
  return "FIXED_SIZE";
 case ArrowVariableSizeType::NORMAL:
  return "NORMAL";
 case ArrowVariableSizeType::SUPER_SIZE:
  return "SUPER_SIZE";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ArrowVariableSizeType EnumUtil::FromString<ArrowVariableSizeType>(const char *value) {
 if (StringUtil::Equals(value, "FIXED_SIZE")) {
  return ArrowVariableSizeType::FIXED_SIZE;
 }
 if (StringUtil::Equals(value, "NORMAL")) {
  return ArrowVariableSizeType::NORMAL;
 }
 if (StringUtil::Equals(value, "SUPER_SIZE")) {
  return ArrowVariableSizeType::SUPER_SIZE;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<BindingMode>(BindingMode value) {
 switch(value) {
 case BindingMode::STANDARD_BINDING:
  return "STANDARD_BINDING";
 case BindingMode::EXTRACT_NAMES:
  return "EXTRACT_NAMES";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
BindingMode EnumUtil::FromString<BindingMode>(const char *value) {
 if (StringUtil::Equals(value, "STANDARD_BINDING")) {
  return BindingMode::STANDARD_BINDING;
 }
 if (StringUtil::Equals(value, "EXTRACT_NAMES")) {
  return BindingMode::EXTRACT_NAMES;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<BitpackingMode>(BitpackingMode value) {
 switch(value) {
 case BitpackingMode::INVALID:
  return "INVALID";
 case BitpackingMode::AUTO:
  return "AUTO";
 case BitpackingMode::CONSTANT:
  return "CONSTANT";
 case BitpackingMode::CONSTANT_DELTA:
  return "CONSTANT_DELTA";
 case BitpackingMode::DELTA_FOR:
  return "DELTA_FOR";
 case BitpackingMode::FOR:
  return "FOR";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
BitpackingMode EnumUtil::FromString<BitpackingMode>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return BitpackingMode::INVALID;
 }
 if (StringUtil::Equals(value, "AUTO")) {
  return BitpackingMode::AUTO;
 }
 if (StringUtil::Equals(value, "CONSTANT")) {
  return BitpackingMode::CONSTANT;
 }
 if (StringUtil::Equals(value, "CONSTANT_DELTA")) {
  return BitpackingMode::CONSTANT_DELTA;
 }
 if (StringUtil::Equals(value, "DELTA_FOR")) {
  return BitpackingMode::DELTA_FOR;
 }
 if (StringUtil::Equals(value, "FOR")) {
  return BitpackingMode::FOR;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<BlockState>(BlockState value) {
 switch(value) {
 case BlockState::BLOCK_UNLOADED:
  return "BLOCK_UNLOADED";
 case BlockState::BLOCK_LOADED:
  return "BLOCK_LOADED";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
BlockState EnumUtil::FromString<BlockState>(const char *value) {
 if (StringUtil::Equals(value, "BLOCK_UNLOADED")) {
  return BlockState::BLOCK_UNLOADED;
 }
 if (StringUtil::Equals(value, "BLOCK_LOADED")) {
  return BlockState::BLOCK_LOADED;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<CAPIResultSetType>(CAPIResultSetType value) {
 switch(value) {
 case CAPIResultSetType::CAPI_RESULT_TYPE_NONE:
  return "CAPI_RESULT_TYPE_NONE";
 case CAPIResultSetType::CAPI_RESULT_TYPE_MATERIALIZED:
  return "CAPI_RESULT_TYPE_MATERIALIZED";
 case CAPIResultSetType::CAPI_RESULT_TYPE_STREAMING:
  return "CAPI_RESULT_TYPE_STREAMING";
 case CAPIResultSetType::CAPI_RESULT_TYPE_DEPRECATED:
  return "CAPI_RESULT_TYPE_DEPRECATED";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
CAPIResultSetType EnumUtil::FromString<CAPIResultSetType>(const char *value) {
 if (StringUtil::Equals(value, "CAPI_RESULT_TYPE_NONE")) {
  return CAPIResultSetType::CAPI_RESULT_TYPE_NONE;
 }
 if (StringUtil::Equals(value, "CAPI_RESULT_TYPE_MATERIALIZED")) {
  return CAPIResultSetType::CAPI_RESULT_TYPE_MATERIALIZED;
 }
 if (StringUtil::Equals(value, "CAPI_RESULT_TYPE_STREAMING")) {
  return CAPIResultSetType::CAPI_RESULT_TYPE_STREAMING;
 }
 if (StringUtil::Equals(value, "CAPI_RESULT_TYPE_DEPRECATED")) {
  return CAPIResultSetType::CAPI_RESULT_TYPE_DEPRECATED;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<CSVState>(CSVState value) {
 switch(value) {
 case CSVState::STANDARD:
  return "STANDARD";
 case CSVState::DELIMITER:
  return "DELIMITER";
 case CSVState::RECORD_SEPARATOR:
  return "RECORD_SEPARATOR";
 case CSVState::CARRIAGE_RETURN:
  return "CARRIAGE_RETURN";
 case CSVState::QUOTED:
  return "QUOTED";
 case CSVState::UNQUOTED:
  return "UNQUOTED";
 case CSVState::ESCAPE:
  return "ESCAPE";
 case CSVState::EMPTY_LINE:
  return "EMPTY_LINE";
 case CSVState::INVALID:
  return "INVALID";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
CSVState EnumUtil::FromString<CSVState>(const char *value) {
 if (StringUtil::Equals(value, "STANDARD")) {
  return CSVState::STANDARD;
 }
 if (StringUtil::Equals(value, "DELIMITER")) {
  return CSVState::DELIMITER;
 }
 if (StringUtil::Equals(value, "RECORD_SEPARATOR")) {
  return CSVState::RECORD_SEPARATOR;
 }
 if (StringUtil::Equals(value, "CARRIAGE_RETURN")) {
  return CSVState::CARRIAGE_RETURN;
 }
 if (StringUtil::Equals(value, "QUOTED")) {
  return CSVState::QUOTED;
 }
 if (StringUtil::Equals(value, "UNQUOTED")) {
  return CSVState::UNQUOTED;
 }
 if (StringUtil::Equals(value, "ESCAPE")) {
  return CSVState::ESCAPE;
 }
 if (StringUtil::Equals(value, "EMPTY_LINE")) {
  return CSVState::EMPTY_LINE;
 }
 if (StringUtil::Equals(value, "INVALID")) {
  return CSVState::INVALID;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<CTEMaterialize>(CTEMaterialize value) {
 switch(value) {
 case CTEMaterialize::CTE_MATERIALIZE_DEFAULT:
  return "CTE_MATERIALIZE_DEFAULT";
 case CTEMaterialize::CTE_MATERIALIZE_ALWAYS:
  return "CTE_MATERIALIZE_ALWAYS";
 case CTEMaterialize::CTE_MATERIALIZE_NEVER:
  return "CTE_MATERIALIZE_NEVER";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
CTEMaterialize EnumUtil::FromString<CTEMaterialize>(const char *value) {
 if (StringUtil::Equals(value, "CTE_MATERIALIZE_DEFAULT")) {
  return CTEMaterialize::CTE_MATERIALIZE_DEFAULT;
 }
 if (StringUtil::Equals(value, "CTE_MATERIALIZE_ALWAYS")) {
  return CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
 }
 if (StringUtil::Equals(value, "CTE_MATERIALIZE_NEVER")) {
  return CTEMaterialize::CTE_MATERIALIZE_NEVER;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<CatalogType>(CatalogType value) {
 switch(value) {
 case CatalogType::INVALID:
  return "INVALID";
 case CatalogType::TABLE_ENTRY:
  return "TABLE_ENTRY";
 case CatalogType::SCHEMA_ENTRY:
  return "SCHEMA_ENTRY";
 case CatalogType::VIEW_ENTRY:
  return "VIEW_ENTRY";
 case CatalogType::INDEX_ENTRY:
  return "INDEX_ENTRY";
 case CatalogType::PREPARED_STATEMENT:
  return "PREPARED_STATEMENT";
 case CatalogType::SEQUENCE_ENTRY:
  return "SEQUENCE_ENTRY";
 case CatalogType::COLLATION_ENTRY:
  return "COLLATION_ENTRY";
 case CatalogType::TYPE_ENTRY:
  return "TYPE_ENTRY";
 case CatalogType::DATABASE_ENTRY:
  return "DATABASE_ENTRY";
 case CatalogType::TABLE_FUNCTION_ENTRY:
  return "TABLE_FUNCTION_ENTRY";
 case CatalogType::SCALAR_FUNCTION_ENTRY:
  return "SCALAR_FUNCTION_ENTRY";
 case CatalogType::AGGREGATE_FUNCTION_ENTRY:
  return "AGGREGATE_FUNCTION_ENTRY";
 case CatalogType::PRAGMA_FUNCTION_ENTRY:
  return "PRAGMA_FUNCTION_ENTRY";
 case CatalogType::COPY_FUNCTION_ENTRY:
  return "COPY_FUNCTION_ENTRY";
 case CatalogType::MACRO_ENTRY:
  return "MACRO_ENTRY";
 case CatalogType::TABLE_MACRO_ENTRY:
  return "TABLE_MACRO_ENTRY";
 case CatalogType::DELETED_ENTRY:
  return "DELETED_ENTRY";
 case CatalogType::RENAMED_ENTRY:
  return "RENAMED_ENTRY";
 case CatalogType::DEPENDENCY_ENTRY:
  return "DEPENDENCY_ENTRY";
 case CatalogType::SECRET_ENTRY:
  return "SECRET_ENTRY";
 case CatalogType::SECRET_TYPE_ENTRY:
  return "SECRET_TYPE_ENTRY";
 case CatalogType::SECRET_FUNCTION_ENTRY:
  return "SECRET_FUNCTION_ENTRY";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
CatalogType EnumUtil::FromString<CatalogType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return CatalogType::INVALID;
 }
 if (StringUtil::Equals(value, "TABLE_ENTRY")) {
  return CatalogType::TABLE_ENTRY;
 }
 if (StringUtil::Equals(value, "SCHEMA_ENTRY")) {
  return CatalogType::SCHEMA_ENTRY;
 }
 if (StringUtil::Equals(value, "VIEW_ENTRY")) {
  return CatalogType::VIEW_ENTRY;
 }
 if (StringUtil::Equals(value, "INDEX_ENTRY")) {
  return CatalogType::INDEX_ENTRY;
 }
 if (StringUtil::Equals(value, "PREPARED_STATEMENT")) {
  return CatalogType::PREPARED_STATEMENT;
 }
 if (StringUtil::Equals(value, "SEQUENCE_ENTRY")) {
  return CatalogType::SEQUENCE_ENTRY;
 }
 if (StringUtil::Equals(value, "COLLATION_ENTRY")) {
  return CatalogType::COLLATION_ENTRY;
 }
 if (StringUtil::Equals(value, "TYPE_ENTRY")) {
  return CatalogType::TYPE_ENTRY;
 }
 if (StringUtil::Equals(value, "DATABASE_ENTRY")) {
  return CatalogType::DATABASE_ENTRY;
 }
 if (StringUtil::Equals(value, "TABLE_FUNCTION_ENTRY")) {
  return CatalogType::TABLE_FUNCTION_ENTRY;
 }
 if (StringUtil::Equals(value, "SCALAR_FUNCTION_ENTRY")) {
  return CatalogType::SCALAR_FUNCTION_ENTRY;
 }
 if (StringUtil::Equals(value, "AGGREGATE_FUNCTION_ENTRY")) {
  return CatalogType::AGGREGATE_FUNCTION_ENTRY;
 }
 if (StringUtil::Equals(value, "PRAGMA_FUNCTION_ENTRY")) {
  return CatalogType::PRAGMA_FUNCTION_ENTRY;
 }
 if (StringUtil::Equals(value, "COPY_FUNCTION_ENTRY")) {
  return CatalogType::COPY_FUNCTION_ENTRY;
 }
 if (StringUtil::Equals(value, "MACRO_ENTRY")) {
  return CatalogType::MACRO_ENTRY;
 }
 if (StringUtil::Equals(value, "TABLE_MACRO_ENTRY")) {
  return CatalogType::TABLE_MACRO_ENTRY;
 }
 if (StringUtil::Equals(value, "DELETED_ENTRY")) {
  return CatalogType::DELETED_ENTRY;
 }
 if (StringUtil::Equals(value, "RENAMED_ENTRY")) {
  return CatalogType::RENAMED_ENTRY;
 }
 if (StringUtil::Equals(value, "DEPENDENCY_ENTRY")) {
  return CatalogType::DEPENDENCY_ENTRY;
 }
 if (StringUtil::Equals(value, "SECRET_ENTRY")) {
  return CatalogType::SECRET_ENTRY;
 }
 if (StringUtil::Equals(value, "SECRET_TYPE_ENTRY")) {
  return CatalogType::SECRET_TYPE_ENTRY;
 }
 if (StringUtil::Equals(value, "SECRET_FUNCTION_ENTRY")) {
  return CatalogType::SECRET_FUNCTION_ENTRY;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<CheckpointAbort>(CheckpointAbort value) {
 switch(value) {
 case CheckpointAbort::NO_ABORT:
  return "NO_ABORT";
 case CheckpointAbort::DEBUG_ABORT_BEFORE_TRUNCATE:
  return "DEBUG_ABORT_BEFORE_TRUNCATE";
 case CheckpointAbort::DEBUG_ABORT_BEFORE_HEADER:
  return "DEBUG_ABORT_BEFORE_HEADER";
 case CheckpointAbort::DEBUG_ABORT_AFTER_FREE_LIST_WRITE:
  return "DEBUG_ABORT_AFTER_FREE_LIST_WRITE";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
CheckpointAbort EnumUtil::FromString<CheckpointAbort>(const char *value) {
 if (StringUtil::Equals(value, "NO_ABORT")) {
  return CheckpointAbort::NO_ABORT;
 }
 if (StringUtil::Equals(value, "DEBUG_ABORT_BEFORE_TRUNCATE")) {
  return CheckpointAbort::DEBUG_ABORT_BEFORE_TRUNCATE;
 }
 if (StringUtil::Equals(value, "DEBUG_ABORT_BEFORE_HEADER")) {
  return CheckpointAbort::DEBUG_ABORT_BEFORE_HEADER;
 }
 if (StringUtil::Equals(value, "DEBUG_ABORT_AFTER_FREE_LIST_WRITE")) {
  return CheckpointAbort::DEBUG_ABORT_AFTER_FREE_LIST_WRITE;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ChunkInfoType>(ChunkInfoType value) {
 switch(value) {
 case ChunkInfoType::CONSTANT_INFO:
  return "CONSTANT_INFO";
 case ChunkInfoType::VECTOR_INFO:
  return "VECTOR_INFO";
 case ChunkInfoType::EMPTY_INFO:
  return "EMPTY_INFO";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ChunkInfoType EnumUtil::FromString<ChunkInfoType>(const char *value) {
 if (StringUtil::Equals(value, "CONSTANT_INFO")) {
  return ChunkInfoType::CONSTANT_INFO;
 }
 if (StringUtil::Equals(value, "VECTOR_INFO")) {
  return ChunkInfoType::VECTOR_INFO;
 }
 if (StringUtil::Equals(value, "EMPTY_INFO")) {
  return ChunkInfoType::EMPTY_INFO;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ColumnDataAllocatorType>(ColumnDataAllocatorType value) {
 switch(value) {
 case ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR:
  return "BUFFER_MANAGER_ALLOCATOR";
 case ColumnDataAllocatorType::IN_MEMORY_ALLOCATOR:
  return "IN_MEMORY_ALLOCATOR";
 case ColumnDataAllocatorType::HYBRID:
  return "HYBRID";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ColumnDataAllocatorType EnumUtil::FromString<ColumnDataAllocatorType>(const char *value) {
 if (StringUtil::Equals(value, "BUFFER_MANAGER_ALLOCATOR")) {
  return ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR;
 }
 if (StringUtil::Equals(value, "IN_MEMORY_ALLOCATOR")) {
  return ColumnDataAllocatorType::IN_MEMORY_ALLOCATOR;
 }
 if (StringUtil::Equals(value, "HYBRID")) {
  return ColumnDataAllocatorType::HYBRID;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ColumnDataScanProperties>(ColumnDataScanProperties value) {
 switch(value) {
 case ColumnDataScanProperties::INVALID:
  return "INVALID";
 case ColumnDataScanProperties::ALLOW_ZERO_COPY:
  return "ALLOW_ZERO_COPY";
 case ColumnDataScanProperties::DISALLOW_ZERO_COPY:
  return "DISALLOW_ZERO_COPY";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ColumnDataScanProperties EnumUtil::FromString<ColumnDataScanProperties>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return ColumnDataScanProperties::INVALID;
 }
 if (StringUtil::Equals(value, "ALLOW_ZERO_COPY")) {
  return ColumnDataScanProperties::ALLOW_ZERO_COPY;
 }
 if (StringUtil::Equals(value, "DISALLOW_ZERO_COPY")) {
  return ColumnDataScanProperties::DISALLOW_ZERO_COPY;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ColumnSegmentType>(ColumnSegmentType value) {
 switch(value) {
 case ColumnSegmentType::TRANSIENT:
  return "TRANSIENT";
 case ColumnSegmentType::PERSISTENT:
  return "PERSISTENT";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ColumnSegmentType EnumUtil::FromString<ColumnSegmentType>(const char *value) {
 if (StringUtil::Equals(value, "TRANSIENT")) {
  return ColumnSegmentType::TRANSIENT;
 }
 if (StringUtil::Equals(value, "PERSISTENT")) {
  return ColumnSegmentType::PERSISTENT;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<CompressedMaterializationDirection>(CompressedMaterializationDirection value) {
 switch(value) {
 case CompressedMaterializationDirection::INVALID:
  return "INVALID";
 case CompressedMaterializationDirection::COMPRESS:
  return "COMPRESS";
 case CompressedMaterializationDirection::DECOMPRESS:
  return "DECOMPRESS";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
CompressedMaterializationDirection EnumUtil::FromString<CompressedMaterializationDirection>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return CompressedMaterializationDirection::INVALID;
 }
 if (StringUtil::Equals(value, "COMPRESS")) {
  return CompressedMaterializationDirection::COMPRESS;
 }
 if (StringUtil::Equals(value, "DECOMPRESS")) {
  return CompressedMaterializationDirection::DECOMPRESS;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<CompressionType>(CompressionType value) {
 switch(value) {
 case CompressionType::COMPRESSION_AUTO:
  return "COMPRESSION_AUTO";
 case CompressionType::COMPRESSION_UNCOMPRESSED:
  return "COMPRESSION_UNCOMPRESSED";
 case CompressionType::COMPRESSION_CONSTANT:
  return "COMPRESSION_CONSTANT";
 case CompressionType::COMPRESSION_RLE:
  return "COMPRESSION_RLE";
 case CompressionType::COMPRESSION_DICTIONARY:
  return "COMPRESSION_DICTIONARY";
 case CompressionType::COMPRESSION_PFOR_DELTA:
  return "COMPRESSION_PFOR_DELTA";
 case CompressionType::COMPRESSION_BITPACKING:
  return "COMPRESSION_BITPACKING";
 case CompressionType::COMPRESSION_FSST:
  return "COMPRESSION_FSST";
 case CompressionType::COMPRESSION_CHIMP:
  return "COMPRESSION_CHIMP";
 case CompressionType::COMPRESSION_PATAS:
  return "COMPRESSION_PATAS";
 case CompressionType::COMPRESSION_COUNT:
  return "COMPRESSION_COUNT";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
CompressionType EnumUtil::FromString<CompressionType>(const char *value) {
 if (StringUtil::Equals(value, "COMPRESSION_AUTO")) {
  return CompressionType::COMPRESSION_AUTO;
 }
 if (StringUtil::Equals(value, "COMPRESSION_UNCOMPRESSED")) {
  return CompressionType::COMPRESSION_UNCOMPRESSED;
 }
 if (StringUtil::Equals(value, "COMPRESSION_CONSTANT")) {
  return CompressionType::COMPRESSION_CONSTANT;
 }
 if (StringUtil::Equals(value, "COMPRESSION_RLE")) {
  return CompressionType::COMPRESSION_RLE;
 }
 if (StringUtil::Equals(value, "COMPRESSION_DICTIONARY")) {
  return CompressionType::COMPRESSION_DICTIONARY;
 }
 if (StringUtil::Equals(value, "COMPRESSION_PFOR_DELTA")) {
  return CompressionType::COMPRESSION_PFOR_DELTA;
 }
 if (StringUtil::Equals(value, "COMPRESSION_BITPACKING")) {
  return CompressionType::COMPRESSION_BITPACKING;
 }
 if (StringUtil::Equals(value, "COMPRESSION_FSST")) {
  return CompressionType::COMPRESSION_FSST;
 }
 if (StringUtil::Equals(value, "COMPRESSION_CHIMP")) {
  return CompressionType::COMPRESSION_CHIMP;
 }
 if (StringUtil::Equals(value, "COMPRESSION_PATAS")) {
  return CompressionType::COMPRESSION_PATAS;
 }
 if (StringUtil::Equals(value, "COMPRESSION_COUNT")) {
  return CompressionType::COMPRESSION_COUNT;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ConflictManagerMode>(ConflictManagerMode value) {
 switch(value) {
 case ConflictManagerMode::SCAN:
  return "SCAN";
 case ConflictManagerMode::THROW:
  return "THROW";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ConflictManagerMode EnumUtil::FromString<ConflictManagerMode>(const char *value) {
 if (StringUtil::Equals(value, "SCAN")) {
  return ConflictManagerMode::SCAN;
 }
 if (StringUtil::Equals(value, "THROW")) {
  return ConflictManagerMode::THROW;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ConstraintType>(ConstraintType value) {
 switch(value) {
 case ConstraintType::INVALID:
  return "INVALID";
 case ConstraintType::NOT_NULL:
  return "NOT_NULL";
 case ConstraintType::CHECK:
  return "CHECK";
 case ConstraintType::UNIQUE:
  return "UNIQUE";
 case ConstraintType::FOREIGN_KEY:
  return "FOREIGN_KEY";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ConstraintType EnumUtil::FromString<ConstraintType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return ConstraintType::INVALID;
 }
 if (StringUtil::Equals(value, "NOT_NULL")) {
  return ConstraintType::NOT_NULL;
 }
 if (StringUtil::Equals(value, "CHECK")) {
  return ConstraintType::CHECK;
 }
 if (StringUtil::Equals(value, "UNIQUE")) {
  return ConstraintType::UNIQUE;
 }
 if (StringUtil::Equals(value, "FOREIGN_KEY")) {
  return ConstraintType::FOREIGN_KEY;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<DataFileType>(DataFileType value) {
 switch(value) {
 case DataFileType::FILE_DOES_NOT_EXIST:
  return "FILE_DOES_NOT_EXIST";
 case DataFileType::DUCKDB_FILE:
  return "DUCKDB_FILE";
 case DataFileType::SQLITE_FILE:
  return "SQLITE_FILE";
 case DataFileType::PARQUET_FILE:
  return "PARQUET_FILE";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
DataFileType EnumUtil::FromString<DataFileType>(const char *value) {
 if (StringUtil::Equals(value, "FILE_DOES_NOT_EXIST")) {
  return DataFileType::FILE_DOES_NOT_EXIST;
 }
 if (StringUtil::Equals(value, "DUCKDB_FILE")) {
  return DataFileType::DUCKDB_FILE;
 }
 if (StringUtil::Equals(value, "SQLITE_FILE")) {
  return DataFileType::SQLITE_FILE;
 }
 if (StringUtil::Equals(value, "PARQUET_FILE")) {
  return DataFileType::PARQUET_FILE;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<DatePartSpecifier>(DatePartSpecifier value) {
 switch(value) {
 case DatePartSpecifier::YEAR:
  return "YEAR";
 case DatePartSpecifier::MONTH:
  return "MONTH";
 case DatePartSpecifier::DAY:
  return "DAY";
 case DatePartSpecifier::DECADE:
  return "DECADE";
 case DatePartSpecifier::CENTURY:
  return "CENTURY";
 case DatePartSpecifier::MILLENNIUM:
  return "MILLENNIUM";
 case DatePartSpecifier::MICROSECONDS:
  return "MICROSECONDS";
 case DatePartSpecifier::MILLISECONDS:
  return "MILLISECONDS";
 case DatePartSpecifier::SECOND:
  return "SECOND";
 case DatePartSpecifier::MINUTE:
  return "MINUTE";
 case DatePartSpecifier::HOUR:
  return "HOUR";
 case DatePartSpecifier::DOW:
  return "DOW";
 case DatePartSpecifier::ISODOW:
  return "ISODOW";
 case DatePartSpecifier::WEEK:
  return "WEEK";
 case DatePartSpecifier::ISOYEAR:
  return "ISOYEAR";
 case DatePartSpecifier::QUARTER:
  return "QUARTER";
 case DatePartSpecifier::DOY:
  return "DOY";
 case DatePartSpecifier::YEARWEEK:
  return "YEARWEEK";
 case DatePartSpecifier::ERA:
  return "ERA";
 case DatePartSpecifier::TIMEZONE:
  return "TIMEZONE";
 case DatePartSpecifier::TIMEZONE_HOUR:
  return "TIMEZONE_HOUR";
 case DatePartSpecifier::TIMEZONE_MINUTE:
  return "TIMEZONE_MINUTE";
 case DatePartSpecifier::EPOCH:
  return "EPOCH";
 case DatePartSpecifier::JULIAN_DAY:
  return "JULIAN_DAY";
 case DatePartSpecifier::INVALID:
  return "INVALID";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
DatePartSpecifier EnumUtil::FromString<DatePartSpecifier>(const char *value) {
 if (StringUtil::Equals(value, "YEAR")) {
  return DatePartSpecifier::YEAR;
 }
 if (StringUtil::Equals(value, "MONTH")) {
  return DatePartSpecifier::MONTH;
 }
 if (StringUtil::Equals(value, "DAY")) {
  return DatePartSpecifier::DAY;
 }
 if (StringUtil::Equals(value, "DECADE")) {
  return DatePartSpecifier::DECADE;
 }
 if (StringUtil::Equals(value, "CENTURY")) {
  return DatePartSpecifier::CENTURY;
 }
 if (StringUtil::Equals(value, "MILLENNIUM")) {
  return DatePartSpecifier::MILLENNIUM;
 }
 if (StringUtil::Equals(value, "MICROSECONDS")) {
  return DatePartSpecifier::MICROSECONDS;
 }
 if (StringUtil::Equals(value, "MILLISECONDS")) {
  return DatePartSpecifier::MILLISECONDS;
 }
 if (StringUtil::Equals(value, "SECOND")) {
  return DatePartSpecifier::SECOND;
 }
 if (StringUtil::Equals(value, "MINUTE")) {
  return DatePartSpecifier::MINUTE;
 }
 if (StringUtil::Equals(value, "HOUR")) {
  return DatePartSpecifier::HOUR;
 }
 if (StringUtil::Equals(value, "DOW")) {
  return DatePartSpecifier::DOW;
 }
 if (StringUtil::Equals(value, "ISODOW")) {
  return DatePartSpecifier::ISODOW;
 }
 if (StringUtil::Equals(value, "WEEK")) {
  return DatePartSpecifier::WEEK;
 }
 if (StringUtil::Equals(value, "ISOYEAR")) {
  return DatePartSpecifier::ISOYEAR;
 }
 if (StringUtil::Equals(value, "QUARTER")) {
  return DatePartSpecifier::QUARTER;
 }
 if (StringUtil::Equals(value, "DOY")) {
  return DatePartSpecifier::DOY;
 }
 if (StringUtil::Equals(value, "YEARWEEK")) {
  return DatePartSpecifier::YEARWEEK;
 }
 if (StringUtil::Equals(value, "ERA")) {
  return DatePartSpecifier::ERA;
 }
 if (StringUtil::Equals(value, "TIMEZONE")) {
  return DatePartSpecifier::TIMEZONE;
 }
 if (StringUtil::Equals(value, "TIMEZONE_HOUR")) {
  return DatePartSpecifier::TIMEZONE_HOUR;
 }
 if (StringUtil::Equals(value, "TIMEZONE_MINUTE")) {
  return DatePartSpecifier::TIMEZONE_MINUTE;
 }
 if (StringUtil::Equals(value, "EPOCH")) {
  return DatePartSpecifier::EPOCH;
 }
 if (StringUtil::Equals(value, "JULIAN_DAY")) {
  return DatePartSpecifier::JULIAN_DAY;
 }
 if (StringUtil::Equals(value, "INVALID")) {
  return DatePartSpecifier::INVALID;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<DebugInitialize>(DebugInitialize value) {
 switch(value) {
 case DebugInitialize::NO_INITIALIZE:
  return "NO_INITIALIZE";
 case DebugInitialize::DEBUG_ZERO_INITIALIZE:
  return "DEBUG_ZERO_INITIALIZE";
 case DebugInitialize::DEBUG_ONE_INITIALIZE:
  return "DEBUG_ONE_INITIALIZE";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
DebugInitialize EnumUtil::FromString<DebugInitialize>(const char *value) {
 if (StringUtil::Equals(value, "NO_INITIALIZE")) {
  return DebugInitialize::NO_INITIALIZE;
 }
 if (StringUtil::Equals(value, "DEBUG_ZERO_INITIALIZE")) {
  return DebugInitialize::DEBUG_ZERO_INITIALIZE;
 }
 if (StringUtil::Equals(value, "DEBUG_ONE_INITIALIZE")) {
  return DebugInitialize::DEBUG_ONE_INITIALIZE;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<DefaultOrderByNullType>(DefaultOrderByNullType value) {
 switch(value) {
 case DefaultOrderByNullType::INVALID:
  return "INVALID";
 case DefaultOrderByNullType::NULLS_FIRST:
  return "NULLS_FIRST";
 case DefaultOrderByNullType::NULLS_LAST:
  return "NULLS_LAST";
 case DefaultOrderByNullType::NULLS_FIRST_ON_ASC_LAST_ON_DESC:
  return "NULLS_FIRST_ON_ASC_LAST_ON_DESC";
 case DefaultOrderByNullType::NULLS_LAST_ON_ASC_FIRST_ON_DESC:
  return "NULLS_LAST_ON_ASC_FIRST_ON_DESC";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
DefaultOrderByNullType EnumUtil::FromString<DefaultOrderByNullType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return DefaultOrderByNullType::INVALID;
 }
 if (StringUtil::Equals(value, "NULLS_FIRST")) {
  return DefaultOrderByNullType::NULLS_FIRST;
 }
 if (StringUtil::Equals(value, "NULLS_LAST")) {
  return DefaultOrderByNullType::NULLS_LAST;
 }
 if (StringUtil::Equals(value, "NULLS_FIRST_ON_ASC_LAST_ON_DESC")) {
  return DefaultOrderByNullType::NULLS_FIRST_ON_ASC_LAST_ON_DESC;
 }
 if (StringUtil::Equals(value, "NULLS_LAST_ON_ASC_FIRST_ON_DESC")) {
  return DefaultOrderByNullType::NULLS_LAST_ON_ASC_FIRST_ON_DESC;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<DependencyEntryType>(DependencyEntryType value) {
 switch(value) {
 case DependencyEntryType::SUBJECT:
  return "SUBJECT";
 case DependencyEntryType::DEPENDENT:
  return "DEPENDENT";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
DependencyEntryType EnumUtil::FromString<DependencyEntryType>(const char *value) {
 if (StringUtil::Equals(value, "SUBJECT")) {
  return DependencyEntryType::SUBJECT;
 }
 if (StringUtil::Equals(value, "DEPENDENT")) {
  return DependencyEntryType::DEPENDENT;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<DistinctType>(DistinctType value) {
 switch(value) {
 case DistinctType::DISTINCT:
  return "DISTINCT";
 case DistinctType::DISTINCT_ON:
  return "DISTINCT_ON";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
DistinctType EnumUtil::FromString<DistinctType>(const char *value) {
 if (StringUtil::Equals(value, "DISTINCT")) {
  return DistinctType::DISTINCT;
 }
 if (StringUtil::Equals(value, "DISTINCT_ON")) {
  return DistinctType::DISTINCT_ON;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ErrorType>(ErrorType value) {
 switch(value) {
 case ErrorType::UNSIGNED_EXTENSION:
  return "UNSIGNED_EXTENSION";
 case ErrorType::INVALIDATED_TRANSACTION:
  return "INVALIDATED_TRANSACTION";
 case ErrorType::INVALIDATED_DATABASE:
  return "INVALIDATED_DATABASE";
 case ErrorType::ERROR_COUNT:
  return "ERROR_COUNT";
 case ErrorType::INVALID:
  return "INVALID";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ErrorType EnumUtil::FromString<ErrorType>(const char *value) {
 if (StringUtil::Equals(value, "UNSIGNED_EXTENSION")) {
  return ErrorType::UNSIGNED_EXTENSION;
 }
 if (StringUtil::Equals(value, "INVALIDATED_TRANSACTION")) {
  return ErrorType::INVALIDATED_TRANSACTION;
 }
 if (StringUtil::Equals(value, "INVALIDATED_DATABASE")) {
  return ErrorType::INVALIDATED_DATABASE;
 }
 if (StringUtil::Equals(value, "ERROR_COUNT")) {
  return ErrorType::ERROR_COUNT;
 }
 if (StringUtil::Equals(value, "INVALID")) {
  return ErrorType::INVALID;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ExceptionFormatValueType>(ExceptionFormatValueType value) {
 switch(value) {
 case ExceptionFormatValueType::FORMAT_VALUE_TYPE_DOUBLE:
  return "FORMAT_VALUE_TYPE_DOUBLE";
 case ExceptionFormatValueType::FORMAT_VALUE_TYPE_INTEGER:
  return "FORMAT_VALUE_TYPE_INTEGER";
 case ExceptionFormatValueType::FORMAT_VALUE_TYPE_STRING:
  return "FORMAT_VALUE_TYPE_STRING";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ExceptionFormatValueType EnumUtil::FromString<ExceptionFormatValueType>(const char *value) {
 if (StringUtil::Equals(value, "FORMAT_VALUE_TYPE_DOUBLE")) {
  return ExceptionFormatValueType::FORMAT_VALUE_TYPE_DOUBLE;
 }
 if (StringUtil::Equals(value, "FORMAT_VALUE_TYPE_INTEGER")) {
  return ExceptionFormatValueType::FORMAT_VALUE_TYPE_INTEGER;
 }
 if (StringUtil::Equals(value, "FORMAT_VALUE_TYPE_STRING")) {
  return ExceptionFormatValueType::FORMAT_VALUE_TYPE_STRING;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ExplainOutputType>(ExplainOutputType value) {
 switch(value) {
 case ExplainOutputType::ALL:
  return "ALL";
 case ExplainOutputType::OPTIMIZED_ONLY:
  return "OPTIMIZED_ONLY";
 case ExplainOutputType::PHYSICAL_ONLY:
  return "PHYSICAL_ONLY";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ExplainOutputType EnumUtil::FromString<ExplainOutputType>(const char *value) {
 if (StringUtil::Equals(value, "ALL")) {
  return ExplainOutputType::ALL;
 }
 if (StringUtil::Equals(value, "OPTIMIZED_ONLY")) {
  return ExplainOutputType::OPTIMIZED_ONLY;
 }
 if (StringUtil::Equals(value, "PHYSICAL_ONLY")) {
  return ExplainOutputType::PHYSICAL_ONLY;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ExplainType>(ExplainType value) {
 switch(value) {
 case ExplainType::EXPLAIN_STANDARD:
  return "EXPLAIN_STANDARD";
 case ExplainType::EXPLAIN_ANALYZE:
  return "EXPLAIN_ANALYZE";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ExplainType EnumUtil::FromString<ExplainType>(const char *value) {
 if (StringUtil::Equals(value, "EXPLAIN_STANDARD")) {
  return ExplainType::EXPLAIN_STANDARD;
 }
 if (StringUtil::Equals(value, "EXPLAIN_ANALYZE")) {
  return ExplainType::EXPLAIN_ANALYZE;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ExpressionClass>(ExpressionClass value) {
 switch(value) {
 case ExpressionClass::INVALID:
  return "INVALID";
 case ExpressionClass::AGGREGATE:
  return "AGGREGATE";
 case ExpressionClass::CASE:
  return "CASE";
 case ExpressionClass::CAST:
  return "CAST";
 case ExpressionClass::COLUMN_REF:
  return "COLUMN_REF";
 case ExpressionClass::COMPARISON:
  return "COMPARISON";
 case ExpressionClass::CONJUNCTION:
  return "CONJUNCTION";
 case ExpressionClass::CONSTANT:
  return "CONSTANT";
 case ExpressionClass::DEFAULT:
  return "DEFAULT";
 case ExpressionClass::FUNCTION:
  return "FUNCTION";
 case ExpressionClass::OPERATOR:
  return "OPERATOR";
 case ExpressionClass::STAR:
  return "STAR";
 case ExpressionClass::SUBQUERY:
  return "SUBQUERY";
 case ExpressionClass::WINDOW:
  return "WINDOW";
 case ExpressionClass::PARAMETER:
  return "PARAMETER";
 case ExpressionClass::COLLATE:
  return "COLLATE";
 case ExpressionClass::LAMBDA:
  return "LAMBDA";
 case ExpressionClass::POSITIONAL_REFERENCE:
  return "POSITIONAL_REFERENCE";
 case ExpressionClass::BETWEEN:
  return "BETWEEN";
 case ExpressionClass::LAMBDA_REF:
  return "LAMBDA_REF";
 case ExpressionClass::BOUND_AGGREGATE:
  return "BOUND_AGGREGATE";
 case ExpressionClass::BOUND_CASE:
  return "BOUND_CASE";
 case ExpressionClass::BOUND_CAST:
  return "BOUND_CAST";
 case ExpressionClass::BOUND_COLUMN_REF:
  return "BOUND_COLUMN_REF";
 case ExpressionClass::BOUND_COMPARISON:
  return "BOUND_COMPARISON";
 case ExpressionClass::BOUND_CONJUNCTION:
  return "BOUND_CONJUNCTION";
 case ExpressionClass::BOUND_CONSTANT:
  return "BOUND_CONSTANT";
 case ExpressionClass::BOUND_DEFAULT:
  return "BOUND_DEFAULT";
 case ExpressionClass::BOUND_FUNCTION:
  return "BOUND_FUNCTION";
 case ExpressionClass::BOUND_OPERATOR:
  return "BOUND_OPERATOR";
 case ExpressionClass::BOUND_PARAMETER:
  return "BOUND_PARAMETER";
 case ExpressionClass::BOUND_REF:
  return "BOUND_REF";
 case ExpressionClass::BOUND_SUBQUERY:
  return "BOUND_SUBQUERY";
 case ExpressionClass::BOUND_WINDOW:
  return "BOUND_WINDOW";
 case ExpressionClass::BOUND_BETWEEN:
  return "BOUND_BETWEEN";
 case ExpressionClass::BOUND_UNNEST:
  return "BOUND_UNNEST";
 case ExpressionClass::BOUND_LAMBDA:
  return "BOUND_LAMBDA";
 case ExpressionClass::BOUND_LAMBDA_REF:
  return "BOUND_LAMBDA_REF";
 case ExpressionClass::BOUND_EXPRESSION:
  return "BOUND_EXPRESSION";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ExpressionClass EnumUtil::FromString<ExpressionClass>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return ExpressionClass::INVALID;
 }
 if (StringUtil::Equals(value, "AGGREGATE")) {
  return ExpressionClass::AGGREGATE;
 }
 if (StringUtil::Equals(value, "CASE")) {
  return ExpressionClass::CASE;
 }
 if (StringUtil::Equals(value, "CAST")) {
  return ExpressionClass::CAST;
 }
 if (StringUtil::Equals(value, "COLUMN_REF")) {
  return ExpressionClass::COLUMN_REF;
 }
 if (StringUtil::Equals(value, "COMPARISON")) {
  return ExpressionClass::COMPARISON;
 }
 if (StringUtil::Equals(value, "CONJUNCTION")) {
  return ExpressionClass::CONJUNCTION;
 }
 if (StringUtil::Equals(value, "CONSTANT")) {
  return ExpressionClass::CONSTANT;
 }
 if (StringUtil::Equals(value, "DEFAULT")) {
  return ExpressionClass::DEFAULT;
 }
 if (StringUtil::Equals(value, "FUNCTION")) {
  return ExpressionClass::FUNCTION;
 }
 if (StringUtil::Equals(value, "OPERATOR")) {
  return ExpressionClass::OPERATOR;
 }
 if (StringUtil::Equals(value, "STAR")) {
  return ExpressionClass::STAR;
 }
 if (StringUtil::Equals(value, "SUBQUERY")) {
  return ExpressionClass::SUBQUERY;
 }
 if (StringUtil::Equals(value, "WINDOW")) {
  return ExpressionClass::WINDOW;
 }
 if (StringUtil::Equals(value, "PARAMETER")) {
  return ExpressionClass::PARAMETER;
 }
 if (StringUtil::Equals(value, "COLLATE")) {
  return ExpressionClass::COLLATE;
 }
 if (StringUtil::Equals(value, "LAMBDA")) {
  return ExpressionClass::LAMBDA;
 }
 if (StringUtil::Equals(value, "POSITIONAL_REFERENCE")) {
  return ExpressionClass::POSITIONAL_REFERENCE;
 }
 if (StringUtil::Equals(value, "BETWEEN")) {
  return ExpressionClass::BETWEEN;
 }
 if (StringUtil::Equals(value, "LAMBDA_REF")) {
  return ExpressionClass::LAMBDA_REF;
 }
 if (StringUtil::Equals(value, "BOUND_AGGREGATE")) {
  return ExpressionClass::BOUND_AGGREGATE;
 }
 if (StringUtil::Equals(value, "BOUND_CASE")) {
  return ExpressionClass::BOUND_CASE;
 }
 if (StringUtil::Equals(value, "BOUND_CAST")) {
  return ExpressionClass::BOUND_CAST;
 }
 if (StringUtil::Equals(value, "BOUND_COLUMN_REF")) {
  return ExpressionClass::BOUND_COLUMN_REF;
 }
 if (StringUtil::Equals(value, "BOUND_COMPARISON")) {
  return ExpressionClass::BOUND_COMPARISON;
 }
 if (StringUtil::Equals(value, "BOUND_CONJUNCTION")) {
  return ExpressionClass::BOUND_CONJUNCTION;
 }
 if (StringUtil::Equals(value, "BOUND_CONSTANT")) {
  return ExpressionClass::BOUND_CONSTANT;
 }
 if (StringUtil::Equals(value, "BOUND_DEFAULT")) {
  return ExpressionClass::BOUND_DEFAULT;
 }
 if (StringUtil::Equals(value, "BOUND_FUNCTION")) {
  return ExpressionClass::BOUND_FUNCTION;
 }
 if (StringUtil::Equals(value, "BOUND_OPERATOR")) {
  return ExpressionClass::BOUND_OPERATOR;
 }
 if (StringUtil::Equals(value, "BOUND_PARAMETER")) {
  return ExpressionClass::BOUND_PARAMETER;
 }
 if (StringUtil::Equals(value, "BOUND_REF")) {
  return ExpressionClass::BOUND_REF;
 }
 if (StringUtil::Equals(value, "BOUND_SUBQUERY")) {
  return ExpressionClass::BOUND_SUBQUERY;
 }
 if (StringUtil::Equals(value, "BOUND_WINDOW")) {
  return ExpressionClass::BOUND_WINDOW;
 }
 if (StringUtil::Equals(value, "BOUND_BETWEEN")) {
  return ExpressionClass::BOUND_BETWEEN;
 }
 if (StringUtil::Equals(value, "BOUND_UNNEST")) {
  return ExpressionClass::BOUND_UNNEST;
 }
 if (StringUtil::Equals(value, "BOUND_LAMBDA")) {
  return ExpressionClass::BOUND_LAMBDA;
 }
 if (StringUtil::Equals(value, "BOUND_LAMBDA_REF")) {
  return ExpressionClass::BOUND_LAMBDA_REF;
 }
 if (StringUtil::Equals(value, "BOUND_EXPRESSION")) {
  return ExpressionClass::BOUND_EXPRESSION;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ExpressionType>(ExpressionType value) {
 switch(value) {
 case ExpressionType::INVALID:
  return "INVALID";
 case ExpressionType::OPERATOR_CAST:
  return "OPERATOR_CAST";
 case ExpressionType::OPERATOR_NOT:
  return "OPERATOR_NOT";
 case ExpressionType::OPERATOR_IS_NULL:
  return "OPERATOR_IS_NULL";
 case ExpressionType::OPERATOR_IS_NOT_NULL:
  return "OPERATOR_IS_NOT_NULL";
 case ExpressionType::COMPARE_EQUAL:
  return "COMPARE_EQUAL";
 case ExpressionType::COMPARE_NOTEQUAL:
  return "COMPARE_NOTEQUAL";
 case ExpressionType::COMPARE_LESSTHAN:
  return "COMPARE_LESSTHAN";
 case ExpressionType::COMPARE_GREATERTHAN:
  return "COMPARE_GREATERTHAN";
 case ExpressionType::COMPARE_LESSTHANOREQUALTO:
  return "COMPARE_LESSTHANOREQUALTO";
 case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
  return "COMPARE_GREATERTHANOREQUALTO";
 case ExpressionType::COMPARE_IN:
  return "COMPARE_IN";
 case ExpressionType::COMPARE_NOT_IN:
  return "COMPARE_NOT_IN";
 case ExpressionType::COMPARE_DISTINCT_FROM:
  return "COMPARE_DISTINCT_FROM";
 case ExpressionType::COMPARE_BETWEEN:
  return "COMPARE_BETWEEN";
 case ExpressionType::COMPARE_NOT_BETWEEN:
  return "COMPARE_NOT_BETWEEN";
 case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
  return "COMPARE_NOT_DISTINCT_FROM";
 case ExpressionType::CONJUNCTION_AND:
  return "CONJUNCTION_AND";
 case ExpressionType::CONJUNCTION_OR:
  return "CONJUNCTION_OR";
 case ExpressionType::VALUE_CONSTANT:
  return "VALUE_CONSTANT";
 case ExpressionType::VALUE_PARAMETER:
  return "VALUE_PARAMETER";
 case ExpressionType::VALUE_TUPLE:
  return "VALUE_TUPLE";
 case ExpressionType::VALUE_TUPLE_ADDRESS:
  return "VALUE_TUPLE_ADDRESS";
 case ExpressionType::VALUE_NULL:
  return "VALUE_NULL";
 case ExpressionType::VALUE_VECTOR:
  return "VALUE_VECTOR";
 case ExpressionType::VALUE_SCALAR:
  return "VALUE_SCALAR";
 case ExpressionType::VALUE_DEFAULT:
  return "VALUE_DEFAULT";
 case ExpressionType::AGGREGATE:
  return "AGGREGATE";
 case ExpressionType::BOUND_AGGREGATE:
  return "BOUND_AGGREGATE";
 case ExpressionType::GROUPING_FUNCTION:
  return "GROUPING_FUNCTION";
 case ExpressionType::WINDOW_AGGREGATE:
  return "WINDOW_AGGREGATE";
 case ExpressionType::WINDOW_RANK:
  return "WINDOW_RANK";
 case ExpressionType::WINDOW_RANK_DENSE:
  return "WINDOW_RANK_DENSE";
 case ExpressionType::WINDOW_NTILE:
  return "WINDOW_NTILE";
 case ExpressionType::WINDOW_PERCENT_RANK:
  return "WINDOW_PERCENT_RANK";
 case ExpressionType::WINDOW_CUME_DIST:
  return "WINDOW_CUME_DIST";
 case ExpressionType::WINDOW_ROW_NUMBER:
  return "WINDOW_ROW_NUMBER";
 case ExpressionType::WINDOW_FIRST_VALUE:
  return "WINDOW_FIRST_VALUE";
 case ExpressionType::WINDOW_LAST_VALUE:
  return "WINDOW_LAST_VALUE";
 case ExpressionType::WINDOW_LEAD:
  return "WINDOW_LEAD";
 case ExpressionType::WINDOW_LAG:
  return "WINDOW_LAG";
 case ExpressionType::WINDOW_NTH_VALUE:
  return "WINDOW_NTH_VALUE";
 case ExpressionType::FUNCTION:
  return "FUNCTION";
 case ExpressionType::BOUND_FUNCTION:
  return "BOUND_FUNCTION";
 case ExpressionType::CASE_EXPR:
  return "CASE_EXPR";
 case ExpressionType::OPERATOR_NULLIF:
  return "OPERATOR_NULLIF";
 case ExpressionType::OPERATOR_COALESCE:
  return "OPERATOR_COALESCE";
 case ExpressionType::ARRAY_EXTRACT:
  return "ARRAY_EXTRACT";
 case ExpressionType::ARRAY_SLICE:
  return "ARRAY_SLICE";
 case ExpressionType::STRUCT_EXTRACT:
  return "STRUCT_EXTRACT";
 case ExpressionType::ARRAY_CONSTRUCTOR:
  return "ARRAY_CONSTRUCTOR";
 case ExpressionType::ARROW:
  return "ARROW";
 case ExpressionType::SUBQUERY:
  return "SUBQUERY";
 case ExpressionType::STAR:
  return "STAR";
 case ExpressionType::TABLE_STAR:
  return "TABLE_STAR";
 case ExpressionType::PLACEHOLDER:
  return "PLACEHOLDER";
 case ExpressionType::COLUMN_REF:
  return "COLUMN_REF";
 case ExpressionType::FUNCTION_REF:
  return "FUNCTION_REF";
 case ExpressionType::TABLE_REF:
  return "TABLE_REF";
 case ExpressionType::LAMBDA_REF:
  return "LAMBDA_REF";
 case ExpressionType::CAST:
  return "CAST";
 case ExpressionType::BOUND_REF:
  return "BOUND_REF";
 case ExpressionType::BOUND_COLUMN_REF:
  return "BOUND_COLUMN_REF";
 case ExpressionType::BOUND_UNNEST:
  return "BOUND_UNNEST";
 case ExpressionType::COLLATE:
  return "COLLATE";
 case ExpressionType::LAMBDA:
  return "LAMBDA";
 case ExpressionType::POSITIONAL_REFERENCE:
  return "POSITIONAL_REFERENCE";
 case ExpressionType::BOUND_LAMBDA_REF:
  return "BOUND_LAMBDA_REF";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ExpressionType EnumUtil::FromString<ExpressionType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return ExpressionType::INVALID;
 }
 if (StringUtil::Equals(value, "OPERATOR_CAST")) {
  return ExpressionType::OPERATOR_CAST;
 }
 if (StringUtil::Equals(value, "OPERATOR_NOT")) {
  return ExpressionType::OPERATOR_NOT;
 }
 if (StringUtil::Equals(value, "OPERATOR_IS_NULL")) {
  return ExpressionType::OPERATOR_IS_NULL;
 }
 if (StringUtil::Equals(value, "OPERATOR_IS_NOT_NULL")) {
  return ExpressionType::OPERATOR_IS_NOT_NULL;
 }
 if (StringUtil::Equals(value, "COMPARE_EQUAL")) {
  return ExpressionType::COMPARE_EQUAL;
 }
 if (StringUtil::Equals(value, "COMPARE_NOTEQUAL")) {
  return ExpressionType::COMPARE_NOTEQUAL;
 }
 if (StringUtil::Equals(value, "COMPARE_LESSTHAN")) {
  return ExpressionType::COMPARE_LESSTHAN;
 }
 if (StringUtil::Equals(value, "COMPARE_GREATERTHAN")) {
  return ExpressionType::COMPARE_GREATERTHAN;
 }
 if (StringUtil::Equals(value, "COMPARE_LESSTHANOREQUALTO")) {
  return ExpressionType::COMPARE_LESSTHANOREQUALTO;
 }
 if (StringUtil::Equals(value, "COMPARE_GREATERTHANOREQUALTO")) {
  return ExpressionType::COMPARE_GREATERTHANOREQUALTO;
 }
 if (StringUtil::Equals(value, "COMPARE_IN")) {
  return ExpressionType::COMPARE_IN;
 }
 if (StringUtil::Equals(value, "COMPARE_NOT_IN")) {
  return ExpressionType::COMPARE_NOT_IN;
 }
 if (StringUtil::Equals(value, "COMPARE_DISTINCT_FROM")) {
  return ExpressionType::COMPARE_DISTINCT_FROM;
 }
 if (StringUtil::Equals(value, "COMPARE_BETWEEN")) {
  return ExpressionType::COMPARE_BETWEEN;
 }
 if (StringUtil::Equals(value, "COMPARE_NOT_BETWEEN")) {
  return ExpressionType::COMPARE_NOT_BETWEEN;
 }
 if (StringUtil::Equals(value, "COMPARE_NOT_DISTINCT_FROM")) {
  return ExpressionType::COMPARE_NOT_DISTINCT_FROM;
 }
 if (StringUtil::Equals(value, "CONJUNCTION_AND")) {
  return ExpressionType::CONJUNCTION_AND;
 }
 if (StringUtil::Equals(value, "CONJUNCTION_OR")) {
  return ExpressionType::CONJUNCTION_OR;
 }
 if (StringUtil::Equals(value, "VALUE_CONSTANT")) {
  return ExpressionType::VALUE_CONSTANT;
 }
 if (StringUtil::Equals(value, "VALUE_PARAMETER")) {
  return ExpressionType::VALUE_PARAMETER;
 }
 if (StringUtil::Equals(value, "VALUE_TUPLE")) {
  return ExpressionType::VALUE_TUPLE;
 }
 if (StringUtil::Equals(value, "VALUE_TUPLE_ADDRESS")) {
  return ExpressionType::VALUE_TUPLE_ADDRESS;
 }
 if (StringUtil::Equals(value, "VALUE_NULL")) {
  return ExpressionType::VALUE_NULL;
 }
 if (StringUtil::Equals(value, "VALUE_VECTOR")) {
  return ExpressionType::VALUE_VECTOR;
 }
 if (StringUtil::Equals(value, "VALUE_SCALAR")) {
  return ExpressionType::VALUE_SCALAR;
 }
 if (StringUtil::Equals(value, "VALUE_DEFAULT")) {
  return ExpressionType::VALUE_DEFAULT;
 }
 if (StringUtil::Equals(value, "AGGREGATE")) {
  return ExpressionType::AGGREGATE;
 }
 if (StringUtil::Equals(value, "BOUND_AGGREGATE")) {
  return ExpressionType::BOUND_AGGREGATE;
 }
 if (StringUtil::Equals(value, "GROUPING_FUNCTION")) {
  return ExpressionType::GROUPING_FUNCTION;
 }
 if (StringUtil::Equals(value, "WINDOW_AGGREGATE")) {
  return ExpressionType::WINDOW_AGGREGATE;
 }
 if (StringUtil::Equals(value, "WINDOW_RANK")) {
  return ExpressionType::WINDOW_RANK;
 }
 if (StringUtil::Equals(value, "WINDOW_RANK_DENSE")) {
  return ExpressionType::WINDOW_RANK_DENSE;
 }
 if (StringUtil::Equals(value, "WINDOW_NTILE")) {
  return ExpressionType::WINDOW_NTILE;
 }
 if (StringUtil::Equals(value, "WINDOW_PERCENT_RANK")) {
  return ExpressionType::WINDOW_PERCENT_RANK;
 }
 if (StringUtil::Equals(value, "WINDOW_CUME_DIST")) {
  return ExpressionType::WINDOW_CUME_DIST;
 }
 if (StringUtil::Equals(value, "WINDOW_ROW_NUMBER")) {
  return ExpressionType::WINDOW_ROW_NUMBER;
 }
 if (StringUtil::Equals(value, "WINDOW_FIRST_VALUE")) {
  return ExpressionType::WINDOW_FIRST_VALUE;
 }
 if (StringUtil::Equals(value, "WINDOW_LAST_VALUE")) {
  return ExpressionType::WINDOW_LAST_VALUE;
 }
 if (StringUtil::Equals(value, "WINDOW_LEAD")) {
  return ExpressionType::WINDOW_LEAD;
 }
 if (StringUtil::Equals(value, "WINDOW_LAG")) {
  return ExpressionType::WINDOW_LAG;
 }
 if (StringUtil::Equals(value, "WINDOW_NTH_VALUE")) {
  return ExpressionType::WINDOW_NTH_VALUE;
 }
 if (StringUtil::Equals(value, "FUNCTION")) {
  return ExpressionType::FUNCTION;
 }
 if (StringUtil::Equals(value, "BOUND_FUNCTION")) {
  return ExpressionType::BOUND_FUNCTION;
 }
 if (StringUtil::Equals(value, "CASE_EXPR")) {
  return ExpressionType::CASE_EXPR;
 }
 if (StringUtil::Equals(value, "OPERATOR_NULLIF")) {
  return ExpressionType::OPERATOR_NULLIF;
 }
 if (StringUtil::Equals(value, "OPERATOR_COALESCE")) {
  return ExpressionType::OPERATOR_COALESCE;
 }
 if (StringUtil::Equals(value, "ARRAY_EXTRACT")) {
  return ExpressionType::ARRAY_EXTRACT;
 }
 if (StringUtil::Equals(value, "ARRAY_SLICE")) {
  return ExpressionType::ARRAY_SLICE;
 }
 if (StringUtil::Equals(value, "STRUCT_EXTRACT")) {
  return ExpressionType::STRUCT_EXTRACT;
 }
 if (StringUtil::Equals(value, "ARRAY_CONSTRUCTOR")) {
  return ExpressionType::ARRAY_CONSTRUCTOR;
 }
 if (StringUtil::Equals(value, "ARROW")) {
  return ExpressionType::ARROW;
 }
 if (StringUtil::Equals(value, "SUBQUERY")) {
  return ExpressionType::SUBQUERY;
 }
 if (StringUtil::Equals(value, "STAR")) {
  return ExpressionType::STAR;
 }
 if (StringUtil::Equals(value, "TABLE_STAR")) {
  return ExpressionType::TABLE_STAR;
 }
 if (StringUtil::Equals(value, "PLACEHOLDER")) {
  return ExpressionType::PLACEHOLDER;
 }
 if (StringUtil::Equals(value, "COLUMN_REF")) {
  return ExpressionType::COLUMN_REF;
 }
 if (StringUtil::Equals(value, "FUNCTION_REF")) {
  return ExpressionType::FUNCTION_REF;
 }
 if (StringUtil::Equals(value, "TABLE_REF")) {
  return ExpressionType::TABLE_REF;
 }
 if (StringUtil::Equals(value, "LAMBDA_REF")) {
  return ExpressionType::LAMBDA_REF;
 }
 if (StringUtil::Equals(value, "CAST")) {
  return ExpressionType::CAST;
 }
 if (StringUtil::Equals(value, "BOUND_REF")) {
  return ExpressionType::BOUND_REF;
 }
 if (StringUtil::Equals(value, "BOUND_COLUMN_REF")) {
  return ExpressionType::BOUND_COLUMN_REF;
 }
 if (StringUtil::Equals(value, "BOUND_UNNEST")) {
  return ExpressionType::BOUND_UNNEST;
 }
 if (StringUtil::Equals(value, "COLLATE")) {
  return ExpressionType::COLLATE;
 }
 if (StringUtil::Equals(value, "LAMBDA")) {
  return ExpressionType::LAMBDA;
 }
 if (StringUtil::Equals(value, "POSITIONAL_REFERENCE")) {
  return ExpressionType::POSITIONAL_REFERENCE;
 }
 if (StringUtil::Equals(value, "BOUND_LAMBDA_REF")) {
  return ExpressionType::BOUND_LAMBDA_REF;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ExtensionLoadResult>(ExtensionLoadResult value) {
 switch(value) {
 case ExtensionLoadResult::LOADED_EXTENSION:
  return "LOADED_EXTENSION";
 case ExtensionLoadResult::EXTENSION_UNKNOWN:
  return "EXTENSION_UNKNOWN";
 case ExtensionLoadResult::NOT_LOADED:
  return "NOT_LOADED";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ExtensionLoadResult EnumUtil::FromString<ExtensionLoadResult>(const char *value) {
 if (StringUtil::Equals(value, "LOADED_EXTENSION")) {
  return ExtensionLoadResult::LOADED_EXTENSION;
 }
 if (StringUtil::Equals(value, "EXTENSION_UNKNOWN")) {
  return ExtensionLoadResult::EXTENSION_UNKNOWN;
 }
 if (StringUtil::Equals(value, "NOT_LOADED")) {
  return ExtensionLoadResult::NOT_LOADED;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ExtraTypeInfoType>(ExtraTypeInfoType value) {
 switch(value) {
 case ExtraTypeInfoType::INVALID_TYPE_INFO:
  return "INVALID_TYPE_INFO";
 case ExtraTypeInfoType::GENERIC_TYPE_INFO:
  return "GENERIC_TYPE_INFO";
 case ExtraTypeInfoType::DECIMAL_TYPE_INFO:
  return "DECIMAL_TYPE_INFO";
 case ExtraTypeInfoType::STRING_TYPE_INFO:
  return "STRING_TYPE_INFO";
 case ExtraTypeInfoType::LIST_TYPE_INFO:
  return "LIST_TYPE_INFO";
 case ExtraTypeInfoType::STRUCT_TYPE_INFO:
  return "STRUCT_TYPE_INFO";
 case ExtraTypeInfoType::ENUM_TYPE_INFO:
  return "ENUM_TYPE_INFO";
 case ExtraTypeInfoType::USER_TYPE_INFO:
  return "USER_TYPE_INFO";
 case ExtraTypeInfoType::AGGREGATE_STATE_TYPE_INFO:
  return "AGGREGATE_STATE_TYPE_INFO";
 case ExtraTypeInfoType::ARRAY_TYPE_INFO:
  return "ARRAY_TYPE_INFO";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ExtraTypeInfoType EnumUtil::FromString<ExtraTypeInfoType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID_TYPE_INFO")) {
  return ExtraTypeInfoType::INVALID_TYPE_INFO;
 }
 if (StringUtil::Equals(value, "GENERIC_TYPE_INFO")) {
  return ExtraTypeInfoType::GENERIC_TYPE_INFO;
 }
 if (StringUtil::Equals(value, "DECIMAL_TYPE_INFO")) {
  return ExtraTypeInfoType::DECIMAL_TYPE_INFO;
 }
 if (StringUtil::Equals(value, "STRING_TYPE_INFO")) {
  return ExtraTypeInfoType::STRING_TYPE_INFO;
 }
 if (StringUtil::Equals(value, "LIST_TYPE_INFO")) {
  return ExtraTypeInfoType::LIST_TYPE_INFO;
 }
 if (StringUtil::Equals(value, "STRUCT_TYPE_INFO")) {
  return ExtraTypeInfoType::STRUCT_TYPE_INFO;
 }
 if (StringUtil::Equals(value, "ENUM_TYPE_INFO")) {
  return ExtraTypeInfoType::ENUM_TYPE_INFO;
 }
 if (StringUtil::Equals(value, "USER_TYPE_INFO")) {
  return ExtraTypeInfoType::USER_TYPE_INFO;
 }
 if (StringUtil::Equals(value, "AGGREGATE_STATE_TYPE_INFO")) {
  return ExtraTypeInfoType::AGGREGATE_STATE_TYPE_INFO;
 }
 if (StringUtil::Equals(value, "ARRAY_TYPE_INFO")) {
  return ExtraTypeInfoType::ARRAY_TYPE_INFO;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<FileBufferType>(FileBufferType value) {
 switch(value) {
 case FileBufferType::BLOCK:
  return "BLOCK";
 case FileBufferType::MANAGED_BUFFER:
  return "MANAGED_BUFFER";
 case FileBufferType::TINY_BUFFER:
  return "TINY_BUFFER";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
FileBufferType EnumUtil::FromString<FileBufferType>(const char *value) {
 if (StringUtil::Equals(value, "BLOCK")) {
  return FileBufferType::BLOCK;
 }
 if (StringUtil::Equals(value, "MANAGED_BUFFER")) {
  return FileBufferType::MANAGED_BUFFER;
 }
 if (StringUtil::Equals(value, "TINY_BUFFER")) {
  return FileBufferType::TINY_BUFFER;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<FileCompressionType>(FileCompressionType value) {
 switch(value) {
 case FileCompressionType::AUTO_DETECT:
  return "AUTO_DETECT";
 case FileCompressionType::UNCOMPRESSED:
  return "UNCOMPRESSED";
 case FileCompressionType::GZIP:
  return "GZIP";
 case FileCompressionType::ZSTD:
  return "ZSTD";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
FileCompressionType EnumUtil::FromString<FileCompressionType>(const char *value) {
 if (StringUtil::Equals(value, "AUTO_DETECT")) {
  return FileCompressionType::AUTO_DETECT;
 }
 if (StringUtil::Equals(value, "UNCOMPRESSED")) {
  return FileCompressionType::UNCOMPRESSED;
 }
 if (StringUtil::Equals(value, "GZIP")) {
  return FileCompressionType::GZIP;
 }
 if (StringUtil::Equals(value, "ZSTD")) {
  return FileCompressionType::ZSTD;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<FileGlobOptions>(FileGlobOptions value) {
 switch(value) {
 case FileGlobOptions::DISALLOW_EMPTY:
  return "DISALLOW_EMPTY";
 case FileGlobOptions::ALLOW_EMPTY:
  return "ALLOW_EMPTY";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
FileGlobOptions EnumUtil::FromString<FileGlobOptions>(const char *value) {
 if (StringUtil::Equals(value, "DISALLOW_EMPTY")) {
  return FileGlobOptions::DISALLOW_EMPTY;
 }
 if (StringUtil::Equals(value, "ALLOW_EMPTY")) {
  return FileGlobOptions::ALLOW_EMPTY;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<FileLockType>(FileLockType value) {
 switch(value) {
 case FileLockType::NO_LOCK:
  return "NO_LOCK";
 case FileLockType::READ_LOCK:
  return "READ_LOCK";
 case FileLockType::WRITE_LOCK:
  return "WRITE_LOCK";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
FileLockType EnumUtil::FromString<FileLockType>(const char *value) {
 if (StringUtil::Equals(value, "NO_LOCK")) {
  return FileLockType::NO_LOCK;
 }
 if (StringUtil::Equals(value, "READ_LOCK")) {
  return FileLockType::READ_LOCK;
 }
 if (StringUtil::Equals(value, "WRITE_LOCK")) {
  return FileLockType::WRITE_LOCK;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<FilterPropagateResult>(FilterPropagateResult value) {
 switch(value) {
 case FilterPropagateResult::NO_PRUNING_POSSIBLE:
  return "NO_PRUNING_POSSIBLE";
 case FilterPropagateResult::FILTER_ALWAYS_TRUE:
  return "FILTER_ALWAYS_TRUE";
 case FilterPropagateResult::FILTER_ALWAYS_FALSE:
  return "FILTER_ALWAYS_FALSE";
 case FilterPropagateResult::FILTER_TRUE_OR_NULL:
  return "FILTER_TRUE_OR_NULL";
 case FilterPropagateResult::FILTER_FALSE_OR_NULL:
  return "FILTER_FALSE_OR_NULL";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
FilterPropagateResult EnumUtil::FromString<FilterPropagateResult>(const char *value) {
 if (StringUtil::Equals(value, "NO_PRUNING_POSSIBLE")) {
  return FilterPropagateResult::NO_PRUNING_POSSIBLE;
 }
 if (StringUtil::Equals(value, "FILTER_ALWAYS_TRUE")) {
  return FilterPropagateResult::FILTER_ALWAYS_TRUE;
 }
 if (StringUtil::Equals(value, "FILTER_ALWAYS_FALSE")) {
  return FilterPropagateResult::FILTER_ALWAYS_FALSE;
 }
 if (StringUtil::Equals(value, "FILTER_TRUE_OR_NULL")) {
  return FilterPropagateResult::FILTER_TRUE_OR_NULL;
 }
 if (StringUtil::Equals(value, "FILTER_FALSE_OR_NULL")) {
  return FilterPropagateResult::FILTER_FALSE_OR_NULL;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ForeignKeyType>(ForeignKeyType value) {
 switch(value) {
 case ForeignKeyType::FK_TYPE_PRIMARY_KEY_TABLE:
  return "FK_TYPE_PRIMARY_KEY_TABLE";
 case ForeignKeyType::FK_TYPE_FOREIGN_KEY_TABLE:
  return "FK_TYPE_FOREIGN_KEY_TABLE";
 case ForeignKeyType::FK_TYPE_SELF_REFERENCE_TABLE:
  return "FK_TYPE_SELF_REFERENCE_TABLE";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ForeignKeyType EnumUtil::FromString<ForeignKeyType>(const char *value) {
 if (StringUtil::Equals(value, "FK_TYPE_PRIMARY_KEY_TABLE")) {
  return ForeignKeyType::FK_TYPE_PRIMARY_KEY_TABLE;
 }
 if (StringUtil::Equals(value, "FK_TYPE_FOREIGN_KEY_TABLE")) {
  return ForeignKeyType::FK_TYPE_FOREIGN_KEY_TABLE;
 }
 if (StringUtil::Equals(value, "FK_TYPE_SELF_REFERENCE_TABLE")) {
  return ForeignKeyType::FK_TYPE_SELF_REFERENCE_TABLE;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<FunctionNullHandling>(FunctionNullHandling value) {
 switch(value) {
 case FunctionNullHandling::DEFAULT_NULL_HANDLING:
  return "DEFAULT_NULL_HANDLING";
 case FunctionNullHandling::SPECIAL_HANDLING:
  return "SPECIAL_HANDLING";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
FunctionNullHandling EnumUtil::FromString<FunctionNullHandling>(const char *value) {
 if (StringUtil::Equals(value, "DEFAULT_NULL_HANDLING")) {
  return FunctionNullHandling::DEFAULT_NULL_HANDLING;
 }
 if (StringUtil::Equals(value, "SPECIAL_HANDLING")) {
  return FunctionNullHandling::SPECIAL_HANDLING;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<FunctionSideEffects>(FunctionSideEffects value) {
 switch(value) {
 case FunctionSideEffects::NO_SIDE_EFFECTS:
  return "NO_SIDE_EFFECTS";
 case FunctionSideEffects::HAS_SIDE_EFFECTS:
  return "HAS_SIDE_EFFECTS";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
FunctionSideEffects EnumUtil::FromString<FunctionSideEffects>(const char *value) {
 if (StringUtil::Equals(value, "NO_SIDE_EFFECTS")) {
  return FunctionSideEffects::NO_SIDE_EFFECTS;
 }
 if (StringUtil::Equals(value, "HAS_SIDE_EFFECTS")) {
  return FunctionSideEffects::HAS_SIDE_EFFECTS;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<HLLStorageType>(HLLStorageType value) {
 switch(value) {
 case HLLStorageType::UNCOMPRESSED:
  return "UNCOMPRESSED";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
HLLStorageType EnumUtil::FromString<HLLStorageType>(const char *value) {
 if (StringUtil::Equals(value, "UNCOMPRESSED")) {
  return HLLStorageType::UNCOMPRESSED;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<IndexConstraintType>(IndexConstraintType value) {
 switch(value) {
 case IndexConstraintType::NONE:
  return "NONE";
 case IndexConstraintType::UNIQUE:
  return "UNIQUE";
 case IndexConstraintType::PRIMARY:
  return "PRIMARY";
 case IndexConstraintType::FOREIGN:
  return "FOREIGN";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
IndexConstraintType EnumUtil::FromString<IndexConstraintType>(const char *value) {
 if (StringUtil::Equals(value, "NONE")) {
  return IndexConstraintType::NONE;
 }
 if (StringUtil::Equals(value, "UNIQUE")) {
  return IndexConstraintType::UNIQUE;
 }
 if (StringUtil::Equals(value, "PRIMARY")) {
  return IndexConstraintType::PRIMARY;
 }
 if (StringUtil::Equals(value, "FOREIGN")) {
  return IndexConstraintType::FOREIGN;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<IndexType>(IndexType value) {
 switch(value) {
 case IndexType::INVALID:
  return "INVALID";
 case IndexType::ART:
  return "ART";
 case IndexType::EXTENSION:
  return "EXTENSION";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
IndexType EnumUtil::FromString<IndexType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return IndexType::INVALID;
 }
 if (StringUtil::Equals(value, "ART")) {
  return IndexType::ART;
 }
 if (StringUtil::Equals(value, "EXTENSION")) {
  return IndexType::EXTENSION;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<InsertColumnOrder>(InsertColumnOrder value) {
 switch(value) {
 case InsertColumnOrder::INSERT_BY_POSITION:
  return "INSERT_BY_POSITION";
 case InsertColumnOrder::INSERT_BY_NAME:
  return "INSERT_BY_NAME";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
InsertColumnOrder EnumUtil::FromString<InsertColumnOrder>(const char *value) {
 if (StringUtil::Equals(value, "INSERT_BY_POSITION")) {
  return InsertColumnOrder::INSERT_BY_POSITION;
 }
 if (StringUtil::Equals(value, "INSERT_BY_NAME")) {
  return InsertColumnOrder::INSERT_BY_NAME;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<InterruptMode>(InterruptMode value) {
 switch(value) {
 case InterruptMode::NO_INTERRUPTS:
  return "NO_INTERRUPTS";
 case InterruptMode::TASK:
  return "TASK";
 case InterruptMode::BLOCKING:
  return "BLOCKING";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
InterruptMode EnumUtil::FromString<InterruptMode>(const char *value) {
 if (StringUtil::Equals(value, "NO_INTERRUPTS")) {
  return InterruptMode::NO_INTERRUPTS;
 }
 if (StringUtil::Equals(value, "TASK")) {
  return InterruptMode::TASK;
 }
 if (StringUtil::Equals(value, "BLOCKING")) {
  return InterruptMode::BLOCKING;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<JoinRefType>(JoinRefType value) {
 switch(value) {
 case JoinRefType::REGULAR:
  return "REGULAR";
 case JoinRefType::NATURAL:
  return "NATURAL";
 case JoinRefType::CROSS:
  return "CROSS";
 case JoinRefType::POSITIONAL:
  return "POSITIONAL";
 case JoinRefType::ASOF:
  return "ASOF";
 case JoinRefType::DEPENDENT:
  return "DEPENDENT";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
JoinRefType EnumUtil::FromString<JoinRefType>(const char *value) {
 if (StringUtil::Equals(value, "REGULAR")) {
  return JoinRefType::REGULAR;
 }
 if (StringUtil::Equals(value, "NATURAL")) {
  return JoinRefType::NATURAL;
 }
 if (StringUtil::Equals(value, "CROSS")) {
  return JoinRefType::CROSS;
 }
 if (StringUtil::Equals(value, "POSITIONAL")) {
  return JoinRefType::POSITIONAL;
 }
 if (StringUtil::Equals(value, "ASOF")) {
  return JoinRefType::ASOF;
 }
 if (StringUtil::Equals(value, "DEPENDENT")) {
  return JoinRefType::DEPENDENT;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<JoinType>(JoinType value) {
 switch(value) {
 case JoinType::INVALID:
  return "INVALID";
 case JoinType::LEFT:
  return "LEFT";
 case JoinType::RIGHT:
  return "RIGHT";
 case JoinType::INNER:
  return "INNER";
 case JoinType::OUTER:
  return "FULL";
 case JoinType::SEMI:
  return "SEMI";
 case JoinType::ANTI:
  return "ANTI";
 case JoinType::MARK:
  return "MARK";
 case JoinType::SINGLE:
  return "SINGLE";
 case JoinType::RIGHT_SEMI:
  return "RIGHT_SEMI";
 case JoinType::RIGHT_ANTI:
  return "RIGHT_ANTI";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
JoinType EnumUtil::FromString<JoinType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return JoinType::INVALID;
 }
 if (StringUtil::Equals(value, "LEFT")) {
  return JoinType::LEFT;
 }
 if (StringUtil::Equals(value, "RIGHT")) {
  return JoinType::RIGHT;
 }
 if (StringUtil::Equals(value, "INNER")) {
  return JoinType::INNER;
 }
 if (StringUtil::Equals(value, "FULL")) {
  return JoinType::OUTER;
 }
 if (StringUtil::Equals(value, "SEMI")) {
  return JoinType::SEMI;
 }
 if (StringUtil::Equals(value, "ANTI")) {
  return JoinType::ANTI;
 }
 if (StringUtil::Equals(value, "MARK")) {
  return JoinType::MARK;
 }
 if (StringUtil::Equals(value, "SINGLE")) {
  return JoinType::SINGLE;
 }
 if (StringUtil::Equals(value, "RIGHT_SEMI")) {
  return JoinType::RIGHT_SEMI;
 }
 if (StringUtil::Equals(value, "RIGHT_ANTI")) {
  return JoinType::RIGHT_ANTI;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<KeywordCategory>(KeywordCategory value) {
 switch(value) {
 case KeywordCategory::KEYWORD_RESERVED:
  return "KEYWORD_RESERVED";
 case KeywordCategory::KEYWORD_UNRESERVED:
  return "KEYWORD_UNRESERVED";
 case KeywordCategory::KEYWORD_TYPE_FUNC:
  return "KEYWORD_TYPE_FUNC";
 case KeywordCategory::KEYWORD_COL_NAME:
  return "KEYWORD_COL_NAME";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
KeywordCategory EnumUtil::FromString<KeywordCategory>(const char *value) {
 if (StringUtil::Equals(value, "KEYWORD_RESERVED")) {
  return KeywordCategory::KEYWORD_RESERVED;
 }
 if (StringUtil::Equals(value, "KEYWORD_UNRESERVED")) {
  return KeywordCategory::KEYWORD_UNRESERVED;
 }
 if (StringUtil::Equals(value, "KEYWORD_TYPE_FUNC")) {
  return KeywordCategory::KEYWORD_TYPE_FUNC;
 }
 if (StringUtil::Equals(value, "KEYWORD_COL_NAME")) {
  return KeywordCategory::KEYWORD_COL_NAME;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<LoadType>(LoadType value) {
 switch(value) {
 case LoadType::LOAD:
  return "LOAD";
 case LoadType::INSTALL:
  return "INSTALL";
 case LoadType::FORCE_INSTALL:
  return "FORCE_INSTALL";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
LoadType EnumUtil::FromString<LoadType>(const char *value) {
 if (StringUtil::Equals(value, "LOAD")) {
  return LoadType::LOAD;
 }
 if (StringUtil::Equals(value, "INSTALL")) {
  return LoadType::INSTALL;
 }
 if (StringUtil::Equals(value, "FORCE_INSTALL")) {
  return LoadType::FORCE_INSTALL;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<LogicalOperatorType>(LogicalOperatorType value) {
 switch(value) {
 case LogicalOperatorType::LOGICAL_INVALID:
  return "LOGICAL_INVALID";
 case LogicalOperatorType::LOGICAL_PROJECTION:
  return "LOGICAL_PROJECTION";
 case LogicalOperatorType::LOGICAL_FILTER:
  return "LOGICAL_FILTER";
 case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
  return "LOGICAL_AGGREGATE_AND_GROUP_BY";
 case LogicalOperatorType::LOGICAL_WINDOW:
  return "LOGICAL_WINDOW";
 case LogicalOperatorType::LOGICAL_UNNEST:
  return "LOGICAL_UNNEST";
 case LogicalOperatorType::LOGICAL_LIMIT:
  return "LOGICAL_LIMIT";
 case LogicalOperatorType::LOGICAL_ORDER_BY:
  return "LOGICAL_ORDER_BY";
 case LogicalOperatorType::LOGICAL_TOP_N:
  return "LOGICAL_TOP_N";
 case LogicalOperatorType::LOGICAL_COPY_TO_FILE:
  return "LOGICAL_COPY_TO_FILE";
 case LogicalOperatorType::LOGICAL_DISTINCT:
  return "LOGICAL_DISTINCT";
 case LogicalOperatorType::LOGICAL_SAMPLE:
  return "LOGICAL_SAMPLE";
 case LogicalOperatorType::LOGICAL_LIMIT_PERCENT:
  return "LOGICAL_LIMIT_PERCENT";
 case LogicalOperatorType::LOGICAL_PIVOT:
  return "LOGICAL_PIVOT";
 case LogicalOperatorType::LOGICAL_COPY_DATABASE:
  return "LOGICAL_COPY_DATABASE";
 case LogicalOperatorType::LOGICAL_GET:
  return "LOGICAL_GET";
 case LogicalOperatorType::LOGICAL_CHUNK_GET:
  return "LOGICAL_CHUNK_GET";
 case LogicalOperatorType::LOGICAL_DELIM_GET:
  return "LOGICAL_DELIM_GET";
 case LogicalOperatorType::LOGICAL_EXPRESSION_GET:
  return "LOGICAL_EXPRESSION_GET";
 case LogicalOperatorType::LOGICAL_DUMMY_SCAN:
  return "LOGICAL_DUMMY_SCAN";
 case LogicalOperatorType::LOGICAL_EMPTY_RESULT:
  return "LOGICAL_EMPTY_RESULT";
 case LogicalOperatorType::LOGICAL_CTE_REF:
  return "LOGICAL_CTE_REF";
 case LogicalOperatorType::LOGICAL_JOIN:
  return "LOGICAL_JOIN";
 case LogicalOperatorType::LOGICAL_DELIM_JOIN:
  return "LOGICAL_DELIM_JOIN";
 case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
  return "LOGICAL_COMPARISON_JOIN";
 case LogicalOperatorType::LOGICAL_ANY_JOIN:
  return "LOGICAL_ANY_JOIN";
 case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
  return "LOGICAL_CROSS_PRODUCT";
 case LogicalOperatorType::LOGICAL_POSITIONAL_JOIN:
  return "LOGICAL_POSITIONAL_JOIN";
 case LogicalOperatorType::LOGICAL_ASOF_JOIN:
  return "LOGICAL_ASOF_JOIN";
 case LogicalOperatorType::LOGICAL_DEPENDENT_JOIN:
  return "LOGICAL_DEPENDENT_JOIN";
 case LogicalOperatorType::LOGICAL_UNION:
  return "LOGICAL_UNION";
 case LogicalOperatorType::LOGICAL_EXCEPT:
  return "LOGICAL_EXCEPT";
 case LogicalOperatorType::LOGICAL_INTERSECT:
  return "LOGICAL_INTERSECT";
 case LogicalOperatorType::LOGICAL_RECURSIVE_CTE:
  return "LOGICAL_RECURSIVE_CTE";
 case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
  return "LOGICAL_MATERIALIZED_CTE";
 case LogicalOperatorType::LOGICAL_INSERT:
  return "LOGICAL_INSERT";
 case LogicalOperatorType::LOGICAL_DELETE:
  return "LOGICAL_DELETE";
 case LogicalOperatorType::LOGICAL_UPDATE:
  return "LOGICAL_UPDATE";
 case LogicalOperatorType::LOGICAL_ALTER:
  return "LOGICAL_ALTER";
 case LogicalOperatorType::LOGICAL_CREATE_TABLE:
  return "LOGICAL_CREATE_TABLE";
 case LogicalOperatorType::LOGICAL_CREATE_INDEX:
  return "LOGICAL_CREATE_INDEX";
 case LogicalOperatorType::LOGICAL_CREATE_SEQUENCE:
  return "LOGICAL_CREATE_SEQUENCE";
 case LogicalOperatorType::LOGICAL_CREATE_VIEW:
  return "LOGICAL_CREATE_VIEW";
 case LogicalOperatorType::LOGICAL_CREATE_SCHEMA:
  return "LOGICAL_CREATE_SCHEMA";
 case LogicalOperatorType::LOGICAL_CREATE_MACRO:
  return "LOGICAL_CREATE_MACRO";
 case LogicalOperatorType::LOGICAL_DROP:
  return "LOGICAL_DROP";
 case LogicalOperatorType::LOGICAL_PRAGMA:
  return "LOGICAL_PRAGMA";
 case LogicalOperatorType::LOGICAL_TRANSACTION:
  return "LOGICAL_TRANSACTION";
 case LogicalOperatorType::LOGICAL_CREATE_TYPE:
  return "LOGICAL_CREATE_TYPE";
 case LogicalOperatorType::LOGICAL_ATTACH:
  return "LOGICAL_ATTACH";
 case LogicalOperatorType::LOGICAL_DETACH:
  return "LOGICAL_DETACH";
 case LogicalOperatorType::LOGICAL_EXPLAIN:
  return "LOGICAL_EXPLAIN";
 case LogicalOperatorType::LOGICAL_SHOW:
  return "LOGICAL_SHOW";
 case LogicalOperatorType::LOGICAL_PREPARE:
  return "LOGICAL_PREPARE";
 case LogicalOperatorType::LOGICAL_EXECUTE:
  return "LOGICAL_EXECUTE";
 case LogicalOperatorType::LOGICAL_EXPORT:
  return "LOGICAL_EXPORT";
 case LogicalOperatorType::LOGICAL_VACUUM:
  return "LOGICAL_VACUUM";
 case LogicalOperatorType::LOGICAL_SET:
  return "LOGICAL_SET";
 case LogicalOperatorType::LOGICAL_LOAD:
  return "LOGICAL_LOAD";
 case LogicalOperatorType::LOGICAL_RESET:
  return "LOGICAL_RESET";
 case LogicalOperatorType::LOGICAL_CREATE_SECRET:
  return "LOGICAL_CREATE_SECRET";
 case LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR:
  return "LOGICAL_EXTENSION_OPERATOR";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
LogicalOperatorType EnumUtil::FromString<LogicalOperatorType>(const char *value) {
 if (StringUtil::Equals(value, "LOGICAL_INVALID")) {
  return LogicalOperatorType::LOGICAL_INVALID;
 }
 if (StringUtil::Equals(value, "LOGICAL_PROJECTION")) {
  return LogicalOperatorType::LOGICAL_PROJECTION;
 }
 if (StringUtil::Equals(value, "LOGICAL_FILTER")) {
  return LogicalOperatorType::LOGICAL_FILTER;
 }
 if (StringUtil::Equals(value, "LOGICAL_AGGREGATE_AND_GROUP_BY")) {
  return LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY;
 }
 if (StringUtil::Equals(value, "LOGICAL_WINDOW")) {
  return LogicalOperatorType::LOGICAL_WINDOW;
 }
 if (StringUtil::Equals(value, "LOGICAL_UNNEST")) {
  return LogicalOperatorType::LOGICAL_UNNEST;
 }
 if (StringUtil::Equals(value, "LOGICAL_LIMIT")) {
  return LogicalOperatorType::LOGICAL_LIMIT;
 }
 if (StringUtil::Equals(value, "LOGICAL_ORDER_BY")) {
  return LogicalOperatorType::LOGICAL_ORDER_BY;
 }
 if (StringUtil::Equals(value, "LOGICAL_TOP_N")) {
  return LogicalOperatorType::LOGICAL_TOP_N;
 }
 if (StringUtil::Equals(value, "LOGICAL_COPY_TO_FILE")) {
  return LogicalOperatorType::LOGICAL_COPY_TO_FILE;
 }
 if (StringUtil::Equals(value, "LOGICAL_DISTINCT")) {
  return LogicalOperatorType::LOGICAL_DISTINCT;
 }
 if (StringUtil::Equals(value, "LOGICAL_SAMPLE")) {
  return LogicalOperatorType::LOGICAL_SAMPLE;
 }
 if (StringUtil::Equals(value, "LOGICAL_LIMIT_PERCENT")) {
  return LogicalOperatorType::LOGICAL_LIMIT_PERCENT;
 }
 if (StringUtil::Equals(value, "LOGICAL_PIVOT")) {
  return LogicalOperatorType::LOGICAL_PIVOT;
 }
 if (StringUtil::Equals(value, "LOGICAL_COPY_DATABASE")) {
  return LogicalOperatorType::LOGICAL_COPY_DATABASE;
 }
 if (StringUtil::Equals(value, "LOGICAL_GET")) {
  return LogicalOperatorType::LOGICAL_GET;
 }
 if (StringUtil::Equals(value, "LOGICAL_CHUNK_GET")) {
  return LogicalOperatorType::LOGICAL_CHUNK_GET;
 }
 if (StringUtil::Equals(value, "LOGICAL_DELIM_GET")) {
  return LogicalOperatorType::LOGICAL_DELIM_GET;
 }
 if (StringUtil::Equals(value, "LOGICAL_EXPRESSION_GET")) {
  return LogicalOperatorType::LOGICAL_EXPRESSION_GET;
 }
 if (StringUtil::Equals(value, "LOGICAL_DUMMY_SCAN")) {
  return LogicalOperatorType::LOGICAL_DUMMY_SCAN;
 }
 if (StringUtil::Equals(value, "LOGICAL_EMPTY_RESULT")) {
  return LogicalOperatorType::LOGICAL_EMPTY_RESULT;
 }
 if (StringUtil::Equals(value, "LOGICAL_CTE_REF")) {
  return LogicalOperatorType::LOGICAL_CTE_REF;
 }
 if (StringUtil::Equals(value, "LOGICAL_JOIN")) {
  return LogicalOperatorType::LOGICAL_JOIN;
 }
 if (StringUtil::Equals(value, "LOGICAL_DELIM_JOIN")) {
  return LogicalOperatorType::LOGICAL_DELIM_JOIN;
 }
 if (StringUtil::Equals(value, "LOGICAL_COMPARISON_JOIN")) {
  return LogicalOperatorType::LOGICAL_COMPARISON_JOIN;
 }
 if (StringUtil::Equals(value, "LOGICAL_ANY_JOIN")) {
  return LogicalOperatorType::LOGICAL_ANY_JOIN;
 }
 if (StringUtil::Equals(value, "LOGICAL_CROSS_PRODUCT")) {
  return LogicalOperatorType::LOGICAL_CROSS_PRODUCT;
 }
 if (StringUtil::Equals(value, "LOGICAL_POSITIONAL_JOIN")) {
  return LogicalOperatorType::LOGICAL_POSITIONAL_JOIN;
 }
 if (StringUtil::Equals(value, "LOGICAL_ASOF_JOIN")) {
  return LogicalOperatorType::LOGICAL_ASOF_JOIN;
 }
 if (StringUtil::Equals(value, "LOGICAL_DEPENDENT_JOIN")) {
  return LogicalOperatorType::LOGICAL_DEPENDENT_JOIN;
 }
 if (StringUtil::Equals(value, "LOGICAL_UNION")) {
  return LogicalOperatorType::LOGICAL_UNION;
 }
 if (StringUtil::Equals(value, "LOGICAL_EXCEPT")) {
  return LogicalOperatorType::LOGICAL_EXCEPT;
 }
 if (StringUtil::Equals(value, "LOGICAL_INTERSECT")) {
  return LogicalOperatorType::LOGICAL_INTERSECT;
 }
 if (StringUtil::Equals(value, "LOGICAL_RECURSIVE_CTE")) {
  return LogicalOperatorType::LOGICAL_RECURSIVE_CTE;
 }
 if (StringUtil::Equals(value, "LOGICAL_MATERIALIZED_CTE")) {
  return LogicalOperatorType::LOGICAL_MATERIALIZED_CTE;
 }
 if (StringUtil::Equals(value, "LOGICAL_INSERT")) {
  return LogicalOperatorType::LOGICAL_INSERT;
 }
 if (StringUtil::Equals(value, "LOGICAL_DELETE")) {
  return LogicalOperatorType::LOGICAL_DELETE;
 }
 if (StringUtil::Equals(value, "LOGICAL_UPDATE")) {
  return LogicalOperatorType::LOGICAL_UPDATE;
 }
 if (StringUtil::Equals(value, "LOGICAL_ALTER")) {
  return LogicalOperatorType::LOGICAL_ALTER;
 }
 if (StringUtil::Equals(value, "LOGICAL_CREATE_TABLE")) {
  return LogicalOperatorType::LOGICAL_CREATE_TABLE;
 }
 if (StringUtil::Equals(value, "LOGICAL_CREATE_INDEX")) {
  return LogicalOperatorType::LOGICAL_CREATE_INDEX;
 }
 if (StringUtil::Equals(value, "LOGICAL_CREATE_SEQUENCE")) {
  return LogicalOperatorType::LOGICAL_CREATE_SEQUENCE;
 }
 if (StringUtil::Equals(value, "LOGICAL_CREATE_VIEW")) {
  return LogicalOperatorType::LOGICAL_CREATE_VIEW;
 }
 if (StringUtil::Equals(value, "LOGICAL_CREATE_SCHEMA")) {
  return LogicalOperatorType::LOGICAL_CREATE_SCHEMA;
 }
 if (StringUtil::Equals(value, "LOGICAL_CREATE_MACRO")) {
  return LogicalOperatorType::LOGICAL_CREATE_MACRO;
 }
 if (StringUtil::Equals(value, "LOGICAL_DROP")) {
  return LogicalOperatorType::LOGICAL_DROP;
 }
 if (StringUtil::Equals(value, "LOGICAL_PRAGMA")) {
  return LogicalOperatorType::LOGICAL_PRAGMA;
 }
 if (StringUtil::Equals(value, "LOGICAL_TRANSACTION")) {
  return LogicalOperatorType::LOGICAL_TRANSACTION;
 }
 if (StringUtil::Equals(value, "LOGICAL_CREATE_TYPE")) {
  return LogicalOperatorType::LOGICAL_CREATE_TYPE;
 }
 if (StringUtil::Equals(value, "LOGICAL_ATTACH")) {
  return LogicalOperatorType::LOGICAL_ATTACH;
 }
 if (StringUtil::Equals(value, "LOGICAL_DETACH")) {
  return LogicalOperatorType::LOGICAL_DETACH;
 }
 if (StringUtil::Equals(value, "LOGICAL_EXPLAIN")) {
  return LogicalOperatorType::LOGICAL_EXPLAIN;
 }
 if (StringUtil::Equals(value, "LOGICAL_SHOW")) {
  return LogicalOperatorType::LOGICAL_SHOW;
 }
 if (StringUtil::Equals(value, "LOGICAL_PREPARE")) {
  return LogicalOperatorType::LOGICAL_PREPARE;
 }
 if (StringUtil::Equals(value, "LOGICAL_EXECUTE")) {
  return LogicalOperatorType::LOGICAL_EXECUTE;
 }
 if (StringUtil::Equals(value, "LOGICAL_EXPORT")) {
  return LogicalOperatorType::LOGICAL_EXPORT;
 }
 if (StringUtil::Equals(value, "LOGICAL_VACUUM")) {
  return LogicalOperatorType::LOGICAL_VACUUM;
 }
 if (StringUtil::Equals(value, "LOGICAL_SET")) {
  return LogicalOperatorType::LOGICAL_SET;
 }
 if (StringUtil::Equals(value, "LOGICAL_LOAD")) {
  return LogicalOperatorType::LOGICAL_LOAD;
 }
 if (StringUtil::Equals(value, "LOGICAL_RESET")) {
  return LogicalOperatorType::LOGICAL_RESET;
 }
 if (StringUtil::Equals(value, "LOGICAL_CREATE_SECRET")) {
  return LogicalOperatorType::LOGICAL_CREATE_SECRET;
 }
 if (StringUtil::Equals(value, "LOGICAL_EXTENSION_OPERATOR")) {
  return LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<LogicalTypeId>(LogicalTypeId value) {
 switch(value) {
 case LogicalTypeId::INVALID:
  return "INVALID";
 case LogicalTypeId::SQLNULL:
  return "NULL";
 case LogicalTypeId::UNKNOWN:
  return "UNKNOWN";
 case LogicalTypeId::ANY:
  return "ANY";
 case LogicalTypeId::USER:
  return "USER";
 case LogicalTypeId::BOOLEAN:
  return "BOOLEAN";
 case LogicalTypeId::TINYINT:
  return "TINYINT";
 case LogicalTypeId::SMALLINT:
  return "SMALLINT";
 case LogicalTypeId::INTEGER:
  return "INTEGER";
 case LogicalTypeId::BIGINT:
  return "BIGINT";
 case LogicalTypeId::DATE:
  return "DATE";
 case LogicalTypeId::TIME:
  return "TIME";
 case LogicalTypeId::TIMESTAMP_SEC:
  return "TIMESTAMP_S";
 case LogicalTypeId::TIMESTAMP_MS:
  return "TIMESTAMP_MS";
 case LogicalTypeId::TIMESTAMP:
  return "TIMESTAMP";
 case LogicalTypeId::TIMESTAMP_NS:
  return "TIMESTAMP_NS";
 case LogicalTypeId::DECIMAL:
  return "DECIMAL";
 case LogicalTypeId::FLOAT:
  return "FLOAT";
 case LogicalTypeId::DOUBLE:
  return "DOUBLE";
 case LogicalTypeId::CHAR:
  return "CHAR";
 case LogicalTypeId::VARCHAR:
  return "VARCHAR";
 case LogicalTypeId::BLOB:
  return "BLOB";
 case LogicalTypeId::INTERVAL:
  return "INTERVAL";
 case LogicalTypeId::UTINYINT:
  return "UTINYINT";
 case LogicalTypeId::USMALLINT:
  return "USMALLINT";
 case LogicalTypeId::UINTEGER:
  return "UINTEGER";
 case LogicalTypeId::UBIGINT:
  return "UBIGINT";
 case LogicalTypeId::TIMESTAMP_TZ:
  return "TIMESTAMP WITH TIME ZONE";
 case LogicalTypeId::TIME_TZ:
  return "TIME WITH TIME ZONE";
 case LogicalTypeId::BIT:
  return "BIT";
 case LogicalTypeId::HUGEINT:
  return "HUGEINT";
 case LogicalTypeId::POINTER:
  return "POINTER";
 case LogicalTypeId::VALIDITY:
  return "VALIDITY";
 case LogicalTypeId::UUID:
  return "UUID";
 case LogicalTypeId::STRUCT:
  return "STRUCT";
 case LogicalTypeId::LIST:
  return "LIST";
 case LogicalTypeId::MAP:
  return "MAP";
 case LogicalTypeId::TABLE:
  return "TABLE";
 case LogicalTypeId::ENUM:
  return "ENUM";
 case LogicalTypeId::AGGREGATE_STATE:
  return "AGGREGATE_STATE";
 case LogicalTypeId::LAMBDA:
  return "LAMBDA";
 case LogicalTypeId::UNION:
  return "UNION";
 case LogicalTypeId::ARRAY:
  return "ARRAY";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
LogicalTypeId EnumUtil::FromString<LogicalTypeId>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return LogicalTypeId::INVALID;
 }
 if (StringUtil::Equals(value, "NULL")) {
  return LogicalTypeId::SQLNULL;
 }
 if (StringUtil::Equals(value, "UNKNOWN")) {
  return LogicalTypeId::UNKNOWN;
 }
 if (StringUtil::Equals(value, "ANY")) {
  return LogicalTypeId::ANY;
 }
 if (StringUtil::Equals(value, "USER")) {
  return LogicalTypeId::USER;
 }
 if (StringUtil::Equals(value, "BOOLEAN")) {
  return LogicalTypeId::BOOLEAN;
 }
 if (StringUtil::Equals(value, "TINYINT")) {
  return LogicalTypeId::TINYINT;
 }
 if (StringUtil::Equals(value, "SMALLINT")) {
  return LogicalTypeId::SMALLINT;
 }
 if (StringUtil::Equals(value, "INTEGER")) {
  return LogicalTypeId::INTEGER;
 }
 if (StringUtil::Equals(value, "BIGINT")) {
  return LogicalTypeId::BIGINT;
 }
 if (StringUtil::Equals(value, "DATE")) {
  return LogicalTypeId::DATE;
 }
 if (StringUtil::Equals(value, "TIME")) {
  return LogicalTypeId::TIME;
 }
 if (StringUtil::Equals(value, "TIMESTAMP_S")) {
  return LogicalTypeId::TIMESTAMP_SEC;
 }
 if (StringUtil::Equals(value, "TIMESTAMP_MS")) {
  return LogicalTypeId::TIMESTAMP_MS;
 }
 if (StringUtil::Equals(value, "TIMESTAMP")) {
  return LogicalTypeId::TIMESTAMP;
 }
 if (StringUtil::Equals(value, "TIMESTAMP_NS")) {
  return LogicalTypeId::TIMESTAMP_NS;
 }
 if (StringUtil::Equals(value, "DECIMAL")) {
  return LogicalTypeId::DECIMAL;
 }
 if (StringUtil::Equals(value, "FLOAT")) {
  return LogicalTypeId::FLOAT;
 }
 if (StringUtil::Equals(value, "DOUBLE")) {
  return LogicalTypeId::DOUBLE;
 }
 if (StringUtil::Equals(value, "CHAR")) {
  return LogicalTypeId::CHAR;
 }
 if (StringUtil::Equals(value, "VARCHAR")) {
  return LogicalTypeId::VARCHAR;
 }
 if (StringUtil::Equals(value, "BLOB")) {
  return LogicalTypeId::BLOB;
 }
 if (StringUtil::Equals(value, "INTERVAL")) {
  return LogicalTypeId::INTERVAL;
 }
 if (StringUtil::Equals(value, "UTINYINT")) {
  return LogicalTypeId::UTINYINT;
 }
 if (StringUtil::Equals(value, "USMALLINT")) {
  return LogicalTypeId::USMALLINT;
 }
 if (StringUtil::Equals(value, "UINTEGER")) {
  return LogicalTypeId::UINTEGER;
 }
 if (StringUtil::Equals(value, "UBIGINT")) {
  return LogicalTypeId::UBIGINT;
 }
 if (StringUtil::Equals(value, "TIMESTAMP WITH TIME ZONE")) {
  return LogicalTypeId::TIMESTAMP_TZ;
 }
 if (StringUtil::Equals(value, "TIME WITH TIME ZONE")) {
  return LogicalTypeId::TIME_TZ;
 }
 if (StringUtil::Equals(value, "BIT")) {
  return LogicalTypeId::BIT;
 }
 if (StringUtil::Equals(value, "HUGEINT")) {
  return LogicalTypeId::HUGEINT;
 }
 if (StringUtil::Equals(value, "POINTER")) {
  return LogicalTypeId::POINTER;
 }
 if (StringUtil::Equals(value, "VALIDITY")) {
  return LogicalTypeId::VALIDITY;
 }
 if (StringUtil::Equals(value, "UUID")) {
  return LogicalTypeId::UUID;
 }
 if (StringUtil::Equals(value, "STRUCT")) {
  return LogicalTypeId::STRUCT;
 }
 if (StringUtil::Equals(value, "LIST")) {
  return LogicalTypeId::LIST;
 }
 if (StringUtil::Equals(value, "MAP")) {
  return LogicalTypeId::MAP;
 }
 if (StringUtil::Equals(value, "TABLE")) {
  return LogicalTypeId::TABLE;
 }
 if (StringUtil::Equals(value, "ENUM")) {
  return LogicalTypeId::ENUM;
 }
 if (StringUtil::Equals(value, "AGGREGATE_STATE")) {
  return LogicalTypeId::AGGREGATE_STATE;
 }
 if (StringUtil::Equals(value, "LAMBDA")) {
  return LogicalTypeId::LAMBDA;
 }
 if (StringUtil::Equals(value, "UNION")) {
  return LogicalTypeId::UNION;
 }
 if (StringUtil::Equals(value, "ARRAY")) {
  return LogicalTypeId::ARRAY;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<LookupResultType>(LookupResultType value) {
 switch(value) {
 case LookupResultType::LOOKUP_MISS:
  return "LOOKUP_MISS";
 case LookupResultType::LOOKUP_HIT:
  return "LOOKUP_HIT";
 case LookupResultType::LOOKUP_NULL:
  return "LOOKUP_NULL";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
LookupResultType EnumUtil::FromString<LookupResultType>(const char *value) {
 if (StringUtil::Equals(value, "LOOKUP_MISS")) {
  return LookupResultType::LOOKUP_MISS;
 }
 if (StringUtil::Equals(value, "LOOKUP_HIT")) {
  return LookupResultType::LOOKUP_HIT;
 }
 if (StringUtil::Equals(value, "LOOKUP_NULL")) {
  return LookupResultType::LOOKUP_NULL;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<MacroType>(MacroType value) {
 switch(value) {
 case MacroType::VOID_MACRO:
  return "VOID_MACRO";
 case MacroType::TABLE_MACRO:
  return "TABLE_MACRO";
 case MacroType::SCALAR_MACRO:
  return "SCALAR_MACRO";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
MacroType EnumUtil::FromString<MacroType>(const char *value) {
 if (StringUtil::Equals(value, "VOID_MACRO")) {
  return MacroType::VOID_MACRO;
 }
 if (StringUtil::Equals(value, "TABLE_MACRO")) {
  return MacroType::TABLE_MACRO;
 }
 if (StringUtil::Equals(value, "SCALAR_MACRO")) {
  return MacroType::SCALAR_MACRO;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<MapInvalidReason>(MapInvalidReason value) {
 switch(value) {
 case MapInvalidReason::VALID:
  return "VALID";
 case MapInvalidReason::NULL_KEY_LIST:
  return "NULL_KEY_LIST";
 case MapInvalidReason::NULL_KEY:
  return "NULL_KEY";
 case MapInvalidReason::DUPLICATE_KEY:
  return "DUPLICATE_KEY";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
MapInvalidReason EnumUtil::FromString<MapInvalidReason>(const char *value) {
 if (StringUtil::Equals(value, "VALID")) {
  return MapInvalidReason::VALID;
 }
 if (StringUtil::Equals(value, "NULL_KEY_LIST")) {
  return MapInvalidReason::NULL_KEY_LIST;
 }
 if (StringUtil::Equals(value, "NULL_KEY")) {
  return MapInvalidReason::NULL_KEY;
 }
 if (StringUtil::Equals(value, "DUPLICATE_KEY")) {
  return MapInvalidReason::DUPLICATE_KEY;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<NType>(NType value) {
 switch(value) {
 case NType::PREFIX:
  return "PREFIX";
 case NType::LEAF:
  return "LEAF";
 case NType::NODE_4:
  return "NODE_4";
 case NType::NODE_16:
  return "NODE_16";
 case NType::NODE_48:
  return "NODE_48";
 case NType::NODE_256:
  return "NODE_256";
 case NType::LEAF_INLINED:
  return "LEAF_INLINED";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
NType EnumUtil::FromString<NType>(const char *value) {
 if (StringUtil::Equals(value, "PREFIX")) {
  return NType::PREFIX;
 }
 if (StringUtil::Equals(value, "LEAF")) {
  return NType::LEAF;
 }
 if (StringUtil::Equals(value, "NODE_4")) {
  return NType::NODE_4;
 }
 if (StringUtil::Equals(value, "NODE_16")) {
  return NType::NODE_16;
 }
 if (StringUtil::Equals(value, "NODE_48")) {
  return NType::NODE_48;
 }
 if (StringUtil::Equals(value, "NODE_256")) {
  return NType::NODE_256;
 }
 if (StringUtil::Equals(value, "LEAF_INLINED")) {
  return NType::LEAF_INLINED;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<NewLineIdentifier>(NewLineIdentifier value) {
 switch(value) {
 case NewLineIdentifier::SINGLE:
  return "SINGLE";
 case NewLineIdentifier::CARRY_ON:
  return "CARRY_ON";
 case NewLineIdentifier::MIX:
  return "MIX";
 case NewLineIdentifier::NOT_SET:
  return "NOT_SET";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
NewLineIdentifier EnumUtil::FromString<NewLineIdentifier>(const char *value) {
 if (StringUtil::Equals(value, "SINGLE")) {
  return NewLineIdentifier::SINGLE;
 }
 if (StringUtil::Equals(value, "CARRY_ON")) {
  return NewLineIdentifier::CARRY_ON;
 }
 if (StringUtil::Equals(value, "MIX")) {
  return NewLineIdentifier::MIX;
 }
 if (StringUtil::Equals(value, "NOT_SET")) {
  return NewLineIdentifier::NOT_SET;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<OnConflictAction>(OnConflictAction value) {
 switch(value) {
 case OnConflictAction::THROW:
  return "THROW";
 case OnConflictAction::NOTHING:
  return "NOTHING";
 case OnConflictAction::UPDATE:
  return "UPDATE";
 case OnConflictAction::REPLACE:
  return "REPLACE";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
OnConflictAction EnumUtil::FromString<OnConflictAction>(const char *value) {
 if (StringUtil::Equals(value, "THROW")) {
  return OnConflictAction::THROW;
 }
 if (StringUtil::Equals(value, "NOTHING")) {
  return OnConflictAction::NOTHING;
 }
 if (StringUtil::Equals(value, "UPDATE")) {
  return OnConflictAction::UPDATE;
 }
 if (StringUtil::Equals(value, "REPLACE")) {
  return OnConflictAction::REPLACE;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<OnCreateConflict>(OnCreateConflict value) {
 switch(value) {
 case OnCreateConflict::ERROR_ON_CONFLICT:
  return "ERROR_ON_CONFLICT";
 case OnCreateConflict::IGNORE_ON_CONFLICT:
  return "IGNORE_ON_CONFLICT";
 case OnCreateConflict::REPLACE_ON_CONFLICT:
  return "REPLACE_ON_CONFLICT";
 case OnCreateConflict::ALTER_ON_CONFLICT:
  return "ALTER_ON_CONFLICT";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
OnCreateConflict EnumUtil::FromString<OnCreateConflict>(const char *value) {
 if (StringUtil::Equals(value, "ERROR_ON_CONFLICT")) {
  return OnCreateConflict::ERROR_ON_CONFLICT;
 }
 if (StringUtil::Equals(value, "IGNORE_ON_CONFLICT")) {
  return OnCreateConflict::IGNORE_ON_CONFLICT;
 }
 if (StringUtil::Equals(value, "REPLACE_ON_CONFLICT")) {
  return OnCreateConflict::REPLACE_ON_CONFLICT;
 }
 if (StringUtil::Equals(value, "ALTER_ON_CONFLICT")) {
  return OnCreateConflict::ALTER_ON_CONFLICT;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<OnEntryNotFound>(OnEntryNotFound value) {
 switch(value) {
 case OnEntryNotFound::THROW_EXCEPTION:
  return "THROW_EXCEPTION";
 case OnEntryNotFound::RETURN_NULL:
  return "RETURN_NULL";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
OnEntryNotFound EnumUtil::FromString<OnEntryNotFound>(const char *value) {
 if (StringUtil::Equals(value, "THROW_EXCEPTION")) {
  return OnEntryNotFound::THROW_EXCEPTION;
 }
 if (StringUtil::Equals(value, "RETURN_NULL")) {
  return OnEntryNotFound::RETURN_NULL;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<OperatorFinalizeResultType>(OperatorFinalizeResultType value) {
 switch(value) {
 case OperatorFinalizeResultType::HAVE_MORE_OUTPUT:
  return "HAVE_MORE_OUTPUT";
 case OperatorFinalizeResultType::FINISHED:
  return "FINISHED";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
OperatorFinalizeResultType EnumUtil::FromString<OperatorFinalizeResultType>(const char *value) {
 if (StringUtil::Equals(value, "HAVE_MORE_OUTPUT")) {
  return OperatorFinalizeResultType::HAVE_MORE_OUTPUT;
 }
 if (StringUtil::Equals(value, "FINISHED")) {
  return OperatorFinalizeResultType::FINISHED;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<OperatorResultType>(OperatorResultType value) {
 switch(value) {
 case OperatorResultType::NEED_MORE_INPUT:
  return "NEED_MORE_INPUT";
 case OperatorResultType::HAVE_MORE_OUTPUT:
  return "HAVE_MORE_OUTPUT";
 case OperatorResultType::FINISHED:
  return "FINISHED";
 case OperatorResultType::BLOCKED:
  return "BLOCKED";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
OperatorResultType EnumUtil::FromString<OperatorResultType>(const char *value) {
 if (StringUtil::Equals(value, "NEED_MORE_INPUT")) {
  return OperatorResultType::NEED_MORE_INPUT;
 }
 if (StringUtil::Equals(value, "HAVE_MORE_OUTPUT")) {
  return OperatorResultType::HAVE_MORE_OUTPUT;
 }
 if (StringUtil::Equals(value, "FINISHED")) {
  return OperatorResultType::FINISHED;
 }
 if (StringUtil::Equals(value, "BLOCKED")) {
  return OperatorResultType::BLOCKED;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<OptimizerType>(OptimizerType value) {
 switch(value) {
 case OptimizerType::INVALID:
  return "INVALID";
 case OptimizerType::EXPRESSION_REWRITER:
  return "EXPRESSION_REWRITER";
 case OptimizerType::FILTER_PULLUP:
  return "FILTER_PULLUP";
 case OptimizerType::FILTER_PUSHDOWN:
  return "FILTER_PUSHDOWN";
 case OptimizerType::REGEX_RANGE:
  return "REGEX_RANGE";
 case OptimizerType::IN_CLAUSE:
  return "IN_CLAUSE";
 case OptimizerType::JOIN_ORDER:
  return "JOIN_ORDER";
 case OptimizerType::DELIMINATOR:
  return "DELIMINATOR";
 case OptimizerType::UNNEST_REWRITER:
  return "UNNEST_REWRITER";
 case OptimizerType::UNUSED_COLUMNS:
  return "UNUSED_COLUMNS";
 case OptimizerType::STATISTICS_PROPAGATION:
  return "STATISTICS_PROPAGATION";
 case OptimizerType::COMMON_SUBEXPRESSIONS:
  return "COMMON_SUBEXPRESSIONS";
 case OptimizerType::COMMON_AGGREGATE:
  return "COMMON_AGGREGATE";
 case OptimizerType::COLUMN_LIFETIME:
  return "COLUMN_LIFETIME";
 case OptimizerType::TOP_N:
  return "TOP_N";
 case OptimizerType::COMPRESSED_MATERIALIZATION:
  return "COMPRESSED_MATERIALIZATION";
 case OptimizerType::DUPLICATE_GROUPS:
  return "DUPLICATE_GROUPS";
 case OptimizerType::REORDER_FILTER:
  return "REORDER_FILTER";
 case OptimizerType::EXTENSION:
  return "EXTENSION";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
OptimizerType EnumUtil::FromString<OptimizerType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return OptimizerType::INVALID;
 }
 if (StringUtil::Equals(value, "EXPRESSION_REWRITER")) {
  return OptimizerType::EXPRESSION_REWRITER;
 }
 if (StringUtil::Equals(value, "FILTER_PULLUP")) {
  return OptimizerType::FILTER_PULLUP;
 }
 if (StringUtil::Equals(value, "FILTER_PUSHDOWN")) {
  return OptimizerType::FILTER_PUSHDOWN;
 }
 if (StringUtil::Equals(value, "REGEX_RANGE")) {
  return OptimizerType::REGEX_RANGE;
 }
 if (StringUtil::Equals(value, "IN_CLAUSE")) {
  return OptimizerType::IN_CLAUSE;
 }
 if (StringUtil::Equals(value, "JOIN_ORDER")) {
  return OptimizerType::JOIN_ORDER;
 }
 if (StringUtil::Equals(value, "DELIMINATOR")) {
  return OptimizerType::DELIMINATOR;
 }
 if (StringUtil::Equals(value, "UNNEST_REWRITER")) {
  return OptimizerType::UNNEST_REWRITER;
 }
 if (StringUtil::Equals(value, "UNUSED_COLUMNS")) {
  return OptimizerType::UNUSED_COLUMNS;
 }
 if (StringUtil::Equals(value, "STATISTICS_PROPAGATION")) {
  return OptimizerType::STATISTICS_PROPAGATION;
 }
 if (StringUtil::Equals(value, "COMMON_SUBEXPRESSIONS")) {
  return OptimizerType::COMMON_SUBEXPRESSIONS;
 }
 if (StringUtil::Equals(value, "COMMON_AGGREGATE")) {
  return OptimizerType::COMMON_AGGREGATE;
 }
 if (StringUtil::Equals(value, "COLUMN_LIFETIME")) {
  return OptimizerType::COLUMN_LIFETIME;
 }
 if (StringUtil::Equals(value, "TOP_N")) {
  return OptimizerType::TOP_N;
 }
 if (StringUtil::Equals(value, "COMPRESSED_MATERIALIZATION")) {
  return OptimizerType::COMPRESSED_MATERIALIZATION;
 }
 if (StringUtil::Equals(value, "DUPLICATE_GROUPS")) {
  return OptimizerType::DUPLICATE_GROUPS;
 }
 if (StringUtil::Equals(value, "REORDER_FILTER")) {
  return OptimizerType::REORDER_FILTER;
 }
 if (StringUtil::Equals(value, "EXTENSION")) {
  return OptimizerType::EXTENSION;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<OrderByNullType>(OrderByNullType value) {
 switch(value) {
 case OrderByNullType::INVALID:
  return "INVALID";
 case OrderByNullType::ORDER_DEFAULT:
  return "ORDER_DEFAULT";
 case OrderByNullType::NULLS_FIRST:
  return "NULLS_FIRST";
 case OrderByNullType::NULLS_LAST:
  return "NULLS_LAST";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
OrderByNullType EnumUtil::FromString<OrderByNullType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return OrderByNullType::INVALID;
 }
 if (StringUtil::Equals(value, "ORDER_DEFAULT") || StringUtil::Equals(value, "DEFAULT")) {
  return OrderByNullType::ORDER_DEFAULT;
 }
 if (StringUtil::Equals(value, "NULLS_FIRST") || StringUtil::Equals(value, "NULLS FIRST")) {
  return OrderByNullType::NULLS_FIRST;
 }
 if (StringUtil::Equals(value, "NULLS_LAST") || StringUtil::Equals(value, "NULLS LAST")) {
  return OrderByNullType::NULLS_LAST;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<OrderPreservationType>(OrderPreservationType value) {
 switch(value) {
 case OrderPreservationType::NO_ORDER:
  return "NO_ORDER";
 case OrderPreservationType::INSERTION_ORDER:
  return "INSERTION_ORDER";
 case OrderPreservationType::FIXED_ORDER:
  return "FIXED_ORDER";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
OrderPreservationType EnumUtil::FromString<OrderPreservationType>(const char *value) {
 if (StringUtil::Equals(value, "NO_ORDER")) {
  return OrderPreservationType::NO_ORDER;
 }
 if (StringUtil::Equals(value, "INSERTION_ORDER")) {
  return OrderPreservationType::INSERTION_ORDER;
 }
 if (StringUtil::Equals(value, "FIXED_ORDER")) {
  return OrderPreservationType::FIXED_ORDER;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<OrderType>(OrderType value) {
 switch(value) {
 case OrderType::INVALID:
  return "INVALID";
 case OrderType::ORDER_DEFAULT:
  return "ORDER_DEFAULT";
 case OrderType::ASCENDING:
  return "ASCENDING";
 case OrderType::DESCENDING:
  return "DESCENDING";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
OrderType EnumUtil::FromString<OrderType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return OrderType::INVALID;
 }
 if (StringUtil::Equals(value, "ORDER_DEFAULT") || StringUtil::Equals(value, "DEFAULT")) {
  return OrderType::ORDER_DEFAULT;
 }
 if (StringUtil::Equals(value, "ASCENDING") || StringUtil::Equals(value, "ASC")) {
  return OrderType::ASCENDING;
 }
 if (StringUtil::Equals(value, "DESCENDING") || StringUtil::Equals(value, "DESC")) {
  return OrderType::DESCENDING;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<OutputStream>(OutputStream value) {
 switch(value) {
 case OutputStream::STREAM_STDOUT:
  return "STREAM_STDOUT";
 case OutputStream::STREAM_STDERR:
  return "STREAM_STDERR";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
OutputStream EnumUtil::FromString<OutputStream>(const char *value) {
 if (StringUtil::Equals(value, "STREAM_STDOUT")) {
  return OutputStream::STREAM_STDOUT;
 }
 if (StringUtil::Equals(value, "STREAM_STDERR")) {
  return OutputStream::STREAM_STDERR;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ParseInfoType>(ParseInfoType value) {
 switch(value) {
 case ParseInfoType::ALTER_INFO:
  return "ALTER_INFO";
 case ParseInfoType::ATTACH_INFO:
  return "ATTACH_INFO";
 case ParseInfoType::COPY_INFO:
  return "COPY_INFO";
 case ParseInfoType::CREATE_INFO:
  return "CREATE_INFO";
 case ParseInfoType::CREATE_SECRET_INFO:
  return "CREATE_SECRET_INFO";
 case ParseInfoType::DETACH_INFO:
  return "DETACH_INFO";
 case ParseInfoType::DROP_INFO:
  return "DROP_INFO";
 case ParseInfoType::BOUND_EXPORT_DATA:
  return "BOUND_EXPORT_DATA";
 case ParseInfoType::LOAD_INFO:
  return "LOAD_INFO";
 case ParseInfoType::PRAGMA_INFO:
  return "PRAGMA_INFO";
 case ParseInfoType::SHOW_SELECT_INFO:
  return "SHOW_SELECT_INFO";
 case ParseInfoType::TRANSACTION_INFO:
  return "TRANSACTION_INFO";
 case ParseInfoType::VACUUM_INFO:
  return "VACUUM_INFO";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ParseInfoType EnumUtil::FromString<ParseInfoType>(const char *value) {
 if (StringUtil::Equals(value, "ALTER_INFO")) {
  return ParseInfoType::ALTER_INFO;
 }
 if (StringUtil::Equals(value, "ATTACH_INFO")) {
  return ParseInfoType::ATTACH_INFO;
 }
 if (StringUtil::Equals(value, "COPY_INFO")) {
  return ParseInfoType::COPY_INFO;
 }
 if (StringUtil::Equals(value, "CREATE_INFO")) {
  return ParseInfoType::CREATE_INFO;
 }
 if (StringUtil::Equals(value, "CREATE_SECRET_INFO")) {
  return ParseInfoType::CREATE_SECRET_INFO;
 }
 if (StringUtil::Equals(value, "DETACH_INFO")) {
  return ParseInfoType::DETACH_INFO;
 }
 if (StringUtil::Equals(value, "DROP_INFO")) {
  return ParseInfoType::DROP_INFO;
 }
 if (StringUtil::Equals(value, "BOUND_EXPORT_DATA")) {
  return ParseInfoType::BOUND_EXPORT_DATA;
 }
 if (StringUtil::Equals(value, "LOAD_INFO")) {
  return ParseInfoType::LOAD_INFO;
 }
 if (StringUtil::Equals(value, "PRAGMA_INFO")) {
  return ParseInfoType::PRAGMA_INFO;
 }
 if (StringUtil::Equals(value, "SHOW_SELECT_INFO")) {
  return ParseInfoType::SHOW_SELECT_INFO;
 }
 if (StringUtil::Equals(value, "TRANSACTION_INFO")) {
  return ParseInfoType::TRANSACTION_INFO;
 }
 if (StringUtil::Equals(value, "VACUUM_INFO")) {
  return ParseInfoType::VACUUM_INFO;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ParserExtensionResultType>(ParserExtensionResultType value) {
 switch(value) {
 case ParserExtensionResultType::PARSE_SUCCESSFUL:
  return "PARSE_SUCCESSFUL";
 case ParserExtensionResultType::DISPLAY_ORIGINAL_ERROR:
  return "DISPLAY_ORIGINAL_ERROR";
 case ParserExtensionResultType::DISPLAY_EXTENSION_ERROR:
  return "DISPLAY_EXTENSION_ERROR";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ParserExtensionResultType EnumUtil::FromString<ParserExtensionResultType>(const char *value) {
 if (StringUtil::Equals(value, "PARSE_SUCCESSFUL")) {
  return ParserExtensionResultType::PARSE_SUCCESSFUL;
 }
 if (StringUtil::Equals(value, "DISPLAY_ORIGINAL_ERROR")) {
  return ParserExtensionResultType::DISPLAY_ORIGINAL_ERROR;
 }
 if (StringUtil::Equals(value, "DISPLAY_EXTENSION_ERROR")) {
  return ParserExtensionResultType::DISPLAY_EXTENSION_ERROR;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ParserMode>(ParserMode value) {
 switch(value) {
 case ParserMode::PARSING:
  return "PARSING";
 case ParserMode::SNIFFING_DATATYPES:
  return "SNIFFING_DATATYPES";
 case ParserMode::PARSING_HEADER:
  return "PARSING_HEADER";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ParserMode EnumUtil::FromString<ParserMode>(const char *value) {
 if (StringUtil::Equals(value, "PARSING")) {
  return ParserMode::PARSING;
 }
 if (StringUtil::Equals(value, "SNIFFING_DATATYPES")) {
  return ParserMode::SNIFFING_DATATYPES;
 }
 if (StringUtil::Equals(value, "PARSING_HEADER")) {
  return ParserMode::PARSING_HEADER;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<PartitionSortStage>(PartitionSortStage value) {
 switch(value) {
 case PartitionSortStage::INIT:
  return "INIT";
 case PartitionSortStage::SCAN:
  return "SCAN";
 case PartitionSortStage::PREPARE:
  return "PREPARE";
 case PartitionSortStage::MERGE:
  return "MERGE";
 case PartitionSortStage::SORTED:
  return "SORTED";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
PartitionSortStage EnumUtil::FromString<PartitionSortStage>(const char *value) {
 if (StringUtil::Equals(value, "INIT")) {
  return PartitionSortStage::INIT;
 }
 if (StringUtil::Equals(value, "SCAN")) {
  return PartitionSortStage::SCAN;
 }
 if (StringUtil::Equals(value, "PREPARE")) {
  return PartitionSortStage::PREPARE;
 }
 if (StringUtil::Equals(value, "MERGE")) {
  return PartitionSortStage::MERGE;
 }
 if (StringUtil::Equals(value, "SORTED")) {
  return PartitionSortStage::SORTED;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<PartitionedColumnDataType>(PartitionedColumnDataType value) {
 switch(value) {
 case PartitionedColumnDataType::INVALID:
  return "INVALID";
 case PartitionedColumnDataType::RADIX:
  return "RADIX";
 case PartitionedColumnDataType::HIVE:
  return "HIVE";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
PartitionedColumnDataType EnumUtil::FromString<PartitionedColumnDataType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return PartitionedColumnDataType::INVALID;
 }
 if (StringUtil::Equals(value, "RADIX")) {
  return PartitionedColumnDataType::RADIX;
 }
 if (StringUtil::Equals(value, "HIVE")) {
  return PartitionedColumnDataType::HIVE;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<PartitionedTupleDataType>(PartitionedTupleDataType value) {
 switch(value) {
 case PartitionedTupleDataType::INVALID:
  return "INVALID";
 case PartitionedTupleDataType::RADIX:
  return "RADIX";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
PartitionedTupleDataType EnumUtil::FromString<PartitionedTupleDataType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return PartitionedTupleDataType::INVALID;
 }
 if (StringUtil::Equals(value, "RADIX")) {
  return PartitionedTupleDataType::RADIX;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<PendingExecutionResult>(PendingExecutionResult value) {
 switch(value) {
 case PendingExecutionResult::RESULT_READY:
  return "RESULT_READY";
 case PendingExecutionResult::RESULT_NOT_READY:
  return "RESULT_NOT_READY";
 case PendingExecutionResult::EXECUTION_ERROR:
  return "EXECUTION_ERROR";
 case PendingExecutionResult::NO_TASKS_AVAILABLE:
  return "NO_TASKS_AVAILABLE";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
PendingExecutionResult EnumUtil::FromString<PendingExecutionResult>(const char *value) {
 if (StringUtil::Equals(value, "RESULT_READY")) {
  return PendingExecutionResult::RESULT_READY;
 }
 if (StringUtil::Equals(value, "RESULT_NOT_READY")) {
  return PendingExecutionResult::RESULT_NOT_READY;
 }
 if (StringUtil::Equals(value, "EXECUTION_ERROR")) {
  return PendingExecutionResult::EXECUTION_ERROR;
 }
 if (StringUtil::Equals(value, "NO_TASKS_AVAILABLE")) {
  return PendingExecutionResult::NO_TASKS_AVAILABLE;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<PhysicalOperatorType>(PhysicalOperatorType value) {
 switch(value) {
 case PhysicalOperatorType::INVALID:
  return "INVALID";
 case PhysicalOperatorType::ORDER_BY:
  return "ORDER_BY";
 case PhysicalOperatorType::LIMIT:
  return "LIMIT";
 case PhysicalOperatorType::STREAMING_LIMIT:
  return "STREAMING_LIMIT";
 case PhysicalOperatorType::LIMIT_PERCENT:
  return "LIMIT_PERCENT";
 case PhysicalOperatorType::TOP_N:
  return "TOP_N";
 case PhysicalOperatorType::WINDOW:
  return "WINDOW";
 case PhysicalOperatorType::UNNEST:
  return "UNNEST";
 case PhysicalOperatorType::UNGROUPED_AGGREGATE:
  return "UNGROUPED_AGGREGATE";
 case PhysicalOperatorType::HASH_GROUP_BY:
  return "HASH_GROUP_BY";
 case PhysicalOperatorType::PERFECT_HASH_GROUP_BY:
  return "PERFECT_HASH_GROUP_BY";
 case PhysicalOperatorType::FILTER:
  return "FILTER";
 case PhysicalOperatorType::PROJECTION:
  return "PROJECTION";
 case PhysicalOperatorType::COPY_TO_FILE:
  return "COPY_TO_FILE";
 case PhysicalOperatorType::BATCH_COPY_TO_FILE:
  return "BATCH_COPY_TO_FILE";
 case PhysicalOperatorType::FIXED_BATCH_COPY_TO_FILE:
  return "FIXED_BATCH_COPY_TO_FILE";
 case PhysicalOperatorType::RESERVOIR_SAMPLE:
  return "RESERVOIR_SAMPLE";
 case PhysicalOperatorType::STREAMING_SAMPLE:
  return "STREAMING_SAMPLE";
 case PhysicalOperatorType::STREAMING_WINDOW:
  return "STREAMING_WINDOW";
 case PhysicalOperatorType::PIVOT:
  return "PIVOT";
 case PhysicalOperatorType::COPY_DATABASE:
  return "COPY_DATABASE";
 case PhysicalOperatorType::TABLE_SCAN:
  return "TABLE_SCAN";
 case PhysicalOperatorType::DUMMY_SCAN:
  return "DUMMY_SCAN";
 case PhysicalOperatorType::COLUMN_DATA_SCAN:
  return "COLUMN_DATA_SCAN";
 case PhysicalOperatorType::CHUNK_SCAN:
  return "CHUNK_SCAN";
 case PhysicalOperatorType::RECURSIVE_CTE_SCAN:
  return "RECURSIVE_CTE_SCAN";
 case PhysicalOperatorType::CTE_SCAN:
  return "CTE_SCAN";
 case PhysicalOperatorType::DELIM_SCAN:
  return "DELIM_SCAN";
 case PhysicalOperatorType::EXPRESSION_SCAN:
  return "EXPRESSION_SCAN";
 case PhysicalOperatorType::POSITIONAL_SCAN:
  return "POSITIONAL_SCAN";
 case PhysicalOperatorType::BLOCKWISE_NL_JOIN:
  return "BLOCKWISE_NL_JOIN";
 case PhysicalOperatorType::NESTED_LOOP_JOIN:
  return "NESTED_LOOP_JOIN";
 case PhysicalOperatorType::HASH_JOIN:
  return "HASH_JOIN";
 case PhysicalOperatorType::CROSS_PRODUCT:
  return "CROSS_PRODUCT";
 case PhysicalOperatorType::PIECEWISE_MERGE_JOIN:
  return "PIECEWISE_MERGE_JOIN";
 case PhysicalOperatorType::IE_JOIN:
  return "IE_JOIN";
 case PhysicalOperatorType::DELIM_JOIN:
  return "DELIM_JOIN";
 case PhysicalOperatorType::POSITIONAL_JOIN:
  return "POSITIONAL_JOIN";
 case PhysicalOperatorType::ASOF_JOIN:
  return "ASOF_JOIN";
 case PhysicalOperatorType::UNION:
  return "UNION";
 case PhysicalOperatorType::RECURSIVE_CTE:
  return "RECURSIVE_CTE";
 case PhysicalOperatorType::CTE:
  return "CTE";
 case PhysicalOperatorType::INSERT:
  return "INSERT";
 case PhysicalOperatorType::BATCH_INSERT:
  return "BATCH_INSERT";
 case PhysicalOperatorType::DELETE_OPERATOR:
  return "DELETE_OPERATOR";
 case PhysicalOperatorType::UPDATE:
  return "UPDATE";
 case PhysicalOperatorType::CREATE_TABLE:
  return "CREATE_TABLE";
 case PhysicalOperatorType::CREATE_TABLE_AS:
  return "CREATE_TABLE_AS";
 case PhysicalOperatorType::BATCH_CREATE_TABLE_AS:
  return "BATCH_CREATE_TABLE_AS";
 case PhysicalOperatorType::CREATE_INDEX:
  return "CREATE_INDEX";
 case PhysicalOperatorType::ALTER:
  return "ALTER";
 case PhysicalOperatorType::CREATE_SEQUENCE:
  return "CREATE_SEQUENCE";
 case PhysicalOperatorType::CREATE_VIEW:
  return "CREATE_VIEW";
 case PhysicalOperatorType::CREATE_SCHEMA:
  return "CREATE_SCHEMA";
 case PhysicalOperatorType::CREATE_MACRO:
  return "CREATE_MACRO";
 case PhysicalOperatorType::DROP:
  return "DROP";
 case PhysicalOperatorType::PRAGMA:
  return "PRAGMA";
 case PhysicalOperatorType::TRANSACTION:
  return "TRANSACTION";
 case PhysicalOperatorType::CREATE_TYPE:
  return "CREATE_TYPE";
 case PhysicalOperatorType::ATTACH:
  return "ATTACH";
 case PhysicalOperatorType::DETACH:
  return "DETACH";
 case PhysicalOperatorType::EXPLAIN:
  return "EXPLAIN";
 case PhysicalOperatorType::EXPLAIN_ANALYZE:
  return "EXPLAIN_ANALYZE";
 case PhysicalOperatorType::EMPTY_RESULT:
  return "EMPTY_RESULT";
 case PhysicalOperatorType::EXECUTE:
  return "EXECUTE";
 case PhysicalOperatorType::PREPARE:
  return "PREPARE";
 case PhysicalOperatorType::VACUUM:
  return "VACUUM";
 case PhysicalOperatorType::EXPORT:
  return "EXPORT";
 case PhysicalOperatorType::SET:
  return "SET";
 case PhysicalOperatorType::LOAD:
  return "LOAD";
 case PhysicalOperatorType::INOUT_FUNCTION:
  return "INOUT_FUNCTION";
 case PhysicalOperatorType::RESULT_COLLECTOR:
  return "RESULT_COLLECTOR";
 case PhysicalOperatorType::RESET:
  return "RESET";
 case PhysicalOperatorType::EXTENSION:
  return "EXTENSION";
 case PhysicalOperatorType::CREATE_SECRET:
  return "CREATE_SECRET";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
PhysicalOperatorType EnumUtil::FromString<PhysicalOperatorType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return PhysicalOperatorType::INVALID;
 }
 if (StringUtil::Equals(value, "ORDER_BY")) {
  return PhysicalOperatorType::ORDER_BY;
 }
 if (StringUtil::Equals(value, "LIMIT")) {
  return PhysicalOperatorType::LIMIT;
 }
 if (StringUtil::Equals(value, "STREAMING_LIMIT")) {
  return PhysicalOperatorType::STREAMING_LIMIT;
 }
 if (StringUtil::Equals(value, "LIMIT_PERCENT")) {
  return PhysicalOperatorType::LIMIT_PERCENT;
 }
 if (StringUtil::Equals(value, "TOP_N")) {
  return PhysicalOperatorType::TOP_N;
 }
 if (StringUtil::Equals(value, "WINDOW")) {
  return PhysicalOperatorType::WINDOW;
 }
 if (StringUtil::Equals(value, "UNNEST")) {
  return PhysicalOperatorType::UNNEST;
 }
 if (StringUtil::Equals(value, "UNGROUPED_AGGREGATE")) {
  return PhysicalOperatorType::UNGROUPED_AGGREGATE;
 }
 if (StringUtil::Equals(value, "HASH_GROUP_BY")) {
  return PhysicalOperatorType::HASH_GROUP_BY;
 }
 if (StringUtil::Equals(value, "PERFECT_HASH_GROUP_BY")) {
  return PhysicalOperatorType::PERFECT_HASH_GROUP_BY;
 }
 if (StringUtil::Equals(value, "FILTER")) {
  return PhysicalOperatorType::FILTER;
 }
 if (StringUtil::Equals(value, "PROJECTION")) {
  return PhysicalOperatorType::PROJECTION;
 }
 if (StringUtil::Equals(value, "COPY_TO_FILE")) {
  return PhysicalOperatorType::COPY_TO_FILE;
 }
 if (StringUtil::Equals(value, "BATCH_COPY_TO_FILE")) {
  return PhysicalOperatorType::BATCH_COPY_TO_FILE;
 }
 if (StringUtil::Equals(value, "FIXED_BATCH_COPY_TO_FILE")) {
  return PhysicalOperatorType::FIXED_BATCH_COPY_TO_FILE;
 }
 if (StringUtil::Equals(value, "RESERVOIR_SAMPLE")) {
  return PhysicalOperatorType::RESERVOIR_SAMPLE;
 }
 if (StringUtil::Equals(value, "STREAMING_SAMPLE")) {
  return PhysicalOperatorType::STREAMING_SAMPLE;
 }
 if (StringUtil::Equals(value, "STREAMING_WINDOW")) {
  return PhysicalOperatorType::STREAMING_WINDOW;
 }
 if (StringUtil::Equals(value, "PIVOT")) {
  return PhysicalOperatorType::PIVOT;
 }
 if (StringUtil::Equals(value, "COPY_DATABASE")) {
  return PhysicalOperatorType::COPY_DATABASE;
 }
 if (StringUtil::Equals(value, "TABLE_SCAN")) {
  return PhysicalOperatorType::TABLE_SCAN;
 }
 if (StringUtil::Equals(value, "DUMMY_SCAN")) {
  return PhysicalOperatorType::DUMMY_SCAN;
 }
 if (StringUtil::Equals(value, "COLUMN_DATA_SCAN")) {
  return PhysicalOperatorType::COLUMN_DATA_SCAN;
 }
 if (StringUtil::Equals(value, "CHUNK_SCAN")) {
  return PhysicalOperatorType::CHUNK_SCAN;
 }
 if (StringUtil::Equals(value, "RECURSIVE_CTE_SCAN")) {
  return PhysicalOperatorType::RECURSIVE_CTE_SCAN;
 }
 if (StringUtil::Equals(value, "CTE_SCAN")) {
  return PhysicalOperatorType::CTE_SCAN;
 }
 if (StringUtil::Equals(value, "DELIM_SCAN")) {
  return PhysicalOperatorType::DELIM_SCAN;
 }
 if (StringUtil::Equals(value, "EXPRESSION_SCAN")) {
  return PhysicalOperatorType::EXPRESSION_SCAN;
 }
 if (StringUtil::Equals(value, "POSITIONAL_SCAN")) {
  return PhysicalOperatorType::POSITIONAL_SCAN;
 }
 if (StringUtil::Equals(value, "BLOCKWISE_NL_JOIN")) {
  return PhysicalOperatorType::BLOCKWISE_NL_JOIN;
 }
 if (StringUtil::Equals(value, "NESTED_LOOP_JOIN")) {
  return PhysicalOperatorType::NESTED_LOOP_JOIN;
 }
 if (StringUtil::Equals(value, "HASH_JOIN")) {
  return PhysicalOperatorType::HASH_JOIN;
 }
 if (StringUtil::Equals(value, "CROSS_PRODUCT")) {
  return PhysicalOperatorType::CROSS_PRODUCT;
 }
 if (StringUtil::Equals(value, "PIECEWISE_MERGE_JOIN")) {
  return PhysicalOperatorType::PIECEWISE_MERGE_JOIN;
 }
 if (StringUtil::Equals(value, "IE_JOIN")) {
  return PhysicalOperatorType::IE_JOIN;
 }
 if (StringUtil::Equals(value, "DELIM_JOIN")) {
  return PhysicalOperatorType::DELIM_JOIN;
 }
 if (StringUtil::Equals(value, "POSITIONAL_JOIN")) {
  return PhysicalOperatorType::POSITIONAL_JOIN;
 }
 if (StringUtil::Equals(value, "ASOF_JOIN")) {
  return PhysicalOperatorType::ASOF_JOIN;
 }
 if (StringUtil::Equals(value, "UNION")) {
  return PhysicalOperatorType::UNION;
 }
 if (StringUtil::Equals(value, "RECURSIVE_CTE")) {
  return PhysicalOperatorType::RECURSIVE_CTE;
 }
 if (StringUtil::Equals(value, "CTE")) {
  return PhysicalOperatorType::CTE;
 }
 if (StringUtil::Equals(value, "INSERT")) {
  return PhysicalOperatorType::INSERT;
 }
 if (StringUtil::Equals(value, "BATCH_INSERT")) {
  return PhysicalOperatorType::BATCH_INSERT;
 }
 if (StringUtil::Equals(value, "DELETE_OPERATOR")) {
  return PhysicalOperatorType::DELETE_OPERATOR;
 }
 if (StringUtil::Equals(value, "UPDATE")) {
  return PhysicalOperatorType::UPDATE;
 }
 if (StringUtil::Equals(value, "CREATE_TABLE")) {
  return PhysicalOperatorType::CREATE_TABLE;
 }
 if (StringUtil::Equals(value, "CREATE_TABLE_AS")) {
  return PhysicalOperatorType::CREATE_TABLE_AS;
 }
 if (StringUtil::Equals(value, "BATCH_CREATE_TABLE_AS")) {
  return PhysicalOperatorType::BATCH_CREATE_TABLE_AS;
 }
 if (StringUtil::Equals(value, "CREATE_INDEX")) {
  return PhysicalOperatorType::CREATE_INDEX;
 }
 if (StringUtil::Equals(value, "ALTER")) {
  return PhysicalOperatorType::ALTER;
 }
 if (StringUtil::Equals(value, "CREATE_SEQUENCE")) {
  return PhysicalOperatorType::CREATE_SEQUENCE;
 }
 if (StringUtil::Equals(value, "CREATE_VIEW")) {
  return PhysicalOperatorType::CREATE_VIEW;
 }
 if (StringUtil::Equals(value, "CREATE_SCHEMA")) {
  return PhysicalOperatorType::CREATE_SCHEMA;
 }
 if (StringUtil::Equals(value, "CREATE_MACRO")) {
  return PhysicalOperatorType::CREATE_MACRO;
 }
 if (StringUtil::Equals(value, "DROP")) {
  return PhysicalOperatorType::DROP;
 }
 if (StringUtil::Equals(value, "PRAGMA")) {
  return PhysicalOperatorType::PRAGMA;
 }
 if (StringUtil::Equals(value, "TRANSACTION")) {
  return PhysicalOperatorType::TRANSACTION;
 }
 if (StringUtil::Equals(value, "CREATE_TYPE")) {
  return PhysicalOperatorType::CREATE_TYPE;
 }
 if (StringUtil::Equals(value, "ATTACH")) {
  return PhysicalOperatorType::ATTACH;
 }
 if (StringUtil::Equals(value, "DETACH")) {
  return PhysicalOperatorType::DETACH;
 }
 if (StringUtil::Equals(value, "EXPLAIN")) {
  return PhysicalOperatorType::EXPLAIN;
 }
 if (StringUtil::Equals(value, "EXPLAIN_ANALYZE")) {
  return PhysicalOperatorType::EXPLAIN_ANALYZE;
 }
 if (StringUtil::Equals(value, "EMPTY_RESULT")) {
  return PhysicalOperatorType::EMPTY_RESULT;
 }
 if (StringUtil::Equals(value, "EXECUTE")) {
  return PhysicalOperatorType::EXECUTE;
 }
 if (StringUtil::Equals(value, "PREPARE")) {
  return PhysicalOperatorType::PREPARE;
 }
 if (StringUtil::Equals(value, "VACUUM")) {
  return PhysicalOperatorType::VACUUM;
 }
 if (StringUtil::Equals(value, "EXPORT")) {
  return PhysicalOperatorType::EXPORT;
 }
 if (StringUtil::Equals(value, "SET")) {
  return PhysicalOperatorType::SET;
 }
 if (StringUtil::Equals(value, "LOAD")) {
  return PhysicalOperatorType::LOAD;
 }
 if (StringUtil::Equals(value, "INOUT_FUNCTION")) {
  return PhysicalOperatorType::INOUT_FUNCTION;
 }
 if (StringUtil::Equals(value, "RESULT_COLLECTOR")) {
  return PhysicalOperatorType::RESULT_COLLECTOR;
 }
 if (StringUtil::Equals(value, "RESET")) {
  return PhysicalOperatorType::RESET;
 }
 if (StringUtil::Equals(value, "EXTENSION")) {
  return PhysicalOperatorType::EXTENSION;
 }
 if (StringUtil::Equals(value, "CREATE_SECRET")) {
  return PhysicalOperatorType::CREATE_SECRET;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<PhysicalType>(PhysicalType value) {
 switch(value) {
 case PhysicalType::BOOL:
  return "BOOL";
 case PhysicalType::UINT8:
  return "UINT8";
 case PhysicalType::INT8:
  return "INT8";
 case PhysicalType::UINT16:
  return "UINT16";
 case PhysicalType::INT16:
  return "INT16";
 case PhysicalType::UINT32:
  return "UINT32";
 case PhysicalType::INT32:
  return "INT32";
 case PhysicalType::UINT64:
  return "UINT64";
 case PhysicalType::INT64:
  return "INT64";
 case PhysicalType::FLOAT:
  return "FLOAT";
 case PhysicalType::DOUBLE:
  return "DOUBLE";
 case PhysicalType::INTERVAL:
  return "INTERVAL";
 case PhysicalType::LIST:
  return "LIST";
 case PhysicalType::STRUCT:
  return "STRUCT";
 case PhysicalType::ARRAY:
  return "ARRAY";
 case PhysicalType::VARCHAR:
  return "VARCHAR";
 case PhysicalType::INT128:
  return "INT128";
 case PhysicalType::UNKNOWN:
  return "UNKNOWN";
 case PhysicalType::BIT:
  return "BIT";
 case PhysicalType::INVALID:
  return "INVALID";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
PhysicalType EnumUtil::FromString<PhysicalType>(const char *value) {
 if (StringUtil::Equals(value, "BOOL")) {
  return PhysicalType::BOOL;
 }
 if (StringUtil::Equals(value, "UINT8")) {
  return PhysicalType::UINT8;
 }
 if (StringUtil::Equals(value, "INT8")) {
  return PhysicalType::INT8;
 }
 if (StringUtil::Equals(value, "UINT16")) {
  return PhysicalType::UINT16;
 }
 if (StringUtil::Equals(value, "INT16")) {
  return PhysicalType::INT16;
 }
 if (StringUtil::Equals(value, "UINT32")) {
  return PhysicalType::UINT32;
 }
 if (StringUtil::Equals(value, "INT32")) {
  return PhysicalType::INT32;
 }
 if (StringUtil::Equals(value, "UINT64")) {
  return PhysicalType::UINT64;
 }
 if (StringUtil::Equals(value, "INT64")) {
  return PhysicalType::INT64;
 }
 if (StringUtil::Equals(value, "FLOAT")) {
  return PhysicalType::FLOAT;
 }
 if (StringUtil::Equals(value, "DOUBLE")) {
  return PhysicalType::DOUBLE;
 }
 if (StringUtil::Equals(value, "INTERVAL")) {
  return PhysicalType::INTERVAL;
 }
 if (StringUtil::Equals(value, "LIST")) {
  return PhysicalType::LIST;
 }
 if (StringUtil::Equals(value, "STRUCT")) {
  return PhysicalType::STRUCT;
 }
 if (StringUtil::Equals(value, "ARRAY")) {
  return PhysicalType::ARRAY;
 }
 if (StringUtil::Equals(value, "VARCHAR")) {
  return PhysicalType::VARCHAR;
 }
 if (StringUtil::Equals(value, "INT128")) {
  return PhysicalType::INT128;
 }
 if (StringUtil::Equals(value, "UNKNOWN")) {
  return PhysicalType::UNKNOWN;
 }
 if (StringUtil::Equals(value, "BIT")) {
  return PhysicalType::BIT;
 }
 if (StringUtil::Equals(value, "INVALID")) {
  return PhysicalType::INVALID;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<PragmaType>(PragmaType value) {
 switch(value) {
 case PragmaType::PRAGMA_STATEMENT:
  return "PRAGMA_STATEMENT";
 case PragmaType::PRAGMA_CALL:
  return "PRAGMA_CALL";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
PragmaType EnumUtil::FromString<PragmaType>(const char *value) {
 if (StringUtil::Equals(value, "PRAGMA_STATEMENT")) {
  return PragmaType::PRAGMA_STATEMENT;
 }
 if (StringUtil::Equals(value, "PRAGMA_CALL")) {
  return PragmaType::PRAGMA_CALL;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<PreparedParamType>(PreparedParamType value) {
 switch(value) {
 case PreparedParamType::AUTO_INCREMENT:
  return "AUTO_INCREMENT";
 case PreparedParamType::POSITIONAL:
  return "POSITIONAL";
 case PreparedParamType::NAMED:
  return "NAMED";
 case PreparedParamType::INVALID:
  return "INVALID";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
PreparedParamType EnumUtil::FromString<PreparedParamType>(const char *value) {
 if (StringUtil::Equals(value, "AUTO_INCREMENT")) {
  return PreparedParamType::AUTO_INCREMENT;
 }
 if (StringUtil::Equals(value, "POSITIONAL")) {
  return PreparedParamType::POSITIONAL;
 }
 if (StringUtil::Equals(value, "NAMED")) {
  return PreparedParamType::NAMED;
 }
 if (StringUtil::Equals(value, "INVALID")) {
  return PreparedParamType::INVALID;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ProfilerPrintFormat>(ProfilerPrintFormat value) {
 switch(value) {
 case ProfilerPrintFormat::QUERY_TREE:
  return "QUERY_TREE";
 case ProfilerPrintFormat::JSON:
  return "JSON";
 case ProfilerPrintFormat::QUERY_TREE_OPTIMIZER:
  return "QUERY_TREE_OPTIMIZER";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ProfilerPrintFormat EnumUtil::FromString<ProfilerPrintFormat>(const char *value) {
 if (StringUtil::Equals(value, "QUERY_TREE")) {
  return ProfilerPrintFormat::QUERY_TREE;
 }
 if (StringUtil::Equals(value, "JSON")) {
  return ProfilerPrintFormat::JSON;
 }
 if (StringUtil::Equals(value, "QUERY_TREE_OPTIMIZER")) {
  return ProfilerPrintFormat::QUERY_TREE_OPTIMIZER;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<QuantileSerializationType>(QuantileSerializationType value) {
 switch(value) {
 case QuantileSerializationType::NON_DECIMAL:
  return "NON_DECIMAL";
 case QuantileSerializationType::DECIMAL_DISCRETE:
  return "DECIMAL_DISCRETE";
 case QuantileSerializationType::DECIMAL_DISCRETE_LIST:
  return "DECIMAL_DISCRETE_LIST";
 case QuantileSerializationType::DECIMAL_CONTINUOUS:
  return "DECIMAL_CONTINUOUS";
 case QuantileSerializationType::DECIMAL_CONTINUOUS_LIST:
  return "DECIMAL_CONTINUOUS_LIST";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
QuantileSerializationType EnumUtil::FromString<QuantileSerializationType>(const char *value) {
 if (StringUtil::Equals(value, "NON_DECIMAL")) {
  return QuantileSerializationType::NON_DECIMAL;
 }
 if (StringUtil::Equals(value, "DECIMAL_DISCRETE")) {
  return QuantileSerializationType::DECIMAL_DISCRETE;
 }
 if (StringUtil::Equals(value, "DECIMAL_DISCRETE_LIST")) {
  return QuantileSerializationType::DECIMAL_DISCRETE_LIST;
 }
 if (StringUtil::Equals(value, "DECIMAL_CONTINUOUS")) {
  return QuantileSerializationType::DECIMAL_CONTINUOUS;
 }
 if (StringUtil::Equals(value, "DECIMAL_CONTINUOUS_LIST")) {
  return QuantileSerializationType::DECIMAL_CONTINUOUS_LIST;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<QueryNodeType>(QueryNodeType value) {
 switch(value) {
 case QueryNodeType::SELECT_NODE:
  return "SELECT_NODE";
 case QueryNodeType::SET_OPERATION_NODE:
  return "SET_OPERATION_NODE";
 case QueryNodeType::BOUND_SUBQUERY_NODE:
  return "BOUND_SUBQUERY_NODE";
 case QueryNodeType::RECURSIVE_CTE_NODE:
  return "RECURSIVE_CTE_NODE";
 case QueryNodeType::CTE_NODE:
  return "CTE_NODE";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
QueryNodeType EnumUtil::FromString<QueryNodeType>(const char *value) {
 if (StringUtil::Equals(value, "SELECT_NODE")) {
  return QueryNodeType::SELECT_NODE;
 }
 if (StringUtil::Equals(value, "SET_OPERATION_NODE")) {
  return QueryNodeType::SET_OPERATION_NODE;
 }
 if (StringUtil::Equals(value, "BOUND_SUBQUERY_NODE")) {
  return QueryNodeType::BOUND_SUBQUERY_NODE;
 }
 if (StringUtil::Equals(value, "RECURSIVE_CTE_NODE")) {
  return QueryNodeType::RECURSIVE_CTE_NODE;
 }
 if (StringUtil::Equals(value, "CTE_NODE")) {
  return QueryNodeType::CTE_NODE;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<QueryResultType>(QueryResultType value) {
 switch(value) {
 case QueryResultType::MATERIALIZED_RESULT:
  return "MATERIALIZED_RESULT";
 case QueryResultType::STREAM_RESULT:
  return "STREAM_RESULT";
 case QueryResultType::PENDING_RESULT:
  return "PENDING_RESULT";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
QueryResultType EnumUtil::FromString<QueryResultType>(const char *value) {
 if (StringUtil::Equals(value, "MATERIALIZED_RESULT")) {
  return QueryResultType::MATERIALIZED_RESULT;
 }
 if (StringUtil::Equals(value, "STREAM_RESULT")) {
  return QueryResultType::STREAM_RESULT;
 }
 if (StringUtil::Equals(value, "PENDING_RESULT")) {
  return QueryResultType::PENDING_RESULT;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<QuoteRule>(QuoteRule value) {
 switch(value) {
 case QuoteRule::QUOTES_RFC:
  return "QUOTES_RFC";
 case QuoteRule::QUOTES_OTHER:
  return "QUOTES_OTHER";
 case QuoteRule::NO_QUOTES:
  return "NO_QUOTES";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
QuoteRule EnumUtil::FromString<QuoteRule>(const char *value) {
 if (StringUtil::Equals(value, "QUOTES_RFC")) {
  return QuoteRule::QUOTES_RFC;
 }
 if (StringUtil::Equals(value, "QUOTES_OTHER")) {
  return QuoteRule::QUOTES_OTHER;
 }
 if (StringUtil::Equals(value, "NO_QUOTES")) {
  return QuoteRule::NO_QUOTES;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<RelationType>(RelationType value) {
 switch(value) {
 case RelationType::INVALID_RELATION:
  return "INVALID_RELATION";
 case RelationType::TABLE_RELATION:
  return "TABLE_RELATION";
 case RelationType::PROJECTION_RELATION:
  return "PROJECTION_RELATION";
 case RelationType::FILTER_RELATION:
  return "FILTER_RELATION";
 case RelationType::EXPLAIN_RELATION:
  return "EXPLAIN_RELATION";
 case RelationType::CROSS_PRODUCT_RELATION:
  return "CROSS_PRODUCT_RELATION";
 case RelationType::JOIN_RELATION:
  return "JOIN_RELATION";
 case RelationType::AGGREGATE_RELATION:
  return "AGGREGATE_RELATION";
 case RelationType::SET_OPERATION_RELATION:
  return "SET_OPERATION_RELATION";
 case RelationType::DISTINCT_RELATION:
  return "DISTINCT_RELATION";
 case RelationType::LIMIT_RELATION:
  return "LIMIT_RELATION";
 case RelationType::ORDER_RELATION:
  return "ORDER_RELATION";
 case RelationType::CREATE_VIEW_RELATION:
  return "CREATE_VIEW_RELATION";
 case RelationType::CREATE_TABLE_RELATION:
  return "CREATE_TABLE_RELATION";
 case RelationType::INSERT_RELATION:
  return "INSERT_RELATION";
 case RelationType::VALUE_LIST_RELATION:
  return "VALUE_LIST_RELATION";
 case RelationType::DELETE_RELATION:
  return "DELETE_RELATION";
 case RelationType::UPDATE_RELATION:
  return "UPDATE_RELATION";
 case RelationType::WRITE_CSV_RELATION:
  return "WRITE_CSV_RELATION";
 case RelationType::WRITE_PARQUET_RELATION:
  return "WRITE_PARQUET_RELATION";
 case RelationType::READ_CSV_RELATION:
  return "READ_CSV_RELATION";
 case RelationType::SUBQUERY_RELATION:
  return "SUBQUERY_RELATION";
 case RelationType::TABLE_FUNCTION_RELATION:
  return "TABLE_FUNCTION_RELATION";
 case RelationType::VIEW_RELATION:
  return "VIEW_RELATION";
 case RelationType::QUERY_RELATION:
  return "QUERY_RELATION";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
RelationType EnumUtil::FromString<RelationType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID_RELATION")) {
  return RelationType::INVALID_RELATION;
 }
 if (StringUtil::Equals(value, "TABLE_RELATION")) {
  return RelationType::TABLE_RELATION;
 }
 if (StringUtil::Equals(value, "PROJECTION_RELATION")) {
  return RelationType::PROJECTION_RELATION;
 }
 if (StringUtil::Equals(value, "FILTER_RELATION")) {
  return RelationType::FILTER_RELATION;
 }
 if (StringUtil::Equals(value, "EXPLAIN_RELATION")) {
  return RelationType::EXPLAIN_RELATION;
 }
 if (StringUtil::Equals(value, "CROSS_PRODUCT_RELATION")) {
  return RelationType::CROSS_PRODUCT_RELATION;
 }
 if (StringUtil::Equals(value, "JOIN_RELATION")) {
  return RelationType::JOIN_RELATION;
 }
 if (StringUtil::Equals(value, "AGGREGATE_RELATION")) {
  return RelationType::AGGREGATE_RELATION;
 }
 if (StringUtil::Equals(value, "SET_OPERATION_RELATION")) {
  return RelationType::SET_OPERATION_RELATION;
 }
 if (StringUtil::Equals(value, "DISTINCT_RELATION")) {
  return RelationType::DISTINCT_RELATION;
 }
 if (StringUtil::Equals(value, "LIMIT_RELATION")) {
  return RelationType::LIMIT_RELATION;
 }
 if (StringUtil::Equals(value, "ORDER_RELATION")) {
  return RelationType::ORDER_RELATION;
 }
 if (StringUtil::Equals(value, "CREATE_VIEW_RELATION")) {
  return RelationType::CREATE_VIEW_RELATION;
 }
 if (StringUtil::Equals(value, "CREATE_TABLE_RELATION")) {
  return RelationType::CREATE_TABLE_RELATION;
 }
 if (StringUtil::Equals(value, "INSERT_RELATION")) {
  return RelationType::INSERT_RELATION;
 }
 if (StringUtil::Equals(value, "VALUE_LIST_RELATION")) {
  return RelationType::VALUE_LIST_RELATION;
 }
 if (StringUtil::Equals(value, "DELETE_RELATION")) {
  return RelationType::DELETE_RELATION;
 }
 if (StringUtil::Equals(value, "UPDATE_RELATION")) {
  return RelationType::UPDATE_RELATION;
 }
 if (StringUtil::Equals(value, "WRITE_CSV_RELATION")) {
  return RelationType::WRITE_CSV_RELATION;
 }
 if (StringUtil::Equals(value, "WRITE_PARQUET_RELATION")) {
  return RelationType::WRITE_PARQUET_RELATION;
 }
 if (StringUtil::Equals(value, "READ_CSV_RELATION")) {
  return RelationType::READ_CSV_RELATION;
 }
 if (StringUtil::Equals(value, "SUBQUERY_RELATION")) {
  return RelationType::SUBQUERY_RELATION;
 }
 if (StringUtil::Equals(value, "TABLE_FUNCTION_RELATION")) {
  return RelationType::TABLE_FUNCTION_RELATION;
 }
 if (StringUtil::Equals(value, "VIEW_RELATION")) {
  return RelationType::VIEW_RELATION;
 }
 if (StringUtil::Equals(value, "QUERY_RELATION")) {
  return RelationType::QUERY_RELATION;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<RenderMode>(RenderMode value) {
 switch(value) {
 case RenderMode::ROWS:
  return "ROWS";
 case RenderMode::COLUMNS:
  return "COLUMNS";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
RenderMode EnumUtil::FromString<RenderMode>(const char *value) {
 if (StringUtil::Equals(value, "ROWS")) {
  return RenderMode::ROWS;
 }
 if (StringUtil::Equals(value, "COLUMNS")) {
  return RenderMode::COLUMNS;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<ResultModifierType>(ResultModifierType value) {
 switch(value) {
 case ResultModifierType::LIMIT_MODIFIER:
  return "LIMIT_MODIFIER";
 case ResultModifierType::ORDER_MODIFIER:
  return "ORDER_MODIFIER";
 case ResultModifierType::DISTINCT_MODIFIER:
  return "DISTINCT_MODIFIER";
 case ResultModifierType::LIMIT_PERCENT_MODIFIER:
  return "LIMIT_PERCENT_MODIFIER";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
ResultModifierType EnumUtil::FromString<ResultModifierType>(const char *value) {
 if (StringUtil::Equals(value, "LIMIT_MODIFIER")) {
  return ResultModifierType::LIMIT_MODIFIER;
 }
 if (StringUtil::Equals(value, "ORDER_MODIFIER")) {
  return ResultModifierType::ORDER_MODIFIER;
 }
 if (StringUtil::Equals(value, "DISTINCT_MODIFIER")) {
  return ResultModifierType::DISTINCT_MODIFIER;
 }
 if (StringUtil::Equals(value, "LIMIT_PERCENT_MODIFIER")) {
  return ResultModifierType::LIMIT_PERCENT_MODIFIER;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<SampleMethod>(SampleMethod value) {
 switch(value) {
 case SampleMethod::SYSTEM_SAMPLE:
  return "System";
 case SampleMethod::BERNOULLI_SAMPLE:
  return "Bernoulli";
 case SampleMethod::RESERVOIR_SAMPLE:
  return "Reservoir";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
SampleMethod EnumUtil::FromString<SampleMethod>(const char *value) {
 if (StringUtil::Equals(value, "System")) {
  return SampleMethod::SYSTEM_SAMPLE;
 }
 if (StringUtil::Equals(value, "Bernoulli")) {
  return SampleMethod::BERNOULLI_SAMPLE;
 }
 if (StringUtil::Equals(value, "Reservoir")) {
  return SampleMethod::RESERVOIR_SAMPLE;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<SecretDisplayType>(SecretDisplayType value) {
 switch(value) {
 case SecretDisplayType::REDACTED:
  return "REDACTED";
 case SecretDisplayType::UNREDACTED:
  return "UNREDACTED";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
SecretDisplayType EnumUtil::FromString<SecretDisplayType>(const char *value) {
 if (StringUtil::Equals(value, "REDACTED")) {
  return SecretDisplayType::REDACTED;
 }
 if (StringUtil::Equals(value, "UNREDACTED")) {
  return SecretDisplayType::UNREDACTED;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<SecretPersistMode>(SecretPersistMode value) {
 switch(value) {
 case SecretPersistMode::DEFAULT:
  return "DEFAULT";
 case SecretPersistMode::TEMPORARY:
  return "TEMPORARY";
 case SecretPersistMode::PERSISTENT:
  return "PERSISTENT";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
SecretPersistMode EnumUtil::FromString<SecretPersistMode>(const char *value) {
 if (StringUtil::Equals(value, "DEFAULT")) {
  return SecretPersistMode::DEFAULT;
 }
 if (StringUtil::Equals(value, "TEMPORARY")) {
  return SecretPersistMode::TEMPORARY;
 }
 if (StringUtil::Equals(value, "PERSISTENT")) {
  return SecretPersistMode::PERSISTENT;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<SequenceInfo>(SequenceInfo value) {
 switch(value) {
 case SequenceInfo::SEQ_START:
  return "SEQ_START";
 case SequenceInfo::SEQ_INC:
  return "SEQ_INC";
 case SequenceInfo::SEQ_MIN:
  return "SEQ_MIN";
 case SequenceInfo::SEQ_MAX:
  return "SEQ_MAX";
 case SequenceInfo::SEQ_CYCLE:
  return "SEQ_CYCLE";
 case SequenceInfo::SEQ_OWN:
  return "SEQ_OWN";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
SequenceInfo EnumUtil::FromString<SequenceInfo>(const char *value) {
 if (StringUtil::Equals(value, "SEQ_START")) {
  return SequenceInfo::SEQ_START;
 }
 if (StringUtil::Equals(value, "SEQ_INC")) {
  return SequenceInfo::SEQ_INC;
 }
 if (StringUtil::Equals(value, "SEQ_MIN")) {
  return SequenceInfo::SEQ_MIN;
 }
 if (StringUtil::Equals(value, "SEQ_MAX")) {
  return SequenceInfo::SEQ_MAX;
 }
 if (StringUtil::Equals(value, "SEQ_CYCLE")) {
  return SequenceInfo::SEQ_CYCLE;
 }
 if (StringUtil::Equals(value, "SEQ_OWN")) {
  return SequenceInfo::SEQ_OWN;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<SetOperationType>(SetOperationType value) {
 switch(value) {
 case SetOperationType::NONE:
  return "NONE";
 case SetOperationType::UNION:
  return "UNION";
 case SetOperationType::EXCEPT:
  return "EXCEPT";
 case SetOperationType::INTERSECT:
  return "INTERSECT";
 case SetOperationType::UNION_BY_NAME:
  return "UNION_BY_NAME";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
SetOperationType EnumUtil::FromString<SetOperationType>(const char *value) {
 if (StringUtil::Equals(value, "NONE")) {
  return SetOperationType::NONE;
 }
 if (StringUtil::Equals(value, "UNION")) {
  return SetOperationType::UNION;
 }
 if (StringUtil::Equals(value, "EXCEPT")) {
  return SetOperationType::EXCEPT;
 }
 if (StringUtil::Equals(value, "INTERSECT")) {
  return SetOperationType::INTERSECT;
 }
 if (StringUtil::Equals(value, "UNION_BY_NAME")) {
  return SetOperationType::UNION_BY_NAME;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<SetScope>(SetScope value) {
 switch(value) {
 case SetScope::AUTOMATIC:
  return "AUTOMATIC";
 case SetScope::LOCAL:
  return "LOCAL";
 case SetScope::SESSION:
  return "SESSION";
 case SetScope::GLOBAL:
  return "GLOBAL";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
SetScope EnumUtil::FromString<SetScope>(const char *value) {
 if (StringUtil::Equals(value, "AUTOMATIC")) {
  return SetScope::AUTOMATIC;
 }
 if (StringUtil::Equals(value, "LOCAL")) {
  return SetScope::LOCAL;
 }
 if (StringUtil::Equals(value, "SESSION")) {
  return SetScope::SESSION;
 }
 if (StringUtil::Equals(value, "GLOBAL")) {
  return SetScope::GLOBAL;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<SetType>(SetType value) {
 switch(value) {
 case SetType::SET:
  return "SET";
 case SetType::RESET:
  return "RESET";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
SetType EnumUtil::FromString<SetType>(const char *value) {
 if (StringUtil::Equals(value, "SET")) {
  return SetType::SET;
 }
 if (StringUtil::Equals(value, "RESET")) {
  return SetType::RESET;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<SimplifiedTokenType>(SimplifiedTokenType value) {
 switch(value) {
 case SimplifiedTokenType::SIMPLIFIED_TOKEN_IDENTIFIER:
  return "SIMPLIFIED_TOKEN_IDENTIFIER";
 case SimplifiedTokenType::SIMPLIFIED_TOKEN_NUMERIC_CONSTANT:
  return "SIMPLIFIED_TOKEN_NUMERIC_CONSTANT";
 case SimplifiedTokenType::SIMPLIFIED_TOKEN_STRING_CONSTANT:
  return "SIMPLIFIED_TOKEN_STRING_CONSTANT";
 case SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR:
  return "SIMPLIFIED_TOKEN_OPERATOR";
 case SimplifiedTokenType::SIMPLIFIED_TOKEN_KEYWORD:
  return "SIMPLIFIED_TOKEN_KEYWORD";
 case SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT:
  return "SIMPLIFIED_TOKEN_COMMENT";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
SimplifiedTokenType EnumUtil::FromString<SimplifiedTokenType>(const char *value) {
 if (StringUtil::Equals(value, "SIMPLIFIED_TOKEN_IDENTIFIER")) {
  return SimplifiedTokenType::SIMPLIFIED_TOKEN_IDENTIFIER;
 }
 if (StringUtil::Equals(value, "SIMPLIFIED_TOKEN_NUMERIC_CONSTANT")) {
  return SimplifiedTokenType::SIMPLIFIED_TOKEN_NUMERIC_CONSTANT;
 }
 if (StringUtil::Equals(value, "SIMPLIFIED_TOKEN_STRING_CONSTANT")) {
  return SimplifiedTokenType::SIMPLIFIED_TOKEN_STRING_CONSTANT;
 }
 if (StringUtil::Equals(value, "SIMPLIFIED_TOKEN_OPERATOR")) {
  return SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR;
 }
 if (StringUtil::Equals(value, "SIMPLIFIED_TOKEN_KEYWORD")) {
  return SimplifiedTokenType::SIMPLIFIED_TOKEN_KEYWORD;
 }
 if (StringUtil::Equals(value, "SIMPLIFIED_TOKEN_COMMENT")) {
  return SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<SinkCombineResultType>(SinkCombineResultType value) {
 switch(value) {
 case SinkCombineResultType::FINISHED:
  return "FINISHED";
 case SinkCombineResultType::BLOCKED:
  return "BLOCKED";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
SinkCombineResultType EnumUtil::FromString<SinkCombineResultType>(const char *value) {
 if (StringUtil::Equals(value, "FINISHED")) {
  return SinkCombineResultType::FINISHED;
 }
 if (StringUtil::Equals(value, "BLOCKED")) {
  return SinkCombineResultType::BLOCKED;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<SinkFinalizeType>(SinkFinalizeType value) {
 switch(value) {
 case SinkFinalizeType::READY:
  return "READY";
 case SinkFinalizeType::NO_OUTPUT_POSSIBLE:
  return "NO_OUTPUT_POSSIBLE";
 case SinkFinalizeType::BLOCKED:
  return "BLOCKED";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
SinkFinalizeType EnumUtil::FromString<SinkFinalizeType>(const char *value) {
 if (StringUtil::Equals(value, "READY")) {
  return SinkFinalizeType::READY;
 }
 if (StringUtil::Equals(value, "NO_OUTPUT_POSSIBLE")) {
  return SinkFinalizeType::NO_OUTPUT_POSSIBLE;
 }
 if (StringUtil::Equals(value, "BLOCKED")) {
  return SinkFinalizeType::BLOCKED;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<SinkNextBatchType>(SinkNextBatchType value) {
 switch(value) {
 case SinkNextBatchType::READY:
  return "READY";
 case SinkNextBatchType::BLOCKED:
  return "BLOCKED";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
SinkNextBatchType EnumUtil::FromString<SinkNextBatchType>(const char *value) {
 if (StringUtil::Equals(value, "READY")) {
  return SinkNextBatchType::READY;
 }
 if (StringUtil::Equals(value, "BLOCKED")) {
  return SinkNextBatchType::BLOCKED;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<SinkResultType>(SinkResultType value) {
 switch(value) {
 case SinkResultType::NEED_MORE_INPUT:
  return "NEED_MORE_INPUT";
 case SinkResultType::FINISHED:
  return "FINISHED";
 case SinkResultType::BLOCKED:
  return "BLOCKED";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
SinkResultType EnumUtil::FromString<SinkResultType>(const char *value) {
 if (StringUtil::Equals(value, "NEED_MORE_INPUT")) {
  return SinkResultType::NEED_MORE_INPUT;
 }
 if (StringUtil::Equals(value, "FINISHED")) {
  return SinkResultType::FINISHED;
 }
 if (StringUtil::Equals(value, "BLOCKED")) {
  return SinkResultType::BLOCKED;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<SourceResultType>(SourceResultType value) {
 switch(value) {
 case SourceResultType::HAVE_MORE_OUTPUT:
  return "HAVE_MORE_OUTPUT";
 case SourceResultType::FINISHED:
  return "FINISHED";
 case SourceResultType::BLOCKED:
  return "BLOCKED";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
SourceResultType EnumUtil::FromString<SourceResultType>(const char *value) {
 if (StringUtil::Equals(value, "HAVE_MORE_OUTPUT")) {
  return SourceResultType::HAVE_MORE_OUTPUT;
 }
 if (StringUtil::Equals(value, "FINISHED")) {
  return SourceResultType::FINISHED;
 }
 if (StringUtil::Equals(value, "BLOCKED")) {
  return SourceResultType::BLOCKED;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<StatementReturnType>(StatementReturnType value) {
 switch(value) {
 case StatementReturnType::QUERY_RESULT:
  return "QUERY_RESULT";
 case StatementReturnType::CHANGED_ROWS:
  return "CHANGED_ROWS";
 case StatementReturnType::NOTHING:
  return "NOTHING";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
StatementReturnType EnumUtil::FromString<StatementReturnType>(const char *value) {
 if (StringUtil::Equals(value, "QUERY_RESULT")) {
  return StatementReturnType::QUERY_RESULT;
 }
 if (StringUtil::Equals(value, "CHANGED_ROWS")) {
  return StatementReturnType::CHANGED_ROWS;
 }
 if (StringUtil::Equals(value, "NOTHING")) {
  return StatementReturnType::NOTHING;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<StatementType>(StatementType value) {
 switch(value) {
 case StatementType::INVALID_STATEMENT:
  return "INVALID_STATEMENT";
 case StatementType::SELECT_STATEMENT:
  return "SELECT_STATEMENT";
 case StatementType::INSERT_STATEMENT:
  return "INSERT_STATEMENT";
 case StatementType::UPDATE_STATEMENT:
  return "UPDATE_STATEMENT";
 case StatementType::CREATE_STATEMENT:
  return "CREATE_STATEMENT";
 case StatementType::DELETE_STATEMENT:
  return "DELETE_STATEMENT";
 case StatementType::PREPARE_STATEMENT:
  return "PREPARE_STATEMENT";
 case StatementType::EXECUTE_STATEMENT:
  return "EXECUTE_STATEMENT";
 case StatementType::ALTER_STATEMENT:
  return "ALTER_STATEMENT";
 case StatementType::TRANSACTION_STATEMENT:
  return "TRANSACTION_STATEMENT";
 case StatementType::COPY_STATEMENT:
  return "COPY_STATEMENT";
 case StatementType::ANALYZE_STATEMENT:
  return "ANALYZE_STATEMENT";
 case StatementType::VARIABLE_SET_STATEMENT:
  return "VARIABLE_SET_STATEMENT";
 case StatementType::CREATE_FUNC_STATEMENT:
  return "CREATE_FUNC_STATEMENT";
 case StatementType::EXPLAIN_STATEMENT:
  return "EXPLAIN_STATEMENT";
 case StatementType::DROP_STATEMENT:
  return "DROP_STATEMENT";
 case StatementType::EXPORT_STATEMENT:
  return "EXPORT_STATEMENT";
 case StatementType::PRAGMA_STATEMENT:
  return "PRAGMA_STATEMENT";
 case StatementType::SHOW_STATEMENT:
  return "SHOW_STATEMENT";
 case StatementType::VACUUM_STATEMENT:
  return "VACUUM_STATEMENT";
 case StatementType::CALL_STATEMENT:
  return "CALL_STATEMENT";
 case StatementType::SET_STATEMENT:
  return "SET_STATEMENT";
 case StatementType::LOAD_STATEMENT:
  return "LOAD_STATEMENT";
 case StatementType::RELATION_STATEMENT:
  return "RELATION_STATEMENT";
 case StatementType::EXTENSION_STATEMENT:
  return "EXTENSION_STATEMENT";
 case StatementType::LOGICAL_PLAN_STATEMENT:
  return "LOGICAL_PLAN_STATEMENT";
 case StatementType::ATTACH_STATEMENT:
  return "ATTACH_STATEMENT";
 case StatementType::DETACH_STATEMENT:
  return "DETACH_STATEMENT";
 case StatementType::MULTI_STATEMENT:
  return "MULTI_STATEMENT";
 case StatementType::COPY_DATABASE_STATEMENT:
  return "COPY_DATABASE_STATEMENT";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
StatementType EnumUtil::FromString<StatementType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID_STATEMENT")) {
  return StatementType::INVALID_STATEMENT;
 }
 if (StringUtil::Equals(value, "SELECT_STATEMENT")) {
  return StatementType::SELECT_STATEMENT;
 }
 if (StringUtil::Equals(value, "INSERT_STATEMENT")) {
  return StatementType::INSERT_STATEMENT;
 }
 if (StringUtil::Equals(value, "UPDATE_STATEMENT")) {
  return StatementType::UPDATE_STATEMENT;
 }
 if (StringUtil::Equals(value, "CREATE_STATEMENT")) {
  return StatementType::CREATE_STATEMENT;
 }
 if (StringUtil::Equals(value, "DELETE_STATEMENT")) {
  return StatementType::DELETE_STATEMENT;
 }
 if (StringUtil::Equals(value, "PREPARE_STATEMENT")) {
  return StatementType::PREPARE_STATEMENT;
 }
 if (StringUtil::Equals(value, "EXECUTE_STATEMENT")) {
  return StatementType::EXECUTE_STATEMENT;
 }
 if (StringUtil::Equals(value, "ALTER_STATEMENT")) {
  return StatementType::ALTER_STATEMENT;
 }
 if (StringUtil::Equals(value, "TRANSACTION_STATEMENT")) {
  return StatementType::TRANSACTION_STATEMENT;
 }
 if (StringUtil::Equals(value, "COPY_STATEMENT")) {
  return StatementType::COPY_STATEMENT;
 }
 if (StringUtil::Equals(value, "ANALYZE_STATEMENT")) {
  return StatementType::ANALYZE_STATEMENT;
 }
 if (StringUtil::Equals(value, "VARIABLE_SET_STATEMENT")) {
  return StatementType::VARIABLE_SET_STATEMENT;
 }
 if (StringUtil::Equals(value, "CREATE_FUNC_STATEMENT")) {
  return StatementType::CREATE_FUNC_STATEMENT;
 }
 if (StringUtil::Equals(value, "EXPLAIN_STATEMENT")) {
  return StatementType::EXPLAIN_STATEMENT;
 }
 if (StringUtil::Equals(value, "DROP_STATEMENT")) {
  return StatementType::DROP_STATEMENT;
 }
 if (StringUtil::Equals(value, "EXPORT_STATEMENT")) {
  return StatementType::EXPORT_STATEMENT;
 }
 if (StringUtil::Equals(value, "PRAGMA_STATEMENT")) {
  return StatementType::PRAGMA_STATEMENT;
 }
 if (StringUtil::Equals(value, "SHOW_STATEMENT")) {
  return StatementType::SHOW_STATEMENT;
 }
 if (StringUtil::Equals(value, "VACUUM_STATEMENT")) {
  return StatementType::VACUUM_STATEMENT;
 }
 if (StringUtil::Equals(value, "CALL_STATEMENT")) {
  return StatementType::CALL_STATEMENT;
 }
 if (StringUtil::Equals(value, "SET_STATEMENT")) {
  return StatementType::SET_STATEMENT;
 }
 if (StringUtil::Equals(value, "LOAD_STATEMENT")) {
  return StatementType::LOAD_STATEMENT;
 }
 if (StringUtil::Equals(value, "RELATION_STATEMENT")) {
  return StatementType::RELATION_STATEMENT;
 }
 if (StringUtil::Equals(value, "EXTENSION_STATEMENT")) {
  return StatementType::EXTENSION_STATEMENT;
 }
 if (StringUtil::Equals(value, "LOGICAL_PLAN_STATEMENT")) {
  return StatementType::LOGICAL_PLAN_STATEMENT;
 }
 if (StringUtil::Equals(value, "ATTACH_STATEMENT")) {
  return StatementType::ATTACH_STATEMENT;
 }
 if (StringUtil::Equals(value, "DETACH_STATEMENT")) {
  return StatementType::DETACH_STATEMENT;
 }
 if (StringUtil::Equals(value, "MULTI_STATEMENT")) {
  return StatementType::MULTI_STATEMENT;
 }
 if (StringUtil::Equals(value, "COPY_DATABASE_STATEMENT")) {
  return StatementType::COPY_DATABASE_STATEMENT;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<StatisticsType>(StatisticsType value) {
 switch(value) {
 case StatisticsType::NUMERIC_STATS:
  return "NUMERIC_STATS";
 case StatisticsType::STRING_STATS:
  return "STRING_STATS";
 case StatisticsType::LIST_STATS:
  return "LIST_STATS";
 case StatisticsType::STRUCT_STATS:
  return "STRUCT_STATS";
 case StatisticsType::BASE_STATS:
  return "BASE_STATS";
 case StatisticsType::ARRAY_STATS:
  return "ARRAY_STATS";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
StatisticsType EnumUtil::FromString<StatisticsType>(const char *value) {
 if (StringUtil::Equals(value, "NUMERIC_STATS")) {
  return StatisticsType::NUMERIC_STATS;
 }
 if (StringUtil::Equals(value, "STRING_STATS")) {
  return StatisticsType::STRING_STATS;
 }
 if (StringUtil::Equals(value, "LIST_STATS")) {
  return StatisticsType::LIST_STATS;
 }
 if (StringUtil::Equals(value, "STRUCT_STATS")) {
  return StatisticsType::STRUCT_STATS;
 }
 if (StringUtil::Equals(value, "BASE_STATS")) {
  return StatisticsType::BASE_STATS;
 }
 if (StringUtil::Equals(value, "ARRAY_STATS")) {
  return StatisticsType::ARRAY_STATS;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<StatsInfo>(StatsInfo value) {
 switch(value) {
 case StatsInfo::CAN_HAVE_NULL_VALUES:
  return "CAN_HAVE_NULL_VALUES";
 case StatsInfo::CANNOT_HAVE_NULL_VALUES:
  return "CANNOT_HAVE_NULL_VALUES";
 case StatsInfo::CAN_HAVE_VALID_VALUES:
  return "CAN_HAVE_VALID_VALUES";
 case StatsInfo::CANNOT_HAVE_VALID_VALUES:
  return "CANNOT_HAVE_VALID_VALUES";
 case StatsInfo::CAN_HAVE_NULL_AND_VALID_VALUES:
  return "CAN_HAVE_NULL_AND_VALID_VALUES";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
StatsInfo EnumUtil::FromString<StatsInfo>(const char *value) {
 if (StringUtil::Equals(value, "CAN_HAVE_NULL_VALUES")) {
  return StatsInfo::CAN_HAVE_NULL_VALUES;
 }
 if (StringUtil::Equals(value, "CANNOT_HAVE_NULL_VALUES")) {
  return StatsInfo::CANNOT_HAVE_NULL_VALUES;
 }
 if (StringUtil::Equals(value, "CAN_HAVE_VALID_VALUES")) {
  return StatsInfo::CAN_HAVE_VALID_VALUES;
 }
 if (StringUtil::Equals(value, "CANNOT_HAVE_VALID_VALUES")) {
  return StatsInfo::CANNOT_HAVE_VALID_VALUES;
 }
 if (StringUtil::Equals(value, "CAN_HAVE_NULL_AND_VALID_VALUES")) {
  return StatsInfo::CAN_HAVE_NULL_AND_VALID_VALUES;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<StrTimeSpecifier>(StrTimeSpecifier value) {
 switch(value) {
 case StrTimeSpecifier::ABBREVIATED_WEEKDAY_NAME:
  return "ABBREVIATED_WEEKDAY_NAME";
 case StrTimeSpecifier::FULL_WEEKDAY_NAME:
  return "FULL_WEEKDAY_NAME";
 case StrTimeSpecifier::WEEKDAY_DECIMAL:
  return "WEEKDAY_DECIMAL";
 case StrTimeSpecifier::DAY_OF_MONTH_PADDED:
  return "DAY_OF_MONTH_PADDED";
 case StrTimeSpecifier::DAY_OF_MONTH:
  return "DAY_OF_MONTH";
 case StrTimeSpecifier::ABBREVIATED_MONTH_NAME:
  return "ABBREVIATED_MONTH_NAME";
 case StrTimeSpecifier::FULL_MONTH_NAME:
  return "FULL_MONTH_NAME";
 case StrTimeSpecifier::MONTH_DECIMAL_PADDED:
  return "MONTH_DECIMAL_PADDED";
 case StrTimeSpecifier::MONTH_DECIMAL:
  return "MONTH_DECIMAL";
 case StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED:
  return "YEAR_WITHOUT_CENTURY_PADDED";
 case StrTimeSpecifier::YEAR_WITHOUT_CENTURY:
  return "YEAR_WITHOUT_CENTURY";
 case StrTimeSpecifier::YEAR_DECIMAL:
  return "YEAR_DECIMAL";
 case StrTimeSpecifier::HOUR_24_PADDED:
  return "HOUR_24_PADDED";
 case StrTimeSpecifier::HOUR_24_DECIMAL:
  return "HOUR_24_DECIMAL";
 case StrTimeSpecifier::HOUR_12_PADDED:
  return "HOUR_12_PADDED";
 case StrTimeSpecifier::HOUR_12_DECIMAL:
  return "HOUR_12_DECIMAL";
 case StrTimeSpecifier::AM_PM:
  return "AM_PM";
 case StrTimeSpecifier::MINUTE_PADDED:
  return "MINUTE_PADDED";
 case StrTimeSpecifier::MINUTE_DECIMAL:
  return "MINUTE_DECIMAL";
 case StrTimeSpecifier::SECOND_PADDED:
  return "SECOND_PADDED";
 case StrTimeSpecifier::SECOND_DECIMAL:
  return "SECOND_DECIMAL";
 case StrTimeSpecifier::MICROSECOND_PADDED:
  return "MICROSECOND_PADDED";
 case StrTimeSpecifier::MILLISECOND_PADDED:
  return "MILLISECOND_PADDED";
 case StrTimeSpecifier::UTC_OFFSET:
  return "UTC_OFFSET";
 case StrTimeSpecifier::TZ_NAME:
  return "TZ_NAME";
 case StrTimeSpecifier::DAY_OF_YEAR_PADDED:
  return "DAY_OF_YEAR_PADDED";
 case StrTimeSpecifier::DAY_OF_YEAR_DECIMAL:
  return "DAY_OF_YEAR_DECIMAL";
 case StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST:
  return "WEEK_NUMBER_PADDED_SUN_FIRST";
 case StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST:
  return "WEEK_NUMBER_PADDED_MON_FIRST";
 case StrTimeSpecifier::LOCALE_APPROPRIATE_DATE_AND_TIME:
  return "LOCALE_APPROPRIATE_DATE_AND_TIME";
 case StrTimeSpecifier::LOCALE_APPROPRIATE_DATE:
  return "LOCALE_APPROPRIATE_DATE";
 case StrTimeSpecifier::LOCALE_APPROPRIATE_TIME:
  return "LOCALE_APPROPRIATE_TIME";
 case StrTimeSpecifier::NANOSECOND_PADDED:
  return "NANOSECOND_PADDED";
 case StrTimeSpecifier::YEAR_ISO:
  return "YEAR_ISO";
 case StrTimeSpecifier::WEEKDAY_ISO:
  return "WEEKDAY_ISO";
 case StrTimeSpecifier::WEEK_NUMBER_ISO:
  return "WEEK_NUMBER_ISO";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
StrTimeSpecifier EnumUtil::FromString<StrTimeSpecifier>(const char *value) {
 if (StringUtil::Equals(value, "ABBREVIATED_WEEKDAY_NAME")) {
  return StrTimeSpecifier::ABBREVIATED_WEEKDAY_NAME;
 }
 if (StringUtil::Equals(value, "FULL_WEEKDAY_NAME")) {
  return StrTimeSpecifier::FULL_WEEKDAY_NAME;
 }
 if (StringUtil::Equals(value, "WEEKDAY_DECIMAL")) {
  return StrTimeSpecifier::WEEKDAY_DECIMAL;
 }
 if (StringUtil::Equals(value, "DAY_OF_MONTH_PADDED")) {
  return StrTimeSpecifier::DAY_OF_MONTH_PADDED;
 }
 if (StringUtil::Equals(value, "DAY_OF_MONTH")) {
  return StrTimeSpecifier::DAY_OF_MONTH;
 }
 if (StringUtil::Equals(value, "ABBREVIATED_MONTH_NAME")) {
  return StrTimeSpecifier::ABBREVIATED_MONTH_NAME;
 }
 if (StringUtil::Equals(value, "FULL_MONTH_NAME")) {
  return StrTimeSpecifier::FULL_MONTH_NAME;
 }
 if (StringUtil::Equals(value, "MONTH_DECIMAL_PADDED")) {
  return StrTimeSpecifier::MONTH_DECIMAL_PADDED;
 }
 if (StringUtil::Equals(value, "MONTH_DECIMAL")) {
  return StrTimeSpecifier::MONTH_DECIMAL;
 }
 if (StringUtil::Equals(value, "YEAR_WITHOUT_CENTURY_PADDED")) {
  return StrTimeSpecifier::YEAR_WITHOUT_CENTURY_PADDED;
 }
 if (StringUtil::Equals(value, "YEAR_WITHOUT_CENTURY")) {
  return StrTimeSpecifier::YEAR_WITHOUT_CENTURY;
 }
 if (StringUtil::Equals(value, "YEAR_DECIMAL")) {
  return StrTimeSpecifier::YEAR_DECIMAL;
 }
 if (StringUtil::Equals(value, "HOUR_24_PADDED")) {
  return StrTimeSpecifier::HOUR_24_PADDED;
 }
 if (StringUtil::Equals(value, "HOUR_24_DECIMAL")) {
  return StrTimeSpecifier::HOUR_24_DECIMAL;
 }
 if (StringUtil::Equals(value, "HOUR_12_PADDED")) {
  return StrTimeSpecifier::HOUR_12_PADDED;
 }
 if (StringUtil::Equals(value, "HOUR_12_DECIMAL")) {
  return StrTimeSpecifier::HOUR_12_DECIMAL;
 }
 if (StringUtil::Equals(value, "AM_PM")) {
  return StrTimeSpecifier::AM_PM;
 }
 if (StringUtil::Equals(value, "MINUTE_PADDED")) {
  return StrTimeSpecifier::MINUTE_PADDED;
 }
 if (StringUtil::Equals(value, "MINUTE_DECIMAL")) {
  return StrTimeSpecifier::MINUTE_DECIMAL;
 }
 if (StringUtil::Equals(value, "SECOND_PADDED")) {
  return StrTimeSpecifier::SECOND_PADDED;
 }
 if (StringUtil::Equals(value, "SECOND_DECIMAL")) {
  return StrTimeSpecifier::SECOND_DECIMAL;
 }
 if (StringUtil::Equals(value, "MICROSECOND_PADDED")) {
  return StrTimeSpecifier::MICROSECOND_PADDED;
 }
 if (StringUtil::Equals(value, "MILLISECOND_PADDED")) {
  return StrTimeSpecifier::MILLISECOND_PADDED;
 }
 if (StringUtil::Equals(value, "UTC_OFFSET")) {
  return StrTimeSpecifier::UTC_OFFSET;
 }
 if (StringUtil::Equals(value, "TZ_NAME")) {
  return StrTimeSpecifier::TZ_NAME;
 }
 if (StringUtil::Equals(value, "DAY_OF_YEAR_PADDED")) {
  return StrTimeSpecifier::DAY_OF_YEAR_PADDED;
 }
 if (StringUtil::Equals(value, "DAY_OF_YEAR_DECIMAL")) {
  return StrTimeSpecifier::DAY_OF_YEAR_DECIMAL;
 }
 if (StringUtil::Equals(value, "WEEK_NUMBER_PADDED_SUN_FIRST")) {
  return StrTimeSpecifier::WEEK_NUMBER_PADDED_SUN_FIRST;
 }
 if (StringUtil::Equals(value, "WEEK_NUMBER_PADDED_MON_FIRST")) {
  return StrTimeSpecifier::WEEK_NUMBER_PADDED_MON_FIRST;
 }
 if (StringUtil::Equals(value, "LOCALE_APPROPRIATE_DATE_AND_TIME")) {
  return StrTimeSpecifier::LOCALE_APPROPRIATE_DATE_AND_TIME;
 }
 if (StringUtil::Equals(value, "LOCALE_APPROPRIATE_DATE")) {
  return StrTimeSpecifier::LOCALE_APPROPRIATE_DATE;
 }
 if (StringUtil::Equals(value, "LOCALE_APPROPRIATE_TIME")) {
  return StrTimeSpecifier::LOCALE_APPROPRIATE_TIME;
 }
 if (StringUtil::Equals(value, "NANOSECOND_PADDED")) {
  return StrTimeSpecifier::NANOSECOND_PADDED;
 }
 if (StringUtil::Equals(value, "YEAR_ISO")) {
  return StrTimeSpecifier::YEAR_ISO;
 }
 if (StringUtil::Equals(value, "WEEKDAY_ISO")) {
  return StrTimeSpecifier::WEEKDAY_ISO;
 }
 if (StringUtil::Equals(value, "WEEK_NUMBER_ISO")) {
  return StrTimeSpecifier::WEEK_NUMBER_ISO;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<SubqueryType>(SubqueryType value) {
 switch(value) {
 case SubqueryType::INVALID:
  return "INVALID";
 case SubqueryType::SCALAR:
  return "SCALAR";
 case SubqueryType::EXISTS:
  return "EXISTS";
 case SubqueryType::NOT_EXISTS:
  return "NOT_EXISTS";
 case SubqueryType::ANY:
  return "ANY";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
SubqueryType EnumUtil::FromString<SubqueryType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return SubqueryType::INVALID;
 }
 if (StringUtil::Equals(value, "SCALAR")) {
  return SubqueryType::SCALAR;
 }
 if (StringUtil::Equals(value, "EXISTS")) {
  return SubqueryType::EXISTS;
 }
 if (StringUtil::Equals(value, "NOT_EXISTS")) {
  return SubqueryType::NOT_EXISTS;
 }
 if (StringUtil::Equals(value, "ANY")) {
  return SubqueryType::ANY;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<TableColumnType>(TableColumnType value) {
 switch(value) {
 case TableColumnType::STANDARD:
  return "STANDARD";
 case TableColumnType::GENERATED:
  return "GENERATED";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
TableColumnType EnumUtil::FromString<TableColumnType>(const char *value) {
 if (StringUtil::Equals(value, "STANDARD")) {
  return TableColumnType::STANDARD;
 }
 if (StringUtil::Equals(value, "GENERATED")) {
  return TableColumnType::GENERATED;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<TableFilterType>(TableFilterType value) {
 switch(value) {
 case TableFilterType::CONSTANT_COMPARISON:
  return "CONSTANT_COMPARISON";
 case TableFilterType::IS_NULL:
  return "IS_NULL";
 case TableFilterType::IS_NOT_NULL:
  return "IS_NOT_NULL";
 case TableFilterType::CONJUNCTION_OR:
  return "CONJUNCTION_OR";
 case TableFilterType::CONJUNCTION_AND:
  return "CONJUNCTION_AND";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
TableFilterType EnumUtil::FromString<TableFilterType>(const char *value) {
 if (StringUtil::Equals(value, "CONSTANT_COMPARISON")) {
  return TableFilterType::CONSTANT_COMPARISON;
 }
 if (StringUtil::Equals(value, "IS_NULL")) {
  return TableFilterType::IS_NULL;
 }
 if (StringUtil::Equals(value, "IS_NOT_NULL")) {
  return TableFilterType::IS_NOT_NULL;
 }
 if (StringUtil::Equals(value, "CONJUNCTION_OR")) {
  return TableFilterType::CONJUNCTION_OR;
 }
 if (StringUtil::Equals(value, "CONJUNCTION_AND")) {
  return TableFilterType::CONJUNCTION_AND;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<TableReferenceType>(TableReferenceType value) {
 switch(value) {
 case TableReferenceType::INVALID:
  return "INVALID";
 case TableReferenceType::BASE_TABLE:
  return "BASE_TABLE";
 case TableReferenceType::SUBQUERY:
  return "SUBQUERY";
 case TableReferenceType::JOIN:
  return "JOIN";
 case TableReferenceType::TABLE_FUNCTION:
  return "TABLE_FUNCTION";
 case TableReferenceType::EXPRESSION_LIST:
  return "EXPRESSION_LIST";
 case TableReferenceType::CTE:
  return "CTE";
 case TableReferenceType::EMPTY:
  return "EMPTY";
 case TableReferenceType::PIVOT:
  return "PIVOT";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
TableReferenceType EnumUtil::FromString<TableReferenceType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return TableReferenceType::INVALID;
 }
 if (StringUtil::Equals(value, "BASE_TABLE")) {
  return TableReferenceType::BASE_TABLE;
 }
 if (StringUtil::Equals(value, "SUBQUERY")) {
  return TableReferenceType::SUBQUERY;
 }
 if (StringUtil::Equals(value, "JOIN")) {
  return TableReferenceType::JOIN;
 }
 if (StringUtil::Equals(value, "TABLE_FUNCTION")) {
  return TableReferenceType::TABLE_FUNCTION;
 }
 if (StringUtil::Equals(value, "EXPRESSION_LIST")) {
  return TableReferenceType::EXPRESSION_LIST;
 }
 if (StringUtil::Equals(value, "CTE")) {
  return TableReferenceType::CTE;
 }
 if (StringUtil::Equals(value, "EMPTY")) {
  return TableReferenceType::EMPTY;
 }
 if (StringUtil::Equals(value, "PIVOT")) {
  return TableReferenceType::PIVOT;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<TableScanType>(TableScanType value) {
 switch(value) {
 case TableScanType::TABLE_SCAN_REGULAR:
  return "TABLE_SCAN_REGULAR";
 case TableScanType::TABLE_SCAN_COMMITTED_ROWS:
  return "TABLE_SCAN_COMMITTED_ROWS";
 case TableScanType::TABLE_SCAN_COMMITTED_ROWS_DISALLOW_UPDATES:
  return "TABLE_SCAN_COMMITTED_ROWS_DISALLOW_UPDATES";
 case TableScanType::TABLE_SCAN_COMMITTED_ROWS_OMIT_PERMANENTLY_DELETED:
  return "TABLE_SCAN_COMMITTED_ROWS_OMIT_PERMANENTLY_DELETED";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
TableScanType EnumUtil::FromString<TableScanType>(const char *value) {
 if (StringUtil::Equals(value, "TABLE_SCAN_REGULAR")) {
  return TableScanType::TABLE_SCAN_REGULAR;
 }
 if (StringUtil::Equals(value, "TABLE_SCAN_COMMITTED_ROWS")) {
  return TableScanType::TABLE_SCAN_COMMITTED_ROWS;
 }
 if (StringUtil::Equals(value, "TABLE_SCAN_COMMITTED_ROWS_DISALLOW_UPDATES")) {
  return TableScanType::TABLE_SCAN_COMMITTED_ROWS_DISALLOW_UPDATES;
 }
 if (StringUtil::Equals(value, "TABLE_SCAN_COMMITTED_ROWS_OMIT_PERMANENTLY_DELETED")) {
  return TableScanType::TABLE_SCAN_COMMITTED_ROWS_OMIT_PERMANENTLY_DELETED;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<TaskExecutionMode>(TaskExecutionMode value) {
 switch(value) {
 case TaskExecutionMode::PROCESS_ALL:
  return "PROCESS_ALL";
 case TaskExecutionMode::PROCESS_PARTIAL:
  return "PROCESS_PARTIAL";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
TaskExecutionMode EnumUtil::FromString<TaskExecutionMode>(const char *value) {
 if (StringUtil::Equals(value, "PROCESS_ALL")) {
  return TaskExecutionMode::PROCESS_ALL;
 }
 if (StringUtil::Equals(value, "PROCESS_PARTIAL")) {
  return TaskExecutionMode::PROCESS_PARTIAL;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<TaskExecutionResult>(TaskExecutionResult value) {
 switch(value) {
 case TaskExecutionResult::TASK_FINISHED:
  return "TASK_FINISHED";
 case TaskExecutionResult::TASK_NOT_FINISHED:
  return "TASK_NOT_FINISHED";
 case TaskExecutionResult::TASK_ERROR:
  return "TASK_ERROR";
 case TaskExecutionResult::TASK_BLOCKED:
  return "TASK_BLOCKED";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
TaskExecutionResult EnumUtil::FromString<TaskExecutionResult>(const char *value) {
 if (StringUtil::Equals(value, "TASK_FINISHED")) {
  return TaskExecutionResult::TASK_FINISHED;
 }
 if (StringUtil::Equals(value, "TASK_NOT_FINISHED")) {
  return TaskExecutionResult::TASK_NOT_FINISHED;
 }
 if (StringUtil::Equals(value, "TASK_ERROR")) {
  return TaskExecutionResult::TASK_ERROR;
 }
 if (StringUtil::Equals(value, "TASK_BLOCKED")) {
  return TaskExecutionResult::TASK_BLOCKED;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<TimestampCastResult>(TimestampCastResult value) {
 switch(value) {
 case TimestampCastResult::SUCCESS:
  return "SUCCESS";
 case TimestampCastResult::ERROR_INCORRECT_FORMAT:
  return "ERROR_INCORRECT_FORMAT";
 case TimestampCastResult::ERROR_NON_UTC_TIMEZONE:
  return "ERROR_NON_UTC_TIMEZONE";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
TimestampCastResult EnumUtil::FromString<TimestampCastResult>(const char *value) {
 if (StringUtil::Equals(value, "SUCCESS")) {
  return TimestampCastResult::SUCCESS;
 }
 if (StringUtil::Equals(value, "ERROR_INCORRECT_FORMAT")) {
  return TimestampCastResult::ERROR_INCORRECT_FORMAT;
 }
 if (StringUtil::Equals(value, "ERROR_NON_UTC_TIMEZONE")) {
  return TimestampCastResult::ERROR_NON_UTC_TIMEZONE;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<TransactionType>(TransactionType value) {
 switch(value) {
 case TransactionType::INVALID:
  return "INVALID";
 case TransactionType::BEGIN_TRANSACTION:
  return "BEGIN_TRANSACTION";
 case TransactionType::COMMIT:
  return "COMMIT";
 case TransactionType::ROLLBACK:
  return "ROLLBACK";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
TransactionType EnumUtil::FromString<TransactionType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return TransactionType::INVALID;
 }
 if (StringUtil::Equals(value, "BEGIN_TRANSACTION")) {
  return TransactionType::BEGIN_TRANSACTION;
 }
 if (StringUtil::Equals(value, "COMMIT")) {
  return TransactionType::COMMIT;
 }
 if (StringUtil::Equals(value, "ROLLBACK")) {
  return TransactionType::ROLLBACK;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<TupleDataPinProperties>(TupleDataPinProperties value) {
 switch(value) {
 case TupleDataPinProperties::INVALID:
  return "INVALID";
 case TupleDataPinProperties::KEEP_EVERYTHING_PINNED:
  return "KEEP_EVERYTHING_PINNED";
 case TupleDataPinProperties::UNPIN_AFTER_DONE:
  return "UNPIN_AFTER_DONE";
 case TupleDataPinProperties::DESTROY_AFTER_DONE:
  return "DESTROY_AFTER_DONE";
 case TupleDataPinProperties::ALREADY_PINNED:
  return "ALREADY_PINNED";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
TupleDataPinProperties EnumUtil::FromString<TupleDataPinProperties>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return TupleDataPinProperties::INVALID;
 }
 if (StringUtil::Equals(value, "KEEP_EVERYTHING_PINNED")) {
  return TupleDataPinProperties::KEEP_EVERYTHING_PINNED;
 }
 if (StringUtil::Equals(value, "UNPIN_AFTER_DONE")) {
  return TupleDataPinProperties::UNPIN_AFTER_DONE;
 }
 if (StringUtil::Equals(value, "DESTROY_AFTER_DONE")) {
  return TupleDataPinProperties::DESTROY_AFTER_DONE;
 }
 if (StringUtil::Equals(value, "ALREADY_PINNED")) {
  return TupleDataPinProperties::ALREADY_PINNED;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<UndoFlags>(UndoFlags value) {
 switch(value) {
 case UndoFlags::EMPTY_ENTRY:
  return "EMPTY_ENTRY";
 case UndoFlags::CATALOG_ENTRY:
  return "CATALOG_ENTRY";
 case UndoFlags::INSERT_TUPLE:
  return "INSERT_TUPLE";
 case UndoFlags::DELETE_TUPLE:
  return "DELETE_TUPLE";
 case UndoFlags::UPDATE_TUPLE:
  return "UPDATE_TUPLE";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
UndoFlags EnumUtil::FromString<UndoFlags>(const char *value) {
 if (StringUtil::Equals(value, "EMPTY_ENTRY")) {
  return UndoFlags::EMPTY_ENTRY;
 }
 if (StringUtil::Equals(value, "CATALOG_ENTRY")) {
  return UndoFlags::CATALOG_ENTRY;
 }
 if (StringUtil::Equals(value, "INSERT_TUPLE")) {
  return UndoFlags::INSERT_TUPLE;
 }
 if (StringUtil::Equals(value, "DELETE_TUPLE")) {
  return UndoFlags::DELETE_TUPLE;
 }
 if (StringUtil::Equals(value, "UPDATE_TUPLE")) {
  return UndoFlags::UPDATE_TUPLE;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<UnionInvalidReason>(UnionInvalidReason value) {
 switch(value) {
 case UnionInvalidReason::VALID:
  return "VALID";
 case UnionInvalidReason::TAG_OUT_OF_RANGE:
  return "TAG_OUT_OF_RANGE";
 case UnionInvalidReason::NO_MEMBERS:
  return "NO_MEMBERS";
 case UnionInvalidReason::VALIDITY_OVERLAP:
  return "VALIDITY_OVERLAP";
 case UnionInvalidReason::TAG_MISMATCH:
  return "TAG_MISMATCH";
 case UnionInvalidReason::NULL_TAG:
  return "NULL_TAG";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
UnionInvalidReason EnumUtil::FromString<UnionInvalidReason>(const char *value) {
 if (StringUtil::Equals(value, "VALID")) {
  return UnionInvalidReason::VALID;
 }
 if (StringUtil::Equals(value, "TAG_OUT_OF_RANGE")) {
  return UnionInvalidReason::TAG_OUT_OF_RANGE;
 }
 if (StringUtil::Equals(value, "NO_MEMBERS")) {
  return UnionInvalidReason::NO_MEMBERS;
 }
 if (StringUtil::Equals(value, "VALIDITY_OVERLAP")) {
  return UnionInvalidReason::VALIDITY_OVERLAP;
 }
 if (StringUtil::Equals(value, "TAG_MISMATCH")) {
  return UnionInvalidReason::TAG_MISMATCH;
 }
 if (StringUtil::Equals(value, "NULL_TAG")) {
  return UnionInvalidReason::NULL_TAG;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<VectorAuxiliaryDataType>(VectorAuxiliaryDataType value) {
 switch(value) {
 case VectorAuxiliaryDataType::ARROW_AUXILIARY:
  return "ARROW_AUXILIARY";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
VectorAuxiliaryDataType EnumUtil::FromString<VectorAuxiliaryDataType>(const char *value) {
 if (StringUtil::Equals(value, "ARROW_AUXILIARY")) {
  return VectorAuxiliaryDataType::ARROW_AUXILIARY;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<VectorBufferType>(VectorBufferType value) {
 switch(value) {
 case VectorBufferType::STANDARD_BUFFER:
  return "STANDARD_BUFFER";
 case VectorBufferType::DICTIONARY_BUFFER:
  return "DICTIONARY_BUFFER";
 case VectorBufferType::VECTOR_CHILD_BUFFER:
  return "VECTOR_CHILD_BUFFER";
 case VectorBufferType::STRING_BUFFER:
  return "STRING_BUFFER";
 case VectorBufferType::FSST_BUFFER:
  return "FSST_BUFFER";
 case VectorBufferType::STRUCT_BUFFER:
  return "STRUCT_BUFFER";
 case VectorBufferType::LIST_BUFFER:
  return "LIST_BUFFER";
 case VectorBufferType::MANAGED_BUFFER:
  return "MANAGED_BUFFER";
 case VectorBufferType::OPAQUE_BUFFER:
  return "OPAQUE_BUFFER";
 case VectorBufferType::ARRAY_BUFFER:
  return "ARRAY_BUFFER";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
VectorBufferType EnumUtil::FromString<VectorBufferType>(const char *value) {
 if (StringUtil::Equals(value, "STANDARD_BUFFER")) {
  return VectorBufferType::STANDARD_BUFFER;
 }
 if (StringUtil::Equals(value, "DICTIONARY_BUFFER")) {
  return VectorBufferType::DICTIONARY_BUFFER;
 }
 if (StringUtil::Equals(value, "VECTOR_CHILD_BUFFER")) {
  return VectorBufferType::VECTOR_CHILD_BUFFER;
 }
 if (StringUtil::Equals(value, "STRING_BUFFER")) {
  return VectorBufferType::STRING_BUFFER;
 }
 if (StringUtil::Equals(value, "FSST_BUFFER")) {
  return VectorBufferType::FSST_BUFFER;
 }
 if (StringUtil::Equals(value, "STRUCT_BUFFER")) {
  return VectorBufferType::STRUCT_BUFFER;
 }
 if (StringUtil::Equals(value, "LIST_BUFFER")) {
  return VectorBufferType::LIST_BUFFER;
 }
 if (StringUtil::Equals(value, "MANAGED_BUFFER")) {
  return VectorBufferType::MANAGED_BUFFER;
 }
 if (StringUtil::Equals(value, "OPAQUE_BUFFER")) {
  return VectorBufferType::OPAQUE_BUFFER;
 }
 if (StringUtil::Equals(value, "ARRAY_BUFFER")) {
  return VectorBufferType::ARRAY_BUFFER;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<VectorType>(VectorType value) {
 switch(value) {
 case VectorType::FLAT_VECTOR:
  return "FLAT_VECTOR";
 case VectorType::FSST_VECTOR:
  return "FSST_VECTOR";
 case VectorType::CONSTANT_VECTOR:
  return "CONSTANT_VECTOR";
 case VectorType::DICTIONARY_VECTOR:
  return "DICTIONARY_VECTOR";
 case VectorType::SEQUENCE_VECTOR:
  return "SEQUENCE_VECTOR";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
VectorType EnumUtil::FromString<VectorType>(const char *value) {
 if (StringUtil::Equals(value, "FLAT_VECTOR")) {
  return VectorType::FLAT_VECTOR;
 }
 if (StringUtil::Equals(value, "FSST_VECTOR")) {
  return VectorType::FSST_VECTOR;
 }
 if (StringUtil::Equals(value, "CONSTANT_VECTOR")) {
  return VectorType::CONSTANT_VECTOR;
 }
 if (StringUtil::Equals(value, "DICTIONARY_VECTOR")) {
  return VectorType::DICTIONARY_VECTOR;
 }
 if (StringUtil::Equals(value, "SEQUENCE_VECTOR")) {
  return VectorType::SEQUENCE_VECTOR;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<VerificationType>(VerificationType value) {
 switch(value) {
 case VerificationType::ORIGINAL:
  return "ORIGINAL";
 case VerificationType::COPIED:
  return "COPIED";
 case VerificationType::DESERIALIZED:
  return "DESERIALIZED";
 case VerificationType::PARSED:
  return "PARSED";
 case VerificationType::UNOPTIMIZED:
  return "UNOPTIMIZED";
 case VerificationType::NO_OPERATOR_CACHING:
  return "NO_OPERATOR_CACHING";
 case VerificationType::PREPARED:
  return "PREPARED";
 case VerificationType::EXTERNAL:
  return "EXTERNAL";
 case VerificationType::INVALID:
  return "INVALID";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
VerificationType EnumUtil::FromString<VerificationType>(const char *value) {
 if (StringUtil::Equals(value, "ORIGINAL")) {
  return VerificationType::ORIGINAL;
 }
 if (StringUtil::Equals(value, "COPIED")) {
  return VerificationType::COPIED;
 }
 if (StringUtil::Equals(value, "DESERIALIZED")) {
  return VerificationType::DESERIALIZED;
 }
 if (StringUtil::Equals(value, "PARSED")) {
  return VerificationType::PARSED;
 }
 if (StringUtil::Equals(value, "UNOPTIMIZED")) {
  return VerificationType::UNOPTIMIZED;
 }
 if (StringUtil::Equals(value, "NO_OPERATOR_CACHING")) {
  return VerificationType::NO_OPERATOR_CACHING;
 }
 if (StringUtil::Equals(value, "PREPARED")) {
  return VerificationType::PREPARED;
 }
 if (StringUtil::Equals(value, "EXTERNAL")) {
  return VerificationType::EXTERNAL;
 }
 if (StringUtil::Equals(value, "INVALID")) {
  return VerificationType::INVALID;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<VerifyExistenceType>(VerifyExistenceType value) {
 switch(value) {
 case VerifyExistenceType::APPEND:
  return "APPEND";
 case VerifyExistenceType::APPEND_FK:
  return "APPEND_FK";
 case VerifyExistenceType::DELETE_FK:
  return "DELETE_FK";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
VerifyExistenceType EnumUtil::FromString<VerifyExistenceType>(const char *value) {
 if (StringUtil::Equals(value, "APPEND")) {
  return VerifyExistenceType::APPEND;
 }
 if (StringUtil::Equals(value, "APPEND_FK")) {
  return VerifyExistenceType::APPEND_FK;
 }
 if (StringUtil::Equals(value, "DELETE_FK")) {
  return VerifyExistenceType::DELETE_FK;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<WALType>(WALType value) {
 switch(value) {
 case WALType::INVALID:
  return "INVALID";
 case WALType::CREATE_TABLE:
  return "CREATE_TABLE";
 case WALType::DROP_TABLE:
  return "DROP_TABLE";
 case WALType::CREATE_SCHEMA:
  return "CREATE_SCHEMA";
 case WALType::DROP_SCHEMA:
  return "DROP_SCHEMA";
 case WALType::CREATE_VIEW:
  return "CREATE_VIEW";
 case WALType::DROP_VIEW:
  return "DROP_VIEW";
 case WALType::CREATE_SEQUENCE:
  return "CREATE_SEQUENCE";
 case WALType::DROP_SEQUENCE:
  return "DROP_SEQUENCE";
 case WALType::SEQUENCE_VALUE:
  return "SEQUENCE_VALUE";
 case WALType::CREATE_MACRO:
  return "CREATE_MACRO";
 case WALType::DROP_MACRO:
  return "DROP_MACRO";
 case WALType::CREATE_TYPE:
  return "CREATE_TYPE";
 case WALType::DROP_TYPE:
  return "DROP_TYPE";
 case WALType::ALTER_INFO:
  return "ALTER_INFO";
 case WALType::CREATE_TABLE_MACRO:
  return "CREATE_TABLE_MACRO";
 case WALType::DROP_TABLE_MACRO:
  return "DROP_TABLE_MACRO";
 case WALType::CREATE_INDEX:
  return "CREATE_INDEX";
 case WALType::DROP_INDEX:
  return "DROP_INDEX";
 case WALType::USE_TABLE:
  return "USE_TABLE";
 case WALType::INSERT_TUPLE:
  return "INSERT_TUPLE";
 case WALType::DELETE_TUPLE:
  return "DELETE_TUPLE";
 case WALType::UPDATE_TUPLE:
  return "UPDATE_TUPLE";
 case WALType::CHECKPOINT:
  return "CHECKPOINT";
 case WALType::WAL_FLUSH:
  return "WAL_FLUSH";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
WALType EnumUtil::FromString<WALType>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return WALType::INVALID;
 }
 if (StringUtil::Equals(value, "CREATE_TABLE")) {
  return WALType::CREATE_TABLE;
 }
 if (StringUtil::Equals(value, "DROP_TABLE")) {
  return WALType::DROP_TABLE;
 }
 if (StringUtil::Equals(value, "CREATE_SCHEMA")) {
  return WALType::CREATE_SCHEMA;
 }
 if (StringUtil::Equals(value, "DROP_SCHEMA")) {
  return WALType::DROP_SCHEMA;
 }
 if (StringUtil::Equals(value, "CREATE_VIEW")) {
  return WALType::CREATE_VIEW;
 }
 if (StringUtil::Equals(value, "DROP_VIEW")) {
  return WALType::DROP_VIEW;
 }
 if (StringUtil::Equals(value, "CREATE_SEQUENCE")) {
  return WALType::CREATE_SEQUENCE;
 }
 if (StringUtil::Equals(value, "DROP_SEQUENCE")) {
  return WALType::DROP_SEQUENCE;
 }
 if (StringUtil::Equals(value, "SEQUENCE_VALUE")) {
  return WALType::SEQUENCE_VALUE;
 }
 if (StringUtil::Equals(value, "CREATE_MACRO")) {
  return WALType::CREATE_MACRO;
 }
 if (StringUtil::Equals(value, "DROP_MACRO")) {
  return WALType::DROP_MACRO;
 }
 if (StringUtil::Equals(value, "CREATE_TYPE")) {
  return WALType::CREATE_TYPE;
 }
 if (StringUtil::Equals(value, "DROP_TYPE")) {
  return WALType::DROP_TYPE;
 }
 if (StringUtil::Equals(value, "ALTER_INFO")) {
  return WALType::ALTER_INFO;
 }
 if (StringUtil::Equals(value, "CREATE_TABLE_MACRO")) {
  return WALType::CREATE_TABLE_MACRO;
 }
 if (StringUtil::Equals(value, "DROP_TABLE_MACRO")) {
  return WALType::DROP_TABLE_MACRO;
 }
 if (StringUtil::Equals(value, "CREATE_INDEX")) {
  return WALType::CREATE_INDEX;
 }
 if (StringUtil::Equals(value, "DROP_INDEX")) {
  return WALType::DROP_INDEX;
 }
 if (StringUtil::Equals(value, "USE_TABLE")) {
  return WALType::USE_TABLE;
 }
 if (StringUtil::Equals(value, "INSERT_TUPLE")) {
  return WALType::INSERT_TUPLE;
 }
 if (StringUtil::Equals(value, "DELETE_TUPLE")) {
  return WALType::DELETE_TUPLE;
 }
 if (StringUtil::Equals(value, "UPDATE_TUPLE")) {
  return WALType::UPDATE_TUPLE;
 }
 if (StringUtil::Equals(value, "CHECKPOINT")) {
  return WALType::CHECKPOINT;
 }
 if (StringUtil::Equals(value, "WAL_FLUSH")) {
  return WALType::WAL_FLUSH;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<WindowAggregationMode>(WindowAggregationMode value) {
 switch(value) {
 case WindowAggregationMode::WINDOW:
  return "WINDOW";
 case WindowAggregationMode::COMBINE:
  return "COMBINE";
 case WindowAggregationMode::SEPARATE:
  return "SEPARATE";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
WindowAggregationMode EnumUtil::FromString<WindowAggregationMode>(const char *value) {
 if (StringUtil::Equals(value, "WINDOW")) {
  return WindowAggregationMode::WINDOW;
 }
 if (StringUtil::Equals(value, "COMBINE")) {
  return WindowAggregationMode::COMBINE;
 }
 if (StringUtil::Equals(value, "SEPARATE")) {
  return WindowAggregationMode::SEPARATE;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<WindowBoundary>(WindowBoundary value) {
 switch(value) {
 case WindowBoundary::INVALID:
  return "INVALID";
 case WindowBoundary::UNBOUNDED_PRECEDING:
  return "UNBOUNDED_PRECEDING";
 case WindowBoundary::UNBOUNDED_FOLLOWING:
  return "UNBOUNDED_FOLLOWING";
 case WindowBoundary::CURRENT_ROW_RANGE:
  return "CURRENT_ROW_RANGE";
 case WindowBoundary::CURRENT_ROW_ROWS:
  return "CURRENT_ROW_ROWS";
 case WindowBoundary::EXPR_PRECEDING_ROWS:
  return "EXPR_PRECEDING_ROWS";
 case WindowBoundary::EXPR_FOLLOWING_ROWS:
  return "EXPR_FOLLOWING_ROWS";
 case WindowBoundary::EXPR_PRECEDING_RANGE:
  return "EXPR_PRECEDING_RANGE";
 case WindowBoundary::EXPR_FOLLOWING_RANGE:
  return "EXPR_FOLLOWING_RANGE";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
WindowBoundary EnumUtil::FromString<WindowBoundary>(const char *value) {
 if (StringUtil::Equals(value, "INVALID")) {
  return WindowBoundary::INVALID;
 }
 if (StringUtil::Equals(value, "UNBOUNDED_PRECEDING")) {
  return WindowBoundary::UNBOUNDED_PRECEDING;
 }
 if (StringUtil::Equals(value, "UNBOUNDED_FOLLOWING")) {
  return WindowBoundary::UNBOUNDED_FOLLOWING;
 }
 if (StringUtil::Equals(value, "CURRENT_ROW_RANGE")) {
  return WindowBoundary::CURRENT_ROW_RANGE;
 }
 if (StringUtil::Equals(value, "CURRENT_ROW_ROWS")) {
  return WindowBoundary::CURRENT_ROW_ROWS;
 }
 if (StringUtil::Equals(value, "EXPR_PRECEDING_ROWS")) {
  return WindowBoundary::EXPR_PRECEDING_ROWS;
 }
 if (StringUtil::Equals(value, "EXPR_FOLLOWING_ROWS")) {
  return WindowBoundary::EXPR_FOLLOWING_ROWS;
 }
 if (StringUtil::Equals(value, "EXPR_PRECEDING_RANGE")) {
  return WindowBoundary::EXPR_PRECEDING_RANGE;
 }
 if (StringUtil::Equals(value, "EXPR_FOLLOWING_RANGE")) {
  return WindowBoundary::EXPR_FOLLOWING_RANGE;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<WindowExcludeMode>(WindowExcludeMode value) {
 switch(value) {
 case WindowExcludeMode::NO_OTHER:
  return "NO_OTHER";
 case WindowExcludeMode::CURRENT_ROW:
  return "CURRENT_ROW";
 case WindowExcludeMode::GROUP:
  return "GROUP";
 case WindowExcludeMode::TIES:
  return "TIES";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
WindowExcludeMode EnumUtil::FromString<WindowExcludeMode>(const char *value) {
 if (StringUtil::Equals(value, "NO_OTHER")) {
  return WindowExcludeMode::NO_OTHER;
 }
 if (StringUtil::Equals(value, "CURRENT_ROW")) {
  return WindowExcludeMode::CURRENT_ROW;
 }
 if (StringUtil::Equals(value, "GROUP")) {
  return WindowExcludeMode::GROUP;
 }
 if (StringUtil::Equals(value, "TIES")) {
  return WindowExcludeMode::TIES;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
template<>
const char* EnumUtil::ToChars<WithinCollection>(WithinCollection value) {
 switch(value) {
 case WithinCollection::NO:
  return "NO";
 case WithinCollection::LIST:
  return "LIST";
 case WithinCollection::ARRAY:
  return "ARRAY";
 default:
  throw NotImplementedException(StringUtil::Format("Enum value: '%d' not implemented", value));
 }
}
template<>
WithinCollection EnumUtil::FromString<WithinCollection>(const char *value) {
 if (StringUtil::Equals(value, "NO")) {
  return WithinCollection::NO;
 }
 if (StringUtil::Equals(value, "LIST")) {
  return WithinCollection::LIST;
 }
 if (StringUtil::Equals(value, "ARRAY")) {
  return WithinCollection::ARRAY;
 }
 throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}
}
