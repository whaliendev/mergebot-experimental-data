       
#include "duckdb/common/constants.hpp"
namespace duckdb {
enum class CatalogType : uint8_t {
 INVALID = 0,
 TABLE_ENTRY = 1,
 SCHEMA_ENTRY = 2,
 VIEW_ENTRY = 3,
 INDEX_ENTRY = 4,
 PREPARED_STATEMENT = 5,
 SEQUENCE_ENTRY = 6,
 COLLATION_ENTRY = 7,
 TYPE_ENTRY = 8,
 DATABASE_ENTRY = 9,
 TABLE_FUNCTION_ENTRY = 25,
 SCALAR_FUNCTION_ENTRY = 26,
 AGGREGATE_FUNCTION_ENTRY = 27,
 PRAGMA_FUNCTION_ENTRY = 28,
 COPY_FUNCTION_ENTRY = 29,
 MACRO_ENTRY = 30,
 TABLE_MACRO_ENTRY = 31,
 DELETED_ENTRY = 51,
<<<<<<< HEAD
 SECRET_ENTRY = 71,
 SECRET_TYPE_ENTRY = 72,
 SECRET_FUNCTION_ENTRY = 73,
||||||| d02b472cbf
=======
 RENAMED_ENTRY = 52,
 DEPENDENCY_ENTRY = 100
>>>>>>> e4dd1e9d
};
DUCKDB_API string CatalogTypeToString(CatalogType type);
CatalogType CatalogTypeFromString(const string &type);
}
