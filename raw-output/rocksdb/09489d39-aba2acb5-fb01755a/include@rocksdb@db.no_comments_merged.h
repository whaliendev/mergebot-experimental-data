#ifndef STORAGE_ROCKSDB_INCLUDE_DB_H_
#define STORAGE_ROCKSDB_INCLUDE_DB_H_ 
#include <stdint.h>
#include <stdio.h>
#include <memory>
#include <vector>
#include <string>
#include "rocksdb/iterator.h"
#include "rocksdb/options.h"
#include "rocksdb/types.h"
#include "rocksdb/transaction_log.h"
namespace rocksdb {
using std::unique_ptr;
struct ColumnFamilyHandle;
extern const ColumnFamilyHandle default_column_family;
struct ColumnFamilyDescriptor {
  std::string name;
  ColumnFamilyOptions options;
  ColumnFamilyDescriptor()
      : name(default_column_family_name), options(ColumnFamilyOptions()) {}
  ColumnFamilyDescriptor(const std::string& name,
                         const ColumnFamilyOptions& options)
      : name(name), options(options) {}
};
static const int kMajorVersion = 2;
static const int kMinorVersion = 0;
struct Options;
struct ReadOptions;
struct WriteOptions;
struct FlushOptions;
class WriteBatch;
struct LiveFileMetaData {
  std::string name;
  int level;
  size_t size;
  std::string smallestkey;
  std::string largestkey;
  SequenceNumber smallest_seqno;
  SequenceNumber largest_seqno;
};
class Snapshot {
 protected:
  virtual ~Snapshot();
};
struct Range {
  Slice start;
  Slice limit;
  Range() { }
  Range(const Slice& s, const Slice& l) : start(s), limit(l) { }
};
class DB {
 public:
  static Status Open(const Options& options,
                     const std::string& name,
                     DB** dbptr);
  static Status OpenForReadOnly(const Options& options,
      const std::string& name, DB** dbptr,
      bool error_if_log_file_exist = false);
  static Status OpenWithColumnFamilies(
      const DBOptions& db_options, const std::string& name,
      const std::vector<ColumnFamilyDescriptor>& column_families,
      std::vector<ColumnFamilyHandle>* handles, DB** dbptr);
  static Status ListColumnFamilies(const DBOptions& db_options,
                                   const std::string& name,
                                   std::vector<std::string>* column_families);
  DB() { }
  virtual ~DB();
  virtual Status CreateColumnFamily(const ColumnFamilyOptions& options,
                                    const std::string& column_family_name,
                                    ColumnFamilyHandle* handle);
  virtual Status DropColumnFamily(const ColumnFamilyHandle& column_family);
  virtual Status Put(const WriteOptions& options,
                     const ColumnFamilyHandle& column_family, const Slice& key,
                     const Slice& value) = 0;
  Status Put(const WriteOptions& options, const Slice& key,
             const Slice& value) {
    return Put(options, default_column_family, key, value);
  }
  virtual Status Delete(const WriteOptions& options,
                        const ColumnFamilyHandle& column_family,
                        const Slice& key) = 0;
  Status Delete(const WriteOptions& options, const Slice& key) {
    return Delete(options, default_column_family, key);
  }
  virtual Status Merge(const WriteOptions& options,
                       const ColumnFamilyHandle& column_family,
                       const Slice& key, const Slice& value) = 0;
  Status Merge(const WriteOptions& options, const Slice& key,
               const Slice& value) {
    return Merge(options, default_column_family, key, value);
  }
  virtual Status Write(const WriteOptions& options, WriteBatch* updates) = 0;
  virtual Status Get(const ReadOptions& options,
                     const ColumnFamilyHandle& column_family, const Slice& key,
                     std::string* value) = 0;
  Status Get(const ReadOptions& options, const Slice& key, std::string* value) {
    return Get(options, default_column_family, key, value);
  }
  virtual std::vector<Status> MultiGet(
      const ReadOptions& options,
      const std::vector<ColumnFamilyHandle>& column_family,
      const std::vector<Slice>& keys, std::vector<std::string>* values) = 0;
  std::vector<Status> MultiGet(const ReadOptions& options,
                               const std::vector<Slice>& keys,
                               std::vector<std::string>* values) {
    return MultiGet(options, std::vector<ColumnFamilyHandle>(
                                 keys.size(), default_column_family),
                    keys, values);
  }
  virtual bool KeyMayExist(const ReadOptions& options,
                           const ColumnFamilyHandle& column_family,
                           const Slice& key, std::string* value,
                           bool* value_found = nullptr) {
    if (value_found != nullptr) {
      *value_found = false;
    }
    return true;
  }
  bool KeyMayExist(const ReadOptions& options, const Slice& key,
                   std::string* value, bool* value_found = nullptr) {
    return KeyMayExist(options, default_column_family, key, value, value_found);
  }
  virtual Iterator* NewIterator(const ReadOptions& options,
                                const ColumnFamilyHandle& column_family) = 0;
  Iterator* NewIterator(const ReadOptions& options) {
    return NewIterator(options, default_column_family);
  }
  virtual Status NewIterators(
      const ReadOptions& options,
      const std::vector<ColumnFamilyHandle>& column_family,
      std::vector<Iterator*>* iterators) = 0;
  virtual const Snapshot* GetSnapshot() = 0;
  virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;
  virtual bool GetProperty(const ColumnFamilyHandle& column_family,
                           const Slice& property, std::string* value) = 0;
  bool GetProperty(const Slice& property, std::string* value) {
    return GetProperty(default_column_family, property, value);
  }
  virtual void GetApproximateSizes(const ColumnFamilyHandle& column_family,
                                   const Range* range, int n,
                                   uint64_t* sizes) = 0;
  void GetApproximateSizes(const Range* range, int n, uint64_t* sizes) {
    GetApproximateSizes(default_column_family, range, n, sizes);
  }
<<<<<<< HEAD
  virtual void CompactRange(const ColumnFamilyHandle& column_family,
                            const Slice* begin, const Slice* end,
                            bool reduce_level = false,
                            int target_level = -1) = 0;
  void CompactRange(const Slice* begin, const Slice* end,
                    bool reduce_level = false, int target_level = -1) {
    CompactRange(default_column_family, begin, end, reduce_level, target_level);
  }
||||||| fb01755aa
  virtual void CompactRange(const Slice* begin, const Slice* end,
                            bool reduce_level = false,
                            int target_level = -1) = 0;
=======
  virtual Status CompactRange(const Slice* begin, const Slice* end,
                              bool reduce_level = false,
                              int target_level = -1) = 0;
>>>>>>> aba2acb5
  virtual int NumberLevels(const ColumnFamilyHandle& column_family) = 0;
  int NumberLevels() {
    return NumberLevels(default_column_family);
  }
  virtual int MaxMemCompactionLevel(
      const ColumnFamilyHandle& column_family) = 0;
  int MaxMemCompactionLevel() {
    return MaxMemCompactionLevel(default_column_family);
  }
  virtual int Level0StopWriteTrigger(
      const ColumnFamilyHandle& column_family) = 0;
  int Level0StopWriteTrigger() {
    return Level0StopWriteTrigger(default_column_family);
  }
  virtual const std::string& GetName() const = 0;
  virtual Env* GetEnv() const = 0;
  virtual const Options& GetOptions(const ColumnFamilyHandle& column_family)
      const = 0;
  const Options& GetOptions() const {
    return GetOptions(default_column_family);
  }
  virtual Status Flush(const FlushOptions& options,
                       const ColumnFamilyHandle& column_family) = 0;
  Status Flush(const FlushOptions& options) {
    return Flush(options, default_column_family);
  }
  virtual Status DisableFileDeletions() = 0;
  virtual Status EnableFileDeletions(bool force = true) = 0;
  virtual Status GetLiveFiles(std::vector<std::string>&,
                              uint64_t* manifest_file_size,
                              bool flush_memtable = true) = 0;
  virtual Status GetSortedWalFiles(VectorLogPtr& files) = 0;
  virtual SequenceNumber GetLatestSequenceNumber() const = 0;
  virtual Status GetUpdatesSince(SequenceNumber seq_number,
                                 unique_ptr<TransactionLogIterator>* iter) = 0;
  virtual Status DeleteFile(std::string name) = 0;
  virtual void GetLiveFilesMetaData(std::vector<LiveFileMetaData>* metadata) {}
  virtual Status GetDbIdentity(std::string& identity) = 0;
 private:
  DB(const DB&);
  void operator=(const DB&);
};
Status DestroyDB(const std::string& name, const Options& options);
Status RepairDB(const std::string& dbname, const Options& options);
}
#endif
