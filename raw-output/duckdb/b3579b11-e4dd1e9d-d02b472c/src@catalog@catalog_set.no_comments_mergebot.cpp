#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/dependency_manager.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/catalog/mapping_value.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/catalog/dependency_list.hpp"
namespace duckdb {
void CatalogEntryMap::AddEntry(unique_ptr<CatalogEntry> entry) {
 auto name = entry->name;
 if (entries.find(name) != entries.end()) {
  throw InternalException("Entry with name \"%s\" already exists", name);
 }
 entries.insert(make_pair(name, std::move(entry)));
}
void CatalogEntryMap::UpdateEntry(unique_ptr<CatalogEntry> catalog_entry) {
 auto name = catalog_entry->name;
 auto entry = entries.find(name);
 if (entry == entries.end()) {
  throw InternalException("Entry with name \"%s\" does not exist", name);
 }
 auto existing = std::move(entry->second);
 entry->second = std::move(catalog_entry);
 entry->second->SetChild(std::move(existing));
}
case_insensitive_tree_t<unique_ptr<CatalogEntry>> &CatalogEntryMap::Entries() {
 return entries;
}
void CatalogEntryMap::DropEntry(CatalogEntry &entry) {
 auto &name = entry.name;
 auto chain = GetEntry(name);
 if (!chain) {
  throw InternalException("Attempting to drop entry with name \"%s\" but no chain with that name exists", name);
 }
 auto child = entry.TakeChild();
 if (!entry.HasParent()) {
  D_ASSERT(chain.get() == &entry);
  auto it = entries.find(name);
  D_ASSERT(it != entries.end());
  it->second.reset();
  if (child) {
   it->second = std::move(child);
  } else {
   entries.erase(it);
  }
 } else {
  auto &parent = entry.Parent();
  parent.SetChild(std::move(child));
 }
}
optional_ptr<CatalogEntry> CatalogEntryMap::GetEntry(const string &name) {
 auto entry = entries.find(name);
 if (entry == entries.end()) {
  return nullptr;
 }
 return entry->second.get();
}
CatalogSet::CatalogSet(Catalog &catalog_p, unique_ptr<DefaultGenerator> defaults)
    : catalog(catalog_p.Cast<DuckCatalog>()), defaults(std::move(defaults)) {
 D_ASSERT(catalog_p.IsDuckCatalog());
}
CatalogSet::~CatalogSet() {
}
bool IsDependencyEntry(CatalogEntry &entry) {
 return entry.type == CatalogType::DEPENDENCY_ENTRY;
}
bool CatalogSet::StartChain(CatalogTransaction transaction, const string &name, unique_lock<mutex> &read_lock) {
 D_ASSERT(!map.GetEntry(name));
 auto entry = CreateDefaultEntry(transaction, name, read_lock);
 if (entry) {
  return false;
 }
 auto dummy_node = make_uniq<InCatalogEntry>(CatalogType::INVALID, catalog, name);
 dummy_node->timestamp = 0;
 dummy_node->deleted = true;
 dummy_node->set = this;
 map.AddEntry(std::move(dummy_node));
 return true;
}
bool CatalogSet::VerifyVacancy(CatalogTransaction transaction, CatalogEntry &entry) {
 if (HasConflict(transaction, entry.timestamp)) {
  throw TransactionException("Catalog write-write conflict on create with \"%s\"", entry.name);
 }
 if (!entry.deleted) {
  return false;
 }
 return true;
}
void CatalogSet::CheckCatalogEntryInvariants(CatalogEntry &value, const string &name) {
 if (value.internal && !catalog.IsSystemCatalog() && name != DEFAULT_SCHEMA) {
  throw InternalException("Attempting to create internal entry \"%s\" in non-system catalog - internal entries "
                          "can only be created in the system catalog",
                          name);
 }
 if (!value.internal) {
  if (!value.temporary && catalog.IsSystemCatalog() && !IsDependencyEntry(value)) {
   throw InternalException(
       "Attempting to create non-internal entry \"%s\" in system catalog - the system catalog "
       "can only contain internal entries",
       name);
  }
  if (value.temporary && !catalog.IsTemporaryCatalog()) {
   throw InternalException("Attempting to create temporary entry \"%s\" in non-temporary catalog", name);
  }
  if (!value.temporary && catalog.IsTemporaryCatalog() && name != DEFAULT_SCHEMA) {
   throw InvalidInputException("Cannot create non-temporary entry \"%s\" in temporary catalog", name);
  }
 }
}
optional_ptr<CatalogEntry> CatalogSet::CreateCommittedEntry(unique_ptr<CatalogEntry> entry) {
 auto existing_entry = map.GetEntry(entry->name);
 if (existing_entry) {
  return nullptr;
 }
 auto catalog_entry = entry.get();
 entry->set = this;
 entry->timestamp = 0;
 map.AddEntry(std::move(entry));
 return catalog_entry;
}
bool CatalogSet::CreateEntryInternal(CatalogTransaction transaction, const string &name, unique_ptr<CatalogEntry> value, unique_lock<mutex> &read_lock, bool should_be_empty) {
 auto entry_value = map.GetEntry(name);
 if (!entry_value) {
  if (!StartChain(transaction, name, read_lock)) {
   return false;
  }
 } else if (should_be_empty) {
  if (!VerifyVacancy(transaction, *entry_value)) {
   return false;
  }
 }
 auto value_ptr = value.get();
 map.UpdateEntry(std::move(value));
 if (transaction.transaction) {
  auto &dtransaction = transaction.transaction->Cast<DuckTransaction>();
  dtransaction.PushCatalogEntry(value_ptr->Child());
 }
 return true;
}
bool CatalogSet::CreateEntry(CatalogTransaction transaction, const string &name, unique_ptr<CatalogEntry> value,
                             const DependencyList &dependencies) {
 CheckCatalogEntryInvariants(*value, name);
 value->timestamp = transaction.transaction_id;
 value->set = this;
 catalog.GetDependencyManager().AddObject(transaction, *value, dependencies);
 lock_guard<mutex> write_lock(catalog.GetWriteLock());
 unique_lock<mutex> read_lock(catalog_lock);
 return CreateEntryInternal(transaction, name, std::move(value), read_lock);
}
bool CatalogSet::CreateEntry(ClientContext &context, const string &name, unique_ptr<CatalogEntry> value,
                             const DependencyList &dependencies) {
 return CreateEntry(catalog.GetCatalogTransaction(context), name, std::move(value), dependencies);
}
optional_ptr<CatalogEntry> CatalogSet::GetEntryInternal(CatalogTransaction transaction, const string &name) {
 auto entry_value = map.GetEntry(name);
 if (!entry_value) {
  return nullptr;
 }
 auto &catalog_entry = *entry_value;
 if (HasConflict(transaction, catalog_entry.timestamp)) {
  throw TransactionException("Catalog write-write conflict on alter with \"%s\"", catalog_entry.name);
 }
 if (catalog_entry.deleted) {
  return nullptr;
 }
 return &catalog_entry;
}
bool CatalogSet::AlterOwnership(CatalogTransaction transaction, ChangeOwnershipInfo &info) {
 unique_lock<mutex> write_lock(catalog.GetWriteLock());
 auto entry = GetEntryInternal(transaction, info.name);
 if (!entry) {
  return false;
 }
 auto &owner_entry = catalog.GetEntry(transaction.GetContext(), info.owner_schema, info.owner_name);
 write_lock.unlock();
 catalog.GetDependencyManager().AddOwnership(transaction, owner_entry, *entry);
 return true;
}
bool CatalogSet::RenameEntryInternal(CatalogTransaction transaction, CatalogEntry &old, const string &new_name, AlterInfo &alter_info, unique_lock<mutex> &read_lock) {
 auto &original_name = old.name;
 auto &context = *transaction.context;
 auto entry_value = map.GetEntry(new_name);
 if (entry_value) {
  auto &existing_entry = GetEntryForTransaction(transaction, *entry_value);
  if (!existing_entry.deleted) {
   old.UndoAlter(context, alter_info);
   throw CatalogException("Could not rename \"%s\" to \"%s\": another entry with this name already exists!",
                          original_name, new_name);
  }
 }
 auto renamed_tombstone = make_uniq<InCatalogEntry>(CatalogType::RENAMED_ENTRY, old.ParentCatalog(), original_name);
 renamed_tombstone->timestamp = transaction.transaction_id;
 renamed_tombstone->deleted = false;
 renamed_tombstone->set = this;
 if (!CreateEntryInternal(transaction, original_name, std::move(renamed_tombstone), read_lock,
                                                 false)) {
  return false;
 }
 if (!DropEntryInternal(transaction, original_name, false)) {
  return false;
 }
 auto renamed_node = make_uniq<InCatalogEntry>(CatalogType::RENAMED_ENTRY, catalog, new_name);
 renamed_node->timestamp = transaction.transaction_id;
 renamed_node->deleted = false;
 renamed_node->set = this;
 return CreateEntryInternal(transaction, new_name, std::move(renamed_node), read_lock);
}
bool CatalogSet::AlterEntry(CatalogTransaction transaction, const string &name, AlterInfo &alter_info) {
 unique_lock<mutex> write_lock(catalog.GetWriteLock());
 unique_lock<mutex> read_lock(catalog_lock);
 auto entry = GetEntryInternal(transaction, name);
 if (!entry) {
  return false;
 }
 if (!alter_info.allow_internal && entry->internal) {
  throw CatalogException("Cannot alter entry \"%s\" because it is an internal system entry", entry->name);
 }
 if (!transaction.context) {
  throw InternalException("Cannot AlterEntry without client context");
 }
 auto &context = *transaction.context;
 auto value = entry->AlterEntry(context, alter_info);
 if (!value) {
  return true;
 }
 value->timestamp = transaction.transaction_id;
 value->set = this;
 if (!StringUtil::CIEquals(value->name, entry->name)) {
  if (!RenameEntryInternal(transaction, *entry, value->name, alter_info, read_lock)) {
   return false;
  }
 }
 auto new_entry = value.get();
 map.UpdateEntry(std::move(value));
 if (transaction.transaction) {
  MemoryStream stream;
  BinarySerializer serializer(stream);
  serializer.Begin();
  serializer.WriteProperty(100, "column_name", alter_info.GetColumnName());
  serializer.WriteProperty(101, "alter_info", &alter_info);
  serializer.End();
  auto &dtransaction = transaction.transaction->Cast<DuckTransaction>();
  dtransaction.PushCatalogEntry(new_entry->Child(), stream.GetData(), stream.GetPosition());
 }
 read_lock.unlock();
 write_lock.unlock();
 catalog.GetDependencyManager().AlterObject(transaction, *entry, *new_entry);
 return true;
}
bool CatalogSet::DropDependencies(CatalogTransaction transaction, const string &name, bool cascade, bool allow_drop_internal) {
 auto entry = GetEntry(transaction, name);
 if (!entry) {
  return false;
 }
 if (entry->internal && !allow_drop_internal) {
  throw CatalogException("Cannot drop entry \"%s\" because it is an internal system entry", entry->name);
 }
 D_ASSERT(entry->ParentCatalog().IsDuckCatalog());
 auto &duck_catalog = entry->ParentCatalog().Cast<DuckCatalog>();
 duck_catalog.GetDependencyManager().DropObject(transaction, *entry, cascade);
 return true;
}
bool CatalogSet::DropEntryInternal(CatalogTransaction transaction, const string &name, bool allow_drop_internal) {
 auto entry = GetEntryInternal(transaction, name);
 if (!entry) {
  return false;
 }
 if (entry->internal && !allow_drop_internal) {
  throw CatalogException("Cannot drop entry \"%s\" because it is an internal system entry", entry->name);
 }
 auto value = make_uniq<InCatalogEntry>(CatalogType::DELETED_ENTRY, entry->ParentCatalog(), entry->name);
 value->timestamp = transaction.transaction_id;
 value->set = this;
 value->deleted = true;
 auto value_ptr = value.get();
 map.UpdateEntry(std::move(value));
 if (transaction.transaction) {
  auto &dtransaction = transaction.transaction->Cast<DuckTransaction>();
  dtransaction.PushCatalogEntry(value_ptr->Child());
 }
 return true;
}
bool CatalogSet::DropEntry(CatalogTransaction transaction, const string &name, bool cascade, bool allow_drop_internal) {
 if (!DropDependencies(transaction, name, cascade, allow_drop_internal)) {
  return false;
 }
 lock_guard<mutex> write_lock(catalog.GetWriteLock());
 lock_guard<mutex> read_lock(catalog_lock);
 return DropEntryInternal(transaction, name, allow_drop_internal);
}
bool CatalogSet::DropEntry(ClientContext &context, const string &name, bool cascade, bool allow_drop_internal) {
 return DropEntry(catalog.GetCatalogTransaction(context), name, cascade, allow_drop_internal);
}
DuckCatalog &CatalogSet::GetCatalog() {
 return catalog;
}
void CatalogSet::CleanupEntry(CatalogEntry &catalog_entry) {
 lock_guard<mutex> write_lock(catalog.GetWriteLock());
 lock_guard<mutex> lock(catalog_lock);
 auto &parent = catalog_entry.Parent();
 map.DropEntry(catalog_entry);
 if (parent.deleted && !parent.HasChild() && !parent.HasParent()) {
  D_ASSERT(map.GetEntry(parent.name).get() == &parent);
  map.DropEntry(parent);
 }
}
bool CatalogSet::CreatedByOtherActiveTransaction(CatalogTransaction transaction, transaction_t timestamp) {
 return (timestamp >= TRANSACTION_ID_START && timestamp != transaction.transaction_id);
}
bool CatalogSet::CommittedAfterStarting(CatalogTransaction transaction, transaction_t timestamp) {
 return (timestamp < TRANSACTION_ID_START && timestamp > transaction.start_time);
}
bool CatalogSet::HasConflict(CatalogTransaction transaction, transaction_t timestamp) {
 return CreatedByOtherActiveTransaction(transaction, timestamp) || CommittedAfterStarting(transaction, timestamp);
}
bool CatalogSet::UseTimestamp(CatalogTransaction transaction, transaction_t timestamp) {
 if (timestamp == transaction.transaction_id) {
  return true;
 }
 if (timestamp < transaction.start_time) {
  return true;
 }
 return false;
}
CatalogEntry &CatalogSet::GetEntryForTransaction(CatalogTransaction transaction, CatalogEntry &current) {
 reference<CatalogEntry> entry(current);
 while (entry.get().HasChild()) {
  if (UseTimestamp(transaction, entry.get().timestamp)) {
   break;
  }
  entry = entry.get().Child();
 }
 return entry.get();
}
CatalogEntry &CatalogSet::GetCommittedEntry(CatalogEntry &current) {
 reference<CatalogEntry> entry(current);
 while (entry.get().HasChild()) {
  if (entry.get().timestamp < TRANSACTION_ID_START) {
   break;
  }
  entry = entry.get().Child();
 }
 return entry.get();
}
SimilarCatalogEntry CatalogSet::SimilarEntry(CatalogTransaction transaction, const string &name) {
 unique_lock<mutex> lock(catalog_lock);
 CreateDefaultEntries(transaction, lock);
 SimilarCatalogEntry result;
 for (auto &kv : map.Entries()) {
  auto ldist = StringUtil::SimilarityScore(kv.first, name);
  if (ldist < result.distance) {
   result.distance = ldist;
   result.name = kv.first;
  }
 }
 return result;
}
optional_ptr<CatalogEntry> CatalogSet::CreateDefaultEntry(CatalogTransaction transaction, const string &name,
                                                          unique_lock<mutex> &read_lock) {
 if (!defaults || defaults->created_all_entries) {
  return nullptr;
 }
 if (!transaction.context) {
  return nullptr;
 }
 read_lock.unlock();
 auto entry = defaults->CreateDefaultEntry(*transaction.context, name);
 read_lock.lock();
 if (!entry) {
  return nullptr;
 }
 auto result = CreateCommittedEntry(std::move(entry));
 if (result) {
  return result;
 }
 read_lock.unlock();
 return GetEntry(transaction, name);
}
CatalogSet::EntryLookup CatalogSet::GetEntryDetailed(CatalogTransaction transaction, const string &name) {
 unique_lock<mutex> read_lock(catalog_lock);
 auto entry_value = map.GetEntry(name);
 if (entry_value) {
  auto &catalog_entry = *entry_value;
  auto &current = GetEntryForTransaction(transaction, catalog_entry);
  if (current.deleted) {
   return EntryLookup {nullptr, EntryLookup::FailureReason::DELETED};
  }
  D_ASSERT(StringUtil::CIEquals(name, current.name));
  return EntryLookup {&current, EntryLookup::FailureReason::SUCCESS};
 }
 auto default_entry = CreateDefaultEntry(transaction, name, read_lock);
 if (!default_entry) {
  return EntryLookup {default_entry, EntryLookup::FailureReason::NOT_PRESENT};
 }
 return EntryLookup {default_entry, EntryLookup::FailureReason::SUCCESS};
}
optional_ptr<CatalogEntry> CatalogSet::GetEntry(CatalogTransaction transaction, const string &name) {
 auto lookup = GetEntryDetailed(transaction, name);
 return lookup.result;
}
optional_ptr<CatalogEntry> CatalogSet::GetEntry(ClientContext &context, const string &name) {
 return GetEntry(catalog.GetCatalogTransaction(context), name);
}
void CatalogSet::UpdateTimestamp(CatalogEntry &entry, transaction_t timestamp) {
 entry.timestamp = timestamp;
}
void CatalogSet::Undo(CatalogEntry &entry) {
 lock_guard<mutex> write_lock(catalog.GetWriteLock());
 lock_guard<mutex> lock(catalog_lock);
 auto &to_be_removed_node = entry.Parent();
 D_ASSERT(StringUtil::CIEquals(entry.name, to_be_removed_node.name));
 if (!to_be_removed_node.HasParent()) {
  to_be_removed_node.Child().SetAsRoot();
 }
 map.DropEntry(to_be_removed_node);
 if (entry.type == CatalogType::INVALID) {
  map.DropEntry(entry);
 }
 catalog.ModifyCatalog();
}
void CatalogSet::CreateDefaultEntries(CatalogTransaction transaction, unique_lock<mutex> &read_lock) {
 if (!defaults || defaults->created_all_entries || !transaction.context) {
  return;
 }
 auto default_entries = defaults->GetDefaultEntries();
 for (auto &default_entry : default_entries) {
  auto entry_value = map.GetEntry(default_entry);
  if (!entry_value) {
   read_lock.unlock();
   auto entry = defaults->CreateDefaultEntry(*transaction.context, default_entry);
   if (!entry) {
    throw InternalException("Failed to create default entry for %s", default_entry);
   }
<<<<<<< HEAD
   lock.lock();
   CreateEntryInternal(transaction, std::move(entry));
||||||| d02b472cbf
   lock.lock();
   CreateEntryInternal(transaction, std::move(entry));
=======
   read_lock.lock();
   CreateCommittedEntry(std::move(entry));
>>>>>>> e4dd1e9d
  }
 }
 defaults->created_all_entries = true;
}
void CatalogSet::Scan(CatalogTransaction transaction, const std::function<void(CatalogEntry &)> &callback) {
 unique_lock<mutex> lock(catalog_lock);
 CreateDefaultEntries(transaction, lock);
 for (auto &kv : map.Entries()) {
  auto &entry = *kv.second;
  auto &entry_for_transaction = GetEntryForTransaction(transaction, entry);
  if (!entry_for_transaction.deleted) {
   callback(entry_for_transaction);
  }
 }
}
void CatalogSet::Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback) {
 Scan(catalog.GetCatalogTransaction(context), callback);
}
void CatalogSet::ScanWithPrefix(CatalogTransaction transaction, const std::function<void(CatalogEntry &)> &callback,
                                const string &prefix) {
 unique_lock<mutex> lock(catalog_lock);
 CreateDefaultEntries(transaction, lock);
 auto &entries = map.Entries();
 auto it = entries.lower_bound(prefix);
 auto end = entries.upper_bound(prefix + char(255));
 for (; it != end; it++) {
  auto &entry = *it->second;
  auto &entry_for_transaction = GetEntryForTransaction(transaction, entry);
  if (!entry_for_transaction.deleted) {
   callback(entry_for_transaction);
  }
 }
}
void CatalogSet::Scan(const std::function<void(CatalogEntry &)> &callback) {
 lock_guard<mutex> lock(catalog_lock);
 for (auto &kv : map.Entries()) {
  auto &entry = *kv.second;
  auto &commited_entry = GetCommittedEntry(entry);
  if (!commited_entry.deleted) {
   callback(commited_entry);
  }
 }
}
void CatalogSet::Verify(Catalog &catalog_p) {
 D_ASSERT(&catalog_p == &catalog);
 vector<reference<CatalogEntry>> entries;
 Scan([&](CatalogEntry &entry) { entries.push_back(entry); });
 for (auto &entry : entries) {
  entry.get().Verify(catalog_p);
 }
}
}
