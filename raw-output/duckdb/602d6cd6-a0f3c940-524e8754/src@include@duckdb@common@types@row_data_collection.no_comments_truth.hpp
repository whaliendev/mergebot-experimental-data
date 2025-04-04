       
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/buffer_manager.hpp"
namespace duckdb {
struct RowDataBlock {
public:
 RowDataBlock(BufferManager &buffer_manager, idx_t capacity, idx_t entry_size)
     : capacity(capacity), entry_size(entry_size), count(0), byte_offset(0) {
  idx_t size = MaxValue<idx_t>(Storage::BLOCK_SIZE, capacity * entry_size);
  buffer_manager.Allocate(size, false, &block);
  D_ASSERT(BufferManager::GetAllocSize(size) == block->GetMemoryUsage());
 }
 explicit RowDataBlock(idx_t entry_size) : entry_size(entry_size) {
 }
 shared_ptr<BlockHandle> block;
 idx_t capacity;
 const idx_t entry_size;
 idx_t count;
 idx_t byte_offset;
private:
 RowDataBlock(const RowDataBlock &) = delete;
public:
 unique_ptr<RowDataBlock> Copy() {
  auto result = make_unique<RowDataBlock>(entry_size);
  result->block = block;
  result->capacity = capacity;
  result->count = count;
  result->byte_offset = byte_offset;
  return result;
 }
};
struct BlockAppendEntry {
 BlockAppendEntry(data_ptr_t baseptr, idx_t count) : baseptr(baseptr), count(count) {
 }
 data_ptr_t baseptr;
 idx_t count;
};
class RowDataCollection {
public:
 RowDataCollection(BufferManager &buffer_manager, idx_t block_capacity, idx_t entry_size, bool keep_pinned = false);
 unique_ptr<RowDataCollection> CloneEmpty(bool keep_pinned = false) const {
  return make_unique<RowDataCollection>(buffer_manager, block_capacity, entry_size, keep_pinned);
 }
 BufferManager &buffer_manager;
 idx_t count;
 idx_t block_capacity;
 idx_t entry_size;
 vector<unique_ptr<RowDataBlock>> blocks;
 vector<BufferHandle> pinned_blocks;
 const bool keep_pinned;
public:
 idx_t AppendToBlock(RowDataBlock &block, BufferHandle &handle, vector<BlockAppendEntry> &append_entries,
                     idx_t remaining, idx_t entry_sizes[]);
 RowDataBlock &CreateBlock();
 vector<BufferHandle> Build(idx_t added_count, data_ptr_t key_locations[], idx_t entry_sizes[],
                            const SelectionVector *sel = FlatVector::IncrementalSelectionVector());
 void Merge(RowDataCollection &other);
 void Clear() {
  blocks.clear();
  pinned_blocks.clear();
  count = 0;
 }
 idx_t SizeInBytes() const {
  VerifyBlockSizes();
  idx_t size = 0;
  for (auto &block : blocks) {
   size += block->block->GetMemoryUsage();
  }
  return size;
 }
 void VerifyBlockSizes() const {
#ifdef DEBUG
  for (auto &block : blocks) {
   D_ASSERT(block->block->GetMemoryUsage() == BufferManager::GetAllocSize(block->capacity * entry_size));
  }
#endif
 }
 static inline idx_t EntriesPerBlock(idx_t width) {
  return Storage::BLOCK_SIZE / width;
 }
private:
 mutex rdc_lock;
 RowDataCollection(const RowDataCollection &) = delete;
};
}
