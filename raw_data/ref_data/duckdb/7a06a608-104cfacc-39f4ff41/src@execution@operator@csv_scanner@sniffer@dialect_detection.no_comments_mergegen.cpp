#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/execution/operator/csv_scanner/sniffer/csv_sniffer.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"
namespace duckdb {
constexpr idx_t CSVReaderOptions::sniff_size;
bool IsQuoteDefault(char quote) {
 if (quote == '\"' || quote == '\'' || quote == '\0') {
  return true;
 }
 return false;
}
vector<char> DialectCandidates::GetDefaultDelimiter() {
 return {',', '|', ';', '\t'};
}
vector<vector<char>> DialectCandidates::GetDefaultQuote() {
 return {{'\0'}, {'\"', '\''}, {'\"'}};
}
vector<QuoteRule> DialectCandidates::GetDefaultQuoteRule() {
 return {QuoteRule::NO_QUOTES, QuoteRule::QUOTES_OTHER, QuoteRule::QUOTES_RFC};
}
vector<vector<char>> DialectCandidates::GetDefaultEscape() {
 return {{'\0'}, {'\\'}, {'\"', '\0', '\''}};
}
vector<char> DialectCandidates::GetDefaultComment() {
 return {'#', '\0'};
}
string DialectCandidates::Print() {
 std::ostringstream search_space;
 search_space << "Delimiter Candidates: ";
 for (idx_t i = 0; i < delim_candidates.size(); i++) {
  search_space << "\'" << delim_candidates[i] << "\'";
  if (i < delim_candidates.size() - 1) {
   search_space << ", ";
  }
 }
 search_space << "\n";
 search_space << "Quote/Escape Candidates: ";
 for (uint8_t i = 0; i < static_cast<uint8_t>(quote_rule_candidates.size()); i++) {
  auto quote_candidate = quote_candidates_map[i];
  auto escape_candidate = escape_candidates_map[i];
  for (idx_t j = 0; j < quote_candidate.size(); j++) {
   for (idx_t k = 0; k < escape_candidate.size(); k++) {
    search_space << "[\'" << quote_candidate[j] << "\',\'" << escape_candidate[k] << "\']";
    if (k < escape_candidate.size() - 1) {
     search_space << ",";
    }
   }
   if (j < quote_candidate.size() - 1) {
    search_space << ",";
   }
  }
  if (i < quote_rule_candidates.size() - 1) {
   search_space << ",";
  }
 }
 search_space << "\n";
 search_space << "Comment Candidates: ";
 for (idx_t i = 0; i < comment_candidates.size(); i++) {
  search_space << "\'" << comment_candidates[i] << "\'";
  if (i < comment_candidates.size() - 1) {
   search_space << ", ";
  }
 }
 search_space << "\n";
 return search_space.str();
}
DialectCandidates::DialectCandidates(const CSVStateMachineOptions &options) {
 auto default_quote = GetDefaultQuote();
 auto default_escape = GetDefaultEscape();
 auto default_quote_rule = GetDefaultQuoteRule();
 auto default_delimiter = GetDefaultDelimiter();
 auto default_comment = GetDefaultComment();
 D_ASSERT(default_quote.size() == default_quote_rule.size() && default_quote_rule.size() == default_escape.size());
 for (idx_t i = 0; i < default_quote_rule.size(); i++) {
  escape_candidates_map[static_cast<uint8_t>(default_quote_rule[i])] = default_escape[i];
 }
 if (options.delimiter.IsSetByUser()) {
  delim_candidates = {options.delimiter.GetValue()};
 } else {
  delim_candidates = default_delimiter;
 }
 if (options.comment.IsSetByUser()) {
  comment_candidates = {options.comment.GetValue()};
 } else {
  comment_candidates = default_comment;
 }
 if (options.quote.IsSetByUser()) {
  for (auto &quote_rule : default_quote_rule) {
   quote_candidates_map[static_cast<uint8_t>(quote_rule)] = {options.quote.GetValue()};
  }
  if (!IsQuoteDefault(options.quote.GetValue())) {
   escape_candidates_map[static_cast<uint8_t>(QuoteRule::QUOTES_RFC)].emplace_back(options.quote.GetValue());
  }
 } else {
  for (idx_t i = 0; i < default_quote_rule.size(); i++) {
   quote_candidates_map[static_cast<uint8_t>(default_quote_rule[i])] = {default_quote[i]};
  }
 }
 if (options.escape.IsSetByUser()) {
  if (options.escape == '\0') {
   quote_rule_candidates = {QuoteRule::QUOTES_RFC};
  } else {
   quote_rule_candidates = {QuoteRule::QUOTES_OTHER};
  }
  escape_candidates_map[static_cast<uint8_t>(quote_rule_candidates[0])] = {options.escape.GetValue()};
 } else {
  quote_rule_candidates = default_quote_rule;
 }
}
void CSVSniffer::GenerateStateMachineSearchSpace(vector<unique_ptr<ColumnCountScanner>> &column_count_scanners,
                                                 const DialectCandidates &dialect_candidates) {
 NewLineIdentifier new_line_id;
 if (options.dialect_options.state_machine_options.new_line.IsSetByUser()) {
  new_line_id = options.dialect_options.state_machine_options.new_line.GetValue();
 } else {
  new_line_id = DetectNewLineDelimiter(*buffer_manager);
 }
 CSVIterator first_iterator;
 bool iterator_set = false;
 for (const auto quote_rule : dialect_candidates.quote_rule_candidates) {
  const auto &quote_candidates = dialect_candidates.quote_candidates_map.at(static_cast<uint8_t>(quote_rule));
  for (const auto &quote : quote_candidates) {
   for (const auto &delimiter : dialect_candidates.delim_candidates) {
    const auto &escape_candidates =
        dialect_candidates.escape_candidates_map.at(static_cast<uint8_t>(quote_rule));
    for (const auto &escape : escape_candidates) {
     for (const auto &comment : dialect_candidates.comment_candidates) {
      D_ASSERT(buffer_manager);
      CSVStateMachineOptions state_machine_options(delimiter, quote, escape, comment, new_line_id);
      auto sniffing_state_machine =
          make_shared_ptr<CSVStateMachine>(options, state_machine_options, state_machine_cache);
      if (options.dialect_options.skip_rows.IsSetByUser()) {
       if (!iterator_set) {
        first_iterator = BaseScanner::SkipCSVRows(buffer_manager, sniffing_state_machine,
                                                  options.dialect_options.skip_rows.GetValue());
        iterator_set = true;
       }
       column_count_scanners.emplace_back(make_uniq<ColumnCountScanner>(
           buffer_manager, std::move(sniffing_state_machine), detection_error_handler,
           CSVReaderOptions::sniff_size, first_iterator));
       continue;
      }
      column_count_scanners.emplace_back(
          make_uniq<ColumnCountScanner>(buffer_manager, std::move(sniffing_state_machine),
                                        detection_error_handler, CSVReaderOptions::sniff_size));
     }
    }
   }
  }
 }
}
bool AreCommentsAcceptable(const ColumnCountResult &result, idx_t num_cols, bool comment_set_by_user) {
 constexpr double min_majority = 0.6;
 double detected_comments = 0;
 bool has_full_line_comment = false;
 double valid_comments = 0;
 for (idx_t i = 0; i < result.result_position; i++) {
  if (result.column_counts[i].is_comment || result.column_counts[i].is_mid_comment) {
   detected_comments++;
   if (result.column_counts[i].number_of_columns != num_cols && result.column_counts[i].is_comment) {
    has_full_line_comment = true;
    valid_comments++;
   }
   if (result.column_counts[i].number_of_columns == num_cols && result.column_counts[i].is_mid_comment) {
    valid_comments++;
   }
  }
 }
 if (valid_comments == 0 || (!has_full_line_comment && !comment_set_by_user)) {
  if (result.state_machine.state_machine_options.comment.GetValue() == '\0') {
   return true;
  }
  return false;
 }
 return valid_comments / detected_comments >= min_majority;
}
void CSVSniffer::AnalyzeDialectCandidate(unique_ptr<ColumnCountScanner> scanner, idx_t &rows_read,
                                         idx_t &best_consistent_rows, idx_t &prev_padding_count,
                                         idx_t &min_ignored_rows) {
 auto &sniffed_column_counts = scanner->ParseChunk();
 idx_t dirty_notes = 0;
 idx_t dirty_notes_minus_comments = 0;
 if (sniffed_column_counts.error) {
  return;
 }
 idx_t consistent_rows = 0;
 idx_t num_cols = sniffed_column_counts.result_position == 0 ? 1 : sniffed_column_counts[0].number_of_columns;
 const bool ignore_errors = options.ignore_errors.GetValue();
 bool use_most_frequent_columns = ignore_errors && !options.null_padding;
 if (use_most_frequent_columns) {
  num_cols = sniffed_column_counts.GetMostFrequentColumnCount();
 }
 idx_t padding_count = 0;
 idx_t comment_rows = 0;
 idx_t ignored_rows = 0;
 bool allow_padding = options.null_padding;
 bool first_valid = false;
 if (sniffed_column_counts.result_position > rows_read) {
  rows_read = sniffed_column_counts.result_position;
 }
 if (set_columns.IsCandidateUnacceptable(num_cols, options.null_padding, ignore_errors,
                                         sniffed_column_counts[0].last_value_always_empty)) {
  return;
 }
 idx_t header_idx = 0;
 for (idx_t row = 0; row < sniffed_column_counts.result_position; row++) {
  if (set_columns.IsCandidateUnacceptable(sniffed_column_counts[row].number_of_columns, options.null_padding,
                                          ignore_errors, sniffed_column_counts[row].last_value_always_empty)) {
   return;
  }
  if (sniffed_column_counts[row].is_comment) {
   comment_rows++;
  } else if (sniffed_column_counts[row].last_value_always_empty &&
             sniffed_column_counts[row].number_of_columns ==
                 sniffed_column_counts[header_idx].number_of_columns + 1) {
   consistent_rows++;
  } else if (num_cols < sniffed_column_counts[row].number_of_columns &&
             (!options.dialect_options.skip_rows.IsSetByUser() || comment_rows > 0) &&
             (!set_columns.IsSet() || options.null_padding) && (!first_valid || (!use_most_frequent_columns))) {
   if (!first_valid) {
    first_valid = true;
    sniffed_column_counts.state_machine.dialect_options.rows_until_header = row;
   }
   padding_count = 0;
   num_cols = sniffed_column_counts[row].number_of_columns;
   dirty_notes = row;
   dirty_notes_minus_comments = dirty_notes - comment_rows;
   header_idx = row;
   consistent_rows = 1;
  } else if (sniffed_column_counts[row].number_of_columns == num_cols || (use_most_frequent_columns)) {
   if (!first_valid) {
    first_valid = true;
    sniffed_column_counts.state_machine.dialect_options.rows_until_header = row;
    dirty_notes = row;
   }
   if (sniffed_column_counts[row].number_of_columns != num_cols) {
    ignored_rows++;
   }
   consistent_rows++;
  } else if (num_cols >= sniffed_column_counts[row].number_of_columns) {
   padding_count++;
  }
 }
 if (sniffed_column_counts.state_machine.options.dialect_options.skip_rows.IsSetByUser()) {
  sniffed_column_counts.state_machine.dialect_options.rows_until_header +=
      sniffed_column_counts.state_machine.options.dialect_options.skip_rows.GetValue();
 }
 consistent_rows += padding_count;
 bool more_values = consistent_rows > best_consistent_rows && num_cols >= max_columns_found;
 bool more_columns = consistent_rows == best_consistent_rows && num_cols > max_columns_found;
 bool require_more_padding = padding_count > prev_padding_count;
 bool require_less_padding = padding_count < prev_padding_count;
 bool single_column_before = max_columns_found < 2 && num_cols > max_columns_found * candidates.size();
 bool rows_consistent =
     consistent_rows + (dirty_notes_minus_comments - options.dialect_options.skip_rows.GetValue()) + comment_rows ==
     sniffed_column_counts.result_position - options.dialect_options.skip_rows.GetValue();
 bool more_than_one_row = consistent_rows > 1;
 bool more_than_one_column = num_cols > 1;
 bool start_good = !candidates.empty() &&
                   dirty_notes <= candidates.front()->GetStateMachine().dialect_options.skip_rows.GetValue();
 bool invalid_padding = !allow_padding && padding_count > 0;
 bool comments_are_acceptable = AreCommentsAcceptable(
     sniffed_column_counts, num_cols, options.dialect_options.state_machine_options.comment.IsSetByUser());
 bool quoted = scanner->ever_quoted &&
               sniffed_column_counts.state_machine.dialect_options.state_machine_options.quote.GetValue() != '\0';
 bool columns_match_set = num_cols == set_columns.Size() ||
                          (num_cols == set_columns.Size() + 1 && sniffed_column_counts[0].last_value_always_empty) ||
                          !set_columns.IsSet();
if (columns_match_set && rows_consistent && (single_column_before || ((more_values || more_columns) && !require_more_padding) || (more_than_one_column && require_less_padding) || quoted) && !invalid_padding && comments_are_acceptable) {
     !invalid_padding && comments_are_acceptable) {
  if (!candidates.empty() && set_columns.IsSet() && max_columns_found == set_columns.Size()) {
   if (candidates.front()->ever_quoted || !scanner->ever_quoted) {
    return;
   }
  }
  auto &sniffing_state_machine = scanner->GetStateMachine();
  if (!candidates.empty() && candidates.front()->ever_quoted) {
   if (!scanner->ever_quoted) {
    return;
   } else {
    if (!scanner->ever_escaped && candidates.front()->ever_escaped) {
     return;
    }
    if (best_consistent_rows == consistent_rows) {
     sniffing_state_machine.dialect_options.num_cols = num_cols;
     candidates.emplace_back(std::move(scanner));
     return;
    }
   }
  }
  if (max_columns_found == num_cols && ignored_rows > min_ignored_rows) {
   return;
  }
  best_consistent_rows = consistent_rows;
  max_columns_found = num_cols;
  prev_padding_count = padding_count;
  min_ignored_rows = ignored_rows;
  if (options.dialect_options.skip_rows.IsSetByUser()) {
   if (dirty_notes != 0 && !options.null_padding && !options.ignore_errors.GetValue() && comment_rows == 0) {
    return;
   }
   sniffing_state_machine.dialect_options.skip_rows = options.dialect_options.skip_rows.GetValue();
  } else if (!options.null_padding) {
   sniffing_state_machine.dialect_options.skip_rows = dirty_notes;
  }
  candidates.clear();
  sniffing_state_machine.dialect_options.num_cols = num_cols;
  candidates.emplace_back(std::move(scanner));
  return;
 }
 if (columns_match_set && more_than_one_row && more_than_one_column && start_good && rows_consistent &&
     !require_more_padding && !invalid_padding && num_cols == max_columns_found && comments_are_acceptable) {
  auto &sniffing_state_machine = scanner->GetStateMachine();
  bool same_quote_is_candidate = false;
  for (auto &candidate : candidates) {
   if (sniffing_state_machine.dialect_options.state_machine_options.quote ==
       candidate->GetStateMachine().dialect_options.state_machine_options.quote) {
    same_quote_is_candidate = true;
   }
  }
  if (!same_quote_is_candidate) {
   if (options.dialect_options.skip_rows.IsSetByUser()) {
    if (dirty_notes != 0 && !options.null_padding && !options.ignore_errors.GetValue()) {
     return;
    }
    sniffing_state_machine.dialect_options.skip_rows = options.dialect_options.skip_rows.GetValue();
   } else if (!options.null_padding) {
    sniffing_state_machine.dialect_options.skip_rows = dirty_notes;
   }
   sniffing_state_machine.dialect_options.num_cols = num_cols;
   candidates.emplace_back(std::move(scanner));
  }
 }
}
bool CSVSniffer::RefineCandidateNextChunk(ColumnCountScanner &candidate) const {
 auto &sniffed_column_counts = candidate.ParseChunk();
 for (idx_t i = 0; i < sniffed_column_counts.result_position; i++) {
  if (set_columns.IsSet()) {
   return !set_columns.IsCandidateUnacceptable(sniffed_column_counts[i].number_of_columns,
                                               options.null_padding, options.ignore_errors.GetValue(),
                                               sniffed_column_counts[i].last_value_always_empty);
  }
  if (max_columns_found != sniffed_column_counts[i].number_of_columns &&
      (!options.null_padding && !options.ignore_errors.GetValue() && !sniffed_column_counts[i].is_comment)) {
   return false;
  }
 }
 return true;
}
void CSVSniffer::RefineCandidates() {
 if (candidates.empty()) {
  return;
 }
 if (candidates.size() == 1 || candidates[0]->FinishedFile()) {
  return;
 }
 vector<unique_ptr<ColumnCountScanner>> successful_candidates;
 for (auto &cur_candidate : candidates) {
  for (idx_t i = 1; i <= options.sample_size_chunks; i++) {
   bool finished_file = cur_candidate->FinishedFile();
   if (finished_file || i == options.sample_size_chunks) {
    successful_candidates.push_back(std::move(cur_candidate));
    break;
   }
   if (!RefineCandidateNextChunk(*cur_candidate) || cur_candidate->GetResult().error) {
    break;
   }
  }
 }
 candidates.clear();
 if (!successful_candidates.empty()) {
  for (idx_t i = 0; i < successful_candidates.size(); i++) {
   unique_ptr<ColumnCountScanner> cc_best_candidate = std::move(successful_candidates[i]);
   if (cc_best_candidate->state_machine->state_machine_options.quote != '\0' &&
       cc_best_candidate->ever_quoted) {
    candidates.clear();
    candidates.push_back(std::move(cc_best_candidate));
    return;
   }
   candidates.push_back(std::move(cc_best_candidate));
  }
 }
}
NewLineIdentifier CSVSniffer::DetectNewLineDelimiter(CSVBufferManager &buffer_manager) {
 auto buffer = buffer_manager.GetBuffer(0);
 auto buffer_ptr = buffer->Ptr();
 bool carriage_return = false;
 bool n = false;
 for (idx_t i = 0; i < buffer->actual_size; i++) {
  if (buffer_ptr[i] == '\r') {
   carriage_return = true;
  } else if (buffer_ptr[i] == '\n') {
   n = true;
   break;
  } else if (carriage_return) {
   break;
  }
 }
 if (carriage_return && n) {
  return NewLineIdentifier::CARRY_ON;
 }
 if (carriage_return) {
  return NewLineIdentifier::SINGLE_R;
 }
 return NewLineIdentifier::SINGLE_N;
}
void CSVSniffer::DetectDialect() {
 DialectCandidates dialect_candidates(options.dialect_options.state_machine_options);
 idx_t rows_read = 0;
 idx_t best_consistent_rows = 0;
 idx_t prev_padding_count = 0;
 idx_t best_ignored_rows = 0;
 vector<unique_ptr<ColumnCountScanner>> csv_state_machines;
 GenerateStateMachineSearchSpace(csv_state_machines, dialect_candidates);
 for (auto &state_machine : csv_state_machines) {
  AnalyzeDialectCandidate(std::move(state_machine), rows_read, best_consistent_rows, prev_padding_count,
                          best_ignored_rows);
 }
 RefineCandidates();
 if (candidates.empty()) {
  auto error = CSVError::SniffingError(options, dialect_candidates.Print());
  error_handler->Error(error, true);
 }
}
}
