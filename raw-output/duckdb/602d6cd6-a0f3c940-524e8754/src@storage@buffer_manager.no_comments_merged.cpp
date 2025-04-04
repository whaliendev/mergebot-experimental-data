#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/parallel/concurrentqueue.hpp"
#include "duckdb/storage/in_memory_block_manager.hpp"
#include "duckdb/storage/storage_manager.hpp"
namespace duckdb {
BufferPoolReservation::BufferPoolReservation(BufferPoolReservation &&src) noexcept {
 size = src.size;
 src.size = 0;
}
BufferPoolReservation &BufferPoolReservation::operator=(BufferPoolReservation &&src) noexcept {
 size = src.size;
 src.size = 0;
 return *this;
}
BufferPoolReservation::~BufferPoolReservation() {
 D_ASSERT(size == 0);
}
void BufferPoolReservation::Resize(atomic<idx_t> &counter, idx_t new_size) {
 int64_t delta = (int64_t)new_size - size;
 D_ASSERT(delta > 0 || (int64_t)counter >= -delta);
 counter += delta;
 size = new_size;
}
void BufferPoolReservation::Merge(BufferPoolReservation &&src) {
 size += src.size;
 src.size = 0;
}
struct BufferAllocatorData : PrivateAllocatorData {
 explicit BufferAllocatorData(BufferManager &manager) : manager(manager) {
 }
 BufferManager &manager;
};
BlockHandle::BlockHandle(BlockManager &block_manager, block_id_t block_id_p)
    : block_manager(block_manager), readers(0), block_id(block_id_p), buffer(nullptr), eviction_timestamp(0),
      can_destroy(false), unswizzled(nullptr) {
 eviction_timestamp = 0;
 state = BlockState::BLOCK_UNLOADED;
 memory_usage = Storage::BLOCK_ALLOC_SIZE;
}
BlockHandle::BlockHandle(BlockManager &block_manager, block_id_t block_id_p, unique_ptr<FileBuffer> buffer_p,
                         bool can_destroy_p, idx_t block_size, BufferPoolReservation &&reservation)
    : block_manager(block_manager), readers(0), block_id(block_id_p), eviction_timestamp(0), can_destroy(can_destroy_p),
      unswizzled(nullptr) {
 buffer = move(buffer_p);
 state = BlockState::BLOCK_LOADED;
 memory_usage = block_size;
 memory_charge = move(reservation);
}
BlockHandle::~BlockHandle() {
 unswizzled = nullptr;
 auto &buffer_manager = block_manager.buffer_manager;
 if (buffer && state == BlockState::BLOCK_LOADED) {
  D_ASSERT(memory_charge.size > 0);
  buffer.reset();
  memory_charge.Resize(buffer_manager.current_memory, 0);
 } else {
  D_ASSERT(memory_charge.size == 0);
 }
 block_manager.UnregisterBlock(block_id, can_destroy);
}
unique_ptr<Block> AllocateBlock(BlockManager &block_manager, unique_ptr<FileBuffer> reusable_buffer,
                                block_id_t block_id) {
 if (reusable_buffer) {
  if (reusable_buffer->type == FileBufferType::BLOCK) {
   auto &block = (Block &)*reusable_buffer;
   block.id = block_id;
   return unique_ptr_cast<FileBuffer, Block>(move(reusable_buffer));
  }
  auto block = block_manager.CreateBlock(block_id, reusable_buffer.get());
  reusable_buffer.reset();
  return block;
 } else {
  return block_manager.CreateBlock(block_id, nullptr);
 }
}
idx_t GetAllocSize(idx_t size) {
 return AlignValue<idx_t, Storage::SECTOR_SIZE>(size + Storage::BLOCK_HEADER_SIZE);
}
unique_ptr<FileBuffer> BufferManager::ConstructManagedBuffer(idx_t size, unique_ptr<FileBuffer> &&source,
                                                             FileBufferType type) {
 if (source) {
  auto tmp = move(source);
<<<<<<< HEAD
  D_ASSERT(tmp->AllocSize() == GetAllocSize(size));
||||||| 524e8754d6
  D_ASSERT(tmp->size == size);
=======
  D_ASSERT(tmp->AllocSize() == BufferManager::GetAllocSize(size));
>>>>>>> a0f3c940
  return make_unique<FileBuffer>(*tmp, type);
 } else {
  return make_unique<FileBuffer>(Allocator::Get(db), type, size);
 }
}
BufferHandle BlockHandle::Load(shared_ptr<BlockHandle> &handle, unique_ptr<FileBuffer> reusable_buffer) {
 if (handle->state == BlockState::BLOCK_LOADED) {
  D_ASSERT(handle->buffer);
  return BufferHandle(handle, handle->buffer.get());
 }
 auto &block_manager = handle->block_manager;
 if (handle->block_id < MAXIMUM_BLOCK) {
  auto block = AllocateBlock(block_manager, move(reusable_buffer), handle->block_id);
  block_manager.Read(*block);
  handle->buffer = move(block);
 } else {
  if (handle->can_destroy) {
   return BufferHandle();
  } else {
   handle->buffer = block_manager.buffer_manager.ReadTemporaryBuffer(handle->block_id, move(reusable_buffer));
  }
 }
 handle->state = BlockState::BLOCK_LOADED;
 return BufferHandle(handle, handle->buffer.get());
}
unique_ptr<FileBuffer> BlockHandle::UnloadAndTakeBlock() {
 if (state == BlockState::BLOCK_UNLOADED) {
  return nullptr;
 }
 D_ASSERT(!unswizzled);
 D_ASSERT(CanUnload());
 if (block_id >= MAXIMUM_BLOCK && !can_destroy) {
  block_manager.buffer_manager.WriteTemporaryBuffer(block_id, *buffer);
 }
 memory_charge.Resize(block_manager.buffer_manager.current_memory, 0);
 state = BlockState::BLOCK_UNLOADED;
 return move(buffer);
}
void BlockHandle::Unload() {
 auto block = UnloadAndTakeBlock();
 block.reset();
}
bool BlockHandle::CanUnload() {
 if (state == BlockState::BLOCK_UNLOADED) {
  return false;
 }
 if (readers > 0) {
  return false;
 }
 if (block_id >= MAXIMUM_BLOCK && !can_destroy && block_manager.buffer_manager.temp_directory.empty()) {
  return false;
 }
 return true;
}
struct BufferEvictionNode {
 BufferEvictionNode() {
 }
 BufferEvictionNode(weak_ptr<BlockHandle> handle_p, idx_t timestamp_p)
     : handle(move(handle_p)), timestamp(timestamp_p) {
  D_ASSERT(!handle.expired());
 }
 weak_ptr<BlockHandle> handle;
 idx_t timestamp;
 bool CanUnload(BlockHandle &handle_p) {
  if (timestamp != handle_p.eviction_timestamp) {
   return false;
  }
  return handle_p.CanUnload();
 }
 shared_ptr<BlockHandle> TryGetBlockHandle() {
  auto handle_p = handle.lock();
  if (!handle_p) {
   return nullptr;
  }
  if (!CanUnload(*handle_p)) {
   return nullptr;
  }
  return handle_p;
 }
};
typedef duckdb_moodycamel::ConcurrentQueue<BufferEvictionNode> eviction_queue_t;
struct EvictionQueue {
 eviction_queue_t q;
};
class TemporaryFileManager;
class TemporaryDirectoryHandle {
public:
 TemporaryDirectoryHandle(DatabaseInstance &db, string path_p);
 ~TemporaryDirectoryHandle();
 TemporaryFileManager &GetTempFile();
private:
 DatabaseInstance &db;
 string temp_directory;
 unique_ptr<TemporaryFileManager> temp_file;
};
void BufferManager::SetTemporaryDirectory(string new_dir) {
 if (temp_directory_handle) {
  throw NotImplementedException("Cannot switch temporary directory after the current one has been used");
 }
 this->temp_directory = move(new_dir);
}
BufferManager::BufferManager(DatabaseInstance &db, string tmp, idx_t maximum_memory)
    : db(db), current_memory(0), maximum_memory(maximum_memory), temp_directory(move(tmp)),
      queue(make_unique<EvictionQueue>()), temporary_id(MAXIMUM_BLOCK), queue_insertions(0),
      buffer_allocator(BufferAllocatorAllocate, BufferAllocatorFree, BufferAllocatorRealloc,
                       make_unique<BufferAllocatorData>(*this)) {
 temp_block_manager = make_unique<InMemoryBlockManager>(*this);
}
BufferManager::~BufferManager() {
}
shared_ptr<BlockHandle> BlockManager::RegisterBlock(block_id_t block_id) {
 lock_guard<mutex> lock(blocks_lock);
 auto entry = blocks.find(block_id);
 if (entry != blocks.end()) {
  auto existing_ptr = entry->second.lock();
  if (existing_ptr) {
   return existing_ptr;
  }
 }
 auto result = make_shared<BlockHandle>(*this, block_id);
 blocks[block_id] = weak_ptr<BlockHandle>(result);
 return result;
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
 new_block->memory_charge = move(old_block->memory_charge);
 old_block->buffer.reset();
 old_block->state = BlockState::BLOCK_UNLOADED;
 old_block->memory_usage = 0;
 old_handle.Destroy();
 old_block.reset();
 Write(*new_block->buffer, block_id);
 buffer_manager.AddToEvictionQueue(new_block);
 return new_block;
}
template <typename... ARGS>
TempBufferPoolReservation BufferManager::EvictBlocksOrThrow(idx_t memory_delta, idx_t limit,
                                                            unique_ptr<FileBuffer> *buffer, ARGS... args) {
 auto r = EvictBlocks(memory_delta, limit, buffer);
 if (!r.success) {
  throw OutOfMemoryException(args..., InMemoryWarning());
 }
 return move(r.reservation);
}
shared_ptr<BlockHandle> BufferManager::RegisterSmallMemory(idx_t block_size) {
 D_ASSERT(block_size < Storage::BLOCK_SIZE);
 auto res = EvictBlocksOrThrow(block_size, maximum_memory, nullptr,
                               "could not allocate block of %lld bytes (%lld/%lld used) %s", block_size,
                               GetUsedMemory(), GetMaxMemory());
 auto buffer = ConstructManagedBuffer(block_size, nullptr, FileBufferType::TINY_BUFFER);
 return make_shared<BlockHandle>(*temp_block_manager, ++temporary_id, move(buffer), false, block_size, move(res));
}
shared_ptr<BlockHandle> BufferManager::RegisterMemory(idx_t block_size, bool can_destroy) {
 D_ASSERT(block_size >= Storage::BLOCK_SIZE);
 auto alloc_size = GetAllocSize(block_size);
 unique_ptr<FileBuffer> reusable_buffer;
 auto res = EvictBlocksOrThrow(alloc_size, maximum_memory, &reusable_buffer,
                               "could not allocate block of %lld bytes (%lld/%lld used) %s", alloc_size,
                               GetUsedMemory(), GetMaxMemory());
 auto buffer = ConstructManagedBuffer(block_size, move(reusable_buffer));
 return make_shared<BlockHandle>(*temp_block_manager, ++temporary_id, move(buffer), can_destroy, alloc_size,
                                 move(res));
}
BufferHandle BufferManager::Allocate(idx_t block_size, bool can_destroy, shared_ptr<BlockHandle> *block) {
 shared_ptr<BlockHandle> local_block;
 auto block_ptr = block ? block : &local_block;
 *block_ptr = RegisterMemory(block_size, can_destroy);
 return Pin(*block_ptr);
}
void BufferManager::ReAllocate(shared_ptr<BlockHandle> &handle, idx_t block_size) {
 D_ASSERT(block_size >= Storage::BLOCK_SIZE);
 lock_guard<mutex> lock(handle->lock);
 D_ASSERT(handle->state == BlockState::BLOCK_LOADED);
 D_ASSERT(handle->memory_usage == handle->buffer->AllocSize());
 D_ASSERT(handle->memory_usage == handle->memory_charge.size);
 auto req = handle->buffer->CalculateMemory(block_size);
 int64_t memory_delta = (int64_t)req.alloc_size - handle->memory_usage;
 if (memory_delta == 0) {
  return;
 } else if (memory_delta > 0) {
  auto reservation =
      EvictBlocksOrThrow(memory_delta, maximum_memory, nullptr, "failed to resize block from %lld to %lld%s",
                         handle->memory_usage, req.alloc_size);
  handle->memory_charge.Merge(move(reservation));
 } else {
  handle->memory_charge.Resize(current_memory, req.alloc_size);
 }
 handle->buffer->Resize(block_size);
 handle->memory_usage += memory_delta;
 D_ASSERT(handle->memory_usage == handle->buffer->AllocSize());
}
BufferHandle BufferManager::Pin(shared_ptr<BlockHandle> &handle) {
 idx_t required_memory;
 {
  lock_guard<mutex> lock(handle->lock);
  if (handle->state == BlockState::BLOCK_LOADED) {
   handle->readers++;
   return handle->Load(handle);
  }
  required_memory = handle->memory_usage;
 }
 unique_ptr<FileBuffer> reusable_buffer;
 auto reservation = EvictBlocksOrThrow(required_memory, maximum_memory, &reusable_buffer,
                                       "failed to pin block of size %lld%s", required_memory);
 lock_guard<mutex> lock(handle->lock);
 if (handle->state == BlockState::BLOCK_LOADED) {
  handle->readers++;
  reservation.Resize(current_memory, 0);
  return handle->Load(handle);
 }
 D_ASSERT(handle->readers == 0);
 handle->readers = 1;
 auto buf = handle->Load(handle, move(reusable_buffer));
 handle->memory_charge = move(reservation);
 int64_t delta = handle->buffer->AllocSize() - handle->memory_usage;
 if (delta) {
  D_ASSERT(delta < 0);
  handle->memory_usage += delta;
  handle->memory_charge.Resize(current_memory, handle->memory_usage);
 }
 D_ASSERT(handle->memory_usage == handle->buffer->AllocSize());
 return buf;
}
void BufferManager::AddToEvictionQueue(shared_ptr<BlockHandle> &handle) {
 constexpr int INSERT_INTERVAL = 1024;
 D_ASSERT(handle->readers == 0);
 handle->eviction_timestamp++;
 if ((++queue_insertions % INSERT_INTERVAL) == 0) {
  PurgeQueue();
 }
 queue->q.enqueue(BufferEvictionNode(weak_ptr<BlockHandle>(handle), handle->eviction_timestamp));
}
void BufferManager::VerifyZeroReaders(shared_ptr<BlockHandle> &handle) {
#ifdef DUCKDB_DEBUG_DESTROY_BLOCKS
 auto replacement_buffer = make_unique<FileBuffer>(Allocator::Get(db), handle->buffer->type,
                                                   handle->memory_usage - Storage::BLOCK_HEADER_SIZE);
 memcpy(replacement_buffer->buffer, handle->buffer->buffer, handle->buffer->size);
 memset(handle->buffer->buffer, 165, handle->buffer->size);
 handle->buffer = move(replacement_buffer);
#endif
}
void BufferManager::Unpin(shared_ptr<BlockHandle> &handle) {
 lock_guard<mutex> lock(handle->lock);
 if (!handle->buffer || handle->buffer->type == FileBufferType::TINY_BUFFER) {
  return;
 }
 D_ASSERT(handle->readers > 0);
 handle->readers--;
 if (handle->readers == 0) {
  VerifyZeroReaders(handle);
  AddToEvictionQueue(handle);
 }
}
BufferManager::EvictionResult BufferManager::EvictBlocks(idx_t extra_memory, idx_t memory_limit,
                                                         unique_ptr<FileBuffer> *buffer) {
 BufferEvictionNode node;
 TempBufferPoolReservation r(current_memory, extra_memory);
 while (current_memory > memory_limit) {
  if (!queue->q.try_dequeue(node)) {
   r.Resize(current_memory, 0);
   return {false, move(r)};
  }
  auto handle = node.TryGetBlockHandle();
  if (!handle) {
   continue;
  }
  lock_guard<mutex> lock(handle->lock);
  if (!node.CanUnload(*handle)) {
   continue;
  }
  if (buffer && handle->buffer->AllocSize() == extra_memory) {
   *buffer = handle->UnloadAndTakeBlock();
   return {true, move(r)};
  } else {
   handle->Unload();
  }
 }
 return {true, move(r)};
}
void BufferManager::PurgeQueue() {
 BufferEvictionNode node;
 while (true) {
  if (!queue->q.try_dequeue(node)) {
   break;
  }
  auto handle = node.TryGetBlockHandle();
  if (!handle) {
   continue;
  } else {
   queue->q.enqueue(move(node));
   break;
  }
 }
}
void BlockManager::UnregisterBlock(block_id_t block_id, bool can_destroy) {
 if (block_id >= MAXIMUM_BLOCK) {
  if (!can_destroy) {
   buffer_manager.DeleteTemporaryFile(block_id);
  }
 } else {
  lock_guard<mutex> lock(blocks_lock);
  blocks.erase(block_id);
 }
}
void BufferManager::SetLimit(idx_t limit) {
 lock_guard<mutex> l_lock(limit_lock);
 if (!EvictBlocks(0, limit).success) {
  throw OutOfMemoryException(
      "Failed to change memory limit to %lld: could not free up enough memory for the new limit%s", limit,
      InMemoryWarning());
 }
 idx_t old_limit = maximum_memory;
 maximum_memory = limit;
 if (!EvictBlocks(0, limit).success) {
  maximum_memory = old_limit;
  throw OutOfMemoryException(
      "Failed to change memory limit to %lld: could not free up enough memory for the new limit%s", limit,
      InMemoryWarning());
 }
}
unique_ptr<FileBuffer> ReadTemporaryBufferInternal(BufferManager &buffer_manager, FileHandle &handle, idx_t position,
                                                   idx_t size, block_id_t id, unique_ptr<FileBuffer> reusable_buffer) {
 auto buffer = buffer_manager.ConstructManagedBuffer(size, move(reusable_buffer));
 buffer->Read(handle, position);
 return buffer;
}
struct TemporaryFileIndex {
 explicit TemporaryFileIndex(idx_t file_index = DConstants::INVALID_INDEX,
                             idx_t block_index = DConstants::INVALID_INDEX)
     : file_index(file_index), block_index(block_index) {
 }
 idx_t file_index;
 idx_t block_index;
public:
 bool IsValid() {
  return block_index != DConstants::INVALID_INDEX;
 }
};
struct BlockIndexManager {
 BlockIndexManager() : max_index(0) {
 }
public:
 idx_t GetNewBlockIndex() {
  auto index = GetNewBlockIndexInternal();
  indexes_in_use.insert(index);
  return index;
 }
 bool RemoveIndex(idx_t index) {
  indexes_in_use.erase(index);
  free_indexes.insert(index);
  auto max_index_in_use = indexes_in_use.empty() ? 0 : *indexes_in_use.rbegin();
  if (max_index_in_use < max_index) {
   max_index = max_index_in_use + 1;
   while (!free_indexes.empty()) {
    auto max_entry = *free_indexes.rbegin();
    if (max_entry < max_index) {
     break;
    }
    free_indexes.erase(max_entry);
   }
   return true;
  }
  return false;
 }
 idx_t GetMaxIndex() {
  return max_index;
 }
 bool HasFreeBlocks() {
  return !free_indexes.empty();
 }
private:
 idx_t GetNewBlockIndexInternal() {
  if (free_indexes.empty()) {
   return max_index++;
  }
  auto entry = free_indexes.begin();
  auto index = *entry;
  free_indexes.erase(entry);
  return index;
 }
 idx_t max_index;
 set<idx_t> free_indexes;
 set<idx_t> indexes_in_use;
};
class TemporaryFileHandle {
 constexpr static idx_t MAX_ALLOWED_INDEX = 4000;
public:
 TemporaryFileHandle(DatabaseInstance &db, const string &temp_directory, idx_t index)
     : db(db), file_index(index), path(FileSystem::GetFileSystem(db).JoinPath(
                                      temp_directory, "duckdb_temp_storage-" + to_string(index) + ".tmp")) {
 }
public:
 struct TemporaryFileLock {
  explicit TemporaryFileLock(mutex &mutex) : lock(mutex) {
  }
  lock_guard<mutex> lock;
 };
public:
 TemporaryFileIndex TryGetBlockIndex() {
  TemporaryFileLock lock(file_lock);
  if (index_manager.GetMaxIndex() >= MAX_ALLOWED_INDEX && index_manager.HasFreeBlocks()) {
   return TemporaryFileIndex();
  }
  CreateFileIfNotExists(lock);
  auto block_index = index_manager.GetNewBlockIndex();
  return TemporaryFileIndex(file_index, block_index);
 }
 void WriteTemporaryFile(FileBuffer &buffer, TemporaryFileIndex index) {
  D_ASSERT(buffer.size == Storage::BLOCK_SIZE);
  buffer.Write(*handle, GetPositionInFile(index.block_index));
 }
 unique_ptr<FileBuffer> ReadTemporaryBuffer(block_id_t id, idx_t block_index,
                                            unique_ptr<FileBuffer> reusable_buffer) {
  auto buffer =
      ReadTemporaryBufferInternal(BufferManager::GetBufferManager(db), *handle, GetPositionInFile(block_index),
                                  Storage::BLOCK_SIZE, id, move(reusable_buffer));
  {
   TemporaryFileLock lock(file_lock);
   D_ASSERT(handle);
   RemoveTempBlockIndex(lock, block_index);
  }
  return buffer;
 }
 bool DeleteIfEmpty() {
  TemporaryFileLock lock(file_lock);
  if (index_manager.GetMaxIndex() > 0) {
   return false;
  }
  handle.reset();
  auto &fs = FileSystem::GetFileSystem(db);
  fs.RemoveFile(path);
  return true;
 }
private:
 void CreateFileIfNotExists(TemporaryFileLock &) {
  if (handle) {
   return;
  }
  auto &fs = FileSystem::GetFileSystem(db);
  handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_WRITE |
                                 FileFlags::FILE_FLAGS_FILE_CREATE);
 }
 void RemoveTempBlockIndex(TemporaryFileLock &, idx_t index) {
  if (index_manager.RemoveIndex(index)) {
   auto max_index = index_manager.GetMaxIndex();
   auto &fs = FileSystem::GetFileSystem(db);
   fs.Truncate(*handle, GetPositionInFile(max_index + 1));
  }
 }
 idx_t GetPositionInFile(idx_t index) {
  return index * Storage::BLOCK_ALLOC_SIZE;
 }
private:
 DatabaseInstance &db;
 unique_ptr<FileHandle> handle;
 idx_t file_index;
 string path;
 mutex file_lock;
 BlockIndexManager index_manager;
};
class TemporaryFileManager {
public:
 TemporaryFileManager(DatabaseInstance &db, const string &temp_directory_p)
     : db(db), temp_directory(temp_directory_p) {
 }
public:
 struct TemporaryManagerLock {
  explicit TemporaryManagerLock(mutex &mutex) : lock(mutex) {
  }
  lock_guard<mutex> lock;
 };
 void WriteTemporaryBuffer(block_id_t block_id, FileBuffer &buffer) {
  D_ASSERT(buffer.size == Storage::BLOCK_SIZE);
  TemporaryFileIndex index;
  TemporaryFileHandle *handle = nullptr;
  {
   TemporaryManagerLock lock(manager_lock);
   for (auto &entry : files) {
    auto &temp_file = entry.second;
    index = temp_file->TryGetBlockIndex();
    if (index.IsValid()) {
     handle = entry.second.get();
     break;
    }
   }
   if (!handle) {
    auto new_file_index = index_manager.GetNewBlockIndex();
    auto new_file = make_unique<TemporaryFileHandle>(db, temp_directory, new_file_index);
    handle = new_file.get();
    files[new_file_index] = move(new_file);
    index = handle->TryGetBlockIndex();
   }
   D_ASSERT(used_blocks.find(block_id) == used_blocks.end());
   used_blocks[block_id] = index;
  }
  D_ASSERT(handle);
  D_ASSERT(index.IsValid());
  handle->WriteTemporaryFile(buffer, index);
 }
 bool HasTemporaryBuffer(block_id_t block_id) {
  lock_guard<mutex> lock(manager_lock);
  return used_blocks.find(block_id) != used_blocks.end();
 }
 unique_ptr<FileBuffer> ReadTemporaryBuffer(block_id_t id, unique_ptr<FileBuffer> reusable_buffer) {
  TemporaryFileIndex index;
  TemporaryFileHandle *handle;
  {
   TemporaryManagerLock lock(manager_lock);
   index = GetTempBlockIndex(lock, id);
   handle = GetFileHandle(lock, index.file_index);
  }
  auto buffer = handle->ReadTemporaryBuffer(id, index.block_index, move(reusable_buffer));
  {
   TemporaryManagerLock lock(manager_lock);
   EraseUsedBlock(lock, id, handle, index.file_index);
  }
  return buffer;
 }
 void DeleteTemporaryBuffer(block_id_t id) {
  TemporaryManagerLock lock(manager_lock);
  auto index = GetTempBlockIndex(lock, id);
  auto handle = GetFileHandle(lock, index.file_index);
  EraseUsedBlock(lock, id, handle, index.file_index);
 }
private:
 void EraseUsedBlock(TemporaryManagerLock &lock, block_id_t id, TemporaryFileHandle *handle, idx_t file_index) {
  used_blocks.erase(id);
  if (handle->DeleteIfEmpty()) {
   EraseFileHandle(lock, file_index);
  }
 }
 TemporaryFileHandle *GetFileHandle(TemporaryManagerLock &, idx_t index) {
  return files[index].get();
 }
 TemporaryFileIndex GetTempBlockIndex(TemporaryManagerLock &, block_id_t id) {
  D_ASSERT(used_blocks.find(id) != used_blocks.end());
  return used_blocks[id];
 }
 void EraseFileHandle(TemporaryManagerLock &, idx_t file_index) {
  files.erase(file_index);
  index_manager.RemoveIndex(file_index);
 }
private:
 DatabaseInstance &db;
 mutex manager_lock;
 string temp_directory;
 unordered_map<idx_t, unique_ptr<TemporaryFileHandle>> files;
 unordered_map<block_id_t, TemporaryFileIndex> used_blocks;
 BlockIndexManager index_manager;
};
TemporaryDirectoryHandle::TemporaryDirectoryHandle(DatabaseInstance &db, string path_p)
    : db(db), temp_directory(move(path_p)), temp_file(make_unique<TemporaryFileManager>(db, temp_directory)) {
 auto &fs = FileSystem::GetFileSystem(db);
 if (!temp_directory.empty()) {
  fs.CreateDirectory(temp_directory);
 }
}
TemporaryDirectoryHandle::~TemporaryDirectoryHandle() {
 temp_file.reset();
 auto &fs = FileSystem::GetFileSystem(db);
 if (!temp_directory.empty()) {
  fs.RemoveDirectory(temp_directory);
 }
}
TemporaryFileManager &TemporaryDirectoryHandle::GetTempFile() {
 return *temp_file;
}
string BufferManager::GetTemporaryPath(block_id_t id) {
 auto &fs = FileSystem::GetFileSystem(db);
 return fs.JoinPath(temp_directory, to_string(id) + ".block");
}
void BufferManager::RequireTemporaryDirectory() {
 if (temp_directory.empty()) {
  throw Exception(
      "Out-of-memory: cannot write buffer because no temporary directory is specified!\nTo enable "
      "temporary buffer eviction set a temporary directory using PRAGMA temp_directory='/path/to/tmp.tmp'");
 }
 lock_guard<mutex> temp_handle_guard(temp_handle_lock);
 if (!temp_directory_handle) {
  temp_directory_handle = make_unique<TemporaryDirectoryHandle>(db, temp_directory);
 }
}
void BufferManager::WriteTemporaryBuffer(block_id_t block_id, FileBuffer &buffer) {
 RequireTemporaryDirectory();
 if (buffer.size == Storage::BLOCK_SIZE) {
  temp_directory_handle->GetTempFile().WriteTemporaryBuffer(block_id, buffer);
  return;
 }
 auto path = GetTemporaryPath(block_id);
 D_ASSERT(buffer.size > Storage::BLOCK_SIZE);
 auto &fs = FileSystem::GetFileSystem(db);
 auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
 handle->Write(&buffer.size, sizeof(idx_t), 0);
 buffer.Write(*handle, sizeof(idx_t));
}
unique_ptr<FileBuffer> BufferManager::ReadTemporaryBuffer(block_id_t id, unique_ptr<FileBuffer> reusable_buffer) {
 D_ASSERT(!temp_directory.empty());
 D_ASSERT(temp_directory_handle.get());
 if (temp_directory_handle->GetTempFile().HasTemporaryBuffer(id)) {
  return temp_directory_handle->GetTempFile().ReadTemporaryBuffer(id, move(reusable_buffer));
 }
 idx_t block_size;
 auto path = GetTemporaryPath(id);
 auto &fs = FileSystem::GetFileSystem(db);
 auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
 handle->Read(&block_size, sizeof(idx_t), 0);
 auto buffer = ReadTemporaryBufferInternal(*this, *handle, sizeof(idx_t), block_size, id, move(reusable_buffer));
 handle.reset();
 DeleteTemporaryFile(id);
 return buffer;
}
void BufferManager::DeleteTemporaryFile(block_id_t id) {
 if (temp_directory.empty()) {
  return;
 }
 {
  lock_guard<mutex> temp_handle_guard(temp_handle_lock);
  if (!temp_directory_handle) {
   return;
  }
 }
 if (temp_directory_handle->GetTempFile().HasTemporaryBuffer(id)) {
  temp_directory_handle->GetTempFile().DeleteTemporaryBuffer(id);
  return;
 }
 auto &fs = FileSystem::GetFileSystem(db);
 auto path = GetTemporaryPath(id);
 if (fs.FileExists(path)) {
  fs.RemoveFile(path);
 }
}
string BufferManager::InMemoryWarning() {
 if (!temp_directory.empty()) {
  return "";
 }
 return "\nDatabase is launched in in-memory mode and no temporary directory is specified."
        "\nUnused blocks cannot be offloaded to disk."
        "\n\nLaunch the database with a persistent storage back-end"
        "\nOr set PRAGMA temp_directory='/path/to/tmp.tmp'";
}
void BufferManager::ReserveMemory(idx_t size) {
 if (size == 0) {
  return;
 }
 auto reservation =
     EvictBlocksOrThrow(size, maximum_memory, nullptr, "failed to reserve memory data of size %lld%s", size);
 reservation.size = 0;
}
void BufferManager::FreeReservedMemory(idx_t size) {
 if (size == 0) {
  return;
 }
 current_memory -= size;
}
data_ptr_t BufferManager::BufferAllocatorAllocate(PrivateAllocatorData *private_data, idx_t size) {
 auto &data = (BufferAllocatorData &)*private_data;
 auto reservation = data.manager.EvictBlocksOrThrow(size, data.manager.maximum_memory, nullptr,
                                                    "failed to allocate data of size %lld%s", size);
 reservation.size = 0;
 return Allocator::Get(data.manager.db).AllocateData(size);
}
void BufferManager::BufferAllocatorFree(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t size) {
 auto &data = (BufferAllocatorData &)*private_data;
 BufferPoolReservation r;
 r.size = size;
 r.Resize(data.manager.current_memory, 0);
 return Allocator::Get(data.manager.db).FreeData(pointer, size);
}
data_ptr_t BufferManager::BufferAllocatorRealloc(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t old_size,
                                                 idx_t size) {
 if (old_size == size) {
  return pointer;
 }
 auto &data = (BufferAllocatorData &)*private_data;
 BufferPoolReservation r;
 r.size = old_size;
 r.Resize(data.manager.current_memory, size);
 r.size = 0;
 return Allocator::Get(data.manager.db).ReallocateData(pointer, old_size, size);
}
Allocator &BufferAllocator::Get(ClientContext &context) {
 auto &manager = BufferManager::GetBufferManager(context);
 return manager.GetBufferAllocator();
}
Allocator &BufferAllocator::Get(DatabaseInstance &db) {
 return BufferManager::GetBufferManager(db).GetBufferAllocator();
}
Allocator &BufferManager::GetBufferAllocator() {
 return buffer_allocator;
}
}
