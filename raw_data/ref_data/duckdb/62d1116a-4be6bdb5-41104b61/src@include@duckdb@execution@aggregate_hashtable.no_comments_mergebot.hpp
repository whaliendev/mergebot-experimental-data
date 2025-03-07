       
#include "duckdb/common/types/row/tuple_data_collection.hpp"
#include "duckdb/execution/base_aggregate_hashtable.hpp"
#include "duckdb/storage/buffer/buffer_handle.hpp"
namespace duckdb {
class BlockHandle;
class BufferHandle;
struct FlushMoveState;
struct aggr_ht_entry_64 {
 uint16_t salt;
 uint16_t page_offset;
 uint32_t page_nr;
};
struct aggr_ht_entry_32 {
 uint8_t salt;
 uint8_t page_nr;
 uint16_t page_offset;
};
enum HtEntryType { HT_WIDTH_32, HT_WIDTH_64 };
struct AggregateHTScanState {
 mutex lock;
 TupleDataScanState scan_state;
};
struct AggregateHTAppendState {
 AggregateHTAppendState();
 Vector ht_offsets;
 Vector hash_salts;
 SelectionVector group_compare_vector;
 SelectionVector no_match_vector;
 SelectionVector empty_vector;
 SelectionVector new_groups;
 Vector addresses;
 unique_ptr<UnifiedVectorFormat[]> group_data;
 DataChunk group_chunk;
};
class GroupedAggregateHashTable : public BaseAggregateHashTable {
public:
 constexpr static float LOAD_FACTOR = 1.5;
 constexpr static uint8_t HASH_WIDTH = sizeof(hash_t);
 GroupedAggregateHashTable(ClientContext &context, Allocator &allocator, vector<LogicalType> group_types,
                           vector<LogicalType> payload_types, const vector<BoundAggregateExpression *> &aggregates,
                           HtEntryType entry_type = HtEntryType::HT_WIDTH_64,
                           idx_t initial_capacity = InitialCapacity());
 GroupedAggregateHashTable(ClientContext &context, Allocator &allocator, vector<LogicalType> group_types,
                           vector<LogicalType> payload_types, vector<AggregateObject> aggregates,
                           HtEntryType entry_type = HtEntryType::HT_WIDTH_64,
                           idx_t initial_capacity = InitialCapacity());
 GroupedAggregateHashTable(ClientContext &context, Allocator &allocator, vector<LogicalType> group_types);
 ~GroupedAggregateHashTable() override;
 idx_t AddChunk(AggregateHTAppendState &state, DataChunk &groups, DataChunk &payload, const vector<idx_t> &filter);
 idx_t AddChunk(AggregateHTAppendState &state, DataChunk &groups, Vector &group_hashes, DataChunk &payload,
                const vector<idx_t> &filter);
 idx_t AddChunk(AggregateHTAppendState &state, DataChunk &groups, DataChunk &payload, AggregateType filter);
 idx_t Scan(TupleDataParallelScanState &gstate, TupleDataLocalScanState &lstate, DataChunk &result);
 void FetchAggregates(DataChunk &groups, DataChunk &result);
 idx_t FindOrCreateGroups(AggregateHTAppendState &state, DataChunk &groups, Vector &group_hashes,
                          Vector &addresses_out, SelectionVector &new_groups_out);
 idx_t FindOrCreateGroups(AggregateHTAppendState &state, DataChunk &groups, Vector &addresses_out,
                          SelectionVector &new_groups_out);
 void FindOrCreateGroups(AggregateHTAppendState &state, DataChunk &groups, Vector &addresses_out);
 void Combine(GroupedAggregateHashTable &other);
 TupleDataCollection &GetDataCollection() {
  return *data_collection;
 }
 idx_t Count() {
  return data_collection->Count();
 }
 static idx_t InitialCapacity();
 idx_t Capacity() {
  return capacity;
 }
 idx_t ResizeThreshold();
 idx_t MaxCapacity();
 static idx_t GetMaxCapacity(HtEntryType entry_type, idx_t tuple_size);
 void Partition(vector<GroupedAggregateHashTable *> &partition_hts, idx_t radix_bits);
 void InitializeFirstPart();
 void Finalize();
private:
 HtEntryType entry_type;
 idx_t tuple_size;
 idx_t tuples_per_block;
 unique_ptr<TupleDataCollection> data_collection;
 TupleDataAppendState append_state;
 idx_t capacity;
 vector<data_ptr_t> payload_hds_ptrs;
 BufferHandle hashes_hdl;
 data_ptr_t hashes_hdl_ptr;
 idx_t hash_offset;
 hash_t hash_prefix_shift;
 hash_t bitmask;
 bool is_finalized;
 vector<ExpressionType> predicates;
 GroupedAggregateHashTable(const GroupedAggregateHashTable &)
     void Destroy();
 void Verify();
 void UpdateBlockPointers();
};
}
