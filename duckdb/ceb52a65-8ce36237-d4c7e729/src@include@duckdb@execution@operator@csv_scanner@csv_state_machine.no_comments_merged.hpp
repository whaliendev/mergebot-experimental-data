       
#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_buffer_manager.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_state_machine_cache.hpp"
namespace duckdb {
struct CSVStates {
 void Initialize(CSVState initial_state = CSVState::NOT_SET) {
  states[0] = initial_state;
  states[1] = initial_state;
 }
 inline bool NewValue() const {
  return states[1] == CSVState::DELIMITER;
 }
 inline bool NewRow() const {
  return states[0] != CSVState::RECORD_SEPARATOR && states[0] != CSVState::CARRIAGE_RETURN &&
         (states[1] == CSVState::RECORD_SEPARATOR || states[1] == CSVState::CARRIAGE_RETURN);
 }
 inline bool WasStandard() const {
  return states[0] == CSVState::STANDARD;
 }
 inline bool EmptyLastValue() const {
  return (states[0] == CSVState::DELIMITER &&
          (states[1] == CSVState::RECORD_SEPARATOR || states[1] == CSVState::CARRIAGE_RETURN ||
           states[1] == CSVState::DELIMITER)) ||
         (states[0] == CSVState::STANDARD && states[1] == CSVState::DELIMITER);
 }
 inline bool EmptyLine() const {
  return (states[1] == CSVState::CARRIAGE_RETURN || states[1] == CSVState::RECORD_SEPARATOR) &&
         (states[0] == CSVState::RECORD_SEPARATOR || states[0] == CSVState::NOT_SET);
 }
 inline bool IsNotSet() const {
  return states[1] == CSVState::NOT_SET;
 }
 inline bool IsComment() const {
  return states[1] == CSVState::COMMENT;
 }
 inline bool IsCurrentNewRow() const {
  return states[1] == CSVState::RECORD_SEPARATOR || states[1] == CSVState::CARRIAGE_RETURN;
 }
 inline bool IsCarriageReturn() const {
  return states[1] == CSVState::CARRIAGE_RETURN;
 }
 inline bool IsInvalid() const {
  return states[1] == CSVState::INVALID;
 }
 inline bool IsQuoted() const {
  return states[0] == CSVState::QUOTED;
 }
 inline bool IsEscaped() const {
  switch (states[1]) {
  case CSVState::ESCAPE:
  case CSVState::UNQUOTED_ESCAPE:
  case CSVState::ESCAPED_RETURN:
   return true;
  case CSVState::QUOTED:
   return states[0] == CSVState::UNQUOTED;
  default:
   return false;
  }
 }
 inline bool IsQuotedCurrent() const {
  return states[1] == CSVState::QUOTED || states[1] == CSVState::QUOTED_NEW_LINE;
 }
 inline bool IsState(const CSVState state) const {
  return states[1] == state;
 }
 inline bool WasState(const CSVState state) const {
  return states[0] == state;
 }
 CSVState states[2];
};
class CSVStateMachine {
public:
 explicit CSVStateMachine(CSVReaderOptions &options_p, const CSVStateMachineOptions &state_machine_options,
                          CSVStateMachineCache &csv_state_machine_cache_p);
 explicit CSVStateMachine(const StateMachine &transition_array, const CSVReaderOptions &options);
 inline void Transition(CSVStates &states, char current_char) const {
  states.states[0] = states.states[1];
  states.states[1] = transition_array[static_cast<uint8_t>(current_char)][static_cast<uint8_t>(states.states[1])];
 }
 void Print() const {
  std::cout << "State Machine Options" << '\n';
  std::cout << "Delim: " << state_machine_options.delimiter.GetValue() << '\n';
  std::cout << "Quote: " << state_machine_options.quote.GetValue() << '\n';
  std::cout << "Escape: " << state_machine_options.escape.GetValue() << '\n';
  std::cout << "Comment: " << state_machine_options.comment.GetValue() << '\n';
  std::cout << "---------------------" << '\n';
 }
 const StateMachine &transition_array;
 const CSVStateMachineOptions state_machine_options;
 const CSVReaderOptions &options;
 DialectOptions dialect_options;
};
}
