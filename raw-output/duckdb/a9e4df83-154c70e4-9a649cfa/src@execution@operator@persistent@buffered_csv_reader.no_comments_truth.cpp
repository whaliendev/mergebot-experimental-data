#include "duckdb/execution/operator/persistent/buffered_csv_reader.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/gzip_stream.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/scalar/strftime.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/storage/data_table.hpp"
#include "utf8proc_wrapper.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
using namespace std;
namespace duckdb {
static char is_newline(char c) {
 return c == '\n' || c == '\r';
}
static string GenerateColumnName(const idx_t total_cols, const idx_t col_number, const string prefix = "column") {
 uint8_t max_digits = total_cols > 10 ? (int)log10((double)total_cols - 1) + 1 : 1;
 uint8_t digits = col_number >= 10 ? (int)log10((double)col_number) + 1 : 1;
 string leading_zeros = string("0", max_digits - digits);
 string value = std::to_string(col_number);
 return string(prefix + leading_zeros + value);
}
static string GetLineNumberStr(idx_t linenr, bool linenr_estimated) {
 string estimated = (linenr_estimated ? string(" (estimated)") : string(""));
 return std::to_string(linenr + 1) + estimated;
}
static bool is_digit(char c) {
 return std::isdigit(c);
}
static bool StartsWithNumericDate(string &separator, const string_t &value) {
 auto begin = value.GetData();
 auto end = begin + value.GetSize();
 auto field1 = std::find_if_not(begin, end, StringUtil::CharacterIsSpace);
 if (field1 == end) {
  return false;
 }
 if (!std::isdigit(*field1)) {
  return false;
 }
 auto literal1 = std::find_if_not(field1, end, is_digit);
 if (literal1 == end) {
  return false;
 }
 auto field2 = std::find_if(literal1, end, is_digit);
 if (field2 == end) {
  return false;
 }
 auto literal2 = std::find_if_not(field2, end, is_digit);
 if (literal2 == end) {
  return false;
 }
 auto field3 = std::find_if(literal2, end, is_digit);
 if (field3 == end) {
  return false;
 }
 if (((field3 - literal2) != (field2 - literal1)) || strncmp(literal1, literal2, (field2 - literal1))) {
  return false;
 }
 separator.clear();
 while (literal1 < field2) {
  const auto literal_char = *literal1++;
  if (literal_char == '%') {
   separator.push_back(literal_char);
  }
  separator.push_back(literal_char);
 }
 return true;
}
string GenerateDateFormat(const string &separator, const char *format_template) {
 string format_specifier = format_template;
 for (auto pos = std::find(format_specifier.begin(), format_specifier.end(), '-'); pos != format_specifier.end();
      pos = std::find(pos + separator.size(), format_specifier.end(), '-')) {
  format_specifier.replace(pos, pos + 1, separator);
 }
 return format_specifier;
}
TextSearchShiftArray::TextSearchShiftArray() {
}
TextSearchShiftArray::TextSearchShiftArray(string search_term) : length(search_term.size()) {
 if (length > 255) {
  throw Exception("Size of delimiter/quote/escape in CSV reader is limited to 255 bytes");
 }
 shifts = unique_ptr<uint8_t[]>(new uint8_t[length * 255]);
 memset(shifts.get(), 0, length * 255 * sizeof(uint8_t));
 for (idx_t main_idx = 0; main_idx < length; main_idx++) {
  uint8_t current_char = (uint8_t)search_term[main_idx];
  for (idx_t i = main_idx; i < length; i++) {
   bool is_match = true;
   for (idx_t j = 0; j < main_idx; j++) {
    if (search_term[i - main_idx + j] != search_term[j]) {
     is_match = false;
    }
   }
   if (!is_match) {
    continue;
   }
   shifts[i * 255 + current_char] = main_idx + 1;
  }
 }
}
BufferedCSVReader::BufferedCSVReader(ClientContext &context, BufferedCSVReaderOptions options,
                                     vector<LogicalType> requested_types)
    : options(options), buffer_size(0), position(0), start(0) {
 source = OpenCSV(context, options);
 Initialize(requested_types);
}
BufferedCSVReader::BufferedCSVReader(BufferedCSVReaderOptions options, vector<LogicalType> requested_types,
                                     unique_ptr<istream> ssource)
    : options(options), source(move(ssource)), buffer_size(0), position(0), start(0) {
 Initialize(requested_types);
}
void BufferedCSVReader::Initialize(vector<LogicalType> requested_types) {
 if (options.auto_detect) {
  sql_types = SniffCSV(requested_types);
 } else {
  sql_types = requested_types;
 }
 PrepareComplexParser();
 InitParseChunk(sql_types.size());
 SkipHeader(options.skip_rows, options.header);
}
void BufferedCSVReader::PrepareComplexParser() {
 delimiter_search = TextSearchShiftArray(options.delimiter);
 escape_search = TextSearchShiftArray(options.escape);
 quote_search = TextSearchShiftArray(options.quote);
}
void BufferedCSVReader::ConfigureSampling() {
 if (options.sample_size > STANDARD_VECTOR_SIZE) {
  throw ParserException("Chunk size (%d) cannot be bigger than STANDARD_VECTOR_SIZE (%d)", options.sample_size,
                        STANDARD_VECTOR_SIZE);
 } else if (options.sample_size < 1) {
  throw ParserException("Chunk size cannot be smaller than 1.");
 }
 SAMPLE_CHUNK_SIZE = options.sample_size;
 MAX_SAMPLE_CHUNKS = options.num_samples;
}
unique_ptr<istream> BufferedCSVReader::OpenCSV(ClientContext &context, BufferedCSVReaderOptions options) {
 if (!FileSystem::GetFileSystem(context).FileExists(options.file_path)) {
  throw IOException("File \"%s\" not found", options.file_path.c_str());
 }
 unique_ptr<istream> result;
 if (StringUtil::EndsWith(StringUtil::Lower(options.file_path), ".gz")) {
  result = make_unique<GzipStream>(options.file_path);
  plain_file_source = false;
 } else {
  auto csv_local = make_unique<ifstream>();
  csv_local->open(options.file_path);
  result = move(csv_local);
  plain_file_source = true;
  result->seekg(0, result->end);
  file_size = (idx_t)result->tellg();
  result->clear();
  result->seekg(0, result->beg);
 }
 return result;
}
void BufferedCSVReader::SkipHeader(idx_t skip_rows, bool skip_header) {
 for (idx_t i = 0; i < skip_rows; i++) {
  string read_line;
  getline(*source, read_line);
  linenr++;
 }
 if (skip_header) {
  string read_line;
  getline(*source, read_line);
  linenr++;
 }
}
void BufferedCSVReader::ResetBuffer() {
 buffer.reset();
 buffer_size = 0;
 position = 0;
 start = 0;
 cached_buffers.clear();
}
void BufferedCSVReader::ResetStream() {
 if (!plain_file_source && StringUtil::EndsWith(StringUtil::Lower(options.file_path), ".gz")) {
  source = make_unique<GzipStream>(options.file_path);
 } else {
  source->clear();
  source->seekg(0, source->beg);
 }
 linenr = 0;
 linenr_estimated = false;
 bytes_per_line_avg = 0;
 sample_chunk_idx = 0;
 jumping_samples = false;
}
void BufferedCSVReader::ResetParseChunk() {
 bytes_in_chunk = 0;
 parse_chunk.Reset();
}
void BufferedCSVReader::InitParseChunk(idx_t num_cols) {
 if (options.force_not_null.size() != num_cols) {
  options.force_not_null.resize(num_cols, false);
 }
 parse_chunk.Destroy();
 vector<LogicalType> varchar_types(num_cols, LogicalType::VARCHAR);
 parse_chunk.Initialize(varchar_types);
}
void BufferedCSVReader::JumpToBeginning(idx_t skip_rows, bool skip_header) {
 ResetBuffer();
 ResetStream();
 ResetParseChunk();
 SkipHeader(skip_rows, skip_header);
}
bool BufferedCSVReader::JumpToNextSample() {
 if (end_of_file_reached || sample_chunk_idx >= MAX_SAMPLE_CHUNKS) {
  return false;
 }
 idx_t remaining_bytes_in_buffer = buffer_size - start;
 bytes_in_chunk -= remaining_bytes_in_buffer;
 double bytes_per_line = bytes_in_chunk / (double)SAMPLE_CHUNK_SIZE;
 bytes_per_line_avg = ((bytes_per_line_avg * sample_chunk_idx) + bytes_per_line) / (sample_chunk_idx + 1);
 if (sample_chunk_idx == 0) {
  idx_t bytes_first_chunk = bytes_in_chunk;
  double chunks_fit = (file_size / (double)bytes_first_chunk);
  jumping_samples = chunks_fit >= (MAX_SAMPLE_CHUNKS - 1);
 }
 if (!plain_file_source || !jumping_samples) {
  sample_chunk_idx++;
  ResetParseChunk();
  return true;
 }
 idx_t partition_size = (idx_t)round(file_size / (double)MAX_SAMPLE_CHUNKS);
 int64_t offset = partition_size - bytes_in_chunk - remaining_bytes_in_buffer;
 idx_t current_pos = (idx_t)source->tellg();
 if (current_pos + offset < file_size) {
  source->clear();
  source->seekg(offset, source->cur);
  linenr += (idx_t)round((offset + remaining_bytes_in_buffer) / bytes_per_line_avg);
  linenr_estimated = true;
 } else {
  source->seekg(-streamoff(bytes_in_chunk), source->end);
  linenr = (idx_t)round((file_size - bytes_in_chunk) / bytes_per_line_avg);
  linenr_estimated = true;
 }
 ResetBuffer();
 ResetParseChunk();
 string read_line;
 getline(*source, read_line);
 linenr++;
 sample_chunk_idx++;
 return true;
}
void BufferedCSVReader::SetDateFormat(const string &format_specifier, const LogicalTypeId &sql_type) {
 options.has_format[sql_type] = true;
 auto &date_format = options.date_format[sql_type];
 date_format.format_specifier = format_specifier;
 StrTimeFormat::ParseFormatSpecifier(date_format.format_specifier, date_format);
}
bool BufferedCSVReader::TryCastValue(Value value, LogicalType sql_type) {
 try {
  if (options.has_format[LogicalTypeId::DATE] && sql_type.id() == LogicalTypeId::DATE) {
   options.date_format[LogicalTypeId::DATE].ParseDate(value.str_value);
  } else if (options.has_format[LogicalTypeId::TIMESTAMP] && sql_type.id() == LogicalTypeId::TIMESTAMP) {
   options.date_format[LogicalTypeId::TIMESTAMP].ParseTimestamp(value.str_value);
  } else {
   value.CastAs(sql_type, true);
  }
  return true;
 } catch (...) {
  return false;
 }
 return false;
}
bool BufferedCSVReader::TryCastVector(Vector &parse_chunk_col, idx_t size, LogicalType sql_type) {
 try {
  Vector dummy_result(sql_type);
  if (options.has_format[LogicalTypeId::DATE] && sql_type == LogicalTypeId::DATE) {
   UnaryExecutor::Execute<string_t, date_t, true>(parse_chunk_col, dummy_result, size, [&](string_t input) {
    return options.date_format[LogicalTypeId::DATE].ParseDate(input);
   });
  } else if (options.has_format[LogicalTypeId::TIMESTAMP] && sql_type == LogicalTypeId::TIMESTAMP) {
   UnaryExecutor::Execute<string_t, timestamp_t, true>(
       parse_chunk_col, dummy_result, size,
       [&](string_t input) { return options.date_format[LogicalTypeId::TIMESTAMP].ParseTimestamp(input); });
  } else {
   VectorOperations::Cast(parse_chunk_col, dummy_result, size, true);
  }
 } catch (const Exception &e) {
  return false;
 }
 return true;
}
void BufferedCSVReader::PrepareCandidateSets() {
 if (options.has_delimiter) {
  delim_candidates = {options.delimiter};
 }
 if (options.has_quote) {
  quote_candidates_map = {{options.quote}, {options.quote}, {options.quote}};
 }
 if (options.has_escape) {
  if (options.escape == "") {
   quoterule_candidates = {QuoteRule::QUOTES_RFC};
  } else {
   quoterule_candidates = {QuoteRule::QUOTES_OTHER};
  }
  escape_candidates_map[static_cast<uint8_t>(quoterule_candidates[0])] = {options.escape};
 }
}
vector<LogicalType> BufferedCSVReader::SniffCSV(vector<LogicalType> requested_types) {
 for (auto &type : requested_types) {
  if (type.id() == LogicalTypeId::BLOB) {
   return requested_types;
  }
 }
 ConfigureSampling();
 PrepareCandidateSets();
 BufferedCSVReaderOptions original_options = options;
 vector<BufferedCSVReaderOptions> info_candidates;
 idx_t best_consistent_rows = 0;
 idx_t best_num_cols = 0;
 JumpToBeginning(0, false);
 for (QuoteRule quoterule : quoterule_candidates) {
  vector<string> quote_candidates = quote_candidates_map[static_cast<uint8_t>(quoterule)];
  for (const auto &quote : quote_candidates) {
   for (const auto &delim : delim_candidates) {
    vector<string> escape_candidates = escape_candidates_map[static_cast<uint8_t>(quoterule)];
    for (const auto &escape : escape_candidates) {
     BufferedCSVReaderOptions sniff_info = original_options;
     sniff_info.delimiter = delim;
     sniff_info.quote = quote;
     sniff_info.escape = escape;
     options = sniff_info;
     PrepareComplexParser();
     ResetBuffer();
     ResetStream();
     sniffed_column_counts.clear();
     try {
      ParseCSV(ParserMode::SNIFFING_DIALECT);
     } catch (const ParserException &e) {
      continue;
     }
     idx_t start_row = 0;
     idx_t consistent_rows = 0;
     idx_t num_cols = 0;
     for (idx_t row = 0; row < sniffed_column_counts.size(); row++) {
      if (sniffed_column_counts[row] == num_cols) {
       consistent_rows++;
      } else {
       num_cols = sniffed_column_counts[row];
       start_row = row;
       consistent_rows = 1;
      }
     }
     bool more_values = (consistent_rows > best_consistent_rows && num_cols >= best_num_cols);
     bool single_column_before = best_num_cols < 2 && num_cols > best_num_cols;
     bool rows_consistent = start_row + consistent_rows == sniffed_column_counts.size();
     bool more_than_one_row = (consistent_rows > 1);
     bool more_than_one_column = (num_cols > 1);
     bool start_good = info_candidates.size() > 0 && (start_row <= info_candidates.front().skip_rows);
     if (requested_types.size() > 0 && requested_types.size() != num_cols) {
      continue;
     } else if ((more_values || single_column_before) && rows_consistent) {
      sniff_info.skip_rows = start_row;
      sniff_info.num_cols = num_cols;
      best_consistent_rows = consistent_rows;
      best_num_cols = num_cols;
      info_candidates.clear();
      info_candidates.push_back(sniff_info);
     } else if (more_than_one_row && more_than_one_column && start_good && rows_consistent) {
      bool same_quote_is_candidate = false;
      for (auto &info_candidate : info_candidates) {
       if (quote.compare(info_candidate.quote) == 0) {
        same_quote_is_candidate = true;
       }
      }
      if (!same_quote_is_candidate) {
       sniff_info.skip_rows = start_row;
       sniff_info.num_cols = num_cols;
       info_candidates.push_back(sniff_info);
      }
     }
    }
   }
  }
 }
 if (info_candidates.size() < 1) {
  if (requested_types.size() == 0) {
   col_names.push_back("col0");
   requested_types.push_back(LogicalType::VARCHAR);
  }
  options = original_options;
  JumpToBeginning(0, false);
  return requested_types;
 }
 vector<LogicalType> type_candidates = {
     LogicalType::VARCHAR, LogicalType::TIMESTAMP,
     LogicalType::DATE, LogicalType::TIME,
     LogicalType::DOUBLE, LogicalType::BIGINT,
     LogicalType::INTEGER, LogicalType::BOOLEAN};
 std::map<LogicalTypeId, vector<const char *>> format_template_candidates = {
     {LogicalTypeId::DATE, {"%m-%d-%Y", "%m-%d-%y", "%d-%m-%Y", "%d-%m-%y", "%Y-%m-%d", "%y-%m-%d"}},
     {LogicalTypeId::TIMESTAMP,
      {"%m-%d-%Y %I:%M:%S %p", "%m-%d-%y %I:%M:%S %p", "%d-%m-%Y %H:%M:%S", "%d-%m-%y %H:%M:%S", "%Y-%m-%d %H:%M:%S",
       "%y-%m-%d %H:%M:%S"}},
 };
 BufferedCSVReaderOptions best_options;
 idx_t min_varchar_cols = best_num_cols + 1;
 vector<vector<LogicalType>> best_sql_types_candidates;
 std::map<LogicalTypeId, vector<string>> best_format_candidates;
 for (const auto &t : format_template_candidates) {
  best_format_candidates[t.first].clear();
 }
 for (auto &info_candidate : info_candidates) {
  options = info_candidate;
  vector<vector<LogicalType>> info_sql_types_candidates(options.num_cols, type_candidates);
  std::map<LogicalTypeId, bool> has_format_candidates;
  std::map<LogicalTypeId, vector<string>> format_candidates;
  for (const auto &t : format_template_candidates) {
   has_format_candidates[t.first] = false;
   format_candidates[t.first].clear();
  }
  sql_types.clear();
  sql_types.assign(options.num_cols, LogicalType::VARCHAR);
  InitParseChunk(sql_types.size());
  JumpToBeginning(options.skip_rows, false);
  ParseCSV(ParserMode::SNIFFING_DATATYPES);
  for (idx_t row = 0; row < parse_chunk.size(); row++) {
   for (idx_t col = 0; col < parse_chunk.column_count(); col++) {
    vector<LogicalType> &col_type_candidates = info_sql_types_candidates[col];
    while (col_type_candidates.size() > 1) {
     const auto &sql_type = col_type_candidates.back();
     auto dummy_val = parse_chunk.GetValue(col, row);
     string separator;
     if (has_format_candidates.count(sql_type.id()) && !original_options.has_format[sql_type.id()] &&
         StartsWithNumericDate(separator, dummy_val.str_value)) {
      auto &type_format_candidates = format_candidates[sql_type.id()];
      const auto had_format_candidates = has_format_candidates[sql_type.id()];
      if (!has_format_candidates[sql_type.id()]) {
       has_format_candidates[sql_type.id()] = true;
       for (const auto &t : format_template_candidates[sql_type.id()]) {
        const auto format_string = GenerateDateFormat(separator, t);
        if (format_string.find("%Y-%m-%d") == string::npos)
         type_format_candidates.emplace_back(format_string);
       }
       options.has_format[sql_type.id()] = true;
       SetDateFormat(type_format_candidates.back(), sql_type.id());
      }
      StrpTimeFormat::ParseResult result;
      auto save_format_candidates = type_format_candidates;
      while (type_format_candidates.size()) {
       auto &current_format = options.date_format[sql_type.id()];
       if (current_format.Parse(dummy_val.str_value, result)) {
        break;
       }
       type_format_candidates.pop_back();
       options.has_format[sql_type.id()] = (type_format_candidates.size() > 0);
       if (type_format_candidates.size() > 0) {
        SetDateFormat(type_format_candidates.back(), sql_type.id());
       }
      }
      if (!type_format_candidates.size()) {
       if (had_format_candidates) {
        type_format_candidates.swap(save_format_candidates);
        if (type_format_candidates.size()) {
         SetDateFormat(type_format_candidates.back(), sql_type.id());
        }
       } else {
        has_format_candidates[sql_type.id()] = false;
       }
      }
     }
     if (TryCastValue(dummy_val, sql_type)) {
      break;
     } else {
      col_type_candidates.pop_back();
     }
    }
   }
   if (parse_chunk.size() > 1 && row == 0) {
    info_sql_types_candidates = vector<vector<LogicalType>>(options.num_cols, type_candidates);
    for (auto &f : format_candidates) {
     f.second.clear();
    }
    for (auto &h : has_format_candidates) {
     h.second = false;
    }
   }
  }
  idx_t varchar_cols = 0;
  for (idx_t col = 0; col < parse_chunk.column_count(); col++) {
   const auto &col_type = info_sql_types_candidates[col].back();
   if (col_type == LogicalType::VARCHAR) {
    varchar_cols++;
   }
  }
  if (varchar_cols < min_varchar_cols && parse_chunk.column_count() > (best_num_cols * 0.7)) {
   best_options = info_candidate;
   min_varchar_cols = varchar_cols;
   best_sql_types_candidates = info_sql_types_candidates;
   best_format_candidates = format_candidates;
  }
 }
 options = best_options;
 for (const auto &best : best_format_candidates) {
  if (best.second.size()) {
   SetDateFormat(best.second.back(), best.first);
  }
 }
 sql_types.clear();
 sql_types.assign(options.num_cols, LogicalType::VARCHAR);
 InitParseChunk(sql_types.size());
 vector<LogicalType> detected_types;
 if (requested_types.size() > 0) {
  if (requested_types.size() != options.num_cols) {
   throw ParserException("Error while determining column types: found %lld columns but expected %d",
                         options.num_cols, requested_types.size());
  } else {
   detected_types = requested_types;
  }
 } else {
  while (JumpToNextSample()) {
   try {
    ParseCSV(ParserMode::SNIFFING_DATATYPES);
   } catch (const ParserException &e) {
    continue;
   }
   for (idx_t col = 0; col < parse_chunk.column_count(); col++) {
    vector<LogicalType> &col_type_candidates = best_sql_types_candidates[col];
    while (col_type_candidates.size() > 1) {
     const auto &sql_type = col_type_candidates.back();
     if (best_format_candidates.count(sql_type.id())) {
      auto &best_type_format_candidates = best_format_candidates[sql_type.id()];
      auto save_format_candidates = best_type_format_candidates;
      while (best_type_format_candidates.size()) {
       if (TryCastVector(parse_chunk.data[col], parse_chunk.size(), sql_type)) {
        break;
       }
       best_type_format_candidates.pop_back();
       options.has_format[sql_type.id()] = (best_type_format_candidates.size() > 0);
       if (best_type_format_candidates.size() > 0) {
        SetDateFormat(best_type_format_candidates.back(), sql_type.id());
       }
      }
      if (!best_type_format_candidates.size()) {
       best_type_format_candidates.swap(save_format_candidates);
       if (best_type_format_candidates.size()) {
        SetDateFormat(best_type_format_candidates.back(), sql_type.id());
       }
      }
     }
     if (TryCastVector(parse_chunk.data[col], parse_chunk.size(), sql_type)) {
      break;
     } else {
      col_type_candidates.pop_back();
     }
    }
   }
  }
  for (idx_t col = 0; col < best_sql_types_candidates.size(); col++) {
   LogicalType d_type = best_sql_types_candidates[col].back();
   detected_types.push_back(d_type);
  }
 }
 bool first_row_consistent = true;
 bool first_row_nulls = false;
 JumpToBeginning(options.skip_rows, false);
 ParseCSV(ParserMode::SNIFFING_DATATYPES);
 if (parse_chunk.size() > 1) {
  first_row_nulls = true;
  for (idx_t col = 0; col < parse_chunk.column_count(); col++) {
   auto dummy_val = parse_chunk.GetValue(col, 0);
   try {
    dummy_val.CastAs(LogicalType::SQLNULL, true);
   } catch (const Exception &e) {
    first_row_nulls = false;
   }
   const auto &sql_type = detected_types[col];
   if (!TryCastValue(dummy_val, sql_type)) {
    first_row_consistent = false;
   }
  }
 }
 if (((!first_row_consistent || first_row_nulls) && !options.has_header) || (options.has_header && options.header)) {
  options.header = true;
  vector<string> t_col_names;
  for (idx_t col = 0; col < parse_chunk.column_count(); col++) {
   const auto &val = parse_chunk.GetValue(col, 0);
   string col_name = val.ToString();
   if (col_name.empty() || val.is_null) {
    col_name = GenerateColumnName(parse_chunk.column_count(), col);
   }
   t_col_names.push_back(col_name);
  }
  for (idx_t col = 0; col < t_col_names.size(); col++) {
   string col_name = t_col_names[col];
   idx_t exists_n_times = std::count(t_col_names.begin(), t_col_names.end(), col_name);
   idx_t exists_n_times_before = std::count(t_col_names.begin(), t_col_names.begin() + col, col_name);
   if (exists_n_times > 1) {
    col_name = GenerateColumnName(exists_n_times, exists_n_times_before, col_name + "_");
   }
   col_names.push_back(col_name);
  }
 } else {
  options.header = false;
  idx_t total_columns = parse_chunk.column_count();
  for (idx_t col = 0; col < total_columns; col++) {
   string column_name = GenerateColumnName(total_columns, col);
   col_names.push_back(column_name);
  }
 }
 JumpToBeginning(0, false);
 return detected_types;
}
void BufferedCSVReader::ParseComplexCSV(DataChunk &insert_chunk) {
 bool finished_chunk = false;
 idx_t column = 0;
 vector<idx_t> escape_positions;
 uint8_t delimiter_pos = 0, escape_pos = 0, quote_pos = 0;
 idx_t offset = 0;
 if (position >= buffer_size) {
  if (!ReadBuffer(start)) {
   return;
  }
 }
 start = position;
 goto value_start;
value_start:
 offset = 0;
 delimiter_pos = 0;
 quote_pos = 0;
 do {
  idx_t count = 0;
  for (; position < buffer_size; position++) {
   quote_search.Match(quote_pos, buffer[position]);
   delimiter_search.Match(delimiter_pos, buffer[position]);
   count++;
   if (delimiter_pos == options.delimiter.size()) {
    offset = options.delimiter.size() - 1;
    goto add_value;
   } else if (is_newline(buffer[position])) {
    goto add_row;
   }
   if (count > quote_pos) {
    goto normal;
   }
   if (quote_pos == options.quote.size()) {
    start += options.quote.size();
    goto in_quotes;
   }
  }
 } while (ReadBuffer(start));
 goto final_state;
normal:
 position++;
 do {
  for (; position < buffer_size; position++) {
   delimiter_search.Match(delimiter_pos, buffer[position]);
   if (delimiter_pos == options.delimiter.size()) {
    offset = options.delimiter.size() - 1;
    goto add_value;
   } else if (is_newline(buffer[position])) {
    goto add_row;
   }
  }
 } while (ReadBuffer(start));
 goto final_state;
add_value:
 AddValue(buffer.get() + start, position - start - offset, column, escape_positions);
 offset = 0;
 start = ++position;
 if (position >= buffer_size && !ReadBuffer(start)) {
  goto final_state;
 }
 goto value_start;
add_row : {
 bool carriage_return = buffer[position] == '\r';
 AddValue(buffer.get() + start, position - start - offset, column, escape_positions);
 finished_chunk = AddRow(insert_chunk, column);
 offset = 0;
 start = ++position;
 if (position >= buffer_size && !ReadBuffer(start)) {
  goto final_state;
 }
 if (carriage_return) {
  goto carriage_return;
 } else {
  if (finished_chunk) {
   return;
  }
  goto value_start;
 }
}
in_quotes:
 quote_pos = 0;
 escape_pos = 0;
 position++;
 do {
  for (; position < buffer_size; position++) {
   quote_search.Match(quote_pos, buffer[position]);
   escape_search.Match(escape_pos, buffer[position]);
   if (quote_pos == options.quote.size()) {
    goto unquote;
   } else if (escape_pos == options.escape.size()) {
    escape_positions.push_back(position - start - (options.escape.size() - 1));
    goto handle_escape;
   }
  }
 } while (ReadBuffer(start));
 throw ParserException("Error in file \"%s\" on line %s: unterminated quotes", options.file_path,
                       GetLineNumberStr(linenr, linenr_estimated).c_str());
unquote:
 delimiter_pos = 0;
 quote_pos = 0;
 position++;
 if (position >= buffer_size && !ReadBuffer(start)) {
  offset = options.quote.size();
  goto final_state;
 }
 if (is_newline(buffer[position])) {
  offset = options.quote.size();
  goto add_row;
 }
 do {
  idx_t count = 0;
  for (; position < buffer_size; position++) {
   quote_search.Match(quote_pos, buffer[position]);
   delimiter_search.Match(delimiter_pos, buffer[position]);
   count++;
   if (count > delimiter_pos && count > quote_pos) {
    throw ParserException("Error in file \"%s\" on line %s: quote should be followed by end of value, end "
                          "of row or another quote",
                          options.file_path, GetLineNumberStr(linenr, linenr_estimated).c_str());
   }
   if (delimiter_pos == options.delimiter.size()) {
    offset = options.quote.size() + options.delimiter.size() - 1;
    goto add_value;
   } else if (quote_pos == options.quote.size() &&
              (options.escape.size() == 0 || options.escape == options.quote)) {
    escape_positions.push_back(position - start - (options.quote.size() - 1));
    goto in_quotes;
   }
  }
 } while (ReadBuffer(start));
 throw ParserException(
     "Error in file \"%s\" on line %s: quote should be followed by end of value, end of row or another quote",
     options.file_path, GetLineNumberStr(linenr, linenr_estimated).c_str());
handle_escape:
 escape_pos = 0;
 quote_pos = 0;
 position++;
 do {
  idx_t count = 0;
  for (; position < buffer_size; position++) {
   quote_search.Match(quote_pos, buffer[position]);
   escape_search.Match(escape_pos, buffer[position]);
   count++;
   if (count > escape_pos && count > quote_pos) {
    throw ParserException(
        "Error in file \"%s\" on line %s: neither QUOTE nor ESCAPE is proceeded by ESCAPE",
        options.file_path, GetLineNumberStr(linenr, linenr_estimated).c_str());
   }
   if (quote_pos == options.quote.size() || escape_pos == options.escape.size()) {
    goto in_quotes;
   }
  }
 } while (ReadBuffer(start));
 throw ParserException("Error in file \"%s\" on line %s: neither QUOTE nor ESCAPE is proceeded by ESCAPE",
                       options.file_path, GetLineNumberStr(linenr, linenr_estimated).c_str());
carriage_return:
 if (buffer[position] == '\n') {
  start = ++position;
  if (position >= buffer_size && !ReadBuffer(start)) {
   goto final_state;
  }
 }
 if (finished_chunk) {
  return;
 }
 goto value_start;
final_state:
 if (finished_chunk) {
  return;
 }
 if (column > 0 || position > start) {
  AddValue(buffer.get() + start, position - start - offset, column, escape_positions);
  finished_chunk = AddRow(insert_chunk, column);
 }
 if (mode == ParserMode::PARSING) {
  Flush(insert_chunk);
 }
 end_of_file_reached = true;
}
void BufferedCSVReader::ParseSimpleCSV(DataChunk &insert_chunk) {
 bool finished_chunk = false;
 idx_t column = 0;
 idx_t offset = 0;
 vector<idx_t> escape_positions;
 if (position >= buffer_size) {
  if (!ReadBuffer(start)) {
   return;
  }
 }
 goto value_start;
value_start:
 offset = 0;
 if (buffer[position] == options.quote[0]) {
  start = position + 1;
  goto in_quotes;
 } else {
  start = position;
  goto normal;
 }
normal:
 do {
  for (; position < buffer_size; position++) {
   if (buffer[position] == options.delimiter[0]) {
    goto add_value;
   } else if (is_newline(buffer[position])) {
    goto add_row;
   }
  }
 } while (ReadBuffer(start));
 goto final_state;
add_value:
 AddValue(buffer.get() + start, position - start - offset, column, escape_positions);
 offset = 0;
 start = ++position;
 if (position >= buffer_size && !ReadBuffer(start)) {
  goto final_state;
 }
 goto value_start;
add_row : {
 bool carriage_return = buffer[position] == '\r';
 AddValue(buffer.get() + start, position - start - offset, column, escape_positions);
 finished_chunk = AddRow(insert_chunk, column);
 offset = 0;
 start = ++position;
 if (position >= buffer_size && !ReadBuffer(start)) {
  goto final_state;
 }
 if (carriage_return) {
  goto carriage_return;
 } else {
  if (finished_chunk) {
   return;
  }
  goto value_start;
 }
}
in_quotes:
 position++;
 do {
  for (; position < buffer_size; position++) {
   if (buffer[position] == options.quote[0]) {
    goto unquote;
   } else if (buffer[position] == options.escape[0]) {
    escape_positions.push_back(position - start);
    goto handle_escape;
   }
  }
 } while (ReadBuffer(start));
 throw ParserException("Error in file \"%s\" on line %s: unterminated quotes", options.file_path,
                       GetLineNumberStr(linenr, linenr_estimated).c_str());
unquote:
 position++;
 if (position >= buffer_size && !ReadBuffer(start)) {
  offset = 1;
  goto final_state;
 }
 if (buffer[position] == options.quote[0] && (options.escape.size() == 0 || options.escape[0] == options.quote[0])) {
  escape_positions.push_back(position - start);
  goto in_quotes;
 } else if (buffer[position] == options.delimiter[0]) {
  offset = 1;
  goto add_value;
 } else if (is_newline(buffer[position])) {
  offset = 1;
  goto add_row;
 } else {
  throw ParserException(
      "Error in file \"%s\" on line %s: quote should be followed by end of value, end of row or another quote",
      options.file_path, GetLineNumberStr(linenr, linenr_estimated).c_str());
 }
handle_escape:
 position++;
 if (position >= buffer_size && !ReadBuffer(start)) {
  throw ParserException("Error in file \"%s\" on line %s: neither QUOTE nor ESCAPE is proceeded by ESCAPE",
                        options.file_path, GetLineNumberStr(linenr, linenr_estimated).c_str());
 }
 if (buffer[position] != options.quote[0] && buffer[position] != options.escape[0]) {
  throw ParserException("Error in file \"%s\" on line %s: neither QUOTE nor ESCAPE is proceeded by ESCAPE",
                        options.file_path, GetLineNumberStr(linenr, linenr_estimated).c_str());
 }
 goto in_quotes;
carriage_return:
 if (buffer[position] == '\n') {
  start = ++position;
  if (position >= buffer_size && !ReadBuffer(start)) {
   goto final_state;
  }
 }
 if (finished_chunk) {
  return;
 }
 goto value_start;
final_state:
 if (finished_chunk) {
  return;
 }
 if (column > 0 || position > start) {
  AddValue(buffer.get() + start, position - start - offset, column, escape_positions);
  finished_chunk = AddRow(insert_chunk, column);
 }
 if (mode == ParserMode::PARSING) {
  Flush(insert_chunk);
 }
 end_of_file_reached = true;
}
bool BufferedCSVReader::ReadBuffer(idx_t &start) {
 auto old_buffer = move(buffer);
 idx_t remaining = buffer_size - start;
 idx_t buffer_read_size = INITIAL_BUFFER_SIZE;
 while (remaining > buffer_read_size) {
  buffer_read_size *= 2;
 }
 if (remaining + buffer_read_size > MAXIMUM_CSV_LINE_SIZE) {
  throw ParserException("Maximum line size of %llu bytes exceeded!", MAXIMUM_CSV_LINE_SIZE);
 }
 buffer = unique_ptr<char[]>(new char[buffer_read_size + remaining + 1]);
 buffer_size = remaining + buffer_read_size;
 if (remaining > 0) {
  memcpy(buffer.get(), old_buffer.get() + start, remaining);
 }
 source->read(buffer.get() + remaining, buffer_read_size);
 idx_t read_count = source->eof() ? source->gcount() : buffer_read_size;
 bytes_in_chunk += read_count;
 buffer_size = remaining + read_count;
 buffer[buffer_size] = '\0';
 if (old_buffer) {
  cached_buffers.push_back(move(old_buffer));
 }
 start = 0;
 position = remaining;
 return read_count > 0;
}
void BufferedCSVReader::ParseCSV(DataChunk &insert_chunk) {
 cached_buffers.clear();
 ParseCSV(ParserMode::PARSING, insert_chunk);
}
void BufferedCSVReader::ParseCSV(ParserMode parser_mode, DataChunk &insert_chunk) {
 mode = parser_mode;
 if (options.quote.size() <= 1 && options.escape.size() <= 1 && options.delimiter.size() == 1) {
  ParseSimpleCSV(insert_chunk);
 } else {
  ParseComplexCSV(insert_chunk);
 }
}
void BufferedCSVReader::AddValue(char *str_val, idx_t length, idx_t &column, vector<idx_t> &escape_positions) {
 if (sql_types.size() > 0 && column == sql_types.size() && length == 0) {
  return;
 }
 if (mode == ParserMode::SNIFFING_DIALECT) {
  column++;
  return;
 }
 if (column >= sql_types.size()) {
  throw ParserException("Error on line %s: expected %lld values but got %d",
                        GetLineNumberStr(linenr, linenr_estimated).c_str(), sql_types.size(), column + 1);
 }
 idx_t row_entry = parse_chunk.size();
 str_val[length] = '\0';
 if (!options.force_not_null[column] && strcmp(options.null_str.c_str(), str_val) == 0) {
  FlatVector::SetNull(parse_chunk.data[column], row_entry, true);
 } else {
  auto &v = parse_chunk.data[column];
  auto parse_data = FlatVector::GetData<string_t>(v);
  if (escape_positions.size() > 0) {
   string old_val = str_val;
   string new_val = "";
   idx_t prev_pos = 0;
   for (idx_t i = 0; i < escape_positions.size(); i++) {
    idx_t next_pos = escape_positions[i];
    new_val += old_val.substr(prev_pos, next_pos - prev_pos);
    if (options.escape.size() == 0 || options.escape == options.quote) {
     prev_pos = next_pos + options.quote.size();
    } else {
     prev_pos = next_pos + options.escape.size();
    }
   }
   new_val += old_val.substr(prev_pos, old_val.size() - prev_pos);
   escape_positions.clear();
   parse_data[row_entry] = StringVector::AddStringOrBlob(v, string_t(new_val));
  } else {
   parse_data[row_entry] = string_t(str_val, length);
  }
 }
 column++;
}
bool BufferedCSVReader::AddRow(DataChunk &insert_chunk, idx_t &column) {
 linenr++;
 if (column < sql_types.size() && mode != ParserMode::SNIFFING_DIALECT) {
  throw ParserException("Error on line %s: expected %lld values but got %d",
                        GetLineNumberStr(linenr, linenr_estimated).c_str(), sql_types.size(), column);
 }
 if (mode == ParserMode::SNIFFING_DIALECT) {
  sniffed_column_counts.push_back(column);
  if (sniffed_column_counts.size() == SAMPLE_CHUNK_SIZE) {
   return true;
  }
 } else {
  parse_chunk.SetCardinality(parse_chunk.size() + 1);
 }
 if (mode == ParserMode::SNIFFING_DATATYPES && parse_chunk.size() == SAMPLE_CHUNK_SIZE) {
  return true;
 }
 if (mode == ParserMode::PARSING && parse_chunk.size() == STANDARD_VECTOR_SIZE) {
  Flush(insert_chunk);
  return true;
 }
 column = 0;
 return false;
}
void BufferedCSVReader::Flush(DataChunk &insert_chunk) {
 if (parse_chunk.size() == 0) {
  return;
 }
 insert_chunk.SetCardinality(parse_chunk);
 for (idx_t col_idx = 0; col_idx < sql_types.size(); col_idx++) {
  if (sql_types[col_idx].id() == LogicalTypeId::VARCHAR) {
   auto parse_data = FlatVector::GetData<string_t>(parse_chunk.data[col_idx]);
   for (idx_t i = 0; i < parse_chunk.size(); i++) {
    if (!FlatVector::IsNull(parse_chunk.data[col_idx], i)) {
     auto s = parse_data[i];
     auto utf_type = Utf8Proc::Analyze(s.GetData(), s.GetSize());
     if (utf_type == UnicodeType::INVALID) {
      throw ParserException("Error in file \"%s\" between line %d and %d: file is not valid UTF8",
                            options.file_path, linenr - parse_chunk.size(), linenr);
     }
    }
   }
   insert_chunk.data[col_idx].Reference(parse_chunk.data[col_idx]);
  } else if (options.has_format[LogicalTypeId::DATE] && sql_types[col_idx].id() == LogicalTypeId::DATE) {
   try {
    UnaryExecutor::Execute<string_t, date_t, true>(
        parse_chunk.data[col_idx], insert_chunk.data[col_idx], parse_chunk.size(),
        [&](string_t input) { return options.date_format[LogicalTypeId::DATE].ParseDate(input); });
   } catch (const Exception &e) {
    throw ParserException("Error in file \"%s\" between line %llu and %llu: %s", options.file_path,
                          linenr - parse_chunk.size(), linenr, e.what());
   }
  } else if (options.has_format[LogicalTypeId::TIMESTAMP] &&
             sql_types[col_idx].id() == LogicalTypeId::TIMESTAMP) {
   try {
    UnaryExecutor::Execute<string_t, timestamp_t, true>(
        parse_chunk.data[col_idx], insert_chunk.data[col_idx], parse_chunk.size(), [&](string_t input) {
         return options.date_format[LogicalTypeId::TIMESTAMP].ParseTimestamp(input);
        });
   } catch (const Exception &e) {
    throw ParserException("Error in file \"%s\" between line %llu and %llu: %s", options.file_path,
                          linenr - parse_chunk.size(), linenr, e.what());
   }
  } else {
   try {
    VectorOperations::Cast(parse_chunk.data[col_idx], insert_chunk.data[col_idx], parse_chunk.size());
   } catch (const Exception &e) {
    throw ParserException("Error in file \"%s\" between line %llu and %llu: %s", options.file_path,
                          linenr - parse_chunk.size(), linenr, e.what());
   }
  }
 }
 parse_chunk.Reset();
}
}
