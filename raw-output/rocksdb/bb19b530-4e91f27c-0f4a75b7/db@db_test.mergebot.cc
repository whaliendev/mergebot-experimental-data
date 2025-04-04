//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <algorithm>
#include <set>
#include <unistd.h>
#include "rocksdb/db.h"
#include "rocksdb/filter_policy.h"
#include "db/db_impl.h"
#include "db/filename.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include "db/db_statistics.h"
#include "rocksdb/cache.h"
#include "rocksdb/compaction_filter.h"
#include "rocksdb/env.h"
#include "rocksdb/table.h"
#include "rocksdb/perf_context.h"
#include "rocksdb/plain_table_factory.h"
#include "util/hash.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/testharness.h"
#include "util/testutil.h"
#include "util/statistics.h"
#include "util/hash_linklist_rep.h"
#include "utilities/merge_operators.h"

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
  AtomicCounter() : count_(0) { }
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

} 

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
          : env_(env),
            base_(std::move(base)) {
      }
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
          : env_(env), base_(std::move(b)) { }
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
          : env_(env), base_(std::move(b)) { }
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
          : target_(std::move(target)), counter_(counter) {
      }
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
  
  static std::unique_ptr<const SliceTransform> prefix_1_transform;
  static std::unique_ptr<const SliceTransform> noop_transform;
  
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
  
  DBTest() : option_config_(kDefault),
             env_(new SpecialEnv(Env::Default())) {
    filter_policy_ = NewBloomFilterPolicy(10);
    dbname_ = test::TmpDir() + "/db_test";
    ASSERT_OK(DestroyDB(dbname_, Options()));
    db_ = nullptr;
    Reopen();
  }
  
  ~DBTest() {
    delete db_;
    ASSERT_OK(DestroyDB(dbname_, Options()));
    delete env_;
    delete filter_policy_;
  }{
    delete db_;
    ASSERT_OK(DestroyDB(dbname_, Options()));
    delete env_;
    delete filter_policy_;
  }
  
  // Switch to a fresh database with the next option configuration to
  // test.  Return false if there are no more configurations to test.
  bool ChangeOptions(int skip_mask = kNoSkip) {
    // skip some options
    for(option_config_++; option_config_ < kEnd; option_config_++) {
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
      if ((skip_mask & kSkipPlainTable)
          && (option_config_ == kPlainTableAllBytesPrefix
              || option_config_ == kPlainTableFirstBytePrefix)) {
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
    switch (option_config_) {
      case kHashSkipList:
        options.memtable_factory.reset(
            NewHashSkipListRepFactory(NewFixedPrefixTransform(1)));
        break;
      case kPlainTableFirstBytePrefix:
        options.table_factory.reset(new PlainTableFactory());
        options.prefix_extractor = prefix_1_transform.get();
        options.allow_mmap_reads = true;
        options.max_sequential_skip_in_iterations = 999999;
        break;
      case kPlainTableAllBytesPrefix:
        options.table_factory.reset(new PlainTableFactory());
        options.prefix_extractor = noop_transform.get();
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
        options.max_manifest_file_size = 50; // 50 bytes
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
      options.memtable_factory.reset(
          NewHashLinkListRepFactory(NewFixedPrefixTransform(1), 4));
        break;
      case kUniversalCompaction:
        options.compaction_style = kCompactionStyleUniversal;
        break;
      case kCompressedBlockCache:
        options.block_cache_compressed = NewLRUCache(8*1024*1024);
        break;
      case kInfiniteMaxOpenFiles:
        options.max_open_files = -1;
        break;
      default:
        break;
    }
    return options;
  }
  
  DBImpl* dbfull() {
    return reinterpret_cast<DBImpl*>(db_);
  }
  
  void Reopen(Options* options = nullptr) {
    ASSERT_OK(TryReopen(options));
  }
  
  void Close() {
    delete db_;
    db_ = nullptr;
  }
  
  void DestroyAndReopen(Options* options = nullptr) {
    //Destroy using last options
    Destroy(&last_options_);
    ASSERT_OK(TryReopen(options));
  }
  
  void Destroy(Options* options) {
    delete db_;
    db_ = nullptr;
    ASSERT_OK(DestroyDB(dbname_, *options));
  }
  
  Status PureReopen(Options* options, DB** db) {
    return DB::Open(*options, dbname_, db);
  }
  
  Status TryReopen(Options* options = nullptr) {
    delete db_;
    db_ = nullptr;
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
  
  Status Put(const Slice& k, const Slice& v, WriteOptions wo = WriteOptions()) {
    if (kMergePut == option_config_ ) {
      return db_->Merge(wo, k, v);
    } else {
      return db_->Put(wo, k, v);
    }
  }
  
  Status Delete(const std::string& k) {
    return db_->Delete(WriteOptions(), k);
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
  
  // Return a string that contains all key,value pairs in order,
  // formatted like "(k1->v1)(k2->v2)".
  std::string Contents() {
    std::vector<std::string> forward;
    std::string result;
    Iterator* iter = db_->NewIterator(ReadOptions());
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
  
  std::string AllEntriesFor(const Slice& user_key) {
    Iterator* iter = dbfull()->TEST_NewInternalIterator();
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
  
  int NumTableFilesAtLevel(int level) {
    std::string property;
    ASSERT_TRUE(
        db_->GetProperty("rocksdb.num-files-at-level" + NumberToString(level),
                         &property));
    return atoi(property.c_str());
  }
  
  int TotalTableFiles() {
    int result = 0;
    for (int level = 0; level < db_->NumberLevels(); level++) {
      result += NumTableFilesAtLevel(level);
    }
    return result;
  }
  
  // Return spread of files per level
  std::string FilesPerLevel() {
    std::string result;
    int last_non_zero_offset = 0;
    for (int level = 0; level < db_->NumberLevels(); level++) {
      int f = NumTableFilesAtLevel(level);
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
    std::vector<std::string> files;
    uint64_t manifest_file_size;
    db_->GetLiveFiles(files, &manifest_file_size);
    return files.size();
  }
  
  uint64_t Size(const Slice& start, const Slice& limit) {
    Range r(start, limit);
    uint64_t size;
    db_->GetApproximateSizes(&r, 1, &size);
    return size;
  }
  
  void Compact(const Slice& start, const Slice& limit) {
    db_->CompactRange(&start, &limit);
  }
  
  // Do n memtable compactions, each of which produces an sstable
  // covering the range [small,large].
  void MakeTables(int n, const std::string& small, const std::string& large) {
    for (int i = 0; i < n; i++) {
      Put(small, "begin");
      Put(large, "end");
      dbfull()->TEST_FlushMemTable();
    }
  }
  
  // Prevent pushing of new sstables into deeper levels by adding
  // tables that cover a specified range to all levels.
  void FillLevels(const std::string& smallest, const std::string& largest) {
    MakeTables(db_->NumberLevels(), smallest, largest);
  }
  
  void DumpFileCounts(const char* label) {
    fprintf(stderr, "---\n%s:\n", label);
    fprintf(stderr, "maxoverlap: %lld\n",
            static_cast<long long>(
                dbfull()->TEST_MaxNextLevelOverlappingBytes()));
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
  
  void VerifyIterLast(std::string expected_key) {
    Iterator* iter = db_->NewIterator(ReadOptions());
    iter->SeekToLast();
    ASSERT_EQ(IterStatus(iter), expected_key);
    delete iter;
  }
  
  // Used to test InplaceUpdate
  // If previous value is nullptr or delta is > than previous value,
  //   sets newValue with delta
  // If previous value is not empty,
  //   updates previous value with 'b' string of previous value size - 1.
  static UpdateStatusupdateInPlaceSmallerSize(char* prevValue, uint32_t* prevSize, Slice delta, std::string* newValue) {
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
  
  static UpdateStatusupdateInPlaceSmallerVarintSize(char* prevValue, uint32_t* prevSize, Slice delta, std::string* newValue) {
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
  
  static UpdateStatusupdateInPlaceLargerSize(char* prevValue, uint32_t* prevSize, Slice delta, std::string* newValue) {
    *newValue = std::string(delta.size(), 'c');
    return UpdateStatus::UPDATED;
  }
  
  static UpdateStatusupdateInPlaceNoAction(char* prevValue, uint32_t* prevSize, Slice delta, std::string* newValue) {
    return UpdateStatus::UPDATE_FAILED;
  }
  
  // Utility method to test InplaceUpdate
  void validateNumberOfEntries(int numValues) {
      Iterator* iter = dbfull()->TEST_NewInternalIterator();
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
  void CopyFile(const std::string& source, const std::string& destination, uint64_t size = 0) {
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
  
};

std::unique_ptr<const SliceTransform> DBTest::noop_transform(
    NewNoopTransform());

static std::string Key(int i) {
  char buf[100];
  snprintf(buf, sizeof(buf), "key%06d", i);
  return std::string(buf);
}

static long TestGetTickerCount(const Options& options, Tickers ticker_type) {
  return options.statistics->getTickerCount(ticker_type);
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

namespace {
typedef std::map<std::string, std::string> KVMap;
} 

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

void MinLevelHelper(DBTest* self, Options& options) {
  Random rnd(301);

  for (int num = 0;
    num < options.level0_file_num_compaction_trigger - 1;
    num++)
  {
    std::vector<std::string> values;
    // Write 120KB (12 values, each 10K)
    for (int i = 0; i < 12; i++) {
      values.push_back(RandomString(&rnd, 10000));
      ASSERT_OK(self->Put(Key(i), values[i]));
    }
    self->dbfull()->TEST_WaitForFlushMemTable();
    ASSERT_EQ(self->NumTableFilesAtLevel(0), num + 1);
  }

  //generate one more file in level-0, and should trigger level-0 compaction
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
  fprintf(stderr, "Test with compression options : window_bits = %d, level =  %d, strategy = %d}\n", wbits, lev, strategy);
  options.write_buffer_size = 100<<10; //100KB
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

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

// This is a static filter used for filtering
// kvs during the compaction process.
static int cfilter_count;
static std::string NEW_VALUE = "NewValue";

class KeepFilter : public CompactionFilter {
public:
  virtual bool Filter(int level, const Slice& key,
                      const Slice& value, std::string* new_value,
                      bool* value_changed) const override {
    cfilter_count++;
    return false;
  }
  
  virtual const char* Name() const override {
    return "KeepFilter";
  }
  
};

class DeleteFilter : public CompactionFilter {
public:
  virtual bool Filter(int level, const Slice& key,
                      const Slice& value, std::string* new_value,
                      bool* value_changed) const override {
    cfilter_count++;
    return true;
  }
  
  virtual const char* Name() const override {
    return "DeleteFilter";
  }
};

class ChangeFilter : public CompactionFilter {
public:
  explicitChangeFilter(){}
  
  virtual bool Filter(int level, const Slice& key,
                      const Slice& value, std::string* new_value,
                      bool* value_changed) const override {
    assert(new_value != nullptr);
    *new_value = NEW_VALUE;
    *value_changed = true;
    return false;
  }
  
  virtual const char* Name() const override {
    return "ChangeFilter";
  }
};

class KeepFilterFactory : public CompactionFilterFactory {
public:
    virtual std::unique_ptr<CompactionFilter>
    CreateCompactionFilter(const CompactionFilter::Context& context) override {
      return std::unique_ptr<CompactionFilter>(new KeepFilter());
    }
    
    virtual const char* Name() const override {
      return "KeepFilterFactory";
    }
};

class DeleteFilterFactory : public CompactionFilterFactory {
public:
    virtual std::unique_ptr<CompactionFilter>
    CreateCompactionFilter(const CompactionFilter::Context& context) override {
      return std::unique_ptr<CompactionFilter>(new DeleteFilter());
    }
    
    virtual const char* Name() const override {
      return "DeleteFilterFactory";
    }
};

class ChangeFilterFactory : public CompactionFilterFactory {
public:
    explicitChangeFilterFactory(){}
    
    virtual std::unique_ptr<CompactionFilter>
    CreateCompactionFilter(const CompactionFilter::Context& context) override {
      return std::unique_ptr<CompactionFilter>(new ChangeFilter());
    }
    
    virtual const char* Name() const override {
      return "ChangeFilterFactory";
    }
    
};

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

static bool Between(uint64_t val, uint64_t low, uint64_t high) {
  bool result = (val >= low) && (val <= high);
  if (!result) {
    fprintf(stderr, "Value %llu is not in range [%llu, %llu]\n",
            (unsigned long long)(val),
            (unsigned long long)(low),
            (unsigned long long)(high));
  }
  return result;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
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

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
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

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

SequenceNumber ReadRecords(
    std::unique_ptr<TransactionLogIterator>& iter,
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

void ExpectRecords(
    const int expected_no_records,
    std::unique_ptr<TransactionLogIterator>& iter) {
  int num_records;
  ReadRecords(iter, num_records);
  ASSERT_EQ(num_records, expected_no_records);
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
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

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

// Multi-threaded test:
namespace {

static const int kNumThreads = 4;
static const int kTestSeconds = 10;
static const int kNumKeys = 1000;

struct MTState {
  DBTest* test;
  port::AtomicPointer stop;
  port::AtomicPointer counter[kNumThreads];
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
  std::string value;
  char valbuf[1500];
  while (t->state->stop.Acquire_Load() == nullptr) {
    t->state->counter[id].Release_Store(reinterpret_cast<void*>(counter));

    int key = rnd.Uniform(kNumKeys);
    char keybuf[20];
    snprintf(keybuf, sizeof(keybuf), "%016d", key);

    if (rnd.OneIn(2)) {
      // Write values of the form <key, my id, counter>.
      // We add some padding for force compactions.
      snprintf(valbuf, sizeof(valbuf), "%d.%d.%-1000d",
               key, id, static_cast<int>(counter));
      ASSERT_OK(t->state->test->Put(Slice(keybuf), Slice(valbuf)));
    } else {
      // Read a value and verify that it matches the pattern written above.
      Status s = db->Get(ReadOptions(), Slice(keybuf), &value);
      if (s.IsNotFound()) {
        // Key has not yet been written
      } else {
        // Check that the writer thread counter is >= the counter in the value
        ASSERT_OK(s);
        int k, w, c;
        ASSERT_EQ(3, sscanf(value.c_str(), "%d.%d.%d", &k, &w, &c)) << value;
        ASSERT_EQ(k, key);
        ASSERT_GE(w, 0);
        ASSERT_LT(w, kNumThreads);
        ASSERT_LE((unsigned int)c, reinterpret_cast<uintptr_t>(
            t->state->counter[w].Acquire_Load()));
      }
    }
    counter++;
  }
  t->state->thread_done[id].Release_Store(t);
  fprintf(stderr, "... stopping thread %d after %d ops\n", id, int(counter));
}

} // namespace

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
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

} // namespace

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

namespace {
typedef std::map<std::string, std::string> KVMap;
} 

class ModelDB: public DB {
public:
  class ModelSnapshot : public Snapshot {
   public:
    KVMap map_;
  };
  
  explicit ModelDB(const Options& options): options_(options) { }
  virtual Status Put(const WriteOptions& o, const Slice& k, const Slice& v) {
    return DB::Put(o, k, v);
  }
  virtual Status Merge(const WriteOptions& o, const Slice& k, const Slice& v) {
    return DB::Merge(o, k, v);
  }
  virtual Status Delete(const WriteOptions& o, const Slice& key) {
    return DB::Delete(o, key);
  }
  virtual Status Get(const ReadOptions& options,
                     const Slice& key, std::string* value) {
    return Status::NotSupported(key);
  }
  
  virtual std::vector<Status> MultiGet(const ReadOptions& options,
                                       const std::vector<Slice>& keys,
                                       std::vector<std::string>* values) {
    std::vector<Status> s(keys.size(),
                          Status::NotSupported("Not implemented."));
    return s;
  }
  virtual bool KeyMayExist(const ReadOptions& options,
                           const Slice& key,
                           std::string* value,
                           bool* value_found = nullptr) {
    if (value_found != nullptr) {
      *value_found = false;
    }
    return true; // Not Supported directly
  }
  virtual Iterator* NewIterator(const ReadOptions& options) {
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
      virtual void Delete(const Slice& key) {
        map_->erase(key.ToString());
      }
    };
    Handler handler;
    handler.map_ = &map_;
    return batch->Iterate(&handler);
  }
  
  virtual bool GetProperty(const Slice& property, std::string* value) {
    return false;
  }
  virtual void GetApproximateSizes(const Range* r, int n, uint64_t* sizes) {
    for (int i = 0; i < n; i++) {
      sizes[i] = 0;
    }
  }
  virtual void CompactRange(const Slice* start, const Slice* end,
                            bool reduce_level, int target_level) {
  }
  
  virtual int NumberLevels()
  {
  return 1;
  }
  
  virtual int MaxMemCompactionLevel()
  {
  return 1;
  }
  
  virtual int Level0StopWriteTrigger()
  {
  return -1;
  }
  
  virtual const std::string& GetName() const {
    return name_;
  }
  
  virtual Env* GetEnv() const {
    return nullptr;
  }
  
  virtual const Options& GetOptions() const {
    return options_;
  }
  
  virtual Status Flush(const rocksdb::FlushOptions& options) {
    Status ret;
    return ret;
  }
  
  virtual Status DisableFileDeletions() {
    return Status::OK();
  }
  virtual Status EnableFileDeletions(bool force) {
    return Status::OK();
  }
  virtual Status GetLiveFiles(std::vector<std::string>&, uint64_t* size,
                              bool flush_memtable = true) {
    return Status::OK();
  }
  
  virtual Status GetSortedWalFiles(VectorLogPtr& files) {
    return Status::OK();
  }
  
  virtual Status DeleteFile(std::string name) {
    return Status::OK();
  }
  
  virtual Status GetDbIdentity(std::string& identity) {
    return Status::OK();
  }
  
  virtual SequenceNumber GetLatestSequenceNumber() const {
    return 0;
  }
  virtual Status GetUpdatesSince(rocksdb::SequenceNumber,
                                 unique_ptr<rocksdb::TransactionLogIterator>*) {
    return Status::NotSupported("Not supported in Model DB");
  }
  
private:
  class ModelIter: public Iterator {
   public:
    ModelIter(const KVMap* map, bool owned)
        : map_(map), owned_(owned), iter_(map_->end()) {
    }
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
};

static std::string RandomKey(Random* rnd, int minimum = 0) {
  int len;
  do {
    len = (rnd->OneIn(3)
           ? 1                // Short sometimes to encourage collisions
           : (rnd->OneIn(100) ? rnd->Skewed(10) : rnd->Uniform(10)));
  } while (len < minimum);
  return test::RandomKey(rnd, len);
}

static bool CompareIterators(int step,
                             DB* model,
                             DB* db,
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
       ok && miter->Valid() && dbiter->Valid();
       miter->Next(), dbiter->Next()) {
    count++;
    if (miter->key().compare(dbiter->key()) != 0) {
      fprintf(stderr, "step %d: Key mismatch: '%s' vs. '%s'\n",
              step,
              EscapeString(miter->key()).c_str(),
              EscapeString(dbiter->key()).c_str());
      ok = false;
      break;
    }

    if (miter->value().compare(dbiter->value()) != 0) {
      fprintf(stderr, "step %d: Value mismatch for key '%s': '%s' vs. '%s'\n",
              step,
              EscapeString(miter->key()).c_str(),
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

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
}

void PrefixScanInit(DBTest *dbtest) {
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
  dbtest->dbfull()->TEST_FlushMemTable();
  dbtest->dbfull()->CompactRange(nullptr, nullptr); // move to level 1

  // GROUP 1
  for (int i = 1; i <= small_range_sstfiles; i++) {
    snprintf(buf, sizeof(buf), "%02d______:start", i);
    keystr = std::string(buf);
    ASSERT_OK(dbtest->Put(keystr, keystr));
    snprintf(buf, sizeof(buf), "%02d______:end", i+1);
    keystr = std::string(buf);
    ASSERT_OK(dbtest->Put(keystr, keystr));
    dbtest->dbfull()->TEST_FlushMemTable();
  }

  // GROUP 2
  for (int i = 1; i <= big_range_sstfiles; i++) {
    std::string keystr;
    snprintf(buf, sizeof(buf), "%02d______:start", 0);
    keystr = std::string(buf);
    ASSERT_OK(dbtest->Put(keystr, keystr));
    snprintf(buf, sizeof(buf), "%02d______:end",
             small_range_sstfiles+i+1);
    keystr = std::string(buf);
    ASSERT_OK(dbtest->Put(keystr, keystr));
    dbtest->dbfull()->TEST_FlushMemTable();
  }
}

TEST(DBTest, PrefixScan) {
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
  options.create_if_missing = true;
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
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

  InternalKeyComparator cmp(BytewiseComparator());
  Options options;
  EnvOptions sopt;
  VersionSet vset(dbname, &options, sopt, nullptr, &cmp);
  ASSERT_OK(vset.Recover());
  VersionEdit vbase;
  uint64_t fnum = 1;
  for (int i = 0; i < num_base_files; i++) {
    InternalKey start(MakeKey(2*fnum), 1, kTypeValue);
    InternalKey limit(MakeKey(2*fnum+1), 1, kTypeDeletion);
    vbase.AddFile(2, fnum++, 1 /* file size */, start, limit, 1, 1);
  }
  ASSERT_OK(vset.LogAndApply(&vbase, &mu));

  uint64_t start_micros = env->NowMicros();

  for (int i = 0; i < iters; i++) {
    VersionEdit vedit;
    vedit.DeleteFile(2, fnum);
    InternalKey start(MakeKey(2*fnum), 1, kTypeValue);
    InternalKey limit(MakeKey(2*fnum+1), 1, kTypeDeletion);
    vedit.AddFile(2, fnum++, 1 /* file size */, start, limit, 1, 1);
    vset.LogAndApply(&vedit, &mu);
  }
  uint64_t stop_micros = env->NowMicros();
  unsigned int us = stop_micros - start_micros;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", num_base_files);
  fprintf(stderr,
          "BM_LogAndApply/%-6s   %8d iters : %9u us (%7.0f us / iter)\n",
          buf, iters, us, ((float)us) / iters);
}

} // namespace rocksdb

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