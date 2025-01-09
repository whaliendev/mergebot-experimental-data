       
#include "duckdb/common/common.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/storage/storage_options.hpp"
namespace duckdb {
class Catalog;
class DatabaseInstance;
class StorageManager;
class TransactionManager;
class StorageExtension;
class DatabaseManager;
struct AttachInfo;
enum class AttachedDatabaseType {
 READ_WRITE_DATABASE,
 READ_ONLY_DATABASE,
 SYSTEM_DATABASE,
 TEMP_DATABASE,
};
struct AttachOptions {
 explicit AttachOptions(const DBConfigOptions &options);
 AttachOptions(const unique_ptr<AttachInfo> &info, const AccessMode default_access_mode);
 AccessMode access_mode;
 string db_type;
};
class AttachedDatabase : public CatalogEntry {
public:
 explicit AttachedDatabase(DatabaseInstance &db, AttachedDatabaseType type = AttachedDatabaseType::SYSTEM_DATABASE);
 AttachedDatabase(DatabaseInstance &db, Catalog &catalog, string name, string file_path,
                  const AttachOptions &options);
 AttachedDatabase(DatabaseInstance &db, Catalog &catalog, StorageExtension &ext, ClientContext &context, string name,
                  const AttachInfo &info, const AttachOptions &options);
 ~AttachedDatabase() override;
 void Initialize(StorageOptions options = StorageOptions());
 void Close();
 Catalog &ParentCatalog() override;
 const Catalog &ParentCatalog() const override;
 StorageManager &GetStorageManager();
 Catalog &GetCatalog();
 TransactionManager &GetTransactionManager();
 DatabaseInstance &GetDatabase() {
  return db;
 }
 optional_ptr<StorageExtension> GetStorageExtension() {
  return storage_extension;
 }
 const string &GetName() const {
  return name;
 }
 bool IsSystem() const;
 bool IsTemporary() const;
 bool IsReadOnly() const;
 bool IsInitialDatabase() const;
 void SetInitialDatabase();
 void SetReadOnlyDatabase();
 static bool NameIsReserved(const string &name);
 static string ExtractDatabaseName(const string &dbpath, FileSystem &fs);
private:
 DatabaseInstance &db;
 unique_ptr<StorageManager> storage;
 unique_ptr<Catalog> catalog;
 unique_ptr<TransactionManager> transaction_manager;
 AttachedDatabaseType type;
 optional_ptr<Catalog> parent_catalog;
 optional_ptr<StorageExtension> storage_extension;
 bool is_initial_database = false;
 bool is_closed = false;
};
}
