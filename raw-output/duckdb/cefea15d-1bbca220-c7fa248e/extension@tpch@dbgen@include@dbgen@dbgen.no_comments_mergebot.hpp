       
#include "duckdb.hpp"
#ifndef DUCKDB_AMALGAMATION
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#endif
namespace duckdb {
class ClientContext;
}
namespace tpch {
struct DBGenWrapper {
 static void CreateTPCHSchema(duckdb::ClientContext &context, std::string catalog, std::string schema,
                              std::string suffix);
 static void LoadTPCHData(duckdb::ClientContext &context, double sf, std::string catalog, std::string schema,
                          std::string suffix, int children, int step);
 static std::string GetQuery(int query);
 static std::string GetAnswer(double sf, int query);
};
}
