       
#include "duckdb/common/assert.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/single_thread_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include <limits>
namespace duckdb {
class Serializer;
class Deserializer;
class Value;
class TypeCatalogEntry;
class Vector;
class ClientContext;
struct hugeint_t {
public:
 uint64_t lower;
 int64_t upper;
public:
 DUCKDB_API hugeint_t() = default;
 DUCKDB_API hugeint_t(int64_t value);
 DUCKDB_API hugeint_t(const hugeint_t &rhs) = default;
 DUCKDB_API hugeint_t(hugeint_t &&rhs) = default;
 DUCKDB_API hugeint_t &operator=(const hugeint_t &rhs) = default;
 DUCKDB_API hugeint_t &operator=(hugeint_t &&rhs) = default;
 DUCKDB_API string ToString() const;
 DUCKDB_API bool operator==(const hugeint_t &rhs) const;
 DUCKDB_API bool operator!=(const hugeint_t &rhs) const;
 DUCKDB_API bool operator<=(const hugeint_t &rhs) const;
 DUCKDB_API bool operator<(const hugeint_t &rhs) const;
 DUCKDB_API bool operator>(const hugeint_t &rhs) const;
 DUCKDB_API bool operator>=(const hugeint_t &rhs) const;
 DUCKDB_API hugeint_t operator+(const hugeint_t &rhs) const;
 DUCKDB_API hugeint_t operator-(const hugeint_t &rhs) const;
 DUCKDB_API hugeint_t operator*(const hugeint_t &rhs) const;
 DUCKDB_API hugeint_t operator/(const hugeint_t &rhs) const;
 DUCKDB_API hugeint_t operator%(const hugeint_t &rhs) const;
 DUCKDB_API hugeint_t operator-() const;
 DUCKDB_API hugeint_t operator>>(const hugeint_t &rhs) const;
 DUCKDB_API hugeint_t operator<<(const hugeint_t &rhs) const;
 DUCKDB_API hugeint_t operator&(const hugeint_t &rhs) const;
 DUCKDB_API hugeint_t operator|(const hugeint_t &rhs) const;
 DUCKDB_API hugeint_t operator^(const hugeint_t &rhs) const;
 DUCKDB_API hugeint_t operator~() const;
 DUCKDB_API hugeint_t &operator+=(const hugeint_t &rhs);
 DUCKDB_API hugeint_t &operator-=(const hugeint_t &rhs);
 DUCKDB_API hugeint_t &operator*=(const hugeint_t &rhs);
 DUCKDB_API hugeint_t &operator/=(const hugeint_t &rhs);
 DUCKDB_API hugeint_t &operator%=(const hugeint_t &rhs);
 DUCKDB_API hugeint_t &operator>>=(const hugeint_t &rhs);
 DUCKDB_API hugeint_t &operator<<=(const hugeint_t &rhs);
 DUCKDB_API hugeint_t &operator&=(const hugeint_t &rhs);
 DUCKDB_API hugeint_t &operator|=(const hugeint_t &rhs);
 DUCKDB_API hugeint_t &operator^=(const hugeint_t &rhs);
};
struct string_t;
template <class T>
using child_list_t = std::vector<std::pair<std::string, T>>;
template <class T>
using buffer_ptr = shared_ptr<T>;
template <class T, typename... Args>
buffer_ptr<T> make_buffer(Args &&...args) {
 return make_shared<T>(std::forward<Args>(args)...);
}
struct list_entry_t {
 list_entry_t() = default;
 list_entry_t(uint64_t offset, uint64_t length) : offset(offset), length(length) {
 }
 uint64_t offset;
 uint64_t length;
};
using union_tag_t = uint8_t;
enum class PhysicalType : uint8_t {
 BOOL = 1,
 UINT8 = 2,
 INT8 = 3,
 UINT16 = 4,
 INT16 = 5,
 UINT32 = 6,
 INT32 = 7,
 UINT64 = 8,
 INT64 = 9,
 FLOAT = 11,
 DOUBLE = 12,
 INTERVAL = 21,
 LIST = 23,
 STRUCT = 24,
 MAP = 27,
 VARCHAR = 200,
 INT128 = 204,
 UNKNOWN = 205,
 BIT = 206,
 INVALID = 255
};
enum class LogicalTypeId : uint8_t {
 INVALID = 0,
 SQLNULL = 1,
 UNKNOWN = 2,
 ANY = 3,
 USER = 4,
 BOOLEAN = 10,
 TINYINT = 11,
 SMALLINT = 12,
 INTEGER = 13,
 BIGINT = 14,
 DATE = 15,
 TIME = 16,
 TIMESTAMP_SEC = 17,
 TIMESTAMP_MS = 18,
 TIMESTAMP = 19,
 TIMESTAMP_NS = 20,
 DECIMAL = 21,
 FLOAT = 22,
 DOUBLE = 23,
 CHAR = 24,
 VARCHAR = 25,
 BLOB = 26,
 INTERVAL = 27,
 UTINYINT = 28,
 USMALLINT = 29,
 UINTEGER = 30,
 UBIGINT = 31,
 TIMESTAMP_TZ = 32,
 TIME_TZ = 34,
 HUGEINT = 50,
 POINTER = 51,
 VALIDITY = 53,
 UUID = 54,
 STRUCT = 100,
 LIST = 101,
 MAP = 102,
 TABLE = 103,
 ENUM = 104,
 AGGREGATE_STATE = 105,
 LAMBDA = 106,
 UNION = 107
};
struct ExtraTypeInfo;
struct aggregate_state_t;
struct LogicalType {
 DUCKDB_API LogicalType();
 DUCKDB_API LogicalType(LogicalTypeId id);
 DUCKDB_API LogicalType(LogicalTypeId id, shared_ptr<ExtraTypeInfo> type_info);
 DUCKDB_API LogicalType(const LogicalType &other);
 DUCKDB_API LogicalType(LogicalType &&other) noexcept;
 DUCKDB_API ~LogicalType();
 inline LogicalTypeId id() const {
  return id_;
 }
 inline PhysicalType InternalType() const {
  return physical_type_;
 }
 inline const ExtraTypeInfo *AuxInfo() const {
  return type_info_.get();
 }
 inline void CopyAuxInfo(const LogicalType& other) {
  type_info_ = other.type_info_;
 }
 bool EqualTypeInfo(const LogicalType& rhs) const;
 inline LogicalType& operator=(const LogicalType &other) {
  id_ = other.id_;
  physical_type_ = other.physical_type_;
  type_info_ = other.type_info_;
  return *this;
 }
 inline LogicalType& operator=(LogicalType&& other) noexcept {
  id_ = other.id_;
  physical_type_ = other.physical_type_;
  type_info_ = move(other.type_info_);
  return *this;
 }
 DUCKDB_API bool operator==(const LogicalType &rhs) const;
 inline bool operator!=(const LogicalType &rhs) const {
  return !(*this == rhs);
 }
 DUCKDB_API void Serialize(Serializer &serializer) const;
 DUCKDB_API static LogicalType Deserialize(Deserializer &source);
 DUCKDB_API static bool TypeIsTimestamp(LogicalTypeId id) {
  return (id == LogicalTypeId::TIMESTAMP ||
    id == LogicalTypeId::TIMESTAMP_MS ||
    id == LogicalTypeId::TIMESTAMP_NS ||
    id == LogicalTypeId::TIMESTAMP_SEC ||
    id == LogicalTypeId::TIMESTAMP_TZ);
 }
 DUCKDB_API static bool TypeIsTimestamp(const LogicalType& type) {
  return TypeIsTimestamp(type.id());
 }
 DUCKDB_API string ToString() const;
 DUCKDB_API bool IsIntegral() const;
 DUCKDB_API bool IsNumeric() const;
 DUCKDB_API hash_t Hash() const;
 DUCKDB_API void SetAlias(string alias);
 DUCKDB_API bool HasAlias() const;
 DUCKDB_API string GetAlias() const;
 DUCKDB_API static LogicalType MaxLogicalType(const LogicalType &left, const LogicalType &right);
 DUCKDB_API static void SetCatalog(LogicalType &type, TypeCatalogEntry* catalog_entry);
 DUCKDB_API static TypeCatalogEntry* GetCatalog(const LogicalType &type);
 DUCKDB_API bool GetDecimalProperties(uint8_t &width, uint8_t &scale) const;
 DUCKDB_API void Verify() const;
 DUCKDB_API bool IsValid() const;
private:
 LogicalTypeId id_;
 PhysicalType physical_type_;
 shared_ptr<ExtraTypeInfo> type_info_;
private:
 PhysicalType GetInternalType();
public:
 static constexpr const LogicalTypeId SQLNULL = LogicalTypeId::SQLNULL;
 static constexpr const LogicalTypeId UNKNOWN = LogicalTypeId::UNKNOWN;
 static constexpr const LogicalTypeId BOOLEAN = LogicalTypeId::BOOLEAN;
 static constexpr const LogicalTypeId TINYINT = LogicalTypeId::TINYINT;
 static constexpr const LogicalTypeId UTINYINT = LogicalTypeId::UTINYINT;
 static constexpr const LogicalTypeId SMALLINT = LogicalTypeId::SMALLINT;
 static constexpr const LogicalTypeId USMALLINT = LogicalTypeId::USMALLINT;
 static constexpr const LogicalTypeId INTEGER = LogicalTypeId::INTEGER;
 static constexpr const LogicalTypeId UINTEGER = LogicalTypeId::UINTEGER;
 static constexpr const LogicalTypeId BIGINT = LogicalTypeId::BIGINT;
 static constexpr const LogicalTypeId UBIGINT = LogicalTypeId::UBIGINT;
 static constexpr const LogicalTypeId FLOAT = LogicalTypeId::FLOAT;
 static constexpr const LogicalTypeId DOUBLE = LogicalTypeId::DOUBLE;
 static constexpr const LogicalTypeId DATE = LogicalTypeId::DATE;
 static constexpr const LogicalTypeId TIMESTAMP = LogicalTypeId::TIMESTAMP;
 static constexpr const LogicalTypeId TIMESTAMP_S = LogicalTypeId::TIMESTAMP_SEC;
 static constexpr const LogicalTypeId TIMESTAMP_MS = LogicalTypeId::TIMESTAMP_MS;
 static constexpr const LogicalTypeId TIMESTAMP_NS = LogicalTypeId::TIMESTAMP_NS;
 static constexpr const LogicalTypeId TIME = LogicalTypeId::TIME;
 static constexpr const LogicalTypeId TIMESTAMP_TZ = LogicalTypeId::TIMESTAMP_TZ;
 static constexpr const LogicalTypeId TIME_TZ = LogicalTypeId::TIME_TZ;
 static constexpr const LogicalTypeId VARCHAR = LogicalTypeId::VARCHAR;
 static constexpr const LogicalTypeId ANY = LogicalTypeId::ANY;
 static constexpr const LogicalTypeId BLOB = LogicalTypeId::BLOB;
 static constexpr const LogicalTypeId INTERVAL = LogicalTypeId::INTERVAL;
 static constexpr const LogicalTypeId HUGEINT = LogicalTypeId::HUGEINT;
 static constexpr const LogicalTypeId UUID = LogicalTypeId::UUID;
 static constexpr const LogicalTypeId HASH = LogicalTypeId::UBIGINT;
 static constexpr const LogicalTypeId POINTER = LogicalTypeId::POINTER;
 static constexpr const LogicalTypeId TABLE = LogicalTypeId::TABLE;
 static constexpr const LogicalTypeId LAMBDA = LogicalTypeId::LAMBDA;
 static constexpr const LogicalTypeId INVALID = LogicalTypeId::INVALID;
 static constexpr const LogicalTypeId ROW_TYPE = LogicalTypeId::BIGINT;
 DUCKDB_API static LogicalType DECIMAL(int width, int scale);
 DUCKDB_API static LogicalType VARCHAR_COLLATION(string collation);
 DUCKDB_API static LogicalType LIST( LogicalType child);
 DUCKDB_API static LogicalType STRUCT( child_list_t<LogicalType> children);
 DUCKDB_API static LogicalType AGGREGATE_STATE(aggregate_state_t state_type);
 DUCKDB_API static LogicalType MAP( child_list_t<LogicalType> children);
 DUCKDB_API static LogicalType MAP(LogicalType key, LogicalType value);
 DUCKDB_API static LogicalType UNION( child_list_t<LogicalType> members);
 DUCKDB_API static LogicalType ENUM(const string &enum_name, Vector &ordered_data, idx_t size);
 DUCKDB_API static LogicalType DEDUP_POINTER_ENUM();
 DUCKDB_API static LogicalType USER(const string &user_type_name);
 DUCKDB_API static const vector<LogicalType> Numeric();
 DUCKDB_API static const vector<LogicalType> Integral();
 DUCKDB_API static const vector<LogicalType> AllTypes();
};
struct DecimalType {
 DUCKDB_API static uint8_t GetWidth(const LogicalType &type);
 DUCKDB_API static uint8_t GetScale(const LogicalType &type);
 DUCKDB_API static uint8_t MaxWidth();
};
struct StringType {
 DUCKDB_API static string GetCollation(const LogicalType &type);
};
struct ListType {
 DUCKDB_API static const LogicalType &GetChildType(const LogicalType &type);
};
struct UserType{
 DUCKDB_API static const string &GetTypeName(const LogicalType &type);
};
struct EnumType{
 DUCKDB_API static const string &GetTypeName(const LogicalType &type);
 DUCKDB_API static int64_t GetPos(const LogicalType &type, const string_t& key);
 DUCKDB_API static Vector &GetValuesInsertOrder(const LogicalType &type);
 DUCKDB_API static idx_t GetSize(const LogicalType &type);
 DUCKDB_API static const string GetValue(const Value &val);
 DUCKDB_API static void SetCatalog(LogicalType &type, TypeCatalogEntry* catalog_entry);
 DUCKDB_API static TypeCatalogEntry* GetCatalog(const LogicalType &type);
 DUCKDB_API static PhysicalType GetPhysicalType(const LogicalType &type);
};
struct StructType {
 DUCKDB_API static const child_list_t<LogicalType> &GetChildTypes(const LogicalType &type);
 DUCKDB_API static const LogicalType &GetChildType(const LogicalType &type, idx_t index);
 DUCKDB_API static const string &GetChildName(const LogicalType &type, idx_t index);
 DUCKDB_API static idx_t GetChildCount(const LogicalType &type);
};
struct MapType {
 DUCKDB_API static const LogicalType &KeyType(const LogicalType &type);
 DUCKDB_API static const LogicalType &ValueType(const LogicalType &type);
};
struct UnionType {
 DUCKDB_API static const idx_t MAX_UNION_MEMBERS = 256;
 DUCKDB_API static idx_t GetMemberCount(const LogicalType &type);
 DUCKDB_API static const LogicalType &GetMemberType(const LogicalType &type, idx_t index);
 DUCKDB_API static const string &GetMemberName(const LogicalType &type, idx_t index);
 DUCKDB_API static const child_list_t<LogicalType> CopyMemberTypes(const LogicalType &type);
};
struct AggregateStateType {
 DUCKDB_API static const string GetTypeName(const LogicalType &type);
 DUCKDB_API static const aggregate_state_t &GetStateType(const LogicalType &type);
};
DUCKDB_API string LogicalTypeIdToString(LogicalTypeId type);
DUCKDB_API LogicalTypeId TransformStringToLogicalTypeId(const string &str);
DUCKDB_API LogicalType TransformStringToLogicalType(const string &str);
DUCKDB_API LogicalType TransformStringToLogicalType(const string &str, ClientContext &context);
extern const PhysicalType ROW_TYPE;
DUCKDB_API string TypeIdToString(PhysicalType type);
idx_t GetTypeIdSize(PhysicalType type);
bool TypeIsConstantSize(PhysicalType type);
bool TypeIsIntegral(PhysicalType type);
bool TypeIsNumeric(PhysicalType type);
bool TypeIsInteger(PhysicalType type);
bool ApproxEqual(float l, float r);
bool ApproxEqual(double l, double r);
struct aggregate_state_t {
 aggregate_state_t(string function_name_p, LogicalType return_type_p, vector<LogicalType> bound_argument_types_p) : function_name(move(function_name_p)), return_type(move(return_type_p)), bound_argument_types(move(bound_argument_types_p)) {
 }
 string function_name;
 LogicalType return_type;
 vector<LogicalType> bound_argument_types;
};
}
