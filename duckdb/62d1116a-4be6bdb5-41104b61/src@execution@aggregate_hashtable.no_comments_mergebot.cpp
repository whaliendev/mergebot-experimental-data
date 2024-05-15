#include "duckdb/execution/aggregate_hashtable.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/radix_partitioning.hpp"
#include "duckdb/common/row_operations/row_operations.hpp"
#include "duckdb/common/types/null_value.hpp"
#include "duckdb/common/types/row_data_collection.hpp"
#include "duckdb/common/types/row/tuple_data_iterator.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include <cmath>
namespace duckdb {
using ValidityBytes = TupleDataLayout::ValidityBytes;
GroupedAggregateHashTable::GroupedAggregateHashTable(ClientContext &context, Allocator &allocator,
                                                     vector<LogicalType> group_types, vector<LogicalType> payload_types,
                                                     const vector<BoundAggregateExpression *> &bindings,
                                                     HtEntryType entry_type, idx_t initial_capacity)
    : GroupedAggregateHashTable(context, allocator, std::move(group_types), std::move(payload_types),
                                AggregateObject::CreateAggregateObjects(bindings), entry_type, initial_capacity) {
}
GroupedAggregateHashTable::GroupedAggregateHashTable(ClientContext &context, Allocator &allocator,
                                                     vector<LogicalType> group_types)
    : GroupedAggregateHashTable(context, allocator, std::move(group_types), {
}
AggregateHTAppendState::AggregateHTAppendState(): ht_offsets(LogicalTypeId::BIGINT), hash_salts(LogicalTypeId::SMALLINT), group_compare_vector(STANDARD_VECTOR_SIZE), no_match_vector(STANDARD_VECTOR_SIZE), empty_vector(STANDARD_VECTOR_SIZE), new_groups(STANDARD_VECTOR_SIZE), addresses(LogicalType::POINTER) {
}
GroupedAggregateHashTable::GroupedAggregateHashTable(ClientContext &context, Allocator &allocator, vector<LogicalType> group_types_p, vector<LogicalType> payload_types_p, vector<AggregateObject> aggregate_objects_p, HtEntryType entry_type, idx_t initial_capacity): BaseAggregateHashTable(context, allocator, aggregate_objects_p, std::move(payload_types_p)), entry_type(entry_type), capacity(0), entries(0), payload_page_offset(0), is_finalized(false) {
 group_types_p.emplace_back(LogicalType::HASH);
 layout.Initialize(std::move(group_types_p), std::move(aggregate_objects_p));
 hash_offset = layout.GetOffsets()[layout.ColumnCount() - 1];
 tuple_size = layout.GetRowWidth();
 D_ASSERT(tuple_size <= Storage::BLOCK_SIZE);
 tuples_per_block = Storage::BLOCK_SIZE / tuple_size;
 hashes_hdl = buffer_manager.Allocate(Storage::BLOCK_SIZE);
 hashes_hdl_ptr = hashes_hdl.Ptr();
 switch (entry_type) {
 case HtEntryType::HT_WIDTH_64: {
  hash_prefix_shift = (HASH_WIDTH - sizeof(aggr_ht_entry_64::salt)) * 8;
  Resize<aggr_ht_entry_64>(initial_capacity);
  break;
 }
 case HtEntryType::HT_WIDTH_32: {
  hash_prefix_shift = (HASH_WIDTH - sizeof(aggr_ht_entry_32::salt)) * 8;
  Resize<aggr_ht_entry_32>(initial_capacity);
  break;
 }
 default:
  throw InternalException("Unknown HT entry width");
 }
 predicates.resize(layout.ColumnCount() - 1, ExpressionType::COMPARE_EQUAL);
 string_heap = make_unique<RowDataCollection>(buffer_manager, (idx_t)Storage::BLOCK_SIZE, 1, true);
}
GroupedAggregateHashTable::~GroupedAggregateHashTable() {
 Destroy();
}
void GroupedAggregateHashTable::Destroy() {
 if (data_collection->Count() == 0) {
  return;
 }
 bool has_destructor = false;
 for (auto &aggr : layout.GetAggregates()) {
  if (aggr.function.destructor) {
   has_destructor = true;
  }
 }
 if (!has_destructor) {
  return;
 }
 TupleDataChunkIterator iterator(*data_collection, TupleDataPinProperties::DESTROY_AFTER_DONE, false);
 auto &row_locations = iterator.GetChunkState().row_locations;
 do {
  RowOperations::DestroyStates(layout, row_locations, iterator.GetCount());
 } while (iterator.Next());
 data_collection->Reset();
}
template <class ENTRY>
void GroupedAggregateHashTable::VerifyInternal() {
 auto hashes_ptr = (ENTRY *)hashes_hdl_ptr;
 idx_t count = 0;
 for (idx_t i = 0; i < capacity; i++) {
  if (hashes_ptr[i].page_nr > 0) {
   D_ASSERT(hashes_ptr[i].page_offset < tuples_per_block);
   D_ASSERT(hashes_ptr[i].page_nr <= payload_hds_ptrs.size());
   auto ptr = payload_hds_ptrs[hashes_ptr[i].page_nr - 1] + ((hashes_ptr[i].page_offset) * tuple_size);
   auto hash = Load<hash_t>(ptr + hash_offset);
   D_ASSERT((hashes_ptr[i].salt) == (hash >> hash_prefix_shift));
   count++;
  }
 }
 (void)count;
 D_ASSERT(count == Count());
}
idx_t GroupedAggregateHashTable::InitialCapacity() {
 return STANDARD_VECTOR_SIZE * 2ULL;
}
idx_t GroupedAggregateHashTable::GetMaxCapacity(HtEntryType entry_type, idx_t tuple_size) {
 idx_t max_pages;
 idx_t max_tuples;
 switch (entry_type) {
 case HtEntryType::HT_WIDTH_32:
  max_pages = NumericLimits<uint8_t>::Maximum();
  max_tuples = NumericLimits<uint16_t>::Maximum();
  break;
 case HtEntryType::HT_WIDTH_64:
  max_pages = NumericLimits<uint32_t>::Maximum();
  max_tuples = NumericLimits<uint16_t>::Maximum();
  break;
 default:
  throw InternalException("Unsupported hash table width");
 }
 return max_pages * MinValue(max_tuples, (idx_t)Storage::BLOCK_SIZE / tuple_size);
}
idx_t GroupedAggregateHashTable::MaxCapacity() {
 return GetMaxCapacity(entry_type, tuple_size);
}
void GroupedAggregateHashTable::Verify() {
#ifdef DEBUG
 switch (entry_type) {
 case HtEntryType::HT_WIDTH_32:
  VerifyInternal<aggr_ht_entry_32>();
  break;
 case HtEntryType::HT_WIDTH_64:
  VerifyInternal<aggr_ht_entry_64>();
  break;
 }
#endif
}
template <class ENTRY>
void GroupedAggregateHashTable::Resize(idx_t size) {
 D_ASSERT(!is_finalized);
 D_ASSERT(size >= STANDARD_VECTOR_SIZE);
 D_ASSERT(IsPowerOfTwo(size));
 if (size < capacity) {
  throw InternalException("Cannot downsize a hash table!");
 }
 capacity = size;
 bitmask = size - 1;
 const auto byte_size = capacity * sizeof(ENTRY);
 if (byte_size > (idx_t)Storage::BLOCK_SIZE) {
  hashes_hdl = buffer_manager.Allocate(byte_size);
  hashes_hdl_ptr = hashes_hdl.Ptr();
 }
 memset(hashes_hdl_ptr, 0, byte_size);
<<<<<<< HEAD
 if (Count() != 0) {
  D_ASSERT(!payload_hds_ptrs.empty());
|||||||
 hashes_end_ptr = hashes_hdl_ptr + byte_size;
 capacity = size;
=======
 capacity = size;
>>>>>>> 4be6bdb565f5cdd5618fc012b77a3bd579105255
  auto hashes_arr = (ENTRY *)hashes_hdl_ptr;
  idx_t block_id = 0;
  auto block_pointer = payload_hds_ptrs[block_id];
  auto block_end = block_pointer + tuples_per_block * tuple_size;
  TupleDataChunkIterator iterator(*data_collection, TupleDataPinProperties::ALREADY_PINNED, false);
  auto row_locations = iterator.GetRowLocations();
  do {
   for (idx_t i = 0; i < iterator.GetCount(); i++) {
    const auto &row_location = row_locations[i];
    if (row_location > block_end || row_location < block_pointer) {
     block_id++;
     block_pointer = payload_hds_ptrs[block_id];
     block_end = block_pointer + tuples_per_block * tuple_size;
    }
    D_ASSERT(row_location >= block_pointer && row_location < block_end);
    D_ASSERT((row_location - block_pointer) % tuple_size == 0);
    const auto hash = Load<hash_t>(row_location + hash_offset);
    D_ASSERT((hash & bitmask) == (hash % capacity));
    D_ASSERT(hash >> hash_prefix_shift <= NumericLimits<uint16_t>::Maximum());
    auto entry_idx = (idx_t)hash & bitmask;
    while (hashes_arr[entry_idx].page_nr > 0) {
     entry_idx++;
     if (entry_idx >= capacity) {
      entry_idx = 0;
     }
    }
    auto &ht_entry = hashes_arr[entry_idx];
    D_ASSERT(!ht_entry.page_nr);
    ht_entry.salt = hash >> hash_prefix_shift;
    ht_entry.page_nr = block_id + 1;
    ht_entry.page_offset = (row_location - block_pointer) / tuple_size;
   }
  } while (iterator.Next());
 }
 Verify();
}
idx_t GroupedAggregateHashTable::AddChunk(AggregateHTAppendState &state, DataChunk &groups, DataChunk &payload, AggregateType filter) {
 vector<idx_t> aggregate_filter;
 auto &aggregates = layout.GetAggregates();
 for (idx_t i = 0; i < aggregates.size(); i++) {
  auto &aggregate = aggregates[i];
  if (aggregate.aggr_type == filter) {
   aggregate_filter.push_back(i);
  }
 }
 return AddChunk(state, groups, payload, aggregate_filter);
}
idx_t GroupedAggregateHashTable::AddChunk(AggregateHTAppendState &state, DataChunk &groups, DataChunk &payload, const vector<idx_t> &filter) {
 Vector hashes(LogicalType::HASH);
 groups.Hash(hashes);
 return AddChunk(state, groups, hashes, payload, filter);
}
idx_t GroupedAggregateHashTable::AddChunk(AggregateHTAppendState &state, DataChunk &groups, Vector &group_hashes,
                                          DataChunk &payload, const vector<idx_t> &filter) {
 D_ASSERT(!is_finalized);
 if (groups.size() == 0) {
  return 0;
 }
#ifdef DEBUG
 D_ASSERT(groups.ColumnCount() + 1 == layout.ColumnCount());
 for (idx_t i = 0; i < groups.ColumnCount(); i++) {
  D_ASSERT(groups.GetTypes()[i] == layout.GetTypes()[i]);
 }
#endif
<<<<<<< HEAD
 Vector addresses(LogicalType::POINTER);
 SelectionVector new_groups(STANDARD_VECTOR_SIZE);
 auto new_group_count = FindOrCreateGroups(groups, group_hashes, addresses, new_groups);
 VectorOperations::AddInPlace(addresses, layout.GetAggrOffset(), payload.size());
|||||||
 Vector addresses(LogicalType::POINTER);
 auto new_group_count = FindOrCreateGroups(groups, group_hashes, addresses, new_groups);
 VectorOperations::AddInPlace(addresses, layout.GetAggrOffset(), payload.size());
=======
 auto new_group_count = FindOrCreateGroups(state, groups, group_hashes, state.addresses, state.new_groups);
 VectorOperations::AddInPlace(state.addresses, layout.GetAggrOffset(), payload.size());
>>>>>>> 4be6bdb565f5cdd5618fc012b77a3bd579105255
 auto &aggregates = layout.GetAggregates();
 idx_t filter_idx = 0;
 idx_t payload_idx = 0;
 for (idx_t i = 0; i < aggregates.size(); i++) {
  auto &aggr = aggregates[i];
  if (filter_idx >= filter.size() || i < filter[filter_idx]) {
   payload_idx += aggr.child_count;
   VectorOperations::AddInPlace(state.addresses, aggr.payload_size, payload.size());
   continue;
  }
  D_ASSERT(i == filter[filter_idx]);
  if (aggr.aggr_type != AggregateType::DISTINCT && aggr.filter) {
   RowOperations::UpdateFilteredStates(filter_set.GetFilterData(i), aggr, state.addresses, payload,
                                       payload_idx);
  } else {
   RowOperations::UpdateStates(aggr, state.addresses, payload, payload_idx, payload.size());
  }
  payload_idx += aggr.child_count;
  VectorOperations::AddInPlace(state.addresses, aggr.payload_size, payload.size());
  filter_idx++;
 }
 Verify();
 return new_group_count;
}
void GroupedAggregateHashTable::FetchAggregates(DataChunk &groups, DataChunk &result) {
 groups.Verify();
 D_ASSERT(groups.ColumnCount() + 1 == layout.ColumnCount());
 for (idx_t i = 0; i < result.ColumnCount(); i++) {
  D_ASSERT(result.data[i].GetType() == payload_types[i]);
 }
 result.SetCardinality(groups);
 if (groups.size() == 0) {
  return;
 }
 AggregateHTAppendState append_state;
 Vector addresses(LogicalType::POINTER);
 FindOrCreateGroups(append_state, groups, addresses);
 RowOperations::FinalizeStates(layout, addresses, result, 0);
}
idx_t GroupedAggregateHashTable::ResizeThreshold() {
 return capacity / LOAD_FACTOR;
}
template <class ENTRY>
idx_t GroupedAggregateHashTable::FindOrCreateGroupsInternal(AggregateHTAppendState &state, DataChunk &groups, Vector &group_hashes, Vector &addresses, SelectionVector &new_groups_out) {
 D_ASSERT(!is_finalized);
 if (entries + groups.size() > MaxCapacity()) {
  throw InternalException("Hash table capacity reached");
 }
 if (capacity - entries <= groups.size() || entries > ResizeThreshold()) {
  Resize<ENTRY>(capacity * 2);
 }
 D_ASSERT(capacity - entries >= groups.size());
 D_ASSERT(groups.ColumnCount() + 1 == layout.ColumnCount());
 D_ASSERT(capacity - entries >= groups.size());
 D_ASSERT(group_hashes.GetType() == LogicalType::HASH);
 group_hashes.Flatten(groups.size());
 auto group_hashes_ptr = FlatVector::GetData<hash_t>(group_hashes);
 D_ASSERT(state.ht_offsets.GetVectorType() == VectorType::FLAT_VECTOR);
 D_ASSERT(state.ht_offsets.GetType() == LogicalType::BIGINT);
 D_ASSERT(addresses.GetType() == LogicalType::POINTER);
 addresses.Flatten(groups.size());
 auto addresses_ptr = FlatVector::GetData<data_ptr_t>(addresses);
 D_ASSERT(state.hash_salts.GetType() == LogicalType::SMALLINT);
 auto ht_offsets_ptr = FlatVector::GetData<uint64_t>(state.ht_offsets);
 auto hash_salts_ptr = FlatVector::GetData<uint16_t>(state.hash_salts);
 for (idx_t r = 0; r < groups.size(); r++) {
  auto element = group_hashes_ptr[r];
  D_ASSERT((element & bitmask) == (element % capacity));
  ht_offsets_ptr[r] = element & bitmask;
  hash_salts_ptr[r] = element >> hash_prefix_shift;
 }
 const SelectionVector *sel_vector = FlatVector::IncrementalSelectionVector();
 idx_t remaining_entries = groups.size();
 if (state.group_chunk.ColumnCount() == 0) {
  state.group_chunk.InitializeEmpty(layout.GetTypes());
 }
 D_ASSERT(state.group_chunk.ColumnCount() == layout.GetTypes().size());
 for (idx_t grp_idx = 0; grp_idx < groups.ColumnCount(); grp_idx++) {
  state.group_chunk.data[grp_idx].Reference(groups.data[grp_idx]);
 }
 state.group_chunk.data[groups.ColumnCount()].Reference(group_hashes);
 state.group_chunk.SetCardinality(groups);
 if (!state.group_data) {
  state.group_data = unique_ptr<UnifiedVectorFormat[]>(new UnifiedVectorFormat[state.group_chunk.ColumnCount()]);
 }
 for (idx_t col_idx = 0; col_idx < state.group_chunk.ColumnCount(); col_idx++) {
  state.group_chunk.data[col_idx].ToUnifiedFormat(state.group_chunk.size(), state.group_data[col_idx]);
 }
 idx_t new_group_count = 0;
 while (remaining_entries > 0) {
  idx_t new_entry_count = 0;
  idx_t need_compare_count = 0;
  idx_t no_match_count = 0;
  for (idx_t i = 0; i < remaining_entries; i++) {
   const idx_t index = sel_vector->get_index(i);
   const auto ht_entry_ptr = ((ENTRY *)this->hashes_hdl_ptr) + ht_offsets_ptr[index];
   if (ht_entry_ptr->page_nr == 0) {
    if (payload_page_offset == tuples_per_block || payload_hds.empty()) {
     NewBlock();
    }
    auto entry_payload_ptr = payload_hds_ptrs.back() + (payload_page_offset * tuple_size);
    D_ASSERT(group_hashes_ptr[index] >> hash_prefix_shift <= NumericLimits<uint16_t>::Maximum());
    D_ASSERT(payload_page_offset < tuples_per_block);
    D_ASSERT(payload_hds.size() < NumericLimits<uint32_t>::Maximum());
    D_ASSERT(payload_page_offset + 1 < NumericLimits<uint16_t>::Maximum());
    ht_entry_ptr->salt = group_hashes_ptr[index] >> hash_prefix_shift;
    ht_entry_ptr->page_nr = payload_hds.size();
    ht_entry_ptr->page_offset = payload_page_offset++;
    state.empty_vector.set_index(new_entry_count++, index);
    new_groups_out.set_index(new_group_count++, index);
    entries++;
    addresses_ptr[index] = entry_payload_ptr;
   } else {
    if (ht_entry_ptr->salt == hash_salts_ptr[index]) {
     state.group_compare_vector.set_index(need_compare_count++, index);
     auto page_ptr = payload_hds_ptrs[ht_entry_ptr->page_nr - 1];
     auto page_offset = ht_entry_ptr->page_offset * tuple_size;
     addresses_ptr[index] = page_ptr + page_offset;
    } else {
     state.no_match_vector.set_index(no_match_count++, index);
    }
   }
  }
  RowOperations::Scatter(state.group_chunk, state.group_data.get(), layout, addresses, *string_heap,
                         state.empty_vector, new_entry_count);
  RowOperations::InitializeStates(layout, addresses, state.empty_vector, new_entry_count);
  RowOperations::Match(state.group_chunk, state.group_data.get(), layout, addresses, predicates,
                       state.group_compare_vector, need_compare_count, &state.no_match_vector, no_match_count);
  for (idx_t i = 0; i < no_match_count; i++) {
   idx_t index = state.no_match_vector.get_index(i);
   ht_offsets_ptr[index]++;
   if (ht_offsets_ptr[index] >= capacity) {
    ht_offsets_ptr[index] = 0;
   }
  }
  sel_vector = &state.no_match_vector;
  remaining_entries = no_match_count;
 }
 return new_group_count;
}
idx_t GroupedAggregateHashTable::FindOrCreateGroups(AggregateHTAppendState &state, DataChunk &groups, Vector &group_hashes, Vector &addresses_out, SelectionVector &new_groups_out) {
 switch (entry_type) {
 case HtEntryType::HT_WIDTH_64:
  return FindOrCreateGroupsInternal<aggr_ht_entry_64>(state, groups, group_hashes, addresses_out, new_groups_out);
 case HtEntryType::HT_WIDTH_32:
  return FindOrCreateGroupsInternal<aggr_ht_entry_32>(state, groups, group_hashes, addresses_out, new_groups_out);
 default:
  throw InternalException("Unknown HT entry width");
 }
}
void GroupedAggregateHashTable::FindOrCreateGroups(AggregateHTAppendState &state, DataChunk &groups, Vector &addresses) {
 FindOrCreateGroups(state, groups, addresses, state.new_groups);
}
idx_t GroupedAggregateHashTable::FindOrCreateGroups(AggregateHTAppendState &state, DataChunk &groups,
                                                    Vector &addresses_out, SelectionVector &new_groups_out) {
 Vector hashes(LogicalType::HASH);
 groups.Hash(hashes);
 return FindOrCreateGroups(state, groups, hashes, addresses_out, new_groups_out);
}
struct FlushMoveState {
 DataChunk groups;
 idx_t hash_col_idx;
 Vector hashes;
 Vector group_addresses;
 SelectionVector new_groups_sel;
 explicitFlushMoveState(TupleDataCollection &collection_p): collection(collection_p), hashes(LogicalType::HASH), group_addresses(LogicalType::POINTER), new_groups_sel(STANDARD_VECTOR_SIZE) {
  const auto &layout = collection.GetLayout();
  vector<column_t> column_ids;
  column_ids.reserve(layout.ColumnCount() - 1);
  for (idx_t col_idx = 0; col_idx < layout.ColumnCount() - 1; col_idx++) {
   column_ids.emplace_back(col_idx);
  }
  collection.InitializeScan(scan_state, column_ids, TupleDataPinProperties::UNPIN_AFTER_DONE);
  collection.InitializeScanChunk(scan_state, groups);
  hash_col_idx = layout.ColumnCount() - 1;
 }
 bool Scan();
 TupleDataCollection &collection;
 TupleDataScanState scan_state;
};
bool FlushMoveState::Scan() {
 if (collection.Scan(scan_state, groups)) {
  collection.Gather(scan_state.chunk_state.row_locations, *FlatVector::IncrementalSelectionVector(),
                    groups.size(), hash_col_idx, hashes, *FlatVector::IncrementalSelectionVector());
  return true;
 }
 collection.FinalizePinState(scan_state.pin_state);
 return false;
}
void GroupedAggregateHashTable::Combine(GroupedAggregateHashTable &other) {
 D_ASSERT(!is_finalized);
 D_ASSERT(other.layout.GetAggrWidth() == layout.GetAggrWidth());
 D_ASSERT(other.layout.GetDataWidth() == layout.GetDataWidth());
 D_ASSERT(other.layout.GetRowWidth() == layout.GetRowWidth());
 if (other.Count() == 0) {
  return;
 }
 FlushMoveState state(*other.data_collection);
 while (state.Scan()) {
  FindOrCreateGroups(state.groups, state.hashes, state.group_addresses, state.new_groups_sel);
  RowOperations::CombineStates(layout, state.scan_state.chunk_state.row_locations, state.group_addresses,
                               state.groups.size());
 }
 Verify();
}
void GroupedAggregateHashTable::Partition(vector<GroupedAggregateHashTable *> &partition_hts, idx_t radix_bits) {
 const auto num_partitions = RadixPartitioning::NumberOfPartitions(radix_bits);
 D_ASSERT(partition_hts.size() == num_partitions);
 auto partitioned_data =
     make_unique<RadixPartitionedTupleData>(buffer_manager, layout, radix_bits, layout.ColumnCount() - 1);
 partitioned_data->Partition(*data_collection, TupleDataPinProperties::KEEP_EVERYTHING_PINNED);
 D_ASSERT(partitioned_data->GetPartitions().size() == num_partitions);
 auto &partitions = partitioned_data->GetPartitions();
 for (idx_t partition_idx = 0; partition_idx < num_partitions; partition_idx++) {
  auto &partition_ht = *partition_hts[partition_idx];
  partition_ht.data_collection = std::move(partitions[partition_idx]);
  partition_ht.InitializeFirstPart();
 }
}
void GroupedAggregateHashTable::InitializeFirstPart() {
 data_collection->GetBlockPointers(payload_hds_ptrs);
 auto size = MaxValue<idx_t>(NextPowerOfTwo(Count() * 2L), capacity);
 switch (entry_type) {
 case HtEntryType::HT_WIDTH_64:
  Resize<aggr_ht_entry_64>(size);
  break;
 case HtEntryType::HT_WIDTH_32:
  Resize<aggr_ht_entry_32>(size);
  break;
 default:
  throw InternalException("Unknown HT entry width");
 }
}
idx_t GroupedAggregateHashTable::Scan(TupleDataParallelScanState &gstate, TupleDataLocalScanState &lstate, DataChunk &result) {
 data_collection->Scan(gstate, lstate, result);
 const auto group_cols = layout.ColumnCount() - 1;
 RowOperations::FinalizeStates(layout, lstate.scan_state.chunk_state.row_locations, result, group_cols);
 return result.size();
}
void GroupedAggregateHashTable::Finalize() {
 if (is_finalized) {
  return;
 }
 hashes_hdl.Destroy();
 is_finalized = true;
}
void GroupedAggregateHashTable::UpdateBlockPointers() {
 for (const auto &id_and_handle : append_state.pin_state.row_handles) {
  const auto &id = id_and_handle.first;
  const auto &handle = id_and_handle.second;
  if (payload_hds_ptrs.empty() || id > payload_hds_ptrs.size() - 1) {
   payload_hds_ptrs.resize(id + 1);
  }
  payload_hds_ptrs[id] = handle.Ptr();
 }
}
}
