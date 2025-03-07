#ifndef STORAGE_ROCKSDB_INCLUDE_DB_H_
#define STORAGE_ROCKSDB_INCLUDE_DB_H_ 
#include <stdint.h>
#include <stdio.h>
#include <memory>
#include <vector>
#include <unordered_map>
#include <string>
#include "rocksdb/iterator.h"
#include "rocksdb/options.h"
#include "rocksdb/types.h"
#include "rocksdb/transaction_log.h"
namespace rocksdb {
using std::unique_ptr;
class ColumnFamilyHandle {
 public:
  ~ColumnFamilyHandle() {}
};
extern const std::string default_column_family_name;
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
struct TableProperties;
class WriteBatch;
class Env;
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
  Range() {}
  Range(const Slice& s, const Slice& l) : start(s), limit(l) {}
};
typedef std::unordered_map<std::string, std::shared_ptr<const TableProperties>>
    TablePropertiesCollection;
class DB {
 public:
  static Status Open(const Options& options, const std::string& name,
                     DB** dbptr);
  static Status OpenForReadOnly(const Options& options, const std::string& name,
                                DB** dbptr,
                                bool error_if_log_file_exist = false);
  static Status Open(const DBOptions& db_options, const std::string& name,
                     const std::vector<ColumnFamilyDescriptor>& column_families,
                     std::vector<ColumnFamilyHandle*>* handles, DB** dbptr);
  static Status ListColumnFamilies(const DBOptions& db_options,
                                   const std::string& name,
                                   std::vector<std::string>* column_families);
  DB() {}
  virtual ~DB();
  virtual Status CreateColumnFamily(const ColumnFamilyOptions& options,
                                    const std::string& column_family_name,
                                    ColumnFamilyHandle** handle);
  virtual Status DropColumnFamily(ColumnFamilyHandle* column_family);
  virtual Status Put(const WriteOptions& options,
                     ColumnFamilyHandle* column_family, const Slice& key,
                     const Slice& value) = 0;
  Status Put(const WriteOptions& options, const Slice& key,
             const Slice& value) {
    return Put(options, DefaultColumnFamily(), key, value);
  }
  virtual Status Delete(const WriteOptions& options,
                        ColumnFamilyHandle* column_family,
                        const Slice& key) = 0;
  Status Delete(const WriteOptions& options, const Slice& key) {
    return Delete(options, DefaultColumnFamily(), key);
  }
  virtual Status Merge(const WriteOptions& options,
                       ColumnFamilyHandle* column_family, const Slice& key,
                       const Slice& value) = 0;
  Status Merge(const WriteOptions& options, const Slice& key,
               const Slice& value) {
    return Merge(options, DefaultColumnFamily(), key, value);
  }
  virtual Status Write(const WriteOptions& options, WriteBatch* updates) = 0;
  virtual Status Get(const ReadOptions& options,
                     ColumnFamilyHandle* column_family, const Slice& key,
                     std::string* value) = 0;
  Status Get(const ReadOptions& options, const Slice& key, std::string* value) {
    return Get(options, DefaultColumnFamily(), key, value);
  }
  virtual std::vector<Status> MultiGet(
      const ReadOptions& options,
      const std::vector<ColumnFamilyHandle*>& column_family,
      const std::vector<Slice>& keys, std::vector<std::string>* values) = 0;
  std::vector<Status> MultiGet(const ReadOptions& options,
                               const std::vector<Slice>& keys,
                               std::vector<std::string>* values) {
    return MultiGet(
        options,
        std::vector<ColumnFamilyHandle*>(keys.size(), DefaultColumnFamily()),
        keys, values);
  }
  virtual bool KeyMayExist(const ReadOptions& options,
                           ColumnFamilyHandle* column_family, const Slice& key,
                           std::string* value, bool* value_found = nullptr) {
    if (value_found != nullptr) {
      *value_found = false;
    }
    return true;
  }
  bool KeyMayExist(const ReadOptions& options, const Slice& key,
                   std::string* value, bool* value_found = nullptr) {
    return KeyMayExist(options, DefaultColumnFamily(), key, value, value_found);
  }
  virtual Iterator* NewIterator(const ReadOptions& options,
                                ColumnFamilyHandle* column_family) = 0;
  Iterator* NewIterator(const ReadOptions& options) {
    return NewIterator(options, DefaultColumnFamily());
  }
  virtual Status NewIterators(
      const ReadOptions& options,
      const std::vector<ColumnFamilyHandle*>& column_family,
      std::vector<Iterator*>* iterators) = 0;
  virtual const Snapshot* GetSnapshot() = 0;
  virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;
  virtual bool GetProperty(ColumnFamilyHandle* column_family,
                           const Slice& property, std::string* value) = 0;
  bool GetProperty(const Slice& property, std::string* value) {
    return GetProperty(DefaultColumnFamily(), property, value);
  }
  virtual void GetApproximateSizes(ColumnFamilyHandle* column_family,
                                   const Range* range, int n,
                                   uint64_t* sizes) = 0;
  void GetApproximateSizes(const Range* range, int n, uint64_t* sizes) {
    GetApproximateSizes(DefaultColumnFamily(), range, n, sizes) {
      GetApproximateSizes(DefaultColumnFamily(), range, n, sizes);
    }
    virtual Status CompactRange(
        ColumnFamilyHandle * column_family, const Slice* begin,
        const Slice* end, bool reduce_level = false, int target_level = -1) = 0;
    Status CompactRange(const Slice* begin, const Slice* end,
                        bool reduce_level = false, int target_level = -1) {
      return CompactRange(DefaultColumnFamily(), begin, end, reduce_level,
                          target_level);
    }
    virtual int NumberLevels(ColumnFamilyHandle * column_family) = 0;
    int NumberLevels() {
      return NumberLevels(DefaultColumnFamily()) {
        return NumberLevels(DefaultColumnFamily());
      }
      virtual int MaxMemCompactionLevel(ColumnFamilyHandle * column_family) = 0;
      int MaxMemCompactionLevel() {
        return MaxMemCompactionLevel(DefaultColumnFamily());
      }
      virtual int Level0StopWriteTrigger(ColumnFamilyHandle *
                                         column_family) = 0;
      int Level0StopWriteTrigger() {
        return Level0StopWriteTrigger(DefaultColumnFamily());
      }
      virtual const std::string& GetName() const = 0;
      virtual Env* GetEnv() const = 0;
      virtual const Options& GetOptions(ColumnFamilyHandle * column_family)
          const = 0;
      const Options& GetOptions() const {
        return GetOptions(DefaultColumnFamily());
      }
      virtual Status Flush(const FlushOptions& options,
                           ColumnFamilyHandle* column_family) = 0;
      Status Flush(const FlushOptions& options) {
        return Flush(options, DefaultColumnFamily());
      }
      virtual Status DisableFileDeletions() = 0;
      virtual Status EnableFileDeletions(bool force = true) = 0;
      virtual Status GetLiveFiles(std::vector<std::string>&,
                                  uint64_t * manifest_file_size,
                                  bool flush_memtable = true) = 0;
      virtual Status GetSortedWalFiles(VectorLogPtr & files) = 0;
      virtual SequenceNumber GetLatestSequenceNumber() const = 0;
      virtual Status GetUpdatesSince(
          SequenceNumber seq_number,
          unique_ptr<TransactionLogIterator> * iter) = 0;
      virtual Status DeleteFile(std::string name) = 0;
      virtual void GetLiveFilesMetaData(std::vector<LiveFileMetaData> *
                                        metadata) {}
      virtual Status GetDbIdentity(std::string & identity) = 0;
      virtual ColumnFamilyHandle* DefaultColumnFamily() const = 0;
      virtual Status GetPropertiesOfAllTables(TablePropertiesCollection *
                                              props) = 0;
     private:
      DB(const DB&);
      void operator=(const DB&);
    };
    Status DestroyDB(const std::string& name, const Options& options);
    Status RepairDB(const std::string& dbname, const Options& options);
  }
#endif
