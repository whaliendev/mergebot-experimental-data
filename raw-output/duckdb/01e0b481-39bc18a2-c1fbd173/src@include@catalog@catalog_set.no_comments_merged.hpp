       
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include "common/internal_types.hpp"
#include "catalog/catalog_entry.hpp"
#include "transaction/transaction.hpp"
namespace duckdb {
struct AlterInformation;
class CatalogSet {
  public:
 bool CreateEntry(Transaction &transaction, const std::string &name,
                  std::unique_ptr<CatalogEntry> value);
<<<<<<< HEAD
 bool AlterEntry(Transaction &transaction, const std::string &name, AlterInformation* alter_info);
||||||| c1fbd173f6
 bool AlterEntry(Transaction &transaction, const std::string &name,
     bool cascade);
=======
 bool AlterEntry(Transaction &transaction, const std::string &name,
                 bool cascade);
>>>>>>> 39bc18a2
 bool DropEntry(Transaction &transaction, const std::string &name,
                bool cascade);
 bool EntryExists(Transaction &transaction, const std::string &name);
 CatalogEntry *GetEntry(Transaction &transaction, const std::string &name);
 void Undo(CatalogEntry *entry);
 void DropAllEntries(Transaction &transaction);
 bool IsEmpty(Transaction &transaction);
 template <class T> void Scan(Transaction &transaction, T &&callback) {
  std::lock_guard<std::mutex> lock(catalog_lock);
  for (auto &kv : data) {
   auto entry = kv.second.get();
   entry = GetEntryForTransaction(transaction, entry);
   if (!entry->deleted) {
    callback(entry);
   }
  }
 }
  private:
 bool DropEntry(Transaction &transaction, CatalogEntry &entry, bool cascade);
 CatalogEntry *GetEntryForTransaction(Transaction &transaction,
                                      CatalogEntry *current);
 std::mutex catalog_lock;
 std::unordered_map<std::string, std::unique_ptr<CatalogEntry>> data;
};
}
