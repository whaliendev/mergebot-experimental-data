#include "catalog/catalog_set.hpp"
#include "common/exception.hpp"
#include "transaction/transaction_manager.hpp"
using namespace duckdb;
using namespace std;
bool CatalogSet::CreateEntry(Transaction &transaction, const string &name,
                             unique_ptr<CatalogEntry> value) {
 lock_guard<mutex> lock(catalog_lock);
 auto entry = data.find(name);
 if (entry == data.end()) {
  auto dummy_node = make_unique<CatalogEntry>(CatalogType::INVALID,
                                              value->catalog, name);
  dummy_node->timestamp = 0;
  dummy_node->deleted = true;
  dummy_node->set = this;
  data[name] = move(dummy_node);
 } else {
  CatalogEntry &current = *entry->second;
  if (current.timestamp >= TRANSACTION_ID_START &&
      current.timestamp != transaction.transaction_id) {
   throw TransactionException("Catalog write-write conflict!");
  }
  if (!current.deleted) {
   return false;
  }
 }
 value->timestamp = transaction.transaction_id;
 value->child = move(data[name]);
 value->child->parent = value.get();
 value->set = this;
 transaction.PushCatalogEntry(value->child.get());
 data[name] = move(value);
 return true;
}
bool CatalogSet::AlterEntry(Transaction &transaction, const string &name, AlterInformation* alter_info, bool cascade) {
 lock_guard<mutex> lock(catalog_lock);
 auto entry = data.find(name);
 if (entry == data.end()) {
  return false;
 }
 CatalogEntry &current = *entry->second;
<<<<<<< HEAD
 if (current.timestamp >= TRANSACTION_ID_START &&
     current.timestamp != transaction.transaction_id) {
  throw TransactionException("Catalog write-write conflict!");
 }
 auto value = current.AlterEntry(alter_info);
 value->timestamp = transaction.transaction_id;
 value->child = move(data[name]);
 value->child->parent = value.get();
 value->set = this;
 transaction.PushCatalogEntry(value->child.get());
 data[name] = move(value);
||||||| c1fbd173f6
=======
>>>>>>> 39bc18a2
 return true;
}
bool CatalogSet::DropEntry(Transaction &transaction, CatalogEntry &current,
                           bool cascade) {
 if (current.timestamp >= TRANSACTION_ID_START &&
     current.timestamp != transaction.transaction_id) {
  throw TransactionException("Catalog write-write conflict!");
 }
 if (current.deleted) {
  return false;
 }
 if (cascade) {
  current.DropDependents(transaction);
 } else {
  if (current.HasDependents(transaction)) {
   throw CatalogException(
       "Cannot drop entry \"%s\" because there are entries that "
       "depend on it. Use DROP...CASCADE to drop all dependents.",
       current.name.c_str());
  }
 }
 auto value = make_unique<CatalogEntry>(CatalogType::DELETED_ENTRY,
                                        current.catalog, current.name);
 value->timestamp = transaction.transaction_id;
 value->child = move(data[current.name]);
 value->child->parent = value.get();
 value->set = this;
 value->deleted = true;
 transaction.PushCatalogEntry(value->child.get());
 data[current.name] = move(value);
 return true;
}
bool CatalogSet::DropEntry(Transaction &transaction, const string &name,
                           bool cascade) {
 lock_guard<mutex> lock(catalog_lock);
 auto entry = data.find(name);
 if (entry == data.end()) {
  return false;
 }
 return DropEntry(transaction, *entry->second, cascade);
}
CatalogEntry *CatalogSet::GetEntryForTransaction(Transaction &transaction,
                                                 CatalogEntry *current) {
 while (current->child) {
  if (current->timestamp == transaction.transaction_id) {
   break;
  }
  if (current->timestamp < transaction.start_time) {
   break;
  }
  current = current->child.get();
  assert(current);
 }
 return current;
}
bool CatalogSet::EntryExists(Transaction &transaction, const string &name) {
 lock_guard<mutex> lock(catalog_lock);
 auto entry = data.find(name);
 if (entry == data.end()) {
  return false;
 }
 CatalogEntry *current =
     GetEntryForTransaction(transaction, data[name].get());
 return !current->deleted;
}
CatalogEntry *CatalogSet::GetEntry(Transaction &transaction,
                                   const string &name) {
 lock_guard<mutex> lock(catalog_lock);
 auto entry = data.find(name);
 if (entry == data.end()) {
  return nullptr;
 }
 CatalogEntry *current =
     GetEntryForTransaction(transaction, data[name].get());
 if (current->deleted) {
  return nullptr;
 }
 return current;
}
void CatalogSet::Undo(CatalogEntry *entry) {
 lock_guard<mutex> lock(catalog_lock);
 auto &to_be_removed_node = entry->parent;
 if (to_be_removed_node->parent) {
  to_be_removed_node->parent->child = move(to_be_removed_node->child);
  entry->parent = to_be_removed_node->parent;
 } else {
  auto &name = entry->name;
  data[name] = move(to_be_removed_node->child);
  entry->parent = nullptr;
 }
}
void CatalogSet::DropAllEntries(Transaction &transaction) {
 lock_guard<mutex> lock(catalog_lock);
 for (auto &entry : data) {
  DropEntry(transaction, *entry.second, true);
 }
}
bool CatalogSet::IsEmpty(Transaction &transaction) {
 lock_guard<mutex> lock(catalog_lock);
 for (auto &entry : data) {
  auto current = GetEntryForTransaction(transaction, entry.second.get());
  if (!current->deleted) {
   return false;
  }
 }
 return true;
}
