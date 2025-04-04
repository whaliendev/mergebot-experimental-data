#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_buffer.hpp"
#include "duckdb/storage/standard_buffer_manager.hpp"

namespace duckdb {

<<<<<<< ours
unique_ptr<BufferManager> BufferManager::CreateStandardBufferManager(DatabaseInstance &db, DBConfig &config) {
	return make_unique<StandardBufferManager>(db, config.options.temporary_directory);
||||||| base
struct BufferAllocatorData : PrivateAllocatorData {
	explicit BufferAllocatorData(BufferManager &manager) : manager(manager) {
	}

	BufferManager &manager;
};

idx_t GetAllocSize(idx_t size) {
	return AlignValue<idx_t, Storage::SECTOR_SIZE>(size + Storage::BLOCK_HEADER_SIZE);
}

unique_ptr<FileBuffer> BufferManager::ConstructManagedBuffer(idx_t size, unique_ptr<FileBuffer> &&source,
                                                             FileBufferType type) {
	if (source) {
		auto tmp = std::move(source);
		D_ASSERT(tmp->AllocSize() == BufferManager::GetAllocSize(size));
		return make_unique<FileBuffer>(*tmp, type);
	} else {
		// no re-usable buffer: allocate a new buffer
		return make_unique<FileBuffer>(Allocator::Get(db), type, size);
	}
}

class TemporaryFileManager;

class TemporaryDirectoryHandle {
public:
	TemporaryDirectoryHandle(DatabaseInstance &db, string path_p);
	~TemporaryDirectoryHandle();

	TemporaryFileManager &GetTempFile();

private:
	DatabaseInstance &db;
	string temp_directory;
	bool created_directory = false;
	unique_ptr<TemporaryFileManager> temp_file;
};

void BufferManager::SetTemporaryDirectory(string new_dir) {
	if (temp_directory_handle) {
		throw NotImplementedException("Cannot switch temporary directory after the current one has been used");
	}
	this->temp_directory = std::move(new_dir);
}

BufferManager::BufferManager(DatabaseInstance &db, string tmp)
    : db(db), buffer_pool(db.GetBufferPool()), temp_directory(std::move(tmp)), temporary_id(MAXIMUM_BLOCK),
      buffer_allocator(BufferAllocatorAllocate, BufferAllocatorFree, BufferAllocatorRealloc,
                       make_unique<BufferAllocatorData>(*this)) {
	temp_block_manager = make_unique<InMemoryBlockManager>(*this);
}

BufferManager::~BufferManager() {
}

template <typename... ARGS>
TempBufferPoolReservation BufferManager::EvictBlocksOrThrow(idx_t memory_delta, unique_ptr<FileBuffer> *buffer,
                                                            ARGS... args) {
	auto r = buffer_pool.EvictBlocks(memory_delta, buffer_pool.maximum_memory, buffer);
	if (!r.success) {
		throw OutOfMemoryException(args..., InMemoryWarning());
	}
	return std::move(r.reservation);
=======
struct BufferAllocatorData : PrivateAllocatorData {
	explicit BufferAllocatorData(BufferManager &manager) : manager(manager) {
	}

	BufferManager &manager;
};

idx_t GetAllocSize(idx_t size) {
	return AlignValue<idx_t, Storage::SECTOR_SIZE>(size + Storage::BLOCK_HEADER_SIZE);
}

unique_ptr<FileBuffer> BufferManager::ConstructManagedBuffer(idx_t size, unique_ptr<FileBuffer> &&source,
                                                             FileBufferType type) {
	if (source) {
		auto tmp = std::move(source);
		D_ASSERT(tmp->AllocSize() == BufferManager::GetAllocSize(size));
		return make_uniq<FileBuffer>(*tmp, type);
	} else {
		// no re-usable buffer: allocate a new buffer
		return make_uniq<FileBuffer>(Allocator::Get(db), type, size);
	}
}

class TemporaryFileManager;

class TemporaryDirectoryHandle {
public:
	TemporaryDirectoryHandle(DatabaseInstance &db, string path_p);
	~TemporaryDirectoryHandle();

	TemporaryFileManager &GetTempFile();

private:
	DatabaseInstance &db;
	string temp_directory;
	bool created_directory = false;
	unique_ptr<TemporaryFileManager> temp_file;
};

void BufferManager::SetTemporaryDirectory(string new_dir) {
	if (temp_directory_handle) {
		throw NotImplementedException("Cannot switch temporary directory after the current one has been used");
	}
	this->temp_directory = std::move(new_dir);
}

BufferManager::BufferManager(DatabaseInstance &db, string tmp)
    : db(db), buffer_pool(db.GetBufferPool()), temp_directory(std::move(tmp)), temporary_id(MAXIMUM_BLOCK),
      buffer_allocator(BufferAllocatorAllocate, BufferAllocatorFree, BufferAllocatorRealloc,
                       make_uniq<BufferAllocatorData>(*this)) {
	temp_block_manager = make_uniq<InMemoryBlockManager>(*this);
}

BufferManager::~BufferManager() {
}

template <typename... ARGS>
TempBufferPoolReservation BufferManager::EvictBlocksOrThrow(idx_t memory_delta, unique_ptr<FileBuffer> *buffer,
                                                            ARGS... args) {
	auto r = buffer_pool.EvictBlocks(memory_delta, buffer_pool.maximum_memory, buffer);
	if (!r.success) {
		throw OutOfMemoryException(args..., InMemoryWarning());
	}
	return std::move(r.reservation);
>>>>>>> theirs
}

shared_ptr<BlockHandle> BufferManager::RegisterSmallMemory(idx_t block_size) {
	throw NotImplementedException("This type of BufferManager can not create 'small-memory' blocks");
}

Allocator &BufferManager::GetBufferAllocator() {
	throw NotImplementedException("This type of BufferManager does not have an Allocator");
}

void BufferManager::ReserveMemory(idx_t size) {
	throw NotImplementedException("This type of BufferManager can not reserve memory");
}
void BufferManager::FreeReservedMemory(idx_t size) {
	throw NotImplementedException("This type of BufferManager can not free reserved memory");
}

void BufferManager::SetLimit(idx_t limit) {
	throw NotImplementedException("This type of BufferManager can not set a limit");
}

<<<<<<< ours
vector<TemporaryFileInformation> BufferManager::GetTemporaryFiles() {
	throw InternalException("This type of BufferManager does not allow temporary files");
||||||| base
void BufferManager::VerifyZeroReaders(shared_ptr<BlockHandle> &handle) {
#ifdef DUCKDB_DEBUG_DESTROY_BLOCKS
	auto replacement_buffer = make_unique<FileBuffer>(Allocator::Get(db), handle->buffer->type,
	                                                  handle->memory_usage - Storage::BLOCK_HEADER_SIZE);
	memcpy(replacement_buffer->buffer, handle->buffer->buffer, handle->buffer->size);
	memset(handle->buffer->buffer, 165, handle->buffer->size); // 165 is default memory in debug mode
	handle->buffer = std::move(replacement_buffer);
#endif
=======
void BufferManager::VerifyZeroReaders(shared_ptr<BlockHandle> &handle) {
#ifdef DUCKDB_DEBUG_DESTROY_BLOCKS
	auto replacement_buffer = make_uniq<FileBuffer>(Allocator::Get(db), handle->buffer->type,
	                                                handle->memory_usage - Storage::BLOCK_HEADER_SIZE);
	memcpy(replacement_buffer->buffer, handle->buffer->buffer, handle->buffer->size);
	memset(handle->buffer->buffer, 165, handle->buffer->size); // 165 is default memory in debug mode
	handle->buffer = std::move(replacement_buffer);
#endif
>>>>>>> theirs
}

const string &BufferManager::GetTemporaryDirectory() {
	throw InternalException("This type of BufferManager does not allow a temporary directory");
}

BufferPool &BufferManager::GetBufferPool() {
	throw InternalException("This type of BufferManager does not have a buffer pool");
}

void BufferManager::SetTemporaryDirectory(const string &new_dir) {
	throw NotImplementedException("This type of BufferManager can not set a temporary directory");
}

DatabaseInstance &BufferManager::GetDatabase() {
	throw NotImplementedException("This type of BufferManager is not linked to a DatabaseInstance");
}

<<<<<<< ours
bool BufferManager::HasTemporaryDirectory() const {
	return false;
||||||| base
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
	//! Obtains a new block index from the index manager
	idx_t GetNewBlockIndex() {
		auto index = GetNewBlockIndexInternal();
		indexes_in_use.insert(index);
		return index;
	}

	//! Removes an index from the block manager
	//! Returns true if the max_index has been altered
	bool RemoveIndex(idx_t index) {
		// remove this block from the set of blocks
		auto entry = indexes_in_use.find(index);
		if (entry == indexes_in_use.end()) {
			throw InternalException("RemoveIndex - index %llu not found in indexes_in_use", index);
		}
		indexes_in_use.erase(entry);
		free_indexes.insert(index);
		// check if we can truncate the file

		// get the max_index in use right now
		auto max_index_in_use = indexes_in_use.empty() ? 0 : *indexes_in_use.rbegin();
		if (max_index_in_use < max_index) {
			// max index in use is lower than the max_index
			// reduce the max_index
			max_index = indexes_in_use.empty() ? 0 : max_index_in_use + 1;
			// we can remove any free_indexes that are larger than the current max_index
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
			// file is at capacity
			return TemporaryFileIndex();
		}
		// open the file handle if it does not yet exist
		CreateFileIfNotExists(lock);
		// fetch a new block index to write to
		auto block_index = index_manager.GetNewBlockIndex();
		return TemporaryFileIndex(file_index, block_index);
	}

	void WriteTemporaryFile(FileBuffer &buffer, TemporaryFileIndex index) {
		D_ASSERT(buffer.size == Storage::BLOCK_SIZE);
		buffer.Write(*handle, GetPositionInFile(index.block_index));
	}

	unique_ptr<FileBuffer> ReadTemporaryBuffer(block_id_t id, idx_t block_index,
	                                           unique_ptr<FileBuffer> reusable_buffer) {
		return ReadTemporaryBufferInternal(BufferManager::GetBufferManager(db), *handle, GetPositionInFile(block_index),
		                                   Storage::BLOCK_SIZE, id, std::move(reusable_buffer));
	}

	void EraseBlockIndex(block_id_t block_index) {
		// remove the block (and potentially truncate the temp file)
		TemporaryFileLock lock(file_lock);
		D_ASSERT(handle);
		RemoveTempBlockIndex(lock, block_index);
	}

	bool DeleteIfEmpty() {
		TemporaryFileLock lock(file_lock);
		if (index_manager.GetMaxIndex() > 0) {
			// there are still blocks in this file
			return false;
		}
		// the file is empty: delete it
		handle.reset();
		auto &fs = FileSystem::GetFileSystem(db);
		fs.RemoveFile(path);
		return true;
	}

	TemporaryFileInformation GetTemporaryFile() {
		TemporaryFileLock lock(file_lock);
		TemporaryFileInformation info;
		info.path = path;
		info.size = GetPositionInFile(index_manager.GetMaxIndex());
		return info;
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
		// remove the block index from the index manager
		if (index_manager.RemoveIndex(index)) {
			// the max_index that is currently in use has decreased
			// as a result we can truncate the file
#ifndef WIN32 // this ended up causing issues when sorting
			auto max_index = index_manager.GetMaxIndex();
			auto &fs = FileSystem::GetFileSystem(db);
			fs.Truncate(*handle, GetPositionInFile(max_index + 1));
#endif
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
			// first check if we can write to an open existing file
			for (auto &entry : files) {
				auto &temp_file = entry.second;
				index = temp_file->TryGetBlockIndex();
				if (index.IsValid()) {
					handle = entry.second.get();
					break;
				}
			}
			if (!handle) {
				// no existing handle to write to; we need to create & open a new file
				auto new_file_index = index_manager.GetNewBlockIndex();
				auto new_file = make_unique<TemporaryFileHandle>(db, temp_directory, new_file_index);
				handle = new_file.get();
				files[new_file_index] = std::move(new_file);

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
		auto buffer = handle->ReadTemporaryBuffer(id, index.block_index, std::move(reusable_buffer));
		{
			// remove the block (and potentially erase the temp file)
			TemporaryManagerLock lock(manager_lock);
			EraseUsedBlock(lock, id, handle, index);
		}
		return buffer;
	}

	void DeleteTemporaryBuffer(block_id_t id) {
		TemporaryManagerLock lock(manager_lock);
		auto index = GetTempBlockIndex(lock, id);
		auto handle = GetFileHandle(lock, index.file_index);
		EraseUsedBlock(lock, id, handle, index);
	}

	vector<TemporaryFileInformation> GetTemporaryFiles() {
		lock_guard<mutex> lock(manager_lock);
		vector<TemporaryFileInformation> result;
		for (auto &file : files) {
			result.push_back(file.second->GetTemporaryFile());
		}
		return result;
	}

private:
	void EraseUsedBlock(TemporaryManagerLock &lock, block_id_t id, TemporaryFileHandle *handle,
	                    TemporaryFileIndex index) {
		auto entry = used_blocks.find(id);
		if (entry == used_blocks.end()) {
			throw InternalException("EraseUsedBlock - Block %llu not found in used blocks", id);
		}
		used_blocks.erase(entry);
		handle->EraseBlockIndex(index.block_index);
		if (handle->DeleteIfEmpty()) {
			EraseFileHandle(lock, index.file_index);
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
	//! The temporary directory
	string temp_directory;
	//! The set of active temporary file handles
	unordered_map<idx_t, unique_ptr<TemporaryFileHandle>> files;
	//! map of block_id -> temporary file position
	unordered_map<block_id_t, TemporaryFileIndex> used_blocks;
	//! Manager of in-use temporary file indexes
	BlockIndexManager index_manager;
};

TemporaryDirectoryHandle::TemporaryDirectoryHandle(DatabaseInstance &db, string path_p)
    : db(db), temp_directory(std::move(path_p)), temp_file(make_unique<TemporaryFileManager>(db, temp_directory)) {
	auto &fs = FileSystem::GetFileSystem(db);
	if (!temp_directory.empty()) {
		if (!fs.DirectoryExists(temp_directory)) {
			fs.CreateDirectory(temp_directory);
			created_directory = true;
		}
	}
}
TemporaryDirectoryHandle::~TemporaryDirectoryHandle() {
	// first release any temporary files
	temp_file.reset();
	// then delete the temporary file directory
	auto &fs = FileSystem::GetFileSystem(db);
	if (!temp_directory.empty()) {
		bool delete_directory = created_directory;
		vector<string> files_to_delete;
		if (!created_directory) {
			bool deleted_everything = true;
			fs.ListFiles(temp_directory, [&](const string &path, bool isdir) {
				if (isdir) {
					deleted_everything = false;
					return;
				}
				if (!StringUtil::StartsWith(path, "duckdb_temp_")) {
					deleted_everything = false;
					return;
				}
				files_to_delete.push_back(path);
			});
		}
		if (delete_directory) {
			// we want to remove all files in the directory
			fs.RemoveDirectory(temp_directory);
		} else {
			for (auto &file : files_to_delete) {
				fs.RemoveFile(fs.JoinPath(temp_directory, file));
			}
		}
	}
=======
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
	//! Obtains a new block index from the index manager
	idx_t GetNewBlockIndex() {
		auto index = GetNewBlockIndexInternal();
		indexes_in_use.insert(index);
		return index;
	}

	//! Removes an index from the block manager
	//! Returns true if the max_index has been altered
	bool RemoveIndex(idx_t index) {
		// remove this block from the set of blocks
		auto entry = indexes_in_use.find(index);
		if (entry == indexes_in_use.end()) {
			throw InternalException("RemoveIndex - index %llu not found in indexes_in_use", index);
		}
		indexes_in_use.erase(entry);
		free_indexes.insert(index);
		// check if we can truncate the file

		// get the max_index in use right now
		auto max_index_in_use = indexes_in_use.empty() ? 0 : *indexes_in_use.rbegin();
		if (max_index_in_use < max_index) {
			// max index in use is lower than the max_index
			// reduce the max_index
			max_index = indexes_in_use.empty() ? 0 : max_index_in_use + 1;
			// we can remove any free_indexes that are larger than the current max_index
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
			// file is at capacity
			return TemporaryFileIndex();
		}
		// open the file handle if it does not yet exist
		CreateFileIfNotExists(lock);
		// fetch a new block index to write to
		auto block_index = index_manager.GetNewBlockIndex();
		return TemporaryFileIndex(file_index, block_index);
	}

	void WriteTemporaryFile(FileBuffer &buffer, TemporaryFileIndex index) {
		D_ASSERT(buffer.size == Storage::BLOCK_SIZE);
		buffer.Write(*handle, GetPositionInFile(index.block_index));
	}

	unique_ptr<FileBuffer> ReadTemporaryBuffer(block_id_t id, idx_t block_index,
	                                           unique_ptr<FileBuffer> reusable_buffer) {
		return ReadTemporaryBufferInternal(BufferManager::GetBufferManager(db), *handle, GetPositionInFile(block_index),
		                                   Storage::BLOCK_SIZE, id, std::move(reusable_buffer));
	}

	void EraseBlockIndex(block_id_t block_index) {
		// remove the block (and potentially truncate the temp file)
		TemporaryFileLock lock(file_lock);
		D_ASSERT(handle);
		RemoveTempBlockIndex(lock, block_index);
	}

	bool DeleteIfEmpty() {
		TemporaryFileLock lock(file_lock);
		if (index_manager.GetMaxIndex() > 0) {
			// there are still blocks in this file
			return false;
		}
		// the file is empty: delete it
		handle.reset();
		auto &fs = FileSystem::GetFileSystem(db);
		fs.RemoveFile(path);
		return true;
	}

	TemporaryFileInformation GetTemporaryFile() {
		TemporaryFileLock lock(file_lock);
		TemporaryFileInformation info;
		info.path = path;
		info.size = GetPositionInFile(index_manager.GetMaxIndex());
		return info;
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
		// remove the block index from the index manager
		if (index_manager.RemoveIndex(index)) {
			// the max_index that is currently in use has decreased
			// as a result we can truncate the file
#ifndef WIN32 // this ended up causing issues when sorting
			auto max_index = index_manager.GetMaxIndex();
			auto &fs = FileSystem::GetFileSystem(db);
			fs.Truncate(*handle, GetPositionInFile(max_index + 1));
#endif
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
			// first check if we can write to an open existing file
			for (auto &entry : files) {
				auto &temp_file = entry.second;
				index = temp_file->TryGetBlockIndex();
				if (index.IsValid()) {
					handle = entry.second.get();
					break;
				}
			}
			if (!handle) {
				// no existing handle to write to; we need to create & open a new file
				auto new_file_index = index_manager.GetNewBlockIndex();
				auto new_file = make_uniq<TemporaryFileHandle>(db, temp_directory, new_file_index);
				handle = new_file.get();
				files[new_file_index] = std::move(new_file);

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
		auto buffer = handle->ReadTemporaryBuffer(id, index.block_index, std::move(reusable_buffer));
		{
			// remove the block (and potentially erase the temp file)
			TemporaryManagerLock lock(manager_lock);
			EraseUsedBlock(lock, id, handle, index);
		}
		return buffer;
	}

	void DeleteTemporaryBuffer(block_id_t id) {
		TemporaryManagerLock lock(manager_lock);
		auto index = GetTempBlockIndex(lock, id);
		auto handle = GetFileHandle(lock, index.file_index);
		EraseUsedBlock(lock, id, handle, index);
	}

	vector<TemporaryFileInformation> GetTemporaryFiles() {
		lock_guard<mutex> lock(manager_lock);
		vector<TemporaryFileInformation> result;
		for (auto &file : files) {
			result.push_back(file.second->GetTemporaryFile());
		}
		return result;
	}

private:
	void EraseUsedBlock(TemporaryManagerLock &lock, block_id_t id, TemporaryFileHandle *handle,
	                    TemporaryFileIndex index) {
		auto entry = used_blocks.find(id);
		if (entry == used_blocks.end()) {
			throw InternalException("EraseUsedBlock - Block %llu not found in used blocks", id);
		}
		used_blocks.erase(entry);
		handle->EraseBlockIndex(index.block_index);
		if (handle->DeleteIfEmpty()) {
			EraseFileHandle(lock, index.file_index);
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
	//! The temporary directory
	string temp_directory;
	//! The set of active temporary file handles
	unordered_map<idx_t, unique_ptr<TemporaryFileHandle>> files;
	//! map of block_id -> temporary file position
	unordered_map<block_id_t, TemporaryFileIndex> used_blocks;
	//! Manager of in-use temporary file indexes
	BlockIndexManager index_manager;
};

TemporaryDirectoryHandle::TemporaryDirectoryHandle(DatabaseInstance &db, string path_p)
    : db(db), temp_directory(std::move(path_p)), temp_file(make_uniq<TemporaryFileManager>(db, temp_directory)) {
	auto &fs = FileSystem::GetFileSystem(db);
	if (!temp_directory.empty()) {
		if (!fs.DirectoryExists(temp_directory)) {
			fs.CreateDirectory(temp_directory);
			created_directory = true;
		}
	}
}
TemporaryDirectoryHandle::~TemporaryDirectoryHandle() {
	// first release any temporary files
	temp_file.reset();
	// then delete the temporary file directory
	auto &fs = FileSystem::GetFileSystem(db);
	if (!temp_directory.empty()) {
		bool delete_directory = created_directory;
		vector<string> files_to_delete;
		if (!created_directory) {
			bool deleted_everything = true;
			fs.ListFiles(temp_directory, [&](const string &path, bool isdir) {
				if (isdir) {
					deleted_everything = false;
					return;
				}
				if (!StringUtil::StartsWith(path, "duckdb_temp_")) {
					deleted_everything = false;
					return;
				}
				files_to_delete.push_back(path);
			});
		}
		if (delete_directory) {
			// we want to remove all files in the directory
			fs.RemoveDirectory(temp_directory);
		} else {
			for (auto &file : files_to_delete) {
				fs.RemoveFile(fs.JoinPath(temp_directory, file));
			}
		}
	}
>>>>>>> theirs
}

unique_ptr<FileBuffer> BufferManager::ConstructManagedBuffer(idx_t size, unique_ptr<FileBuffer> &&source,
                                                             FileBufferType type) {
	throw NotImplementedException("This type of BufferManager can not construct managed buffers");
}

// Protected methods

<<<<<<< ours
void BufferManager::AddToEvictionQueue(shared_ptr<BlockHandle> &handle) {
	throw NotImplementedException("This type of BufferManager does not support 'AddToEvictionQueue");
||||||| base
void BufferManager::RequireTemporaryDirectory() {
	if (temp_directory.empty()) {
		throw Exception(
		    "Out-of-memory: cannot write buffer because no temporary directory is specified!\nTo enable "
		    "temporary buffer eviction set a temporary directory using PRAGMA temp_directory='/path/to/tmp.tmp'");
	}
	lock_guard<mutex> temp_handle_guard(temp_handle_lock);
	if (!temp_directory_handle) {
		// temp directory has not been created yet: initialize it
		temp_directory_handle = make_unique<TemporaryDirectoryHandle>(db, temp_directory);
	}
=======
void BufferManager::RequireTemporaryDirectory() {
	if (temp_directory.empty()) {
		throw Exception(
		    "Out-of-memory: cannot write buffer because no temporary directory is specified!\nTo enable "
		    "temporary buffer eviction set a temporary directory using PRAGMA temp_directory='/path/to/tmp.tmp'");
	}
	lock_guard<mutex> temp_handle_guard(temp_handle_lock);
	if (!temp_directory_handle) {
		// temp directory has not been created yet: initialize it
		temp_directory_handle = make_uniq<TemporaryDirectoryHandle>(db, temp_directory);
	}
>>>>>>> theirs
}

void BufferManager::WriteTemporaryBuffer(block_id_t block_id, FileBuffer &buffer) {
	throw NotImplementedException("This type of BufferManager does not support 'WriteTemporaryBuffer");
}

unique_ptr<FileBuffer> BufferManager::ReadTemporaryBuffer(block_id_t id, unique_ptr<FileBuffer> buffer) {
	throw NotImplementedException("This type of BufferManager does not support 'ReadTemporaryBuffer");
}

void BufferManager::DeleteTemporaryFile(block_id_t id) {
	throw NotImplementedException("This type of BufferManager does not support 'DeleteTemporaryFile");
}

} // namespace duckdb
