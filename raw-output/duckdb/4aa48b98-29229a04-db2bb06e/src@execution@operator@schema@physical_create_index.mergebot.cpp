
#include "duckdb/execution/operator/schema/physical_create_index.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/main/database_manager.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
class CreateIndexGlobalSinkState : public GlobalSinkState {
public:
 //! Global index to be added to the table
 unique_ptr<Index> global_index;
};

class CreateIndexLocalSinkState : public LocalSinkState {
public:
explicitCreateIndexLocalSinkState(ClientContext &context): arena_allocator(Allocator::Get(context)) {} unique_ptr<Index> local_index;
 
 ArenaAllocator arena_allocator;
 vector<Key> keys;
 DataChunk key_chunk;
 vector<column_t> key_column_ids;
};

unique_ptr<GlobalSinkState> PhysicalCreateIndex::GetGlobalSinkState(ClientContext &context) const {
	auto state = make_unique<CreateIndexGlobalSinkState>();

	// create the global index
	switch (info->index_type) {
	case IndexType::ART: {
		state->global_index = make_unique<ART>(storage_ids, TableIOManager::Get(*table.storage), unbound_expressions,
		                                       info->constraint_type, table.storage->db);
		break;
	}
	default:
		throw InternalException("Unimplemented index type");
	}
<<<<<<< HEAD
	return (move(state));
||||||| db2bb06ef5

	return (move(state));
=======

	return (std::move(state));
>>>>>>> 29229a04
}

unique_ptr<LocalSinkState> PhysicalCreateIndex::GetLocalSinkState(ExecutionContext &context) const {

	auto state = make_unique<CreateIndexLocalSinkState>(context.client);

	// create the local index
	switch (info->index_type) {
	case IndexType::ART: {
		state->local_index = make_unique<ART>(storage_ids, TableIOManager::Get(*table.storage), unbound_expressions,
		                                      info->constraint_type, table.storage->db);
		break;
	}
	default:
		throw InternalException("Unimplemented index type");
	}
	state->keys = vector<Key>(STANDARD_VECTOR_SIZE);
	state->key_chunk.Initialize(Allocator::Get(context.client), state->local_index->logical_types);

<<<<<<< HEAD
	for (idx_t i = 0; i < state->key_chunk.ColumnCount(); i++) {
		state->key_column_ids.push_back(i);
||||||| db2bb06ef5
	state->key_chunk.Initialize(allocator, state->local_index->logical_types);

	// ordering of the entries of the index
	vector<BoundOrderByNode> orders;
	for (idx_t i = 0; i < state->local_index->logical_types.size(); i++) {
		auto col_expr = make_unique_base<Expression, BoundReferenceExpression>(state->local_index->logical_types[i], i);
		orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_FIRST, move(col_expr));
=======
	state->key_chunk.Initialize(allocator, state->local_index->logical_types);

	// ordering of the entries of the index
	vector<BoundOrderByNode> orders;
	for (idx_t i = 0; i < state->local_index->logical_types.size(); i++) {
		auto col_expr = make_unique_base<Expression, BoundReferenceExpression>(state->local_index->logical_types[i], i);
		orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_FIRST, std::move(col_expr));
>>>>>>> 29229a04
	}
<<<<<<< HEAD
	return move(state);
||||||| db2bb06ef5

	// row layout of the global sort state
	state->payload_types = state->local_index->logical_types;
	state->payload_types.emplace_back(LogicalType::ROW_TYPE);
	state->payload_layout.Initialize(state->payload_types);

	// initialize global and local sort state
	auto &buffer_manager = BufferManager::GetBufferManager(table.storage->db);
	state->global_sort_state = make_unique<GlobalSortState>(buffer_manager, orders, state->payload_layout);
	state->local_sort_state.Initialize(*state->global_sort_state, buffer_manager);

	return move(state);
=======

	// row layout of the global sort state
	state->payload_types = state->local_index->logical_types;
	state->payload_types.emplace_back(LogicalType::ROW_TYPE);
	state->payload_layout.Initialize(state->payload_types);

	// initialize global and local sort state
	auto &buffer_manager = BufferManager::GetBufferManager(table.storage->db);
	state->global_sort_state = make_unique<GlobalSortState>(buffer_manager, orders, state->payload_layout);
	state->local_sort_state.Initialize(*state->global_sort_state, buffer_manager);

	return std::move(state);
>>>>>>> 29229a04
}

SinkResultType PhysicalCreateIndex::Sink(ExecutionContext &context, GlobalSinkState &gstate_p, LocalSinkState &lstate_p,
                                         DataChunk &input) const {

	D_ASSERT(input.ColumnCount() >= 2);
	auto &lstate = (CreateIndexLocalSinkState &)lstate_p;
	auto &row_identifiers = input.data[input.ColumnCount() - 1];

	// generate the keys for the given input
	lstate.key_chunk.ReferenceColumns(input, lstate.key_column_ids);
	lstate.arena_allocator.Reset();
	ART::GenerateKeys(lstate.arena_allocator, lstate.key_chunk, lstate.keys);

	auto art = make_unique<ART>(lstate.local_index->column_ids, lstate.local_index->table_io_manager,
	                            lstate.local_index->unbound_expressions, lstate.local_index->constraint_type,
	                            table.storage->db);
	art->ConstructFromSorted(lstate.key_chunk.size(), lstate.keys, row_identifiers);

	// merge into the local ART
	{
		IndexLock local_lock;
		lstate.local_index->InitializeLock(local_lock);
		if (!lstate.local_index->MergeIndexes(local_lock, art.get())) {
			throw ConstraintException("Data contains duplicates on indexed column(s)");
		}
	}
	return SinkResultType::NEED_MORE_INPUT;
}

void PhysicalCreateIndex::Combine(ExecutionContext &context, GlobalSinkState &gstate_p,
                                  LocalSinkState &lstate_p) const {

	auto &gstate = (CreateIndexGlobalSinkState &)gstate_p;
	auto &lstate = (CreateIndexLocalSinkState &)lstate_p;

	// merge the local index into the global index
	gstate.global_index->MergeIndexes(lstate.local_index.get());
}

SinkFinalizeType PhysicalCreateIndex::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                               GlobalSinkState &gstate_p) const {

	// here, we just set the resulting global index as the newly created index of the table

	auto &state = (CreateIndexGlobalSinkState &)gstate_p;

	if (!table.storage->IsRoot()) {
		throw TransactionException("Transaction conflict: cannot add an index to a table that has been altered!");
	}

	auto &schema = *table.schema;
	auto index_entry = (IndexCatalogEntry *)schema.CreateIndex(context, info.get(), &table);
	if (!index_entry) {
		// index already exists, but error ignored because of IF NOT EXISTS
		return SinkFinalizeType::READY;
	}

	index_entry->index = state.global_index.get();
	index_entry->info = table.storage->info;
	for (auto &parsed_expr : info->parsed_expressions) {
		index_entry->parsed_expressions.push_back(parsed_expr->Copy());
	}

	table.storage->info->indexes.AddIndex(std::move(state.global_index));
	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
void PhysicalCreateIndex::GetData(ExecutionContext &context, DataChunk &chunk, GlobalSourceState &gstate,
                                  LocalSourceState &lstate) const {
	// NOP
}

} // namespace duckdb
