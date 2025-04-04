       
#include "duckdb/common/types/column_data_collection.hpp"
namespace duckdb {
struct ChunkMetaData;
struct VectorMetaData;
struct BlockMetaData {
 shared_ptr<BlockHandle> handle;
 uint32_t size;
 uint32_t capacity;
 uint32_t Capacity();
};
class ColumnDataAllocator {
public:
 ColumnDataAllocator(Allocator &allocator);
 ColumnDataAllocator(BufferManager &buffer_manager);
 ColumnDataAllocator(ClientContext &context, ColumnDataAllocatorType allocator_type);
 Allocator &GetAllocator();
 ColumnDataAllocatorType GetType() {
  return type;
 }
 void MakeShared() {
  shared = true;
 }
 idx_t BlockCount() const {
  return blocks.size();
 }
 void AllocateData(idx_t size, uint32_t &block_id, uint32_t &offset, ChunkManagementState *chunk_state);
 void Initialize(ColumnDataAllocator &other);
 void InitializeChunkState(ChunkManagementState &state, ChunkMetaData &meta_data);
 data_ptr_t GetDataPointer(ChunkManagementState &state, uint32_t block_id, uint32_t offset);
 void UnswizzlePointers(ChunkManagementState &state, Vector &result, uint16_t v_offset, uint16_t count,
                        uint32_t block_id, uint32_t offset);
 void DeleteBlock(uint32_t block_id);
private:
 void AllocateEmptyBlock(idx_t size);
 void AllocateBlock(idx_t size);
 BufferHandle Pin(uint32_t block_id);
 bool HasBlocks() const {
  return !blocks.empty();
 }
 void AllocateBuffer(idx_t size, uint32_t &block_id, uint32_t &offset, ChunkManagementState *chunk_state);
 void AllocateMemory(idx_t size, uint32_t &block_id, uint32_t &offset, ChunkManagementState *chunk_state);
 void AssignPointer(uint32_t &block_id, uint32_t &offset, data_ptr_t pointer);
 ColumnDataAllocatorType type;
 union {
  Allocator *allocator;
  BufferManager *buffer_manager;
 } alloc;
 vector<BlockMetaData> blocks;
 vector<AllocatedData> allocated_data;
 bool shared = false;
 mutex lock;
};
}
