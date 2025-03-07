       
#include <string>
#include <stdint.h>
#include "db/dbformat.h"
#include "port/port.h"
#include "rocksdb/cache.h"
#include "rocksdb/env.h"
#include "rocksdb/table.h"
#include "table/table_reader.h"
namespace rocksdb {
class Env;
struct FileMetaData;
class TableCache {
 public:
  TableCache(const std::string& dbname, const Options* options,
             const EnvOptions& storage_options, Cache* cache);
  ~TableCache();
  Iterator* NewIterator(const ReadOptions& options, const EnvOptions& toptions,
                        const InternalKeyComparator& internal_comparator,
                        const FileMetaData& file_meta,
                        TableReader** table_reader_ptr = nullptr,
                        bool for_compaction = false);
  Status Get(const ReadOptions& options,
             const InternalKeyComparator& internal_comparator,
             const FileMetaData& file_meta, const Slice& k, void* arg,
             bool (*handle_result)(void*, const ParsedInternalKey&,
                                   const Slice&, bool),
             bool* table_io, void (*mark_key_may_exist)(void*) = nullptr);
  bool PrefixMayMatch(const ReadOptions& options,
                      const InternalKeyComparator& internal_comparator,
                      uint64_t file_number, uint64_t file_size,
                      const Slice& internal_prefix, bool* table_io);
  static void Evict(Cache* cache, uint64_t file_number);
  Status FindTable(const EnvOptions& toptions,
                   const InternalKeyComparator& internal_comparator,
                   uint64_t file_number, uint64_t file_size, Cache::Handle**,
                   bool* table_io = nullptr, const bool no_io = false);
  TableReader* GetTableReaderFromHandle(Cache::Handle* handle);
  void ReleaseHandle(Cache::Handle* handle);
 private:
  Env* const env_;
  const std::string dbname_;
  const Options* options_;
  const EnvOptions& storage_options_;
<<<<<<< HEAD
  Cache* const cache_;
  Status FindTable(const EnvOptions& toptions, uint64_t file_number,
                   uint64_t file_size, Cache::Handle**, bool* table_io=nullptr,
                   const bool no_io = false);
||||||| 183ba01a0
  std::shared_ptr<Cache> cache_;
  Status FindTable(const EnvOptions& toptions, uint64_t file_number,
                   uint64_t file_size, Cache::Handle**, bool* table_io=nullptr,
                   const bool no_io = false);
=======
  std::shared_ptr<Cache> cache_;
>>>>>>> 4564b2e8
};
}
