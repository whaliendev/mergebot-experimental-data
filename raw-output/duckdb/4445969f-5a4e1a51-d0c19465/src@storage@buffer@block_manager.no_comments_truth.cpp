#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
namespace duckdb {
shared_ptr<BlockHandle> BlockManager::RegisterBlock(block_id_t block_id, bool is_meta_block) {
 lock_guard<mutex> lock(blocks_lock);
 auto entry = blocks.find(block_id);
 if (entry != blocks.end()) {
  auto existing_ptr = entry->second.lock();
  if (existing_ptr) {
   return existing_ptr;
  }
 }
 auto result = make_shared<BlockHandle>(*this, block_id);
 if (is_meta_block) {
  meta_blocks[block_id] = result;
 }
 blocks[block_id] = weak_ptr<BlockHandle>(result);
 return result;
}
void BlockManager::ClearMetaBlockHandles() {
 meta_blocks.clear();
}
shared_ptr<BlockHandle> BlockManager::ConvertToPersistent(block_id_t block_id, shared_ptr<BlockHandle> old_block) {
 auto old_handle = buffer_manager.Pin(old_block);
 D_ASSERT(old_block->state == BlockState::BLOCK_LOADED);
 D_ASSERT(old_block->buffer);
 D_ASSERT(old_block->buffer->AllocSize() <= Storage::BLOCK_ALLOC_SIZE);
 auto new_block = RegisterBlock(block_id);
 D_ASSERT(new_block->state == BlockState::BLOCK_UNLOADED);
 D_ASSERT(new_block->readers == 0);
 new_block->state = BlockState::BLOCK_LOADED;
 new_block->buffer = CreateBlock(block_id, old_block->buffer.get());
 new_block->memory_usage = old_block->memory_usage;
 new_block->memory_charge = std::move(old_block->memory_charge);
 old_block->buffer.reset();
 old_block->state = BlockState::BLOCK_UNLOADED;
 old_block->memory_usage = 0;
 old_handle.Destroy();
 old_block.reset();
 Write(*new_block->buffer, block_id);
 buffer_manager.GetBufferPool().AddToEvictionQueue(new_block);
 return new_block;
}
void BlockManager::UnregisterBlock(block_id_t block_id, bool can_destroy) {
 if (block_id >= MAXIMUM_BLOCK) {
  buffer_manager.DeleteTemporaryFile(block_id);
 } else {
  lock_guard<mutex> lock(blocks_lock);
  blocks.erase(block_id);
 }
}
}
