       
#include <cstdint>
namespace duckdb {
enum class CSVState : uint8_t {
 STANDARD = 0,
 DELIMITER = 1,
 RECORD_SEPARATOR = 2,
 CARRIAGE_RETURN = 3,
 QUOTED = 4,
 UNQUOTED = 5,
 ESCAPE = 6,
 INVALID = 7,
 NOT_SET = 8,
 QUOTED_NEW_LINE = 9,
 EMPTY_SPACE = 10,
 COMMENT = 11,
 UNQUOTED_ESCAPE = 12,
 ESCAPED_RETURN = 13
 STANDARD_NEWLINE = 12,
};
}
