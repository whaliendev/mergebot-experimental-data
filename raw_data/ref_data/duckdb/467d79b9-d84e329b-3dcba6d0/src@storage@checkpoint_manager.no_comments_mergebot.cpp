#include "duckdb/storage/checkpoint_manager.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/sequence_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/common/field_writer.hpp"
#include "duckdb/common/serializer.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/bound_tableref.hpp"
#include "duckdb/planner/expression_binder/index_binder.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/checkpoint/table_data_reader.hpp"
#include "duckdb/storage/checkpoint/table_data_writer.hpp"
#include "duckdb/storage/meta_block_reader.hpp"
#include "duckdb/storage/table/column_checkpoint_state.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/main/attached_database.hpp"
namespace duckdb {
void ReorderTableEntries(vector<TableCatalogEntry *> &tables);
SingleFileCheckpointWriter::SingleFileCheckpointWriter(AttachedDatabase &db, BlockManager &block_manager)
    : CheckpointWriter(db), partial_block_manager(block_manager) {
}
BlockManager &SingleFileCheckpointWriter::GetBlockManager() {
 auto &storage_manager = (SingleFileStorageManager &)db.GetStorageManager();
 return *storage_manager.block_manager;
}
MetaBlockWriter &SingleFileCheckpointWriter::GetMetaBlockWriter() {
 return *metadata_writer;
}
unique_ptr<TableDataWriter> SingleFileCheckpointWriter::GetTableDataWriter(TableCatalogEntry &table) {
 return make_uniq<SingleFileTableDataWriter>(*this, table, *table_metadata_writer, GetMetaBlockWriter());
}
void SingleFileCheckpointWriter::CreateCheckpoint() {
 auto &config = DBConfig::Get(db);
 auto &storage_manager = (SingleFileStorageManager &)db.GetStorageManager();
 if (storage_manager.InMemory()) {
  return;
 }
 D_ASSERT(!metadata_writer);
 auto &block_manager = GetBlockManager();
 metadata_writer = make_uniq<MetaBlockWriter>(block_manager);
 table_metadata_writer = make_uniq<MetaBlockWriter>(block_manager);
 block_id_t meta_block = metadata_writer->GetBlockPointer().block_id;
 vector<SchemaCatalogEntry *> schemas;
 auto &catalog = (DuckCatalog &)Catalog::GetCatalog(db);
 catalog.ScanSchemas([&](CatalogEntry *entry) { schemas.push_back((SchemaCatalogEntry *)entry); });
 metadata_writer->Write<uint32_t>(schemas.size());
 for (auto &schema : schemas) {
  WriteSchema(*schema);
 }
 partial_block_manager.FlushPartialBlocks();
 metadata_writer->Flush();
 table_metadata_writer->Flush();
 auto wal = storage_manager.GetWriteAheadLog();
 wal->WriteCheckpoint(meta_block);
 wal->Flush();
 if (config.options.checkpoint_abort == CheckpointAbort::DEBUG_ABORT_BEFORE_HEADER) {
  throw FatalException("Checkpoint aborted before header write because of PRAGMA checkpoint_abort flag");
 }
 DatabaseHeader header;
 header.meta_block = meta_block;
 block_manager.WriteHeader(header);
 if (config.options.checkpoint_abort == CheckpointAbort::DEBUG_ABORT_BEFORE_TRUNCATE) {
  throw FatalException("Checkpoint aborted before truncate because of PRAGMA checkpoint_abort flag");
 }
 wal->Truncate(0);
 metadata_writer->MarkWrittenBlocks();
 table_metadata_writer->MarkWrittenBlocks();
}
void SingleFileCheckpointReader::LoadFromStorage() {
 auto &block_manager = *storage.block_manager;
 block_id_t meta_block = block_manager.GetMetaBlock();
 if (meta_block < 0) {
  return;
 }
 Connection con(storage.GetDatabase());
 con.BeginTransaction();
 MetaBlockReader reader(block_manager, meta_block);
 reader.SetCatalog(&catalog.GetAttached().GetCatalog());
 reader.SetContext(con.context.get());
 LoadCheckpoint(*con.context, reader);
 con.Commit();
}
void CheckpointReader::LoadCheckpoint(ClientContext &context, MetaBlockReader &reader) {
 uint32_t schema_count = reader.Read<uint32_t>();
 for (uint32_t i = 0; i < schema_count; i++) {
  ReadSchema(context, reader);
 }
}
void CheckpointWriter::WriteSchema(SchemaCatalogEntry &schema) {
 schema.Serialize(GetMetaBlockWriter());
 vector<TableCatalogEntry *> tables;
 vector<ViewCatalogEntry *> views;
 schema.Scan(CatalogType::TABLE_ENTRY, [&](CatalogEntry *entry) {
  if (entry->internal) {
   return;
  }
  if (entry->type == CatalogType::TABLE_ENTRY) {
   tables.push_back((TableCatalogEntry *)entry);
  } else if (entry->type == CatalogType::VIEW_ENTRY) {
   views.push_back((ViewCatalogEntry *)entry);
  } else {
   throw NotImplementedException("Catalog type for entries");
  }
 });
 vector<SequenceCatalogEntry *> sequences;
 schema.Scan(CatalogType::SEQUENCE_ENTRY, [&](CatalogEntry *entry) {
  if (entry->internal) {
   return;
  }
  sequences.push_back((SequenceCatalogEntry *)entry);
 });
 vector<TypeCatalogEntry *> custom_types;
 schema.Scan(CatalogType::TYPE_ENTRY, [&](CatalogEntry *entry) {
  if (entry->internal) {
   return;
  }
  custom_types.push_back((TypeCatalogEntry *)entry);
 });
 vector<ScalarMacroCatalogEntry *> macros;
 schema.Scan(CatalogType::SCALAR_FUNCTION_ENTRY, [&](CatalogEntry *entry) {
  if (entry->internal) {
   return;
  }
  if (entry->type == CatalogType::MACRO_ENTRY) {
   macros.push_back((ScalarMacroCatalogEntry *)entry);
  }
 });
 vector<TableMacroCatalogEntry *> table_macros;
 schema.Scan(CatalogType::TABLE_FUNCTION_ENTRY, [&](CatalogEntry *entry) {
  if (entry->internal) {
   return;
  }
  if (entry->type == CatalogType::TABLE_MACRO_ENTRY) {
   table_macros.push_back((TableMacroCatalogEntry *)entry);
  }
 });
 vector<IndexCatalogEntry *> indexes;
 schema.Scan(CatalogType::INDEX_ENTRY, [&](CatalogEntry *entry) {
  D_ASSERT(!entry->internal);
  indexes.push_back((IndexCatalogEntry *)entry);
 });
 FieldWriter writer(GetMetaBlockWriter());
 writer.WriteField<uint32_t>(custom_types.size());
 writer.WriteField<uint32_t>(sequences.size());
 writer.WriteField<uint32_t>(tables.size());
 writer.WriteField<uint32_t>(views.size());
 writer.WriteField<uint32_t>(macros.size());
 writer.WriteField<uint32_t>(table_macros.size());
 writer.WriteField<uint32_t>(indexes.size());
 writer.Finalize();
 for (auto &custom_type : custom_types) {
  WriteType(*custom_type);
 }
 for (auto &seq : sequences) {
  WriteSequence(*seq);
 }
 ReorderTableEntries(tables);
 for (auto &table : tables) {
  WriteTable(*table);
 }
 for (auto &view : views) {
  WriteView(*view);
 }
 for (auto &macro : macros) {
  WriteMacro(*macro);
 }
 for (auto &macro : table_macros) {
  WriteTableMacro(*macro);
 }
 for (auto &index : indexes) {
  WriteIndex(*index);
 }
}
void CheckpointReader::ReadSchema(ClientContext &context, MetaBlockReader &reader) {
 auto info = SchemaCatalogEntry::Deserialize(reader);
 info->on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
 catalog.CreateSchema(context, info.get());
 FieldReader field_reader(reader);
 uint32_t enum_count = field_reader.ReadRequired<uint32_t>();
 uint32_t seq_count = field_reader.ReadRequired<uint32_t>();
 uint32_t table_count = field_reader.ReadRequired<uint32_t>();
 uint32_t view_count = field_reader.ReadRequired<uint32_t>();
 uint32_t macro_count = field_reader.ReadRequired<uint32_t>();
 uint32_t table_macro_count = field_reader.ReadRequired<uint32_t>();
 uint32_t table_index_count = field_reader.ReadRequired<uint32_t>();
 field_reader.Finalize();
 for (uint32_t i = 0; i < enum_count; i++) {
  ReadType(context, reader);
 }
 for (uint32_t i = 0; i < seq_count; i++) {
  ReadSequence(context, reader);
 }
 for (uint32_t i = 0; i < table_count; i++) {
  ReadTable(context, reader);
 }
 for (uint32_t i = 0; i < view_count; i++) {
  ReadView(context, reader);
 }
 for (uint32_t i = 0; i < macro_count; i++) {
  ReadMacro(context, reader);
 }
 for (uint32_t i = 0; i < table_macro_count; i++) {
  ReadTableMacro(context, reader);
 }
 for (uint32_t i = 0; i < table_index_count; i++) {
  ReadIndex(context, reader);
 }
}
void CheckpointWriter::WriteView(ViewCatalogEntry &view) {
 view.Serialize(GetMetaBlockWriter());
}
void CheckpointReader::ReadView(ClientContext &context, MetaBlockReader &reader) {
 auto info = ViewCatalogEntry::Deserialize(reader, context);
 catalog.CreateView(context, info.get());
}
void CheckpointWriter::WriteSequence(SequenceCatalogEntry &seq) {
 seq.Serialize(GetMetaBlockWriter());
}
void CheckpointReader::ReadSequence(ClientContext &context, MetaBlockReader &reader) {
 auto info = SequenceCatalogEntry::Deserialize(reader);
 catalog.CreateSequence(context, info.get());
}
void CheckpointWriter::WriteIndex(IndexCatalogEntry &index_catalog) {
 auto root_offset = index_catalog.index->GetSerializedDataPointer();
 auto &metadata_writer = GetMetaBlockWriter();
 index_catalog.Serialize(metadata_writer);
 metadata_writer.Write(root_offset.block_id);
 metadata_writer.Write(root_offset.offset);
}
void CheckpointReader::ReadIndex(ClientContext &context, MetaBlockReader &reader) {
 auto info = IndexCatalogEntry::Deserialize(reader, context);
 auto schema_catalog = catalog.GetSchema(context, info->schema);
 auto table_catalog =
     (DuckTableEntry *)catalog.GetEntry(context, CatalogType::TABLE_ENTRY, info->schema, info->table->table_name);
 auto index_catalog = (DuckIndexEntry *)schema_catalog->CreateIndex(context, info.get(), table_catalog);
 index_catalog->info = table_catalog->GetStorage().info;
 auto root_block_id = reader.Read<block_id_t>();
 auto root_offset = reader.Read<uint32_t>();
 vector<unique_ptr<Expression>> unbound_expressions;
 vector<unique_ptr<ParsedExpression>> parsed_expressions;
 for (auto &p_exp : info->parsed_expressions) {
  parsed_expressions.push_back(p_exp->Copy());
 }
 auto binder = Binder::CreateBinder(context);
 auto table_ref = (TableRef *)info->table.get();
 auto bound_table = binder->Bind(*table_ref);
 D_ASSERT(bound_table->type == TableReferenceType::BASE_TABLE);
 IndexBinder idx_binder(*binder, context);
 unbound_expressions.reserve(parsed_expressions.size());
 for (auto &expr : parsed_expressions) {
  unbound_expressions.push_back(idx_binder.Bind(expr));
 }
 if (parsed_expressions.empty()) {
  unbound_expressions.reserve(info->column_ids.size());
  for (idx_t key_nr = 0; key_nr < info->column_ids.size(); key_nr++) {
   auto &col = table_catalog->GetColumn(LogicalIndex(info->column_ids[key_nr]));
   unbound_expressions.push_back(
       make_uniq<BoundColumnRefExpression>(col.GetName(), col.GetType(), ColumnBinding(0, key_nr)));
  }
 }
 switch (info->index_type) {
 case IndexType::ART: {
  auto &storage = table_catalog->GetStorage();
<<<<<<< HEAD
  auto art = make_unique<ART>(info->column_ids, TableIOManager::Get(storage), std::move(unbound_expressions),
                              info->constraint_type, storage.db, root_block_id, root_offset);
|||||||
  auto art = make_unique<ART>(info->column_ids, TableIOManager::Get(storage), std::move(unbound_expressions),
                              info->constraint_type, storage.db, true, root_block_id, root_offset);
=======
  auto art = make_uniq<ART>(info->column_ids, TableIOManager::Get(storage), std::move(unbound_expressions),
                            info->constraint_type, storage.db, true, root_block_id, root_offset);
>>>>>>> d84e329b281c1246646f5e7c5388bdf11df1e78a
  index_catalog->index = art.get();
  storage.info->indexes.AddIndex(std::move(art));
  break;
 }
 default:
  throw InternalException("Unknown index type for ReadIndex");
 }
}
void CheckpointWriter::WriteType(TypeCatalogEntry &type) {
 type.Serialize(GetMetaBlockWriter());
}
void CheckpointReader::ReadType(ClientContext &context, MetaBlockReader &reader) {
 auto info = TypeCatalogEntry::Deserialize(reader);
 auto catalog_entry = (TypeCatalogEntry *)catalog.CreateType(context, info.get());
 if (info->type.id() == LogicalTypeId::ENUM) {
  EnumType::SetCatalog(info->type, catalog_entry);
 }
}
void CheckpointWriter::WriteMacro(ScalarMacroCatalogEntry &macro) {
 macro.Serialize(GetMetaBlockWriter());
}
void CheckpointReader::ReadMacro(ClientContext &context, MetaBlockReader &reader) {
 auto info = ScalarMacroCatalogEntry::Deserialize(reader, context);
 catalog.CreateFunction(context, info.get());
}
void CheckpointWriter::WriteTableMacro(TableMacroCatalogEntry &macro) {
 macro.Serialize(GetMetaBlockWriter());
}
void CheckpointReader::ReadTableMacro(ClientContext &context, MetaBlockReader &reader) {
 auto info = TableMacroCatalogEntry::Deserialize(reader, context);
 catalog.CreateFunction(context, info.get());
}
void CheckpointWriter::WriteTable(TableCatalogEntry &table) {
 table.Serialize(GetMetaBlockWriter());
 if (auto writer = GetTableDataWriter(table)) {
  writer->WriteTableData();
 }
}
void CheckpointReader::ReadTable(ClientContext &context, MetaBlockReader &reader) {
 auto info = TableCatalogEntry::Deserialize(reader, context);
 auto binder = Binder::CreateBinder(context);
 auto schema = catalog.GetSchema(context, info->schema);
 auto bound_info = binder->BindCreateTableInfo(std::move(info), schema);
 ReadTableData(context, reader, *bound_info);
 catalog.CreateTable(context, bound_info.get());
}
void CheckpointReader::ReadTableData(ClientContext &context, MetaBlockReader &reader,
                                     BoundCreateTableInfo &bound_info) {
 auto block_id = reader.Read<block_id_t>();
 auto offset = reader.Read<uint64_t>();
 MetaBlockReader table_data_reader(reader.block_manager, block_id);
 table_data_reader.offset = offset;
 TableDataReader data_reader(table_data_reader, bound_info);
 data_reader.ReadTableData();
 bound_info.data->total_rows = reader.Read<idx_t>();
 idx_t num_indexes = reader.Read<idx_t>();
 for (idx_t i = 0; i < num_indexes; i++) {
  auto idx_block_id = reader.Read<idx_t>();
  auto idx_offset = reader.Read<idx_t>();
  bound_info.indexes.emplace_back(idx_block_id, idx_offset);
 }
}
}
