//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <algorithm>
#include <iostream>
#include <set>
#include <unistd.h>
#include <unordered_set>
#include "db/dbformat.h"
#include "db/db_impl.h"
#include "db/filename.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include "rocksdb/cache.h"
#include "rocksdb/compaction_filter.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/perf_context.h"
#include "rocksdb/slice.h"
#include "rocksdb/slice_transform.h"
#include "rocksdb/table.h"
#include "rocksdb/table_properties.h"
#include "table/block_based_table_factory.h"
#include "table/plain_table_factory.h"
#include "util/hash.h"
#include "util/hash_linklist_rep.h"
#include "utilities/merge_operators.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/statistics.h"
#include "util/testharness.h"
#include "util/testutil.h"

namespace rocksdb {

static bool SnappyCompressionSupported(const CompressionOptions& options) {
  std::string out;
  Slice in = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  return port::Snappy_Compress(options, in.data(), in.size(), &out);
}

static bool ZlibCompressionSupported(const CompressionOptions& options) {
  std::string out;
  Slice in = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  return port::Zlib_Compress(options, in.data(), in.size(), &out);
}

static bool BZip2CompressionSupported(const CompressionOptions& options) {
  std::string out;
  Slice in = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  return port::BZip2_Compress(options, in.data(), in.size(), &out);
}

static bool LZ4CompressionSupported(const CompressionOptions& options) {
  std::string out;
  Slice in = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  return port::LZ4_Compress(options, in.data(), in.size(), &out);
}

static bool LZ4HCCompressionSupported(const CompressionOptions& options) {
  std::string out;
  Slice in = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  return port::LZ4HC_Compress(options, in.data(), in.size(), &out);
}

static std::string RandomString(Random* rnd, int len) {
  std::string r;
  test::RandomString(rnd, len, &r);
  return r;
}

namespace anon {
class AtomicCounter {
 private:
  port::Mutex mu_;
  int count_;

 public:
  AtomicCounter() : count_(0) {}
  void Increment() {
    MutexLock l(&mu_);
    count_++;
  }
  int Read() {
    MutexLock l(&mu_);
    return count_;
  }
  void Reset() {
    MutexLock l(&mu_);
    count_ = 0;
  }
};

}  // namespace anon

// Special Env used to delay background operations
class SpecialEnv : public EnvWrapper {
 public:
  // sstable Sync() calls are blocked while this pointer is non-nullptr.
  port::AtomicPointer delay_sstable_sync_;

  // Simulate no-space errors while this pointer is non-nullptr.
  port::AtomicPointer no_space_;

  // Simulate non-writable file system while this pointer is non-nullptr
  port::AtomicPointer non_writable_;

  // Force sync of manifest files to fail while this pointer is non-nullptr
  port::AtomicPointer manifest_sync_error_;

  // Force write to manifest files to fail while this pointer is non-nullptr
  port::AtomicPointer manifest_write_error_;

  // Force write to log files to fail while this pointer is non-nullptr
  port::AtomicPointer log_write_error_;

  bool count_random_reads_;
  anon::AtomicCounter random_read_counter_;

  anon::AtomicCounter sleep_counter_;

  explicit SpecialEnv(Env* base) : EnvWrapper(base) {
    delay_sstable_sync_.Release_Store(nullptr);
    no_space_.Release_Store(nullptr);
    non_writable_.Release_Store(nullptr);
    count_random_reads_ = false;
    manifest_sync_error_.Release_Store(nullptr);
    manifest_write_error_.Release_Store(nullptr);
    log_write_error_.Release_Store(nullptr);
  }

  Status NewWritableFile(const std::string& f, unique_ptr<WritableFile>* r,
                         const EnvOptions& soptions) {
    class SSTableFile : public WritableFile {
     private:
      SpecialEnv* env_;
      unique_ptr<WritableFile> base_;

     public:
      SSTableFile(SpecialEnv* env, unique_ptr<WritableFile>&& base)
          : env_(env), base_(std::move(base)) {}
      Status Append(const Slice& data) {
        if (env_->no_space_.Acquire_Load() != nullptr) {
          // Drop writes on the floor
          return Status::OK();
        } else {
          return base_->Append(data);
        }
      }
      Status Close() { return base_->Close(); }
      Status Flush() { return base_->Flush(); }
      Status Sync() {
        while (env_->delay_sstable_sync_.Acquire_Load() != nullptr) {
          env_->SleepForMicroseconds(100000);
        }
        return base_->Sync();
      }
    };
    class ManifestFile : public WritableFile {
     private:
      SpecialEnv* env_;
      unique_ptr<WritableFile> base_;

     public:
      ManifestFile(SpecialEnv* env, unique_ptr<WritableFile>&& b)
          : env_(env), base_(std::move(b)) {}
      Status Append(const Slice& data) {
        if (env_->manifest_write_error_.Acquire_Load() != nullptr) {
          return Status::IOError("simulated writer error");
        } else {
          return base_->Append(data);
        }
      }
      Status Close() { return base_->Close(); }
      Status Flush() { return base_->Flush(); }
      Status Sync() {
        if (env_->manifest_sync_error_.Acquire_Load() != nullptr) {
          return Status::IOError("simulated sync error");
        } else {
          return base_->Sync();
        }
      }
    };
    class LogFile : public WritableFile {
     private:
      SpecialEnv* env_;
      unique_ptr<WritableFile> base_;

     public:
      LogFile(SpecialEnv* env, unique_ptr<WritableFile>&& b)
          : env_(env), base_(std::move(b)) {}
      Status Append(const Slice& data) {
        if (env_->log_write_error_.Acquire_Load() != nullptr) {
          return Status::IOError("simulated writer error");
        } else {
          return base_->Append(data);
        }
      }
      Status Close() { return base_->Close(); }
      Status Flush() { return base_->Flush(); }
      Status Sync() { return base_->Sync(); }
    };

    if (non_writable_.Acquire_Load() != nullptr) {
      return Status::IOError("simulated write error");
    }

    Status s = target()->NewWritableFile(f, r, soptions);
    if (s.ok()) {
      if (strstr(f.c_str(), ".sst") != nullptr) {
        r->reset(new SSTableFile(this, std::move(*r)));
      } else if (strstr(f.c_str(), "MANIFEST") != nullptr) {
        r->reset(new ManifestFile(this, std::move(*r)));
      } else if (strstr(f.c_str(), "log") != nullptr) {
        r->reset(new LogFile(this, std::move(*r)));
      }
    }
    return s;
  }

  Status NewRandomAccessFile(const std::string& f,
                             unique_ptr<RandomAccessFile>* r,
                             const EnvOptions& soptions) {
    class CountingFile : public RandomAccessFile {
     private:
      unique_ptr<RandomAccessFile> target_;
      anon::AtomicCounter* counter_;

     public:
      CountingFile(unique_ptr<RandomAccessFile>&& target,
                   anon::AtomicCounter* counter)
          : target_(std::move(target)), counter_(counter) {}
      virtual Status Read(uint64_t offset, size_t n, Slice* result,
                          char* scratch) const {
        counter_->Increment();
        return target_->Read(offset, n, result, scratch);
      }
    };

    Status s = target()->NewRandomAccessFile(f, r, soptions);
    if (s.ok() && count_random_reads_) {
      r->reset(new CountingFile(std::move(*r), &random_read_counter_));
    }
    return s;
  }

  virtual void SleepForMicroseconds(int micros) {
    sleep_counter_.Increment();
    target()->SleepForMicroseconds(micros);
  }
};

class DBTest {
 private:
  const FilterPolicy* filter_policy_;

 protected:
  // Sequence of option configurations to try
  enum OptionConfig {
    kDefault,
    kPlainTableFirstBytePrefix,
    kPlainTableAllBytesPrefix,
    kVectorRep,
    kHashLinkList,
    kMergePut,
    kFilter,
    kUncompressed,
    kNumLevel_3,
    kDBLogDir,
    kWalDir,
    kManifestFileSize,
    kCompactOnFlush,
    kPerfOptions,
    kDeletesFilterFirst,
    kHashSkipList,
    kUniversalCompaction,
    kCompressedBlockCache,
    kInfiniteMaxOpenFiles,
    kEnd
  };
  int option_config_;

 public:
  std::string dbname_;
  SpecialEnv* env_;
  DB* db_;

  std::vector<ColumnFamilyHandle*> handles_;

  Options last_options_;

  // Skip some options, as they may not be applicable to a specific test.
  // To add more skip constants, use values 4, 8, 16, etc.
  enum OptionSkip {
    kNoSkip = 0,
    kSkipDeletesFilterFirst = 1,
    kSkipUniversalCompaction = 2,
    kSkipMergePut = 4,
    kSkipPlainTable = 8
  };

  DBTest() : option_config_(kDefault), env_(new SpecialEnv(Env::Default())) {
    last_options_.max_background_flushes = 0;
    filter_policy_ = NewBloomFilterPolicy(10);
    dbname_ = test::TmpDir() + "/db_test";
    ASSERT_OK(DestroyDB(dbname_, Options()));
    db_ = nullptr;
    Reopen();
  }

  ~DBTest() {
    Close();
    ASSERT_OK(DestroyDB(dbname_, Options()));
    delete env_;
    delete filter_policy_;
  }
  {
    Close();
    ASSERT_OK(DestroyDB(dbname_, Options()));
    delete env_;
    delete filter_policy_;
  }

  // Switch to a fresh database with the next option configuration to
  // test.  Return false if there are no more configurations to test.
  bool ChangeOptions(int skip_mask = kNoSkip) {
    // skip some options
    for (option_config_++; option_config_ < kEnd; option_config_++) {
      if ((skip_mask & kSkipDeletesFilterFirst) &&
          option_config_ == kDeletesFilterFirst) {
        continue;
      }
      if ((skip_mask & kSkipUniversalCompaction) &&
          option_config_ == kUniversalCompaction) {
        continue;
      }
      if ((skip_mask & kSkipMergePut) && option_config_ == kMergePut) {
        continue;
      }
      if ((skip_mask & kSkipPlainTable) &&
          (option_config_ == kPlainTableAllBytesPrefix ||
           option_config_ == kPlainTableFirstBytePrefix)) {
        continue;
      }
      break;
    }

    if (option_config_ >= kEnd) {
      Destroy(&last_options_);
      return false;
    } else {
      DestroyAndReopen();
      return true;
    }
  }

  // Switch between different compaction styles (we have only 2 now).
  bool ChangeCompactOptions(Options* prev_options = nullptr) {
    if (option_config_ == kDefault) {
      option_config_ = kUniversalCompaction;
      if (prev_options == nullptr) {
        prev_options = &last_options_;
      }
      Destroy(prev_options);
      TryReopen();
      return true;
    } else {
      return false;
    }
  }

  // Return the current option configuration.
  Options CurrentOptions() {
    Options options;
    options.paranoid_checks = false;
    options.max_background_flushes = 0;
    switch (option_config_) {
      case kHashSkipList:
        options.prefix_extractor.reset(NewFixedPrefixTransform(1));
        options.memtable_factory.reset(NewHashSkipListRepFactory());
        break;
      case kPlainTableFirstBytePrefix:
        options.table_factory.reset(new PlainTableFactory());
        options.prefix_extractor.reset(NewFixedPrefixTransform(1));
        options.allow_mmap_reads = true;
        options.max_sequential_skip_in_iterations = 999999;
        break;
      case kPlainTableAllBytesPrefix:
        options.table_factory.reset(new PlainTableFactory());
        options.prefix_extractor.reset(NewNoopTransform());
        options.allow_mmap_reads = true;
        options.max_sequential_skip_in_iterations = 999999;
        break;
      case kMergePut:
        options.merge_operator = MergeOperators::CreatePutOperator();
        break;
      case kFilter:
        options.filter_policy = filter_policy_;
        break;
      case kUncompressed:
        options.compression = kNoCompression;
        break;
      case kNumLevel_3:
        options.num_levels = 3;
        break;
      case kDBLogDir:
        options.db_log_dir = test::TmpDir();
        break;
      case kWalDir:
        options.wal_dir = "/tmp/wal";
        break;
      case kManifestFileSize:
        options.max_manifest_file_size = 50;  // 50 bytes
      case kCompactOnFlush:
        options.purge_redundant_kvs_while_flush =
            !options.purge_redundant_kvs_while_flush;
        break;
      case kPerfOptions:
        options.hard_rate_limit = 2.0;
        options.rate_limit_delay_max_milliseconds = 2;
        // TODO -- test more options
        break;
      case kDeletesFilterFirst:
        options.filter_deletes = true;
        break;
      case kVectorRep:
        options.memtable_factory.reset(new VectorRepFactory(100));
        break;
      case kHashLinkList:
        options.prefix_extractor.reset(NewFixedPrefixTransform(1));
        options.memtable_factory.reset(NewHashLinkListRepFactory(4));
        break;
      case kUniversalCompaction:
        options.compaction_style = kCompactionStyleUniversal;
        break;
      case kCompressedBlockCache:
        options.allow_mmap_writes = true;
        options.block_cache_compressed = NewLRUCache(8 * 1024 * 1024);
        break;
      case kInfiniteMaxOpenFiles:
        options.max_open_files = -1;
        break;
      default:
        break;
    }
    return options;
  }

  DBImpl* dbfull() { return reinterpret_cast<DBImpl*>(db_); }

  void CreateColumnFamilies(const std::vector<std::string>& cfs,
                            const ColumnFamilyOptions* options = nullptr) {
    ColumnFamilyOptions cf_opts;
    if (options != nullptr) {
      cf_opts = ColumnFamilyOptions(*options);
    } else {
      cf_opts = ColumnFamilyOptions(CurrentOptions());
    }
    int cfi = handles_.size();
    handles_.resize(cfi + cfs.size());
    for (auto cf : cfs) {
      ASSERT_OK(db_->CreateColumnFamily(cf_opts, cf, &handles_[cfi++]));
    }
  }

  void CreateAndReopenWithCF(const std::vector<std::string>& cfs,
                             const Options* options = nullptr) {
    CreateColumnFamilies(cfs, options);
    std::vector<std::string> cfs_plus_default = cfs;
    cfs_plus_default.insert(cfs_plus_default.begin(),
                            default_column_family_name);
    ReopenWithColumnFamilies(cfs_plus_default, options);
  }

  void ReopenWithColumnFamilies(const std::vector<std::string>& cfs,
                                const std::vector<const Options*>& options) {
    ASSERT_OK(TryReopenWithColumnFamilies(cfs, options));
  }

  void ReopenWithColumnFamilies(const std::vector<std::string>& cfs,
                                const Options* options = nullptr) {
    ASSERT_OK(TryReopenWithColumnFamilies(cfs, options));
  }

  Status TryReopenWithColumnFamilies(
      const std::vector<std::string>& cfs,
      const std::vector<const Options*>& options) {
    Close();
    ASSERT_EQ(cfs.size(), options.size());
    std::vector<ColumnFamilyDescriptor> column_families;
    for (size_t i = 0; i < cfs.size(); ++i) {
      column_families.push_back(ColumnFamilyDescriptor(cfs[i], *options[i]));
    }
    DBOptions db_opts = DBOptions(*options[0]);
    return DB::Open(db_opts, dbname_, column_families, &handles_, &db_);
  }

  Status TryReopenWithColumnFamilies(const std::vector<std::string>& cfs,
                                     const Options* options = nullptr) {
    Close();
    Options opts = (options == nullptr) ? CurrentOptions() : *options;
    std::vector<const Options*> v_opts(cfs.size(), &opts);
    return TryReopenWithColumnFamilies(cfs, v_opts);
  }

  void Reopen(Options* options = nullptr) { ASSERT_OK(TryReopen(options)); }

  void Close() {
    for (auto h : handles_) {
      delete h;
    }
    handles_.clear();
    delete db_;
    db_ = nullptr;
  }

  void DestroyAndReopen(Options* options = nullptr) {
    // Destroy using last options
    Destroy(&last_options_);
    ASSERT_OK(TryReopen(options));
  }

  void Destroy(Options* options) {
    Close();
    ASSERT_OK(DestroyDB(dbname_, *options));
  }

  Status ReadOnlyReopen(Options* options) {
    return DB::OpenForReadOnly(*options, dbname_, &db_);
  }

  Status TryReopen(Options* options = nullptr) {
    Close();
    Options opts;
    if (options != nullptr) {
      opts = *options;
    } else {
      opts = CurrentOptions();
      opts.create_if_missing = true;
    }
    last_options_ = opts;

    return DB::Open(opts, dbname_, &db_);
  }

  Status Flush(int cf = 0) {
    if (cf == 0) {
      return db_->Flush(FlushOptions());
    } else {
      return db_->Flush(FlushOptions(), handles_[cf]);
    }
  }

  Status Put(const Slice& k, const Slice& v, WriteOptions wo = WriteOptions()) {
    if (kMergePut == option_config_) {
      return db_->Merge(wo, k, v);
    } else {
      return db_->Put(wo, k, v);
    }
  }

  Status Put(int cf, const Slice& k, const Slice& v,
             WriteOptions wo = WriteOptions()) {
    if (kMergePut == option_config_) {
      return db_->Merge(wo, handles_[cf], k, v);
    } else {
      return db_->Put(wo, handles_[cf], k, v);
    }
  }

  Status Delete(const std::string& k) { return db_->Delete(WriteOptions(), k); }

  Status Delete(int cf, const std::string& k) {
    return db_->Delete(WriteOptions(), handles_[cf], k);
  }

  std::string Get(const std::string& k, const Snapshot* snapshot = nullptr) {
    ReadOptions options;
    options.verify_checksums = true;
    options.snapshot = snapshot;
    std::string result;
    Status s = db_->Get(options, k, &result);
    if (s.IsNotFound()) {
      result = "NOT_FOUND";
    } else if (!s.ok()) {
      result = s.ToString();
    }
    return result;
  }

  std::string Get(int cf, const std::string& k,
                  const Snapshot* snapshot = nullptr) {
    ReadOptions options;
    options.verify_checksums = true;
    options.snapshot = snapshot;
    std::string result;
    Status s = db_->Get(options, handles_[cf], k, &result);
    if (s.IsNotFound()) {
      result = "NOT_FOUND";
    } else if (!s.ok()) {
      result = s.ToString();
    }
    return result;
  }

  // Return a string that contains all key,value pairs in order,
  // formatted like "(k1->v1)(k2->v2)".
  std::string Contents(int cf = 0) {
    std::vector<std::string> forward;
    std::string result;
    Iterator* iter = (cf == 0) ? db_->NewIterator(ReadOptions())
                               : db_->NewIterator(ReadOptions(), handles_[cf]);
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      std::string s = IterStatus(iter);
      result.push_back('(');
      result.append(s);
      result.push_back(')');
      forward.push_back(s);
    }

    // Check reverse iteration results are the reverse of forward results
    unsigned int matched = 0;
    for (iter->SeekToLast(); iter->Valid(); iter->Prev()) {
      ASSERT_LT(matched, forward.size());
      ASSERT_EQ(IterStatus(iter), forward[forward.size() - matched - 1]);
      matched++;
    }
    ASSERT_EQ(matched, forward.size());

    delete iter;
    return result;
  }

  int NumTableFilesAtLevel(int level, int cf = 0) {
    std::string property;
    if (cf == 0) {
      // default cfd
      ASSERT_TRUE(db_->GetProperty(
          "rocksdb.num-files-at-level" + NumberToString(level), &property));
    } else {
      ASSERT_TRUE(db_->GetProperty(
          handles_[cf], "rocksdb.num-files-at-level" + NumberToString(level),
          &property));
    }
    return atoi(property.c_str());
  }

  int CountFiles() {
    std::vector<std::string> files;
    env_->GetChildren(dbname_, &files);

    std::vector<std::string> logfiles;
    if (dbname_ != last_options_.wal_dir) {
      env_->GetChildren(last_options_.wal_dir, &logfiles);
    }

    return static_cast<int>(files.size() + logfiles.size());
  }

  int CountLiveFiles() {
    std::vector<LiveFileMetaData> metadata;
    db_->GetLiveFilesMetaData(&metadata);
    return metadata.size();
  }

  uint64_t Size(const Slice& start, const Slice& limit, int cf = 0) {
    Range r(start, limit);
    uint64_t size;
    if (cf == 0) {
      db_->GetApproximateSizes(&r, 1, &size);
    } else {
      db_->GetApproximateSizes(handles_[1], &r, 1, &size);
    }
    return size;
  }

  void Compact(int cf, const Slice& start, const Slice& limit) {
    ASSERT_OK(db_->CompactRange(handles_[cf], &start, &limit));
  }

  void Compact(const Slice& start, const Slice& limit) {
    ASSERT_OK(db_->CompactRange(&start, &limit));
  }

  // Do n memtable compactions, each of which produces an sstable
  // covering the range [small,large].
  void MakeTables(int n, const std::string& small, const std::string& large,
                  int cf = 0) {
    for (int i = 0; i < n; i++) {
      ASSERT_OK(Put(cf, small, "begin"));
      ASSERT_OK(Put(cf, large, "end"));
      ASSERT_OK(Flush(cf));
    }
  }

  // Prevent pushing of new sstables into deeper levels by adding
  // tables that cover a specified range to all levels.
  void FillLevels(const std::string& smallest, const std::string& largest,
                  int cf) {
    MakeTables(db_->NumberLevels(handles_[cf]), smallest, largest, cf);
  }

  void DumpFileCounts(const char* label) {
    fprintf(stderr, "---\n%s:\n", label);
    fprintf(
        stderr, "maxoverlap: %lld\n",
        static_cast<long long>(dbfull()->TEST_MaxNextLevelOverlappingBytes()));
    for (int level = 0; level < db_->NumberLevels(); level++) {
      int num = NumTableFilesAtLevel(level);
      if (num > 0) {
        fprintf(stderr, "  level %3d : %d files\n", level, num);
      }
    }
  }

  std::string DumpSSTableList() {
    std::string property;
    db_->GetProperty("rocksdb.sstables", &property);
    return property;
  }

  std::string IterStatus(Iterator* iter) {
    std::string result;
    if (iter->Valid()) {
      result = iter->key().ToString() + "->" + iter->value().ToString();
    } else {
      result = "(invalid)";
    }
    return result;
  }

  Options OptionsForLogIterTest() {
    Options options = CurrentOptions();
    options.create_if_missing = true;
    options.WAL_ttl_seconds = 1000;
    return options;
  }

  std::unique_ptr<TransactionLogIterator> OpenTransactionLogIter(
      const SequenceNumber seq) {
    unique_ptr<TransactionLogIterator> iter;
    Status status = dbfull()->GetUpdatesSince(seq, &iter);
    ASSERT_OK(status);
    ASSERT_TRUE(iter->Valid());
    return std::move(iter);
  }

  std::string DummyString(size_t len, char c = 'a') {
    return std::string(len, c);
  }

  void VerifyIterLast(std::string expected_key, int cf = 0) {
    Iterator* iter;
    if (cf == 0) {
      iter = db_->NewIterator(ReadOptions());
    } else {
      iter = db_->NewIterator(ReadOptions(), handles_[cf]);
    }
    iter->SeekToLast();
    ASSERT_EQ(IterStatus(iter), expected_key);
    delete iter;
  }

  // Used to test InplaceUpdate
  // If previous value is nullptr or delta is > than previous value,
  //   sets newValue with delta
  // If previous value is not empty,
  //   updates previous value with 'b' string of previous value size - 1.
  static UpdateStatus updateInPlaceSmallerSize(char* prevValue,
                                               uint32_t* prevSize, Slice delta,
                                               std::string* newValue) {
    if (prevValue == nullptr) {
      *newValue = std::string(delta.size(), 'c');
      return UpdateStatus::UPDATED;
    } else {
      *prevSize = *prevSize - 1;
      std::string str_b = std::string(*prevSize, 'b');
      memcpy(prevValue, str_b.c_str(), str_b.size());
      return UpdateStatus::UPDATED_INPLACE;
    }
  }

  static UpdateStatus updateInPlaceSmallerVarintSize(char* prevValue,
                                                     uint32_t* prevSize,
                                                     Slice delta,
                                                     std::string* newValue) {
    if (prevValue == nullptr) {
      *newValue = std::string(delta.size(), 'c');
      return UpdateStatus::UPDATED;
    } else {
      *prevSize = 1;
      std::string str_b = std::string(*prevSize, 'b');
      memcpy(prevValue, str_b.c_str(), str_b.size());
      return UpdateStatus::UPDATED_INPLACE;
    }
  }

  static UpdateStatus updateInPlaceLargerSize(char* prevValue,
                                              uint32_t* prevSize, Slice delta,
                                              std::string* newValue) {
    *newValue = std::string(delta.size(), 'c');
    return UpdateStatus::UPDATED;
  }

  static UpdateStatus updateInPlaceNoAction(char* prevValue, uint32_t* prevSize,
                                            Slice delta,
                                            std::string* newValue) {
    return UpdateStatus::UPDATE_FAILED;
  }

  // Utility method to test InplaceUpdate
  void validateNumberOfEntries(int numValues, int cf = 0) {
    Iterator* iter;
    if (cf != 0) {
      iter = dbfull()->TEST_NewInternalIterator(handles_[cf]);
    } else {
      iter = dbfull()->TEST_NewInternalIterator();
    }
    iter->SeekToFirst();
    ASSERT_EQ(iter->status().ok(), true);
    int seq = numValues;
    while (iter->Valid()) {
      ParsedInternalKey ikey;
      ikey.sequence = -1;
      ASSERT_EQ(ParseInternalKey(iter->key(), &ikey), true);

      // checks sequence number for updates
      ASSERT_EQ(ikey.sequence, (unsigned)seq--);
      iter->Next();
    }
    delete iter;
    ASSERT_EQ(0, seq);
  }

  void CopyFile(const std::string& source, const std::string& destination,
                uint64_t size = 0) {
    const EnvOptions soptions;
    unique_ptr<SequentialFile> srcfile;
    ASSERT_OK(env_->NewSequentialFile(source, &srcfile, soptions));
    unique_ptr<WritableFile> destfile;
    ASSERT_OK(env_->NewWritableFile(destination, &destfile, soptions));

    if (size == 0) {
      // default argument means copy everything
      ASSERT_OK(env_->GetFileSize(source, &size));
    }

    char buffer[4096];
    Slice slice;
    while (size > 0) {
      uint64_t one = std::min(uint64_t(sizeof(buffer)), size);
      ASSERT_OK(srcfile->Read(one, &slice, buffer));
      ASSERT_OK(destfile->Append(slice));
      size -= slice.size();
    }
    ASSERT_OK(destfile->Close());
  }

  std::string AllEntriesFor(const Slice& user_key, int cf = 0) {
    Iterator* iter;
    if (cf == 0) {
      iter = dbfull()->TEST_NewInternalIterator();
    } else {
      iter = dbfull()->TEST_NewInternalIterator(handles_[cf]);
    }
    InternalKey target(user_key, kMaxSequenceNumber, kTypeValue);
    iter->Seek(target.Encode());
    std::string result;
    if (!iter->status().ok()) {
      result = iter->status().ToString();
    } else {
      result = "[ ";
      bool first = true;
      while (iter->Valid()) {
        ParsedInternalKey ikey(Slice(), 0, kTypeValue);
        if (!ParseInternalKey(iter->key(), &ikey)) {
          result += "CORRUPTED";
        } else {
          if (last_options_.comparator->Compare(ikey.user_key, user_key) != 0) {
            break;
          }
          if (!first) {
            result += ", ";
          }
          first = false;
          switch (ikey.type) {
            case kTypeValue:
              result += iter->value().ToString();
              break;
            case kTypeMerge:
              // keep it the same as kTypeValue for testing kMergePut
              result += iter->value().ToString();
              break;
            case kTypeDeletion:
              result += "DEL";
              break;
            default:
              assert(false);
              break;
          }
        }
        iter->Next();
      }
      if (!first) {
        result += " ";
      }
      result += "]";
    }
    delete iter;
    return result;
  }

  int TotalTableFiles(int cf = 0, int levels = -1) {
    if (levels == -1) {
      levels = CurrentOptions().num_levels;
    }
    int result = 0;
    for (int level = 0; level < levels; level++) {
      result += NumTableFilesAtLevel(level, cf);
    }
    return result;
  }

  // Return spread of files per level
  std::string FilesPerLevel(int cf = 0) {
    int num_levels =
        (cf == 0) ? db_->NumberLevels() : db_->NumberLevels(handles_[1]);
    std::string result;
    int last_non_zero_offset = 0;
    for (int level = 0; level < num_levels; level++) {
      int f = NumTableFilesAtLevel(level, cf);
      char buf[100];
      snprintf(buf, sizeof(buf), "%s%d", (level ? "," : ""), f);
      result += buf;
      if (f > 0) {
        last_non_zero_offset = result.size();
      }
    }
    result.resize(last_non_zero_offset);
    return result;
  }
};

static std::string Key(int i) {
  char buf[100];
  snprintf(buf, sizeof(buf), "key%06d", i);
  return std::string(buf);
}

static long TestGetTickerCount(const Options& options, Tickers ticker_type) {
  return options.statistics->getTickerCount(ticker_type);
}

// A helper function that ensures the table properties returned in
// `GetPropertiesOfAllTablesTest` is correct.
// This test assumes entries size is differnt for each of the tables.
void VerifyTableProperties(DB* db, uint64_t expected_entries_size) {
  TablePropertiesCollection props;
  ASSERT_OK(db->GetPropertiesOfAllTables(&props));

  assert(props.size() == 4);
  ASSERT_EQ(4U, props.size());
  std::unordered_set<uint64_t> unique_entries;

  // Indirect test
  uint64_t sum = 0;
  for (const auto& item : props) {
    unique_entries.insert(item.second->num_entries);
    sum += item.second->num_entries;
  }

  ASSERT_EQ(props.size(), unique_entries.size());
  ASSERT_EQ(expected_entries_size, sum);
}

std::unordered_map<std::string, size_t> GetMemoryUsage(MemTable* memtable) {
  const auto& arena = memtable->TEST_GetArena();
  return {{"memtable.approximate.usage", memtable->ApproximateMemoryUsage()},
          {"arena.approximate.usage", arena.ApproximateMemoryUsage()},
          {"arena.allocated.memory", arena.MemoryAllocatedBytes()},
          {"arena.unused.bytes", arena.AllocatedAndUnused()},
          {"irregular.blocks", arena.IrregularBlockNum()}};
}

void PrintMemoryUsage(const std::unordered_map<std::string, size_t>& usage) {
  for (const auto& item : usage) {
    std::cout << "\t" << item.first << ": " << item.second << std::endl;
  }
}

void AddRandomKV(MemTable* memtable, Random* rnd, size_t arena_block_size) {
  memtable->Add(0, kTypeValue, RandomString(rnd, 20) /* key */,
                // make sure we will be able to generate some over sized entries
                RandomString(rnd, rnd->Uniform(arena_block_size / 4) * 1.15 +
                                      10) /* value */);
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

class SleepingBackgroundTask {
 public:
  SleepingBackgroundTask() : bg_cv_(&mutex_), should_sleep_(true) {}
  void DoSleep() {
    MutexLock l(&mutex_);
    while (should_sleep_) {
      bg_cv_.Wait();
    }
  }
  void WakeUp() {
    MutexLock l(&mutex_);
    should_sleep_ = false;
    bg_cv_.SignalAll();
  }

  static void DoSleepTask(void* arg) {
    reinterpret_cast<SleepingBackgroundTask*>(arg)->DoSleep();
  }

 private:
  port::Mutex mutex_;
  port::CondVar bg_cv_;  // Signalled when background work finishes
  bool should_sleep_;
};

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

// This is a static filter used for filtering
// kvs during the compaction process.
static int cfilter_count;
static std::string NEW_VALUE = "NewValue";

class KeepFilter : public CompactionFilter {
 public:
  virtual bool Filter(int level, const Slice& key, const Slice& value,
                      std::string* new_value,
                      bool* value_changed) const override {
    cfilter_count++;
    return false;
  }

  virtual const char* Name() const override { return "KeepFilter"; }
};

class DeleteFilter : public CompactionFilter {
 public:
  virtual bool Filter(int level, const Slice& key, const Slice& value,
                      std::string* new_value,
                      bool* value_changed) const override {
    cfilter_count++;
    return true;
  }

  virtual const char* Name() const override { return "DeleteFilter"; }
};

class ChangeFilter : public CompactionFilter {
 public:
  explicit ChangeFilter() {}

  virtual bool Filter(int level, const Slice& key, const Slice& value,
                      std::string* new_value,
                      bool* value_changed) const override {
    assert(new_value != nullptr);
    *new_value = NEW_VALUE;
    *value_changed = true;
    return false;
  }

  virtual const char* Name() const override { return "ChangeFilter"; }
};

class KeepFilterFactory : public CompactionFilterFactory {
 public:
  explicit KeepFilterFactory(bool check_context = false)
      : check_context_(check_context) {}

  virtual std::unique_ptr<CompactionFilter> CreateCompactionFilter(
      const CompactionFilterContext& context) override {
    if (check_context_) {
      ASSERT_EQ(expect_full_compaction_.load(), context.is_full_compaction);
      ASSERT_EQ(expect_manual_compaction_.load(), context.is_manual_compaction);
    }
    return std::unique_ptr<CompactionFilter>(new KeepFilter());
  }

  virtual const char* Name() const override { return "KeepFilterFactory"; }
  bool check_context_;
  std::atomic_bool expect_full_compaction_;
  std::atomic_bool expect_manual_compaction_;
};

class DeleteFilterFactory : public CompactionFilterFactory {
 public:
  virtual std::unique_ptr<CompactionFilter> CreateCompactionFilter(
      const CompactionFilterContext& context) override {
    if (context.is_manual_compaction) {
      return std::unique_ptr<CompactionFilter>(new DeleteFilter());
    } else {
      return std::unique_ptr<CompactionFilter>(nullptr);
    }
  }

  virtual const char* Name() const override { return "DeleteFilterFactory"; }
};

class ChangeFilterFactory : public CompactionFilterFactory {
 public:
  explicit ChangeFilterFactory() {}

  virtual std::unique_ptr<CompactionFilter> CreateCompactionFilter(
      const CompactionFilterContext& context) override {
    return std::unique_ptr<CompactionFilter>(new ChangeFilter());
  }

  virtual const char* Name() const override { return "ChangeFilterFactory"; }
};

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

void MinLevelHelper(DBTest* self, Options& options) {
  Random rnd(301);

  for (int num = 0; num < options.level0_file_num_compaction_trigger - 1;
       num++) {
    std::vector<std::string> values;
    // Write 120KB (12 values, each 10K)
    for (int i = 0; i < 12; i++) {
      values.push_back(RandomString(&rnd, 10000));
      ASSERT_OK(self->Put(Key(i), values[i]));
    }
    self->dbfull()->TEST_WaitForFlushMemTable();
    ASSERT_EQ(self->NumTableFilesAtLevel(0), num + 1);
  }

  // generate one more file in level-0, and should trigger level-0 compaction
  std::vector<std::string> values;
  for (int i = 0; i < 12; i++) {
    values.push_back(RandomString(&rnd, 10000));
    ASSERT_OK(self->Put(Key(i), values[i]));
  }
  self->dbfull()->TEST_WaitForCompact();

  ASSERT_EQ(self->NumTableFilesAtLevel(0), 0);
  ASSERT_EQ(self->NumTableFilesAtLevel(1), 1);
}

// returns false if the calling-Test should be skipped
bool MinLevelToCompress(CompressionType& type, Options& options, int wbits,
                        int lev, int strategy) {
  fprintf(stderr,
          "Test with compression options : window_bits = %d, level =  %d, "
          "strategy = %d}\n",
          wbits, lev, strategy);
  options.write_buffer_size = 100 << 10;  // 100KB
  options.num_levels = 3;
  options.max_mem_compaction_level = 0;
  options.level0_file_num_compaction_trigger = 3;
  options.create_if_missing = true;

  if (SnappyCompressionSupported(CompressionOptions(wbits, lev, strategy))) {
    type = kSnappyCompression;
    fprintf(stderr, "using snappy\n");
  } else if (ZlibCompressionSupported(
                 CompressionOptions(wbits, lev, strategy))) {
    type = kZlibCompression;
    fprintf(stderr, "using zlib\n");
  } else if (BZip2CompressionSupported(
                 CompressionOptions(wbits, lev, strategy))) {
    type = kBZip2Compression;
    fprintf(stderr, "using bzip2\n");
  } else if (LZ4CompressionSupported(
                 CompressionOptions(wbits, lev, strategy))) {
    type = kLZ4Compression;
    fprintf(stderr, "using lz4\n");
  } else if (LZ4HCCompressionSupported(
                 CompressionOptions(wbits, lev, strategy))) {
    type = kLZ4HCCompression;
    fprintf(stderr, "using lz4hc\n");
  } else {
    fprintf(stderr, "skipping test, compression disabled\n");
    return false;
  }
  options.compression_per_level.resize(options.num_levels);

  // do not compress L0
  for (int i = 0; i < 1; i++) {
    options.compression_per_level[i] = kNoCompression;
  }
  for (int i = 1; i < options.num_levels; i++) {
    options.compression_per_level[i] = type;
  }
  return true;
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, CompactionFilter) {
  Options options = CurrentOptions();
  options.max_open_files = -1;
  options.num_levels = 3;
  options.max_mem_compaction_level = 0;
  options.compaction_filter_factory = std::make_shared<KeepFilterFactory>();
  CreateAndReopenWithCF({"pikachu"}, &options);

  // Write 100K keys, these are written to a few files in L0.
  const std::string value(10, 'x');
  for (int i = 0; i < 100000; i++) {
    char key[100];
    snprintf(key, sizeof(key), "B%010d", i);
    Put(1, key, value);
  }
  ASSERT_OK(Flush(1));

  // Push all files to the highest level L2. Verify that
  // the compaction is each level invokes the filter for
  // all the keys in that level.
  cfilter_count = 0;
  dbfull()->TEST_CompactRange(0, nullptr, nullptr, handles_[1]);
  ASSERT_EQ(cfilter_count, 100000);
  cfilter_count = 0;
  dbfull()->TEST_CompactRange(1, nullptr, nullptr, handles_[1]);
  ASSERT_EQ(cfilter_count, 100000);

  ASSERT_EQ(NumTableFilesAtLevel(0, 1), 0);
  ASSERT_EQ(NumTableFilesAtLevel(1, 1), 0);
  ASSERT_NE(NumTableFilesAtLevel(2, 1), 0);
  cfilter_count = 0;

  // All the files are in the lowest level.
  // Verify that all but the 100001st record
  // has sequence number zero. The 100001st record
  // is at the tip of this snapshot and cannot
  // be zeroed out.
  // TODO: figure out sequence number squashtoo
  int count = 0;
  int total = 0;
  Iterator* iter = dbfull()->TEST_NewInternalIterator(handles_[1]);
  iter->SeekToFirst();
  ASSERT_OK(iter->status());
  while (iter->Valid()) {
    ParsedInternalKey ikey(Slice(), 0, kTypeValue);
    ikey.sequence = -1;
    ASSERT_EQ(ParseInternalKey(iter->key(), &ikey), true);
    total++;
    if (ikey.sequence != 0) {
      count++;
    }
    iter->Next();
  }
  ASSERT_EQ(total, 100000);
  ASSERT_EQ(count, 1);
  delete iter;

  // overwrite all the 100K keys once again.
  for (int i = 0; i < 100000; i++) {
    char key[100];
    snprintf(key, sizeof(key), "B%010d", i);
    ASSERT_OK(Put(1, key, value));
  }
  ASSERT_OK(Flush(1));

  // push all files to the highest level L2. This
  // means that all keys should pass at least once
  // via the compaction filter
  cfilter_count = 0;
  dbfull()->TEST_CompactRange(0, nullptr, nullptr, handles_[1]);
  ASSERT_EQ(cfilter_count, 100000);
  cfilter_count = 0;
  dbfull()->TEST_CompactRange(1, nullptr, nullptr, handles_[1]);
  ASSERT_EQ(cfilter_count, 100000);
  ASSERT_EQ(NumTableFilesAtLevel(0, 1), 0);
  ASSERT_EQ(NumTableFilesAtLevel(1, 1), 0);
  ASSERT_NE(NumTableFilesAtLevel(2, 1), 0);

  // create a new database with the compaction
  // filter in such a way that it deletes all keys
  options.compaction_filter_factory = std::make_shared<DeleteFilterFactory>();
  options.create_if_missing = true;
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  // write all the keys once again.
  for (int i = 0; i < 100000; i++) {
    char key[100];
    snprintf(key, sizeof(key), "B%010d", i);
    ASSERT_OK(Put(1, key, value));
  }
  ASSERT_OK(Flush(1));
  ASSERT_NE(NumTableFilesAtLevel(0, 1), 0);
  ASSERT_EQ(NumTableFilesAtLevel(1, 1), 0);
  ASSERT_EQ(NumTableFilesAtLevel(2, 1), 0);

  // Push all files to the highest level L2. This
  // triggers the compaction filter to delete all keys,
  // verify that at the end of the compaction process,
  // nothing is left.
  cfilter_count = 0;
  dbfull()->TEST_CompactRange(0, nullptr, nullptr, handles_[1]);
  ASSERT_EQ(cfilter_count, 100000);
  cfilter_count = 0;
  dbfull()->TEST_CompactRange(1, nullptr, nullptr, handles_[1]);
  ASSERT_EQ(cfilter_count, 0);
  ASSERT_EQ(NumTableFilesAtLevel(0, 1), 0);
  ASSERT_EQ(NumTableFilesAtLevel(1, 1), 0);

  // Scan the entire database to ensure that nothing is left
  iter = db_->NewIterator(ReadOptions(), handles_[1]);
  iter->SeekToFirst();
  count = 0;
  while (iter->Valid()) {
    count++;
    iter->Next();
  }
  ASSERT_EQ(count, 0);
  delete iter;

  // The sequence number of the remaining record
  // is not zeroed out even though it is at the
  // level Lmax because this record is at the tip
  // TODO: remove the following or design a different
  // test
  count = 0;
  iter = dbfull()->TEST_NewInternalIterator(handles_[1]);
  iter->SeekToFirst();
  ASSERT_OK(iter->status());
  while (iter->Valid()) {
    ParsedInternalKey ikey(Slice(), 0, kTypeValue);
    ASSERT_EQ(ParseInternalKey(iter->key(), &ikey), true);
    ASSERT_NE(ikey.sequence, (unsigned)0);
    count++;
    iter->Next();
  }
  ASSERT_EQ(count, 0);
  delete iter;
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

class KeepFilterV2 : public CompactionFilterV2 {
 public:
  virtual std::vector<bool> Filter(
      int level, const SliceVector& keys, const SliceVector& existing_values,
      std::vector<std::string>* new_values,
      std::vector<bool>* values_changed) const override {
    cfilter_count++;
    std::vector<bool> ret;
    new_values->clear();
    values_changed->clear();
    for (unsigned int i = 0; i < keys.size(); ++i) {
      values_changed->push_back(false);
      ret.push_back(false);
    }
    return ret;
  }

  virtual const char* Name() const override { return "KeepFilterV2"; }
};

class DeleteFilterV2 : public CompactionFilterV2 {
 public:
  virtual std::vector<bool> Filter(
      int level, const SliceVector& keys, const SliceVector& existing_values,
      std::vector<std::string>* new_values,
      std::vector<bool>* values_changed) const override {
    cfilter_count++;
    new_values->clear();
    values_changed->clear();
    std::vector<bool> ret;
    for (unsigned int i = 0; i < keys.size(); ++i) {
      values_changed->push_back(false);
      ret.push_back(true);
    }
    return ret;
  }

  virtual const char* Name() const override { return "DeleteFilterV2"; }
};

class ChangeFilterV2 : public CompactionFilterV2 {
 public:
  virtual std::vector<bool> Filter(
      int level, const SliceVector& keys, const SliceVector& existing_values,
      std::vector<std::string>* new_values,
      std::vector<bool>* values_changed) const override {
    std::vector<bool> ret;
    new_values->clear();
    values_changed->clear();
    for (unsigned int i = 0; i < keys.size(); ++i) {
      values_changed->push_back(true);
      new_values->push_back(NEW_VALUE);
      ret.push_back(false);
    }
    return ret;
  }

  virtual const char* Name() const override { return "ChangeFilterV2"; }
};

class KeepFilterFactoryV2 : public CompactionFilterFactoryV2 {
 public:
  explicit KeepFilterFactoryV2(const SliceTransform* prefix_extractor)
      : CompactionFilterFactoryV2(prefix_extractor) {}

  virtual std::unique_ptr<CompactionFilterV2> CreateCompactionFilterV2(
      const CompactionFilterContext& context) override {
    return std::unique_ptr<CompactionFilterV2>(new KeepFilterV2());
  }

  virtual const char* Name() const override { return "KeepFilterFactoryV2"; }
};

class DeleteFilterFactoryV2 : public CompactionFilterFactoryV2 {
 public:
  explicit DeleteFilterFactoryV2(const SliceTransform* prefix_extractor)
      : CompactionFilterFactoryV2(prefix_extractor) {}

  virtual std::unique_ptr<CompactionFilterV2> CreateCompactionFilterV2(
      const CompactionFilterContext& context) override {
    return std::unique_ptr<CompactionFilterV2>(new DeleteFilterV2());
  }

  virtual const char* Name() const override { return "DeleteFilterFactoryV2"; }
};

class ChangeFilterFactoryV2 : public CompactionFilterFactoryV2 {
 public:
  explicit ChangeFilterFactoryV2(const SliceTransform* prefix_extractor)
      : CompactionFilterFactoryV2(prefix_extractor) {}

  virtual std::unique_ptr<CompactionFilterV2> CreateCompactionFilterV2(
      const CompactionFilterContext& context) override {
    return std::unique_ptr<CompactionFilterV2>(new ChangeFilterV2());
  }

  virtual const char* Name() const override { return "ChangeFilterFactoryV2"; }
};

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

static bool Between(uint64_t val, uint64_t low, uint64_t high) {
  bool result = (val >= low) && (val <= high);
  if (!result) {
    fprintf(stderr, "Value %llu is not in range [%llu, %llu]\n",
            (unsigned long long)(val), (unsigned long long)(low),
            (unsigned long long)(high));
  }
  return result;
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, Snapshot) {
  do {
    CreateAndReopenWithCF({"pikachu"});
    Put(0, "foo", "0v1");
    Put(1, "foo", "1v1");
    const Snapshot* s1 = db_->GetSnapshot();
    Put(0, "foo", "0v2");
    Put(1, "foo", "1v2");
    const Snapshot* s2 = db_->GetSnapshot();
    Put(0, "foo", "0v3");
    Put(1, "foo", "1v3");
    const Snapshot* s3 = db_->GetSnapshot();

    Put(0, "foo", "0v4");
    Put(1, "foo", "1v4");
    ASSERT_EQ("0v1", Get(0, "foo", s1));
    ASSERT_EQ("1v1", Get(1, "foo", s1));
    ASSERT_EQ("0v2", Get(0, "foo", s2));
    ASSERT_EQ("1v2", Get(1, "foo", s2));
    ASSERT_EQ("0v3", Get(0, "foo", s3));
    ASSERT_EQ("1v3", Get(1, "foo", s3));
    ASSERT_EQ("0v4", Get(0, "foo"));
    ASSERT_EQ("1v4", Get(1, "foo"));

    db_->ReleaseSnapshot(s3);
    ASSERT_EQ("0v1", Get(0, "foo", s1));
    ASSERT_EQ("1v1", Get(1, "foo", s1));
    ASSERT_EQ("0v2", Get(0, "foo", s2));
    ASSERT_EQ("1v2", Get(1, "foo", s2));
    ASSERT_EQ("0v4", Get(0, "foo"));
    ASSERT_EQ("1v4", Get(1, "foo"));

    db_->ReleaseSnapshot(s1);
    ASSERT_EQ("0v2", Get(0, "foo", s2));
    ASSERT_EQ("1v2", Get(1, "foo", s2));
    ASSERT_EQ("0v4", Get(0, "foo"));
    ASSERT_EQ("1v4", Get(1, "foo"));

    db_->ReleaseSnapshot(s2);
    ASSERT_EQ("0v4", Get(0, "foo"));
    ASSERT_EQ("1v4", Get(1, "foo"));
  } while (ChangeOptions());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

std::vector<std::uint64_t> ListLogFiles(Env* env, const std::string& path) {
  std::vector<std::string> files;
  std::vector<uint64_t> log_files;
  env->GetChildren(path, &files);
  uint64_t number;
  FileType type;
  for (size_t i = 0; i < files.size(); ++i) {
    if (ParseFileName(files[i], &number, &type)) {
      if (type == kLogFile) {
        log_files.push_back(number);
      }
    }
  }
  return std::move(log_files);
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

uint64_t GetLogDirSize(std::string dir_path, SpecialEnv* env) {
  uint64_t dir_size = 0;
  std::vector<std::string> files;
  env->GetChildren(dir_path, &files);
  for (auto& f : files) {
    uint64_t number;
    FileType type;
    if (ParseFileName(f, &number, &type) && type == kLogFile) {
      std::string const file_path = dir_path + "/" + f;
      uint64_t file_size;
      env->GetFileSize(file_path, &file_size);
      dir_size += file_size;
    }
  }
  return dir_size;
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

SequenceNumber ReadRecords(std::unique_ptr<TransactionLogIterator>& iter,
                           int& count) {
  count = 0;
  SequenceNumber lastSequence = 0;
  BatchResult res;
  while (iter->Valid()) {
    res = iter->GetBatch();
    ASSERT_TRUE(res.sequence > lastSequence);
    ++count;
    lastSequence = res.sequence;
    ASSERT_OK(iter->status());
    iter->Next();
  }
  return res.sequence;
}

void ExpectRecords(const int expected_no_records,
                   std::unique_ptr<TransactionLogIterator>& iter) {
  int num_records;
  ReadRecords(iter, num_records);
  ASSERT_EQ(num_records, expected_no_records);
}

TEST(DBTest, TransactionLogIterator) {
  do {
    Options options = OptionsForLogIterTest();
    DestroyAndReopen(&options);
    CreateAndReopenWithCF({"pikachu"}, &options);
    Put(0, "key1", DummyString(1024));
    Put(1, "key2", DummyString(1024));
    Put(1, "key2", DummyString(1024));
    ASSERT_EQ(dbfull()->GetLatestSequenceNumber(), 3U);
    {
      auto iter = OpenTransactionLogIter(0);
      ExpectRecords(3, iter);
    }
    ReopenWithColumnFamilies({"default", "pikachu"}, &options);
    env_->SleepForMicroseconds(2 * 1000 * 1000);
    {
      Put(0, "key4", DummyString(1024));
      Put(1, "key5", DummyString(1024));
      Put(0, "key6", DummyString(1024));
    }
    {
      auto iter = OpenTransactionLogIter(0);
      ExpectRecords(6, iter);
    }
  } while (ChangeCompactOptions());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

// TODO(kailiu) disable the in non-linux platforms to temporarily solve
// // the unit test failure.
#ifdef OS_LINUX
TEST(DBTest, TransactionLogIteratorStallAtLastRecord) {
  do {
    Options options = OptionsForLogIterTest();
    DestroyAndReopen(&options);
    Put("key1", DummyString(1024));
    auto iter = OpenTransactionLogIter(0);
    ASSERT_OK(iter->status());
    ASSERT_TRUE(iter->Valid());
    iter->Next();
    ASSERT_TRUE(!iter->Valid());
    ASSERT_OK(iter->status());
    Put("key2", DummyString(1024));
    iter->Next();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(iter->Valid());
  } while (ChangeCompactOptions());
}
#endif

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

// Multi-threaded test:
namespace {

static const int kColumnFamilies = 10;
static const int kNumThreads = 10;
static const int kTestSeconds = 10;
static const int kNumKeys = 1000;

struct MTState {
  DBTest* test;
  port::AtomicPointer stop;
  port::AtomicPointer thread_done[kNumThreads];
  port::AtomicPointer thread_done[kNumThreads];
};

struct MTThread {
  MTState* state;
  int id;
};

static void MTThreadBody(void* arg) {
  MTThread* t = reinterpret_cast<MTThread*>(arg);
  int id = t->id;
  DB* db = t->state->test->db_;
  uintptr_t counter = 0;
  fprintf(stderr, "... starting thread %d\n", id);
  Random rnd(1000 + id);
  char valbuf[1500];
  while (t->state->stop.Acquire_Load() == nullptr) {
    t->state->counter[id].Release_Store(reinterpret_cast<void*>(counter));

    int key = rnd.Uniform(kNumKeys);
    char keybuf[20];
    snprintf(keybuf, sizeof(keybuf), "%016d", key);

    if (rnd.OneIn(2)) {
      // Write values of the form <key, my id, counter, cf, unique_id>.
      // into each of the CFs
      // We add some padding for force compactions.
      int unique_id = rnd.Uniform(1000000);
      WriteBatch batch;
      for (int cf = 0; cf < kColumnFamilies; ++cf) {
        snprintf(valbuf, sizeof(valbuf), "%d.%d.%d.%d.%-1000d", key, id,
                 static_cast<int>(counter), cf, unique_id);
        batch.Put(t->state->test->handles_[cf], Slice(keybuf), Slice(valbuf));
      }
      ASSERT_OK(db->Write(WriteOptions(), &batch));
    } else {
      // Read a value and verify that it matches the pattern written above
      // and that writes to all column families were atomic (unique_id is the
      // same)
      std::vector<Slice> keys(kColumnFamilies, Slice(keybuf));
      std::vector<std::string> values;
      std::vector<Status> statuses =
          db->MultiGet(ReadOptions(), t->state->test->handles_, keys, &values);
      Status s = statuses[0];
      // all statuses have to be the same
      for (size_t i = 1; i < statuses.size(); ++i) {
        // they are either both ok or both not-found
        ASSERT_TRUE((s.ok() && statuses[i].ok()) ||
                    (s.IsNotFound() && statuses[i].IsNotFound()));
      }
      if (s.IsNotFound()) {
        // Key has not yet been written
      } else {
        // Check that the writer thread counter is >= the counter in the value
        ASSERT_OK(s);
        int unique_id = -1;
        for (int i = 0; i < kColumnFamilies; ++i) {
          int k, w, c, cf, u;
          ASSERT_EQ(5, sscanf(values[i].c_str(), "%d.%d.%d.%d.%d", &k, &w, &c,
                              &cf, &u))
              << values[i];
          ASSERT_EQ(k, key);
          ASSERT_GE(w, 0);
          ASSERT_LT(w, kNumThreads);
          ASSERT_LE((unsigned int)c, reinterpret_cast<uintptr_t>(
                                         t->state->counter[w].Acquire_Load()));
          ASSERT_EQ(cf, i);
          if (i == 0) {
            unique_id = u;
          } else {
            // this checks that updates across column families happened
            // atomically -- all unique ids are the same
            ASSERT_EQ(u, unique_id);
          }
        }
      }
    }
    counter++;
  }
  t->state->thread_done[id].Release_Store(t);
  fprintf(stderr, "... stopping thread %d after %d ops\n", id, int(counter));
}

}  // namespace

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

// Group commit test:
namespace {

static const int kGCNumThreads = 4;
static const int kGCNumKeys = 1000;

struct GCThread {
  DB* db;
  int id;
  std::atomic<bool> done;
};

static void GCThreadBody(void* arg) {
  GCThread* t = reinterpret_cast<GCThread*>(arg);
  int id = t->id;
  DB* db = t->db;
  WriteOptions wo;

  for (int i = 0; i < kGCNumKeys; ++i) {
    std::string kv(std::to_string(i + id * kGCNumKeys));
    ASSERT_OK(db->Put(wo, kv, kv));
  }
  t->done = true;
}

}  // namespace

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

namespace {
typedef std::map<std::string, std::string> KVMap;
}

class ModelDB : public DB {
 public:
  class ModelSnapshot : public Snapshot {
   public:
    KVMap map_;
  };

  explicit ModelDB(const Options& options) : options_(options) {}
  using DB::Put;
  virtual Status Put(const WriteOptions& o, ColumnFamilyHandle* cf,
                     const Slice& k, const Slice& v) {
    WriteBatch batch;
    batch.Put(cf, k, v);
    return Write(o, &batch);
  }
  using DB::Merge;
  virtual Status Merge(const WriteOptions& o, ColumnFamilyHandle* cf,
                       const Slice& k, const Slice& v) {
    WriteBatch batch;
    batch.Merge(cf, k, v);
    return Write(o, &batch);
  }
  using DB::Delete;
  virtual Status Delete(const WriteOptions& o, ColumnFamilyHandle* cf,
                        const Slice& key) {
    WriteBatch batch;
    batch.Delete(cf, key);
    return Write(o, &batch);
  }
  using DB::Get;
  virtual Status Get(const ReadOptions& options, ColumnFamilyHandle* cf,
                     const Slice& key, std::string* value) {
    return Status::NotSupported(key);
  }

  virtual bool KeyMayExist(const ReadOptions& options,
                           ColumnFamilyHandle* column_family, const Slice& key,
                           std::string* value, bool* value_found = nullptr) {
    if (value_found != nullptr) {
      *value_found = false;
    }
    return true;  // Not Supported directly
  }
  virtual const Snapshot* GetSnapshot() {
    ModelSnapshot* snapshot = new ModelSnapshot;
    snapshot->map_ = map_;
    return snapshot;
  }

  virtual void ReleaseSnapshot(const Snapshot* snapshot) {
    delete reinterpret_cast<const ModelSnapshot*>(snapshot);
  }
  virtual Status Write(const WriteOptions& options, WriteBatch* batch) {
    class Handler : public WriteBatch::Handler {
     public:
      KVMap* map_;
      virtual void Put(const Slice& key, const Slice& value) {
        (*map_)[key.ToString()] = value.ToString();
      }
      virtual void Merge(const Slice& key, const Slice& value) {
        // ignore merge for now
        //(*map_)[key.ToString()] = value.ToString();
      }
      virtual void Delete(const Slice& key) { map_->erase(key.ToString()); }
    };
    Handler handler;
    handler.map_ = &map_;
    return batch->Iterate(&handler);
  }

  using DB::GetProperty;
  virtual bool GetProperty(ColumnFamilyHandle* column_family,
                           const Slice& property, std::string* value) {
    return false;
  }
  using DB::GetApproximateSizes;
  virtual void GetApproximateSizes(ColumnFamilyHandle* column_family,
                                   const Range* range, int n, uint64_t* sizes) {
    for (int i = 0; i < n; i++) {
      sizes[i] = 0;
    }
  }
  using DB::CompactRange;
  virtual Status CompactRange(ColumnFamilyHandle* column_family,
                              const Slice* start, const Slice* end,
                              bool reduce_level, int target_level) {
    return Status::NotSupported("Not supported operation.");
  }

  virtual const std::string& GetName() const { return name_; }

  virtual Env* GetEnv() const { return nullptr; }

  using DB::GetOptions;
  virtual const Options& GetOptions(ColumnFamilyHandle* column_family) const {
    return options_;
  }

  using DB::Flush;
  virtual Status Flush(const rocksdb::FlushOptions& options,
                       ColumnFamilyHandle* column_family) {
    Status ret;
    return ret;
  }

  virtual Status DisableFileDeletions() { return Status::OK(); }
  virtual Status EnableFileDeletions(bool force) { return Status::OK(); }
  virtual Status GetLiveFiles(std::vector<std::string>&, uint64_t* size,
                              bool flush_memtable = true) {
    return Status::OK();
  }

  virtual Status GetSortedWalFiles(VectorLogPtr& files) { return Status::OK(); }

  virtual Status DeleteFile(std::string name) { return Status::OK(); }

  virtual Status GetDbIdentity(std::string& identity) { return Status::OK(); }

  virtual SequenceNumber GetLatestSequenceNumber() const { return 0; }
  virtual Status GetUpdatesSince(
      rocksdb::SequenceNumber, unique_ptr<rocksdb::TransactionLogIterator>*,
      const TransactionLogIterator::ReadOptions& read_options =
          TransactionLogIterator::ReadOptions()) {
    return Status::NotSupported("Not supported in Model DB");
  }

  virtual ColumnFamilyHandle* DefaultColumnFamily() const { return nullptr; }

 private:
  class ModelIter : public Iterator {
   public:
    ModelIter(const KVMap* map, bool owned)
        : map_(map), owned_(owned), iter_(map_->end()) {}
    ~ModelIter() {
      if (owned_) delete map_;
    }
    virtual bool Valid() const { return iter_ != map_->end(); }
    virtual void SeekToFirst() { iter_ = map_->begin(); }
    virtual void SeekToLast() {
      if (map_->empty()) {
        iter_ = map_->end();
      } else {
        iter_ = map_->find(map_->rbegin()->first);
      }
    }
    virtual void Seek(const Slice& k) {
      iter_ = map_->lower_bound(k.ToString());
    }
    virtual void Next() { ++iter_; }
    virtual void Prev() { --iter_; }
    virtual Slice key() const { return iter_->first; }
    virtual Slice value() const { return iter_->second; }
    virtual Status status() const { return Status::OK(); }

   private:
    const KVMap* const map_;
    const bool owned_;  // Do we own map_
    KVMap::const_iterator iter_;
  };
  const Options options_;
  KVMap map_;
  std::string name_ = "";

 public:
  using DB::MultiGet;
  virtual std::vector<Status> MultiGet(
      const ReadOptions& options,
      const std::vector<ColumnFamilyHandle*>& column_family,
      const std::vector<Slice>& keys, std::vector<std::string>* values) {
    std::vector<Status> s(keys.size(),
                          Status::NotSupported("Not implemented."));
    return s;
  }

  using DB::GetPropertiesOfAllTables;
  virtual Status GetPropertiesOfAllTables(ColumnFamilyHandle* column_family,
                                          TablePropertiesCollection* props) {
    return Status();
  }

  using DB::KeyMayExist;
  using DB::NewIterator;
  virtual Iterator* NewIterator(const ReadOptions& options,
                                ColumnFamilyHandle* column_family) {
    if (options.snapshot == nullptr) {
      KVMap* saved = new KVMap;
      *saved = map_;
      return new ModelIter(saved, true);
    } else {
      const KVMap* snapshot_state =
          &(reinterpret_cast<const ModelSnapshot*>(options.snapshot)->map_);
      return new ModelIter(snapshot_state, false);
    }
  }
  virtual Status NewIterators(
      const ReadOptions& options,
      const std::vector<ColumnFamilyHandle*>& column_family,
      std::vector<Iterator*>* iterators) {
    return Status::NotSupported("Not supported yet");
  }
  using DB::NumberLevels;
  virtual int NumberLevels(ColumnFamilyHandle* column_family) { return 1; }

  using DB::MaxMemCompactionLevel;
  virtual int MaxMemCompactionLevel(ColumnFamilyHandle* column_family) {
    return 1;
  }

  using DB::Level0StopWriteTrigger;
  virtual int Level0StopWriteTrigger(ColumnFamilyHandle* column_family) {
    return -1;
  }
};

static std::string RandomKey(Random* rnd, int minimum = 0) {
  int len;
  do {
    len = (rnd->OneIn(3)
               ? 1  // Short sometimes to encourage collisions
               : (rnd->OneIn(100) ? rnd->Skewed(10) : rnd->Uniform(10)));
  } while (len < minimum);
  return test::RandomKey(rnd, len);
}

static bool CompareIterators(int step, DB* model, DB* db,
                             const Snapshot* model_snap,
                             const Snapshot* db_snap) {
  ReadOptions options;
  options.snapshot = model_snap;
  Iterator* miter = model->NewIterator(options);
  options.snapshot = db_snap;
  Iterator* dbiter = db->NewIterator(options);
  bool ok = true;
  int count = 0;
  for (miter->SeekToFirst(), dbiter->SeekToFirst();
       ok && miter->Valid() && dbiter->Valid(); miter->Next(), dbiter->Next()) {
    count++;
    if (miter->key().compare(dbiter->key()) != 0) {
      fprintf(stderr, "step %d: Key mismatch: '%s' vs. '%s'\n", step,
              EscapeString(miter->key()).c_str(),
              EscapeString(dbiter->key()).c_str());
      ok = false;
      break;
    }

    if (miter->value().compare(dbiter->value()) != 0) {
      fprintf(stderr, "step %d: Value mismatch for key '%s': '%s' vs. '%s'\n",
              step, EscapeString(miter->key()).c_str(),
              EscapeString(miter->value()).c_str(),
              EscapeString(miter->value()).c_str());
      ok = false;
    }
  }

  if (ok) {
    if (miter->Valid() != dbiter->Valid()) {
      fprintf(stderr, "step %d: Mismatch at end of iterators: %d vs. %d\n",
              step, miter->Valid(), dbiter->Valid());
      ok = false;
    }
  }
  delete miter;
  delete dbiter;
  return ok;
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

void PrefixScanInit(DBTest* dbtest) {
  char buf[100];
  std::string keystr;
  const int small_range_sstfiles = 5;
  const int big_range_sstfiles = 5;

  // Generate 11 sst files with the following prefix ranges.
  // GROUP 0: [0,10]                              (level 1)
  // GROUP 1: [1,2], [2,3], [3,4], [4,5], [5, 6]  (level 0)
  // GROUP 2: [0,6], [0,7], [0,8], [0,9], [0,10]  (level 0)
  //
  // A seek with the previous API would do 11 random I/Os (to all the
  // files).  With the new API and a prefix filter enabled, we should
  // only do 2 random I/O, to the 2 files containing the key.

  // GROUP 0
  snprintf(buf, sizeof(buf), "%02d______:start", 0);
  keystr = std::string(buf);
  ASSERT_OK(dbtest->Put(keystr, keystr));
  snprintf(buf, sizeof(buf), "%02d______:end", 10);
  keystr = std::string(buf);
  ASSERT_OK(dbtest->Put(keystr, keystr));
  dbtest->Flush();
  dbtest->dbfull()->CompactRange(nullptr, nullptr);  // move to level 1

  // GROUP 1
  for (int i = 1; i <= small_range_sstfiles; i++) {
    snprintf(buf, sizeof(buf), "%02d______:start", i);
    keystr = std::string(buf);
    ASSERT_OK(dbtest->Put(keystr, keystr));
    snprintf(buf, sizeof(buf), "%02d______:end", i + 1);
    keystr = std::string(buf);
    ASSERT_OK(dbtest->Put(keystr, keystr));
    dbtest->Flush();
  }

  // GROUP 2
  for (int i = 1; i <= big_range_sstfiles; i++) {
    std::string keystr;
    snprintf(buf, sizeof(buf), "%02d______:start", 0);
    keystr = std::string(buf);
    ASSERT_OK(dbtest->Put(keystr, keystr));
    snprintf(buf, sizeof(buf), "%02d______:end", small_range_sstfiles + i + 1);
    keystr = std::string(buf);
    ASSERT_OK(dbtest->Put(keystr, keystr));
    dbtest->Flush();
  }
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

std::string MakeKey(unsigned int num) {
  char buf[30];
  snprintf(buf, sizeof(buf), "%016u", num);
  return std::string(buf);
}

void BM_LogAndApply(int iters, int num_base_files) {
  std::string dbname = test::TmpDir() + "/rocksdb_test_benchmark";
  ASSERT_OK(DestroyDB(dbname, Options()));

  DB* db = nullptr;
  Options opts;
  opts.create_if_missing = true;
  Status s = DB::Open(opts, dbname, &db);
  ASSERT_OK(s);
  ASSERT_TRUE(db != nullptr);

  delete db;
  db = nullptr;

  Env* env = Env::Default();

  port::Mutex mu;
  MutexLock l(&mu);

  Options options;
  EnvOptions sopt;
  VersionSet vset(dbname, &options, sopt, nullptr);
  std::vector<ColumnFamilyDescriptor> dummy;
  dummy.push_back(ColumnFamilyDescriptor());
  ASSERT_OK(vset.Recover(dummy));
  auto default_cfd = vset.GetColumnFamilySet()->GetDefault();
  VersionEdit vbase;
  uint64_t fnum = 1;
  for (int i = 0; i < num_base_files; i++) {
    InternalKey start(MakeKey(2 * fnum), 1, kTypeValue);
    InternalKey limit(MakeKey(2 * fnum + 1), 1, kTypeDeletion);
    vbase.AddFile(2, fnum++, 1 /* file size */, start, limit, 1, 1);
  }
  ASSERT_OK(vset.LogAndApply(default_cfd, &vbase, &mu));

  uint64_t start_micros = env->NowMicros();

  for (int i = 0; i < iters; i++) {
    VersionEdit vedit;
    vedit.DeleteFile(2, fnum);
    InternalKey start(MakeKey(2 * fnum), 1, kTypeValue);
    InternalKey limit(MakeKey(2 * fnum + 1), 1, kTypeDeletion);
    vedit.AddFile(2, fnum++, 1 /* file size */, start, limit, 1, 1);
    vset.LogAndApply(default_cfd, &vedit, &mu);
  }
  uint64_t stop_micros = env->NowMicros();
  unsigned int us = stop_micros - start_micros;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", num_base_files);
  fprintf(stderr,
          "BM_LogAndApply/%-6s   %8d iters : %9u us (%7.0f us / iter)\n", buf,
          iters, us, ((float)us) / iters);
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

TEST(DBTest, TailingIteratorPrefixSeek) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory());
  DestroyAndReopen(&options);
  CreateAndReopenWithCF({"pikachu"}, &options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));

  ASSERT_OK(Flush(1));

  ASSERT_OK(Put(1, "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

}  // namespace rocksdb

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--benchmark") {
    rocksdb::BM_LogAndApply(1000, 1);
    rocksdb::BM_LogAndApply(1000, 100);
    rocksdb::BM_LogAndApply(1000, 10000);
    rocksdb::BM_LogAndApply(100, 100000);
    return 0;
  }

  return rocksdb::test::RunAllTests();
}