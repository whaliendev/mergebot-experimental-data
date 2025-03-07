#include "duckdb/function/built_in_functions.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/operator/csv_scanner/sniffer/csv_sniffer.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_buffer_manager.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/function/table/range.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_file_handle.hpp"
#include "duckdb/function/table/read_csv.hpp"
namespace duckdb {
struct CSVSniffFunctionData : public TableFunctionData {
 CSVSniffFunctionData() {
 }
 string path;
 CSVReaderOptions options;
 vector<LogicalType> return_types_csv;
 vector<string> names_csv;
 bool force_match = true;
};
struct CSVSniffGlobalState : public GlobalTableFunctionState {
 CSVSniffGlobalState() {
 }
 bool done = false;
};
static unique_ptr<GlobalTableFunctionState> CSVSniffInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
 return make_uniq<CSVSniffGlobalState>();
}
static unique_ptr<FunctionData> CSVSniffBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
 auto result = make_uniq<CSVSniffFunctionData>();
 auto &config = DBConfig::GetConfig(context);
 if (!config.options.enable_external_access) {
  throw PermissionException("sniff_csv is disabled through configuration");
 }
 result->path = input.inputs[0].ToString();
 auto it = input.named_parameters.find("auto_detect");
 if (it != input.named_parameters.end()) {
  if (!it->second.GetValue<bool>()) {
   throw InvalidInputException("sniff_csv function does not accept auto_detect variable set to false");
  }
  input.named_parameters.erase("auto_detect");
 }
 it = input.named_parameters.find("force_match");
 if (it != input.named_parameters.end()) {
  result->force_match = it->second.GetValue<bool>();
  input.named_parameters.erase("force_match");
 }
 result->options.FromNamedParameters(input.named_parameters, context);
<<<<<<< HEAD
|||||||
=======
 result->options.Verify();
>>>>>>> 104cfacc371eb020431f5ef61ae599b8dc969f07
 return_types.emplace_back(LogicalType::VARCHAR);
 names.emplace_back("Delimiter");
 return_types.emplace_back(LogicalType::VARCHAR);
 names.emplace_back("Quote");
 return_types.emplace_back(LogicalType::VARCHAR);
 names.emplace_back("Escape");
 return_types.emplace_back(LogicalType::VARCHAR);
 names.emplace_back("NewLineDelimiter");
 return_types.emplace_back(LogicalType::VARCHAR);
 names.emplace_back("Comment");
 return_types.emplace_back(LogicalType::UINTEGER);
 names.emplace_back("SkipRows");
 return_types.emplace_back(LogicalType::BOOLEAN);
 names.emplace_back("HasHeader");
 child_list_t<LogicalType> struct_children {{"name", LogicalType::VARCHAR}, {"type", LogicalType::VARCHAR}};
 auto list_child = LogicalType::STRUCT(struct_children);
 return_types.emplace_back(LogicalType::LIST(list_child));
 names.emplace_back("Columns");
 return_types.emplace_back(LogicalType::VARCHAR);
 names.emplace_back("DateFormat");
 return_types.emplace_back(LogicalType::VARCHAR);
 names.emplace_back("TimestampFormat");
 return_types.emplace_back(LogicalType::VARCHAR);
 names.emplace_back("UserArguments");
 return_types.emplace_back(LogicalType::VARCHAR);
 names.emplace_back("Prompt");
 return std::move(result);
}
string FormatOptions(char opt) {
 if (opt == '\'') {
  return "''";
 }
 if (opt == '\0') {
  return "";
 }
 string result;
 result += opt;
 return result;
}
static void CSVSniffFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
 auto &global_state = data_p.global_state->Cast<CSVSniffGlobalState>();
 if (global_state.done) {
  return;
 }
 const CSVSniffFunctionData &data = data_p.bind_data->Cast<CSVSniffFunctionData>();
 auto &fs = duckdb::FileSystem::GetFileSystem(context);
 auto paths = fs.GlobFiles(data.path, context, FileGlobOptions::DISALLOW_EMPTY);
 if (paths.size() > 1) {
  throw NotImplementedException("sniff_csv does not operate on more than one file yet");
 }
 auto sniffer_options = data.options;
 sniffer_options.file_path = paths[0];
 auto buffer_manager = make_shared_ptr<CSVBufferManager>(context, sniffer_options, sniffer_options.file_path, 0);
 if (sniffer_options.name_list.empty()) {
  sniffer_options.name_list = data.names_csv;
 }
 if (sniffer_options.sql_type_list.empty()) {
  sniffer_options.sql_type_list = data.return_types_csv;
 }
 CSVSniffer sniffer(sniffer_options, buffer_manager, CSVStateMachineCache::Get(context));
 auto sniffer_result = sniffer.SniffCSV(data.force_match);
 string str_opt;
 string separator = ", ";
 output.SetCardinality(1);
 str_opt = sniffer_options.dialect_options.state_machine_options.delimiter.GetValue();
 output.SetValue(0, 0, str_opt);
 str_opt = sniffer_options.dialect_options.state_machine_options.quote.GetValue();
 output.SetValue(1, 0, str_opt);
 str_opt = sniffer_options.dialect_options.state_machine_options.escape.GetValue();
 output.SetValue(2, 0, str_opt);
 auto new_line_identifier = sniffer_options.NewLineIdentifierToString();
 output.SetValue(3, 0, new_line_identifier);
 str_opt = sniffer_options.dialect_options.state_machine_options.comment.GetValue();
 output.SetValue(4, 0, str_opt);
 output.SetValue(5, 0, Value::UINTEGER(NumericCast<uint32_t>(sniffer_options.dialect_options.skip_rows.GetValue())));
 auto has_header = Value::BOOLEAN(sniffer_options.dialect_options.header.GetValue()).ToString();
 output.SetValue(6, 0, has_header);
 vector<Value> values;
 std::ostringstream columns;
 columns << "{";
 for (idx_t i = 0; i < sniffer_result.return_types.size(); i++) {
  child_list_t<Value> struct_children {{"name", sniffer_result.names[i]},
                                       {"type", {sniffer_result.return_types[i].ToString()}}};
  values.emplace_back(Value::STRUCT(struct_children));
  columns << "'" << sniffer_result.names[i] << "': '" << sniffer_result.return_types[i].ToString() << "'";
  if (i != sniffer_result.return_types.size() - 1) {
   columns << separator;
  }
 }
 columns << "}";
 output.SetValue(7, 0, Value::LIST(values));
 auto date_format = sniffer_options.dialect_options.date_format[LogicalType::DATE].GetValue();
 if (!date_format.Empty()) {
  output.SetValue(8, 0, date_format.format_specifier);
 } else {
  bool has_date = false;
  for (auto &c_type : sniffer_result.return_types) {
   if (c_type.id() == LogicalTypeId::DATE) {
    output.SetValue(8, 0, Value("%Y-%m-%d"));
    has_date = true;
   }
  }
  if (!has_date) {
   output.SetValue(8, 0, Value(nullptr));
  }
 }
 auto timestamp_format = sniffer_options.dialect_options.date_format[LogicalType::TIMESTAMP].GetValue();
 if (!timestamp_format.Empty()) {
  output.SetValue(9, 0, timestamp_format.format_specifier);
 } else {
  output.SetValue(9, 0, Value(nullptr));
 }
 if (data.options.user_defined_parameters.empty()) {
  output.SetValue(10, 0, Value());
 } else {
  output.SetValue(10, 0, Value(data.options.user_defined_parameters));
 }
 std::ostringstream csv_read;
 csv_read << "FROM read_csv('" << paths[0] << "'" << separator << "auto_detect=false" << separator;
 if (!sniffer_options.dialect_options.state_machine_options.delimiter.IsSetByUser()) {
  csv_read << "delim="
           << "'" << FormatOptions(sniffer_options.dialect_options.state_machine_options.delimiter.GetValue())
           << "'" << separator;
 }
 if (!sniffer_options.dialect_options.state_machine_options.quote.IsSetByUser()) {
  csv_read << "quote="
           << "'" << FormatOptions(sniffer_options.dialect_options.state_machine_options.quote.GetValue()) << "'"
           << separator;
 }
 if (!sniffer_options.dialect_options.state_machine_options.escape.IsSetByUser()) {
  csv_read << "escape="
           << "'" << FormatOptions(sniffer_options.dialect_options.state_machine_options.escape.GetValue()) << "'"
           << separator;
 }
 if (!sniffer_options.dialect_options.state_machine_options.new_line.IsSetByUser()) {
  if (new_line_identifier != "mix") {
   csv_read << "new_line="
            << "'" << new_line_identifier << "'" << separator;
  }
 }
 if (!sniffer_options.dialect_options.skip_rows.IsSetByUser()) {
  csv_read << "skip=" << sniffer_options.dialect_options.skip_rows.GetValue() << separator;
 }
 if (!sniffer_options.dialect_options.state_machine_options.comment.IsSetByUser()) {
  csv_read << "comment="
           << "'" << FormatOptions(sniffer_options.dialect_options.state_machine_options.comment.GetValue())
           << "'" << separator;
 }
 if (!sniffer_options.dialect_options.header.IsSetByUser()) {
  csv_read << "header=" << has_header << separator;
 }
 csv_read << "columns=" << columns.str();
 if (!sniffer_options.dialect_options.date_format[LogicalType::DATE].IsSetByUser()) {
  if (!sniffer_options.dialect_options.date_format[LogicalType::DATE].GetValue().format_specifier.empty()) {
   csv_read << separator << "dateformat="
            << "'"
            << sniffer_options.dialect_options.date_format[LogicalType::DATE].GetValue().format_specifier
            << "'";
  } else {
   for (auto &c_type : sniffer_result.return_types) {
    if (c_type.id() == LogicalTypeId::DATE) {
     csv_read << separator << "dateformat="
              << "'%Y-%m-%d'";
     break;
    }
   }
  }
 }
 if (!sniffer_options.dialect_options.date_format[LogicalType::TIMESTAMP].IsSetByUser()) {
  if (!sniffer_options.dialect_options.date_format[LogicalType::TIMESTAMP].GetValue().format_specifier.empty()) {
   csv_read << separator << "timestampformat="
            << "'"
            << sniffer_options.dialect_options.date_format[LogicalType::TIMESTAMP].GetValue().format_specifier
            << "'";
  }
 }
 if (!data.options.user_defined_parameters.empty()) {
  csv_read << separator << data.options.user_defined_parameters;
 }
 csv_read << ");";
 output.SetValue(11, 0, csv_read.str());
 global_state.done = true;
}
void CSVSnifferFunction::RegisterFunction(BuiltinFunctions &set) {
 TableFunction csv_sniffer("sniff_csv", {LogicalType::VARCHAR}, CSVSniffFunction, CSVSniffBind, CSVSniffInitGlobal);
 ReadCSVTableFunction::ReadCSVAddNamedParameters(csv_sniffer);
 csv_sniffer.named_parameters["force_match"] = LogicalType::BOOLEAN;
 set.AddFunction(csv_sniffer);
}
}
