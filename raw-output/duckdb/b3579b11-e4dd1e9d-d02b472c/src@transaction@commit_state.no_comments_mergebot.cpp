#include "duckdb/transaction/commit_state.hpp"
#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/chunk_info.hpp"
#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/row_version_manager.hpp"
#include "duckdb/storage/table/update_segment.hpp"
#include "duckdb/storage/write_ahead_log.hpp"
#include "duckdb/transaction/append_info.hpp"
#include "duckdb/transaction/delete_info.hpp"
#include "duckdb/transaction/update_info.hpp"
namespace duckdb {
CommitState::CommitState(transaction_t commit_id, optional_ptr<WriteAheadLog> log)
    : log(log), commit_id(commit_id), current_table_info(nullptr) {
}
void CommitState::SwitchTable(DataTableInfo *table_info, UndoFlags new_op) {
 if (current_table_info != table_info) {
  log->WriteSetTable(table_info->schema, table_info->table);
  current_table_info = table_info;
 }
}
void CommitState::WriteCatalogEntry(CatalogEntry &entry, data_ptr_t dataptr) {
 if (entry.temporary || entry.Parent().temporary) {
  return;
 }
 D_ASSERT(log);
 auto &parent = entry.Parent();
 switch (parent.type) {
 case CatalogType::TABLE_ENTRY:
  if (entry.type == CatalogType::RENAMED_ENTRY || entry.type == CatalogType::TABLE_ENTRY) {
   auto extra_data_size = Load<idx_t>(dataptr);
   auto extra_data = data_ptr_cast(dataptr + sizeof(idx_t));
   MemoryStream source(extra_data, extra_data_size);
   BinaryDeserializer deserializer(source);
   deserializer.Begin();
   auto column_name = deserializer.ReadProperty<string>(100, "column_name");
   auto parse_info = deserializer.ReadProperty<unique_ptr<ParseInfo>>(101, "alter_info");
   deserializer.End();
   if (!column_name.empty()) {
    D_ASSERT(entry.type != CatalogType::RENAMED_ENTRY);
    auto &table_entry = entry.Cast<DuckTableEntry>();
    D_ASSERT(table_entry.IsDuckTable());
    table_entry.CommitAlter(column_name);
   }
   auto &alter_info = parse_info->Cast<AlterInfo>();
   log->WriteAlter(alter_info);
  } else {
   log->WriteCreateTable(parent.Cast<TableCatalogEntry>());
  }
  break;
 case CatalogType::SCHEMA_ENTRY:
  if (entry.type == CatalogType::RENAMED_ENTRY || entry.type == CatalogType::SCHEMA_ENTRY) {
   return;
  }
  log->WriteCreateSchema(parent.Cast<SchemaCatalogEntry>());
  break;
 case CatalogType::VIEW_ENTRY:
  if (entry.type == CatalogType::RENAMED_ENTRY || entry.type == CatalogType::VIEW_ENTRY) {
   auto extra_data_size = Load<idx_t>(dataptr);
   auto extra_data = data_ptr_cast(dataptr + sizeof(idx_t));
   MemoryStream source(extra_data, extra_data_size);
   BinaryDeserializer deserializer(source);
   deserializer.Begin();
   auto column_name = deserializer.ReadProperty<string>(100, "column_name");
   auto parse_info = deserializer.ReadProperty<unique_ptr<ParseInfo>>(101, "alter_info");
   deserializer.End();
   (void)column_name;
   auto &alter_info = parse_info->Cast<AlterInfo>();
   log->WriteAlter(alter_info);
  } else {
   log->WriteCreateView(parent.Cast<ViewCatalogEntry>());
  }
  break;
 case CatalogType::SEQUENCE_ENTRY:
  log->WriteCreateSequence(parent.Cast<SequenceCatalogEntry>());
  break;
 case CatalogType::MACRO_ENTRY:
  log->WriteCreateMacro(parent.Cast<ScalarMacroCatalogEntry>());
  break;
 case CatalogType::TABLE_MACRO_ENTRY:
  log->WriteCreateTableMacro(parent.Cast<TableMacroCatalogEntry>());
  break;
 case CatalogType::INDEX_ENTRY:
  log->WriteCreateIndex(parent.Cast<IndexCatalogEntry>());
  break;
 case CatalogType::TYPE_ENTRY:
  log->WriteCreateType(parent.Cast<TypeCatalogEntry>());
  break;
 case CatalogType::RENAMED_ENTRY:
  break;
 case CatalogType::DELETED_ENTRY:
  switch (entry.type) {
  case CatalogType::TABLE_ENTRY: {
   auto &table_entry = entry.Cast<DuckTableEntry>();
   D_ASSERT(table_entry.IsDuckTable());
   table_entry.CommitDrop();
   log->WriteDropTable(table_entry);
   break;
  }
  case CatalogType::SCHEMA_ENTRY:
   log->WriteDropSchema(entry.Cast<SchemaCatalogEntry>());
   break;
  case CatalogType::VIEW_ENTRY:
   log->WriteDropView(entry.Cast<ViewCatalogEntry>());
   break;
  case CatalogType::SEQUENCE_ENTRY:
   log->WriteDropSequence(entry.Cast<SequenceCatalogEntry>());
   break;
  case CatalogType::MACRO_ENTRY:
   log->WriteDropMacro(entry.Cast<ScalarMacroCatalogEntry>());
   break;
  case CatalogType::TABLE_MACRO_ENTRY:
   log->WriteDropTableMacro(entry.Cast<TableMacroCatalogEntry>());
   break;
  case CatalogType::TYPE_ENTRY:
   log->WriteDropType(entry.Cast<TypeCatalogEntry>());
   break;
  case CatalogType::INDEX_ENTRY: {
   auto &index_entry = entry.Cast<DuckIndexEntry>();
   index_entry.CommitDrop();
   log->WriteDropIndex(entry.Cast<IndexCatalogEntry>());
   break;
  }
  case CatalogType::RENAMED_ENTRY:
  case CatalogType::PREPARED_STATEMENT:
  case CatalogType::SCALAR_FUNCTION_ENTRY:
  case CatalogType::DEPENDENCY_ENTRY:
   break;
  default:
   throw InternalException("Don't know how to drop this type!");
  }
  break;
 case CatalogType::PREPARED_STATEMENT:
 case CatalogType::AGGREGATE_FUNCTION_ENTRY:
 case CatalogType::SCALAR_FUNCTION_ENTRY:
 case CatalogType::TABLE_FUNCTION_ENTRY:
 case CatalogType::COPY_FUNCTION_ENTRY:
 case CatalogType::PRAGMA_FUNCTION_ENTRY:
 case CatalogType::COLLATION_ENTRY:
<<<<<<< HEAD
 case CatalogType::SECRET_ENTRY:
 case CatalogType::SECRET_TYPE_ENTRY:
 case CatalogType::SECRET_FUNCTION_ENTRY:
||||||| d02b472cbf
=======
 case CatalogType::DEPENDENCY_ENTRY:
>>>>>>> e4dd1e9d
  break;
 default:
  throw InternalException("UndoBuffer - don't know how to write this entry to the WAL");
 }
}
void CommitState::WriteDelete(DeleteInfo &info) {
 D_ASSERT(log);
 SwitchTable(info.table->info.get(), UndoFlags::DELETE_TUPLE);
 if (!delete_chunk) {
  delete_chunk = make_uniq<DataChunk>();
  vector<LogicalType> delete_types = {LogicalType::ROW_TYPE};
  delete_chunk->Initialize(Allocator::DefaultAllocator(), delete_types);
 }
 auto rows = FlatVector::GetData<row_t>(delete_chunk->data[0]);
 for (idx_t i = 0; i < info.count; i++) {
  rows[i] = info.base_row + info.rows[i];
 }
 delete_chunk->SetCardinality(info.count);
 log->WriteDelete(*delete_chunk);
}
void CommitState::WriteUpdate(UpdateInfo &info) {
 D_ASSERT(log);
 auto &column_data = info.segment->column_data;
 auto &table_info = column_data.GetTableInfo();
 SwitchTable(&table_info, UndoFlags::UPDATE_TUPLE);
 vector<LogicalType> update_types;
 if (column_data.type.id() == LogicalTypeId::VALIDITY) {
  update_types.emplace_back(LogicalType::BOOLEAN);
 } else {
  update_types.push_back(column_data.type);
 }
 update_types.emplace_back(LogicalType::ROW_TYPE);
 update_chunk = make_uniq<DataChunk>();
 update_chunk->Initialize(Allocator::DefaultAllocator(), update_types);
 info.segment->FetchCommitted(info.vector_index, update_chunk->data[0]);
 auto row_ids = FlatVector::GetData<row_t>(update_chunk->data[1]);
 idx_t start = column_data.start + info.vector_index * STANDARD_VECTOR_SIZE;
 for (idx_t i = 0; i < info.N; i++) {
  row_ids[info.tuples[i]] = start + info.tuples[i];
 }
 if (column_data.type.id() == LogicalTypeId::VALIDITY) {
  auto booleans = FlatVector::GetData<bool>(update_chunk->data[0]);
  for (idx_t i = 0; i < info.N; i++) {
   auto idx = info.tuples[i];
   booleans[idx] = false;
  }
 }
 SelectionVector sel(info.tuples);
 update_chunk->Slice(sel, info.N);
 vector<column_t> column_indexes;
 reference<ColumnData> current_column_data = column_data;
 while (current_column_data.get().parent) {
  column_indexes.push_back(current_column_data.get().column_index);
  current_column_data = *current_column_data.get().parent;
 }
 column_indexes.push_back(info.column_index);
 std::reverse(column_indexes.begin(), column_indexes.end());
 log->WriteUpdate(*update_chunk, column_indexes);
}
template <bool HAS_LOG>
void CommitState::CommitEntry(UndoFlags type, data_ptr_t data) {
 switch (type) {
 case UndoFlags::CATALOG_ENTRY: {
  auto catalog_entry = Load<CatalogEntry *>(data);
  D_ASSERT(catalog_entry->HasParent());
  auto &catalog = catalog_entry->ParentCatalog();
  D_ASSERT(catalog.IsDuckCatalog());
  auto &duck_catalog = catalog.Cast<DuckCatalog>();
  lock_guard<mutex> write_lock(duck_catalog.GetWriteLock());
  lock_guard<mutex> read_lock(catalog_entry->set->GetCatalogLock());
  catalog_entry->set->UpdateTimestamp(catalog_entry->Parent(), commit_id);
  if (!StringUtil::CIEquals(catalog_entry->name, catalog_entry->Parent().name)) {
   catalog_entry->set->UpdateTimestamp(*catalog_entry, commit_id);
  }
  if (HAS_LOG) {
   WriteCatalogEntry(*catalog_entry, data + sizeof(CatalogEntry *));
  }
  break;
 }
 case UndoFlags::INSERT_TUPLE: {
  auto info = reinterpret_cast<AppendInfo *>(data);
  if (HAS_LOG && !info->table->info->IsTemporary()) {
   info->table->WriteToLog(*log, info->start_row, info->count);
  }
  info->table->CommitAppend(commit_id, info->start_row, info->count);
  break;
 }
 case UndoFlags::DELETE_TUPLE: {
  auto info = reinterpret_cast<DeleteInfo *>(data);
  if (HAS_LOG && !info->table->info->IsTemporary()) {
   WriteDelete(*info);
  }
  info->version_info->CommitDelete(info->vector_idx, commit_id, info->rows, info->count);
  break;
 }
 case UndoFlags::UPDATE_TUPLE: {
  auto info = reinterpret_cast<UpdateInfo *>(data);
  if (HAS_LOG && !info->segment->column_data.GetTableInfo().IsTemporary()) {
   WriteUpdate(*info);
  }
  info->version_number = commit_id;
  break;
 }
 default:
  throw InternalException("UndoBuffer - don't know how to commit this type!");
 }
}
void CommitState::RevertCommit(UndoFlags type, data_ptr_t data) {
 transaction_t transaction_id = commit_id;
 switch (type) {
 case UndoFlags::CATALOG_ENTRY: {
  auto catalog_entry = Load<CatalogEntry *>(data);
  D_ASSERT(catalog_entry->HasParent());
  catalog_entry->set->UpdateTimestamp(catalog_entry->Parent(), transaction_id);
  if (catalog_entry->name != catalog_entry->Parent().name) {
   catalog_entry->set->UpdateTimestamp(*catalog_entry, transaction_id);
  }
  break;
 }
 case UndoFlags::INSERT_TUPLE: {
  auto info = reinterpret_cast<AppendInfo *>(data);
  info->table->RevertAppend(info->start_row, info->count);
  break;
 }
 case UndoFlags::DELETE_TUPLE: {
  auto info = reinterpret_cast<DeleteInfo *>(data);
  info->table->info->cardinality += info->count;
  info->version_info->CommitDelete(info->vector_idx, transaction_id, info->rows, info->count);
  break;
 }
 case UndoFlags::UPDATE_TUPLE: {
  auto info = reinterpret_cast<UpdateInfo *>(data);
  info->version_number = transaction_id;
  break;
 }
 default:
  throw InternalException("UndoBuffer - don't know how to revert commit of this type!");
 }
}
}
