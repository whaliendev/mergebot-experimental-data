       
#include "duckdb/common/constants.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/common/vector.hpp"
#include <cstring>
namespace duckdb {
#ifndef DUCKDB_QUOTE_DEFINE
#define DUCKDB_QUOTE_DEFINE_IMPL(x) #x
#define DUCKDB_QUOTE_DEFINE(x) DUCKDB_QUOTE_DEFINE_IMPL(x)
#endif
class StringUtil {
public:
 static string GenerateRandomName(idx_t length = 16);
 static uint8_t GetHexValue(char c) {
  if (c >= '0' && c <= '9') {
   return UnsafeNumericCast<uint8_t>(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
   return UnsafeNumericCast<uint8_t>(c - 'a' + 10);
  }
  if (c >= 'A' && c <= 'F') {
   return UnsafeNumericCast<uint8_t>(c - 'A' + 10);
  }
  throw InvalidInputException("Invalid input for hex digit: %s", string(1, c));
 }
 static uint8_t GetBinaryValue(char c) {
  if (c >= '0' && c <= '1') {
   return UnsafeNumericCast<uint8_t>(c - '0');
  }
  throw InvalidInputException("Invalid input for binary digit: %s", string(1, c));
 }
 static bool CharacterIsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
 }
 static bool CharacterIsNewline(char c) {
  return c == '\n' || c == '\r';
 }
 static bool CharacterIsDigit(char c) {
  return c >= '0' && c <= '9';
 }
 static bool CharacterIsHex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
 }
 static char CharacterToUpper(char c) {
  if (c >= 'a' && c <= 'z') {
   return UnsafeNumericCast<char>(c - ('a' - 'A'));
  }
  return c;
 }
 static char CharacterToLower(char c) {
  if (c >= 'A' && c <= 'Z') {
   return UnsafeNumericCast<char>(c + ('a' - 'A'));
  }
  return c;
 }
 static bool CharacterIsAlpha(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
 }
 static bool CharacterIsOperator(char c) {
  if (c == '_') {
   return false;
  }
  if (c >= '!' && c <= '/') {
   return true;
  }
  if (c >= ':' && c <= '@') {
   return true;
  }
  if (c >= '[' && c <= '`') {
   return true;
  }
  if (c >= '{' && c <= '~') {
   return true;
  }
  return false;
 }
 template <class TO>
 static vector<TO> ConvertStrings(const vector<string> &strings) {
  vector<TO> result;
  for (auto &string : strings) {
   result.emplace_back(string);
  }
  return result;
 }
 static vector<SQLIdentifier> ConvertToSQLIdentifiers(const vector<string> &strings) {
  return ConvertStrings<SQLIdentifier>(strings);
 }
 static vector<SQLString> ConvertToSQLStrings(const vector<string> &strings) {
  return ConvertStrings<SQLString>(strings);
 }
 DUCKDB_API static bool Contains(const string &haystack, const string &needle);
 DUCKDB_API static bool StartsWith(string str, string prefix);
 DUCKDB_API static bool EndsWith(const string &str, const string &suffix);
 DUCKDB_API static string Repeat(const string &str, const idx_t n);
 DUCKDB_API static vector<string> Split(const string &str, char delimiter);
 DUCKDB_API static vector<string> SplitWithQuote(const string &str, char delimiter = ',', char quote = '"');
 DUCKDB_API static string Join(const vector<string> &input, const string &separator);
 DUCKDB_API static string Join(const set<string> &input, const string &separator);
 DUCKDB_API static string URLEncode(const string &str, bool encode_slash = true);
 DUCKDB_API static idx_t URLEncodeSize(const char *input, idx_t input_size, bool encode_slash = true);
 DUCKDB_API static void URLEncodeBuffer(const char *input, idx_t input_size, char *output, bool encode_slash = true);
 DUCKDB_API static string URLDecode(const string &str, bool plus_to_space = false);
 DUCKDB_API static idx_t URLDecodeSize(const char *input, idx_t input_size, bool plus_to_space = false);
 DUCKDB_API static void URLDecodeBuffer(const char *input, idx_t input_size, char *output,
                                        bool plus_to_space = false);
 template <class T>
 static string ToString(const vector<T> &input, const string &separator) {
  vector<string> input_list;
  for (auto &i : input) {
   input_list.push_back(i.ToString());
  }
  return StringUtil::Join(input_list, separator);
 }
 template <typename C, typename S, typename FUNC>
 static string Join(const C &input, S count, const string &separator, FUNC f) {
  std::string result;
  if (count > 0) {
   result += f(input[0]);
  }
  for (size_t i = 1; i < count; i++) {
   result += separator + f(input[i]);
  }
  return result;
 }
 DUCKDB_API static string BytesToHumanReadableString(idx_t bytes, idx_t multiplier = 1024);
 DUCKDB_API static string Upper(const string &str);
 DUCKDB_API static string Lower(const string &str);
 DUCKDB_API static string Title(const string &str);
 DUCKDB_API static bool IsLower(const string &str);
 DUCKDB_API static uint64_t CIHash(const string &str);
 DUCKDB_API static bool CIEquals(const string &l1, const string &l2);
 DUCKDB_API static bool CILessThan(const string &l1, const string &l2);
 DUCKDB_API static idx_t CIFind(vector<string> &vec, const string &str);
 template <typename... ARGS>
 static string Format(const string fmt_str, ARGS... params) {
  return Exception::ConstructMessage(fmt_str, params...);
 }
 DUCKDB_API static vector<string> Split(const string &input, const string &split);
 DUCKDB_API static void LTrim(string &str);
 DUCKDB_API static void RTrim(string &str);
 DUCKDB_API static void RTrim(string &str, const string &chars_to_trim);
 DUCKDB_API static void Trim(string &str);
 DUCKDB_API static string Replace(string source, const string &from, const string &to);
 DUCKDB_API static idx_t LevenshteinDistance(const string &s1, const string &s2, idx_t not_equal_penalty = 1);
 DUCKDB_API static idx_t SimilarityScore(const string &s1, const string &s2);
 DUCKDB_API static double SimilarityRating(const string &s1, const string &s2);
 DUCKDB_API static vector<string> TopNStrings(vector<pair<string, double>> scores, idx_t n = 5,
                                              double threshold = 0.5);
 DUCKDB_API static vector<string> TopNStrings(const vector<pair<string, idx_t>> &scores, idx_t n = 5,
                                              idx_t threshold = 5);
 DUCKDB_API static vector<string> TopNLevenshtein(const vector<string> &strings, const string &target, idx_t n = 5,
                                                  idx_t threshold = 5);
 DUCKDB_API static vector<string> TopNJaroWinkler(const vector<string> &strings, const string &target, idx_t n = 5,
                                                  double threshold = 0.5);
 DUCKDB_API static string CandidatesMessage(const vector<string> &candidates,
                                            const string &candidate = "Candidate bindings");
 DUCKDB_API static string CandidatesErrorMessage(const vector<string> &strings, const string &target,
                                                 const string &message_prefix, idx_t n = 5);
 static bool Equals(const char *s1, const char *s2) {
  if (s1 == s2) {
   return true;
  }
  if (s1 == nullptr || s2 == nullptr) {
   return false;
  }
  return strcmp(s1, s2) == 0;
 }
 DUCKDB_API static unordered_map<string, string> ParseJSONMap(const string &json);
 DUCKDB_API static string ToJSONMap(ExceptionType type, const string &message,
                                    const unordered_map<string, string> &map);
 DUCKDB_API static string GetFileName(const string &file_path);
 DUCKDB_API static string GetFileExtension(const string &file_name);
 DUCKDB_API static string GetFileStem(const string &file_name);
 DUCKDB_API static string GetFilePath(const string &file_path);
<<<<<<< HEAD
 struct EnumStringLiteral {
  uint32_t number;
  const char *string;
 };
 DUCKDB_API static uint32_t StringToEnum(const EnumStringLiteral enum_list[], idx_t enum_count,
                                         const char *enum_name, const char *str_value);
 DUCKDB_API static const char *EnumToString(const EnumStringLiteral enum_list[], idx_t enum_count,
                                            const char *enum_name, uint32_t enum_value);
||||||| fb51e97b31
=======
 DUCKDB_API static const uint8_t ASCII_TO_LOWER_MAP[];
 DUCKDB_API static const uint8_t ASCII_TO_UPPER_MAP[];
>>>>>>> 8656b2cc
};
}
