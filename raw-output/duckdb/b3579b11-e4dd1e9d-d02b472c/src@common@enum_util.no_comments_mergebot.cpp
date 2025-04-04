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
<<<<<<< HEAD
 case CatalogType::SECRET_ENTRY:
  return "SECRET_ENTRY";
 case CatalogType::SECRET_TYPE_ENTRY:
  return "SECRET_TYPE_ENTRY";
 case CatalogType::SECRET_FUNCTION_ENTRY:
  return "SECRET_FUNCTION_ENTRY";
||||||| d02b472cbf
=======
 case CatalogType::RENAMED_ENTRY:
  return "RENAMED_ENTRY";
 case CatalogType::DEPENDENCY_ENTRY:
  return "DEPENDENCY_ENTRY";
>>>>>>> e4dd1e9d
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
