#if !(defined NDEBUG) || !defined (OS_WIN)
#include <algorithm>
#include <iostream>
#include <set>
#ifndef OS_WIN
# include <unistd.h>
#endif
#include <thread>
#include <unordered_set>
#include <utility>
#include <fcntl.h>
#include "db/filename.h"
#include "db/dbformat.h"
#include "db/db_impl.h"
#include "db/filename.h"
#include "db/job_context.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include "port/stack_trace.h"
#include "rocksdb/cache.h"
#include "rocksdb/compaction_filter.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/experimental.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/perf_context.h"
#include "rocksdb/slice.h"
#include "rocksdb/slice_transform.h"
#include "rocksdb/table.h"
#include "rocksdb/options.h"
#include "rocksdb/table_properties.h"
#include "rocksdb/thread_status.h"
#include "rocksdb/utilities/write_batch_with_index.h"
#include "rocksdb/utilities/checkpoint.h"
#include "rocksdb/utilities/convenience.h"
#include "rocksdb/utilities/optimistic_transaction_db.h"
#include "table/block_based_table_factory.h"
#include "table/mock_table.h"
#include "table/plain_table_factory.h"
#include "util/hash.h"
#include "util/hash_linklist_rep.h"
#include "utilities/merge_operators.h"
#include "util/logging.h"
#include "util/compression.h"
#include "util/mutexlock.h"
#include "util/rate_limiter.h"
#include "util/statistics.h"
#include "util/testharness.h"
#include "util/scoped_arena_iterator.h"
#include "util/sync_point.h"
#include "util/testutil.h"
#include "util/mock_env.h"
#include "util/string_util.h"
#include "util/thread_status_util.h"
#include "util/xfunc.h"
namespace rocksdb {
static std::string RandomString(Random* rnd, int len) {
  std::string r;
  test::RandomString(rnd, len, &r);
  return r;
}
namespace anon {
class AtomicCounter {
 private:
  Env* env_;
  port::Mutex mu_;
  port::CondVar cond_count_;
  int count_;
 public:
  AtomicCounter(Env* env = NULL) : env_(env), cond_count_(&mu_), count_(0) {}
  void Increment() {
    MutexLock l(&mu_);
    count_++;
    cond_count_.SignalAll();
  }
  int Read() {
    MutexLock l(&mu_);
    return count_;
  }
  bool WaitFor(int count) {
    MutexLock l(&mu_);
    uint64_t start = env_->NowMicros();
    while (count_ < count) {
      uint64_t now = env_->NowMicros();
      cond_count_.TimedWait(now + 1 * 000 * 000);
      if (env_->NowMicros() - start > 10 * 000 * 000) {
        return false;
      }
      if (count_ < count) {
        GTEST_LOG_(WARNING) << "WaitFor is taking more time than usual";
      }
    }
    return true;
  }
  void Reset() {
    MutexLock l(&mu_);
    count_ = 0;
    cond_count_.SignalAll();
  }
};
struct OptionsOverride {
  std::shared_ptr<const FilterPolicy> filter_policy = nullptr;
  int skip_policy = 0;
};
}
static std::string Key(int i) {
  char buf[100];
  snprintf(buf, sizeof(buf), "key%06d", i);
  return std::string(buf);
}
class SpecialEnv : public EnvWrapper {
 public:
  Random rnd_;
  port::Mutex rnd_mutex_;
  std::atomic<bool> delay_sstable_sync_;
  std::atomic<bool> drop_writes_;
  std::atomic<bool> no_space_;
  std::atomic<bool> non_writable_;
  std::atomic<bool> manifest_sync_error_;
  std::atomic<bool> manifest_write_error_;
  std::atomic<bool> log_write_error_;
  std::atomic<int> log_write_slowdown_;
  bool count_random_reads_;
  anon::AtomicCounter random_read_counter_;
  bool count_sequential_reads_;
  anon::AtomicCounter sequential_read_counter_;
  anon::AtomicCounter sleep_counter_;
  std::atomic<int64_t> bytes_written_;
  std::atomic<int> sync_counter_;
  std::atomic<uint32_t> non_writeable_rate_;
  std::atomic<uint32_t> new_writable_count_;
  std::atomic<uint32_t> non_writable_count_;
  std::function<void()>* table_write_callback_;
  std::atomic<int64_t> addon_time_;
  bool no_sleep_;
  explicit SpecialEnv(Env* base)
      : EnvWrapper(base),
        rnd_(301),
        sleep_counter_(this),
        addon_time_(0),
        no_sleep_(false) {
    delay_sstable_sync_.store(false, std::memory_order_release);
    drop_writes_.store(false, std::memory_order_release);
    no_space_.store(false, std::memory_order_release);
    non_writable_.store(false, std::memory_order_release);
    count_random_reads_ = false;
    count_sequential_reads_ = false;
    manifest_sync_error_.store(false, std::memory_order_release);
    manifest_write_error_.store(false, std::memory_order_release);
    log_write_error_.store(false, std::memory_order_release);
    log_write_slowdown_ = 0;
    bytes_written_ = 0;
    sync_counter_ = 0;
    non_writeable_rate_ = 0;
    new_writable_count_ = 0;
    non_writable_count_ = 0;
    table_write_callback_ = nullptr;
  }
  Status NewWritableFile(const std::string& f, unique_ptr<WritableFile>* r,
                         const EnvOptions& soptions) override {
    class SSTableFile : public WritableFile {
     private:
      SpecialEnv* env_;
      unique_ptr<WritableFile> base_;
     public:
      SSTableFile(SpecialEnv* env, unique_ptr<WritableFile>&& base)
          : env_(env),
            base_(std::move(base)) {
      }
      Status Append(const Slice& data) override {
        if (env_->table_write_callback_) {
          (*env_->table_write_callback_)();
        }
        if (env_->drop_writes_.load(std::memory_order_acquire)) {
          return Status::OK();
        } else if (env_->no_space_.load(std::memory_order_acquire)) {
          return Status::IOError("No space left on device");
        } else {
          env_->bytes_written_ += data.size();
          return base_->Append(data);
        }
      }
      Status Close() override {
        size_t preallocation_size = preallocation_block_size();
        TEST_SYNC_POINT_CALLBACK("DBTestWritableFile.GetPreallocationStatus",
                                 &preallocation_size);
        return base_->Close();
      }
      Status Flush() override { return base_->Flush(); }
      Status Sync() override {
        ++env_->sync_counter_;
        while (env_->delay_sstable_sync_.load(std::memory_order_acquire)) {
          env_->SleepForMicroseconds(100000);
        }
        return base_->Sync();
      }
      void SetIOPriority(Env::IOPriority pri) override {
        base_->SetIOPriority(pri);
      }
    };
    class ManifestFile : public WritableFile {
     private:
      SpecialEnv* env_;
      unique_ptr<WritableFile> base_;
     public:
      ManifestFile(SpecialEnv* env, unique_ptr<WritableFile>&& b)
          : env_(env), base_(std::move(b)) { }
      Status Append(const Slice& data) override {
        if (env_->manifest_write_error_.load(std::memory_order_acquire)) {
          return Status::IOError("simulated writer error");
        } else {
          return base_->Append(data);
        }
      }
      Status Close() override { return base_->Close(); }
      Status Flush() override { return base_->Flush(); }
      Status Sync() override {
        ++env_->sync_counter_;
        if (env_->manifest_sync_error_.load(std::memory_order_acquire)) {
          return Status::IOError("simulated sync error");
        } else {
          return base_->Sync();
        }
      }
      uint64_t GetFileSize() override { return base_->GetFileSize(); }
    };
    class WalFile : public WritableFile {
     private:
      SpecialEnv* env_;
      unique_ptr<WritableFile> base_;
     public:
      WalFile(SpecialEnv* env, unique_ptr<WritableFile>&& b)
          : env_(env), base_(std::move(b)) {}
      Status Append(const Slice& data) override {
        if (env_->log_write_error_.load(std::memory_order_acquire)) {
          return Status::IOError("simulated writer error");
        } else {
          int slowdown =
              env_->log_write_slowdown_.load(std::memory_order_acquire);
          if (slowdown > 0) {
            env_->SleepForMicroseconds(slowdown);
          }
          return base_->Append(data);
        }
      }
      Status Close() override { return base_->Close(); }
      Status Flush() override { return base_->Flush(); }
      Status Sync() override {
        ++env_->sync_counter_;
        return base_->Sync();
      }
    };
    if (non_writeable_rate_.load(std::memory_order_acquire) > 0) {
      uint32_t random_number;
      {
        MutexLock l(&rnd_mutex_);
        random_number = rnd_.Uniform(100);
      }
      if (random_number < non_writeable_rate_.load()) {
        return Status::IOError("simulated random write error");
      }
    }
    new_writable_count_++;
    if (non_writable_count_.load() > 0) {
      non_writable_count_--;
      return Status::IOError("simulated write error");
    }
    Status s = target()->NewWritableFile(f, r, soptions);
    if (s.ok()) {
      if (strstr(f.c_str(), ".sst") != nullptr) {
        r->reset(new SSTableFile(this, std::move(*r)));
      } else if (strstr(f.c_str(), "MANIFEST") != nullptr) {
        r->reset(new ManifestFile(this, std::move(*r)));
      } else if (strstr(f.c_str(), "log") != nullptr) {
        r->reset(new WalFile(this, std::move(*r)));
      }
    }
    return s;
  }
  Status NewRandomAccessFile(const std::string& f,
                             unique_ptr<RandomAccessFile>* r,
                             const EnvOptions& soptions) override {
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
                          char* scratch) const override {
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
  Status NewSequentialFile(const std::string& f, unique_ptr<SequentialFile>* r,
                           const EnvOptions& soptions) override {
    class CountingFile : public SequentialFile {
     private:
      unique_ptr<SequentialFile> target_;
      anon::AtomicCounter* counter_;
     public:
      CountingFile(unique_ptr<SequentialFile>&& target,
                   anon::AtomicCounter* counter)
          : target_(std::move(target)), counter_(counter) {}
      virtual Status Read(size_t n, Slice* result, char* scratch) override {
        counter_->Increment();
        return target_->Read(n, result, scratch);
      }
      virtual Status Skip(uint64_t n) override { return target_->Skip(n); }
    };
    Status s = target()->NewSequentialFile(f, r, soptions);
    if (s.ok() && count_sequential_reads_) {
      r->reset(new CountingFile(std::move(*r), &sequential_read_counter_));
    }
    return s;
  }
  virtual void SleepForMicroseconds(int micros) override {
    sleep_counter_.Increment();
    if (no_sleep_) {
      addon_time_.fetch_add(micros);
    } else {
      target()->SleepForMicroseconds(micros);
    }
  }
  virtual Status GetCurrentTime(int64_t* unix_time) override {
    Status s = target()->GetCurrentTime(unix_time);
    if (s.ok()) {
      *unix_time += addon_time_.load();
    }
    return s;
  }
  virtual uint64_t NowNanos() override {
    return target()->NowNanos() + addon_time_.load() * 1000;
  }
  virtual uint64_t NowMicros() override {
    return target()->NowMicros() + addon_time_.load();
  }
};
class DBTest : public testing::Test {
 protected:
  enum OptionConfig {
    kDefault = 0,
    kBlockBasedTableWithPrefixHashIndex = 1,
    kBlockBasedTableWithWholeKeyHashIndex = 2,
    kPlainTableFirstBytePrefix = 3,
    kPlainTableCappedPrefix = 4,
    kPlainTableAllBytesPrefix = 5,
    kVectorRep = 6,
    kHashLinkList = 7,
    kHashCuckoo = 8,
    kMergePut = 9,
    kFilter = 10,
    kFullFilter = 11,
    kUncompressed = 12,
    kNumLevel_3 = 13,
    kDBLogDir = 14,
    kWalDirAndMmapReads = 15,
    kManifestFileSize = 16,
    kCompactOnFlush = 17,
    kPerfOptions = 18,
    kDeletesFilterFirst = 19,
    kHashSkipList = 20,
    kUniversalCompaction = 21,
    kUniversalCompactionMultiLevel = 22,
    kCompressedBlockCache = 23,
    kInfiniteMaxOpenFiles = 24,
    kxxHashChecksum = 25,
    kFIFOCompaction = 26,
    kOptimizeFiltersForHits = 27,
    kRowCache = 28,
    kEnd = 29
  };
  int option_config_;
 public:
  std::string dbname_;
  std::string alternative_wal_dir_;
  MockEnv* mem_env_;
  SpecialEnv* env_;
  DB* db_;
  std::vector<ColumnFamilyHandle*> handles_;
  Options last_options_;
  enum OptionSkip {
    kNoSkip = 0,
    kSkipDeletesFilterFirst = 1,
    kSkipUniversalCompaction = 2,
    kSkipMergePut = 4,
    kSkipPlainTable = 8,
    kSkipHashIndex = 16,
    kSkipNoSeekToLast = 32,
    kSkipHashCuckoo = 64,
    kSkipFIFOCompaction = 128,
    kSkipMmapReads = 256,
  };
  DBTest() : option_config_(kDefault),
             mem_env_(!getenv("MEM_ENV") ? nullptr :
                                           new MockEnv(Env::Default())),
             env_(new SpecialEnv(mem_env_ ? mem_env_ : Env::Default())) {
    env_->SetBackgroundThreads(1, Env::LOW);
    env_->SetBackgroundThreads(1, Env::HIGH);
    dbname_ = test::TmpDir(env_) + "/db_test";
    alternative_wal_dir_ = dbname_ + "/wal";
    auto options = CurrentOptions();
    auto delete_options = options;
    delete_options.wal_dir = alternative_wal_dir_;
    EXPECT_OK(DestroyDB(dbname_, delete_options));
    EXPECT_OK(DestroyDB(dbname_, options));
    db_ = nullptr;
    Reopen(options);
  }
  ~DBTest() {
    rocksdb::SyncPoint::GetInstance()->DisableProcessing();
    rocksdb::SyncPoint::GetInstance()->LoadDependency({});
    rocksdb::SyncPoint::GetInstance()->ClearAllCallBacks();
    Close();
    Options options;
    options.db_paths.emplace_back(dbname_, 0);
    options.db_paths.emplace_back(dbname_ + "_2", 0);
    options.db_paths.emplace_back(dbname_ + "_3", 0);
    options.db_paths.emplace_back(dbname_ + "_4", 0);
    EXPECT_OK(DestroyDB(dbname_, options));
    delete env_;
  }
  bool ChangeOptions(int skip_mask = kNoSkip) {
    for(option_config_++; option_config_ < kEnd; option_config_++) {
      if ((skip_mask & kSkipDeletesFilterFirst) &&
          option_config_ == kDeletesFilterFirst) {
        continue;
      }
      if ((skip_mask & kSkipUniversalCompaction) &&
          (option_config_ == kUniversalCompaction ||
           option_config_ == kUniversalCompactionMultiLevel)) {
        continue;
      }
      if ((skip_mask & kSkipMergePut) && option_config_ == kMergePut) {
        continue;
      }
      if ((skip_mask & kSkipNoSeekToLast) &&
          (option_config_ == kHashLinkList ||
           option_config_ == kHashSkipList)) {;
        continue;
      }
      if ((skip_mask & kSkipPlainTable) &&
          (option_config_ == kPlainTableAllBytesPrefix ||
           option_config_ == kPlainTableFirstBytePrefix ||
           option_config_ == kPlainTableCappedPrefix)) {
        continue;
      }
      if ((skip_mask & kSkipHashIndex) &&
          (option_config_ == kBlockBasedTableWithPrefixHashIndex ||
           option_config_ == kBlockBasedTableWithWholeKeyHashIndex)) {
        continue;
      }
      if ((skip_mask & kSkipHashCuckoo) && (option_config_ == kHashCuckoo)) {
        continue;
      }
      if ((skip_mask & kSkipFIFOCompaction) &&
          option_config_ == kFIFOCompaction) {
        continue;
      }
      if ((skip_mask & kSkipMmapReads) &&
          option_config_ == kWalDirAndMmapReads) {
        continue;
      }
      break;
    }
    if (option_config_ >= kEnd) {
      Destroy(last_options_);
      return false;
    } else {
      auto options = CurrentOptions();
      options.create_if_missing = true;
      DestroyAndReopen(options);
      return true;
    }
  }
  bool ChangeCompactOptions() {
    if (option_config_ == kDefault) {
      option_config_ = kUniversalCompaction;
      Destroy(last_options_);
      auto options = CurrentOptions();
      options.create_if_missing = true;
      TryReopen(options);
      return true;
    } else if (option_config_ == kUniversalCompaction) {
      option_config_ = kUniversalCompactionMultiLevel;
      Destroy(last_options_);
      auto options = CurrentOptions();
      options.create_if_missing = true;
      TryReopen(options);
      return true;
    } else {
      return false;
    }
  }
  bool ChangeFilterOptions() {
    if (option_config_ == kDefault) {
      option_config_ = kFilter;
    } else if (option_config_ == kFilter) {
      option_config_ = kFullFilter;
    } else {
      return false;
    }
    Destroy(last_options_);
    auto options = CurrentOptions();
    options.create_if_missing = true;
    TryReopen(options);
    return true;
  }
  Options CurrentOptions(
      const anon::OptionsOverride& options_override = anon::OptionsOverride()) {
    Options options;
    return CurrentOptions(options, options_override);
  }
  Options CurrentOptions(
      const Options& defaultOptions,
      const anon::OptionsOverride& options_override = anon::OptionsOverride()) {
    Options options = defaultOptions;
    XFUNC_TEST("", "dbtest_options", inplace_options1, GetXFTestOptions,
               reinterpret_cast<Options*>(&options),
               options_override.skip_policy);
    BlockBasedTableOptions table_options;
    bool set_block_based_table_factory = true;
    switch (option_config_) {
      case kHashSkipList:
        options.prefix_extractor.reset(NewFixedPrefixTransform(1));
        options.memtable_factory.reset(
            NewHashSkipListRepFactory(16));
        break;
      case kPlainTableFirstBytePrefix:
        options.table_factory.reset(new PlainTableFactory());
        options.prefix_extractor.reset(NewFixedPrefixTransform(1));
        options.allow_mmap_reads = true;
        options.max_sequential_skip_in_iterations = 999999;
        set_block_based_table_factory = false;
        break;
      case kPlainTableCappedPrefix:
        options.table_factory.reset(new PlainTableFactory());
        options.prefix_extractor.reset(NewCappedPrefixTransform(8));
        options.allow_mmap_reads = true;
        options.max_sequential_skip_in_iterations = 999999;
        set_block_based_table_factory = false;
        break;
      case kPlainTableAllBytesPrefix:
        options.table_factory.reset(new PlainTableFactory());
        options.prefix_extractor.reset(NewNoopTransform());
        options.allow_mmap_reads = true;
        options.max_sequential_skip_in_iterations = 999999;
        set_block_based_table_factory = false;
        break;
      case kMergePut:
        options.merge_operator = MergeOperators::CreatePutOperator();
        break;
      case kFilter:
        table_options.filter_policy.reset(NewBloomFilterPolicy(10, true));
        break;
      case kFullFilter:
        table_options.filter_policy.reset(NewBloomFilterPolicy(10, false));
        break;
      case kUncompressed:
        options.compression = kNoCompression;
        break;
      case kNumLevel_3:
        options.num_levels = 3;
        break;
      case kDBLogDir:
        options.db_log_dir = test::TmpDir(env_);
        break;
      case kWalDirAndMmapReads:
        options.wal_dir = alternative_wal_dir_;
        options.allow_mmap_reads = true;
        break;
      case kManifestFileSize:
        options.max_manifest_file_size = 50;
      case kCompactOnFlush:
        options.purge_redundant_kvs_while_flush =
          !options.purge_redundant_kvs_while_flush;
        break;
      case kPerfOptions:
        options.soft_rate_limit = 2.0;
        options.delayed_write_rate = 8 * 1024 * 1024;
        break;
      case kDeletesFilterFirst:
        options.filter_deletes = true;
        break;
      case kVectorRep:
        options.memtable_factory.reset(new VectorRepFactory(100));
        break;
      case kHashLinkList:
        options.prefix_extractor.reset(NewFixedPrefixTransform(1));
        options.memtable_factory.reset(
            NewHashLinkListRepFactory(4, 0, 3, true, 4));
        break;
      case kHashCuckoo:
        options.memtable_factory.reset(
            NewHashCuckooRepFactory(options.write_buffer_size));
        break;
      case kUniversalCompaction:
        options.compaction_style = kCompactionStyleUniversal;
        options.num_levels = 1;
        break;
      case kUniversalCompactionMultiLevel:
        options.compaction_style = kCompactionStyleUniversal;
        options.num_levels = 8;
        break;
      case kCompressedBlockCache:
        options.allow_mmap_writes = true;
        table_options.block_cache_compressed = NewLRUCache(8*1024*1024);
        break;
      case kInfiniteMaxOpenFiles:
        options.max_open_files = -1;
        break;
      case kxxHashChecksum: {
        table_options.checksum = kxxHash;
        break;
      }
      case kFIFOCompaction: {
        options.compaction_style = kCompactionStyleFIFO;
        break;
      }
      case kBlockBasedTableWithPrefixHashIndex: {
        table_options.index_type = BlockBasedTableOptions::kHashSearch;
        options.prefix_extractor.reset(NewFixedPrefixTransform(1));
        break;
      }
      case kBlockBasedTableWithWholeKeyHashIndex: {
        table_options.index_type = BlockBasedTableOptions::kHashSearch;
        options.prefix_extractor.reset(NewNoopTransform());
        break;
      }
      case kOptimizeFiltersForHits: {
        options.optimize_filters_for_hits = true;
        set_block_based_table_factory = true;
        break;
      }
      case kRowCache: {
        options.row_cache = NewLRUCache(1024 * 1024);
        break;
      }
      default:
        break;
    }
    if (options_override.filter_policy) {
      table_options.filter_policy = options_override.filter_policy;
    }
    if (set_block_based_table_factory) {
      options.table_factory.reset(NewBlockBasedTableFactory(table_options));
    }
    options.env = env_;
    options.create_if_missing = true;
    return options;
  }
  DBImpl* dbfull() {
    return reinterpret_cast<DBImpl*>(db_);
  }
  void CreateColumnFamilies(const std::vector<std::string>& cfs,
                            const Options& options) {
    ColumnFamilyOptions cf_opts(options);
    size_t cfi = handles_.size();
    handles_.resize(cfi + cfs.size());
    for (auto cf : cfs) {
      ASSERT_OK(db_->CreateColumnFamily(cf_opts, cf, &handles_[cfi++]));
    }
  }
  void CreateAndReopenWithCF(const std::vector<std::string>& cfs,
                             const Options& options) {
    CreateColumnFamilies(cfs, options);
    std::vector<std::string> cfs_plus_default = cfs;
    cfs_plus_default.insert(cfs_plus_default.begin(), kDefaultColumnFamilyName);
    ReopenWithColumnFamilies(cfs_plus_default, options);
  }
  void ReopenWithColumnFamilies(const std::vector<std::string>& cfs,
                                const std::vector<Options>& options) {
    ASSERT_OK(TryReopenWithColumnFamilies(cfs, options));
  }
  void ReopenWithColumnFamilies(const std::vector<std::string>& cfs,
                                const Options& options) {
    ASSERT_OK(TryReopenWithColumnFamilies(cfs, options));
  }
  Status TryReopenWithColumnFamilies(
      const std::vector<std::string>& cfs,
      const std::vector<Options>& options) {
    Close();
    EXPECT_EQ(cfs.size(), options.size());
    std::vector<ColumnFamilyDescriptor> column_families;
    for (size_t i = 0; i < cfs.size(); ++i) {
      column_families.push_back(ColumnFamilyDescriptor(cfs[i], options[i]));
    }
    DBOptions db_opts = DBOptions(options[0]);
    return DB::Open(db_opts, dbname_, column_families, &handles_, &db_);
  }
  Status TryReopenWithColumnFamilies(const std::vector<std::string>& cfs,
                                     const Options& options) {
    Close();
    std::vector<Options> v_opts(cfs.size(), options);
    return TryReopenWithColumnFamilies(cfs, v_opts);
  }
  void Reopen(const Options& options) {
    ASSERT_OK(TryReopen(options));
  }
  void Close() {
    for (auto h : handles_) {
      delete h;
    }
    handles_.clear();
    delete db_;
    db_ = nullptr;
  }
  void DestroyAndReopen(const Options& options) {
    Destroy(last_options_);
    ASSERT_OK(TryReopen(options));
  }
  void Destroy(const Options& options) {
    Close();
    ASSERT_OK(DestroyDB(dbname_, options));
  }
  Status ReadOnlyReopen(const Options& options) {
    return DB::OpenForReadOnly(options, dbname_, &db_);
  }
  Status TryReopen(const Options& options) {
    Close();
    last_options_ = options;
    return DB::Open(options, dbname_, &db_);
  }
  Status Flush(int cf = 0) {
    if (cf == 0) {
      return db_->Flush(FlushOptions());
    } else {
      return db_->Flush(FlushOptions(), handles_[cf]);
    }
  }
  Status Put(const Slice& k, const Slice& v, WriteOptions wo = WriteOptions()) {
    if (kMergePut == option_config_ ) {
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
  Status Delete(const std::string& k) {
    return db_->Delete(WriteOptions(), k);
  }
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
  uint64_t GetNumSnapshots() {
    uint64_t int_num;
    EXPECT_TRUE(dbfull()->GetIntProperty("rocksdb.num-snapshots", &int_num));
    return int_num;
  }
  uint64_t GetTimeOldestSnapshots() {
    uint64_t int_num;
    EXPECT_TRUE(
        dbfull()->GetIntProperty("rocksdb.oldest-snapshot-time", &int_num));
    return int_num;
  }
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
    unsigned int matched = 0;
    for (iter->SeekToLast(); iter->Valid(); iter->Prev()) {
      EXPECT_LT(matched, forward.size());
      EXPECT_EQ(IterStatus(iter), forward[forward.size() - matched - 1]);
      matched++;
    }
    EXPECT_EQ(matched, forward.size());
    delete iter;
    return result;
  }
  std::string AllEntriesFor(const Slice& user_key, int cf = 0) {
    Arena arena;
    ScopedArenaIterator iter;
    if (cf == 0) {
      iter.set(dbfull()->TEST_NewInternalIterator(&arena));
    } else {
      iter.set(dbfull()->TEST_NewInternalIterator(&arena, handles_[cf]));
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
    return result;
  }
  int NumSortedRuns(int cf = 0) {
    ColumnFamilyMetaData cf_meta;
    if (cf == 0) {
      db_->GetColumnFamilyMetaData(&cf_meta);
    } else {
      db_->GetColumnFamilyMetaData(handles_[cf], &cf_meta);
    }
    int num_sr = static_cast<int>(cf_meta.levels[0].files.size());
    for (size_t i = 1U; i < cf_meta.levels.size(); i++) {
      if (cf_meta.levels[i].files.size() > 0) {
        num_sr++;
      }
    }
    return num_sr;
  }
  uint64_t TotalSize(int cf = 0) {
    ColumnFamilyMetaData cf_meta;
    if (cf == 0) {
      db_->GetColumnFamilyMetaData(&cf_meta);
    } else {
      db_->GetColumnFamilyMetaData(handles_[cf], &cf_meta);
    }
    return cf_meta.size;
  }
  int NumTableFilesAtLevel(int level, int cf = 0) {
    std::string property;
    if (cf == 0) {
      EXPECT_TRUE(db_->GetProperty(
          "rocksdb.num-files-at-level" + NumberToString(level), &property));
    } else {
      EXPECT_TRUE(db_->GetProperty(
          handles_[cf], "rocksdb.num-files-at-level" + NumberToString(level),
          &property));
    }
    return atoi(property.c_str());
  }
  uint64_t SizeAtLevel(int level) {
    std::vector<LiveFileMetaData> metadata;
    db_->GetLiveFilesMetaData(&metadata);
    uint64_t sum = 0;
    for (const auto& m : metadata) {
      if (m.level == level) {
        sum += m.size;
      }
    }
    return sum;
  }
  int TotalLiveFiles(int cf = 0) {
    ColumnFamilyMetaData cf_meta;
    if (cf == 0) {
      db_->GetColumnFamilyMetaData(&cf_meta);
    } else {
      db_->GetColumnFamilyMetaData(handles_[cf], &cf_meta);
    }
    int num_files = 0;
    for (auto& level : cf_meta.levels) {
      num_files += level.files.size();
    }
    return num_files;
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
  std::string FilesPerLevel(int cf = 0) {
    int num_levels =
        (cf == 0) ? db_->NumberLevels() : db_->NumberLevels(handles_[1]);
    std::string result;
    size_t last_non_zero_offset = 0;
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
  size_t CountFiles() {
    std::vector<std::string> files;
    env_->GetChildren(dbname_, &files);
    std::vector<std::string> logfiles;
    if (dbname_ != last_options_.wal_dir) {
      env_->GetChildren(last_options_.wal_dir, &logfiles);
    }
    return files.size() + logfiles.size();
  }
  size_t CountLiveFiles() {
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
  void Compact(int cf, const Slice& start, const Slice& limit,
               uint32_t target_path_id) {
    CompactRangeOptions compact_options;
    compact_options.target_path_id = target_path_id;
    ASSERT_OK(db_->CompactRange(compact_options, handles_[cf], &start, &limit));
  }
  void Compact(int cf, const Slice& start, const Slice& limit) {
    ASSERT_OK(
        db_->CompactRange(CompactRangeOptions(), handles_[cf], &start, &limit));
  }
  void Compact(const Slice& start, const Slice& limit) {
    ASSERT_OK(db_->CompactRange(CompactRangeOptions(), &start, &limit));
  }
  void MakeTables(int n, const std::string& small, const std::string& large,
                  int cf = 0) {
    for (int i = 0; i < n; i++) {
      ASSERT_OK(Put(cf, small, "begin"));
      ASSERT_OK(Put(cf, large, "end"));
      ASSERT_OK(Flush(cf));
    }
  }
  void FillLevels(const std::string& smallest, const std::string& largest,
                  int cf) {
    MakeTables(db_->NumberLevels(handles_[cf]), smallest, largest, cf);
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
  int GetSstFileCount(std::string path) {
    std::vector<std::string> files;
    env_->GetChildren(path, &files);
    int sst_count = 0;
    uint64_t number;
    FileType type;
    for (size_t i = 0; i < files.size(); i++) {
      if (ParseFileName(files[i], &number, &type) && type == kTableFile) {
        sst_count++;
      }
    }
    return sst_count;
  }
  void GenerateNewFile(Random* rnd, int* key_idx, bool nowait = false) {
    for (int i = 0; i < 11; i++) {
      ASSERT_OK(Put(Key(*key_idx), RandomString(rnd, (i == 10) ? 1 : 10000)));
      (*key_idx)++;
    }
    if (!nowait) {
      dbfull()->TEST_WaitForFlushMemTable();
      dbfull()->TEST_WaitForCompact();
    }
  }
  void GenerateNewRandomFile(Random* rnd, bool nowait = false) {
    for (int i = 0; i < 100; i++) {
      ASSERT_OK(Put("key" + RandomString(rnd, 7), RandomString(rnd, 1000)));
    }
    ASSERT_OK(Put("key" + RandomString(rnd, 7), RandomString(rnd, 1)));
    if (!nowait) {
      dbfull()->TEST_WaitForFlushMemTable();
      dbfull()->TEST_WaitForCompact();
    }
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
    EXPECT_OK(status);
    EXPECT_TRUE(iter->Valid());
    return std::move(iter);
  }
  std::string DummyString(size_t len, char c = 'a') {
    return std::string(len, c);
  }
  void VerifyIterLast(std::string expected_key, int cf = 0) {
    Iterator* iter;
    ReadOptions ro;
    if (cf == 0) {
      iter = db_->NewIterator(ro);
    } else {
      iter = db_->NewIterator(ro, handles_[cf]);
    }
    iter->SeekToLast();
    ASSERT_EQ(IterStatus(iter), expected_key);
    delete iter;
  }
  static UpdateStatus
      updateInPlaceSmallerSize(char* prevValue, uint32_t* prevSize,
                               Slice delta, std::string* newValue) {
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
  static UpdateStatus
      updateInPlaceSmallerVarintSize(char* prevValue, uint32_t* prevSize,
                                     Slice delta, std::string* newValue) {
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
  static UpdateStatus
      updateInPlaceLargerSize(char* prevValue, uint32_t* prevSize,
                              Slice delta, std::string* newValue) {
    *newValue = std::string(delta.size(), 'c');
    return UpdateStatus::UPDATED;
  }
  static UpdateStatus
      updateInPlaceNoAction(char* prevValue, uint32_t* prevSize,
                            Slice delta, std::string* newValue) {
    return UpdateStatus::UPDATE_FAILED;
  }
  void validateNumberOfEntries(int numValues, int cf = 0) {
    ScopedArenaIterator iter;
    Arena arena;
    if (cf != 0) {
      iter.set(dbfull()->TEST_NewInternalIterator(&arena, handles_[cf]));
    } else {
      iter.set(dbfull()->TEST_NewInternalIterator(&arena));
    }
    iter->SeekToFirst();
    ASSERT_EQ(iter->status().ok(), true);
    int seq = numValues;
    while (iter->Valid()) {
      ParsedInternalKey ikey;
      ikey.sequence = -1;
      ASSERT_EQ(ParseInternalKey(iter->key(), &ikey), true);
      ASSERT_EQ(ikey.sequence, (unsigned)seq--);
      iter->Next();
    }
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
static long TestGetTickerCount(const Options& options, Tickers ticker_type) {
  return options.statistics->getTickerCount(ticker_type);
}
namespace {
void VerifyTableProperties(DB* db, uint64_t expected_entries_size) {
  TablePropertiesCollection props;
  ASSERT_OK(db->GetPropertiesOfAllTables(&props));
  ASSERT_EQ(4U, props.size());
  std::unordered_set<uint64_t> unique_entries;
  uint64_t sum = 0;
  for (const auto& item : props) {
    unique_entries.insert(item.second->num_entries);
    sum += item.second->num_entries;
  }
  ASSERT_EQ(props.size(), unique_entries.size());
  ASSERT_EQ(expected_entries_size, sum);
}
uint64_t GetNumberOfSstFilesForColumnFamily(DB* db,
                                            std::string column_family_name) {
  std::vector<LiveFileMetaData> metadata;
  db->GetLiveFilesMetaData(&metadata);
  uint64_t result = 0;
  for (auto& fileMetadata : metadata) {
    result += (fileMetadata.column_family_name == column_family_name);
  }
  return result;
}
}
TEST_F(DBTest, Empty) {
  do {
    Options options;
    options.env = env_;
    options.write_buffer_size = 100000;
    options = CurrentOptions(options);
    CreateAndReopenWithCF({"pikachu"}, options);
    std::string num;
    ASSERT_TRUE(dbfull()->GetProperty(
        handles_[1], "rocksdb.num-entries-active-mem-table", &num));
    ASSERT_EQ("0", num);
    ASSERT_OK(Put(1, "foo", "v1"));
    ASSERT_EQ("v1", Get(1, "foo"));
    ASSERT_TRUE(dbfull()->GetProperty(
        handles_[1], "rocksdb.num-entries-active-mem-table", &num));
    ASSERT_EQ("1", num);
    env_->delay_sstable_sync_.store(true, std::memory_order_release);
    Put(1, "k1", std::string(100000, 'x'));
    ASSERT_TRUE(dbfull()->GetProperty(
        handles_[1], "rocksdb.num-entries-active-mem-table", &num));
    ASSERT_EQ("2", num);
    Put(1, "k2", std::string(100000, 'y'));
    ASSERT_TRUE(dbfull()->GetProperty(
        handles_[1], "rocksdb.num-entries-active-mem-table", &num));
    ASSERT_EQ("1", num);
    ASSERT_EQ("v1", Get(1, "foo"));
    env_->delay_sstable_sync_.store(false, std::memory_order_release);
    ASSERT_OK(db_->DisableFileDeletions());
    ASSERT_TRUE(
        dbfull()->GetProperty("rocksdb.is-file-deletions-enabled", &num));
    ASSERT_EQ("1", num);
    ASSERT_OK(db_->DisableFileDeletions());
    ASSERT_TRUE(
        dbfull()->GetProperty("rocksdb.is-file-deletions-enabled", &num));
    ASSERT_EQ("2", num);
    ASSERT_OK(db_->DisableFileDeletions());
    ASSERT_TRUE(
        dbfull()->GetProperty("rocksdb.is-file-deletions-enabled", &num));
    ASSERT_EQ("3", num);
    ASSERT_OK(db_->EnableFileDeletions(false));
    ASSERT_TRUE(
        dbfull()->GetProperty("rocksdb.is-file-deletions-enabled", &num));
    ASSERT_EQ("2", num);
    ASSERT_OK(db_->EnableFileDeletions());
    ASSERT_TRUE(
        dbfull()->GetProperty("rocksdb.is-file-deletions-enabled", &num));
    ASSERT_EQ("0", num);
  } while (ChangeOptions());
}
TEST_F(DBTest, WriteEmptyBatch) {
  Options options;
  options.env = env_;
  options.write_buffer_size = 100000;
  options = CurrentOptions(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  ASSERT_OK(Put(1, "foo", "bar"));
  env_->sync_counter_.store(0);
  WriteOptions wo;
  wo.sync = true;
  wo.disableWAL = false;
  WriteBatch empty_batch;
  ASSERT_OK(dbfull()->Write(wo, &empty_batch));
  ASSERT_GE(env_->sync_counter_.load(), 1);
  ASSERT_OK(TryReopenWithColumnFamilies({"default", "pikachu"}, options));
  ASSERT_EQ("bar", Get(1, "foo"));
}
TEST_F(DBTest, ReadOnlyDB) {
  ASSERT_OK(Put("foo", "v1"));
  ASSERT_OK(Put("bar", "v2"));
  ASSERT_OK(Put("foo", "v3"));
  Close();
  auto options = CurrentOptions();
  assert(options.env = env_);
  ASSERT_OK(ReadOnlyReopen(options));
  ASSERT_EQ("v3", Get("foo"));
  ASSERT_EQ("v2", Get("bar"));
  Iterator* iter = db_->NewIterator(ReadOptions());
  int count = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ASSERT_OK(iter->status());
    ++count;
  }
  ASSERT_EQ(count, 2);
  delete iter;
  Close();
  Reopen(options);
  Flush();
  Close();
  ASSERT_OK(ReadOnlyReopen(options));
  ASSERT_EQ("v3", Get("foo"));
  ASSERT_EQ("v2", Get("bar"));
}
TEST_F(DBTest, CompactedDB) {
  const uint64_t kFileSize = 1 << 20;
  Options options;
  options.disable_auto_compactions = true;
  options.max_mem_compaction_level = 0;
  options.write_buffer_size = kFileSize;
  options.target_file_size_base = kFileSize;
  options.max_bytes_for_level_base = 1 << 30;
  options.compression = kNoCompression;
  options = CurrentOptions(options);
  Reopen(options);
  ASSERT_OK(Put("aaa", DummyString(kFileSize / 2, '1')));
  Flush();
  Close();
  ASSERT_OK(ReadOnlyReopen(options));
  Status s = Put("new", "value");
  ASSERT_EQ(s.ToString(),
            "Not implemented: Not supported operation in read only mode.");
  ASSERT_EQ(DummyString(kFileSize / 2, '1'), Get("aaa"));
  Close();
  options.max_open_files = -1;
  ASSERT_OK(ReadOnlyReopen(options));
  s = Put("new", "value");
  ASSERT_EQ(s.ToString(),
            "Not implemented: Not supported in compacted db mode.");
  ASSERT_EQ(DummyString(kFileSize / 2, '1'), Get("aaa"));
  Close();
  Reopen(options);
  ASSERT_OK(Put("bbb", DummyString(kFileSize / 2, '2')));
  Flush();
  ASSERT_OK(Put("aaa", DummyString(kFileSize / 2, 'a')));
  Flush();
  ASSERT_OK(Put("bbb", DummyString(kFileSize / 2, 'b')));
  ASSERT_OK(Put("eee", DummyString(kFileSize / 2, 'e')));
  Flush();
  Close();
  ASSERT_OK(ReadOnlyReopen(options));
  s = Put("new", "value");
  ASSERT_EQ(s.ToString(),
            "Not implemented: Not supported operation in read only mode.");
  Close();
  Reopen(options);
  ASSERT_OK(Put("fff", DummyString(kFileSize / 2, 'f')));
  ASSERT_OK(Put("hhh", DummyString(kFileSize / 2, 'h')));
  ASSERT_OK(Put("iii", DummyString(kFileSize / 2, 'i')));
  ASSERT_OK(Put("jjj", DummyString(kFileSize / 2, 'j')));
  db_->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  ASSERT_EQ(3, NumTableFilesAtLevel(1));
  Close();
  ASSERT_OK(ReadOnlyReopen(options));
  s = Put("new", "value");
  ASSERT_EQ(s.ToString(),
            "Not implemented: Not supported in compacted db mode.");
  ASSERT_EQ("NOT_FOUND", Get("abc"));
  ASSERT_EQ(DummyString(kFileSize / 2, 'a'), Get("aaa"));
  ASSERT_EQ(DummyString(kFileSize / 2, 'b'), Get("bbb"));
  ASSERT_EQ("NOT_FOUND", Get("ccc"));
  ASSERT_EQ(DummyString(kFileSize / 2, 'e'), Get("eee"));
  ASSERT_EQ(DummyString(kFileSize / 2, 'f'), Get("fff"));
  ASSERT_EQ("NOT_FOUND", Get("ggg"));
  ASSERT_EQ(DummyString(kFileSize / 2, 'h'), Get("hhh"));
  ASSERT_EQ(DummyString(kFileSize / 2, 'i'), Get("iii"));
  ASSERT_EQ(DummyString(kFileSize / 2, 'j'), Get("jjj"));
  ASSERT_EQ("NOT_FOUND", Get("kkk"));
  std::vector<std::string> values;
  std::vector<Status> status_list = dbfull()->MultiGet(ReadOptions(),
      std::vector<Slice>({Slice("aaa"), Slice("ccc"), Slice("eee"),
                          Slice("ggg"), Slice("iii"), Slice("kkk")}),
      &values);
  ASSERT_EQ(status_list.size(), static_cast<uint64_t>(6));
  ASSERT_EQ(values.size(), static_cast<uint64_t>(6));
  ASSERT_OK(status_list[0]);
  ASSERT_EQ(DummyString(kFileSize / 2, 'a'), values[0]);
  ASSERT_TRUE(status_list[1].IsNotFound());
  ASSERT_OK(status_list[2]);
  ASSERT_EQ(DummyString(kFileSize / 2, 'e'), values[2]);
  ASSERT_TRUE(status_list[3].IsNotFound());
  ASSERT_OK(status_list[4]);
  ASSERT_EQ(DummyString(kFileSize / 2, 'i'), values[4]);
  ASSERT_TRUE(status_list[5].IsNotFound());
}
TEST_F(DBTest, IndexAndFilterBlocksOfNewTableAddedToCache) {
  Options options = CurrentOptions();
  options.create_if_missing = true;
  options.statistics = rocksdb::CreateDBStatistics();
  BlockBasedTableOptions table_options;
  table_options.cache_index_and_filter_blocks = true;
  table_options.filter_policy.reset(NewBloomFilterPolicy(20));
  options.table_factory.reset(new BlockBasedTableFactory(table_options));
  CreateAndReopenWithCF({"pikachu"}, options);
  ASSERT_OK(Put(1, "key", "val"));
  ASSERT_OK(Flush(1));
  ASSERT_EQ(1, TestGetTickerCount(options, BLOCK_CACHE_INDEX_MISS));
  ASSERT_EQ(1, TestGetTickerCount(options, BLOCK_CACHE_FILTER_MISS));
  ASSERT_EQ(2,
            TestGetTickerCount(options, BLOCK_CACHE_ADD));
  ASSERT_EQ(0, TestGetTickerCount(options, BLOCK_CACHE_DATA_MISS));
  uint64_t int_num;
  ASSERT_TRUE(
      dbfull()->GetIntProperty("rocksdb.estimate-table-readers-mem", &int_num));
  ASSERT_EQ(int_num, 0U);
  std::string value;
  ReadOptions ropt;
  db_->KeyMayExist(ReadOptions(), handles_[1], "key", &value);
  ASSERT_EQ(1, TestGetTickerCount(options, BLOCK_CACHE_FILTER_MISS));
  ASSERT_EQ(1, TestGetTickerCount(options, BLOCK_CACHE_FILTER_HIT));
  db_->KeyMayExist(ReadOptions(), handles_[1], "key", &value);
  ASSERT_EQ(1, TestGetTickerCount(options, BLOCK_CACHE_FILTER_MISS));
  ASSERT_EQ(2, TestGetTickerCount(options, BLOCK_CACHE_FILTER_HIT));
  auto index_block_hit = TestGetTickerCount(options, BLOCK_CACHE_FILTER_HIT);
  value = Get(1, "key");
  ASSERT_EQ(1, TestGetTickerCount(options, BLOCK_CACHE_FILTER_MISS));
  ASSERT_EQ(index_block_hit + 1,
            TestGetTickerCount(options, BLOCK_CACHE_FILTER_HIT));
  value = Get(1, "key");
  ASSERT_EQ(1, TestGetTickerCount(options, BLOCK_CACHE_FILTER_MISS));
  ASSERT_EQ(index_block_hit + 2,
            TestGetTickerCount(options, BLOCK_CACHE_FILTER_HIT));
}
TEST_F(DBTest, ParanoidFileChecks) {
  Options options = CurrentOptions();
  options.create_if_missing = true;
  options.statistics = rocksdb::CreateDBStatistics();
  options.level0_file_num_compaction_trigger = 2;
  options.paranoid_file_checks = true;
  BlockBasedTableOptions table_options;
  table_options.cache_index_and_filter_blocks = false;
  table_options.filter_policy.reset(NewBloomFilterPolicy(20));
  options.table_factory.reset(new BlockBasedTableFactory(table_options));
  CreateAndReopenWithCF({"pikachu"}, options);
  ASSERT_OK(Put(1, "1_key", "val"));
  ASSERT_OK(Put(1, "9_key", "val"));
  ASSERT_OK(Flush(1));
  ASSERT_EQ(1,
            TestGetTickerCount(options, BLOCK_CACHE_ADD));
  ASSERT_OK(Put(1, "1_key2", "val2"));
  ASSERT_OK(Put(1, "9_key2", "val2"));
  ASSERT_OK(Flush(1));
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ(3,
            TestGetTickerCount(options, BLOCK_CACHE_ADD));
  ASSERT_OK(
      dbfull()->SetOptions(handles_[1], {{"paranoid_file_checks", "false"}}));
  ASSERT_OK(Put(1, "1_key3", "val3"));
  ASSERT_OK(Put(1, "9_key3", "val3"));
  ASSERT_OK(Flush(1));
  ASSERT_OK(Put(1, "1_key4", "val4"));
  ASSERT_OK(Put(1, "9_key4", "val4"));
  ASSERT_OK(Flush(1));
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ(3,
            TestGetTickerCount(options, BLOCK_CACHE_ADD));
}
TEST_F(DBTest, GetPropertiesOfAllTablesTest) {
  Options options = CurrentOptions();
  options.max_background_flushes = 0;
  Reopen(options);
  for (int table = 0; table < 4; ++table) {
    for (int i = 0; i < 10 + table; ++i) {
      db_->Put(WriteOptions(), ToString(table * 100 + i), "val");
    }
    db_->Flush(FlushOptions());
  }
  Reopen(options);
  VerifyTableProperties(db_, 10 + 11 + 12 + 13);
  Reopen(options);
  for (int i = 0; i < 2; ++i) {
    Get(ToString(i * 100 + 0));
  }
  VerifyTableProperties(db_, 10 + 11 + 12 + 13);
  Reopen(options);
  for (int i = 0; i < 4; ++i) {
    Get(ToString(i * 100 + 0));
  }
  VerifyTableProperties(db_, 10 + 11 + 12 + 13);
}
class CoutingUserTblPropCollector : public TablePropertiesCollector {
 public:
  const char* Name() const override { return "CoutingUserTblPropCollector"; }
  Status Finish(UserCollectedProperties* properties) override {
    std::string encoded;
    PutVarint32(&encoded, count_);
    *properties = UserCollectedProperties{
        {"CoutingUserTblPropCollector", message_}, {"Count", encoded},
    };
    return Status::OK();
  }
  Status AddUserKey(const Slice& user_key, const Slice& value, EntryType type,
                    SequenceNumber seq, uint64_t file_size) override {
    ++count_;
    return Status::OK();
  }
  virtual UserCollectedProperties GetReadableProperties() const override {
    return UserCollectedProperties{};
  }
 private:
  std::string message_ = "Rocksdb";
  uint32_t count_ = 0;
};
class CoutingUserTblPropCollectorFactory
    : public TablePropertiesCollectorFactory {
 public:
  virtual TablePropertiesCollector* CreateTablePropertiesCollector() override {
    return new CoutingUserTblPropCollector();
  }
  const char* Name() const override {
    return "CoutingUserTblPropCollectorFactory";
  }
};
TEST_F(DBTest, GetUserDefinedTablaProperties) {
  Options options = CurrentOptions();
  options.max_background_flushes = 0;
  options.table_properties_collector_factories.resize(1);
  options.table_properties_collector_factories[0] =
      std::make_shared<CoutingUserTblPropCollectorFactory>();
  Reopen(options);
  for (int table = 0; table < 4; ++table) {
    for (int i = 0; i < 10 + table; ++i) {
      db_->Put(WriteOptions(), ToString(table * 100 + i), "val");
    }
    db_->Flush(FlushOptions());
  }
  TablePropertiesCollection props;
  ASSERT_OK(db_->GetPropertiesOfAllTables(&props));
  ASSERT_EQ(4U, props.size());
  uint32_t sum = 0;
  for (const auto& item : props) {
    auto& user_collected = item.second->user_collected_properties;
    ASSERT_TRUE(user_collected.find("CoutingUserTblPropCollector") !=
                user_collected.end());
    ASSERT_EQ(user_collected.at("CoutingUserTblPropCollector"), "Rocksdb");
    ASSERT_TRUE(user_collected.find("Count") != user_collected.end());
    Slice key(user_collected.at("Count"));
    uint32_t count;
    ASSERT_TRUE(GetVarint32(&key, &count));
    sum += count;
  }
  ASSERT_EQ(10u + 11u + 12u + 13u, sum);
}
TEST_F(DBTest, LevelLimitReopen) {
  Options options = CurrentOptions();
  CreateAndReopenWithCF({"pikachu"}, options);
  const std::string value(1024 * 1024, ' ');
  int i = 0;
  while (NumTableFilesAtLevel(2, 1) == 0) {
    ASSERT_OK(Put(1, Key(i++), value));
  }
  options.num_levels = 1;
  options.max_bytes_for_level_multiplier_additional.resize(1, 1);
  Status s = TryReopenWithColumnFamilies({"default", "pikachu"}, options);
  ASSERT_EQ(s.IsInvalidArgument(), true);
  ASSERT_EQ(s.ToString(),
            "Invalid argument: db has more levels than options.num_levels");
  options.num_levels = 10;
  options.max_bytes_for_level_multiplier_additional.resize(10, 1);
  ASSERT_OK(TryReopenWithColumnFamilies({"default", "pikachu"}, options));
}
TEST_F(DBTest, PutDeleteGet) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    ASSERT_OK(Put(1, "foo", "v1"));
    ASSERT_EQ("v1", Get(1, "foo"));
    ASSERT_OK(Put(1, "foo", "v2"));
    ASSERT_EQ("v2", Get(1, "foo"));
    ASSERT_OK(Delete(1, "foo"));
    ASSERT_EQ("NOT_FOUND", Get(1, "foo"));
  } while (ChangeOptions());
}
TEST_F(DBTest, GetFromImmutableLayer) {
  do {
    Options options;
    options.env = env_;
    options.write_buffer_size = 100000;
    options = CurrentOptions(options);
    CreateAndReopenWithCF({"pikachu"}, options);
    ASSERT_OK(Put(1, "foo", "v1"));
    ASSERT_EQ("v1", Get(1, "foo"));
    env_->delay_sstable_sync_.store(true, std::memory_order_release);
    Put(1, "k1", std::string(100000, 'x'));
    Put(1, "k2", std::string(100000, 'y'));
    ASSERT_EQ("v1", Get(1, "foo"));
    ASSERT_EQ("NOT_FOUND", Get(0, "foo"));
    env_->delay_sstable_sync_.store(false, std::memory_order_release);
  } while (ChangeOptions());
}
TEST_F(DBTest, GetFromVersions) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    ASSERT_OK(Put(1, "foo", "v1"));
    ASSERT_OK(Flush(1));
    ASSERT_EQ("v1", Get(1, "foo"));
    ASSERT_EQ("NOT_FOUND", Get(0, "foo"));
  } while (ChangeOptions());
}
TEST_F(DBTest, GetSnapshot) {
  anon::OptionsOverride options_override;
  options_override.skip_policy = kSkipNoSnapshot;
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions(options_override));
    for (int i = 0; i < 2; i++) {
      std::string key = (i == 0) ? std::string("foo") : std::string(200, 'x');
      ASSERT_OK(Put(1, key, "v1"));
      const Snapshot* s1 = db_->GetSnapshot();
      if (option_config_ == kHashCuckoo) {
        ASSERT_TRUE(s1 == nullptr);
        break;
      }
      ASSERT_OK(Put(1, key, "v2"));
      ASSERT_EQ("v2", Get(1, key));
      ASSERT_EQ("v1", Get(1, key, s1));
      ASSERT_OK(Flush(1));
      ASSERT_EQ("v2", Get(1, key));
      ASSERT_EQ("v1", Get(1, key, s1));
      db_->ReleaseSnapshot(s1);
    }
  } while (ChangeOptions());
}
TEST_F(DBTest, GetLevel0Ordering) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    ASSERT_OK(Put(1, "bar", "b"));
    ASSERT_OK(Put(1, "foo", "v1"));
    ASSERT_OK(Flush(1));
    ASSERT_OK(Put(1, "foo", "v2"));
    ASSERT_OK(Flush(1));
    ASSERT_EQ("v2", Get(1, "foo"));
  } while (ChangeOptions());
}
TEST_F(DBTest, WrongLevel0Config) {
  Options options = CurrentOptions();
  Close();
  ASSERT_OK(DestroyDB(dbname_, options));
  options.level0_stop_writes_trigger = 1;
  options.level0_slowdown_writes_trigger = 2;
  options.level0_file_num_compaction_trigger = 3;
  ASSERT_OK(DB::Open(options, dbname_, &db_));
}
TEST_F(DBTest, GetOrderedByLevels) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    ASSERT_OK(Put(1, "foo", "v1"));
    Compact(1, "a", "z");
    ASSERT_EQ("v1", Get(1, "foo"));
    ASSERT_OK(Put(1, "foo", "v2"));
    ASSERT_EQ("v2", Get(1, "foo"));
    ASSERT_OK(Flush(1));
    ASSERT_EQ("v2", Get(1, "foo"));
  } while (ChangeOptions());
}
TEST_F(DBTest, GetPicksCorrectFile) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    ASSERT_OK(Put(1, "a", "va"));
    Compact(1, "a", "b");
    ASSERT_OK(Put(1, "x", "vx"));
    Compact(1, "x", "y");
    ASSERT_OK(Put(1, "f", "vf"));
    Compact(1, "f", "g");
    ASSERT_EQ("va", Get(1, "a"));
    ASSERT_EQ("vf", Get(1, "f"));
    ASSERT_EQ("vx", Get(1, "x"));
  } while (ChangeOptions());
}
TEST_F(DBTest, GetEncountersEmptyLevel) {
  do {
    Options options = CurrentOptions();
    options.max_background_flushes = 0;
    options.disableDataSync = true;
    CreateAndReopenWithCF({"pikachu"}, options);
    int compaction_count = 0;
    while (NumTableFilesAtLevel(0, 1) == 0 || NumTableFilesAtLevel(2, 1) == 0) {
      ASSERT_LE(compaction_count, 100) << "could not fill levels 0 and 2";
      compaction_count++;
      Put(1, "a", "begin");
      Put(1, "z", "end");
      ASSERT_OK(Flush(1));
    }
    dbfull()->TEST_CompactRange(1, nullptr, nullptr, handles_[1]);
    ASSERT_EQ(NumTableFilesAtLevel(0, 1), 1);
    ASSERT_EQ(NumTableFilesAtLevel(1, 1), 0);
    ASSERT_EQ(NumTableFilesAtLevel(2, 1), 1);
    for (int i = 0; i < 1000; i++) {
      ASSERT_EQ("NOT_FOUND", Get(1, "missing"));
    }
    env_->SleepForMicroseconds(1000000);
    ASSERT_EQ(NumTableFilesAtLevel(0, 1), 1);
  } while (ChangeOptions(kSkipUniversalCompaction | kSkipFIFOCompaction));
}
TEST_F(DBTest, KeyMayExist) {
  do {
    ReadOptions ropts;
    std::string value;
    anon::OptionsOverride options_override;
    options_override.filter_policy.reset(NewBloomFilterPolicy(20));
    Options options = CurrentOptions(options_override);
    options.statistics = rocksdb::CreateDBStatistics();
    CreateAndReopenWithCF({"pikachu"}, options);
    ASSERT_TRUE(!db_->KeyMayExist(ropts, handles_[1], "a", &value));
    ASSERT_OK(Put(1, "a", "b"));
    bool value_found = false;
    ASSERT_TRUE(
        db_->KeyMayExist(ropts, handles_[1], "a", &value, &value_found));
    ASSERT_TRUE(value_found);
    ASSERT_EQ("b", value);
    ASSERT_OK(Flush(1));
    value.clear();
    long numopen = TestGetTickerCount(options, NO_FILE_OPENS);
    long cache_added = TestGetTickerCount(options, BLOCK_CACHE_ADD);
    ASSERT_TRUE(
        db_->KeyMayExist(ropts, handles_[1], "a", &value, &value_found));
    ASSERT_TRUE(!value_found);
    ASSERT_EQ(numopen, TestGetTickerCount(options, NO_FILE_OPENS));
    ASSERT_EQ(cache_added, TestGetTickerCount(options, BLOCK_CACHE_ADD));
    ASSERT_OK(Delete(1, "a"));
    numopen = TestGetTickerCount(options, NO_FILE_OPENS);
    cache_added = TestGetTickerCount(options, BLOCK_CACHE_ADD);
    ASSERT_TRUE(!db_->KeyMayExist(ropts, handles_[1], "a", &value));
    ASSERT_EQ(numopen, TestGetTickerCount(options, NO_FILE_OPENS));
    ASSERT_EQ(cache_added, TestGetTickerCount(options, BLOCK_CACHE_ADD));
    ASSERT_OK(Flush(1));
    dbfull()->TEST_CompactRange(0, nullptr, nullptr, handles_[1],
                                true );
    numopen = TestGetTickerCount(options, NO_FILE_OPENS);
    cache_added = TestGetTickerCount(options, BLOCK_CACHE_ADD);
    ASSERT_TRUE(!db_->KeyMayExist(ropts, handles_[1], "a", &value));
    ASSERT_EQ(numopen, TestGetTickerCount(options, NO_FILE_OPENS));
    ASSERT_EQ(cache_added, TestGetTickerCount(options, BLOCK_CACHE_ADD));
    ASSERT_OK(Delete(1, "c"));
    numopen = TestGetTickerCount(options, NO_FILE_OPENS);
    cache_added = TestGetTickerCount(options, BLOCK_CACHE_ADD);
    ASSERT_TRUE(!db_->KeyMayExist(ropts, handles_[1], "c", &value));
    ASSERT_EQ(numopen, TestGetTickerCount(options, NO_FILE_OPENS));
    ASSERT_EQ(cache_added, TestGetTickerCount(options, BLOCK_CACHE_ADD));
  } while (
      ChangeOptions(kSkipPlainTable | kSkipHashIndex | kSkipFIFOCompaction));
}
TEST_F(DBTest, NonBlockingIteration) {
  do {
    ReadOptions non_blocking_opts, regular_opts;
    Options options = CurrentOptions();
    options.statistics = rocksdb::CreateDBStatistics();
    non_blocking_opts.read_tier = kBlockCacheTier;
    CreateAndReopenWithCF({"pikachu"}, options);
    ASSERT_OK(Put(1, "a", "b"));
    Iterator* iter = db_->NewIterator(non_blocking_opts, handles_[1]);
    int count = 0;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      ASSERT_OK(iter->status());
      count++;
    }
    ASSERT_EQ(count, 1);
    delete iter;
    ASSERT_OK(Flush(1));
    long numopen = TestGetTickerCount(options, NO_FILE_OPENS);
    long cache_added = TestGetTickerCount(options, BLOCK_CACHE_ADD);
    iter = db_->NewIterator(non_blocking_opts, handles_[1]);
    count = 0;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      count++;
    }
    ASSERT_EQ(count, 0);
    ASSERT_TRUE(iter->status().IsIncomplete());
    ASSERT_EQ(numopen, TestGetTickerCount(options, NO_FILE_OPENS));
    ASSERT_EQ(cache_added, TestGetTickerCount(options, BLOCK_CACHE_ADD));
    delete iter;
    ASSERT_EQ(Get(1, "a"), "b");
    numopen = TestGetTickerCount(options, NO_FILE_OPENS);
    cache_added = TestGetTickerCount(options, BLOCK_CACHE_ADD);
    iter = db_->NewIterator(non_blocking_opts, handles_[1]);
    count = 0;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      ASSERT_OK(iter->status());
      count++;
    }
    ASSERT_EQ(count, 1);
    ASSERT_EQ(numopen, TestGetTickerCount(options, NO_FILE_OPENS));
    ASSERT_EQ(cache_added, TestGetTickerCount(options, BLOCK_CACHE_ADD));
    delete iter;
  } while (ChangeOptions(kSkipPlainTable | kSkipNoSeekToLast | kSkipHashCuckoo |
                         kSkipMmapReads));
}
TEST_F(DBTest, ManagedNonBlockingIteration) {
  do {
    ReadOptions non_blocking_opts, regular_opts;
    Options options = CurrentOptions();
    options.statistics = rocksdb::CreateDBStatistics();
    non_blocking_opts.read_tier = kBlockCacheTier;
    non_blocking_opts.managed = true;
    CreateAndReopenWithCF({"pikachu"}, options);
    ASSERT_OK(Put(1, "a", "b"));
    Iterator* iter = db_->NewIterator(non_blocking_opts, handles_[1]);
    int count = 0;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      ASSERT_OK(iter->status());
      count++;
    }
    ASSERT_EQ(count, 1);
    delete iter;
    ASSERT_OK(Flush(1));
    int64_t numopen = TestGetTickerCount(options, NO_FILE_OPENS);
    int64_t cache_added = TestGetTickerCount(options, BLOCK_CACHE_ADD);
    iter = db_->NewIterator(non_blocking_opts, handles_[1]);
    count = 0;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      count++;
    }
    ASSERT_EQ(count, 0);
    ASSERT_TRUE(iter->status().IsIncomplete());
    ASSERT_EQ(numopen, TestGetTickerCount(options, NO_FILE_OPENS));
    ASSERT_EQ(cache_added, TestGetTickerCount(options, BLOCK_CACHE_ADD));
    delete iter;
    ASSERT_EQ(Get(1, "a"), "b");
    numopen = TestGetTickerCount(options, NO_FILE_OPENS);
    cache_added = TestGetTickerCount(options, BLOCK_CACHE_ADD);
    iter = db_->NewIterator(non_blocking_opts, handles_[1]);
    count = 0;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      ASSERT_OK(iter->status());
      count++;
    }
    ASSERT_EQ(count, 1);
    ASSERT_EQ(numopen, TestGetTickerCount(options, NO_FILE_OPENS));
    ASSERT_EQ(cache_added, TestGetTickerCount(options, BLOCK_CACHE_ADD));
    delete iter;
  } while (ChangeOptions(kSkipPlainTable | kSkipNoSeekToLast | kSkipHashCuckoo |
                         kSkipMmapReads));
}
TEST_F(DBTest, FilterDeletes) {
  do {
    anon::OptionsOverride options_override;
    options_override.filter_policy.reset(NewBloomFilterPolicy(20));
    Options options = CurrentOptions(options_override);
    options.filter_deletes = true;
    CreateAndReopenWithCF({"pikachu"}, options);
    WriteBatch batch;
    batch.Delete(handles_[1], "a");
    dbfull()->Write(WriteOptions(), &batch);
    ASSERT_EQ(AllEntriesFor("a", 1), "[ ]");
    batch.Clear();
    batch.Put(handles_[1], "a", "b");
    batch.Delete(handles_[1], "a");
    dbfull()->Write(WriteOptions(), &batch);
    ASSERT_EQ(Get(1, "a"), "NOT_FOUND");
    ASSERT_EQ(AllEntriesFor("a", 1), "[ DEL, b ]");
    batch.Clear();
    batch.Delete(handles_[1], "c");
    batch.Put(handles_[1], "c", "d");
    dbfull()->Write(WriteOptions(), &batch);
    ASSERT_EQ(Get(1, "c"), "d");
    ASSERT_EQ(AllEntriesFor("c", 1), "[ d ]");
    batch.Clear();
    ASSERT_OK(Flush(1));
    batch.Delete(handles_[1], "c");
    dbfull()->Write(WriteOptions(), &batch);
    ASSERT_EQ(AllEntriesFor("c", 1), "[ DEL, d ]");
    batch.Clear();
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, GetFilterByPrefixBloom) {
  Options options = last_options_;
  options.prefix_extractor.reset(NewFixedPrefixTransform(8));
  options.statistics = rocksdb::CreateDBStatistics();
  BlockBasedTableOptions bbto;
  bbto.filter_policy.reset(NewBloomFilterPolicy(10, false));
  bbto.whole_key_filtering = false;
  options.table_factory.reset(NewBlockBasedTableFactory(bbto));
  DestroyAndReopen(options);
  WriteOptions wo;
  ReadOptions ro;
  FlushOptions fo;
  fo.wait = true;
  std::string value;
  ASSERT_OK(dbfull()->Put(wo, "barbarbar", "foo"));
  ASSERT_OK(dbfull()->Put(wo, "barbarbar2", "foo2"));
  ASSERT_OK(dbfull()->Put(wo, "foofoofoo", "bar"));
  dbfull()->Flush(fo);
  ASSERT_EQ("foo", Get("barbarbar"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 0);
  ASSERT_EQ("foo2", Get("barbarbar2"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 0);
  ASSERT_EQ("NOT_FOUND", Get("barbarbar3"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 0);
  ASSERT_EQ("NOT_FOUND", Get("barfoofoo"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 1);
  ASSERT_EQ("NOT_FOUND", Get("foobarbar"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 2);
}
TEST_F(DBTest, WholeKeyFilterProp) {
  Options options = last_options_;
  options.prefix_extractor.reset(NewFixedPrefixTransform(3));
  options.statistics = rocksdb::CreateDBStatistics();
  BlockBasedTableOptions bbto;
  bbto.filter_policy.reset(NewBloomFilterPolicy(10, false));
  bbto.whole_key_filtering = false;
  options.table_factory.reset(NewBlockBasedTableFactory(bbto));
  DestroyAndReopen(options);
  WriteOptions wo;
  ReadOptions ro;
  FlushOptions fo;
  fo.wait = true;
  std::string value;
  ASSERT_OK(dbfull()->Put(wo, "foobar", "foo"));
  ASSERT_OK(dbfull()->Put(wo, "aaa", ""));
  ASSERT_OK(dbfull()->Put(wo, "zzz", ""));
  dbfull()->Flush(fo);
  Reopen(options);
  ASSERT_EQ("NOT_FOUND", Get("foo"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 0);
  ASSERT_EQ("NOT_FOUND", Get("bar"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 1);
  ASSERT_EQ("foo", Get("foobar"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 1);
  bbto.whole_key_filtering = true;
  options.table_factory.reset(NewBlockBasedTableFactory(bbto));
  options.prefix_extractor.reset();
  Reopen(options);
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 1);
  ASSERT_EQ("NOT_FOUND", Get("foo"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 1);
  ASSERT_EQ("NOT_FOUND", Get("bar"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 1);
  ASSERT_EQ("foo", Get("foobar"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 1);
  ASSERT_OK(dbfull()->Put(wo, "foobar", "foo"));
  ASSERT_OK(dbfull()->Put(wo, "aaa", ""));
  ASSERT_OK(dbfull()->Put(wo, "zzz", ""));
  db_->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  options.prefix_extractor.reset(NewFixedPrefixTransform(3));
  bbto.whole_key_filtering = false;
  options.table_factory.reset(NewBlockBasedTableFactory(bbto));
  Reopen(options);
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 1);
  ASSERT_EQ("NOT_FOUND", Get("foo"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 1);
  ASSERT_EQ("NOT_FOUND", Get("bar"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 1);
  ASSERT_EQ("foo", Get("foobar"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 1);
  ASSERT_OK(dbfull()->Put(wo, "foobar", "foo"));
  ASSERT_OK(dbfull()->Put(wo, "aaa", ""));
  ASSERT_OK(dbfull()->Put(wo, "zzz", ""));
  db_->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  options.prefix_extractor.reset();
  bbto.whole_key_filtering = true;
  options.table_factory.reset(NewBlockBasedTableFactory(bbto));
  Reopen(options);
  ASSERT_OK(dbfull()->Put(wo, "barfoo", "bar"));
  ASSERT_OK(dbfull()->Put(wo, "aaa", ""));
  ASSERT_OK(dbfull()->Put(wo, "zzz", ""));
  Flush();
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 1);
  ASSERT_EQ("NOT_FOUND", Get("foo"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 2);
  ASSERT_EQ("NOT_FOUND", Get("bar"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 3);
  ASSERT_EQ("foo", Get("foobar"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 4);
  ASSERT_EQ("bar", Get("barfoo"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 4);
  Reopen(options);
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 4);
  ASSERT_EQ("NOT_FOUND", Get("foo"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 5);
  ASSERT_EQ("NOT_FOUND", Get("bar"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 6);
  ASSERT_EQ("foo", Get("foobar"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 7);
  ASSERT_EQ("bar", Get("barfoo"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 7);
  options.prefix_extractor.reset(NewFixedPrefixTransform(3));
  bbto.whole_key_filtering = true;
  options.table_factory.reset(NewBlockBasedTableFactory(bbto));
  Reopen(options);
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 7);
  ASSERT_EQ("NOT_FOUND", Get("foo"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 8);
  ASSERT_EQ("NOT_FOUND", Get("bar"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 10);
  ASSERT_EQ("foo", Get("foobar"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 11);
  ASSERT_EQ("bar", Get("barfoo"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 11);
  options.prefix_extractor.reset(NewFixedPrefixTransform(3));
  bbto.whole_key_filtering = false;
  options.table_factory.reset(NewBlockBasedTableFactory(bbto));
  Reopen(options);
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 11);
  ASSERT_EQ("NOT_FOUND", Get("foo"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 11);
  ASSERT_EQ("NOT_FOUND", Get("bar"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 12);
  ASSERT_EQ("foo", Get("foobar"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 12);
  ASSERT_EQ("bar", Get("barfoo"));
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 12);
}
TEST_F(DBTest, IterSeekBeforePrev) {
  ASSERT_OK(Put("a", "b"));
  ASSERT_OK(Put("c", "d"));
  dbfull()->Flush(FlushOptions());
  ASSERT_OK(Put("0", "f"));
  ASSERT_OK(Put("1", "h"));
  dbfull()->Flush(FlushOptions());
  ASSERT_OK(Put("2", "j"));
  auto iter = db_->NewIterator(ReadOptions());
  iter->Seek(Slice("c"));
  iter->Prev();
  iter->Seek(Slice("a"));
  iter->Prev();
  delete iter;
}
namespace {
std::string MakeLongKey(size_t length, char c) {
  return std::string(length, c);
}
}
TEST_F(DBTest, IterLongKeys) {
  ASSERT_OK(Put(MakeLongKey(20, 0), "0"));
  ASSERT_OK(Put(MakeLongKey(32, 2), "2"));
  ASSERT_OK(Put("a", "b"));
  dbfull()->Flush(FlushOptions());
  ASSERT_OK(Put(MakeLongKey(50, 1), "1"));
  ASSERT_OK(Put(MakeLongKey(127, 3), "3"));
  ASSERT_OK(Put(MakeLongKey(64, 4), "4"));
  auto iter = db_->NewIterator(ReadOptions());
  iter->Seek(MakeLongKey(20, 0));
  ASSERT_EQ(IterStatus(iter), MakeLongKey(20, 0) + "->0");
  iter->Next();
  ASSERT_EQ(IterStatus(iter), MakeLongKey(50, 1) + "->1");
  iter->Next();
  ASSERT_EQ(IterStatus(iter), MakeLongKey(32, 2) + "->2");
  iter->Next();
  ASSERT_EQ(IterStatus(iter), MakeLongKey(127, 3) + "->3");
  iter->Next();
  ASSERT_EQ(IterStatus(iter), MakeLongKey(64, 4) + "->4");
  delete iter;
  iter = db_->NewIterator(ReadOptions());
  iter->Seek(MakeLongKey(50, 1));
  ASSERT_EQ(IterStatus(iter), MakeLongKey(50, 1) + "->1");
  iter->Next();
  ASSERT_EQ(IterStatus(iter), MakeLongKey(32, 2) + "->2");
  iter->Next();
  ASSERT_EQ(IterStatus(iter), MakeLongKey(127, 3) + "->3");
  delete iter;
}
TEST_F(DBTest, IterNextWithNewerSeq) {
  ASSERT_OK(Put("0", "0"));
  dbfull()->Flush(FlushOptions());
  ASSERT_OK(Put("a", "b"));
  ASSERT_OK(Put("c", "d"));
  ASSERT_OK(Put("d", "e"));
  auto iter = db_->NewIterator(ReadOptions());
  for (uint64_t i = 0; i < last_options_.max_sequential_skip_in_iterations + 1;
       i++) {
    ASSERT_OK(Put("b", "f"));
  }
  iter->Seek(Slice("a"));
  ASSERT_EQ(IterStatus(iter), "a->b");
  iter->Next();
  ASSERT_EQ(IterStatus(iter), "c->d");
  delete iter;
}
TEST_F(DBTest, IterPrevWithNewerSeq) {
  ASSERT_OK(Put("0", "0"));
  dbfull()->Flush(FlushOptions());
  ASSERT_OK(Put("a", "b"));
  ASSERT_OK(Put("c", "d"));
  ASSERT_OK(Put("d", "e"));
  auto iter = db_->NewIterator(ReadOptions());
  for (uint64_t i = 0; i < last_options_.max_sequential_skip_in_iterations + 1;
       i++) {
    ASSERT_OK(Put("b", "f"));
  }
  iter->Seek(Slice("d"));
  ASSERT_EQ(IterStatus(iter), "d->e");
  iter->Prev();
  ASSERT_EQ(IterStatus(iter), "c->d");
  iter->Prev();
  ASSERT_EQ(IterStatus(iter), "a->b");
  iter->Prev();
  delete iter;
}
TEST_F(DBTest, IterPrevWithNewerSeq2) {
  ASSERT_OK(Put("0", "0"));
  dbfull()->Flush(FlushOptions());
  ASSERT_OK(Put("a", "b"));
  ASSERT_OK(Put("c", "d"));
  ASSERT_OK(Put("d", "e"));
  auto iter = db_->NewIterator(ReadOptions());
  iter->Seek(Slice("c"));
  ASSERT_EQ(IterStatus(iter), "c->d");
  for (uint64_t i = 0; i < last_options_.max_sequential_skip_in_iterations + 1;
      i++) {
    ASSERT_OK(Put("b", "f"));
  }
  iter->Prev();
  ASSERT_EQ(IterStatus(iter), "a->b");
  iter->Prev();
  delete iter;
}
TEST_F(DBTest, IterEmpty) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    Iterator* iter = db_->NewIterator(ReadOptions(), handles_[1]);
    iter->SeekToFirst();
    ASSERT_EQ(IterStatus(iter), "(invalid)");
    iter->SeekToLast();
    ASSERT_EQ(IterStatus(iter), "(invalid)");
    iter->Seek("foo");
    ASSERT_EQ(IterStatus(iter), "(invalid)");
    delete iter;
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, IterSingle) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    ASSERT_OK(Put(1, "a", "va"));
    Iterator* iter = db_->NewIterator(ReadOptions(), handles_[1]);
    iter->SeekToFirst();
    ASSERT_EQ(IterStatus(iter), "a->va");
    iter->Next();
    ASSERT_EQ(IterStatus(iter), "(invalid)");
    iter->SeekToFirst();
    ASSERT_EQ(IterStatus(iter), "a->va");
    iter->Prev();
    ASSERT_EQ(IterStatus(iter), "(invalid)");
    iter->SeekToLast();
    ASSERT_EQ(IterStatus(iter), "a->va");
    iter->Next();
    ASSERT_EQ(IterStatus(iter), "(invalid)");
    iter->SeekToLast();
    ASSERT_EQ(IterStatus(iter), "a->va");
    iter->Prev();
    ASSERT_EQ(IterStatus(iter), "(invalid)");
    iter->Seek("");
    ASSERT_EQ(IterStatus(iter), "a->va");
    iter->Next();
    ASSERT_EQ(IterStatus(iter), "(invalid)");
    iter->Seek("a");
    ASSERT_EQ(IterStatus(iter), "a->va");
    iter->Next();
    ASSERT_EQ(IterStatus(iter), "(invalid)");
    iter->Seek("b");
    ASSERT_EQ(IterStatus(iter), "(invalid)");
    delete iter;
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, IterMulti) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    ASSERT_OK(Put(1, "a", "va"));
    ASSERT_OK(Put(1, "b", "vb"));
    ASSERT_OK(Put(1, "c", "vc"));
    Iterator* iter = db_->NewIterator(ReadOptions(), handles_[1]);
    iter->SeekToFirst();
    ASSERT_EQ(IterStatus(iter), "a->va");
    iter->Next();
    ASSERT_EQ(IterStatus(iter), "b->vb");
    iter->Next();
    ASSERT_EQ(IterStatus(iter), "c->vc");
    iter->Next();
    ASSERT_EQ(IterStatus(iter), "(invalid)");
    iter->SeekToFirst();
    ASSERT_EQ(IterStatus(iter), "a->va");
    iter->Prev();
    ASSERT_EQ(IterStatus(iter), "(invalid)");
    iter->SeekToLast();
    ASSERT_EQ(IterStatus(iter), "c->vc");
    iter->Prev();
    ASSERT_EQ(IterStatus(iter), "b->vb");
    iter->Prev();
    ASSERT_EQ(IterStatus(iter), "a->va");
    iter->Prev();
    ASSERT_EQ(IterStatus(iter), "(invalid)");
    iter->SeekToLast();
    ASSERT_EQ(IterStatus(iter), "c->vc");
    iter->Next();
    ASSERT_EQ(IterStatus(iter), "(invalid)");
    iter->Seek("");
    ASSERT_EQ(IterStatus(iter), "a->va");
    iter->Seek("a");
    ASSERT_EQ(IterStatus(iter), "a->va");
    iter->Seek("ax");
    ASSERT_EQ(IterStatus(iter), "b->vb");
    iter->Seek("b");
    ASSERT_EQ(IterStatus(iter), "b->vb");
    iter->Seek("z");
    ASSERT_EQ(IterStatus(iter), "(invalid)");
    iter->SeekToLast();
    iter->Prev();
    iter->Prev();
    iter->Next();
    ASSERT_EQ(IterStatus(iter), "b->vb");
    iter->SeekToFirst();
    iter->Next();
    iter->Next();
    iter->Prev();
    ASSERT_EQ(IterStatus(iter), "b->vb");
    ASSERT_OK(Put(1, "a", "va2"));
    ASSERT_OK(Put(1, "a2", "va3"));
    ASSERT_OK(Put(1, "b", "vb2"));
    ASSERT_OK(Put(1, "c", "vc2"));
    ASSERT_OK(Delete(1, "b"));
    iter->SeekToFirst();
    ASSERT_EQ(IterStatus(iter), "a->va");
    iter->Next();
    ASSERT_EQ(IterStatus(iter), "b->vb");
    iter->Next();
    ASSERT_EQ(IterStatus(iter), "c->vc");
    iter->Next();
    ASSERT_EQ(IterStatus(iter), "(invalid)");
    iter->SeekToLast();
    ASSERT_EQ(IterStatus(iter), "c->vc");
    iter->Prev();
    ASSERT_EQ(IterStatus(iter), "b->vb");
    iter->Prev();
    ASSERT_EQ(IterStatus(iter), "a->va");
    iter->Prev();
    ASSERT_EQ(IterStatus(iter), "(invalid)");
    delete iter;
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, IterReseek) {
  anon::OptionsOverride options_override;
  options_override.skip_policy = kSkipNoSnapshot;
  Options options = CurrentOptions(options_override);
  options.max_sequential_skip_in_iterations = 3;
  options.create_if_missing = true;
  options.statistics = rocksdb::CreateDBStatistics();
  DestroyAndReopen(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  ASSERT_OK(Put(1, "a", "one"));
  ASSERT_OK(Put(1, "a", "two"));
  ASSERT_OK(Put(1, "b", "bone"));
  Iterator* iter = db_->NewIterator(ReadOptions(), handles_[1]);
  iter->SeekToFirst();
  ASSERT_EQ(TestGetTickerCount(options, NUMBER_OF_RESEEKS_IN_ITERATION), 0);
  ASSERT_EQ(IterStatus(iter), "a->two");
  iter->Next();
  ASSERT_EQ(TestGetTickerCount(options, NUMBER_OF_RESEEKS_IN_ITERATION), 0);
  ASSERT_EQ(IterStatus(iter), "b->bone");
  delete iter;
  ASSERT_OK(Put(1, "a", "three"));
  iter = db_->NewIterator(ReadOptions(), handles_[1]);
  iter->SeekToFirst();
  ASSERT_EQ(IterStatus(iter), "a->three");
  iter->Next();
  ASSERT_EQ(TestGetTickerCount(options, NUMBER_OF_RESEEKS_IN_ITERATION), 0);
  ASSERT_EQ(IterStatus(iter), "b->bone");
  delete iter;
  ASSERT_OK(Put(1, "a", "four"));
  iter = db_->NewIterator(ReadOptions(), handles_[1]);
  iter->SeekToFirst();
  ASSERT_EQ(IterStatus(iter), "a->four");
  ASSERT_EQ(TestGetTickerCount(options, NUMBER_OF_RESEEKS_IN_ITERATION), 0);
  iter->Next();
  ASSERT_EQ(TestGetTickerCount(options, NUMBER_OF_RESEEKS_IN_ITERATION), 1);
  ASSERT_EQ(IterStatus(iter), "b->bone");
  delete iter;
  int num_reseeks =
      (int)TestGetTickerCount(options, NUMBER_OF_RESEEKS_IN_ITERATION);
  ASSERT_OK(Put(1, "b", "btwo"));
  iter = db_->NewIterator(ReadOptions(), handles_[1]);
  iter->SeekToLast();
  ASSERT_EQ(IterStatus(iter), "b->btwo");
  ASSERT_EQ(TestGetTickerCount(options, NUMBER_OF_RESEEKS_IN_ITERATION),
            num_reseeks);
  iter->Prev();
  ASSERT_EQ(TestGetTickerCount(options, NUMBER_OF_RESEEKS_IN_ITERATION),
            num_reseeks + 1);
  ASSERT_EQ(IterStatus(iter), "a->four");
  delete iter;
  ASSERT_OK(Put(1, "b", "bthree"));
  ASSERT_OK(Put(1, "b", "bfour"));
  iter = db_->NewIterator(ReadOptions(), handles_[1]);
  iter->SeekToLast();
  ASSERT_EQ(IterStatus(iter), "b->bfour");
  ASSERT_EQ(TestGetTickerCount(options, NUMBER_OF_RESEEKS_IN_ITERATION),
            num_reseeks + 2);
  iter->Prev();
  ASSERT_EQ(TestGetTickerCount(options, NUMBER_OF_RESEEKS_IN_ITERATION),
            num_reseeks + 3);
  ASSERT_EQ(IterStatus(iter), "a->four");
  delete iter;
}
TEST_F(DBTest, IterSmallAndLargeMix) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    ASSERT_OK(Put(1, "a", "va"));
    ASSERT_OK(Put(1, "b", std::string(100000, 'b')));
    ASSERT_OK(Put(1, "c", "vc"));
    ASSERT_OK(Put(1, "d", std::string(100000, 'd')));
    ASSERT_OK(Put(1, "e", std::string(100000, 'e')));
    Iterator* iter = db_->NewIterator(ReadOptions(), handles_[1]);
    iter->SeekToFirst();
    ASSERT_EQ(IterStatus(iter), "a->va");
    iter->Next();
    ASSERT_EQ(IterStatus(iter), "b->" + std::string(100000, 'b'));
    iter->Next();
    ASSERT_EQ(IterStatus(iter), "c->vc");
    iter->Next();
    ASSERT_EQ(IterStatus(iter), "d->" + std::string(100000, 'd'));
    iter->Next();
    ASSERT_EQ(IterStatus(iter), "e->" + std::string(100000, 'e'));
    iter->Next();
    ASSERT_EQ(IterStatus(iter), "(invalid)");
    iter->SeekToLast();
    ASSERT_EQ(IterStatus(iter), "e->" + std::string(100000, 'e'));
    iter->Prev();
    ASSERT_EQ(IterStatus(iter), "d->" + std::string(100000, 'd'));
    iter->Prev();
    ASSERT_EQ(IterStatus(iter), "c->vc");
    iter->Prev();
    ASSERT_EQ(IterStatus(iter), "b->" + std::string(100000, 'b'));
    iter->Prev();
    ASSERT_EQ(IterStatus(iter), "a->va");
    iter->Prev();
    ASSERT_EQ(IterStatus(iter), "(invalid)");
    delete iter;
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, IterMultiWithDelete) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    ASSERT_OK(Put(1, "ka", "va"));
    ASSERT_OK(Put(1, "kb", "vb"));
    ASSERT_OK(Put(1, "kc", "vc"));
    ASSERT_OK(Delete(1, "kb"));
    ASSERT_EQ("NOT_FOUND", Get(1, "kb"));
    Iterator* iter = db_->NewIterator(ReadOptions(), handles_[1]);
    iter->Seek("kc");
    ASSERT_EQ(IterStatus(iter), "kc->vc");
    if (!CurrentOptions().merge_operator) {
      if (kPlainTableAllBytesPrefix != option_config_&&
          kBlockBasedTableWithWholeKeyHashIndex != option_config_ &&
          kHashLinkList != option_config_) {
        iter->Prev();
        ASSERT_EQ(IterStatus(iter), "ka->va");
      }
    }
    delete iter;
  } while (ChangeOptions());
}
TEST_F(DBTest, IterPrevMaxSkip) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    for (int i = 0; i < 2; i++) {
      ASSERT_OK(Put(1, "key1", "v1"));
      ASSERT_OK(Put(1, "key2", "v2"));
      ASSERT_OK(Put(1, "key3", "v3"));
      ASSERT_OK(Put(1, "key4", "v4"));
      ASSERT_OK(Put(1, "key5", "v5"));
    }
    VerifyIterLast("key5->v5", 1);
    ASSERT_OK(Delete(1, "key5"));
    VerifyIterLast("key4->v4", 1);
    ASSERT_OK(Delete(1, "key4"));
    VerifyIterLast("key3->v3", 1);
    ASSERT_OK(Delete(1, "key3"));
    VerifyIterLast("key2->v2", 1);
    ASSERT_OK(Delete(1, "key2"));
    VerifyIterLast("key1->v1", 1);
    ASSERT_OK(Delete(1, "key1"));
    VerifyIterLast("(invalid)", 1);
  } while (ChangeOptions(kSkipMergePut | kSkipNoSeekToLast));
}
TEST_F(DBTest, IterWithSnapshot) {
  anon::OptionsOverride options_override;
  options_override.skip_policy = kSkipNoSnapshot;
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions(options_override));
    ASSERT_OK(Put(1, "key1", "val1"));
    ASSERT_OK(Put(1, "key2", "val2"));
    ASSERT_OK(Put(1, "key3", "val3"));
    ASSERT_OK(Put(1, "key4", "val4"));
    ASSERT_OK(Put(1, "key5", "val5"));
    const Snapshot *snapshot = db_->GetSnapshot();
    ReadOptions options;
    options.snapshot = snapshot;
    Iterator* iter = db_->NewIterator(options, handles_[1]);
    ASSERT_OK(Put(1, "key100", "val100"));
    ASSERT_OK(Put(1, "key101", "val101"));
    iter->Seek("key5");
    ASSERT_EQ(IterStatus(iter), "key5->val5");
    if (!CurrentOptions().merge_operator) {
      if (kPlainTableAllBytesPrefix != option_config_&&
        kBlockBasedTableWithWholeKeyHashIndex != option_config_ &&
        kHashLinkList != option_config_) {
        iter->Prev();
        ASSERT_EQ(IterStatus(iter), "key4->val4");
        iter->Prev();
        ASSERT_EQ(IterStatus(iter), "key3->val3");
        iter->Next();
        ASSERT_EQ(IterStatus(iter), "key4->val4");
        iter->Next();
        ASSERT_EQ(IterStatus(iter), "key5->val5");
      }
      iter->Next();
      ASSERT_TRUE(!iter->Valid());
    }
    db_->ReleaseSnapshot(snapshot);
    delete iter;
  } while (ChangeOptions(kSkipHashCuckoo));
}
TEST_F(DBTest, Recover) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    ASSERT_OK(Put(1, "foo", "v1"));
    ASSERT_OK(Put(1, "baz", "v5"));
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ASSERT_EQ("v1", Get(1, "foo"));
    ASSERT_EQ("v1", Get(1, "foo"));
    ASSERT_EQ("v5", Get(1, "baz"));
    ASSERT_OK(Put(1, "bar", "v2"));
    ASSERT_OK(Put(1, "foo", "v3"));
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ASSERT_EQ("v3", Get(1, "foo"));
    ASSERT_OK(Put(1, "foo", "v4"));
    ASSERT_EQ("v4", Get(1, "foo"));
    ASSERT_EQ("v2", Get(1, "bar"));
    ASSERT_EQ("v5", Get(1, "baz"));
  } while (ChangeOptions());
}
TEST_F(DBTest, RecoverWithTableHandle) {
  do {
    Options options;
    options.create_if_missing = true;
    options.write_buffer_size = 100;
    options.disable_auto_compactions = true;
    options = CurrentOptions(options);
    DestroyAndReopen(options);
    CreateAndReopenWithCF({"pikachu"}, options);
    ASSERT_OK(Put(1, "foo", "v1"));
    ASSERT_OK(Put(1, "bar", "v2"));
    ASSERT_OK(Flush(1));
    ASSERT_OK(Put(1, "foo", "v3"));
    ASSERT_OK(Put(1, "bar", "v4"));
    ASSERT_OK(Flush(1));
    ASSERT_OK(Put(1, "big", std::string(100, 'a')));
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    std::vector<std::vector<FileMetaData>> files;
    dbfull()->TEST_GetFilesMetaData(handles_[1], &files);
    int total_files = 0;
    for (const auto& level : files) {
      total_files += level.size();
    }
    ASSERT_EQ(total_files, 3);
    for (const auto& level : files) {
      for (const auto& file : level) {
        if (kInfiniteMaxOpenFiles == option_config_) {
          ASSERT_TRUE(file.table_reader_handle != nullptr);
        } else {
          ASSERT_TRUE(file.table_reader_handle == nullptr);
        }
      }
    }
  } while (ChangeOptions());
}
TEST_F(DBTest, IgnoreRecoveredLog) {
  std::string backup_logs = dbname_ + "/backup_logs";
  env_->CreateDirIfMissing(backup_logs);
  std::vector<std::string> old_files;
  env_->GetChildren(backup_logs, &old_files);
  for (auto& file : old_files) {
    if (file != "." && file != "..") {
      env_->DeleteFile(backup_logs + "/" + file);
    }
  }
  do {
    Options options = CurrentOptions();
    options.create_if_missing = true;
    options.merge_operator = MergeOperators::CreateUInt64AddOperator();
    options.wal_dir = dbname_ + "/logs";
    DestroyAndReopen(options);
    std::string one, two;
    PutFixed64(&one, 1);
    PutFixed64(&two, 2);
    ASSERT_OK(db_->Merge(WriteOptions(), Slice("foo"), Slice(one)));
    ASSERT_OK(db_->Merge(WriteOptions(), Slice("foo"), Slice(one)));
    ASSERT_OK(db_->Merge(WriteOptions(), Slice("bar"), Slice(one)));
    std::vector<std::string> logs;
    env_->GetChildren(options.wal_dir, &logs);
    for (auto& log : logs) {
      if (log != ".." && log != ".") {
        CopyFile(options.wal_dir + "/" + log, backup_logs + "/" + log);
      }
    }
    Reopen(options);
    ASSERT_EQ(two, Get("foo"));
    ASSERT_EQ(one, Get("bar"));
    Close();
    for (auto& log : logs) {
      if (log != ".." && log != ".") {
        CopyFile(backup_logs + "/" + log, options.wal_dir + "/" + log);
      }
    }
    Reopen(options);
    ASSERT_EQ(two, Get("foo"));
    ASSERT_EQ(one, Get("bar"));
    Close();
    Destroy(options);
    Reopen(options);
    Close();
    env_->CreateDirIfMissing(options.wal_dir);
    for (auto& log : logs) {
      if (log != ".." && log != ".") {
        CopyFile(backup_logs + "/" + log, options.wal_dir + "/" + log);
      }
    }
    Reopen(options);
    ASSERT_EQ(two, Get("foo"));
    ASSERT_EQ(one, Get("bar"));
    Destroy(options);
    env_->CreateDirIfMissing(options.wal_dir);
    for (auto& log : logs) {
      if (log != ".." && log != ".") {
        CopyFile(backup_logs + "/" + log, options.wal_dir + "/" + log);
        env_->DeleteFile(backup_logs + "/" + log);
      }
    }
    Status s = TryReopen(options);
    ASSERT_TRUE(!s.ok());
  } while (ChangeOptions(kSkipHashCuckoo));
}
TEST_F(DBTest, RollLog) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    ASSERT_OK(Put(1, "foo", "v1"));
    ASSERT_OK(Put(1, "baz", "v5"));
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    for (int i = 0; i < 10; i++) {
      ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    }
    ASSERT_OK(Put(1, "foo", "v4"));
    for (int i = 0; i < 10; i++) {
      ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    }
  } while (ChangeOptions());
}
TEST_F(DBTest, WAL) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    WriteOptions writeOpt = WriteOptions();
    writeOpt.disableWAL = true;
    ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "foo", "v1"));
    ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "bar", "v1"));
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ASSERT_EQ("v1", Get(1, "foo"));
    ASSERT_EQ("v1", Get(1, "bar"));
    writeOpt.disableWAL = false;
    ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "bar", "v2"));
    writeOpt.disableWAL = true;
    ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "foo", "v2"));
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ASSERT_EQ("v2", Get(1, "bar"));
    ASSERT_EQ("v2", Get(1, "foo"));
    writeOpt.disableWAL = true;
    ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "bar", "v3"));
    writeOpt.disableWAL = false;
    ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "foo", "v3"));
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ASSERT_EQ("v3", Get(1, "foo"));
    ASSERT_EQ("v3", Get(1, "bar"));
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, CheckLock) {
  do {
    DB* localdb;
    Options options = CurrentOptions();
    ASSERT_OK(TryReopen(options));
    ASSERT_TRUE(!(DB::Open(options, dbname_, &localdb)).ok());
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, FlushMultipleMemtable) {
  do {
    Options options = CurrentOptions();
    WriteOptions writeOpt = WriteOptions();
    writeOpt.disableWAL = true;
    options.max_write_buffer_number = 4;
    options.min_write_buffer_number_to_merge = 3;
    options.max_write_buffer_number_to_maintain = -1;
    CreateAndReopenWithCF({"pikachu"}, options);
    ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "foo", "v1"));
    ASSERT_OK(Flush(1));
    ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "bar", "v1"));
    ASSERT_EQ("v1", Get(1, "foo"));
    ASSERT_EQ("v1", Get(1, "bar"));
    ASSERT_OK(Flush(1));
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, NumImmutableMemTable) {
  do {
    Options options = CurrentOptions();
    WriteOptions writeOpt = WriteOptions();
    writeOpt.disableWAL = true;
    options.max_write_buffer_number = 4;
    options.min_write_buffer_number_to_merge = 3;
    options.max_write_buffer_number_to_maintain = 0;
    options.write_buffer_size = 1000000;
    CreateAndReopenWithCF({"pikachu"}, options);
    std::string big_value(1000000 * 2, 'x');
    std::string num;
    SetPerfLevel(kEnableTime);;
    ASSERT_TRUE(GetPerfLevel() == kEnableTime);
    ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "k1", big_value));
    ASSERT_TRUE(dbfull()->GetProperty(handles_[1],
                                      "rocksdb.num-immutable-mem-table", &num));
    ASSERT_EQ(num, "0");
    ASSERT_TRUE(dbfull()->GetProperty(
        handles_[1], "rocksdb.num-entries-active-mem-table", &num));
    ASSERT_EQ(num, "1");
    perf_context.Reset();
    Get(1, "k1");
    ASSERT_EQ(1, (int) perf_context.get_from_memtable_count);
    ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "k2", big_value));
    ASSERT_TRUE(dbfull()->GetProperty(handles_[1],
                                      "rocksdb.num-immutable-mem-table", &num));
    ASSERT_EQ(num, "1");
    ASSERT_TRUE(dbfull()->GetProperty(
        handles_[1], "rocksdb.num-entries-active-mem-table", &num));
    ASSERT_EQ(num, "1");
    ASSERT_TRUE(dbfull()->GetProperty(
        handles_[1], "rocksdb.num-entries-imm-mem-tables", &num));
    ASSERT_EQ(num, "1");
    perf_context.Reset();
    Get(1, "k1");
    ASSERT_EQ(2, (int) perf_context.get_from_memtable_count);
    perf_context.Reset();
    Get(1, "k2");
    ASSERT_EQ(1, (int) perf_context.get_from_memtable_count);
    ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "k3", big_value));
    ASSERT_TRUE(dbfull()->GetProperty(
        handles_[1], "rocksdb.cur-size-active-mem-table", &num));
    ASSERT_TRUE(dbfull()->GetProperty(handles_[1],
                                      "rocksdb.num-immutable-mem-table", &num));
    ASSERT_EQ(num, "2");
    ASSERT_TRUE(dbfull()->GetProperty(
        handles_[1], "rocksdb.num-entries-active-mem-table", &num));
    ASSERT_EQ(num, "1");
    ASSERT_TRUE(dbfull()->GetProperty(
        handles_[1], "rocksdb.num-entries-imm-mem-tables", &num));
    ASSERT_EQ(num, "2");
    perf_context.Reset();
    Get(1, "k2");
    ASSERT_EQ(2, (int) perf_context.get_from_memtable_count);
    perf_context.Reset();
    Get(1, "k3");
    ASSERT_EQ(1, (int) perf_context.get_from_memtable_count);
    perf_context.Reset();
    Get(1, "k1");
    ASSERT_EQ(3, (int) perf_context.get_from_memtable_count);
    ASSERT_OK(Flush(1));
    ASSERT_TRUE(dbfull()->GetProperty(handles_[1],
                                      "rocksdb.num-immutable-mem-table", &num));
    ASSERT_EQ(num, "0");
    ASSERT_TRUE(dbfull()->GetProperty(
        handles_[1], "rocksdb.cur-size-active-mem-table", &num));
    ASSERT_EQ(num, "200");
    uint64_t int_num;
    uint64_t base_total_size;
    ASSERT_TRUE(dbfull()->GetIntProperty(
        handles_[1], "rocksdb.estimate-num-keys", &base_total_size));
    ASSERT_OK(dbfull()->Delete(writeOpt, handles_[1], "k2"));
    ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "k3", ""));
    ASSERT_OK(dbfull()->Delete(writeOpt, handles_[1], "k3"));
    ASSERT_TRUE(dbfull()->GetIntProperty(
        handles_[1], "rocksdb.num-deletes-active-mem-table", &int_num));
    ASSERT_EQ(int_num, 2U);
    ASSERT_TRUE(dbfull()->GetIntProperty(
        handles_[1], "rocksdb.num-entries-active-mem-table", &int_num));
    ASSERT_EQ(int_num, 3U);
    ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "k2", big_value));
    ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "k2", big_value));
    ASSERT_TRUE(dbfull()->GetIntProperty(
        handles_[1], "rocksdb.num-entries-imm-mem-tables", &int_num));
    ASSERT_EQ(int_num, 4U);
    ASSERT_TRUE(dbfull()->GetIntProperty(
        handles_[1], "rocksdb.num-deletes-imm-mem-tables", &int_num));
    ASSERT_EQ(int_num, 2U);
    ASSERT_TRUE(dbfull()->GetIntProperty(
        handles_[1], "rocksdb.estimate-num-keys", &int_num));
    ASSERT_EQ(int_num, base_total_size + 1);
    SetPerfLevel(kDisable);
    ASSERT_TRUE(GetPerfLevel() == kDisable);
  } while (ChangeCompactOptions());
}
class SleepingBackgroundTask {
 public:
  SleepingBackgroundTask()
      : bg_cv_(&mutex_), should_sleep_(true), done_with_sleep_(false) {}
  void DoSleep() {
    MutexLock l(&mutex_);
    while (should_sleep_) {
      bg_cv_.Wait();
    }
    done_with_sleep_ = true;
    bg_cv_.SignalAll();
  }
  void WakeUp() {
    MutexLock l(&mutex_);
    should_sleep_ = false;
    bg_cv_.SignalAll();
  }
  void WaitUntilDone() {
    MutexLock l(&mutex_);
    while (!done_with_sleep_) {
      bg_cv_.Wait();
    }
  }
  static void DoSleepTask(void* arg) {
    reinterpret_cast<SleepingBackgroundTask*>(arg)->DoSleep();
  }
 private:
  port::Mutex mutex_;
  port::CondVar bg_cv_;
  bool should_sleep_;
  bool done_with_sleep_;
};
TEST_F(DBTest, FlushEmptyColumnFamily) {
  env_->SetBackgroundThreads(1, Env::HIGH);
  env_->SetBackgroundThreads(1, Env::LOW);
  SleepingBackgroundTask sleeping_task_low;
  env_->Schedule(&SleepingBackgroundTask::DoSleepTask, &sleeping_task_low,
                 Env::Priority::LOW);
  SleepingBackgroundTask sleeping_task_high;
  env_->Schedule(&SleepingBackgroundTask::DoSleepTask, &sleeping_task_high,
                 Env::Priority::HIGH);
  Options options = CurrentOptions();
  options.disable_auto_compactions = true;
  WriteOptions writeOpt = WriteOptions();
  writeOpt.disableWAL = true;
  options.max_write_buffer_number = 2;
  options.min_write_buffer_number_to_merge = 1;
  options.max_write_buffer_number_to_maintain = 1;
  CreateAndReopenWithCF({"pikachu"}, options);
  ASSERT_OK(Flush(0));
  ASSERT_OK(Flush(1));
  ASSERT_OK(dbfull()->Put(writeOpt, handles_[0], "foo", "v1"));
  ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "bar", "v1"));
  ASSERT_EQ("v1", Get(0, "foo"));
  ASSERT_EQ("v1", Get(1, "bar"));
  sleeping_task_high.WakeUp();
  sleeping_task_high.WaitUntilDone();
  ASSERT_OK(Flush(0));
  ASSERT_OK(Flush(1));
  sleeping_task_low.WakeUp();
  sleeping_task_low.WaitUntilDone();
}
TEST_F(DBTest, GetProperty) {
  env_->SetBackgroundThreads(1, Env::HIGH);
  env_->SetBackgroundThreads(1, Env::LOW);
  SleepingBackgroundTask sleeping_task_low;
  env_->Schedule(&SleepingBackgroundTask::DoSleepTask, &sleeping_task_low,
                 Env::Priority::LOW);
  SleepingBackgroundTask sleeping_task_high;
  env_->Schedule(&SleepingBackgroundTask::DoSleepTask, &sleeping_task_high,
                 Env::Priority::HIGH);
  Options options = CurrentOptions();
  WriteOptions writeOpt = WriteOptions();
  writeOpt.disableWAL = true;
  options.compaction_style = kCompactionStyleUniversal;
  options.level0_file_num_compaction_trigger = 1;
  options.compaction_options_universal.size_ratio = 50;
  options.max_background_compactions = 1;
  options.max_background_flushes = 1;
  options.max_write_buffer_number = 10;
  options.min_write_buffer_number_to_merge = 1;
  options.max_write_buffer_number_to_maintain = 0;
  options.write_buffer_size = 1000000;
  Reopen(options);
  std::string big_value(1000000 * 2, 'x');
  std::string num;
  uint64_t int_num;
  SetPerfLevel(kEnableTime);
  ASSERT_TRUE(
      dbfull()->GetIntProperty("rocksdb.estimate-table-readers-mem", &int_num));
  ASSERT_EQ(int_num, 0U);
  ASSERT_OK(dbfull()->Put(writeOpt, "k1", big_value));
  ASSERT_TRUE(dbfull()->GetProperty("rocksdb.num-immutable-mem-table", &num));
  ASSERT_EQ(num, "0");
  ASSERT_TRUE(dbfull()->GetProperty("rocksdb.mem-table-flush-pending", &num));
  ASSERT_EQ(num, "0");
  ASSERT_TRUE(dbfull()->GetProperty("rocksdb.compaction-pending", &num));
  ASSERT_EQ(num, "0");
  ASSERT_TRUE(dbfull()->GetProperty("rocksdb.estimate-num-keys", &num));
  ASSERT_EQ(num, "1");
  perf_context.Reset();
  ASSERT_OK(dbfull()->Put(writeOpt, "k2", big_value));
  ASSERT_TRUE(dbfull()->GetProperty("rocksdb.num-immutable-mem-table", &num));
  ASSERT_EQ(num, "1");
  ASSERT_OK(dbfull()->Delete(writeOpt, "k-non-existing"));
  ASSERT_OK(dbfull()->Put(writeOpt, "k3", big_value));
  ASSERT_TRUE(dbfull()->GetProperty("rocksdb.num-immutable-mem-table", &num));
  ASSERT_EQ(num, "2");
  ASSERT_TRUE(dbfull()->GetProperty("rocksdb.mem-table-flush-pending", &num));
  ASSERT_EQ(num, "1");
  ASSERT_TRUE(dbfull()->GetProperty("rocksdb.compaction-pending", &num));
  ASSERT_EQ(num, "0");
  ASSERT_TRUE(dbfull()->GetProperty("rocksdb.estimate-num-keys", &num));
  ASSERT_EQ(num, "2");
  ASSERT_TRUE(
      dbfull()->GetIntProperty("rocksdb.num-immutable-mem-table", &int_num));
  ASSERT_EQ(int_num, 2U);
  ASSERT_TRUE(
      dbfull()->GetIntProperty("rocksdb.mem-table-flush-pending", &int_num));
  ASSERT_EQ(int_num, 1U);
  ASSERT_TRUE(dbfull()->GetIntProperty("rocksdb.compaction-pending", &int_num));
  ASSERT_EQ(int_num, 0U);
  ASSERT_TRUE(dbfull()->GetIntProperty("rocksdb.estimate-num-keys", &int_num));
  ASSERT_EQ(int_num, 2U);
  ASSERT_TRUE(
      dbfull()->GetIntProperty("rocksdb.estimate-table-readers-mem", &int_num));
  ASSERT_EQ(int_num, 0U);
  sleeping_task_high.WakeUp();
  sleeping_task_high.WaitUntilDone();
  dbfull()->TEST_WaitForFlushMemTable();
  ASSERT_OK(dbfull()->Put(writeOpt, "k4", big_value));
  ASSERT_OK(dbfull()->Put(writeOpt, "k5", big_value));
  dbfull()->TEST_WaitForFlushMemTable();
  ASSERT_TRUE(dbfull()->GetProperty("rocksdb.mem-table-flush-pending", &num));
  ASSERT_EQ(num, "0");
  ASSERT_TRUE(dbfull()->GetProperty("rocksdb.compaction-pending", &num));
  ASSERT_EQ(num, "1");
  ASSERT_TRUE(dbfull()->GetProperty("rocksdb.estimate-num-keys", &num));
  ASSERT_EQ(num, "4");
  ASSERT_TRUE(
      dbfull()->GetIntProperty("rocksdb.estimate-table-readers-mem", &int_num));
  ASSERT_GT(int_num, 0U);
  sleeping_task_low.WakeUp();
  sleeping_task_low.WaitUntilDone();
  dbfull()->TEST_WaitForFlushMemTable();
  options.max_open_files = 10;
  Reopen(options);
  ASSERT_TRUE(
      dbfull()->GetIntProperty("rocksdb.estimate-table-readers-mem", &int_num));
  ASSERT_EQ(int_num, 0U);
  ASSERT_TRUE(dbfull()->GetIntProperty("rocksdb.estimate-num-keys", &int_num));
  ASSERT_GT(int_num, 0U);
  Get("k5");
  ASSERT_TRUE(
      dbfull()->GetIntProperty("rocksdb.estimate-table-readers-mem", &int_num));
  ASSERT_GT(int_num, 0U);
  {
    options.level0_file_num_compaction_trigger = 20;
    Reopen(options);
    ASSERT_TRUE(
        dbfull()->GetIntProperty("rocksdb.num-live-versions", &int_num));
    ASSERT_EQ(int_num, 1U);
    std::unique_ptr<Iterator> iter1(dbfull()->NewIterator(ReadOptions()));
    ASSERT_OK(dbfull()->Put(writeOpt, "k6", big_value));
    Flush();
    ASSERT_TRUE(
        dbfull()->GetIntProperty("rocksdb.num-live-versions", &int_num));
    ASSERT_EQ(int_num, 2U);
    std::unique_ptr<Iterator> iter2(dbfull()->NewIterator(ReadOptions()));
    ASSERT_OK(dbfull()->Put(writeOpt, "k7", big_value));
    Flush();
    ASSERT_TRUE(
        dbfull()->GetIntProperty("rocksdb.num-live-versions", &int_num));
    ASSERT_EQ(int_num, 3U);
    iter2.reset();
    ASSERT_TRUE(
        dbfull()->GetIntProperty("rocksdb.num-live-versions", &int_num));
    ASSERT_EQ(int_num, 2U);
    iter1.reset();
    ASSERT_TRUE(
        dbfull()->GetIntProperty("rocksdb.num-live-versions", &int_num));
    ASSERT_EQ(int_num, 1U);
  }
}
TEST_F(DBTest, FLUSH) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    WriteOptions writeOpt = WriteOptions();
    writeOpt.disableWAL = true;
    SetPerfLevel(kEnableTime);;
    ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "foo", "v1"));
    ASSERT_OK(Flush(1));
    ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "bar", "v1"));
    perf_context.Reset();
    Get(1, "foo");
    ASSERT_TRUE((int) perf_context.get_from_output_files_time > 0);
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ASSERT_EQ("v1", Get(1, "foo"));
    ASSERT_EQ("v1", Get(1, "bar"));
    writeOpt.disableWAL = true;
    ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "bar", "v2"));
    ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "foo", "v2"));
    ASSERT_OK(Flush(1));
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ASSERT_EQ("v2", Get(1, "bar"));
    perf_context.Reset();
    ASSERT_EQ("v2", Get(1, "foo"));
    ASSERT_TRUE((int) perf_context.get_from_output_files_time > 0);
    writeOpt.disableWAL = false;
    ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "bar", "v3"));
    ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "foo", "v3"));
    ASSERT_OK(Flush(1));
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ASSERT_EQ("v3", Get(1, "foo"));
    ASSERT_EQ("v3", Get(1, "bar"));
    SetPerfLevel(kDisable);
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, RecoveryWithEmptyLog) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    ASSERT_OK(Put(1, "foo", "v1"));
    ASSERT_OK(Put(1, "foo", "v2"));
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ASSERT_OK(Put(1, "foo", "v3"));
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ASSERT_EQ("v3", Get(1, "foo"));
  } while (ChangeOptions());
}
TEST_F(DBTest, RecoverDuringMemtableCompaction) {
  do {
    Options options;
    options.env = env_;
    options.write_buffer_size = 1000000;
    options = CurrentOptions(options);
    CreateAndReopenWithCF({"pikachu"}, options);
    ASSERT_OK(Put(1, "foo", "v1"));
    ASSERT_OK(Put(1, "big1", std::string(10000000, 'x')));
    ASSERT_OK(Put(1, "big2", std::string(1000, 'y')));
    ASSERT_OK(Put(1, "bar", "v2"));
    ReopenWithColumnFamilies({"default", "pikachu"}, options);
    ASSERT_EQ("v1", Get(1, "foo"));
    ASSERT_EQ("v2", Get(1, "bar"));
    ASSERT_EQ(std::string(10000000, 'x'), Get(1, "big1"));
    ASSERT_EQ(std::string(1000, 'y'), Get(1, "big2"));
  } while (ChangeOptions());
}
#ifndef ROCKSDB_TSAN_RUN
TEST_F(DBTest, FlushSchedule) {
  Options options = CurrentOptions();
  options.disable_auto_compactions = true;
  options.level0_stop_writes_trigger = 1 << 10;
  options.level0_slowdown_writes_trigger = 1 << 10;
  options.min_write_buffer_number_to_merge = 1;
  options.max_write_buffer_number_to_maintain = 1;
  options.max_write_buffer_number = 2;
  options.write_buffer_size = 100 * 1000;
  CreateAndReopenWithCF({"pikachu"}, options);
  std::vector<std::thread> threads;
  std::atomic<int> thread_num(0);
  for (int i = 0; i < 10; ++i) {
    threads.emplace_back([&]() {
      int a = thread_num.fetch_add(1);
      Random rnd(a);
      WriteOptions wo;
      for (int k = 0; k < 5000; ++k) {
        ASSERT_OK(db_->Put(wo, handles_[a & 1], RandomString(&rnd, 13), ""));
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  auto default_tables = GetNumberOfSstFilesForColumnFamily(db_, "default");
  auto pikachu_tables = GetNumberOfSstFilesForColumnFamily(db_, "pikachu");
  ASSERT_LE(default_tables, static_cast<uint64_t>(10));
  ASSERT_GT(default_tables, static_cast<uint64_t>(0));
  ASSERT_LE(pikachu_tables, static_cast<uint64_t>(10));
  ASSERT_GT(pikachu_tables, static_cast<uint64_t>(0));
}
#endif
TEST_F(DBTest, MinorCompactionsHappen) {
  do {
    Options options;
    options.write_buffer_size = 10000;
    options = CurrentOptions(options);
    CreateAndReopenWithCF({"pikachu"}, options);
    const int N = 500;
    int starting_num_tables = TotalTableFiles(1);
    for (int i = 0; i < N; i++) {
      ASSERT_OK(Put(1, Key(i), Key(i) + std::string(1000, 'v')));
    }
    int ending_num_tables = TotalTableFiles(1);
    ASSERT_GT(ending_num_tables, starting_num_tables);
    for (int i = 0; i < N; i++) {
      ASSERT_EQ(Key(i) + std::string(1000, 'v'), Get(1, Key(i)));
    }
    ReopenWithColumnFamilies({"default", "pikachu"}, options);
    for (int i = 0; i < N; i++) {
      ASSERT_EQ(Key(i) + std::string(1000, 'v'), Get(1, Key(i)));
    }
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, ManifestRollOver) {
  do {
    Options options;
    options.max_manifest_file_size = 10 ;
    options = CurrentOptions(options);
    CreateAndReopenWithCF({"pikachu"}, options);
    {
      ASSERT_OK(Put(1, "manifest_key1", std::string(1000, '1')));
      ASSERT_OK(Put(1, "manifest_key2", std::string(1000, '2')));
      ASSERT_OK(Put(1, "manifest_key3", std::string(1000, '3')));
      uint64_t manifest_before_flush = dbfull()->TEST_Current_Manifest_FileNo();
      ASSERT_OK(Flush(1));
      uint64_t manifest_after_flush = dbfull()->TEST_Current_Manifest_FileNo();
      ASSERT_GT(manifest_after_flush, manifest_before_flush);
      ReopenWithColumnFamilies({"default", "pikachu"}, options);
      ASSERT_GT(dbfull()->TEST_Current_Manifest_FileNo(), manifest_after_flush);
      ASSERT_EQ(std::string(1000, '1'), Get(1, "manifest_key1"));
      ASSERT_EQ(std::string(1000, '2'), Get(1, "manifest_key2"));
      ASSERT_EQ(std::string(1000, '3'), Get(1, "manifest_key3"));
    }
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, IdentityAcrossRestarts) {
  do {
    std::string id1;
    ASSERT_OK(db_->GetDbIdentity(id1));
    Options options = CurrentOptions();
    Reopen(options);
    std::string id2;
    ASSERT_OK(db_->GetDbIdentity(id2));
    ASSERT_EQ(id1.compare(id2), 0);
    std::string idfilename = IdentityFileName(dbname_);
    ASSERT_OK(env_->DeleteFile(idfilename));
    Reopen(options);
    std::string id3;
    ASSERT_OK(db_->GetDbIdentity(id3));
    ASSERT_NE(id1.compare(id3), 0);
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, RecoverWithLargeLog) {
  do {
    {
      Options options = CurrentOptions();
      CreateAndReopenWithCF({"pikachu"}, options);
      ASSERT_OK(Put(1, "big1", std::string(200000, '1')));
      ASSERT_OK(Put(1, "big2", std::string(200000, '2')));
      ASSERT_OK(Put(1, "small3", std::string(10, '3')));
      ASSERT_OK(Put(1, "small4", std::string(10, '4')));
      ASSERT_EQ(NumTableFilesAtLevel(0, 1), 0);
    }
    Options options;
    options.write_buffer_size = 100000;
    options = CurrentOptions(options);
    ReopenWithColumnFamilies({"default", "pikachu"}, options);
    ASSERT_EQ(NumTableFilesAtLevel(0, 1), 3);
    ASSERT_EQ(std::string(200000, '1'), Get(1, "big1"));
    ASSERT_EQ(std::string(200000, '2'), Get(1, "big2"));
    ASSERT_EQ(std::string(10, '3'), Get(1, "small3"));
    ASSERT_EQ(std::string(10, '4'), Get(1, "small4"));
    ASSERT_GT(NumTableFilesAtLevel(0, 1), 1);
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, CompactionsGenerateMultipleFiles) {
  Options options;
  options.write_buffer_size = 100000000;
  options = CurrentOptions(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  Random rnd(301);
  ASSERT_EQ(NumTableFilesAtLevel(0, 1), 0);
  std::vector<std::string> values;
  for (int i = 0; i < 80; i++) {
    values.push_back(RandomString(&rnd, 100000));
    ASSERT_OK(Put(1, Key(i), values[i]));
  }
  ReopenWithColumnFamilies({"default", "pikachu"}, options);
  dbfull()->TEST_CompactRange(0, nullptr, nullptr, handles_[1],
                              true );
  ASSERT_EQ(NumTableFilesAtLevel(0, 1), 0);
  ASSERT_GT(NumTableFilesAtLevel(1, 1), 1);
  for (int i = 0; i < 80; i++) {
    ASSERT_EQ(Get(1, Key(i)), values[i]);
  }
}
TEST_F(DBTest, TrivialMoveOneFile) {
  int32_t trivial_move = 0;
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:TrivialMove",
      [&](void* arg) { trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  Options options;
  options.write_buffer_size = 100000000;
  options = CurrentOptions(options);
  DestroyAndReopen(options);
  int32_t num_keys = 80;
  int32_t value_size = 100 * 1024;
  Random rnd(301);
  std::vector<std::string> values;
  for (int i = 0; i < num_keys; i++) {
    values.push_back(RandomString(&rnd, value_size));
    ASSERT_OK(Put(Key(i), values[i]));
  }
  Reopen(options);
  ASSERT_EQ(NumTableFilesAtLevel(0, 0), 1);
  ASSERT_EQ(NumTableFilesAtLevel(1, 0), 0);
  std::vector<LiveFileMetaData> metadata;
  db_->GetLiveFilesMetaData(&metadata);
  ASSERT_EQ(metadata.size(), 1U);
  LiveFileMetaData level0_file = metadata[0];
  dbfull()->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  ASSERT_EQ(NumTableFilesAtLevel(0, 0), 0);
  ASSERT_EQ(NumTableFilesAtLevel(1, 0), 1);
  metadata.clear();
  db_->GetLiveFilesMetaData(&metadata);
  ASSERT_EQ(metadata.size(), 1U);
  ASSERT_EQ(metadata[0].name , level0_file.name);
  ASSERT_EQ(metadata[0].size , level0_file.size);
  for (int i = 0; i < num_keys; i++) {
    ASSERT_EQ(Get(Key(i)), values[i]);
  }
  ASSERT_EQ(trivial_move, 1);
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}
TEST_F(DBTest, TrivialMoveNonOverlappingFiles) {
  int32_t trivial_move = 0;
  int32_t non_trivial_move = 0;
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:TrivialMove",
      [&](void* arg) { trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:NonTrivial",
      [&](void* arg) { non_trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  Options options = CurrentOptions();
  options.disable_auto_compactions = true;
  options.write_buffer_size = 10 * 1024 * 1024;
  DestroyAndReopen(options);
  std::vector<std::pair<int32_t, int32_t>> ranges = {
    {100, 199},
    {300, 399},
    {0, 99},
    {200, 299},
    {600, 699},
    {400, 499},
    {500, 550},
    {551, 599},
  };
  int32_t value_size = 10 * 1024;
  Random rnd(301);
  std::map<int32_t, std::string> values;
  for (uint32_t i = 0; i < ranges.size(); i++) {
    for (int32_t j = ranges[i].first; j <= ranges[i].second; j++) {
      values[j] = RandomString(&rnd, value_size);
      ASSERT_OK(Put(Key(j), values[j]));
    }
    ASSERT_OK(Flush());
  }
  int32_t level0_files = NumTableFilesAtLevel(0, 0);
  ASSERT_EQ(level0_files, ranges.size());
  ASSERT_EQ(NumTableFilesAtLevel(1, 0), 0);
  db_->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  ASSERT_EQ(NumTableFilesAtLevel(0, 0), 0);
  ASSERT_EQ(NumTableFilesAtLevel(1, 0) , level0_files);
  for (uint32_t i = 0; i < ranges.size(); i++) {
    for (int32_t j = ranges[i].first; j <= ranges[i].second; j++) {
      ASSERT_EQ(Get(Key(j)), values[j]);
    }
  }
  ASSERT_EQ(trivial_move, 1);
  ASSERT_EQ(non_trivial_move, 0);
  trivial_move = 0;
  non_trivial_move = 0;
  values.clear();
  DestroyAndReopen(options);
  ranges = {
    {100, 199},
    {300, 399},
    {0, 99},
    {200, 299},
    {600, 699},
    {400, 499},
    {500, 560},
    {551, 599},
  };
  for (uint32_t i = 0; i < ranges.size(); i++) {
    for (int32_t j = ranges[i].first; j <= ranges[i].second; j++) {
      values[j] = RandomString(&rnd, value_size);
      ASSERT_OK(Put(Key(j), values[j]));
    }
    ASSERT_OK(Flush());
  }
  db_->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  for (uint32_t i = 0; i < ranges.size(); i++) {
    for (int32_t j = ranges[i].first; j <= ranges[i].second; j++) {
      ASSERT_EQ(Get(Key(j)), values[j]);
    }
  }
  ASSERT_EQ(trivial_move, 0);
  ASSERT_EQ(non_trivial_move, 1);
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}
TEST_F(DBTest, TrivialMoveTargetLevel) {
  int32_t trivial_move = 0;
  int32_t non_trivial_move = 0;
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:TrivialMove",
      [&](void* arg) { trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:NonTrivial",
      [&](void* arg) { non_trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  Options options = CurrentOptions();
  options.disable_auto_compactions = true;
  options.write_buffer_size = 10 * 1024 * 1024;
  options.num_levels = 7;
  DestroyAndReopen(options);
  int32_t value_size = 10 * 1024;
  Random rnd(301);
  std::map<int32_t, std::string> values;
  for (int32_t i = 0; i <= 300; i++) {
    values[i] = RandomString(&rnd, value_size);
    ASSERT_OK(Put(Key(i), values[i]));
  }
  ASSERT_OK(Flush());
  for (int32_t i = 600; i <= 700; i++) {
    values[i] = RandomString(&rnd, value_size);
    ASSERT_OK(Put(Key(i), values[i]));
  }
  ASSERT_OK(Flush());
  ASSERT_EQ("2", FilesPerLevel(0));
  CompactRangeOptions compact_options;
  compact_options.change_level = true;
  compact_options.target_level = 6;
  ASSERT_OK(db_->CompactRange(compact_options, nullptr, nullptr));
  ASSERT_EQ("0,0,0,0,0,0,2", FilesPerLevel(0));
  ASSERT_EQ(trivial_move, 1);
  ASSERT_EQ(non_trivial_move, 0);
  for (int32_t i = 0; i <= 300; i++) {
    ASSERT_EQ(Get(Key(i)), values[i]);
  }
  for (int32_t i = 600; i <= 700; i++) {
    ASSERT_EQ(Get(Key(i)), values[i]);
  }
}
TEST_F(DBTest, TrivialMoveToLastLevelWithFiles) {
  int32_t trivial_move = 0;
  int32_t non_trivial_move = 0;
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:TrivialMove",
      [&](void* arg) { trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:NonTrivial",
      [&](void* arg) { non_trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  Options options;
  options.write_buffer_size = 100000000;
  options = CurrentOptions(options);
  DestroyAndReopen(options);
  int32_t value_size = 10 * 1024;
  Random rnd(301);
  std::vector<std::string> values;
  for (int i = 0; i < 100; i++) {
    values.push_back(RandomString(&rnd, value_size));
    ASSERT_OK(Put(Key(i), values[i]));
  }
  ASSERT_OK(Flush());
  ASSERT_EQ("1", FilesPerLevel(0));
  CompactRangeOptions compact_options;
  compact_options.change_level = true;
  compact_options.target_level = 3;
  ASSERT_OK(db_->CompactRange(compact_options, nullptr, nullptr));
  ASSERT_EQ("0,0,0,1", FilesPerLevel(0));
  ASSERT_EQ(trivial_move, 1);
  ASSERT_EQ(non_trivial_move, 0);
  for (int i = 100; i < 200; i++) {
    values.push_back(RandomString(&rnd, value_size));
    ASSERT_OK(Put(Key(i), values[i]));
  }
  ASSERT_OK(Flush());
  ASSERT_EQ("1,0,0,1", FilesPerLevel(0));
  ASSERT_OK(db_->CompactRange(CompactRangeOptions(), nullptr, nullptr));
  ASSERT_EQ("0,0,0,2", FilesPerLevel(0));
  ASSERT_EQ(trivial_move, 4);
  ASSERT_EQ(non_trivial_move, 0);
  for (int i = 0; i < 200; i++) {
    ASSERT_EQ(Get(Key(i)), values[i]);
  }
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}
TEST_F(DBTest, CompactionTrigger) {
  Options options;
  options.write_buffer_size = 100<<10;
  options.num_levels = 3;
  options.max_mem_compaction_level = 0;
  options.level0_file_num_compaction_trigger = 3;
  options = CurrentOptions(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  Random rnd(301);
  for (int num = 0; num < options.level0_file_num_compaction_trigger - 1;
       num++) {
    std::vector<std::string> values;
    for (int i = 0; i < 12; i++) {
      values.push_back(RandomString(&rnd, 10000));
      ASSERT_OK(Put(1, Key(i), values[i]));
    }
    dbfull()->TEST_WaitForFlushMemTable(handles_[1]);
    ASSERT_EQ(NumTableFilesAtLevel(0, 1), num + 1);
  }
  std::vector<std::string> values;
  for (int i = 0; i < 12; i++) {
    values.push_back(RandomString(&rnd, 10000));
    ASSERT_OK(Put(1, Key(i), values[i]));
  }
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ(NumTableFilesAtLevel(0, 1), 0);
  ASSERT_EQ(NumTableFilesAtLevel(1, 1), 1);
}
namespace {
static const int kCDTValueSize = 1000;
static const int kCDTKeysPerBuffer = 4;
static const int kCDTNumLevels = 8;
Options DeletionTriggerOptions() {
  Options options;
  options.compression = kNoCompression;
  options.write_buffer_size = kCDTKeysPerBuffer * (kCDTValueSize + 24);
  options.min_write_buffer_number_to_merge = 1;
  options.max_write_buffer_number_to_maintain = 0;
  options.num_levels = kCDTNumLevels;
  options.max_mem_compaction_level = 0;
  options.level0_file_num_compaction_trigger = 1;
  options.target_file_size_base = options.write_buffer_size * 2;
  options.target_file_size_multiplier = 2;
  options.max_bytes_for_level_base =
      options.target_file_size_base * options.target_file_size_multiplier;
  options.max_bytes_for_level_multiplier = 2;
  options.disable_auto_compactions = false;
  return options;
}
}
TEST_F(DBTest, CompactionDeletionTrigger) {
  for (int tid = 0; tid < 2; ++tid) {
    uint64_t db_size[2];
    Options options = CurrentOptions(DeletionTriggerOptions());
    if (tid == 1) {
      options.compaction_style = kCompactionStyleUniversal;
      options.num_levels = 1;
    }
    DestroyAndReopen(options);
    Random rnd(301);
    const int kTestSize = kCDTKeysPerBuffer * 512;
    std::vector<std::string> values;
    for (int k = 0; k < kTestSize; ++k) {
      values.push_back(RandomString(&rnd, kCDTValueSize));
      ASSERT_OK(Put(Key(k), values[k]));
    }
    dbfull()->TEST_WaitForFlushMemTable();
    dbfull()->TEST_WaitForCompact();
    db_size[0] = Size(Key(0), Key(kTestSize - 1));
    for (int k = 0; k < kTestSize; ++k) {
      ASSERT_OK(Delete(Key(k)));
    }
    dbfull()->TEST_WaitForFlushMemTable();
    dbfull()->TEST_WaitForCompact();
    db_size[1] = Size(Key(0), Key(kTestSize - 1));
    ASSERT_GT(db_size[0] / 3, db_size[1]);
  }
}
TEST_F(DBTest, CompactionDeletionTriggerReopen) {
  for (int tid = 0; tid < 2; ++tid) {
    uint64_t db_size[3];
    Options options = CurrentOptions(DeletionTriggerOptions());
    if (tid == 1) {
      options.compaction_style = kCompactionStyleUniversal;
      options.num_levels = 1;
    }
    DestroyAndReopen(options);
    Random rnd(301);
    const int kTestSize = kCDTKeysPerBuffer * 512;
    std::vector<std::string> values;
    for (int k = 0; k < kTestSize; ++k) {
      values.push_back(RandomString(&rnd, kCDTValueSize));
      ASSERT_OK(Put(Key(k), values[k]));
    }
    dbfull()->TEST_WaitForFlushMemTable();
    dbfull()->TEST_WaitForCompact();
    db_size[0] = Size(Key(0), Key(kTestSize - 1));
    Close();
    options.create_if_missing = false;
    options.disable_auto_compactions = true;
    Reopen(options);
    for (int k = 0; k < kTestSize; ++k) {
      ASSERT_OK(Delete(Key(k)));
    }
    db_size[1] = Size(Key(0), Key(kTestSize - 1));
    Close();
    ASSERT_LT(db_size[0] / 3, db_size[1]);
    options.disable_auto_compactions = false;
    Reopen(options);
    for (int k = 0; k < kTestSize / 10; ++k) {
      ASSERT_OK(Put(Key(k), values[k]));
    }
    dbfull()->TEST_WaitForFlushMemTable();
    dbfull()->TEST_WaitForCompact();
    db_size[2] = Size(Key(0), Key(kTestSize - 1));
    ASSERT_GT(db_size[0] / 3, db_size[2]);
  }
}
static int cfilter_count;
static std::string NEW_VALUE = "NewValue";
class KeepFilter : public CompactionFilter {
 public:
  virtual bool Filter(int level, const Slice& key, const Slice& value,
                      std::string* new_value, bool* value_changed) const
      override {
    cfilter_count++;
    return false;
  }
  virtual const char* Name() const override { return "KeepFilter"; }
};
class DeleteFilter : public CompactionFilter {
 public:
  virtual bool Filter(int level, const Slice& key, const Slice& value,
                      std::string* new_value, bool* value_changed) const
      override {
    cfilter_count++;
    return true;
  }
  virtual const char* Name() const override { return "DeleteFilter"; }
};
class DelayFilter : public CompactionFilter {
 public:
  explicit DelayFilter(DBTest* d) : db_test(d) {}
  virtual bool Filter(int level, const Slice& key, const Slice& value,
                      std::string* new_value,
                      bool* value_changed) const override {
    db_test->env_->addon_time_.fetch_add(1000);
    return true;
  }
  virtual const char* Name() const override { return "DelayFilter"; }
 private:
  DBTest* db_test;
};
class ConditionalFilter : public CompactionFilter {
 public:
  explicit ConditionalFilter(const std::string* filtered_value)
      : filtered_value_(filtered_value) {}
  virtual bool Filter(int level, const Slice& key, const Slice& value,
                      std::string* new_value,
                      bool* value_changed) const override {
    return value.ToString() == *filtered_value_;
  }
  virtual const char* Name() const override { return "ConditionalFilter"; }
 private:
  const std::string* filtered_value_;
};
class ChangeFilter : public CompactionFilter {
 public:
  explicit ChangeFilter() {}
  virtual bool Filter(int level, const Slice& key, const Slice& value,
                      std::string* new_value, bool* value_changed) const
      override {
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
      const CompactionFilter::Context& context) override {
    if (check_context_) {
      EXPECT_EQ(expect_full_compaction_.load(), context.is_full_compaction);
      EXPECT_EQ(expect_manual_compaction_.load(), context.is_manual_compaction);
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
      const CompactionFilter::Context& context) override {
    if (context.is_manual_compaction) {
      return std::unique_ptr<CompactionFilter>(new DeleteFilter());
    } else {
      return std::unique_ptr<CompactionFilter>(nullptr);
    }
  }
  virtual const char* Name() const override { return "DeleteFilterFactory"; }
};
class DelayFilterFactory : public CompactionFilterFactory {
 public:
  explicit DelayFilterFactory(DBTest* d) : db_test(d) {}
  virtual std::unique_ptr<CompactionFilter> CreateCompactionFilter(
      const CompactionFilter::Context& context) override {
    return std::unique_ptr<CompactionFilter>(new DelayFilter(db_test));
  }
  virtual const char* Name() const override { return "DelayFilterFactory"; }
 private:
  DBTest* db_test;
};
class ConditionalFilterFactory : public CompactionFilterFactory {
 public:
  explicit ConditionalFilterFactory(const Slice& filtered_value)
      : filtered_value_(filtered_value.ToString()) {}
  virtual std::unique_ptr<CompactionFilter> CreateCompactionFilter(
      const CompactionFilter::Context& context) override {
    return std::unique_ptr<CompactionFilter>(
        new ConditionalFilter(&filtered_value_));
  }
  virtual const char* Name() const override {
    return "ConditionalFilterFactory";
  }
 private:
  std::string filtered_value_;
};
class ChangeFilterFactory : public CompactionFilterFactory {
 public:
  explicit ChangeFilterFactory() {}
  virtual std::unique_ptr<CompactionFilter> CreateCompactionFilter(
      const CompactionFilter::Context& context) override {
    return std::unique_ptr<CompactionFilter>(new ChangeFilter());
  }
  virtual const char* Name() const override { return "ChangeFilterFactory"; }
};
class DBTestUniversalCompactionBase
    : public DBTest,
      public ::testing::WithParamInterface<int> {
 public:
  virtual void SetUp() override { num_levels_ = GetParam(); }
  int num_levels_;
};
class DBTestUniversalCompaction : public DBTestUniversalCompactionBase {};
TEST_P(DBTestUniversalCompaction, UniversalCompactionTrigger) {
  Options options;
  options.compaction_style = kCompactionStyleUniversal;
  options.num_levels = num_levels_;
  options.write_buffer_size = 100 << 10;
  options.target_file_size_base = 32 << 10;
  options.level0_file_num_compaction_trigger = 4;
  KeepFilterFactory* filter = new KeepFilterFactory(true);
  filter->expect_manual_compaction_.store(false);
  options.compaction_filter_factory.reset(filter);
  options = CurrentOptions(options);
  DestroyAndReopen(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBTestWritableFile.GetPreallocationStatus", [&](void* arg) {
        ASSERT_TRUE(arg != nullptr);
        size_t preallocation_size = *(static_cast<size_t*>(arg));
        if (num_levels_ > 3) {
          ASSERT_LE(preallocation_size, options.target_file_size_base * 1.1);
        }
      });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  Random rnd(301);
  int key_idx = 0;
  filter->expect_full_compaction_.store(true);
  for (int num = 0; num < options.level0_file_num_compaction_trigger - 1;
       num++) {
    for (int i = 0; i < 12; i++) {
      ASSERT_OK(Put(1, Key(key_idx), RandomString(&rnd, 10000)));
      key_idx++;
    }
    dbfull()->TEST_WaitForFlushMemTable(handles_[1]);
    ASSERT_EQ(NumSortedRuns(1), num + 1);
  }
  for (int i = 0; i < 11; i++) {
    ASSERT_OK(Put(1, Key(key_idx), RandomString(&rnd, 10000)));
    key_idx++;
  }
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ(NumSortedRuns(1), 1);
  filter->expect_full_compaction_.store(false);
  ASSERT_OK(Flush(1));
  for (int num = 0; num < options.level0_file_num_compaction_trigger - 3;
       num++) {
    for (int i = 0; i < 11; i++) {
      ASSERT_OK(Put(1, Key(key_idx), RandomString(&rnd, 10000)));
      key_idx++;
    }
    dbfull()->TEST_WaitForFlushMemTable(handles_[1]);
    ASSERT_EQ(NumSortedRuns(1), num + 3);
  }
  for (int i = 0; i < 11; i++) {
    ASSERT_OK(Put(1, Key(key_idx), RandomString(&rnd, 10000)));
    key_idx++;
  }
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ(NumSortedRuns(1), 2);
  for (int num = 0; num < options.level0_file_num_compaction_trigger - 3;
       num++) {
    for (int i = 0; i < 11; i++) {
      ASSERT_OK(Put(1, Key(key_idx), RandomString(&rnd, 10000)));
      key_idx++;
    }
    dbfull()->TEST_WaitForFlushMemTable(handles_[1]);
    ASSERT_EQ(NumSortedRuns(1), num + 3);
  }
  for (int i = 0; i < 12; i++) {
    ASSERT_OK(Put(1, Key(key_idx), RandomString(&rnd, 10000)));
    key_idx++;
  }
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ(NumSortedRuns(1), 3);
  for (int i = 0; i < 11; i++) {
    ASSERT_OK(Put(1, Key(key_idx), RandomString(&rnd, 10000)));
    key_idx++;
  }
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ(NumSortedRuns(1), 4);
  filter->expect_full_compaction_.store(true);
  for (int i = 0; i < 11; i++) {
    ASSERT_OK(Put(1, Key(key_idx), RandomString(&rnd, 10000)));
    key_idx++;
  }
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ(NumSortedRuns(1), 1);
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}
TEST_P(DBTestUniversalCompaction, UniversalCompactionSizeAmplification) {
  Options options;
  options.compaction_style = kCompactionStyleUniversal;
  options.num_levels = num_levels_;
  options.write_buffer_size = 100 << 10;
  options.target_file_size_base = 32 << 10;
  options.level0_file_num_compaction_trigger = 3;
  options = CurrentOptions(options);
  DestroyAndReopen(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  options.compaction_options_universal.max_size_amplification_percent = 110;
  options = CurrentOptions(options);
  ReopenWithColumnFamilies({"default", "pikachu"}, options);
  Random rnd(301);
  int key_idx = 0;
  for (int num = 0; num < options.level0_file_num_compaction_trigger - 1;
       num++) {
    for (int i = 0; i < 11; i++) {
      ASSERT_OK(Put(1, Key(key_idx), RandomString(&rnd, 10000)));
      key_idx++;
    }
    dbfull()->TEST_WaitForFlushMemTable(handles_[1]);
    ASSERT_EQ(NumSortedRuns(1), num + 1);
  }
  ASSERT_EQ(NumSortedRuns(1), 2);
  ASSERT_OK(Flush(1));
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ(NumSortedRuns(1), 1);
}
class DBTestUniversalCompactionMultiLevels
    : public DBTestUniversalCompactionBase {};
TEST_P(DBTestUniversalCompactionMultiLevels, UniversalCompactionMultiLevels) {
  Options options;
  options.compaction_style = kCompactionStyleUniversal;
  options.num_levels = num_levels_;
  options.write_buffer_size = 100 << 10;
  options.level0_file_num_compaction_trigger = 8;
  options.max_background_compactions = 3;
  options.target_file_size_base = 32 * 1024;
  options = CurrentOptions(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  options.compaction_options_universal.max_size_amplification_percent = 110;
  options = CurrentOptions(options);
  ReopenWithColumnFamilies({"default", "pikachu"}, options);
  Random rnd(301);
  int num_keys = 100000;
  for (int i = 0; i < num_keys * 2; i++) {
    ASSERT_OK(Put(1, Key(i % num_keys), Key(i)));
  }
  dbfull()->TEST_WaitForCompact();
  for (int i = num_keys; i < num_keys * 2; i++) {
    ASSERT_EQ(Get(1, Key(i % num_keys)), Key(i));
  }
}
TEST_P(DBTestUniversalCompactionMultiLevels, UniversalCompactionTrivialMove) {
  int32_t trivial_move = 0;
  int32_t non_trivial_move = 0;
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:TrivialMove",
      [&](void* arg) { trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:NonTrivial",
      [&](void* arg) { non_trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  Options options;
  options.compaction_style = kCompactionStyleUniversal;
  options.compaction_options_universal.allow_trivial_move = true;
  options.num_levels = 3;
  options.write_buffer_size = 100 << 10;
  options.level0_file_num_compaction_trigger = 3;
  options.max_background_compactions = 1;
  options.target_file_size_base = 32 * 1024;
  options = CurrentOptions(options);
  DestroyAndReopen(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  options.compaction_options_universal.max_size_amplification_percent = 110;
  options = CurrentOptions(options);
  ReopenWithColumnFamilies({"default", "pikachu"}, options);
  Random rnd(301);
  int num_keys = 15000;
  for (int i = 0; i < num_keys; i++) {
    ASSERT_OK(Put(1, Key(i), Key(i)));
  }
  std::vector<std::string> values;
  ASSERT_OK(Flush(1));
  dbfull()->TEST_WaitForCompact();
  ASSERT_GT(trivial_move, 0);
  ASSERT_EQ(non_trivial_move, 0);
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}
INSTANTIATE_TEST_CASE_P(DBTestUniversalCompactionMultiLevels,
                        DBTestUniversalCompactionMultiLevels,
                        ::testing::Values(3, 20));
class DBTestUniversalCompactionParallel : public DBTestUniversalCompactionBase {
};
TEST_P(DBTestUniversalCompactionParallel, UniversalCompactionParallel) {
  Options options;
  options.compaction_style = kCompactionStyleUniversal;
  options.num_levels = num_levels_;
  options.write_buffer_size = 1 << 10;
  options.level0_file_num_compaction_trigger = 3;
  options.max_background_compactions = 3;
  options.max_background_flushes = 3;
  options.target_file_size_base = 1 * 1024;
  options.compaction_options_universal.max_size_amplification_percent = 110;
  options = CurrentOptions(options);
  DestroyAndReopen(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  std::atomic<int> num_compactions_running(0);
  std::atomic<bool> has_parallel(false);
  rocksdb::SyncPoint::GetInstance()->SetCallBack("CompactionJob::Run():Start",
                                                 [&](void* arg) {
    if (num_compactions_running.fetch_add(1) > 0) {
      has_parallel.store(true);
      return;
    }
    for (int nwait = 0; nwait < 20000; nwait++) {
      if (has_parallel.load() || num_compactions_running.load() > 1) {
        has_parallel.store(true);
        break;
      }
      env_->SleepForMicroseconds(1000);
    }
  });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "CompactionJob::Run():End",
      [&](void* arg) { num_compactions_running.fetch_add(-1); });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  options = CurrentOptions(options);
  ReopenWithColumnFamilies({"default", "pikachu"}, options);
  Random rnd(301);
  int num_keys = 30000;
  for (int i = 0; i < num_keys * 2; i++) {
    ASSERT_OK(Put(1, Key(i % num_keys), Key(i)));
  }
  dbfull()->TEST_WaitForCompact();
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
  ASSERT_EQ(num_compactions_running.load(), 0);
  ASSERT_TRUE(has_parallel.load());
  for (int i = num_keys; i < num_keys * 2; i++) {
    ASSERT_EQ(Get(1, Key(i % num_keys)), Key(i));
  }
  ReopenWithColumnFamilies({"default", "pikachu"}, options);
  for (int i = num_keys; i < num_keys * 2; i++) {
    ASSERT_EQ(Get(1, Key(i % num_keys)), Key(i));
  }
}
INSTANTIATE_TEST_CASE_P(DBTestUniversalCompactionParallel,
                        DBTestUniversalCompactionParallel,
                        ::testing::Values(1, 10));
TEST_P(DBTestUniversalCompaction, UniversalCompactionOptions) {
  Options options;
  options.compaction_style = kCompactionStyleUniversal;
  options.write_buffer_size = 100 << 10;
  options.target_file_size_base = 32 << 10;
  options.level0_file_num_compaction_trigger = 4;
  options.num_levels = num_levels_;
  options.compaction_options_universal.compression_size_percent = -1;
  options = CurrentOptions(options);
  DestroyAndReopen(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  Random rnd(301);
  int key_idx = 0;
  for (int num = 0; num < options.level0_file_num_compaction_trigger; num++) {
    for (int i = 0; i < 11; i++) {
      ASSERT_OK(Put(1, Key(key_idx), RandomString(&rnd, 10000)));
      key_idx++;
    }
    dbfull()->TEST_WaitForFlushMemTable(handles_[1]);
    if (num < options.level0_file_num_compaction_trigger - 1) {
      ASSERT_EQ(NumSortedRuns(1), num + 1);
    }
  }
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ(NumSortedRuns(1), 1);
}
TEST_P(DBTestUniversalCompaction, UniversalCompactionStopStyleSimilarSize) {
  Options options = CurrentOptions();
  options.compaction_style = kCompactionStyleUniversal;
  options.write_buffer_size = 100 << 10;
  options.target_file_size_base = 32 << 10;
  options.level0_file_num_compaction_trigger = 4;
  options.compaction_options_universal.size_ratio = 10;
  options.compaction_options_universal.stop_style =
      kCompactionStopStyleSimilarSize;
  options.num_levels = num_levels_;
  DestroyAndReopen(options);
  Random rnd(301);
  int key_idx = 0;
  for (int num = 0; num < options.level0_file_num_compaction_trigger - 1;
       num++) {
    for (int i = 0; i < 11; i++) {
      ASSERT_OK(Put(Key(key_idx), RandomString(&rnd, 10000)));
      key_idx++;
    }
    dbfull()->TEST_WaitForFlushMemTable();
    ASSERT_EQ(NumSortedRuns(), num + 1);
  }
  for (int i = 0; i < 11; i++) {
    ASSERT_OK(Put(Key(key_idx), RandomString(&rnd, 10000)));
    key_idx++;
  }
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ(NumSortedRuns(), 1);
  dbfull()->Flush(FlushOptions());
  for (int num = 0; num < options.level0_file_num_compaction_trigger - 3;
       num++) {
    for (int i = 0; i < 11; i++) {
      ASSERT_OK(Put(Key(key_idx), RandomString(&rnd, 10000)));
      key_idx++;
    }
    dbfull()->TEST_WaitForFlushMemTable();
    ASSERT_EQ(NumSortedRuns(), num + 3);
  }
  for (int i = 0; i < 11; i++) {
    ASSERT_OK(Put(Key(key_idx), RandomString(&rnd, 10000)));
    key_idx++;
  }
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ(NumSortedRuns(), 3);
  for (int i = 0; i < 11; i++) {
    ASSERT_OK(Put(Key(key_idx), RandomString(&rnd, 10000)));
    key_idx++;
  }
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ(NumSortedRuns(), 4);
}
TEST_F(DBTest, CompressedCache) {
  if (!Snappy_Supported()) {
    return;
  }
  int num_iter = 80;
  for (int iter = 0; iter < 4; iter++) {
    Options options;
    options.write_buffer_size = 64*1024;
    options.statistics = rocksdb::CreateDBStatistics();
    options = CurrentOptions(options);
    BlockBasedTableOptions table_options;
    switch (iter) {
      case 0:
        table_options.block_cache = NewLRUCache(8*1024);
        table_options.block_cache_compressed = nullptr;
        options.table_factory.reset(NewBlockBasedTableFactory(table_options));
        break;
      case 1:
        table_options.no_block_cache = true;
        table_options.block_cache = nullptr;
        table_options.block_cache_compressed = NewLRUCache(8*1024);
        options.table_factory.reset(NewBlockBasedTableFactory(table_options));
        break;
      case 2:
        table_options.block_cache = NewLRUCache(1024);
        table_options.block_cache_compressed = NewLRUCache(8*1024);
        options.table_factory.reset(NewBlockBasedTableFactory(table_options));
        break;
      case 3:
        table_options.block_cache = NewLRUCache(1024 * 1024);
        table_options.block_cache_compressed = NewLRUCache(8 * 1024 * 1024);
        options.table_factory.reset(NewBlockBasedTableFactory(table_options));
        options.compression = kNoCompression;
        break;
      default:
        ASSERT_TRUE(false);
    }
    CreateAndReopenWithCF({"pikachu"}, options);
    Options no_block_cache_opts;
    no_block_cache_opts.statistics = options.statistics;
    no_block_cache_opts = CurrentOptions(no_block_cache_opts);
    BlockBasedTableOptions table_options_no_bc;
    table_options_no_bc.no_block_cache = true;
    no_block_cache_opts.table_factory.reset(
        NewBlockBasedTableFactory(table_options_no_bc));
    ReopenWithColumnFamilies({"default", "pikachu"},
        std::vector<Options>({no_block_cache_opts, options}));
    Random rnd(301);
    ASSERT_EQ(NumTableFilesAtLevel(0, 1), 0);
    std::vector<std::string> values;
    std::string str;
    for (int i = 0; i < num_iter; i++) {
      if (i % 4 == 0) {
        str = RandomString(&rnd, 1000);
      }
      values.push_back(str);
      ASSERT_OK(Put(1, Key(i), values[i]));
    }
    ASSERT_OK(Flush(1));
    for (int i = 0; i < num_iter; i++) {
      ASSERT_EQ(Get(1, Key(i)), values[i]);
    }
    switch (iter) {
      case 0:
        ASSERT_GT(TestGetTickerCount(options, BLOCK_CACHE_MISS), 0);
        ASSERT_EQ(TestGetTickerCount(options, BLOCK_CACHE_COMPRESSED_MISS), 0);
        break;
      case 1:
        ASSERT_EQ(TestGetTickerCount(options, BLOCK_CACHE_MISS), 0);
        ASSERT_GT(TestGetTickerCount(options, BLOCK_CACHE_COMPRESSED_MISS), 0);
        break;
      case 2:
        ASSERT_GT(TestGetTickerCount(options, BLOCK_CACHE_MISS), 0);
        ASSERT_GT(TestGetTickerCount(options, BLOCK_CACHE_COMPRESSED_MISS), 0);
        break;
      case 3:
        ASSERT_GT(TestGetTickerCount(options, BLOCK_CACHE_MISS), 0);
        ASSERT_GT(TestGetTickerCount(options, BLOCK_CACHE_HIT), 0);
        ASSERT_GT(TestGetTickerCount(options, BLOCK_CACHE_COMPRESSED_MISS), 0);
        ASSERT_EQ(TestGetTickerCount(options, BLOCK_CACHE_COMPRESSED_HIT), 0);
        break;
      default:
        ASSERT_TRUE(false);
    }
    options.create_if_missing = true;
    DestroyAndReopen(options);
  }
}
static std::string CompressibleString(Random* rnd, int len) {
  std::string r;
  test::CompressibleString(rnd, 0.8, len, &r);
  return r;
}
TEST_P(DBTestUniversalCompaction, UniversalCompactionCompressRatio1) {
  if (!Snappy_Supported()) {
    return;
  }
  Options options;
  options.compaction_style = kCompactionStyleUniversal;
  options.write_buffer_size = 100 << 10;
  options.target_file_size_base = 32 << 10;
  options.level0_file_num_compaction_trigger = 2;
  options.num_levels = num_levels_;
  options.compaction_options_universal.compression_size_percent = 70;
  options = CurrentOptions(options);
  DestroyAndReopen(options);
  Random rnd(301);
  int key_idx = 0;
  for (int num = 0; num < 2; num++) {
    for (int i = 0; i < 11; i++) {
      ASSERT_OK(Put(Key(key_idx), CompressibleString(&rnd, 10000)));
      key_idx++;
    }
    dbfull()->TEST_WaitForFlushMemTable();
    dbfull()->TEST_WaitForCompact();
  }
  ASSERT_LT(TotalSize(), 110000U * 2 * 0.9);
  for (int num = 0; num < 2; num++) {
    for (int i = 0; i < 11; i++) {
      ASSERT_OK(Put(Key(key_idx), CompressibleString(&rnd, 10000)));
      key_idx++;
    }
    dbfull()->TEST_WaitForFlushMemTable();
    dbfull()->TEST_WaitForCompact();
  }
  ASSERT_LT(TotalSize(), 110000 * 4 * 0.9);
  for (int num = 0; num < 2; num++) {
    for (int i = 0; i < 11; i++) {
      ASSERT_OK(Put(Key(key_idx), CompressibleString(&rnd, 10000)));
      key_idx++;
    }
    dbfull()->TEST_WaitForFlushMemTable();
    dbfull()->TEST_WaitForCompact();
  }
  ASSERT_LT(TotalSize(), 110000 * 6 * 0.9);
  for (int num = 0; num < 8; num++) {
    for (int i = 0; i < 11; i++) {
      ASSERT_OK(Put(Key(key_idx), CompressibleString(&rnd, 10000)));
      key_idx++;
    }
    dbfull()->TEST_WaitForFlushMemTable();
    dbfull()->TEST_WaitForCompact();
  }
  ASSERT_GT(TotalSize(), 110000 * 11 * 0.8 + 110000 * 2);
}
TEST_P(DBTestUniversalCompaction, UniversalCompactionCompressRatio2) {
  if (!Snappy_Supported()) {
    return;
  }
  Options options;
  options.compaction_style = kCompactionStyleUniversal;
  options.write_buffer_size = 100 << 10;
  options.target_file_size_base = 32 << 10;
  options.level0_file_num_compaction_trigger = 2;
  options.num_levels = num_levels_;
  options.compaction_options_universal.compression_size_percent = 95;
  options = CurrentOptions(options);
  DestroyAndReopen(options);
  Random rnd(301);
  int key_idx = 0;
  for (int num = 0; num < 14; num++) {
    for (int i = 0; i < 12; i++) {
      ASSERT_OK(Put(Key(key_idx), CompressibleString(&rnd, 10000)));
      key_idx++;
    }
    dbfull()->TEST_WaitForFlushMemTable();
    dbfull()->TEST_WaitForCompact();
  }
  ASSERT_LT(TotalSize(), 120000U * 12 * 0.8 + 120000 * 2);
}
INSTANTIATE_TEST_CASE_P(UniversalCompactionNumLevels, DBTestUniversalCompaction,
                        ::testing::Values(1, 3, 5));
TEST_F(DBTest, FailMoreDbPaths) {
  Options options = CurrentOptions();
  options.db_paths.emplace_back(dbname_, 10000000);
  options.db_paths.emplace_back(dbname_ + "_2", 1000000);
  options.db_paths.emplace_back(dbname_ + "_3", 1000000);
  options.db_paths.emplace_back(dbname_ + "_4", 1000000);
  options.db_paths.emplace_back(dbname_ + "_5", 1000000);
  ASSERT_TRUE(TryReopen(options).IsNotSupported());
}
TEST_F(DBTest, UniversalCompactionSecondPathRatio) {
  if (!Snappy_Supported()) {
    return;
  }
  Options options;
  options.db_paths.emplace_back(dbname_, 500 * 1024);
  options.db_paths.emplace_back(dbname_ + "_2", 1024 * 1024 * 1024);
  options.compaction_style = kCompactionStyleUniversal;
  options.write_buffer_size = 100 << 10;
  options.level0_file_num_compaction_trigger = 2;
  options.num_levels = 1;
  options = CurrentOptions(options);
  std::vector<std::string> filenames;
  env_->GetChildren(options.db_paths[1].path, &filenames);
  for (size_t i = 0; i < filenames.size(); ++i) {
    env_->DeleteFile(options.db_paths[1].path + "/" + filenames[i]);
  }
  env_->DeleteDir(options.db_paths[1].path);
  Reopen(options);
  Random rnd(301);
  int key_idx = 0;
  for (int num = 0; num < 3; num++) {
    GenerateNewFile(&rnd, &key_idx);
  }
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(2, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(0, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(2, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(2, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(0, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(2, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  for (int i = 0; i < key_idx; i++) {
    auto v = Get(Key(i));
    ASSERT_NE(v, "NOT_FOUND");
    ASSERT_TRUE(v.size() == 1 || v.size() == 10000);
  }
  Reopen(options);
  for (int i = 0; i < key_idx; i++) {
    auto v = Get(Key(i));
    ASSERT_NE(v, "NOT_FOUND");
    ASSERT_TRUE(v.size() == 1 || v.size() == 10000);
  }
  Destroy(options);
}
TEST_F(DBTest, LevelCompactionThirdPath) {
  Options options = CurrentOptions();
  options.db_paths.emplace_back(dbname_, 500 * 1024);
  options.db_paths.emplace_back(dbname_ + "_2", 4 * 1024 * 1024);
  options.db_paths.emplace_back(dbname_ + "_3", 1024 * 1024 * 1024);
  options.compaction_style = kCompactionStyleLevel;
  options.write_buffer_size = 100 << 10;
  options.level0_file_num_compaction_trigger = 2;
  options.num_levels = 4;
  options.max_bytes_for_level_base = 400 * 1024;
  std::vector<std::string> filenames;
  env_->GetChildren(options.db_paths[1].path, &filenames);
  for (size_t i = 0; i < filenames.size(); ++i) {
    env_->DeleteFile(options.db_paths[1].path + "/" + filenames[i]);
  }
  env_->DeleteDir(options.db_paths[1].path);
  Reopen(options);
  Random rnd(301);
  int key_idx = 0;
  for (int num = 0; num < 3; num++) {
    GenerateNewFile(&rnd, &key_idx);
  }
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(3, GetSstFileCount(options.db_paths[1].path));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4", FilesPerLevel(0));
  ASSERT_EQ(4, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,1", FilesPerLevel(0));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(4, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,2", FilesPerLevel(0));
  ASSERT_EQ(2, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(4, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,3", FilesPerLevel(0));
  ASSERT_EQ(3, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(4, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,4", FilesPerLevel(0));
  ASSERT_EQ(4, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(4, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,5", FilesPerLevel(0));
  ASSERT_EQ(5, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(4, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,6", FilesPerLevel(0));
  ASSERT_EQ(6, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(4, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,7", FilesPerLevel(0));
  ASSERT_EQ(7, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(4, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,8", FilesPerLevel(0));
  ASSERT_EQ(8, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(4, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  for (int i = 0; i < key_idx; i++) {
    auto v = Get(Key(i));
    ASSERT_NE(v, "NOT_FOUND");
    ASSERT_TRUE(v.size() == 1 || v.size() == 10000);
  }
  Reopen(options);
  for (int i = 0; i < key_idx; i++) {
    auto v = Get(Key(i));
    ASSERT_NE(v, "NOT_FOUND");
    ASSERT_TRUE(v.size() == 1 || v.size() == 10000);
  }
  Destroy(options);
}
TEST_F(DBTest, LevelCompactionPathUse) {
  Options options = CurrentOptions();
  options.db_paths.emplace_back(dbname_, 500 * 1024);
  options.db_paths.emplace_back(dbname_ + "_2", 4 * 1024 * 1024);
  options.db_paths.emplace_back(dbname_ + "_3", 1024 * 1024 * 1024);
  options.compaction_style = kCompactionStyleLevel;
  options.write_buffer_size = 100 << 10;
  options.level0_file_num_compaction_trigger = 2;
  options.num_levels = 4;
  options.max_bytes_for_level_base = 400 * 1024;
  std::vector<std::string> filenames;
  env_->GetChildren(options.db_paths[1].path, &filenames);
  for (size_t i = 0; i < filenames.size(); ++i) {
    env_->DeleteFile(options.db_paths[1].path + "/" + filenames[i]);
  }
  env_->DeleteDir(options.db_paths[1].path);
  Reopen(options);
  Random rnd(301);
  int key_idx = 0;
  for (int num = 0; num < 3; num++) {
    key_idx = 0;
    GenerateNewFile(&rnd, &key_idx);
  }
  key_idx = 0;
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  key_idx = 0;
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,1", FilesPerLevel(0));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  key_idx = 0;
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("0,1", FilesPerLevel(0));
  ASSERT_EQ(0, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(0, GetSstFileCount(dbname_));
  key_idx = 0;
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,1", FilesPerLevel(0));
  ASSERT_EQ(0, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  key_idx = 0;
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("0,1", FilesPerLevel(0));
  ASSERT_EQ(0, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(0, GetSstFileCount(dbname_));
  key_idx = 0;
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,1", FilesPerLevel(0));
  ASSERT_EQ(0, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  key_idx = 0;
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("0,1", FilesPerLevel(0));
  ASSERT_EQ(0, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(0, GetSstFileCount(dbname_));
  key_idx = 0;
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,1", FilesPerLevel(0));
  ASSERT_EQ(0, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  key_idx = 0;
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("0,1", FilesPerLevel(0));
  ASSERT_EQ(0, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(0, GetSstFileCount(dbname_));
  key_idx = 0;
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,1", FilesPerLevel(0));
  ASSERT_EQ(0, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  for (int i = 0; i < key_idx; i++) {
    auto v = Get(Key(i));
    ASSERT_NE(v, "NOT_FOUND");
    ASSERT_TRUE(v.size() == 1 || v.size() == 10000);
  }
  Reopen(options);
  for (int i = 0; i < key_idx; i++) {
    auto v = Get(Key(i));
    ASSERT_NE(v, "NOT_FOUND");
    ASSERT_TRUE(v.size() == 1 || v.size() == 10000);
  }
  Destroy(options);
}
TEST_F(DBTest, UniversalCompactionFourPaths) {
  Options options;
  options.db_paths.emplace_back(dbname_, 300 * 1024);
  options.db_paths.emplace_back(dbname_ + "_2", 300 * 1024);
  options.db_paths.emplace_back(dbname_ + "_3", 500 * 1024);
  options.db_paths.emplace_back(dbname_ + "_4", 1024 * 1024 * 1024);
  options.compaction_style = kCompactionStyleUniversal;
  options.write_buffer_size = 100 << 10;
  options.level0_file_num_compaction_trigger = 2;
  options.num_levels = 1;
  options = CurrentOptions(options);
  std::vector<std::string> filenames;
  env_->GetChildren(options.db_paths[1].path, &filenames);
  for (size_t i = 0; i < filenames.size(); ++i) {
    env_->DeleteFile(options.db_paths[1].path + "/" + filenames[i]);
  }
  env_->DeleteDir(options.db_paths[1].path);
  Reopen(options);
  Random rnd(301);
  int key_idx = 0;
  for (int num = 0; num < 3; num++) {
    GenerateNewFile(&rnd, &key_idx);
  }
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[2].path));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(0, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[3].path));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[3].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[3].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[3].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[3].path));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[3].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[2].path));
  ASSERT_EQ(1, GetSstFileCount(dbname_));
  for (int i = 0; i < key_idx; i++) {
    auto v = Get(Key(i));
    ASSERT_NE(v, "NOT_FOUND");
    ASSERT_TRUE(v.size() == 1 || v.size() == 10000);
  }
  Reopen(options);
  for (int i = 0; i < key_idx; i++) {
    auto v = Get(Key(i));
    ASSERT_NE(v, "NOT_FOUND");
    ASSERT_TRUE(v.size() == 1 || v.size() == 10000);
  }
  Destroy(options);
}
void CheckColumnFamilyMeta(const ColumnFamilyMetaData& cf_meta) {
  uint64_t cf_size = 0;
  uint64_t cf_csize = 0;
  size_t file_count = 0;
  for (auto level_meta : cf_meta.levels) {
    uint64_t level_size = 0;
    uint64_t level_csize = 0;
    file_count += level_meta.files.size();
    for (auto file_meta : level_meta.files) {
      level_size += file_meta.size;
    }
    ASSERT_EQ(level_meta.size, level_size);
    cf_size += level_size;
    cf_csize += level_csize;
  }
  ASSERT_EQ(cf_meta.file_count, file_count);
  ASSERT_EQ(cf_meta.size, cf_size);
}
TEST_F(DBTest, ColumnFamilyMetaDataTest) {
  Options options = CurrentOptions();
  options.create_if_missing = true;
  DestroyAndReopen(options);
  Random rnd(301);
  int key_index = 0;
  ColumnFamilyMetaData cf_meta;
  for (int i = 0; i < 100; ++i) {
    GenerateNewFile(&rnd, &key_index);
    db_->GetColumnFamilyMetaData(&cf_meta);
    CheckColumnFamilyMeta(cf_meta);
  }
}
TEST_F(DBTest, ConvertCompactionStyle) {
  Random rnd(301);
  int max_key_level_insert = 200;
  int max_key_universal_insert = 600;
  Options options;
  options.write_buffer_size = 100<<10;
  options.num_levels = 4;
  options.level0_file_num_compaction_trigger = 3;
  options.max_bytes_for_level_base = 500<<10;
  options.max_bytes_for_level_multiplier = 1;
  options.target_file_size_base = 200<<10;
  options.target_file_size_multiplier = 1;
  options = CurrentOptions(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  for (int i = 0; i <= max_key_level_insert; i++) {
    ASSERT_OK(Put(1, Key(i), RandomString(&rnd, 10000)));
  }
  ASSERT_OK(Flush(1));
  dbfull()->TEST_WaitForCompact();
  ASSERT_GT(TotalTableFiles(1, 4), 1);
  int non_level0_num_files = 0;
  for (int i = 1; i < options.num_levels; i++) {
    non_level0_num_files += NumTableFilesAtLevel(i, 1);
  }
  ASSERT_GT(non_level0_num_files, 0);
  options = CurrentOptions();
  options.compaction_style = kCompactionStyleUniversal;
  options.num_levels = 1;
  options = CurrentOptions(options);
  Status s = TryReopenWithColumnFamilies({"default", "pikachu"}, options);
  ASSERT_TRUE(s.IsInvalidArgument());
  options = CurrentOptions();
  options.disable_auto_compactions = true;
  options.target_file_size_base = INT_MAX;
  options.target_file_size_multiplier = 1;
  options.max_bytes_for_level_base = INT_MAX;
  options.max_bytes_for_level_multiplier = 1;
  options.num_levels = 4;
  options = CurrentOptions(options);
  ReopenWithColumnFamilies({"default", "pikachu"}, options);
  CompactRangeOptions compact_options;
  compact_options.change_level = true;
  compact_options.target_level = 0;
  compact_options.bottommost_level_compaction =
      BottommostLevelCompaction::kForce;
  dbfull()->CompactRange(compact_options, handles_[1], nullptr, nullptr);
  ASSERT_EQ("1", FilesPerLevel(1));
  options = CurrentOptions();
  options.compaction_style = kCompactionStyleUniversal;
  options.num_levels = 4;
  options.write_buffer_size = 100<<10;
  options.level0_file_num_compaction_trigger = 3;
  options = CurrentOptions(options);
  ReopenWithColumnFamilies({"default", "pikachu"}, options);
  options.num_levels = 1;
  ReopenWithColumnFamilies({"default", "pikachu"}, options);
  for (int i = max_key_level_insert / 2; i <= max_key_universal_insert; i++) {
    ASSERT_OK(Put(1, Key(i), RandomString(&rnd, 10000)));
  }
  dbfull()->Flush(FlushOptions());
  ASSERT_OK(Flush(1));
  dbfull()->TEST_WaitForCompact();
  for (int i = 1; i < options.num_levels; i++) {
    ASSERT_EQ(NumTableFilesAtLevel(i, 1), 0);
  }
  std::string keys_in_db;
  Iterator* iter = dbfull()->NewIterator(ReadOptions(), handles_[1]);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    keys_in_db.append(iter->key().ToString());
    keys_in_db.push_back(',');
  }
  delete iter;
  std::string expected_keys;
  for (int i = 0; i <= max_key_universal_insert; i++) {
    expected_keys.append(Key(i));
    expected_keys.push_back(',');
  }
  ASSERT_EQ(keys_in_db, expected_keys);
}
TEST_F(DBTest, IncreaseUniversalCompactionNumLevels) {
  std::function<void(int)> verify_func = [&](int num_keys_in_db) {
    std::string keys_in_db;
    Iterator* iter = dbfull()->NewIterator(ReadOptions(), handles_[1]);
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      keys_in_db.append(iter->key().ToString());
      keys_in_db.push_back(',');
    }
    delete iter;
    std::string expected_keys;
    for (int i = 0; i <= num_keys_in_db; i++) {
      expected_keys.append(Key(i));
      expected_keys.push_back(',');
    }
    ASSERT_EQ(keys_in_db, expected_keys);
  };
  Random rnd(301);
  int max_key1 = 200;
  int max_key2 = 600;
  int max_key3 = 800;
  Options options = CurrentOptions();
  options.compaction_style = kCompactionStyleUniversal;
  options.num_levels = 1;
  options.write_buffer_size = 100 << 10;
  options.level0_file_num_compaction_trigger = 3;
  options = CurrentOptions(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  for (int i = 0; i <= max_key1; i++) {
    ASSERT_OK(Put(1, Key(i), RandomString(&rnd, 10000)));
  }
  ASSERT_OK(Flush(1));
  dbfull()->TEST_WaitForCompact();
  int non_level0_num_files = 0;
  for (int i = 1; i < options.num_levels; i++) {
    non_level0_num_files += NumTableFilesAtLevel(i, 1);
  }
  ASSERT_EQ(non_level0_num_files, 0);
  options.compaction_style = kCompactionStyleUniversal;
  options.num_levels = 4;
  options = CurrentOptions(options);
  ReopenWithColumnFamilies({"default", "pikachu"}, options);
  verify_func(max_key1);
  for (int i = max_key1 + 1; i <= max_key2; i++) {
    ASSERT_OK(Put(1, Key(i), RandomString(&rnd, 10000)));
  }
  ASSERT_OK(Flush(1));
  dbfull()->TEST_WaitForCompact();
  verify_func(max_key2);
  ASSERT_GT(NumTableFilesAtLevel(options.num_levels - 1, 1), 0);
  options.num_levels = 4;
  options.target_file_size_base = INT_MAX;
  ReopenWithColumnFamilies({"default", "pikachu"}, options);
  CompactRangeOptions compact_options;
  compact_options.change_level = true;
  compact_options.target_level = 0;
  dbfull()->CompactRange(compact_options, handles_[1], nullptr, nullptr);
  ReopenWithColumnFamilies({"default", "pikachu"}, options);
  options.compaction_style = kCompactionStyleUniversal;
  options.num_levels = 1;
  options = CurrentOptions(options);
  ReopenWithColumnFamilies({"default", "pikachu"}, options);
  for (int i = max_key2 + 1; i <= max_key3; i++) {
    ASSERT_OK(Put(1, Key(i), RandomString(&rnd, 10000)));
  }
  ASSERT_OK(Flush(1));
  dbfull()->TEST_WaitForCompact();
  verify_func(max_key3);
}
namespace {
void MinLevelHelper(DBTest* self, Options& options) {
  Random rnd(301);
  for (int num = 0;
    num < options.level0_file_num_compaction_trigger - 1;
    num++)
  {
    std::vector<std::string> values;
    for (int i = 0; i < 12; i++) {
      values.push_back(RandomString(&rnd, 10000));
      ASSERT_OK(self->Put(Key(i), values[i]));
    }
    self->dbfull()->TEST_WaitForFlushMemTable();
    ASSERT_EQ(self->NumTableFilesAtLevel(0), num + 1);
  }
  std::vector<std::string> values;
  for (int i = 0; i < 12; i++) {
    values.push_back(RandomString(&rnd, 10000));
    ASSERT_OK(self->Put(Key(i), values[i]));
  }
  self->dbfull()->TEST_WaitForCompact();
  ASSERT_EQ(self->NumTableFilesAtLevel(0), 0);
  ASSERT_EQ(self->NumTableFilesAtLevel(1), 1);
}
bool MinLevelToCompress(CompressionType& type, Options& options, int wbits,
                        int lev, int strategy) {
  fprintf(stderr, "Test with compression options : window_bits = %d, level =  %d, strategy = %d}\n", wbits, lev, strategy);
  options.write_buffer_size = 100<<10;
  options.num_levels = 3;
  options.max_mem_compaction_level = 0;
  options.level0_file_num_compaction_trigger = 3;
  options.create_if_missing = true;
  if (Snappy_Supported()) {
    type = kSnappyCompression;
    fprintf(stderr, "using snappy\n");
  } else if (Zlib_Supported()) {
    type = kZlibCompression;
    fprintf(stderr, "using zlib\n");
  } else if (BZip2_Supported()) {
    type = kBZip2Compression;
    fprintf(stderr, "using bzip2\n");
  } else if (LZ4_Supported()) {
    type = kLZ4Compression;
    fprintf(stderr, "using lz4\n");
  } else {
    fprintf(stderr, "skipping test, compression disabled\n");
    return false;
  }
  options.compression_per_level.resize(options.num_levels);
  for (int i = 0; i < 1; i++) {
    options.compression_per_level[i] = kNoCompression;
  }
  for (int i = 1; i < options.num_levels; i++) {
    options.compression_per_level[i] = type;
  }
  return true;
}
}
TEST_F(DBTest, MinLevelToCompress1) {
  Options options = CurrentOptions();
  CompressionType type = kSnappyCompression;
  if (!MinLevelToCompress(type, options, -14, -1, 0)) {
    return;
  }
  Reopen(options);
  MinLevelHelper(this, options);
  for (int i = 0; i < 2; i++) {
    options.compression_per_level[i] = kNoCompression;
  }
  for (int i = 2; i < options.num_levels; i++) {
    options.compression_per_level[i] = type;
  }
  DestroyAndReopen(options);
  MinLevelHelper(this, options);
}
TEST_F(DBTest, MinLevelToCompress2) {
  Options options = CurrentOptions();
  CompressionType type = kSnappyCompression;
  if (!MinLevelToCompress(type, options, 15, -1, 0)) {
    return;
  }
  Reopen(options);
  MinLevelHelper(this, options);
  for (int i = 0; i < 2; i++) {
    options.compression_per_level[i] = kNoCompression;
  }
  for (int i = 2; i < options.num_levels; i++) {
    options.compression_per_level[i] = type;
  }
  DestroyAndReopen(options);
  MinLevelHelper(this, options);
}
TEST_F(DBTest, RepeatedWritesToSameKey) {
  do {
    Options options;
    options.env = env_;
    options.write_buffer_size = 100000;
    options = CurrentOptions(options);
    CreateAndReopenWithCF({"pikachu"}, options);
    const int kMaxFiles =
        options.num_levels + options.level0_stop_writes_trigger;
    Random rnd(301);
    std::string value =
        RandomString(&rnd, static_cast<int>(2 * options.write_buffer_size));
    for (int i = 0; i < 5 * kMaxFiles; i++) {
      ASSERT_OK(Put(1, "key", value));
      ASSERT_LE(TotalTableFiles(1), kMaxFiles);
    }
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, InPlaceUpdate) {
  do {
    Options options;
    options.create_if_missing = true;
    options.inplace_update_support = true;
    options.env = env_;
    options.write_buffer_size = 100000;
    options = CurrentOptions(options);
    CreateAndReopenWithCF({"pikachu"}, options);
    int numValues = 10;
    for (int i = numValues; i > 0; i--) {
      std::string value = DummyString(i, 'a');
      ASSERT_OK(Put(1, "key", value));
      ASSERT_EQ(value, Get(1, "key"));
    }
    validateNumberOfEntries(1, 1);
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, InPlaceUpdateLargeNewValue) {
  do {
    Options options;
    options.create_if_missing = true;
    options.inplace_update_support = true;
    options.env = env_;
    options.write_buffer_size = 100000;
    options = CurrentOptions(options);
    CreateAndReopenWithCF({"pikachu"}, options);
    int numValues = 10;
    for (int i = 0; i < numValues; i++) {
      std::string value = DummyString(i, 'a');
      ASSERT_OK(Put(1, "key", value));
      ASSERT_EQ(value, Get(1, "key"));
    }
    validateNumberOfEntries(numValues, 1);
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, InPlaceUpdateCallbackSmallerSize) {
  do {
    Options options;
    options.create_if_missing = true;
    options.inplace_update_support = true;
    options.env = env_;
    options.write_buffer_size = 100000;
    options.inplace_callback =
      rocksdb::DBTest::updateInPlaceSmallerSize;
    options = CurrentOptions(options);
    CreateAndReopenWithCF({"pikachu"}, options);
    int numValues = 10;
    ASSERT_OK(Put(1, "key", DummyString(numValues, 'a')));
    ASSERT_EQ(DummyString(numValues, 'c'), Get(1, "key"));
    for (int i = numValues; i > 0; i--) {
      ASSERT_OK(Put(1, "key", DummyString(i, 'a')));
      ASSERT_EQ(DummyString(i - 1, 'b'), Get(1, "key"));
    }
    validateNumberOfEntries(1, 1);
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, InPlaceUpdateCallbackSmallerVarintSize) {
  do {
    Options options;
    options.create_if_missing = true;
    options.inplace_update_support = true;
    options.env = env_;
    options.write_buffer_size = 100000;
    options.inplace_callback =
      rocksdb::DBTest::updateInPlaceSmallerVarintSize;
    options = CurrentOptions(options);
    CreateAndReopenWithCF({"pikachu"}, options);
    int numValues = 265;
    ASSERT_OK(Put(1, "key", DummyString(numValues, 'a')));
    ASSERT_EQ(DummyString(numValues, 'c'), Get(1, "key"));
    for (int i = numValues; i > 0; i--) {
      ASSERT_OK(Put(1, "key", DummyString(i, 'a')));
      ASSERT_EQ(DummyString(1, 'b'), Get(1, "key"));
    }
    validateNumberOfEntries(1, 1);
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, InPlaceUpdateCallbackLargeNewValue) {
  do {
    Options options;
    options.create_if_missing = true;
    options.inplace_update_support = true;
    options.env = env_;
    options.write_buffer_size = 100000;
    options.inplace_callback =
      rocksdb::DBTest::updateInPlaceLargerSize;
    options = CurrentOptions(options);
    CreateAndReopenWithCF({"pikachu"}, options);
    int numValues = 10;
    for (int i = 0; i < numValues; i++) {
      ASSERT_OK(Put(1, "key", DummyString(i, 'a')));
      ASSERT_EQ(DummyString(i, 'c'), Get(1, "key"));
    }
    validateNumberOfEntries(numValues, 1);
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, InPlaceUpdateCallbackNoAction) {
  do {
    Options options;
    options.create_if_missing = true;
    options.inplace_update_support = true;
    options.env = env_;
    options.write_buffer_size = 100000;
    options.inplace_callback =
      rocksdb::DBTest::updateInPlaceNoAction;
    options = CurrentOptions(options);
    CreateAndReopenWithCF({"pikachu"}, options);
    ASSERT_OK(Put(1, "key", DummyString(1, 'a')));
    ASSERT_EQ(Get(1, "key"), "NOT_FOUND");
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, CompactionFilter) {
  Options options = CurrentOptions();
  options.max_open_files = -1;
  options.num_levels = 3;
  options.max_mem_compaction_level = 0;
  options.compaction_filter_factory = std::make_shared<KeepFilterFactory>();
  options = CurrentOptions(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  const std::string value(10, 'x');
  for (int i = 0; i < 100000; i++) {
    char key[100];
    snprintf(key, sizeof(key), "B%010d", i);
    Put(1, key, value);
  }
  ASSERT_OK(Flush(1));
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
  int count = 0;
  int total = 0;
  Arena arena;
  {
    ScopedArenaIterator iter(
        dbfull()->TEST_NewInternalIterator(&arena, handles_[1]));
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
  }
  ASSERT_EQ(total, 100000);
  ASSERT_EQ(count, 1);
  for (int i = 0; i < 100000; i++) {
    char key[100];
    snprintf(key, sizeof(key), "B%010d", i);
    ASSERT_OK(Put(1, key, value));
  }
  ASSERT_OK(Flush(1));
  cfilter_count = 0;
  dbfull()->TEST_CompactRange(0, nullptr, nullptr, handles_[1]);
  ASSERT_EQ(cfilter_count, 100000);
  cfilter_count = 0;
  dbfull()->TEST_CompactRange(1, nullptr, nullptr, handles_[1]);
  ASSERT_EQ(cfilter_count, 100000);
  ASSERT_EQ(NumTableFilesAtLevel(0, 1), 0);
  ASSERT_EQ(NumTableFilesAtLevel(1, 1), 0);
  ASSERT_NE(NumTableFilesAtLevel(2, 1), 0);
  options.compaction_filter_factory = std::make_shared<DeleteFilterFactory>();
  options.create_if_missing = true;
  DestroyAndReopen(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  for (int i = 0; i < 100000; i++) {
    char key[100];
    snprintf(key, sizeof(key), "B%010d", i);
    ASSERT_OK(Put(1, key, value));
  }
  ASSERT_OK(Flush(1));
  ASSERT_NE(NumTableFilesAtLevel(0, 1), 0);
  ASSERT_EQ(NumTableFilesAtLevel(1, 1), 0);
  ASSERT_EQ(NumTableFilesAtLevel(2, 1), 0);
  cfilter_count = 0;
  dbfull()->TEST_CompactRange(0, nullptr, nullptr, handles_[1]);
  ASSERT_EQ(cfilter_count, 100000);
  cfilter_count = 0;
  dbfull()->TEST_CompactRange(1, nullptr, nullptr, handles_[1]);
  ASSERT_EQ(cfilter_count, 0);
  ASSERT_EQ(NumTableFilesAtLevel(0, 1), 0);
  ASSERT_EQ(NumTableFilesAtLevel(1, 1), 0);
  {
    std::unique_ptr<Iterator> iter(
        db_->NewIterator(ReadOptions(), handles_[1]));
    iter->SeekToFirst();
    count = 0;
    while (iter->Valid()) {
      count++;
      iter->Next();
    }
    ASSERT_EQ(count, 0);
  }
  count = 0;
  {
    ScopedArenaIterator iter(
        dbfull()->TEST_NewInternalIterator(&arena, handles_[1]));
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
  }
}
TEST_F(DBTest, CompactionFilterDeletesAll) {
  Options options;
  options.compaction_filter_factory = std::make_shared<DeleteFilterFactory>();
  options.disable_auto_compactions = true;
  options.create_if_missing = true;
  options = CurrentOptions(options);
  DestroyAndReopen(options);
  for (int table = 0; table < 4; ++table) {
    for (int i = 0; i < 10 + table; ++i) {
      Put(ToString(table * 100 + i), "val");
    }
    Flush();
  }
  ASSERT_OK(db_->CompactRange(CompactRangeOptions(), nullptr, nullptr));
  ASSERT_EQ(0U, CountLiveFiles());
  Reopen(options);
  Iterator* itr = db_->NewIterator(ReadOptions());
  itr->SeekToFirst();
  ASSERT_TRUE(!itr->Valid());
  delete itr;
}
TEST_F(DBTest, CompactionFilterWithValueChange) {
  do {
    Options options;
    options.num_levels = 3;
    options.max_mem_compaction_level = 0;
    options.compaction_filter_factory =
      std::make_shared<ChangeFilterFactory>();
    options = CurrentOptions(options);
    CreateAndReopenWithCF({"pikachu"}, options);
    const std::string value(10, 'x');
    for (int i = 0; i < 100001; i++) {
      char key[100];
      snprintf(key, sizeof(key), "B%010d", i);
      Put(1, key, value);
    }
    ASSERT_OK(Flush(1));
    if (option_config_ != kUniversalCompactionMultiLevel) {
      dbfull()->TEST_CompactRange(0, nullptr, nullptr, handles_[1]);
      dbfull()->TEST_CompactRange(1, nullptr, nullptr, handles_[1]);
    } else {
      dbfull()->CompactRange(CompactRangeOptions(), handles_[1], nullptr,
                             nullptr);
    }
    for (int i = 0; i < 100001; i++) {
      char key[100];
      snprintf(key, sizeof(key), "B%010d", i);
      Put(1, key, value);
    }
    ASSERT_OK(Flush(1));
    if (option_config_ != kUniversalCompactionMultiLevel) {
      dbfull()->TEST_CompactRange(0, nullptr, nullptr, handles_[1]);
      dbfull()->TEST_CompactRange(1, nullptr, nullptr, handles_[1]);
    } else {
      dbfull()->CompactRange(CompactRangeOptions(), handles_[1], nullptr,
                             nullptr);
    }
    for (int i = 0; i < 100001; i++) {
      char key[100];
      snprintf(key, sizeof(key), "B%010d", i);
      std::string newvalue = Get(1, key);
      ASSERT_EQ(newvalue.compare(NEW_VALUE), 0);
    }
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, CompactionFilterWithMergeOperator) {
  std::string one, two, three, four;
  PutFixed64(&one, 1);
  PutFixed64(&two, 2);
  PutFixed64(&three, 3);
  PutFixed64(&four, 4);
  Options options;
  options = CurrentOptions(options);
  options.create_if_missing = true;
  options.merge_operator = MergeOperators::CreateUInt64AddOperator();
  options.num_levels = 3;
  options.max_mem_compaction_level = 0;
  options.compaction_filter_factory =
      std::make_shared<ConditionalFilterFactory>(two);
  DestroyAndReopen(options);
  ASSERT_OK(db_->Put(WriteOptions(), "foo", two));
  ASSERT_OK(Flush());
  ASSERT_OK(db_->Merge(WriteOptions(), "foo", one));
  ASSERT_OK(Flush());
  std::string newvalue = Get("foo");
  ASSERT_EQ(newvalue, three);
  dbfull()->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  newvalue = Get("foo");
  ASSERT_EQ(newvalue, three);
  ASSERT_OK(db_->Put(WriteOptions(), "bar", two));
  ASSERT_OK(Flush());
  dbfull()->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  newvalue = Get("bar");
  ASSERT_EQ("NOT_FOUND", newvalue);
  ASSERT_OK(db_->Merge(WriteOptions(), "bar", two));
  ASSERT_OK(Flush());
  dbfull()->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  newvalue = Get("bar");
  ASSERT_EQ(two, two);
  ASSERT_OK(db_->Put(WriteOptions(), "foobar", one));
  ASSERT_OK(Flush());
  ASSERT_OK(db_->Merge(WriteOptions(), "foobar", two));
  ASSERT_OK(Flush());
  newvalue = Get("foobar");
  ASSERT_EQ(newvalue, three);
  dbfull()->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  newvalue = Get("foobar");
  ASSERT_EQ(newvalue, three);
  ASSERT_OK(db_->Put(WriteOptions(), "barfoo", two));
  ASSERT_OK(Flush());
  ASSERT_OK(db_->Merge(WriteOptions(), "barfoo", two));
  ASSERT_OK(Flush());
  newvalue = Get("barfoo");
  ASSERT_EQ(newvalue, four);
  dbfull()->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  newvalue = Get("barfoo");
  ASSERT_EQ(newvalue, four);
}
TEST_F(DBTest, CompactionFilterContextManual) {
  KeepFilterFactory* filter = new KeepFilterFactory();
  Options options = CurrentOptions();
  options.compaction_style = kCompactionStyleUniversal;
  options.compaction_filter_factory.reset(filter);
  options.compression = kNoCompression;
  options.level0_file_num_compaction_trigger = 8;
  Reopen(options);
  int num_keys_per_file = 400;
  for (int j = 0; j < 3; j++) {
    const std::string value(10, 'x');
    for (int i = 0; i < num_keys_per_file; i++) {
      char key[100];
      snprintf(key, sizeof(key), "B%08d%02d", i, j);
      Put(key, value);
    }
    dbfull()->TEST_FlushMemTable();
    num_keys_per_file /= 2;
  }
  cfilter_count = 0;
  filter->expect_manual_compaction_.store(true);
  filter->expect_full_compaction_.store(false);
  dbfull()->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  ASSERT_EQ(cfilter_count, 700);
  ASSERT_EQ(NumSortedRuns(0), 1);
  {
    int count = 0;
    int total = 0;
    Arena arena;
    ScopedArenaIterator iter(dbfull()->TEST_NewInternalIterator(&arena));
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
    ASSERT_EQ(total, 700);
    ASSERT_EQ(count, 1);
  }
}
class KeepFilterV2 : public CompactionFilterV2 {
 public:
  virtual std::vector<bool> Filter(int level,
                                   const SliceVector& keys,
                                   const SliceVector& existing_values,
                                   std::vector<std::string>* new_values,
                                   std::vector<bool>* values_changed)
    const override {
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
  virtual const char* Name() const override {
    return "KeepFilterV2";
  }
};
class DeleteFilterV2 : public CompactionFilterV2 {
 public:
  virtual std::vector<bool> Filter(int level,
                                   const SliceVector& keys,
                                   const SliceVector& existing_values,
                                   std::vector<std::string>* new_values,
                                   std::vector<bool>* values_changed)
    const override {
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
  virtual const char* Name() const override {
    return "DeleteFilterV2";
  }
};
class ChangeFilterV2 : public CompactionFilterV2 {
 public:
  virtual std::vector<bool> Filter(int level,
                                   const SliceVector& keys,
                                   const SliceVector& existing_values,
                                   std::vector<std::string>* new_values,
                                   std::vector<bool>* values_changed)
    const override {
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
  virtual const char* Name() const override {
    return "ChangeFilterV2";
  }
};
class KeepFilterFactoryV2 : public CompactionFilterFactoryV2 {
 public:
  explicit KeepFilterFactoryV2(const SliceTransform* prefix_extractor)
    : CompactionFilterFactoryV2(prefix_extractor) { }
  virtual std::unique_ptr<CompactionFilterV2>
  CreateCompactionFilterV2(
      const CompactionFilterContext& context) override {
    return std::unique_ptr<CompactionFilterV2>(new KeepFilterV2());
  }
  virtual const char* Name() const override {
    return "KeepFilterFactoryV2";
  }
};
class DeleteFilterFactoryV2 : public CompactionFilterFactoryV2 {
 public:
  explicit DeleteFilterFactoryV2(const SliceTransform* prefix_extractor)
    : CompactionFilterFactoryV2(prefix_extractor) { }
  virtual std::unique_ptr<CompactionFilterV2>
  CreateCompactionFilterV2(
      const CompactionFilterContext& context) override {
    return std::unique_ptr<CompactionFilterV2>(new DeleteFilterV2());
  }
  virtual const char* Name() const override {
    return "DeleteFilterFactoryV2";
  }
};
class ChangeFilterFactoryV2 : public CompactionFilterFactoryV2 {
 public:
  explicit ChangeFilterFactoryV2(const SliceTransform* prefix_extractor)
    : CompactionFilterFactoryV2(prefix_extractor) { }
  virtual std::unique_ptr<CompactionFilterV2>
  CreateCompactionFilterV2(
      const CompactionFilterContext& context) override {
    return std::unique_ptr<CompactionFilterV2>(new ChangeFilterV2());
  }
  virtual const char* Name() const override {
    return "ChangeFilterFactoryV2";
  }
};
TEST_F(DBTest, CompactionFilterV2) {
  Options options = CurrentOptions();
  options.num_levels = 3;
  options.max_mem_compaction_level = 0;
  std::unique_ptr<const SliceTransform> prefix_extractor;
  prefix_extractor.reset(NewFixedPrefixTransform(8));
  options.compaction_filter_factory_v2
    = std::make_shared<KeepFilterFactoryV2>(prefix_extractor.get());
  option_config_ = kUniversalCompaction;
  options.compaction_style = (rocksdb::CompactionStyle)1;
  Reopen(options);
  const std::string value(10, 'x');
  for (int i = 0; i < 100000; i++) {
    char key[100];
    snprintf(key, sizeof(key), "B%08d%010d", i , i);
    Put(key, value);
  }
  dbfull()->TEST_FlushMemTable();
  dbfull()->TEST_CompactRange(0, nullptr, nullptr);
  dbfull()->TEST_CompactRange(1, nullptr, nullptr);
  ASSERT_EQ(NumSortedRuns(0), 1);
  int count = 0;
  int total = 0;
  {
    Arena arena;
    ScopedArenaIterator iter(dbfull()->TEST_NewInternalIterator(&arena));
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
  }
  ASSERT_EQ(total, 100000);
  ASSERT_EQ(count, 1);
  options.compaction_filter_factory_v2 =
    std::make_shared<DeleteFilterFactoryV2>(prefix_extractor.get());
  options.create_if_missing = true;
  DestroyAndReopen(options);
  for (int i = 0; i < 100000; i++) {
    char key[100];
    snprintf(key, sizeof(key), "B%08d%010d", i, i);
    Put(key, value);
  }
  dbfull()->TEST_FlushMemTable();
  ASSERT_NE(NumTableFilesAtLevel(0), 0);
  dbfull()->TEST_CompactRange(0, nullptr, nullptr);
  dbfull()->TEST_CompactRange(1, nullptr, nullptr);
  ASSERT_EQ(NumTableFilesAtLevel(1), 0);
  Iterator* iter = db_->NewIterator(ReadOptions());
  iter->SeekToFirst();
  count = 0;
  while (iter->Valid()) {
    count++;
    iter->Next();
  }
  ASSERT_EQ(count, 0);
  delete iter;
}
TEST_F(DBTest, CompactionFilterV2WithValueChange) {
  Options options = CurrentOptions();
  options.num_levels = 3;
  options.max_mem_compaction_level = 0;
  std::unique_ptr<const SliceTransform> prefix_extractor;
  prefix_extractor.reset(NewFixedPrefixTransform(8));
  options.compaction_filter_factory_v2 =
    std::make_shared<ChangeFilterFactoryV2>(prefix_extractor.get());
  option_config_ = kUniversalCompaction;
  options.compaction_style = (rocksdb::CompactionStyle)1;
  options = CurrentOptions(options);
  Reopen(options);
  const std::string value(10, 'x');
  for (int i = 0; i < 100001; i++) {
    char key[100];
    snprintf(key, sizeof(key), "B%08d%010d", i, i);
    Put(key, value);
  }
  dbfull()->TEST_FlushMemTable();
  dbfull()->TEST_CompactRange(0, nullptr, nullptr);
  dbfull()->TEST_CompactRange(1, nullptr, nullptr);
  for (int i = 0; i < 100001; i++) {
    char key[100];
    snprintf(key, sizeof(key), "B%08d%010d", i, i);
    std::string newvalue = Get(key);
    ASSERT_EQ(newvalue.compare(NEW_VALUE), 0);
  }
}
TEST_F(DBTest, CompactionFilterV2NULLPrefix) {
  Options options = CurrentOptions();
  options.num_levels = 3;
  options.max_mem_compaction_level = 0;
  std::unique_ptr<const SliceTransform> prefix_extractor;
  prefix_extractor.reset(NewFixedPrefixTransform(8));
  options.compaction_filter_factory_v2 =
    std::make_shared<ChangeFilterFactoryV2>(prefix_extractor.get());
  option_config_ = kUniversalCompaction;
  options.compaction_style = (rocksdb::CompactionStyle)1;
  Reopen(options);
  const std::string value(10, 'x');
  char first_key[100];
  snprintf(first_key, sizeof(first_key), "%s0000%010d", "NULL", 1);
  Put(first_key, value);
  for (int i = 1; i < 100000; i++) {
    char key[100];
    snprintf(key, sizeof(key), "%08d%010d", i, i);
    Put(key, value);
  }
  char last_key[100];
  snprintf(last_key, sizeof(last_key), "%s0000%010d", "NULL", 2);
  Put(last_key, value);
  dbfull()->TEST_FlushMemTable();
  dbfull()->TEST_CompactRange(0, nullptr, nullptr);
  std::string newvalue = Get(first_key);
  ASSERT_EQ(newvalue.compare(NEW_VALUE), 0);
  newvalue = Get(last_key);
  ASSERT_EQ(newvalue.compare(NEW_VALUE), 0);
  for (int i = 1; i < 100000; i++) {
    char key[100];
    snprintf(key, sizeof(key), "%08d%010d", i, i);
    newvalue = Get(key);
    ASSERT_EQ(newvalue.compare(NEW_VALUE), 0);
  }
}
TEST_F(DBTest, SparseMerge) {
  do {
    Options options = CurrentOptions();
    options.compression = kNoCompression;
    CreateAndReopenWithCF({"pikachu"}, options);
    FillLevels("A", "Z", 1);
    const std::string value(1000, 'x');
    Put(1, "A", "va");
    for (int i = 0; i < 100000; i++) {
      char key[100];
      snprintf(key, sizeof(key), "B%010d", i);
      Put(1, key, value);
    }
    Put(1, "C", "vc");
    ASSERT_OK(Flush(1));
    dbfull()->TEST_CompactRange(0, nullptr, nullptr, handles_[1]);
    Put(1, "A", "va2");
    Put(1, "B100", "bvalue2");
    Put(1, "C", "vc2");
    ASSERT_OK(Flush(1));
    ASSERT_LE(dbfull()->TEST_MaxNextLevelOverlappingBytes(handles_[1]),
              20 * 1048576);
    dbfull()->TEST_CompactRange(0, nullptr, nullptr);
    ASSERT_LE(dbfull()->TEST_MaxNextLevelOverlappingBytes(handles_[1]),
              20 * 1048576);
    dbfull()->TEST_CompactRange(1, nullptr, nullptr);
    ASSERT_LE(dbfull()->TEST_MaxNextLevelOverlappingBytes(handles_[1]),
              20 * 1048576);
  } while (ChangeCompactOptions());
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
TEST_F(DBTest, ApproximateSizesMemTable) {
  Options options;
  options.write_buffer_size = 100000000;
  options.compression = kNoCompression;
  options.create_if_missing = true;
  options = CurrentOptions(options);
  DestroyAndReopen(options);
  const int N = 128;
  Random rnd(301);
  for (int i = 0; i < N; i++) {
    ASSERT_OK(Put(Key(i), RandomString(&rnd, 1024)));
  }
  uint64_t size;
  std::string start = Key(50);
  std::string end = Key(60);
  Range r(start, end);
  db_->GetApproximateSizes(&r, 1, &size, true);
  ASSERT_GT(size, 6000);
  ASSERT_LT(size, 204800);
  db_->GetApproximateSizes(&r, 1, &size, false);
  ASSERT_EQ(size, 0);
  start = Key(500);
  end = Key(600);
  r = Range(start, end);
  db_->GetApproximateSizes(&r, 1, &size, true);
  ASSERT_EQ(size, 0);
  for (int i = 0; i < N; i++) {
    ASSERT_OK(Put(Key(1000 + i), RandomString(&rnd, 1024)));
  }
  start = Key(500);
  end = Key(600);
  r = Range(start, end);
  db_->GetApproximateSizes(&r, 1, &size, true);
  ASSERT_EQ(size, 0);
  start = Key(100);
  end = Key(1020);
  r = Range(start, end);
  db_->GetApproximateSizes(&r, 1, &size, true);
  ASSERT_GT(size, 6000);
  options.max_write_buffer_number = 8;
  options.min_write_buffer_number_to_merge = 5;
  options.write_buffer_size = 1024 * N;
  DestroyAndReopen(options);
  int keys[N * 3];
  for (int i = 0; i < N; i++) {
    keys[i * 3] = i * 5;
    keys[i * 3 + 1] = i * 5 + 1;
    keys[i * 3 + 2] = i * 5 + 2;
  }
  std::random_shuffle(std::begin(keys), std::end(keys));
  for (int i = 0; i < N * 3; i++) {
    ASSERT_OK(Put(Key(keys[i] + 1000), RandomString(&rnd, 1024)));
  }
  start = Key(100);
  end = Key(300);
  r = Range(start, end);
  db_->GetApproximateSizes(&r, 1, &size, true);
  ASSERT_EQ(size, 0);
  start = Key(1050);
  end = Key(1080);
  r = Range(start, end);
  db_->GetApproximateSizes(&r, 1, &size, true);
  ASSERT_GT(size, 6000);
  start = Key(2100);
  end = Key(2300);
  r = Range(start, end);
  db_->GetApproximateSizes(&r, 1, &size, true);
  ASSERT_EQ(size, 0);
  start = Key(1050);
  end = Key(1080);
  r = Range(start, end);
  uint64_t size_with_mt, size_without_mt;
  db_->GetApproximateSizes(&r, 1, &size_with_mt, true);
  ASSERT_GT(size_with_mt, 6000);
  db_->GetApproximateSizes(&r, 1, &size_without_mt, false);
  ASSERT_EQ(size_without_mt, 0);
  Flush();
  for (int i = 0; i < N; i++) {
    ASSERT_OK(Put(Key(i + 1000), RandomString(&rnd, 1024)));
  }
  start = Key(1050);
  end = Key(1080);
  r = Range(start, end);
  db_->GetApproximateSizes(&r, 1, &size_with_mt, true);
  db_->GetApproximateSizes(&r, 1, &size_without_mt, false);
  ASSERT_GT(size_with_mt, size_without_mt);
  ASSERT_GT(size_without_mt, 6000);
}
TEST_F(DBTest, ApproximateSizes) {
  do {
    Options options;
    options.write_buffer_size = 100000000;
    options.compression = kNoCompression;
    options.create_if_missing = true;
    options = CurrentOptions(options);
    DestroyAndReopen(options);
    CreateAndReopenWithCF({"pikachu"}, options);
    ASSERT_TRUE(Between(Size("", "xyz", 1), 0, 0));
    ReopenWithColumnFamilies({"default", "pikachu"}, options);
    ASSERT_TRUE(Between(Size("", "xyz", 1), 0, 0));
    ASSERT_EQ(NumTableFilesAtLevel(0, 1), 0);
    const int N = 80;
    static const int S1 = 100000;
    static const int S2 = 105000;
    Random rnd(301);
    for (int i = 0; i < N; i++) {
      ASSERT_OK(Put(1, Key(i), RandomString(&rnd, S1)));
    }
    ASSERT_TRUE(Between(Size("", Key(50), 1), 0, 0));
    for (int run = 0; run < 3; run++) {
      ReopenWithColumnFamilies({"default", "pikachu"}, options);
      for (int compact_start = 0; compact_start < N; compact_start += 10) {
        for (int i = 0; i < N; i += 10) {
          ASSERT_TRUE(Between(Size("", Key(i), 1), S1 * i, S2 * i));
          ASSERT_TRUE(Between(Size("", Key(i) + ".suffix", 1), S1 * (i + 1),
                              S2 * (i + 1)));
          ASSERT_TRUE(Between(Size(Key(i), Key(i + 10), 1), S1 * 10, S2 * 10));
        }
        ASSERT_TRUE(Between(Size("", Key(50), 1), S1 * 50, S2 * 50));
        ASSERT_TRUE(
            Between(Size("", Key(50) + ".suffix", 1), S1 * 50, S2 * 50));
        std::string cstart_str = Key(compact_start);
        std::string cend_str = Key(compact_start + 9);
        Slice cstart = cstart_str;
        Slice cend = cend_str;
        dbfull()->TEST_CompactRange(0, &cstart, &cend, handles_[1]);
      }
      ASSERT_EQ(NumTableFilesAtLevel(0, 1), 0);
      ASSERT_GT(NumTableFilesAtLevel(1, 1), 0);
    }
  } while (ChangeOptions(kSkipUniversalCompaction | kSkipFIFOCompaction |
                         kSkipPlainTable | kSkipHashIndex));
}
TEST_F(DBTest, ApproximateSizes_MixOfSmallAndLarge) {
  do {
    Options options = CurrentOptions();
    options.compression = kNoCompression;
    CreateAndReopenWithCF({"pikachu"}, options);
    Random rnd(301);
    std::string big1 = RandomString(&rnd, 100000);
    ASSERT_OK(Put(1, Key(0), RandomString(&rnd, 10000)));
    ASSERT_OK(Put(1, Key(1), RandomString(&rnd, 10000)));
    ASSERT_OK(Put(1, Key(2), big1));
    ASSERT_OK(Put(1, Key(3), RandomString(&rnd, 10000)));
    ASSERT_OK(Put(1, Key(4), big1));
    ASSERT_OK(Put(1, Key(5), RandomString(&rnd, 10000)));
    ASSERT_OK(Put(1, Key(6), RandomString(&rnd, 300000)));
    ASSERT_OK(Put(1, Key(7), RandomString(&rnd, 10000)));
    for (int run = 0; run < 3; run++) {
      ReopenWithColumnFamilies({"default", "pikachu"}, options);
      ASSERT_TRUE(Between(Size("", Key(0), 1), 0, 0));
      ASSERT_TRUE(Between(Size("", Key(1), 1), 10000, 11000));
      ASSERT_TRUE(Between(Size("", Key(2), 1), 20000, 21000));
      ASSERT_TRUE(Between(Size("", Key(3), 1), 120000, 121000));
      ASSERT_TRUE(Between(Size("", Key(4), 1), 130000, 131000));
      ASSERT_TRUE(Between(Size("", Key(5), 1), 230000, 231000));
      ASSERT_TRUE(Between(Size("", Key(6), 1), 240000, 241000));
      ASSERT_TRUE(Between(Size("", Key(7), 1), 540000, 541000));
      ASSERT_TRUE(Between(Size("", Key(8), 1), 550000, 560000));
      ASSERT_TRUE(Between(Size(Key(3), Key(5), 1), 110000, 111000));
      dbfull()->TEST_CompactRange(0, nullptr, nullptr, handles_[1]);
    }
  } while (ChangeOptions(kSkipPlainTable));
}
TEST_F(DBTest, IteratorPinsRef) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    Put(1, "foo", "hello");
    Iterator* iter = db_->NewIterator(ReadOptions(), handles_[1]);
    Put(1, "foo", "newvalue1");
    for (int i = 0; i < 100; i++) {
      ASSERT_OK(Put(1, Key(i), Key(i) + std::string(100000, 'v')));
    }
    Put(1, "foo", "newvalue2");
    iter->SeekToFirst();
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ("foo", iter->key().ToString());
    ASSERT_EQ("hello", iter->value().ToString());
    iter->Next();
    ASSERT_TRUE(!iter->Valid());
    delete iter;
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, Snapshot) {
  anon::OptionsOverride options_override;
  options_override.skip_policy = kSkipNoSnapshot;
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions(options_override));
    Put(0, "foo", "0v1");
    Put(1, "foo", "1v1");
    const Snapshot* s1 = db_->GetSnapshot();
    ASSERT_EQ(1U, GetNumSnapshots());
    uint64_t time_snap1 = GetTimeOldestSnapshots();
    ASSERT_GT(time_snap1, 0U);
    Put(0, "foo", "0v2");
    Put(1, "foo", "1v2");
    env_->addon_time_.fetch_add(1);
    const Snapshot* s2 = db_->GetSnapshot();
    ASSERT_EQ(2U, GetNumSnapshots());
    ASSERT_EQ(time_snap1, GetTimeOldestSnapshots());
    Put(0, "foo", "0v3");
    Put(1, "foo", "1v3");
    const Snapshot* s3 = db_->GetSnapshot();
    ASSERT_EQ(3U, GetNumSnapshots());
    ASSERT_EQ(time_snap1, GetTimeOldestSnapshots());
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
    ASSERT_EQ(2U, GetNumSnapshots());
    ASSERT_EQ(time_snap1, GetTimeOldestSnapshots());
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
    ASSERT_EQ(1U, GetNumSnapshots());
    ASSERT_LT(time_snap1, GetTimeOldestSnapshots());
    db_->ReleaseSnapshot(s2);
    ASSERT_EQ(0U, GetNumSnapshots());
    ASSERT_EQ("0v4", Get(0, "foo"));
    ASSERT_EQ("1v4", Get(1, "foo"));
  } while (ChangeOptions(kSkipHashCuckoo));
}
TEST_F(DBTest, HiddenValuesAreRemoved) {
  anon::OptionsOverride options_override;
  options_override.skip_policy = kSkipNoSnapshot;
  do {
    Options options = CurrentOptions(options_override);
    options.max_background_flushes = 0;
    CreateAndReopenWithCF({"pikachu"}, options);
    Random rnd(301);
    FillLevels("a", "z", 1);
    std::string big = RandomString(&rnd, 50000);
    Put(1, "foo", big);
    Put(1, "pastfoo", "v");
    const Snapshot* snapshot = db_->GetSnapshot();
    Put(1, "foo", "tiny");
    Put(1, "pastfoo2", "v2");
    ASSERT_OK(Flush(1));
    ASSERT_GT(NumTableFilesAtLevel(0, 1), 0);
    ASSERT_EQ(big, Get(1, "foo", snapshot));
    ASSERT_TRUE(Between(Size("", "pastfoo", 1), 50000, 60000));
    db_->ReleaseSnapshot(snapshot);
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ tiny, " + big + " ]");
    Slice x("x");
    dbfull()->TEST_CompactRange(0, nullptr, &x, handles_[1]);
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ tiny ]");
    ASSERT_EQ(NumTableFilesAtLevel(0, 1), 0);
    ASSERT_GE(NumTableFilesAtLevel(1, 1), 1);
    dbfull()->TEST_CompactRange(1, nullptr, &x, handles_[1]);
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ tiny ]");
    ASSERT_TRUE(Between(Size("", "pastfoo", 1), 0, 1000));
  } while (ChangeOptions(kSkipUniversalCompaction | kSkipFIFOCompaction |
                         kSkipPlainTable | kSkipHashCuckoo));
}
TEST_F(DBTest, CompactBetweenSnapshots) {
  anon::OptionsOverride options_override;
  options_override.skip_policy = kSkipNoSnapshot;
  do {
    Options options = CurrentOptions(options_override);
    options.disable_auto_compactions = true;
    CreateAndReopenWithCF({"pikachu"}, options);
    Random rnd(301);
    FillLevels("a", "z", 1);
    Put(1, "foo", "first");
    const Snapshot* snapshot1 = db_->GetSnapshot();
    Put(1, "foo", "second");
    Put(1, "foo", "third");
    Put(1, "foo", "fourth");
    const Snapshot* snapshot2 = db_->GetSnapshot();
    Put(1, "foo", "fifth");
    Put(1, "foo", "sixth");
    ASSERT_OK(Flush(1));
    ASSERT_EQ("sixth", Get(1, "foo"));
    ASSERT_EQ("fourth", Get(1, "foo", snapshot2));
    ASSERT_EQ("first", Get(1, "foo", snapshot1));
    ASSERT_EQ(AllEntriesFor("foo", 1),
              "[ sixth, fifth, fourth, third, second, first ]");
    FillLevels("a", "z", 1);
    dbfull()->CompactRange(CompactRangeOptions(), handles_[1], nullptr,
                           nullptr);
    ASSERT_EQ("sixth", Get(1, "foo"));
    ASSERT_EQ("fourth", Get(1, "foo", snapshot2));
    ASSERT_EQ("first", Get(1, "foo", snapshot1));
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ sixth, fourth, first ]");
    db_->ReleaseSnapshot(snapshot1);
    FillLevels("a", "z", 1);
    dbfull()->CompactRange(CompactRangeOptions(), handles_[1], nullptr,
                           nullptr);
    ASSERT_EQ("sixth", Get(1, "foo"));
    ASSERT_EQ("fourth", Get(1, "foo", snapshot2));
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ sixth, fourth ]");
    db_->ReleaseSnapshot(snapshot2);
    FillLevels("a", "z", 1);
    dbfull()->CompactRange(CompactRangeOptions(), handles_[1], nullptr,
                           nullptr);
    ASSERT_EQ("sixth", Get(1, "foo"));
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ sixth ]");
  } while (ChangeOptions(kSkipHashCuckoo | kSkipFIFOCompaction));
}
TEST_F(DBTest, DeletionMarkers1) {
  Options options = CurrentOptions();
  options.max_background_flushes = 0;
  CreateAndReopenWithCF({"pikachu"}, options);
  Put(1, "foo", "v1");
  ASSERT_OK(Flush(1));
  const int last = CurrentOptions().max_mem_compaction_level;
  ASSERT_EQ(NumTableFilesAtLevel(last, 1), 1);
  Put(1, "a", "begin");
  Put(1, "z", "end");
  Flush(1);
  ASSERT_EQ(NumTableFilesAtLevel(last, 1), 1);
  ASSERT_EQ(NumTableFilesAtLevel(last - 1, 1), 1);
  Delete(1, "foo");
  Put(1, "foo", "v2");
  ASSERT_EQ(AllEntriesFor("foo", 1), "[ v2, DEL, v1 ]");
  ASSERT_OK(Flush(1));
  if (CurrentOptions().purge_redundant_kvs_while_flush) {
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ v2, v1 ]");
  } else {
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ v2, DEL, v1 ]");
  }
  Slice z("z");
  dbfull()->TEST_CompactRange(last - 2, nullptr, &z, handles_[1]);
  ASSERT_EQ(AllEntriesFor("foo", 1), "[ v2, v1 ]");
  dbfull()->TEST_CompactRange(last - 1, nullptr, nullptr, handles_[1]);
  ASSERT_EQ(AllEntriesFor("foo", 1), "[ v2 ]");
}
TEST_F(DBTest, DeletionMarkers2) {
  Options options = CurrentOptions();
  options.max_background_flushes = 0;
  CreateAndReopenWithCF({"pikachu"}, options);
  Put(1, "foo", "v1");
  ASSERT_OK(Flush(1));
  const int last = CurrentOptions().max_mem_compaction_level;
  ASSERT_EQ(NumTableFilesAtLevel(last, 1), 1);
  Put(1, "a", "begin");
  Put(1, "z", "end");
  Flush(1);
  ASSERT_EQ(NumTableFilesAtLevel(last, 1), 1);
  ASSERT_EQ(NumTableFilesAtLevel(last - 1, 1), 1);
  Delete(1, "foo");
  ASSERT_EQ(AllEntriesFor("foo", 1), "[ DEL, v1 ]");
  ASSERT_OK(Flush(1));
  ASSERT_EQ(AllEntriesFor("foo", 1), "[ DEL, v1 ]");
  dbfull()->TEST_CompactRange(last - 2, nullptr, nullptr, handles_[1]);
  ASSERT_EQ(AllEntriesFor("foo", 1), "[ DEL, v1 ]");
  dbfull()->TEST_CompactRange(last - 1, nullptr, nullptr, handles_[1]);
  ASSERT_EQ(AllEntriesFor("foo", 1), "[ ]");
}
TEST_F(DBTest, OverlapInLevel0) {
  do {
    Options options = CurrentOptions();
    options.max_background_flushes = 0;
    CreateAndReopenWithCF({"pikachu"}, options);
    int tmp = CurrentOptions().max_mem_compaction_level;
    ASSERT_EQ(tmp, 2) << "Fix test to match config";
    ASSERT_OK(Put(1, "100", "v100"));
    ASSERT_OK(Put(1, "999", "v999"));
    Flush(1);
    ASSERT_OK(Delete(1, "100"));
    ASSERT_OK(Delete(1, "999"));
    Flush(1);
    ASSERT_EQ("0,1,1", FilesPerLevel(1));
    ASSERT_OK(Put(1, "300", "v300"));
    ASSERT_OK(Put(1, "500", "v500"));
    Flush(1);
    ASSERT_OK(Put(1, "200", "v200"));
    ASSERT_OK(Put(1, "600", "v600"));
    ASSERT_OK(Put(1, "900", "v900"));
    Flush(1);
    ASSERT_EQ("2,1,1", FilesPerLevel(1));
    dbfull()->TEST_CompactRange(1, nullptr, nullptr, handles_[1]);
    dbfull()->TEST_CompactRange(2, nullptr, nullptr, handles_[1]);
    ASSERT_EQ("2", FilesPerLevel(1));
    ASSERT_OK(Delete(1, "600"));
    Flush(1);
    ASSERT_EQ("3", FilesPerLevel(1));
    ASSERT_EQ("NOT_FOUND", Get(1, "600"));
  } while (ChangeOptions(kSkipUniversalCompaction | kSkipFIFOCompaction));
}
TEST_F(DBTest, L0_CompactionBug_Issue44_a) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    ASSERT_OK(Put(1, "b", "v"));
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ASSERT_OK(Delete(1, "b"));
    ASSERT_OK(Delete(1, "a"));
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ASSERT_OK(Delete(1, "a"));
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ASSERT_OK(Put(1, "a", "v"));
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ASSERT_EQ("(a->v)", Contents(1));
    env_->SleepForMicroseconds(1000000);
    ASSERT_EQ("(a->v)", Contents(1));
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, L0_CompactionBug_Issue44_b) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    Put(1, "", "");
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    Delete(1, "e");
    Put(1, "", "");
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    Put(1, "c", "cv");
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    Put(1, "", "");
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    Put(1, "", "");
    env_->SleepForMicroseconds(1000000);
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    Put(1, "d", "dv");
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    Put(1, "", "");
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    Delete(1, "d");
    Delete(1, "b");
    ReopenWithColumnFamilies({"default", "pikachu"}, CurrentOptions());
    ASSERT_EQ("(->)(c->cv)", Contents(1));
    env_->SleepForMicroseconds(1000000);
    ASSERT_EQ("(->)(c->cv)", Contents(1));
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, ComparatorCheck) {
  class NewComparator : public Comparator {
   public:
    virtual const char* Name() const override {
      return "rocksdb.NewComparator";
    }
    virtual int Compare(const Slice& a, const Slice& b) const override {
      return BytewiseComparator()->Compare(a, b);
    }
    virtual void FindShortestSeparator(std::string* s,
                                       const Slice& l) const override {
      BytewiseComparator()->FindShortestSeparator(s, l);
    }
    virtual void FindShortSuccessor(std::string* key) const override {
      BytewiseComparator()->FindShortSuccessor(key);
    }
  };
  Options new_options, options;
  NewComparator cmp;
  do {
    options = CurrentOptions();
    CreateAndReopenWithCF({"pikachu"}, options);
    new_options = CurrentOptions();
    new_options.comparator = &cmp;
    Status s = TryReopenWithColumnFamilies({"default", "pikachu"},
        std::vector<Options>({options, new_options}));
    ASSERT_TRUE(!s.ok());
    ASSERT_TRUE(s.ToString().find("comparator") != std::string::npos)
        << s.ToString();
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, CustomComparator) {
  class NumberComparator : public Comparator {
   public:
    virtual const char* Name() const override {
      return "test.NumberComparator";
    }
    virtual int Compare(const Slice& a, const Slice& b) const override {
      return ToNumber(a) - ToNumber(b);
    }
    virtual void FindShortestSeparator(std::string* s,
                                       const Slice& l) const override {
      ToNumber(*s);
      ToNumber(l);
    }
    virtual void FindShortSuccessor(std::string* key) const override {
      ToNumber(*key);
    }
   private:
    static int ToNumber(const Slice& x) {
      EXPECT_TRUE(x.size() >= 2 && x[0] == '[' && x[x.size() - 1] == ']')
          << EscapeString(x);
      int val;
      char ignored;
      EXPECT_TRUE(sscanf(x.ToString().c_str(), "[%i]%c", &val, &ignored) == 1)
          << EscapeString(x);
      return val;
    }
  };
  Options new_options;
  NumberComparator cmp;
  do {
    new_options = CurrentOptions();
    new_options.create_if_missing = true;
    new_options.comparator = &cmp;
    new_options.write_buffer_size = 1000;
    new_options = CurrentOptions(new_options);
    DestroyAndReopen(new_options);
    CreateAndReopenWithCF({"pikachu"}, new_options);
    ASSERT_OK(Put(1, "[10]", "ten"));
    ASSERT_OK(Put(1, "[0x14]", "twenty"));
    for (int i = 0; i < 2; i++) {
      ASSERT_EQ("ten", Get(1, "[10]"));
      ASSERT_EQ("ten", Get(1, "[0xa]"));
      ASSERT_EQ("twenty", Get(1, "[20]"));
      ASSERT_EQ("twenty", Get(1, "[0x14]"));
      ASSERT_EQ("NOT_FOUND", Get(1, "[15]"));
      ASSERT_EQ("NOT_FOUND", Get(1, "[0xf]"));
      Compact(1, "[0]", "[9999]");
    }
    for (int run = 0; run < 2; run++) {
      for (int i = 0; i < 1000; i++) {
        char buf[100];
        snprintf(buf, sizeof(buf), "[%d]", i*10);
        ASSERT_OK(Put(1, buf, buf));
      }
      Compact(1, "[0]", "[1000000]");
    }
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, ManualCompaction) {
  Options options = CurrentOptions();
  options.max_background_flushes = 0;
  CreateAndReopenWithCF({"pikachu"}, options);
  ASSERT_EQ(dbfull()->MaxMemCompactionLevel(), 2)
      << "Need to update this test to match kMaxMemCompactLevel";
  for (int iter = 0; iter < 2; ++iter) {
    MakeTables(3, "p", "q", 1);
    ASSERT_EQ("1,1,1", FilesPerLevel(1));
    Compact(1, "", "c");
    ASSERT_EQ("1,1,1", FilesPerLevel(1));
    Compact(1, "r", "z");
    ASSERT_EQ("1,1,1", FilesPerLevel(1));
    Compact(1, "p1", "p9");
    ASSERT_EQ("0,0,1", FilesPerLevel(1));
    MakeTables(3, "c", "e", 1);
    ASSERT_EQ("1,1,2", FilesPerLevel(1));
    Compact(1, "b", "f");
    ASSERT_EQ("0,0,2", FilesPerLevel(1));
    MakeTables(1, "a", "z", 1);
    ASSERT_EQ("0,1,2", FilesPerLevel(1));
    db_->CompactRange(CompactRangeOptions(), handles_[1], nullptr, nullptr);
    ASSERT_EQ("0,0,1", FilesPerLevel(1));
    if (iter == 0) {
      options = CurrentOptions();
      options.max_background_flushes = 0;
      options.num_levels = 3;
      options.create_if_missing = true;
      DestroyAndReopen(options);
      CreateAndReopenWithCF({"pikachu"}, options);
    }
  }
}
class DBTestUniversalManualCompactionOutputPathId
    : public DBTestUniversalCompactionBase {};
TEST_P(DBTestUniversalManualCompactionOutputPathId,
       ManualCompactionOutputPathId) {
  Options options = CurrentOptions();
  options.create_if_missing = true;
  options.db_paths.emplace_back(dbname_, 1000000000);
  options.db_paths.emplace_back(dbname_ + "_2", 1000000000);
  options.compaction_style = kCompactionStyleUniversal;
  options.num_levels = num_levels_;
  options.target_file_size_base = 1 << 30;
  options.level0_file_num_compaction_trigger = 10;
  Destroy(options);
  DestroyAndReopen(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  MakeTables(3, "p", "q", 1);
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ(3, TotalLiveFiles(1));
  ASSERT_EQ(3, GetSstFileCount(options.db_paths[0].path));
  ASSERT_EQ(0, GetSstFileCount(options.db_paths[1].path));
  CompactRangeOptions compact_options;
  compact_options.target_path_id = 1;
  db_->CompactRange(compact_options, handles_[1], nullptr, nullptr);
  ASSERT_EQ(1, TotalLiveFiles(1));
  ASSERT_EQ(0, GetSstFileCount(options.db_paths[0].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ReopenWithColumnFamilies({kDefaultColumnFamilyName, "pikachu"}, options);
  ASSERT_EQ(1, TotalLiveFiles(1));
  ASSERT_EQ(0, GetSstFileCount(options.db_paths[0].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  MakeTables(1, "p", "q", 1);
  ASSERT_EQ(2, TotalLiveFiles(1));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[0].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  ReopenWithColumnFamilies({kDefaultColumnFamilyName, "pikachu"}, options);
  ASSERT_EQ(2, TotalLiveFiles(1));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[0].path));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
  compact_options.target_path_id = 0;
  db_->CompactRange(compact_options, handles_[1], nullptr, nullptr);
  ASSERT_EQ(1, TotalLiveFiles(1));
  ASSERT_EQ(1, GetSstFileCount(options.db_paths[0].path));
  ASSERT_EQ(0, GetSstFileCount(options.db_paths[1].path));
  compact_options.target_path_id = 2;
  ASSERT_TRUE(db_->CompactRange(compact_options, handles_[1], nullptr, nullptr)
                  .IsInvalidArgument());
}
INSTANTIATE_TEST_CASE_P(DBTestUniversalManualCompactionOutputPathId,
                        DBTestUniversalManualCompactionOutputPathId,
                        ::testing::Values(1, 8));
TEST_F(DBTest, ManualLevelCompactionOutputPathId) {
  Options options = CurrentOptions();
  options.db_paths.emplace_back(dbname_ + "_2", 2 * 10485760);
  options.db_paths.emplace_back(dbname_ + "_3", 100 * 10485760);
  options.db_paths.emplace_back(dbname_ + "_4", 120 * 10485760);
  options.max_background_flushes = 1;
  CreateAndReopenWithCF({"pikachu"}, options);
  ASSERT_EQ(dbfull()->MaxMemCompactionLevel(), 2)
      << "Need to update this test to match kMaxMemCompactLevel";
  for (int iter = 0; iter < 2; ++iter) {
    MakeTables(3, "p", "q", 1);
    ASSERT_EQ("3", FilesPerLevel(1));
    ASSERT_EQ(3, GetSstFileCount(options.db_paths[0].path));
    ASSERT_EQ(0, GetSstFileCount(dbname_));
    Compact(1, "", "c");
    ASSERT_EQ("3", FilesPerLevel(1));
    Compact(1, "r", "z");
    ASSERT_EQ("3", FilesPerLevel(1));
    Compact(1, "p1", "p9", 1);
    ASSERT_EQ("0,1", FilesPerLevel(1));
    ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
    ASSERT_EQ(0, GetSstFileCount(options.db_paths[0].path));
    ASSERT_EQ(0, GetSstFileCount(dbname_));
    MakeTables(3, "c", "e", 1);
    ASSERT_EQ("3,1", FilesPerLevel(1));
    Compact(1, "b", "f", 1);
    ASSERT_EQ("0,2", FilesPerLevel(1));
    ASSERT_EQ(2, GetSstFileCount(options.db_paths[1].path));
    ASSERT_EQ(0, GetSstFileCount(options.db_paths[0].path));
    ASSERT_EQ(0, GetSstFileCount(dbname_));
    MakeTables(1, "a", "z", 1);
    ASSERT_EQ("1,2", FilesPerLevel(1));
    ASSERT_EQ(2, GetSstFileCount(options.db_paths[1].path));
    ASSERT_EQ(1, GetSstFileCount(options.db_paths[0].path));
    CompactRangeOptions compact_options;
    compact_options.target_path_id = 1;
    db_->CompactRange(compact_options, handles_[1], nullptr, nullptr);
    ASSERT_EQ("0,1", FilesPerLevel(1));
    ASSERT_EQ(1, GetSstFileCount(options.db_paths[1].path));
    ASSERT_EQ(0, GetSstFileCount(options.db_paths[0].path));
    ASSERT_EQ(0, GetSstFileCount(dbname_));
    if (iter == 0) {
      DestroyAndReopen(options);
      options = CurrentOptions();
      options.db_paths.emplace_back(dbname_ + "_2", 2 * 10485760);
      options.db_paths.emplace_back(dbname_ + "_3", 100 * 10485760);
      options.db_paths.emplace_back(dbname_ + "_4", 120 * 10485760);
      options.max_background_flushes = 1;
      options.num_levels = 3;
      options.create_if_missing = true;
      CreateAndReopenWithCF({"pikachu"}, options);
    }
  }
}
TEST_F(DBTest, DBOpen_Options) {
  Options options = CurrentOptions();
  std::string dbname = test::TmpDir(env_) + "/db_options_test";
  ASSERT_OK(DestroyDB(dbname, options));
  DB* db = nullptr;
  options.create_if_missing = false;
  Status s = DB::Open(options, dbname, &db);
  ASSERT_TRUE(strstr(s.ToString().c_str(), "does not exist") != nullptr);
  ASSERT_TRUE(db == nullptr);
  options.create_if_missing = true;
  s = DB::Open(options, dbname, &db);
  ASSERT_OK(s);
  ASSERT_TRUE(db != nullptr);
  delete db;
  db = nullptr;
  options.create_if_missing = false;
  options.error_if_exists = true;
  s = DB::Open(options, dbname, &db);
  ASSERT_TRUE(strstr(s.ToString().c_str(), "exists") != nullptr);
  ASSERT_TRUE(db == nullptr);
  options.create_if_missing = true;
  options.error_if_exists = false;
  s = DB::Open(options, dbname, &db);
  ASSERT_OK(s);
  ASSERT_TRUE(db != nullptr);
  delete db;
  db = nullptr;
}
TEST_F(DBTest, DBOpen_Change_NumLevels) {
  Options options = CurrentOptions();
  options.create_if_missing = true;
  options.max_background_flushes = 0;
  DestroyAndReopen(options);
  ASSERT_TRUE(db_ != nullptr);
  CreateAndReopenWithCF({"pikachu"}, options);
  ASSERT_OK(Put(1, "a", "123"));
  ASSERT_OK(Put(1, "b", "234"));
  db_->CompactRange(CompactRangeOptions(), handles_[1], nullptr, nullptr);
  Close();
  options.create_if_missing = false;
  options.num_levels = 2;
  Status s = TryReopenWithColumnFamilies({"default", "pikachu"}, options);
  ASSERT_TRUE(strstr(s.ToString().c_str(), "Invalid argument") != nullptr);
  ASSERT_TRUE(db_ == nullptr);
}
TEST_F(DBTest, DestroyDBMetaDatabase) {
  std::string dbname = test::TmpDir(env_) + "/db_meta";
  ASSERT_OK(env_->CreateDirIfMissing(dbname));
  std::string metadbname = MetaDatabaseName(dbname, 0);
  ASSERT_OK(env_->CreateDirIfMissing(metadbname));
  std::string metametadbname = MetaDatabaseName(metadbname, 0);
  ASSERT_OK(env_->CreateDirIfMissing(metametadbname));
  Options options = CurrentOptions();
  ASSERT_OK(DestroyDB(metametadbname, options));
  ASSERT_OK(DestroyDB(metadbname, options));
  ASSERT_OK(DestroyDB(dbname, options));
  DB* db = nullptr;
  ASSERT_OK(DB::Open(options, dbname, &db));
  delete db;
  db = nullptr;
  ASSERT_OK(DB::Open(options, metadbname, &db));
  delete db;
  db = nullptr;
  ASSERT_OK(DB::Open(options, metametadbname, &db));
  delete db;
  db = nullptr;
  ASSERT_OK(DestroyDB(dbname, options));
  options.create_if_missing = false;
  ASSERT_TRUE(!(DB::Open(options, dbname, &db)).ok());
  ASSERT_TRUE(!(DB::Open(options, metadbname, &db)).ok());
  ASSERT_TRUE(!(DB::Open(options, metametadbname, &db)).ok());
}
TEST_F(DBTest, DropWrites) {
  do {
    Options options = CurrentOptions();
    options.env = env_;
    options.paranoid_checks = false;
    Reopen(options);
    ASSERT_OK(Put("foo", "v1"));
    ASSERT_EQ("v1", Get("foo"));
    Compact("a", "z");
    const size_t num_files = CountFiles();
    env_->drop_writes_.store(true, std::memory_order_release);
    env_->sleep_counter_.Reset();
    env_->no_sleep_ = true;
    for (int i = 0; i < 5; i++) {
      if (option_config_ != kUniversalCompactionMultiLevel) {
        for (int level = 0; level < dbfull()->NumberLevels(); level++) {
          if (level > 0 && level == dbfull()->NumberLevels() - 1) {
            break;
          }
          dbfull()->TEST_CompactRange(level, nullptr, nullptr, nullptr,
                                      true );
        }
      } else {
        dbfull()->CompactRange(CompactRangeOptions(), nullptr, nullptr);
      }
    }
    std::string property_value;
    ASSERT_TRUE(db_->GetProperty("rocksdb.background-errors", &property_value));
    ASSERT_EQ("5", property_value);
    env_->drop_writes_.store(false, std::memory_order_release);
    ASSERT_LT(CountFiles(), num_files + 3);
    ASSERT_TRUE(env_->sleep_counter_.WaitFor(5));
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, DropWritesFlush) {
  do {
    Options options = CurrentOptions();
    options.env = env_;
    options.max_background_flushes = 1;
    Reopen(options);
    ASSERT_OK(Put("foo", "v1"));
    env_->drop_writes_.store(true, std::memory_order_release);
    std::string property_value;
    ASSERT_TRUE(db_->GetProperty("rocksdb.background-errors", &property_value));
    ASSERT_EQ("0", property_value);
    dbfull()->TEST_FlushMemTable(true);
    ASSERT_TRUE(db_->GetProperty("rocksdb.background-errors", &property_value));
    ASSERT_EQ("1", property_value);
    env_->drop_writes_.store(false, std::memory_order_release);
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, NoSpaceCompactRange) {
  do {
    Options options = CurrentOptions();
    options.env = env_;
    options.disable_auto_compactions = true;
    Reopen(options);
    for (int i = 0; i < 5; ++i) {
      ASSERT_OK(Put(Key(i), Key(i) + "v"));
      ASSERT_OK(Flush());
    }
    env_->no_space_.store(true, std::memory_order_release);
    Status s = dbfull()->TEST_CompactRange(0, nullptr, nullptr, nullptr,
                                           true );
    ASSERT_TRUE(s.IsIOError());
    env_->no_space_.store(false, std::memory_order_release);
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, NonWritableFileSystem) {
  do {
    Options options = CurrentOptions();
    options.write_buffer_size = 1000;
    options.env = env_;
    Reopen(options);
    ASSERT_OK(Put("foo", "v1"));
    env_->non_writeable_rate_.store(100);
    std::string big(100000, 'x');
    int errors = 0;
    for (int i = 0; i < 20; i++) {
      if (!Put("foo", big).ok()) {
        errors++;
        env_->SleepForMicroseconds(100000);
      }
    }
    ASSERT_GT(errors, 0);
    env_->non_writeable_rate_.store(0);
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, ManifestWriteError) {
  for (int iter = 0; iter < 2; iter++) {
    std::atomic<bool>* error_type = (iter == 0)
        ? &env_->manifest_sync_error_
        : &env_->manifest_write_error_;
    Options options = CurrentOptions();
    options.env = env_;
    options.create_if_missing = true;
    options.error_if_exists = false;
    options.max_background_flushes = 0;
    DestroyAndReopen(options);
    ASSERT_OK(Put("foo", "bar"));
    ASSERT_EQ("bar", Get("foo"));
    Flush();
    ASSERT_EQ("bar", Get("foo"));
    const int last = dbfull()->MaxMemCompactionLevel();
    ASSERT_EQ(NumTableFilesAtLevel(last), 1);
    error_type->store(true, std::memory_order_release);
    dbfull()->TEST_CompactRange(last, nullptr, nullptr);
    ASSERT_EQ("bar", Get("foo"));
    error_type->store(false, std::memory_order_release);
    Reopen(options);
    ASSERT_EQ("bar", Get("foo"));
  }
}
TEST_F(DBTest, PutFailsParanoid) {
  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.error_if_exists = false;
  options.paranoid_checks = true;
  DestroyAndReopen(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  Status s;
  ASSERT_OK(Put(1, "foo", "bar"));
  ASSERT_OK(Put(1, "foo1", "bar1"));
  env_->log_write_error_.store(true, std::memory_order_release);
  s = Put(1, "foo2", "bar2");
  ASSERT_TRUE(!s.ok());
  env_->log_write_error_.store(false, std::memory_order_release);
  s = Put(1, "foo3", "bar3");
  ASSERT_TRUE(!s.ok());
  ASSERT_EQ("bar", Get(1, "foo"));
  options.paranoid_checks = false;
  DestroyAndReopen(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  ASSERT_OK(Put(1, "foo", "bar"));
  ASSERT_OK(Put(1, "foo1", "bar1"));
  env_->log_write_error_.store(true, std::memory_order_release);
  s = Put(1, "foo2", "bar2");
  ASSERT_TRUE(!s.ok());
  env_->log_write_error_.store(false, std::memory_order_release);
  s = Put(1, "foo3", "bar3");
  ASSERT_TRUE(s.ok());
}
TEST_F(DBTest, FilesDeletedAfterCompaction) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    ASSERT_OK(Put(1, "foo", "v2"));
    Compact(1, "a", "z");
    const size_t num_files = CountLiveFiles();
    for (int i = 0; i < 10; i++) {
      ASSERT_OK(Put(1, "foo", "v2"));
      Compact(1, "a", "z");
    }
    ASSERT_EQ(CountLiveFiles(), num_files);
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, BloomFilter) {
  do {
    Options options = CurrentOptions();
    env_->count_random_reads_ = true;
    options.env = env_;
    BlockBasedTableOptions table_options;
    table_options.no_block_cache = true;
    table_options.filter_policy.reset(NewBloomFilterPolicy(10));
    options.table_factory.reset(NewBlockBasedTableFactory(table_options));
    CreateAndReopenWithCF({"pikachu"}, options);
    const int N = 10000;
    for (int i = 0; i < N; i++) {
      ASSERT_OK(Put(1, Key(i), Key(i)));
    }
    Compact(1, "a", "z");
    for (int i = 0; i < N; i += 100) {
      ASSERT_OK(Put(1, Key(i), Key(i)));
    }
    Flush(1);
    env_->delay_sstable_sync_.store(true, std::memory_order_release);
    env_->random_read_counter_.Reset();
    for (int i = 0; i < N; i++) {
      ASSERT_EQ(Key(i), Get(1, Key(i)));
    }
    int reads = env_->random_read_counter_.Read();
    fprintf(stderr, "%d present => %d reads\n", N, reads);
    ASSERT_GE(reads, N);
    ASSERT_LE(reads, N + 2*N/100);
    env_->random_read_counter_.Reset();
    for (int i = 0; i < N; i++) {
      ASSERT_EQ("NOT_FOUND", Get(1, Key(i) + ".missing"));
    }
    reads = env_->random_read_counter_.Read();
    fprintf(stderr, "%d missing => %d reads\n", N, reads);
    ASSERT_LE(reads, 3*N/100);
    env_->delay_sstable_sync_.store(false, std::memory_order_release);
    Close();
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, BloomFilterRate) {
  while (ChangeFilterOptions()) {
    Options options = CurrentOptions();
    options.statistics = rocksdb::CreateDBStatistics();
    CreateAndReopenWithCF({"pikachu"}, options);
    const int maxKey = 10000;
    for (int i = 0; i < maxKey; i++) {
      ASSERT_OK(Put(1, Key(i), Key(i)));
    }
    ASSERT_OK(Put(1, Key(maxKey + 55555), Key(maxKey + 55555)));
    Flush(1);
    for (int i = 0; i < maxKey; i++) {
      ASSERT_EQ(Key(i), Get(1, Key(i)));
    }
    ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 0);
    for (int i = 0; i < maxKey; i++) {
      ASSERT_EQ("NOT_FOUND", Get(1, Key(i+33333)));
    }
    ASSERT_GE(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), maxKey*0.98);
  }
}
TEST_F(DBTest, BloomFilterCompatibility) {
  Options options = CurrentOptions();
  options.statistics = rocksdb::CreateDBStatistics();
  BlockBasedTableOptions table_options;
  table_options.filter_policy.reset(NewBloomFilterPolicy(10, true));
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  CreateAndReopenWithCF({"pikachu"}, options);
  const int maxKey = 10000;
  for (int i = 0; i < maxKey; i++) {
    ASSERT_OK(Put(1, Key(i), Key(i)));
  }
  ASSERT_OK(Put(1, Key(maxKey + 55555), Key(maxKey + 55555)));
  Flush(1);
  table_options.filter_policy.reset(NewBloomFilterPolicy(10, false));
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  ReopenWithColumnFamilies({"default", "pikachu"}, options);
  for (int i = 0; i < maxKey; i++) {
    ASSERT_EQ(Key(i), Get(1, Key(i)));
  }
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 0);
}
TEST_F(DBTest, BloomFilterReverseCompatibility) {
  Options options = CurrentOptions();
  options.statistics = rocksdb::CreateDBStatistics();
  BlockBasedTableOptions table_options;
  table_options.filter_policy.reset(NewBloomFilterPolicy(10, false));
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  CreateAndReopenWithCF({"pikachu"}, options);
  const int maxKey = 10000;
  for (int i = 0; i < maxKey; i++) {
    ASSERT_OK(Put(1, Key(i), Key(i)));
  }
  ASSERT_OK(Put(1, Key(maxKey + 55555), Key(maxKey + 55555)));
  Flush(1);
  table_options.filter_policy.reset(NewBloomFilterPolicy(10, true));
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  ReopenWithColumnFamilies({"default", "pikachu"}, options);
  for (int i = 0; i < maxKey; i++) {
    ASSERT_EQ(Key(i), Get(1, Key(i)));
  }
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 0);
}
namespace {
class WrappedBloom : public FilterPolicy {
 public:
  explicit WrappedBloom(int bits_per_key) :
        filter_(NewBloomFilterPolicy(bits_per_key)),
        counter_(0) {}
  ~WrappedBloom() { delete filter_; }
  const char* Name() const override { return "WrappedRocksDbFilterPolicy"; }
  void CreateFilter(const rocksdb::Slice* keys, int n, std::string* dst)
      const override {
    std::unique_ptr<rocksdb::Slice[]> user_keys(new rocksdb::Slice[n]);
    for (int i = 0; i < n; ++i) {
      user_keys[i] = convertKey(keys[i]);
    }
    return filter_->CreateFilter(user_keys.get(), n, dst);
  }
  bool KeyMayMatch(const rocksdb::Slice& key, const rocksdb::Slice& filter)
      const override {
    counter_++;
    return filter_->KeyMayMatch(convertKey(key), filter);
  }
  uint32_t GetCounter() { return counter_; }
 private:
  const FilterPolicy* filter_;
  mutable uint32_t counter_;
  rocksdb::Slice convertKey(const rocksdb::Slice& key) const {
    return key;
  }
};
}
TEST_F(DBTest, BloomFilterWrapper) {
  Options options = CurrentOptions();
  options.statistics = rocksdb::CreateDBStatistics();
  BlockBasedTableOptions table_options;
  WrappedBloom* policy = new WrappedBloom(10);
  table_options.filter_policy.reset(policy);
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  CreateAndReopenWithCF({"pikachu"}, options);
  const int maxKey = 10000;
  for (int i = 0; i < maxKey; i++) {
    ASSERT_OK(Put(1, Key(i), Key(i)));
  }
  ASSERT_OK(Put(1, Key(maxKey + 55555), Key(maxKey + 55555)));
  ASSERT_EQ(0U, policy->GetCounter());
  Flush(1);
  for (int i = 0; i < maxKey; i++) {
    ASSERT_EQ(Key(i), Get(1, Key(i)));
  }
  ASSERT_EQ(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), 0);
  ASSERT_EQ(1U * maxKey, policy->GetCounter());
  for (int i = 0; i < maxKey; i++) {
    ASSERT_EQ("NOT_FOUND", Get(1, Key(i+33333)));
  }
  ASSERT_GE(TestGetTickerCount(options, BLOOM_FILTER_USEFUL), maxKey*0.98);
  ASSERT_EQ(2U * maxKey, policy->GetCounter());
}
TEST_F(DBTest, SnapshotFiles) {
  do {
    Options options = CurrentOptions();
    options.write_buffer_size = 100000000;
    CreateAndReopenWithCF({"pikachu"}, options);
    Random rnd(301);
    ASSERT_EQ(NumTableFilesAtLevel(0, 1), 0);
    std::vector<std::string> values;
    for (int i = 0; i < 80; i++) {
      values.push_back(RandomString(&rnd, 100000));
      ASSERT_OK(Put((i < 40), Key(i), values[i]));
    }
    ASSERT_EQ(NumTableFilesAtLevel(0, 1), 0);
    uint64_t manifest_number = 0;
    uint64_t manifest_size = 0;
    std::vector<std::string> files;
    dbfull()->DisableFileDeletions();
    dbfull()->GetLiveFiles(files, &manifest_size);
    ASSERT_EQ(files.size(), 4U);
    uint64_t number = 0;
    FileType type;
    std::string snapdir = dbname_ + ".snapdir/";
    ASSERT_OK(env_->CreateDirIfMissing(snapdir));
    for (unsigned int i = 0; i < files.size(); i++) {
      ASSERT_EQ(files[i][0], '/');
      std::string src = dbname_ + files[i];
      std::string dest = snapdir + files[i];
      uint64_t size;
      ASSERT_OK(env_->GetFileSize(src, &size));
      if (ParseFileName(files[i].substr(1), &number, &type)) {
        if (type == kDescriptorFile) {
          if (number > manifest_number) {
            manifest_number = number;
            ASSERT_GE(size, manifest_size);
            size = manifest_size;
          }
        }
      }
      CopyFile(src, dest, size);
    }
    dbfull()->DisableFileDeletions();
    std::vector<std::string> extras;
    for (unsigned int i = 0; i < 1; i++) {
      extras.push_back(RandomString(&rnd, 100000));
      ASSERT_OK(Put(0, Key(i), extras[i]));
    }
    std::vector<ColumnFamilyDescriptor> column_families;
    column_families.emplace_back("default", ColumnFamilyOptions());
    column_families.emplace_back("pikachu", ColumnFamilyOptions());
    std::vector<ColumnFamilyHandle*> cf_handles;
    DB* snapdb;
    DBOptions opts;
    opts.env = env_;
    opts.create_if_missing = false;
    Status stat =
        DB::Open(opts, snapdir, column_families, &cf_handles, &snapdb);
    ASSERT_OK(stat);
    ReadOptions roptions;
    std::string val;
    for (unsigned int i = 0; i < 80; i++) {
      stat = snapdb->Get(roptions, cf_handles[i < 40], Key(i), &val);
      ASSERT_EQ(values[i].compare(val), 0);
    }
    for (auto cfh : cf_handles) {
      delete cfh;
    }
    delete snapdb;
    uint64_t new_manifest_number = 0;
    uint64_t new_manifest_size = 0;
    std::vector<std::string> newfiles;
    dbfull()->DisableFileDeletions();
    dbfull()->GetLiveFiles(newfiles, &new_manifest_size);
    for (unsigned int i = 0; i < newfiles.size(); i++) {
      std::string src = dbname_ + "/" + newfiles[i];
      if (ParseFileName(newfiles[i].substr(1), &number, &type)) {
        if (type == kDescriptorFile) {
          if (number > new_manifest_number) {
            uint64_t size;
            new_manifest_number = number;
            ASSERT_OK(env_->GetFileSize(src, &size));
            ASSERT_GE(size, new_manifest_size);
          }
        }
      }
    }
    ASSERT_EQ(manifest_number, new_manifest_number);
    ASSERT_GT(new_manifest_size, manifest_size);
    dbfull()->DisableFileDeletions();
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, CompactOnFlush) {
  anon::OptionsOverride options_override;
  options_override.skip_policy = kSkipNoSnapshot;
  do {
    Options options = CurrentOptions(options_override);
    options.purge_redundant_kvs_while_flush = true;
    options.disable_auto_compactions = true;
    CreateAndReopenWithCF({"pikachu"}, options);
    Put(1, "foo", "v1");
    ASSERT_OK(Flush(1));
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ v1 ]");
    Put(1, "a", "begin");
    Put(1, "z", "end");
    Flush(1);
    Delete(1, "foo");
    Put(1, "foo", "v2");
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ v2, DEL, v1 ]");
    ASSERT_OK(Flush(1));
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ v2, v1 ]");
    dbfull()->CompactRange(CompactRangeOptions(), handles_[1], nullptr,
                           nullptr);
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ v2 ]");
    Delete(1, "foo");
    Delete(1, "foo");
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ DEL, DEL, v2 ]");
    ASSERT_OK(Flush(1));
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ DEL, v2 ]");
    dbfull()->CompactRange(CompactRangeOptions(), handles_[1], nullptr,
                           nullptr);
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ ]");
    Put(1, "foo", "v3");
    Delete(1, "foo");
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ DEL, v3 ]");
    ASSERT_OK(Flush(1));
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ DEL ]");
    dbfull()->CompactRange(CompactRangeOptions(), handles_[1], nullptr,
                           nullptr);
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ ]");
    Put(1, "foo", "v4");
    Put(1, "foo", "v5");
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ v5, v4 ]");
    ASSERT_OK(Flush(1));
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ v5 ]");
    dbfull()->CompactRange(CompactRangeOptions(), handles_[1], nullptr,
                           nullptr);
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ v5 ]");
    Delete(1, "foo");
    dbfull()->CompactRange(CompactRangeOptions(), handles_[1], nullptr,
                           nullptr);
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ ]");
    Put(1, "foo", "v6");
    const Snapshot* snapshot = db_->GetSnapshot();
    Put(1, "foo", "v7");
    ASSERT_OK(Flush(1));
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ v7, v6 ]");
    db_->ReleaseSnapshot(snapshot);
    Delete(1, "foo");
    dbfull()->CompactRange(CompactRangeOptions(), handles_[1], nullptr,
                           nullptr);
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ ]");
    const Snapshot* snapshot1 = db_->GetSnapshot();
    Put(1, "foo", "v8");
    Put(1, "foo", "v9");
    ASSERT_OK(Flush(1));
    ASSERT_EQ(AllEntriesFor("foo", 1), "[ v9 ]");
    db_->ReleaseSnapshot(snapshot1);
  } while (ChangeCompactOptions());
}
namespace {
std::vector<std::uint64_t> ListSpecificFiles(
    Env* env, const std::string& path, const FileType expected_file_type) {
  std::vector<std::string> files;
  std::vector<uint64_t> file_numbers;
  env->GetChildren(path, &files);
  uint64_t number;
  FileType type;
  for (size_t i = 0; i < files.size(); ++i) {
    if (ParseFileName(files[i], &number, &type)) {
      if (type == expected_file_type) {
        file_numbers.push_back(number);
      }
    }
  }
  return std::move(file_numbers);
}
std::vector<std::uint64_t> ListTableFiles(Env* env, const std::string& path) {
  return ListSpecificFiles(env, path, kTableFile);
}
}
TEST_F(DBTest, FlushOneColumnFamily) {
  Options options = CurrentOptions();
  CreateAndReopenWithCF({"pikachu", "ilya", "muromec", "dobrynia", "nikitich",
                         "alyosha", "popovich"},
                        options);
  ASSERT_OK(Put(0, "Default", "Default"));
  ASSERT_OK(Put(1, "pikachu", "pikachu"));
  ASSERT_OK(Put(2, "ilya", "ilya"));
  ASSERT_OK(Put(3, "muromec", "muromec"));
  ASSERT_OK(Put(4, "dobrynia", "dobrynia"));
  ASSERT_OK(Put(5, "nikitich", "nikitich"));
  ASSERT_OK(Put(6, "alyosha", "alyosha"));
  ASSERT_OK(Put(7, "popovich", "popovich"));
  for (int i = 0; i < 8; ++i) {
    Flush(i);
    auto tables = ListTableFiles(env_, dbname_);
    ASSERT_EQ(tables.size(), i + 1U);
  }
}
TEST_F(DBTest, RecoverCheckFileAmountWithSmallWriteBuffer) {
  Options options = CurrentOptions();
  options.write_buffer_size = 5000000;
  CreateAndReopenWithCF({"pikachu", "dobrynia", "nikitich"}, options);
  ASSERT_OK(Put(1, Key(10), DummyString(1000000)));
  ASSERT_OK(Put(1, Key(10), DummyString(1000000)));
  ASSERT_OK(Put(1, Key(10), DummyString(1000000)));
  ASSERT_OK(Put(1, Key(10), DummyString(1000000)));
  ASSERT_OK(Put(3, Key(10), DummyString(1)));
  ASSERT_OK(Put(2, Key(10), DummyString(7500000)));
  ASSERT_OK(Put(2, Key(1), DummyString(1)));
  dbfull()->TEST_WaitForFlushMemTable(handles_[2]);
  {
    auto tables = ListTableFiles(env_, dbname_);
    ASSERT_EQ(tables.size(), static_cast<size_t>(1));
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "dobrynia"),
              static_cast<uint64_t>(1));
  }
  ASSERT_OK(Put(1, Key(1), DummyString(1)));
  ASSERT_OK(Put(1, Key(1), DummyString(1)));
  ASSERT_OK(Put(3, Key(10), DummyString(1)));
  ASSERT_OK(Put(3, Key(10), DummyString(1)));
  ASSERT_OK(Put(3, Key(10), DummyString(1)));
  options.write_buffer_size = 10;
  ReopenWithColumnFamilies({"default", "pikachu", "dobrynia", "nikitich"},
                           options);
  {
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "default"),
              static_cast<uint64_t>(0));
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "pikachu"),
              static_cast<uint64_t>(5));
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "dobrynia"),
              static_cast<uint64_t>(2));
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "nikitich"),
              static_cast<uint64_t>(1));
  }
}
TEST_F(DBTest, RecoverCheckFileAmount) {
  Options options = CurrentOptions();
  options.write_buffer_size = 100000;
  CreateAndReopenWithCF({"pikachu", "dobrynia", "nikitich"}, options);
  ASSERT_OK(Put(0, Key(1), DummyString(1)));
  ASSERT_OK(Put(1, Key(1), DummyString(1)));
  ASSERT_OK(Put(2, Key(1), DummyString(1)));
  ASSERT_OK(Put(3, Key(10), DummyString(1002400)));
  ASSERT_OK(Put(3, Key(1), DummyString(1)));
  dbfull()->TEST_WaitForFlushMemTable(handles_[3]);
  {
    auto tables = ListTableFiles(env_, dbname_);
    ASSERT_EQ(tables.size(), static_cast<size_t>(1));
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "nikitich"),
              static_cast<uint64_t>(1));
  }
  ASSERT_OK(Put(0, Key(1), DummyString(1)));
  ASSERT_OK(Put(1, Key(1), DummyString(1)));
  ASSERT_OK(Put(2, Key(1), DummyString(1)));
  ASSERT_OK(Put(3, Key(10), DummyString(1002400)));
  ASSERT_OK(Put(3, Key(1), DummyString(1)));
  dbfull()->TEST_WaitForFlushMemTable(handles_[3]);
  ASSERT_OK(Put(0, Key(1), DummyString(1)));
  ASSERT_OK(Put(1, Key(1), DummyString(1)));
  ASSERT_OK(Put(2, Key(1), DummyString(1)));
  {
    auto tables = ListTableFiles(env_, dbname_);
    ASSERT_EQ(tables.size(), static_cast<size_t>(2));
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "nikitich"),
              static_cast<uint64_t>(2));
  }
  ReopenWithColumnFamilies({"default", "pikachu", "dobrynia", "nikitich"},
                           options);
  {
    std::vector<uint64_t> table_files = ListTableFiles(env_, dbname_);
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "default"),
              static_cast<uint64_t>(1));
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "nikitich"),
              static_cast<uint64_t>(3));
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "dobrynia"),
              static_cast<uint64_t>(1));
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "pikachu"),
              static_cast<uint64_t>(1));
  }
}
TEST_F(DBTest, SharedWriteBuffer) {
  Options options = CurrentOptions();
  options.db_write_buffer_size = 100000;
  options.write_buffer_size = 500000;
  CreateAndReopenWithCF({"pikachu", "dobrynia", "nikitich"}, options);
  ASSERT_OK(Put(0, Key(1), DummyString(1)));
  ASSERT_OK(Put(1, Key(1), DummyString(1)));
  ASSERT_OK(Put(3, Key(1), DummyString(90000)));
  ASSERT_OK(Put(2, Key(2), DummyString(20000)));
  ASSERT_OK(Put(2, Key(1), DummyString(1)));
  dbfull()->TEST_WaitForFlushMemTable(handles_[0]);
  dbfull()->TEST_WaitForFlushMemTable(handles_[1]);
  dbfull()->TEST_WaitForFlushMemTable(handles_[2]);
  dbfull()->TEST_WaitForFlushMemTable(handles_[3]);
  {
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "default"),
              static_cast<uint64_t>(1));
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "pikachu"),
              static_cast<uint64_t>(1));
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "dobrynia"),
              static_cast<uint64_t>(1));
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "nikitich"),
              static_cast<uint64_t>(1));
  }
  ASSERT_OK(Put(2, Key(2), DummyString(50000)));
  ASSERT_OK(Put(3, Key(2), DummyString(40000)));
  ASSERT_OK(Put(2, Key(3), DummyString(20000)));
  ASSERT_OK(Put(3, Key(2), DummyString(40000)));
  dbfull()->TEST_WaitForFlushMemTable(handles_[1]);
  dbfull()->TEST_WaitForFlushMemTable(handles_[2]);
  dbfull()->TEST_WaitForFlushMemTable(handles_[3]);
  {
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "default"),
              static_cast<uint64_t>(1));
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "pikachu"),
              static_cast<uint64_t>(1));
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "dobrynia"),
              static_cast<uint64_t>(2));
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "nikitich"),
              static_cast<uint64_t>(2));
  }
  ASSERT_OK(Put(2, Key(2), DummyString(40000)));
  ASSERT_OK(Put(1, Key(2), DummyString(20000)));
  ASSERT_OK(Put(0, Key(1), DummyString(1)));
  dbfull()->TEST_WaitForFlushMemTable(handles_[2]);
  dbfull()->TEST_WaitForFlushMemTable(handles_[3]);
  {
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "default"),
              static_cast<uint64_t>(1));
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "pikachu"),
              static_cast<uint64_t>(2));
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "dobrynia"),
              static_cast<uint64_t>(3));
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "nikitich"),
              static_cast<uint64_t>(3));
  }
  ASSERT_OK(Put(3, Key(1), DummyString(1)));
  ReopenWithColumnFamilies({"default", "pikachu", "dobrynia", "nikitich"},
                           options);
  {
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "default"),
              static_cast<uint64_t>(2));
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "pikachu"),
              static_cast<uint64_t>(2));
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "dobrynia"),
              static_cast<uint64_t>(3));
    ASSERT_EQ(GetNumberOfSstFilesForColumnFamily(db_, "nikitich"),
              static_cast<uint64_t>(4));
  }
}
TEST_F(DBTest, PurgeInfoLogs) {
  Options options = CurrentOptions();
  options.keep_log_file_num = 5;
  options.create_if_missing = true;
  for (int mode = 0; mode <= 1; mode++) {
    if (mode == 1) {
      options.db_log_dir = dbname_ + "_logs";
      env_->CreateDirIfMissing(options.db_log_dir);
    } else {
      options.db_log_dir = "";
    }
    for (int i = 0; i < 8; i++) {
      Reopen(options);
    }
    std::vector<std::string> files;
    env_->GetChildren(options.db_log_dir.empty() ? dbname_ : options.db_log_dir,
                      &files);
    int info_log_count = 0;
    for (std::string file : files) {
      if (file.find("LOG") != std::string::npos) {
        info_log_count++;
      }
    }
    ASSERT_EQ(5, info_log_count);
    Destroy(options);
    std::vector<std::string> db_files;
    env_->GetChildren(dbname_, &db_files);
    for (std::string file : db_files) {
      ASSERT_TRUE(file.find("LOG") == std::string::npos);
    }
    if (mode == 1) {
      env_->GetChildren(options.db_log_dir, &files);
      for (std::string file : files) {
        env_->DeleteFile(options.db_log_dir + "/" + file);
      }
      env_->DeleteDir(options.db_log_dir);
    }
  }
}
namespace {
SequenceNumber ReadRecords(
    std::unique_ptr<TransactionLogIterator>& iter,
    int& count) {
  count = 0;
  SequenceNumber lastSequence = 0;
  BatchResult res;
  while (iter->Valid()) {
    res = iter->GetBatch();
    EXPECT_TRUE(res.sequence > lastSequence);
    ++count;
    lastSequence = res.sequence;
    EXPECT_OK(iter->status());
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
}
TEST_F(DBTest, TransactionLogIterator) {
  do {
    Options options = OptionsForLogIterTest();
    DestroyAndReopen(options);
    CreateAndReopenWithCF({"pikachu"}, options);
    Put(0, "key1", DummyString(1024));
    Put(1, "key2", DummyString(1024));
    Put(1, "key2", DummyString(1024));
    ASSERT_EQ(dbfull()->GetLatestSequenceNumber(), 3U);
    {
      auto iter = OpenTransactionLogIter(0);
      ExpectRecords(3, iter);
    }
    ReopenWithColumnFamilies({"default", "pikachu"}, options);
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
TEST_F(DBTest, TransactionLogIteratorRace) {
  static const int LOG_ITERATOR_RACE_TEST_COUNT = 2;
  static const char* sync_points[LOG_ITERATOR_RACE_TEST_COUNT][4] = {
      {"WalManager::GetSortedWalFiles:1", "WalManager::PurgeObsoleteFiles:1",
       "WalManager::PurgeObsoleteFiles:2", "WalManager::GetSortedWalFiles:2"},
      {"WalManager::GetSortedWalsOfType:1",
       "WalManager::PurgeObsoleteFiles:1",
       "WalManager::PurgeObsoleteFiles:2",
       "WalManager::GetSortedWalsOfType:2"}};
  for (int test = 0; test < LOG_ITERATOR_RACE_TEST_COUNT; ++test) {
    rocksdb::SyncPoint::GetInstance()->LoadDependency(
      { { sync_points[test][0], sync_points[test][1] },
        { sync_points[test][2], sync_points[test][3] },
      });
    do {
      rocksdb::SyncPoint::GetInstance()->ClearTrace();
      rocksdb::SyncPoint::GetInstance()->DisableProcessing();
      Options options = OptionsForLogIterTest();
      DestroyAndReopen(options);
      Put("key1", DummyString(1024));
      dbfull()->Flush(FlushOptions());
      Put("key2", DummyString(1024));
      dbfull()->Flush(FlushOptions());
      Put("key3", DummyString(1024));
      dbfull()->Flush(FlushOptions());
      Put("key4", DummyString(1024));
      ASSERT_EQ(dbfull()->GetLatestSequenceNumber(), 4U);
      {
        auto iter = OpenTransactionLogIter(0);
        ExpectRecords(4, iter);
      }
      rocksdb::SyncPoint::GetInstance()->EnableProcessing();
      FlushOptions flush_options;
      flush_options.wait = false;
      dbfull()->Flush(flush_options);
      Put("key5", DummyString(1024));
      {
        auto iter = OpenTransactionLogIter(0);
        ExpectRecords(5, iter);
      }
    } while (ChangeCompactOptions());
  }
}
TEST_F(DBTest, TransactionLogIteratorStallAtLastRecord) {
  do {
    Options options = OptionsForLogIterTest();
    DestroyAndReopen(options);
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
TEST_F(DBTest, TransactionLogIteratorCheckAfterRestart) {
  do {
    Options options = OptionsForLogIterTest();
    DestroyAndReopen(options);
    Put("key1", DummyString(1024));
    Put("key2", DummyString(1023));
    dbfull()->Flush(FlushOptions());
    Reopen(options);
    auto iter = OpenTransactionLogIter(0);
    ExpectRecords(2, iter);
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, TransactionLogIteratorCorruptedLog) {
  do {
    Options options = OptionsForLogIterTest();
    DestroyAndReopen(options);
    for (int i = 0; i < 1024; i++) {
      Put("key"+ToString(i), DummyString(10));
    }
    dbfull()->Flush(FlushOptions());
    rocksdb::VectorLogPtr wal_files;
    ASSERT_OK(dbfull()->GetSortedWalFiles(wal_files));
    const auto logfile_path = dbname_ + "/" + wal_files.front()->PathName();
    if (mem_env_) {
      mem_env_->Truncate(logfile_path, wal_files.front()->SizeFileBytes() / 2);
    } else {
      ASSERT_EQ(0, truncate(logfile_path.c_str(),
                   wal_files.front()->SizeFileBytes() / 2));
    }
    Put("key1025", DummyString(10));
    auto iter = OpenTransactionLogIter(0);
    int count;
    SequenceNumber last_sequence_read = ReadRecords(iter, count);
    ASSERT_LT(last_sequence_read, 1025U);
    auto iter2 = OpenTransactionLogIter(last_sequence_read + 1);
    ExpectRecords(1, iter2);
  } while (ChangeCompactOptions());
}
class RecoveryTestHelper {
 public:
  static const int kWALFilesCount = 10;
  static const int kWALFileOffset = 10;
  static const int kKeysPerWALFile = 1024;
  static const int kValueSize = 10;
  static void FillData(DBTest* test, Options& options,
                       const size_t wal_count, size_t & count) {
    DBOptions & db_options = options;
    count = 0;
    shared_ptr<Cache> table_cache = NewLRUCache(50000, 16);
    EnvOptions env_options;
    WriteBuffer write_buffer(db_options.db_write_buffer_size);
    unique_ptr<VersionSet> versions;
    unique_ptr<WalManager> wal_manager;
    WriteController write_controller;
    versions.reset(new VersionSet(test->dbname_, &db_options, env_options,
                                  table_cache.get(), &write_buffer,
                                  &write_controller));
    wal_manager.reset(new WalManager(db_options, env_options));
    std::unique_ptr<log::Writer> current_log_writer;
    for (size_t j = kWALFileOffset; j < wal_count + kWALFileOffset; j++) {
      uint64_t current_log_number = j;
      std::string fname = LogFileName(test->dbname_, current_log_number);
      unique_ptr<WritableFile> file;
      ASSERT_OK(db_options.env->NewWritableFile(fname, &file, env_options));
      current_log_writer.reset(new log::Writer(std::move(file)));
      for (int i = 0; i < kKeysPerWALFile; i++) {
        std::string key = "key" + ToString(count++);
        std::string value = test->DummyString(kValueSize);
        assert(current_log_writer.get() != nullptr);
        uint64_t seq = versions->LastSequence() + 1;
        WriteBatch batch;
        batch.Put(key, value);
        WriteBatchInternal::SetSequence(&batch, seq);
        current_log_writer->AddRecord(WriteBatchInternal::Contents(&batch));
        versions->SetLastSequence(seq);
      }
    }
  }
  static size_t FillData(DBTest* test, Options& options) {
    options.create_if_missing = true;
    test->DestroyAndReopen(options);
    test->Close();
    size_t count = 0;
    FillData(test, options, kWALFilesCount, count);
    return count;
  }
  static size_t GetData(DBTest* test) {
    size_t count = 0;
    for (size_t i = 0; i < kWALFilesCount * kKeysPerWALFile; i++) {
      if (test->Get("key" + ToString(i)) != "NOT_FOUND") {
        ++count;
      }
    }
    return count;
  }
  static void CorruptWAL(DBTest * test, Options& options,
                         const double off, const double len,
                         const int wal_file_id, const bool trunc = false) {
    Env* env = options.env;
    std::string fname = LogFileName(test->dbname_, wal_file_id);
    uint64_t size;
    ASSERT_OK(env->GetFileSize(fname, &size));
    ASSERT_GT(size, 0);
    if (trunc) {
      ASSERT_EQ(0, truncate(fname.c_str(), size * off));
    } else {
      InduceCorruption(fname, size * off, size * len);
    }
  }
  static void InduceCorruption(const std::string& filename, uint32_t offset,
                               uint32_t len) {
    ASSERT_GT(len, 0);
    int fd = open(filename.c_str(), O_RDWR);
    ASSERT_GT(fd, 0);
    ASSERT_EQ(offset, lseek(fd, offset, SEEK_SET));
    void* buf = alloca(len);
    memset(buf, 'a', len);
    ASSERT_EQ(len, write(fd, buf, len));
    close(fd);
  }
};
TEST_F(DBTest, kTolerateCorruptedTailRecords) {
  const int jstart = RecoveryTestHelper::kWALFileOffset;
  const int jend = jstart + RecoveryTestHelper::kWALFilesCount;
  for (auto trunc : {true, false}) {
    for (int i = 0; i < 4; i++) {
      for (int j = jstart; j < jend; j++) {
        Options options = CurrentOptions();
        const size_t row_count = RecoveryTestHelper::FillData(this, options);
        RecoveryTestHelper::CorruptWAL(this, options, i * .3,
                                                 .1, j, trunc);
        if (trunc) {
          options.wal_recovery_mode =
              WALRecoveryMode::kTolerateCorruptedTailRecords;
          options.create_if_missing = false;
          ASSERT_OK(TryReopen(options));
          const size_t recovered_row_count = RecoveryTestHelper::GetData(this);
          ASSERT_TRUE(i == 0 || recovered_row_count > 0);
          ASSERT_LT(recovered_row_count, row_count);
        } else {
          options.wal_recovery_mode =
              WALRecoveryMode::kTolerateCorruptedTailRecords;
          ASSERT_NOK(TryReopen(options));
        }
      }
    }
  }
}
TEST_F(DBTest, kAbsoluteConsistency) {
  const int jstart = RecoveryTestHelper::kWALFileOffset;
  const int jend = jstart + RecoveryTestHelper::kWALFilesCount;
  Options options = CurrentOptions();
  const size_t row_count = RecoveryTestHelper::FillData(this, options);
  options.wal_recovery_mode = WALRecoveryMode::kAbsoluteConsistency;
  options.create_if_missing = false;
  ASSERT_OK(TryReopen(options));
  ASSERT_EQ(RecoveryTestHelper::GetData(this), row_count);
  for (auto trunc : {true, false}) {
    for (int i = 0; i < 4; i++) {
      if (trunc && i == 0) {
        continue;
      }
      for (int j = jstart; j < jend; j++) {
        RecoveryTestHelper::FillData(this, options);
        RecoveryTestHelper::CorruptWAL(this, options, i * .3,
                                                .1, j, trunc);
        options.wal_recovery_mode = WALRecoveryMode::kAbsoluteConsistency;
        options.create_if_missing = false;
        ASSERT_NOK(TryReopen(options));
      }
    }
  }
}
TEST_F(DBTest, kPointInTimeRecovery) {
  const int jstart = RecoveryTestHelper::kWALFileOffset;
  const int jend = jstart + RecoveryTestHelper::kWALFilesCount;
  const int maxkeys = RecoveryTestHelper::kWALFilesCount *
                        RecoveryTestHelper::kKeysPerWALFile;
  for (auto trunc : {true, false}) {
    for (int i = 0; i < 4; i++) {
      for (int j = jstart; j < jend; j++) {
        Options options = CurrentOptions();
        const size_t row_count = RecoveryTestHelper::FillData(this, options);
        RecoveryTestHelper::CorruptWAL(this, options, i * .3,
                                                .1, j, trunc);
        options.wal_recovery_mode = WALRecoveryMode::kPointInTimeRecovery;
        options.create_if_missing = false;
        ASSERT_OK(TryReopen(options));
        size_t recovered_row_count = RecoveryTestHelper::GetData(this);
        ASSERT_LT(recovered_row_count, row_count);
        bool expect_data = true;
        for (size_t k = 0; k < maxkeys; ++k) {
          bool found = Get("key" + ToString(i)) != "NOT_FOUND";
          if (expect_data && !found) {
            expect_data = false;
          }
          ASSERT_EQ(found, expect_data);
        }
        const size_t min = RecoveryTestHelper::kKeysPerWALFile *
                              (j - RecoveryTestHelper::kWALFileOffset);
        ASSERT_GE(recovered_row_count, min);
        if (!trunc && i != 0) {
          const size_t max = RecoveryTestHelper::kKeysPerWALFile *
                                (j - RecoveryTestHelper::kWALFileOffset + 1);
          ASSERT_LE(recovered_row_count, max);
        }
      }
    }
  }
}
TEST_F(DBTest, kSkipAnyCorruptedRecords) {
  const int jstart = RecoveryTestHelper::kWALFileOffset;
  const int jend = jstart + RecoveryTestHelper::kWALFilesCount;
  for (auto trunc : {true, false}) {
    for (int i = 0; i < 4; i++) {
      for (int j = jstart; j < jend; j++) {
        Options options = CurrentOptions();
        const size_t row_count = RecoveryTestHelper::FillData(this, options);
        RecoveryTestHelper::CorruptWAL(this, options, i * .3,
                                                .1, j, trunc);
        options.wal_recovery_mode = WALRecoveryMode::kSkipAnyCorruptedRecords;
        options.create_if_missing = false;
        ASSERT_OK(TryReopen(options));
        size_t recovered_row_count = RecoveryTestHelper::GetData(this);
        ASSERT_LT(recovered_row_count, row_count);
        if (!trunc) {
          ASSERT_TRUE(i != 0 || recovered_row_count > 0);
        }
      }
    }
  }
}
TEST_F(DBTest, TransactionLogIteratorBatchOperations) {
  do {
    Options options = OptionsForLogIterTest();
    DestroyAndReopen(options);
    CreateAndReopenWithCF({"pikachu"}, options);
    WriteBatch batch;
    batch.Put(handles_[1], "key1", DummyString(1024));
    batch.Put(handles_[0], "key2", DummyString(1024));
    batch.Put(handles_[1], "key3", DummyString(1024));
    batch.Delete(handles_[0], "key2");
    dbfull()->Write(WriteOptions(), &batch);
    Flush(1);
    Flush(0);
    ReopenWithColumnFamilies({"default", "pikachu"}, options);
    Put(1, "key4", DummyString(1024));
    auto iter = OpenTransactionLogIter(3);
    ExpectRecords(2, iter);
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, TransactionLogIteratorBlobs) {
  Options options = OptionsForLogIterTest();
  DestroyAndReopen(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  {
    WriteBatch batch;
    batch.Put(handles_[1], "key1", DummyString(1024));
    batch.Put(handles_[0], "key2", DummyString(1024));
    batch.PutLogData(Slice("blob1"));
    batch.Put(handles_[1], "key3", DummyString(1024));
    batch.PutLogData(Slice("blob2"));
    batch.Delete(handles_[0], "key2");
    dbfull()->Write(WriteOptions(), &batch);
    ReopenWithColumnFamilies({"default", "pikachu"}, options);
  }
  auto res = OpenTransactionLogIter(0)->GetBatch();
  struct Handler : public WriteBatch::Handler {
    std::string seen;
    virtual Status PutCF(uint32_t cf, const Slice& key,
                         const Slice& value) override {
      seen += "Put(" + ToString(cf) + ", " + key.ToString() + ", " +
              ToString(value.size()) + ")";
      return Status::OK();
    }
    virtual Status MergeCF(uint32_t cf, const Slice& key,
                           const Slice& value) override {
      seen += "Merge(" + ToString(cf) + ", " + key.ToString() + ", " +
              ToString(value.size()) + ")";
      return Status::OK();
    }
    virtual void LogData(const Slice& blob) override {
      seen += "LogData(" + blob.ToString() + ")";
    }
    virtual Status DeleteCF(uint32_t cf, const Slice& key) override {
      seen += "Delete(" + ToString(cf) + ", " + key.ToString() + ")";
      return Status::OK();
    }
  } handler;
  res.writeBatchPtr->Iterate(&handler);
  ASSERT_EQ(
      "Put(1, key1, 1024)"
      "Put(0, key2, 1024)"
      "LogData(blob1)"
      "Put(1, key3, 1024)"
      "LogData(blob2)"
      "Delete(0, key2)",
      handler.seen);
}
namespace {
static const int kColumnFamilies = 10;
static const int kNumThreads = 10;
static const int kTestSeconds = 10;
static const int kNumKeys = 1000;
struct MTState {
  DBTest* test;
  std::atomic<bool> stop;
  std::atomic<int> counter[kNumThreads];
  std::atomic<bool> thread_done[kNumThreads];
};
struct MTThread {
  MTState* state;
  int id;
};
static void MTThreadBody(void* arg) {
  MTThread* t = reinterpret_cast<MTThread*>(arg);
  int id = t->id;
  DB* db = t->state->test->db_;
  int counter = 0;
  fprintf(stderr, "... starting thread %d\n", id);
  Random rnd(1000 + id);
  char valbuf[1500];
  while (t->state->stop.load(std::memory_order_acquire) == false) {
    t->state->counter[id].store(counter, std::memory_order_release);
    int key = rnd.Uniform(kNumKeys);
    char keybuf[20];
    snprintf(keybuf, sizeof(keybuf), "%016d", key);
    if (rnd.OneIn(2)) {
      int unique_id = rnd.Uniform(1000000);
      if (rnd.OneIn(2)) {
        WriteBatch batch;
        for (int cf = 0; cf < kColumnFamilies; ++cf) {
          snprintf(valbuf, sizeof(valbuf), "%d.%d.%d.%d.%-1000d", key, id,
                   static_cast<int>(counter), cf, unique_id);
          batch.Put(t->state->test->handles_[cf], Slice(keybuf), Slice(valbuf));
        }
        ASSERT_OK(db->Write(WriteOptions(), &batch));
      } else {
        WriteBatchWithIndex batch(db->GetOptions().comparator);
        for (int cf = 0; cf < kColumnFamilies; ++cf) {
          snprintf(valbuf, sizeof(valbuf), "%d.%d.%d.%d.%-1000d", key, id,
                   static_cast<int>(counter), cf, unique_id);
          batch.Put(t->state->test->handles_[cf], Slice(keybuf), Slice(valbuf));
        }
        ASSERT_OK(db->Write(WriteOptions(), batch.GetWriteBatch()));
      }
    } else {
      std::vector<Slice> keys(kColumnFamilies, Slice(keybuf));
      std::vector<std::string> values;
      std::vector<Status> statuses =
          db->MultiGet(ReadOptions(), t->state->test->handles_, keys, &values);
      Status s = statuses[0];
      for (size_t i = 1; i < statuses.size(); ++i) {
        ASSERT_TRUE((s.ok() && statuses[i].ok()) ||
                    (s.IsNotFound() && statuses[i].IsNotFound()));
      }
      if (s.IsNotFound()) {
      } else {
        ASSERT_OK(s);
        int unique_id = -1;
        for (int i = 0; i < kColumnFamilies; ++i) {
          int k, w, c, cf, u;
          ASSERT_EQ(5, sscanf(values[i].c_str(), "%d.%d.%d.%d.%d", &k, &w,
                              &c, &cf, &u))
              << values[i];
          ASSERT_EQ(k, key);
          ASSERT_GE(w, 0);
          ASSERT_LT(w, kNumThreads);
          ASSERT_LE(c, t->state->counter[w].load(std::memory_order_acquire));
          ASSERT_EQ(cf, i);
          if (i == 0) {
            unique_id = u;
          } else {
            ASSERT_EQ(u, unique_id);
          }
        }
      }
    }
    counter++;
  }
  t->state->thread_done[id].store(true, std::memory_order_release);
  fprintf(stderr, "... stopping thread %d after %d ops\n", id, int(counter));
}
}
class MultiThreadedDBTest : public DBTest,
                            public ::testing::WithParamInterface<int> {
 public:
  virtual void SetUp() override { option_config_ = GetParam(); }
  static std::vector<int> GenerateOptionConfigs() {
    std::vector<int> optionConfigs;
    for (int optionConfig = kDefault; optionConfig < kEnd; ++optionConfig) {
      if (optionConfig != kHashCuckoo) {
        optionConfigs.push_back(optionConfig);
      }
    }
    return optionConfigs;
  }
};
TEST_P(MultiThreadedDBTest, MultiThreaded) {
  anon::OptionsOverride options_override;
  options_override.skip_policy = kSkipNoSnapshot;
  std::vector<std::string> cfs;
  for (int i = 1; i < kColumnFamilies; ++i) {
    cfs.push_back(ToString(i));
  }
  CreateAndReopenWithCF(cfs, CurrentOptions(options_override));
  MTState mt;
  mt.test = this;
  mt.stop.store(false, std::memory_order_release);
  for (int id = 0; id < kNumThreads; id++) {
    mt.counter[id].store(0, std::memory_order_release);
    mt.thread_done[id].store(false, std::memory_order_release);
  }
  MTThread thread[kNumThreads];
  for (int id = 0; id < kNumThreads; id++) {
    thread[id].state = &mt;
    thread[id].id = id;
    env_->StartThread(MTThreadBody, &thread[id]);
  }
  env_->SleepForMicroseconds(kTestSeconds * 1000000);
  mt.stop.store(true, std::memory_order_release);
  for (int id = 0; id < kNumThreads; id++) {
    while (mt.thread_done[id].load(std::memory_order_acquire) == false) {
      env_->SleepForMicroseconds(100000);
    }
  }
}
INSTANTIATE_TEST_CASE_P(
    MultiThreaded, MultiThreadedDBTest,
    ::testing::ValuesIn(MultiThreadedDBTest::GenerateOptionConfigs()));
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
    std::string kv(ToString(i + id * kGCNumKeys));
    ASSERT_OK(db->Put(wo, kv, kv));
  }
  t->done = true;
}
}
TEST_F(DBTest, GroupCommitTest) {
  do {
    Options options = CurrentOptions();
    options.env = env_;
    env_->log_write_slowdown_.store(100);
    options.statistics = rocksdb::CreateDBStatistics();
    Reopen(options);
    GCThread thread[kGCNumThreads];
    for (int id = 0; id < kGCNumThreads; id++) {
      thread[id].id = id;
      thread[id].db = db_;
      thread[id].done = false;
      env_->StartThread(GCThreadBody, &thread[id]);
    }
    for (int id = 0; id < kGCNumThreads; id++) {
      while (thread[id].done == false) {
        env_->SleepForMicroseconds(100000);
      }
    }
    env_->log_write_slowdown_.store(0);
    ASSERT_GT(TestGetTickerCount(options, WRITE_DONE_BY_OTHER), 0);
    std::vector<std::string> expected_db;
    for (int i = 0; i < kGCNumThreads * kGCNumKeys; ++i) {
      expected_db.push_back(ToString(i));
    }
    sort(expected_db.begin(), expected_db.end());
    Iterator* itr = db_->NewIterator(ReadOptions());
    itr->SeekToFirst();
    for (auto x : expected_db) {
      ASSERT_TRUE(itr->Valid());
      ASSERT_EQ(itr->key().ToString(), x);
      ASSERT_EQ(itr->value().ToString(), x);
      itr->Next();
    }
    ASSERT_TRUE(!itr->Valid());
    delete itr;
    HistogramData hist_data = {0};
    options.statistics->histogramData(DB_WRITE, &hist_data);
    ASSERT_GT(hist_data.average, 0.0);
  } while (ChangeOptions(kSkipNoSeekToLast));
}
namespace {
typedef std::map<std::string, std::string> KVMap;
}
class ModelDB: public DB {
 public:
  class ModelSnapshot : public Snapshot {
   public:
    KVMap map_;
    virtual SequenceNumber GetSequenceNumber() const override {
      assert(false);
      return 0;
    }
  };
  explicit ModelDB(const Options& options) : options_(options) {}
  using DB::Put;
  virtual Status Put(const WriteOptions& o, ColumnFamilyHandle* cf,
                     const Slice& k, const Slice& v) override {
    WriteBatch batch;
    batch.Put(cf, k, v);
    return Write(o, &batch);
  }
  using DB::Merge;
  virtual Status Merge(const WriteOptions& o, ColumnFamilyHandle* cf,
                       const Slice& k, const Slice& v) override {
    WriteBatch batch;
    batch.Merge(cf, k, v);
    return Write(o, &batch);
  }
  using DB::Delete;
  virtual Status Delete(const WriteOptions& o, ColumnFamilyHandle* cf,
                        const Slice& key) override {
    WriteBatch batch;
    batch.Delete(cf, key);
    return Write(o, &batch);
  }
  using DB::Get;
  virtual Status Get(const ReadOptions& options, ColumnFamilyHandle* cf,
                     const Slice& key, std::string* value) override {
    return Status::NotSupported(key);
  }
  using DB::MultiGet;
  virtual std::vector<Status> MultiGet(
      const ReadOptions& options,
      const std::vector<ColumnFamilyHandle*>& column_family,
      const std::vector<Slice>& keys,
      std::vector<std::string>* values) override {
    std::vector<Status> s(keys.size(),
                          Status::NotSupported("Not implemented."));
    return s;
  }
  using DB::GetPropertiesOfAllTables;
  virtual Status GetPropertiesOfAllTables(
      ColumnFamilyHandle* column_family,
      TablePropertiesCollection* props) override {
    return Status();
  }
  using DB::KeyMayExist;
  virtual bool KeyMayExist(const ReadOptions& options,
                           ColumnFamilyHandle* column_family, const Slice& key,
                           std::string* value,
                           bool* value_found = nullptr) override {
    if (value_found != nullptr) {
      *value_found = false;
    }
    return true;
  }
  using DB::NewIterator;
  virtual Iterator* NewIterator(const ReadOptions& options,
                                ColumnFamilyHandle* column_family) override {
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
      std::vector<Iterator*>* iterators) override {
    return Status::NotSupported("Not supported yet");
  }
  virtual const Snapshot* GetSnapshot() override {
    ModelSnapshot* snapshot = new ModelSnapshot;
    snapshot->map_ = map_;
    return snapshot;
  }
  virtual void ReleaseSnapshot(const Snapshot* snapshot) override {
    delete reinterpret_cast<const ModelSnapshot*>(snapshot);
  }
  virtual Status Write(const WriteOptions& options,
                       WriteBatch* batch) override {
    class Handler : public WriteBatch::Handler {
     public:
      KVMap* map_;
      virtual void Put(const Slice& key, const Slice& value) override {
        (*map_)[key.ToString()] = value.ToString();
      }
      virtual void Merge(const Slice& key, const Slice& value) override {
      }
      virtual void Delete(const Slice& key) override {
        map_->erase(key.ToString());
      }
    };
    Handler handler;
    handler.map_ = &map_;
    return batch->Iterate(&handler);
  }
  using DB::GetProperty;
  virtual bool GetProperty(ColumnFamilyHandle* column_family,
                           const Slice& property, std::string* value) override {
    return false;
  }
  using DB::GetIntProperty;
  virtual bool GetIntProperty(ColumnFamilyHandle* column_family,
                              const Slice& property, uint64_t* value) override {
    return false;
  }
  using DB::GetApproximateSizes;
  virtual void GetApproximateSizes(ColumnFamilyHandle* column_family,
                                   const Range* range, int n, uint64_t* sizes,
                                   bool include_memtable) override {
    for (int i = 0; i < n; i++) {
      sizes[i] = 0;
    }
  }
  using DB::CompactRange;
  virtual Status CompactRange(const CompactRangeOptions& options,
                              ColumnFamilyHandle* column_family,
                              const Slice* start, const Slice* end) override {
    return Status::NotSupported("Not supported operation.");
  }
  using DB::CompactFiles;
  virtual Status CompactFiles(
      const CompactionOptions& compact_options,
      ColumnFamilyHandle* column_family,
      const std::vector<std::string>& input_file_names,
      const int output_level, const int output_path_id = -1) override {
    return Status::NotSupported("Not supported operation.");
  }
  using DB::NumberLevels;
  virtual int NumberLevels(ColumnFamilyHandle* column_family) override {
    return 1;
  }
  using DB::MaxMemCompactionLevel;
  virtual int MaxMemCompactionLevel(
      ColumnFamilyHandle* column_family) override {
    return 1;
  }
  using DB::Level0StopWriteTrigger;
  virtual int Level0StopWriteTrigger(
      ColumnFamilyHandle* column_family) override {
    return -1;
  }
  virtual const std::string& GetName() const override { return name_; }
  virtual Env* GetEnv() const override { return nullptr; }
  using DB::GetOptions;
  virtual const Options& GetOptions(
      ColumnFamilyHandle* column_family) const override {
    return options_;
  }
  using DB::GetDBOptions;
  virtual const DBOptions& GetDBOptions() const override { return options_; }
  using DB::Flush;
  virtual Status Flush(const rocksdb::FlushOptions& options,
                       ColumnFamilyHandle* column_family) override {
    Status ret;
    return ret;
  }
  virtual Status DisableFileDeletions() override { return Status::OK(); }
  virtual Status EnableFileDeletions(bool force) override {
    return Status::OK();
  }
  virtual Status GetLiveFiles(std::vector<std::string>&, uint64_t* size,
                              bool flush_memtable = true) override {
    return Status::OK();
  }
  virtual Status GetSortedWalFiles(VectorLogPtr& files) override {
    return Status::OK();
  }
  virtual Status DeleteFile(std::string name) override { return Status::OK(); }
  virtual Status GetDbIdentity(std::string& identity) const override {
    return Status::OK();
  }
  virtual SequenceNumber GetLatestSequenceNumber() const override { return 0; }
  virtual Status GetUpdatesSince(
      rocksdb::SequenceNumber, unique_ptr<rocksdb::TransactionLogIterator>*,
      const TransactionLogIterator::ReadOptions&
          read_options = TransactionLogIterator::ReadOptions()) override {
    return Status::NotSupported("Not supported in Model DB");
  }
  virtual ColumnFamilyHandle* DefaultColumnFamily() const override {
    return nullptr;
  }
  virtual void GetColumnFamilyMetaData(
      ColumnFamilyHandle* column_family,
      ColumnFamilyMetaData* metadata) override {}
 private:
  class ModelIter: public Iterator {
   public:
    ModelIter(const KVMap* map, bool owned)
        : map_(map), owned_(owned), iter_(map_->end()) {
    }
    ~ModelIter() {
      if (owned_) delete map_;
    }
    virtual bool Valid() const override { return iter_ != map_->end(); }
    virtual void SeekToFirst() override { iter_ = map_->begin(); }
    virtual void SeekToLast() override {
      if (map_->empty()) {
        iter_ = map_->end();
      } else {
        iter_ = map_->find(map_->rbegin()->first);
      }
    }
    virtual void Seek(const Slice& k) override {
      iter_ = map_->lower_bound(k.ToString());
    }
    virtual void Next() override { ++iter_; }
    virtual void Prev() override {
      if (iter_ == map_->begin()) {
        iter_ = map_->end();
        return;
      }
      --iter_;
    }
    virtual Slice key() const override { return iter_->first; }
    virtual Slice value() const override { return iter_->second; }
    virtual Status status() const override { return Status::OK(); }
   private:
    const KVMap* const map_;
    const bool owned_;
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
           ? 1
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
TEST_F(DBTest, Randomized) {
  anon::OptionsOverride options_override;
  options_override.skip_policy = kSkipNoSnapshot;
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions(options_override));
    const int N = 10000;
    const Snapshot* model_snap = nullptr;
    const Snapshot* db_snap = nullptr;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      int p = rnd.Uniform(100);
      int minimum = 0;
      if (option_config_ == kHashSkipList ||
          option_config_ == kHashLinkList ||
          option_config_ == kHashCuckoo ||
          option_config_ == kPlainTableFirstBytePrefix ||
          option_config_ == kBlockBasedTableWithWholeKeyHashIndex ||
          option_config_ == kBlockBasedTableWithPrefixHashIndex) {
        minimum = 1;
      }
      if (p < 45) {
        k = RandomKey(&rnd, minimum);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd, minimum);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd, minimum);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        if (option_config_ != kBlockBasedTableWithWholeKeyHashIndex &&
            option_config_ != kBlockBasedTableWithPrefixHashIndex) {
          ASSERT_TRUE(CompareIterators(step, &model, db_, nullptr, nullptr));
          ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        }
        if (model_snap != nullptr) model.ReleaseSnapshot(model_snap);
        if (db_snap != nullptr) db_->ReleaseSnapshot(db_snap);
        auto options = CurrentOptions(options_override);
        Reopen(options);
        ASSERT_TRUE(CompareIterators(step, &model, db_, nullptr, nullptr));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
      if ((step % 2000) == 0) {
        fprintf(stderr,
                "DBTest.Randomized, option ID: %d, step: %d out of %d\n",
                option_config_, step, N);
      }
    }
    if (model_snap != nullptr) model.ReleaseSnapshot(model_snap);
    if (db_snap != nullptr) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions(kSkipDeletesFilterFirst | kSkipNoSeekToLast |
                         kSkipHashCuckoo));
}
TEST_F(DBTest, MultiGetSimple) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    ASSERT_OK(Put(1, "k1", "v1"));
    ASSERT_OK(Put(1, "k2", "v2"));
    ASSERT_OK(Put(1, "k3", "v3"));
    ASSERT_OK(Put(1, "k4", "v4"));
    ASSERT_OK(Delete(1, "k4"));
    ASSERT_OK(Put(1, "k5", "v5"));
    ASSERT_OK(Delete(1, "no_key"));
    std::vector<Slice> keys({"k1", "k2", "k3", "k4", "k5", "no_key"});
    std::vector<std::string> values(20, "Temporary data to be overwritten");
    std::vector<ColumnFamilyHandle*> cfs(keys.size(), handles_[1]);
    std::vector<Status> s = db_->MultiGet(ReadOptions(), cfs, keys, &values);
    ASSERT_EQ(values.size(), keys.size());
    ASSERT_EQ(values[0], "v1");
    ASSERT_EQ(values[1], "v2");
    ASSERT_EQ(values[2], "v3");
    ASSERT_EQ(values[4], "v5");
    ASSERT_OK(s[0]);
    ASSERT_OK(s[1]);
    ASSERT_OK(s[2]);
    ASSERT_TRUE(s[3].IsNotFound());
    ASSERT_OK(s[4]);
    ASSERT_TRUE(s[5].IsNotFound());
  } while (ChangeCompactOptions());
}
TEST_F(DBTest, MultiGetEmpty) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    std::vector<Slice> keys;
    std::vector<std::string> values;
    std::vector<ColumnFamilyHandle*> cfs;
    std::vector<Status> s = db_->MultiGet(ReadOptions(), cfs, keys, &values);
    ASSERT_EQ(s.size(), 0U);
    Options options = CurrentOptions();
    options.create_if_missing = true;
    DestroyAndReopen(options);
    CreateAndReopenWithCF({"pikachu"}, options);
    s = db_->MultiGet(ReadOptions(), cfs, keys, &values);
    ASSERT_EQ(s.size(), 0U);
    keys.resize(2);
    keys[0] = "a";
    keys[1] = "b";
    cfs.push_back(handles_[0]);
    cfs.push_back(handles_[1]);
    s = db_->MultiGet(ReadOptions(), cfs, keys, &values);
    ASSERT_EQ((int)s.size(), 2);
    ASSERT_TRUE(s[0].IsNotFound() && s[1].IsNotFound());
  } while (ChangeCompactOptions());
}
namespace {
void PrefixScanInit(DBTest *dbtest) {
  char buf[100];
  std::string keystr;
  const int small_range_sstfiles = 5;
  const int big_range_sstfiles = 5;
  snprintf(buf, sizeof(buf), "%02d______:start", 0);
  keystr = std::string(buf);
  ASSERT_OK(dbtest->Put(keystr, keystr));
  snprintf(buf, sizeof(buf), "%02d______:end", 10);
  keystr = std::string(buf);
  ASSERT_OK(dbtest->Put(keystr, keystr));
  dbtest->Flush();
  dbtest->dbfull()->CompactRange(CompactRangeOptions(), nullptr,
                                 nullptr);
  for (int i = 1; i <= small_range_sstfiles; i++) {
    snprintf(buf, sizeof(buf), "%02d______:start", i);
    keystr = std::string(buf);
    ASSERT_OK(dbtest->Put(keystr, keystr));
    snprintf(buf, sizeof(buf), "%02d______:end", i+1);
    keystr = std::string(buf);
    ASSERT_OK(dbtest->Put(keystr, keystr));
    dbtest->Flush();
  }
  for (int i = 1; i <= big_range_sstfiles; i++) {
    snprintf(buf, sizeof(buf), "%02d______:start", 0);
    keystr = std::string(buf);
    ASSERT_OK(dbtest->Put(keystr, keystr));
    snprintf(buf, sizeof(buf), "%02d______:end",
             small_range_sstfiles+i+1);
    keystr = std::string(buf);
    ASSERT_OK(dbtest->Put(keystr, keystr));
    dbtest->Flush();
  }
}
}
TEST_F(DBTest, PrefixScan) {
  XFUNC_TEST("", "dbtest_prefix", prefix_skip1, XFuncPoint::SetSkip,
             kSkipNoPrefix);
  while (ChangeFilterOptions()) {
    int count;
    Slice prefix;
    Slice key;
    char buf[100];
    Iterator* iter;
    snprintf(buf, sizeof(buf), "03______:");
    prefix = Slice(buf, 8);
    key = Slice(buf, 9);
    env_->count_random_reads_ = true;
    Options options = CurrentOptions();
    options.env = env_;
    options.prefix_extractor.reset(NewFixedPrefixTransform(8));
    options.disable_auto_compactions = true;
    options.max_background_compactions = 2;
    options.create_if_missing = true;
    options.memtable_factory.reset(NewHashSkipListRepFactory(16));
    BlockBasedTableOptions table_options;
    table_options.no_block_cache = true;
    table_options.filter_policy.reset(NewBloomFilterPolicy(10));
    table_options.whole_key_filtering = false;
    options.table_factory.reset(NewBlockBasedTableFactory(table_options));
    DestroyAndReopen(options);
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
    ASSERT_EQ(env_->random_read_counter_.Read(), 2);
    Close();
  }
  XFUNC_TEST("", "dbtest_prefix", prefix_skip1, XFuncPoint::SetSkip, 0);
}
TEST_F(DBTest, TailingIteratorSingle) {
  ReadOptions read_options;
  read_options.tailing = true;
  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  iter->SeekToFirst();
  ASSERT_TRUE(!iter->Valid());
  ASSERT_OK(db_->Put(WriteOptions(), "mirko", "fodor"));
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "mirko");
  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}
TEST_F(DBTest, TailingIteratorKeepAdding) {
  CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
  ReadOptions read_options;
  read_options.tailing = true;
  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  std::string value(1024, 'a');
  const int num_records = 10000;
  for (int i = 0; i < num_records; ++i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%016d", i);
    Slice key(buf, 16);
    ASSERT_OK(Put(1, key, value));
    iter->Seek(key);
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().compare(key), 0);
  }
}
TEST_F(DBTest, TailingIteratorSeekToNext) {
  CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
  ReadOptions read_options;
  read_options.tailing = true;
  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  std::string value(1024, 'a');
  const int num_records = 1000;
  for (int i = 1; i < num_records; ++i) {
    char buf1[32];
    char buf2[32];
    snprintf(buf1, sizeof(buf1), "00a0%016d", i * 5);
    Slice key(buf1, 20);
    ASSERT_OK(Put(1, key, value));
    if (i % 100 == 99) {
      ASSERT_OK(Flush(1));
    }
    snprintf(buf2, sizeof(buf2), "00a0%016d", i * 5 - 2);
    Slice target(buf2, 20);
    iter->Seek(target);
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().compare(key), 0);
  }
  for (int i = 2 * num_records; i > 0; --i) {
    char buf1[32];
    char buf2[32];
    snprintf(buf1, sizeof(buf1), "00a0%016d", i * 5);
    Slice key(buf1, 20);
    ASSERT_OK(Put(1, key, value));
    if (i % 100 == 99) {
      ASSERT_OK(Flush(1));
    }
    snprintf(buf2, sizeof(buf2), "00a0%016d", i * 5 - 2);
    Slice target(buf2, 20);
    iter->Seek(target);
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().compare(key), 0);
  }
}
TEST_F(DBTest, TailingIteratorDeletes) {
  CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
  ReadOptions read_options;
  read_options.tailing = true;
  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0test", "test"));
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0test");
  ASSERT_OK(Delete(1, "0test"));
  const int num_records = 10000;
  std::string value(1024, 'A');
  for (int i = 0; i < num_records; ++i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "1%015d", i);
    Slice key(buf, 16);
    ASSERT_OK(Put(1, key, value));
  }
  ASSERT_OK(Flush(1));
  iter->Next();
  int count = 0;
  for (; iter->Valid(); iter->Next(), ++count) ;
  ASSERT_EQ(count, num_records);
}
TEST_F(DBTest, TailingIteratorPrefixSeek) {
  XFUNC_TEST("", "dbtest_prefix", prefix_skip1, XFuncPoint::SetSkip,
             kSkipNoPrefix);
  ReadOptions read_options;
  read_options.tailing = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory(16));
  DestroyAndReopen(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));
  ASSERT_OK(Flush(1));
  ASSERT_OK(Put(1, "0202", "test"));
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());
  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");
  iter->Next();
  ASSERT_TRUE(!iter->Valid());
  XFUNC_TEST("", "dbtest_prefix", prefix_skip1, XFuncPoint::SetSkip, 0);
}
TEST_F(DBTest, TailingIteratorIncomplete) {
  CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.read_tier = kBlockCacheTier;
  std::string key("key");
  std::string value("value");
  ASSERT_OK(db_->Put(WriteOptions(), key, value));
  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid() || iter->status().IsIncomplete());
  ASSERT_OK(db_->CompactRange(CompactRangeOptions(), nullptr, nullptr));
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid() || iter->status().IsIncomplete());
}
TEST_F(DBTest, TailingIteratorSeekToSame) {
  Options options = CurrentOptions();
  options.compaction_style = kCompactionStyleUniversal;
  options.write_buffer_size = 1000;
  CreateAndReopenWithCF({"pikachu"}, options);
  ReadOptions read_options;
  read_options.tailing = true;
  const int NROWS = 10000;
  for (int i = 0; i < NROWS; ++i) {
    char buf[100];
    snprintf(buf, sizeof(buf), "%05d", 2*i);
    std::string key(buf);
    std::string value("value");
    ASSERT_OK(db_->Put(WriteOptions(), key, value));
  }
  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  std::string start_key = "00001";
  iter->Seek(start_key);
  ASSERT_TRUE(iter->Valid());
  std::string found = iter->key().ToString();
  ASSERT_EQ("00002", found);
  iter->Seek(found);
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(found, iter->key().ToString());
}
TEST_F(DBTest, ManagedTailingIteratorSingle) {
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.managed = true;
  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  iter->SeekToFirst();
  ASSERT_TRUE(!iter->Valid());
  ASSERT_OK(db_->Put(WriteOptions(), "mirko", "fodor"));
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "mirko");
  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}
TEST_F(DBTest, ManagedTailingIteratorKeepAdding) {
  CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.managed = true;
  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  std::string value(1024, 'a');
  const int num_records = 10000;
  for (int i = 0; i < num_records; ++i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%016d", i);
    Slice key(buf, 16);
    ASSERT_OK(Put(1, key, value));
    iter->Seek(key);
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().compare(key), 0);
  }
}
TEST_F(DBTest, ManagedTailingIteratorSeekToNext) {
  CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.managed = true;
  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  std::string value(1024, 'a');
  const int num_records = 1000;
  for (int i = 1; i < num_records; ++i) {
    char buf1[32];
    char buf2[32];
    snprintf(buf1, sizeof(buf1), "00a0%016d", i * 5);
    Slice key(buf1, 20);
    ASSERT_OK(Put(1, key, value));
    if (i % 100 == 99) {
      ASSERT_OK(Flush(1));
    }
    snprintf(buf2, sizeof(buf2), "00a0%016d", i * 5 - 2);
    Slice target(buf2, 20);
    iter->Seek(target);
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().compare(key), 0);
  }
  for (int i = 2 * num_records; i > 0; --i) {
    char buf1[32];
    char buf2[32];
    snprintf(buf1, sizeof(buf1), "00a0%016d", i * 5);
    Slice key(buf1, 20);
    ASSERT_OK(Put(1, key, value));
    if (i % 100 == 99) {
      ASSERT_OK(Flush(1));
    }
    snprintf(buf2, sizeof(buf2), "00a0%016d", i * 5 - 2);
    Slice target(buf2, 20);
    iter->Seek(target);
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().compare(key), 0);
  }
}
TEST_F(DBTest, ManagedTailingIteratorDeletes) {
  CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.managed = true;
  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0test", "test"));
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0test");
  ASSERT_OK(Delete(1, "0test"));
  const int num_records = 10000;
  std::string value(1024, 'A');
  for (int i = 0; i < num_records; ++i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "1%015d", i);
    Slice key(buf, 16);
    ASSERT_OK(Put(1, key, value));
  }
  ASSERT_OK(Flush(1));
  iter->Next();
  int count = 0;
  for (; iter->Valid(); iter->Next(), ++count) {
  }
  ASSERT_EQ(count, num_records);
}
TEST_F(DBTest, ManagedTailingIteratorPrefixSeek) {
  XFUNC_TEST("", "dbtest_prefix", prefix_skip1, XFuncPoint::SetSkip,
             kSkipNoPrefix);
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.managed = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.prefix_extractor.reset(NewFixedPrefixTransform(2));
  options.memtable_factory.reset(NewHashSkipListRepFactory(16));
  DestroyAndReopen(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options, handles_[1]));
  ASSERT_OK(Put(1, "0101", "test"));
  ASSERT_OK(Flush(1));
  ASSERT_OK(Put(1, "0202", "test"));
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());
  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");
  iter->Next();
  ASSERT_TRUE(!iter->Valid());
  XFUNC_TEST("", "dbtest_prefix", prefix_skip1, XFuncPoint::SetSkip, 0);
}
TEST_F(DBTest, ManagedTailingIteratorIncomplete) {
  CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.managed = true;
  read_options.read_tier = kBlockCacheTier;
  std::string key = "key";
  std::string value = "value";
  ASSERT_OK(db_->Put(WriteOptions(), key, value));
  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid() || iter->status().IsIncomplete());
  ASSERT_OK(db_->CompactRange(CompactRangeOptions(), nullptr, nullptr));
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid() || iter->status().IsIncomplete());
}
TEST_F(DBTest, ManagedTailingIteratorSeekToSame) {
  Options options = CurrentOptions();
  options.compaction_style = kCompactionStyleUniversal;
  options.write_buffer_size = 1000;
  CreateAndReopenWithCF({"pikachu"}, options);
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.managed = true;
  const int NROWS = 10000;
  for (int i = 0; i < NROWS; ++i) {
    char buf[100];
    snprintf(buf, sizeof(buf), "%05d", 2 * i);
    std::string key(buf);
    std::string value("value");
    ASSERT_OK(db_->Put(WriteOptions(), key, value));
  }
  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  std::string start_key = "00001";
  iter->Seek(start_key);
  ASSERT_TRUE(iter->Valid());
  std::string found = iter->key().ToString();
  ASSERT_EQ("00002", found);
  iter->Seek(found);
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(found, iter->key().ToString());
}
TEST_F(DBTest, BlockBasedTablePrefixIndexTest) {
  BlockBasedTableOptions table_options;
  Options options = CurrentOptions();
  table_options.index_type = BlockBasedTableOptions::kHashSearch;
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  options.prefix_extractor.reset(NewFixedPrefixTransform(1));
  Reopen(options);
  ASSERT_OK(Put("k1", "v1"));
  Flush();
  ASSERT_OK(Put("k2", "v2"));
  table_options.index_type = BlockBasedTableOptions::kBinarySearch;
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  options.prefix_extractor.reset();
  Reopen(options);
  ASSERT_EQ("v1", Get("k1"));
  ASSERT_EQ("v2", Get("k2"));
}
TEST_F(DBTest, ChecksumTest) {
  BlockBasedTableOptions table_options;
  Options options = CurrentOptions();
  table_options.checksum = kCRC32c;
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  Reopen(options);
  ASSERT_OK(Put("a", "b"));
  ASSERT_OK(Put("c", "d"));
  ASSERT_OK(Flush());
  table_options.checksum = kxxHash;
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  Reopen(options);
  ASSERT_OK(Put("e", "f"));
  ASSERT_OK(Put("g", "h"));
  ASSERT_OK(Flush());
  table_options.checksum = kCRC32c;
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  Reopen(options);
  ASSERT_EQ("b", Get("a"));
  ASSERT_EQ("d", Get("c"));
  ASSERT_EQ("f", Get("e"));
  ASSERT_EQ("h", Get("g"));
  table_options.checksum = kCRC32c;
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  Reopen(options);
  ASSERT_EQ("b", Get("a"));
  ASSERT_EQ("d", Get("c"));
  ASSERT_EQ("f", Get("e"));
  ASSERT_EQ("h", Get("g"));
}
TEST_F(DBTest, FIFOCompactionTest) {
  for (int iter = 0; iter < 2; ++iter) {
    Options options;
    options.compaction_style = kCompactionStyleFIFO;
    options.write_buffer_size = 100 << 10;
    options.compaction_options_fifo.max_table_files_size = 500 << 10;
    options.compression = kNoCompression;
    options.create_if_missing = true;
    if (iter == 1) {
      options.disable_auto_compactions = true;
    }
    options = CurrentOptions(options);
    DestroyAndReopen(options);
    Random rnd(301);
    for (int i = 0; i < 6; ++i) {
      for (int j = 0; j < 100; ++j) {
        ASSERT_OK(Put(ToString(i * 100 + j), RandomString(&rnd, 1024)));
      }
      ASSERT_OK(dbfull()->TEST_WaitForFlushMemTable());
    }
    if (iter == 0) {
      ASSERT_OK(dbfull()->TEST_WaitForCompact());
    } else {
      ASSERT_OK(db_->CompactRange(CompactRangeOptions(), nullptr, nullptr));
    }
    ASSERT_EQ(NumTableFilesAtLevel(0), 5);
    for (int i = 0; i < 50; ++i) {
      ASSERT_EQ("NOT_FOUND", Get(ToString(i)));
    }
  }
}
TEST_F(DBTest, SimpleWriteTimeoutTest) {
  env_->SetBackgroundThreads(1, Env::LOW);
  SleepingBackgroundTask sleeping_task_low;
  env_->Schedule(&SleepingBackgroundTask::DoSleepTask, &sleeping_task_low,
                 Env::Priority::LOW);
  Options options;
  options.env = env_;
  options.create_if_missing = true;
  options.write_buffer_size = 100000;
  options.max_background_flushes = 0;
  options.max_write_buffer_number = 2;
  options.max_total_wal_size = std::numeric_limits<uint64_t>::max();
  WriteOptions write_opt;
  write_opt.timeout_hint_us = 0;
  DestroyAndReopen(options);
  ASSERT_OK(Put(Key(1), Key(1) + std::string(100000, 'v'), write_opt));
  ASSERT_OK(Put(Key(2), Key(2) + std::string(100000, 'v'), write_opt));
  write_opt.timeout_hint_us = 50;
  ASSERT_TRUE(
      Put(Key(3), Key(3) + std::string(100000, 'v'), write_opt).IsTimedOut());
  sleeping_task_low.WakeUp();
  sleeping_task_low.WaitUntilDone();
}
namespace {
static const int kValueSize = 1000;
static const int kWriteBufferSize = 100000;
struct TimeoutWriterState {
  int id;
  DB* db;
  std::atomic<bool> done;
  std::map<int, std::string> success_kvs;
};
static void RandomTimeoutWriter(void* arg) {
  TimeoutWriterState* state = reinterpret_cast<TimeoutWriterState*>(arg);
  static const uint64_t kTimerBias = 50;
  int thread_id = state->id;
  DB* db = state->db;
  Random rnd(1000 + thread_id);
  WriteOptions write_opt;
  write_opt.timeout_hint_us = 500;
  int timeout_count = 0;
  int num_keys = kNumKeys * 5;
  for (int k = 0; k < num_keys; ++k) {
    int key = k + thread_id * num_keys;
    std::string value = RandomString(&rnd, kValueSize);
    if (k > num_keys / 2) {
      switch (rnd.Next() % 5) {
        case 0:
          write_opt.timeout_hint_us = 500 * thread_id;
          break;
        case 1:
          write_opt.timeout_hint_us = num_keys - k;
          break;
        case 2:
          write_opt.timeout_hint_us = 1;
          break;
        default:
          write_opt.timeout_hint_us = 0;
          state->success_kvs.insert({key, value});
      }
    }
    uint64_t time_before_put = db->GetEnv()->NowMicros();
    Status s = db->Put(write_opt, Key(key), value);
    uint64_t put_duration = db->GetEnv()->NowMicros() - time_before_put;
    if (write_opt.timeout_hint_us == 0 ||
        put_duration + kTimerBias < write_opt.timeout_hint_us) {
      ASSERT_OK(s);
    }
    if (s.IsTimedOut()) {
      timeout_count++;
      ASSERT_GT(put_duration + kTimerBias, write_opt.timeout_hint_us);
    }
  }
  state->done = true;
}
TEST_F(DBTest, MTRandomTimeoutTest) {
  Options options;
  options.env = env_;
  options.create_if_missing = true;
  options.max_write_buffer_number = 2;
  options.compression = kNoCompression;
  options.level0_slowdown_writes_trigger = 10;
  options.level0_stop_writes_trigger = 20;
  options.write_buffer_size = kWriteBufferSize;
  DestroyAndReopen(options);
  TimeoutWriterState thread_states[kNumThreads];
  for (int tid = 0; tid < kNumThreads; ++tid) {
    thread_states[tid].id = tid;
    thread_states[tid].db = db_;
    thread_states[tid].done = false;
    env_->StartThread(RandomTimeoutWriter, &thread_states[tid]);
  }
  for (int tid = 0; tid < kNumThreads; ++tid) {
    while (thread_states[tid].done == false) {
      env_->SleepForMicroseconds(100000);
    }
  }
  Flush();
  for (int tid = 0; tid < kNumThreads; ++tid) {
    auto& success_kvs = thread_states[tid].success_kvs;
    for (auto it = success_kvs.begin(); it != success_kvs.end(); ++it) {
      ASSERT_EQ(Get(Key(it->first)), it->second);
    }
  }
}
TEST_F(DBTest, Level0StopWritesTest) {
  Options options = CurrentOptions();
  options.level0_slowdown_writes_trigger = 2;
  options.level0_stop_writes_trigger = 4;
  options.disable_auto_compactions = true;
  options.max_mem_compaction_level = 0;
  Reopen(options);
  for (int i = 0; i < 4; ++i) {
    Put("a", "b");
    Flush();
  }
  WriteOptions woptions;
  woptions.timeout_hint_us = 30 * 1000;
  Status s = Put("a", "b", woptions);
  ASSERT_TRUE(s.IsTimedOut());
}
}
TEST_F(DBTest, RateLimitingTest) {
  Options options = CurrentOptions();
  options.write_buffer_size = 1 << 20;
  options.level0_file_num_compaction_trigger = 2;
  options.target_file_size_base = 1 << 20;
  options.max_bytes_for_level_base = 4 << 20;
  options.max_bytes_for_level_multiplier = 4;
  options.compression = kNoCompression;
  options.create_if_missing = true;
  options.env = env_;
  options.IncreaseParallelism(4);
  DestroyAndReopen(options);
  WriteOptions wo;
  wo.disableWAL = true;
  Random rnd(301);
  uint64_t start = env_->NowMicros();
  for (int64_t i = 0; i < (96 << 10); ++i) {
    ASSERT_OK(Put(RandomString(&rnd, 32),
                  RandomString(&rnd, (1 << 10) + 1), wo));
  }
  uint64_t elapsed = env_->NowMicros() - start;
  double raw_rate = env_->bytes_written_ * 1000000 / elapsed;
  Close();
  options.rate_limiter.reset(
    NewGenericRateLimiter(static_cast<int64_t>(0.7 * raw_rate)));
  env_->bytes_written_ = 0;
  DestroyAndReopen(options);
  start = env_->NowMicros();
  for (int64_t i = 0; i < (96 << 10); ++i) {
    ASSERT_OK(Put(RandomString(&rnd, 32),
                  RandomString(&rnd, (1 << 10) + 1), wo));
  }
  elapsed = env_->NowMicros() - start;
  Close();
  ASSERT_TRUE(options.rate_limiter->GetTotalBytesThrough() ==
              env_->bytes_written_);
  double ratio = env_->bytes_written_ * 1000000 / elapsed / raw_rate;
  fprintf(stderr, "write rate ratio = %.2lf, expected 0.7\n", ratio);
  ASSERT_TRUE(ratio < 0.8);
  options.rate_limiter.reset(
    NewGenericRateLimiter(static_cast<int64_t>(raw_rate / 2)));
  env_->bytes_written_ = 0;
  DestroyAndReopen(options);
  start = env_->NowMicros();
  for (int64_t i = 0; i < (96 << 10); ++i) {
    ASSERT_OK(Put(RandomString(&rnd, 32),
                  RandomString(&rnd, (1 << 10) + 1), wo));
  }
  elapsed = env_->NowMicros() - start;
  Close();
  ASSERT_TRUE(options.rate_limiter->GetTotalBytesThrough() ==
              env_->bytes_written_);
  ratio = env_->bytes_written_ * 1000000 / elapsed / raw_rate;
  fprintf(stderr, "write rate ratio = %.2lf, expected 0.5\n", ratio);
  ASSERT_TRUE(ratio < 0.6);
}
namespace {
  bool HaveOverlappingKeyRanges(
      const Comparator* c,
      const SstFileMetaData& a, const SstFileMetaData& b) {
    if (c->Compare(a.smallestkey, b.smallestkey) >= 0) {
      if (c->Compare(a.smallestkey, b.largestkey) <= 0) {
        return true;
      }
    } else if (c->Compare(a.largestkey, b.smallestkey) >= 0) {
      return true;
    }
    if (c->Compare(a.largestkey, b.largestkey) <= 0) {
      if (c->Compare(a.largestkey, b.smallestkey) >= 0) {
        return true;
      }
    } else if (c->Compare(a.smallestkey, b.largestkey) <= 0) {
      return true;
    }
    return false;
  }
  void GetOverlappingFileNumbersForLevelCompaction(
      const ColumnFamilyMetaData& cf_meta,
      const Comparator* comparator,
      int min_level, int max_level,
      const SstFileMetaData* input_file_meta,
      std::set<std::string>* overlapping_file_names) {
    std::set<const SstFileMetaData*> overlapping_files;
    overlapping_files.insert(input_file_meta);
    for (int m = min_level; m <= max_level; ++m) {
      for (auto& file : cf_meta.levels[m].files) {
        for (auto* included_file : overlapping_files) {
          if (HaveOverlappingKeyRanges(
                  comparator, *included_file, file)) {
            overlapping_files.insert(&file);
            overlapping_file_names->insert(file.name);
            break;
          }
        }
      }
    }
  }
  void VerifyCompactionResult(
      const ColumnFamilyMetaData& cf_meta,
      const std::set<std::string>& overlapping_file_numbers) {
#ifndef NDEBUG
    for (auto& level : cf_meta.levels) {
      for (auto& file : level.files) {
        assert(overlapping_file_numbers.find(file.name) ==
               overlapping_file_numbers.end());
      }
    }
#endif
  }
  const SstFileMetaData* PickFileRandomly(
      const ColumnFamilyMetaData& cf_meta,
      Random* rand,
      int* level = nullptr) {
    auto file_id = rand->Uniform(static_cast<int>(
        cf_meta.file_count)) + 1;
    for (auto& level_meta : cf_meta.levels) {
      if (file_id <= level_meta.files.size()) {
        if (level != nullptr) {
          *level = level_meta.level;
        }
        auto result = rand->Uniform(file_id);
        return &(level_meta.files[result]);
      }
      file_id -= level_meta.files.size();
    }
    assert(false);
    return nullptr;
  }
}
TEST_F(DBTest, DISABLED_CompactFilesOnLevelCompaction) {
  const int kTestKeySize = 16;
  const int kTestValueSize = 984;
  const int kEntrySize = kTestKeySize + kTestValueSize;
  const int kEntriesPerBuffer = 100;
  Options options;
  options.create_if_missing = true;
  options.write_buffer_size = kEntrySize * kEntriesPerBuffer;
  options.compaction_style = kCompactionStyleLevel;
  options.target_file_size_base = options.write_buffer_size;
  options.max_bytes_for_level_base = options.target_file_size_base * 2;
  options.level0_stop_writes_trigger = 2;
  options.max_bytes_for_level_multiplier = 2;
  options.compression = kNoCompression;
  options = CurrentOptions(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  Random rnd(301);
  for (int key = 64 * kEntriesPerBuffer; key >= 0; --key) {
    ASSERT_OK(Put(1, ToString(key), RandomString(&rnd, kTestValueSize)));
  }
  dbfull()->TEST_WaitForFlushMemTable(handles_[1]);
  dbfull()->TEST_WaitForCompact();
  ColumnFamilyMetaData cf_meta;
  dbfull()->GetColumnFamilyMetaData(handles_[1], &cf_meta);
  int output_level = static_cast<int>(cf_meta.levels.size()) - 1;
  for (int file_picked = 5; file_picked > 0; --file_picked) {
    std::set<std::string> overlapping_file_names;
    std::vector<std::string> compaction_input_file_names;
    for (int f = 0; f < file_picked; ++f) {
      int level;
      auto file_meta = PickFileRandomly(cf_meta, &rnd, &level);
      compaction_input_file_names.push_back(file_meta->name);
      GetOverlappingFileNumbersForLevelCompaction(
          cf_meta, options.comparator, level, output_level,
          file_meta, &overlapping_file_names);
    }
    ASSERT_OK(dbfull()->CompactFiles(
        CompactionOptions(), handles_[1],
        compaction_input_file_names,
        output_level));
    dbfull()->GetColumnFamilyMetaData(handles_[1], &cf_meta);
    VerifyCompactionResult(cf_meta, overlapping_file_names);
  }
  for (int key = 64 * kEntriesPerBuffer; key >= 0; --key) {
    ASSERT_NE(Get(1, ToString(key)), "NOT_FOUND");
  }
}
TEST_F(DBTest, CompactFilesOnUniversalCompaction) {
  const int kTestKeySize = 16;
  const int kTestValueSize = 984;
  const int kEntrySize = kTestKeySize + kTestValueSize;
  const int kEntriesPerBuffer = 10;
  ChangeCompactOptions();
  Options options;
  options.create_if_missing = true;
  options.write_buffer_size = kEntrySize * kEntriesPerBuffer;
  options.compaction_style = kCompactionStyleLevel;
  options.num_levels = 1;
  options.target_file_size_base = options.write_buffer_size;
  options.compression = kNoCompression;
  options = CurrentOptions(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  ASSERT_EQ(options.compaction_style, kCompactionStyleUniversal);
  Random rnd(301);
  for (int key = 1024 * kEntriesPerBuffer; key >= 0; --key) {
    ASSERT_OK(Put(1, ToString(key), RandomString(&rnd, kTestValueSize)));
  }
  dbfull()->TEST_WaitForFlushMemTable(handles_[1]);
  dbfull()->TEST_WaitForCompact();
  ColumnFamilyMetaData cf_meta;
  dbfull()->GetColumnFamilyMetaData(handles_[1], &cf_meta);
  std::vector<std::string> compaction_input_file_names;
  for (auto file : cf_meta.levels[0].files) {
    if (rnd.OneIn(2)) {
      compaction_input_file_names.push_back(file.name);
    }
  }
  if (compaction_input_file_names.size() == 0) {
    compaction_input_file_names.push_back(
        cf_meta.levels[0].files[0].name);
  }
  ASSERT_TRUE(!dbfull()->CompactFiles(
      CompactionOptions(), handles_[1],
      compaction_input_file_names, 1).ok());
  ASSERT_OK(dbfull()->CompactFiles(
      CompactionOptions(), handles_[1],
      compaction_input_file_names, 0));
  dbfull()->GetColumnFamilyMetaData(handles_[1], &cf_meta);
  VerifyCompactionResult(
      cf_meta,
      std::set<std::string>(compaction_input_file_names.begin(),
          compaction_input_file_names.end()));
  compaction_input_file_names.clear();
  compaction_input_file_names.push_back(
      cf_meta.levels[0].files[0].name);
  compaction_input_file_names.push_back(
      cf_meta.levels[0].files[
          cf_meta.levels[0].files.size() - 1].name);
  ASSERT_OK(dbfull()->CompactFiles(
      CompactionOptions(), handles_[1],
      compaction_input_file_names, 0));
  dbfull()->GetColumnFamilyMetaData(handles_[1], &cf_meta);
  ASSERT_EQ(cf_meta.levels[0].files.size(), 1U);
}
TEST_F(DBTest, TableOptionsSanitizeTest) {
  Options options = CurrentOptions();
  options.create_if_missing = true;
  DestroyAndReopen(options);
  ASSERT_EQ(db_->GetOptions().allow_mmap_reads, false);
  options.table_factory.reset(new PlainTableFactory());
  options.prefix_extractor.reset(NewNoopTransform());
  Destroy(options);
  ASSERT_TRUE(TryReopen(options).IsNotSupported());
  BlockBasedTableOptions to;
  to.index_type = BlockBasedTableOptions::kHashSearch;
  options = CurrentOptions();
  options.create_if_missing = true;
  options.table_factory.reset(NewBlockBasedTableFactory(to));
  ASSERT_TRUE(TryReopen(options).IsInvalidArgument());
  options.prefix_extractor.reset(NewFixedPrefixTransform(1));
  ASSERT_OK(TryReopen(options));
}
TEST_F(DBTest, SanitizeNumThreads) {
  for (int attempt = 0; attempt < 2; attempt++) {
    const size_t kTotalTasks = 8;
    SleepingBackgroundTask sleeping_tasks[kTotalTasks];
    Options options = CurrentOptions();
    if (attempt == 0) {
      options.max_background_compactions = 3;
      options.max_background_flushes = 2;
    }
    options.create_if_missing = true;
    DestroyAndReopen(options);
    for (size_t i = 0; i < kTotalTasks; i++) {
      env_->Schedule(&SleepingBackgroundTask::DoSleepTask, &sleeping_tasks[i],
                     (i < 4) ? Env::Priority::LOW : Env::Priority::HIGH);
    }
    env_->SleepForMicroseconds(100000);
    ASSERT_EQ(1U, options.env->GetThreadPoolQueueLen(Env::Priority::LOW));
    ASSERT_EQ(2U, options.env->GetThreadPoolQueueLen(Env::Priority::HIGH));
    for (size_t i = 0; i < kTotalTasks; i++) {
      sleeping_tasks[i].WakeUp();
      sleeping_tasks[i].WaitUntilDone();
    }
    ASSERT_OK(Put("abc", "def"));
    ASSERT_EQ("def", Get("abc"));
    Flush();
    ASSERT_EQ("def", Get("abc"));
  }
}
TEST_F(DBTest, DBIteratorBoundTest) {
  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.prefix_extractor = nullptr;
  DestroyAndReopen(options);
  ASSERT_OK(Put("a", "0"));
  ASSERT_OK(Put("foo", "bar"));
  ASSERT_OK(Put("foo1", "bar1"));
  ASSERT_OK(Put("g1", "0"));
  {
    ReadOptions ro;
    ro.iterate_upper_bound = nullptr;
    std::unique_ptr<Iterator> iter(db_->NewIterator(ro));
    iter->Seek("foo");
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().compare(Slice("foo")), 0);
    iter->Next();
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().compare(Slice("foo1")), 0);
    iter->Next();
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().compare(Slice("g1")), 0);
  }
  {
    ReadOptions ro;
    Slice prefix("foo2");
    ro.iterate_upper_bound = &prefix;
    std::unique_ptr<Iterator> iter(db_->NewIterator(ro));
    iter->Seek("foo");
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().compare(Slice("foo")), 0);
    iter->Next();
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().compare(("foo1")), 0);
    iter->Next();
    ASSERT_TRUE(!iter->Valid());
  }
  {
    ReadOptions ro;
    Slice prefix("foo");
    ro.iterate_upper_bound = &prefix;
    std::unique_ptr<Iterator> iter(db_->NewIterator(ro));
    iter->SeekToLast();
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().compare(Slice("a")), 0);
  }
  options.prefix_extractor.reset(NewFixedPrefixTransform(1));
  DestroyAndReopen(options);
  ASSERT_OK(Put("a", "0"));
  ASSERT_OK(Put("foo", "bar"));
  ASSERT_OK(Put("foo1", "bar1"));
  ASSERT_OK(Put("g1", "0"));
  {
    ReadOptions ro;
    Slice prefix("g1");
    ro.iterate_upper_bound = &prefix;
    std::unique_ptr<Iterator> iter(db_->NewIterator(ro));
    iter->Seek("foo");
    ASSERT_TRUE(!iter->Valid());
    ASSERT_TRUE(iter->status().IsInvalidArgument());
  }
  {
    options.prefix_extractor = nullptr;
    DestroyAndReopen(options);
    ASSERT_OK(Put("a", "0"));
    ASSERT_OK(Put("b", "0"));
    ASSERT_OK(Put("b1", "0"));
    ASSERT_OK(Put("c", "0"));
    ASSERT_OK(Put("d", "0"));
    ASSERT_OK(Put("e", "0"));
    ASSERT_OK(Delete("c"));
    ASSERT_OK(Delete("d"));
    ReadOptions ro;
    ro.iterate_upper_bound = nullptr;
    std::unique_ptr<Iterator> iter(db_->NewIterator(ro));
    iter->Seek("b");
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().compare(Slice("b")), 0);
    iter->Next();
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().compare(("b1")), 0);
    perf_context.Reset();
    iter->Next();
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(static_cast<int>(perf_context.internal_delete_skipped_count), 2);
    Slice prefix("c");
    ro.iterate_upper_bound = &prefix;
    iter.reset(db_->NewIterator(ro));
    perf_context.Reset();
    iter->Seek("b");
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().compare(Slice("b")), 0);
    iter->Next();
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().compare(("b1")), 0);
    iter->Next();
    ASSERT_TRUE(!iter->Valid());
    ASSERT_EQ(static_cast<int>(perf_context.internal_delete_skipped_count), 0);
  }
}
TEST_F(DBTest, WriteSingleThreadEntry) {
  std::vector<std::thread> threads;
  dbfull()->TEST_LockMutex();
  auto w = dbfull()->TEST_BeginWrite();
  threads.emplace_back([&] { Put("a", "b"); });
  env_->SleepForMicroseconds(10000);
  threads.emplace_back([&] { Flush(); });
  env_->SleepForMicroseconds(10000);
  dbfull()->TEST_UnlockMutex();
  dbfull()->TEST_LockMutex();
  dbfull()->TEST_EndWrite(w);
  dbfull()->TEST_UnlockMutex();
  for (auto& t : threads) {
    t.join();
  }
}
TEST_F(DBTest, DisableDataSyncTest) {
  env_->sync_counter_.store(0);
  for (int iter = 0; iter < 2; ++iter) {
    Options options = CurrentOptions();
    options.disableDataSync = iter == 0;
    options.create_if_missing = true;
    options.env = env_;
    Reopen(options);
    CreateAndReopenWithCF({"pikachu"}, options);
    MakeTables(10, "a", "z");
    Compact("a", "z");
    if (iter == 0) {
      ASSERT_EQ(env_->sync_counter_.load(), 0);
    } else {
      ASSERT_GT(env_->sync_counter_.load(), 0);
    }
    Destroy(options);
  }
}
TEST_F(DBTest, DynamicMemtableOptions) {
  const uint64_t k64KB = 1 << 16;
  const uint64_t k128KB = 1 << 17;
  const uint64_t k5KB = 5 * 1024;
  Options options;
  options.env = env_;
  options.create_if_missing = true;
  options.compression = kNoCompression;
  options.max_background_compactions = 1;
  options.max_mem_compaction_level = 0;
  options.write_buffer_size = k64KB;
  options.max_write_buffer_number = 2;
  options.level0_file_num_compaction_trigger = 1024;
  options.level0_slowdown_writes_trigger = 1024;
  options.level0_stop_writes_trigger = 1024;
  DestroyAndReopen(options);
  auto gen_l0_kb = [this](int size) {
    Random rnd(301);
    for (int i = 0; i < size; i++) {
      ASSERT_OK(Put(Key(i), RandomString(&rnd, 1024)));
    }
    dbfull()->TEST_WaitForFlushMemTable();
  };
  gen_l0_kb(64);
  ASSERT_EQ(NumTableFilesAtLevel(0), 1);
  ASSERT_LT(SizeAtLevel(0), k64KB + k5KB);
  ASSERT_GT(SizeAtLevel(0), k64KB - k5KB);
  dbfull()->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  ASSERT_EQ(NumTableFilesAtLevel(0), 0);
  ASSERT_OK(dbfull()->SetOptions({
    {"write_buffer_size", "131072"},
  }));
  gen_l0_kb(256);
  ASSERT_EQ(NumTableFilesAtLevel(0), 2);
  ASSERT_LT(SizeAtLevel(0), k128KB + k64KB + 2 * k5KB);
  ASSERT_GT(SizeAtLevel(0), k128KB + k64KB - 2 * k5KB);
  env_->SetBackgroundThreads(1, Env::LOW);
  SleepingBackgroundTask sleeping_task_low1;
  env_->Schedule(&SleepingBackgroundTask::DoSleepTask, &sleeping_task_low1,
                 Env::Priority::LOW);
  options.max_background_flushes = 0;
  options.disable_auto_compactions = true;
  DestroyAndReopen(options);
  int count = 0;
  Random rnd(301);
  WriteOptions wo;
  wo.timeout_hint_us = 100000;
  std::atomic<int> sleep_count(0);
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::DelayWrite:TimedWait",
      [&](void* arg) { sleep_count.fetch_add(1); });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  while (Put(Key(count), RandomString(&rnd, 1024), wo).ok() && count < 256) {
    count++;
  }
  ASSERT_GT(sleep_count.load(), 0);
  ASSERT_GT(static_cast<double>(count), 128 * 0.8);
  ASSERT_LT(static_cast<double>(count), 128 * 1.2);
  sleeping_task_low1.WakeUp();
  sleeping_task_low1.WaitUntilDone();
  ASSERT_OK(dbfull()->SetOptions({
    {"max_write_buffer_number", "8"},
  }));
  dbfull()->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  SleepingBackgroundTask sleeping_task_low2;
  env_->Schedule(&SleepingBackgroundTask::DoSleepTask, &sleeping_task_low2,
                 Env::Priority::LOW);
  count = 0;
  sleep_count.store(0);
  while (Put(Key(count), RandomString(&rnd, 1024), wo).ok() && count < 1024) {
    count++;
  }
  ASSERT_GT(sleep_count.load(), 0);
#ifndef OS_WIN
  ASSERT_GT(static_cast<double>(count), 512 * 0.8);
  ASSERT_LT(static_cast<double>(count), 512 * 1.2);
#endif
  sleeping_task_low2.WakeUp();
  sleeping_task_low2.WaitUntilDone();
  ASSERT_OK(dbfull()->SetOptions({
    {"max_write_buffer_number", "4"},
  }));
  dbfull()->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  SleepingBackgroundTask sleeping_task_low3;
  env_->Schedule(&SleepingBackgroundTask::DoSleepTask, &sleeping_task_low3,
                 Env::Priority::LOW);
  count = 0;
  sleep_count.store(0);
  while (Put(Key(count), RandomString(&rnd, 1024), wo).ok() && count < 1024) {
    count++;
  }
  ASSERT_GT(sleep_count.load(), 0);
#ifndef OS_WIN
  ASSERT_GT(static_cast<double>(count), 256 * 0.8);
  ASSERT_LT(static_cast<double>(count), 266 * 1.2);
#endif
  sleeping_task_low3.WakeUp();
  sleeping_task_low3.WaitUntilDone();
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}
#if ROCKSDB_USING_THREAD_STATUS
namespace {
void VerifyOperationCount(Env* env, ThreadStatus::OperationType op_type,
                          int expected_count) {
  int op_count = 0;
  std::vector<ThreadStatus> thread_list;
  ASSERT_OK(env->GetThreadList(&thread_list));
  for (auto thread : thread_list) {
    if (thread.operation_type == op_type) {
      op_count++;
    }
  }
  ASSERT_EQ(op_count, expected_count);
}
}
TEST_F(DBTest, GetThreadStatus) {
  Options options;
  options.env = env_;
  options.enable_thread_tracking = true;
  TryReopen(options);
  std::vector<ThreadStatus> thread_list;
  Status s = env_->GetThreadList(&thread_list);
  for (int i = 0; i < 2; ++i) {
    const int kTestCount = 3;
    const unsigned int kHighPriCounts[kTestCount] = {3, 2, 5};
    const unsigned int kLowPriCounts[kTestCount] = {10, 15, 3};
    for (int test = 0; test < kTestCount; ++test) {
      env_->SetBackgroundThreads(kHighPriCounts[test], Env::HIGH);
      env_->SetBackgroundThreads(kLowPriCounts[test], Env::LOW);
      env_->SleepForMicroseconds(100000);
      s = env_->GetThreadList(&thread_list);
      ASSERT_OK(s);
      unsigned int thread_type_counts[ThreadStatus::NUM_THREAD_TYPES];
      memset(thread_type_counts, 0, sizeof(thread_type_counts));
      for (auto thread : thread_list) {
        ASSERT_LT(thread.thread_type, ThreadStatus::NUM_THREAD_TYPES);
        thread_type_counts[thread.thread_type]++;
      }
      ASSERT_EQ(
          thread_type_counts[ThreadStatus::HIGH_PRIORITY] +
              thread_type_counts[ThreadStatus::LOW_PRIORITY],
          kHighPriCounts[test] + kLowPriCounts[test]);
      ASSERT_EQ(
          thread_type_counts[ThreadStatus::HIGH_PRIORITY],
          kHighPriCounts[test]);
      ASSERT_EQ(
          thread_type_counts[ThreadStatus::LOW_PRIORITY],
          kLowPriCounts[test]);
    }
    if (i == 0) {
      CreateAndReopenWithCF({"pikachu", "about-to-remove"}, options);
      env_->GetThreadStatusUpdater()->TEST_VerifyColumnFamilyInfoMap(
          handles_, true);
    }
  }
  db_->DropColumnFamily(handles_[2]);
  delete handles_[2];
  handles_.erase(handles_.begin() + 2);
  env_->GetThreadStatusUpdater()->TEST_VerifyColumnFamilyInfoMap(
      handles_, true);
  Close();
  env_->GetThreadStatusUpdater()->TEST_VerifyColumnFamilyInfoMap(
      handles_, true);
}
TEST_F(DBTest, DisableThreadStatus) {
  Options options;
  options.env = env_;
  options.enable_thread_tracking = false;
  TryReopen(options);
  CreateAndReopenWithCF({"pikachu", "about-to-remove"}, options);
  env_->GetThreadStatusUpdater()->TEST_VerifyColumnFamilyInfoMap(
      handles_, false);
}
TEST_F(DBTest, ThreadStatusFlush) {
  Options options;
  options.env = env_;
  options.write_buffer_size = 100000;
  options.enable_thread_tracking = true;
  options = CurrentOptions(options);
  rocksdb::SyncPoint::GetInstance()->LoadDependency({
      {"FlushJob::FlushJob()", "DBTest::ThreadStatusFlush:1"},
      {"DBTest::ThreadStatusFlush:2", "FlushJob::~FlushJob()"},
  });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  CreateAndReopenWithCF({"pikachu"}, options);
  VerifyOperationCount(env_, ThreadStatus::OP_FLUSH, 0);
  ASSERT_OK(Put(1, "foo", "v1"));
  ASSERT_EQ("v1", Get(1, "foo"));
  VerifyOperationCount(env_, ThreadStatus::OP_FLUSH, 0);
  Put(1, "k1", std::string(100000, 'x'));
  VerifyOperationCount(env_, ThreadStatus::OP_FLUSH, 0);
  Put(1, "k2", std::string(100000, 'y'));
  env_->SleepForMicroseconds(250000);
  TEST_SYNC_POINT("DBTest::ThreadStatusFlush:1");
  VerifyOperationCount(env_, ThreadStatus::OP_FLUSH, 1);
  TEST_SYNC_POINT("DBTest::ThreadStatusFlush:2");
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}
TEST_F(DBTest, ThreadStatusSingleCompaction) {
  const int kTestKeySize = 16;
  const int kTestValueSize = 984;
  const int kEntrySize = kTestKeySize + kTestValueSize;
  const int kEntriesPerBuffer = 100;
  Options options;
  options.create_if_missing = true;
  options.write_buffer_size = kEntrySize * kEntriesPerBuffer;
  options.compaction_style = kCompactionStyleLevel;
  options.target_file_size_base = options.write_buffer_size;
  options.max_bytes_for_level_base = options.target_file_size_base * 2;
  options.max_bytes_for_level_multiplier = 2;
  options.compression = kNoCompression;
  options = CurrentOptions(options);
  options.env = env_;
  options.enable_thread_tracking = true;
  const int kNumL0Files = 4;
  options.level0_file_num_compaction_trigger = kNumL0Files;
  rocksdb::SyncPoint::GetInstance()->LoadDependency({
      {"DBTest::ThreadStatusSingleCompaction:0", "DBImpl::BGWorkCompaction"},
      {"CompactionJob::Run():Start", "DBTest::ThreadStatusSingleCompaction:1"},
      {"DBTest::ThreadStatusSingleCompaction:2", "CompactionJob::Run():End"},
  });
  for (int tests = 0; tests < 2; ++tests) {
    DestroyAndReopen(options);
    rocksdb::SyncPoint::GetInstance()->ClearTrace();
    rocksdb::SyncPoint::GetInstance()->EnableProcessing();
    Random rnd(301);
    for (int file = 0; file < kNumL0Files; ++file) {
      for (int key = 0; key < kEntriesPerBuffer; ++key) {
        ASSERT_OK(Put(ToString(key + file * kEntriesPerBuffer),
                      RandomString(&rnd, kTestValueSize)));
      }
      Flush();
    }
    TEST_SYNC_POINT("DBTest::ThreadStatusSingleCompaction:0");
    ASSERT_GE(NumTableFilesAtLevel(0),
              options.level0_file_num_compaction_trigger);
    TEST_SYNC_POINT("DBTest::ThreadStatusSingleCompaction:1");
    if (options.enable_thread_tracking) {
      VerifyOperationCount(env_, ThreadStatus::OP_COMPACTION, 1);
    } else {
      VerifyOperationCount(env_, ThreadStatus::OP_COMPACTION, 0);
    }
    TEST_SYNC_POINT("DBTest::ThreadStatusSingleCompaction:2");
    options.enable_thread_tracking = false;
    rocksdb::SyncPoint::GetInstance()->DisableProcessing();
  }
}
TEST_F(DBTest, PreShutdownManualCompaction) {
  Options options = CurrentOptions();
  options.max_background_flushes = 0;
  CreateAndReopenWithCF({"pikachu"}, options);
  ASSERT_EQ(dbfull()->MaxMemCompactionLevel(), 2)
      << "Need to update this test to match kMaxMemCompactLevel";
  for (int iter = 0; iter < 2; ++iter) {
    MakeTables(3, "p", "q", 1);
    ASSERT_EQ("1,1,1", FilesPerLevel(1));
    Compact(1, "", "c");
    ASSERT_EQ("1,1,1", FilesPerLevel(1));
    Compact(1, "r", "z");
    ASSERT_EQ("1,1,1", FilesPerLevel(1));
    Compact(1, "p1", "p9");
    ASSERT_EQ("0,0,1", FilesPerLevel(1));
    MakeTables(3, "c", "e", 1);
    ASSERT_EQ("1,1,2", FilesPerLevel(1));
    Compact(1, "b", "f");
    ASSERT_EQ("0,0,2", FilesPerLevel(1));
    MakeTables(1, "a", "z", 1);
    ASSERT_EQ("0,1,2", FilesPerLevel(1));
    CancelAllBackgroundWork(db_);
    db_->CompactRange(CompactRangeOptions(), handles_[1], nullptr, nullptr);
    ASSERT_EQ("0,1,2", FilesPerLevel(1));
    if (iter == 0) {
      options = CurrentOptions();
      options.max_background_flushes = 0;
      options.num_levels = 3;
      options.create_if_missing = true;
      DestroyAndReopen(options);
      CreateAndReopenWithCF({"pikachu"}, options);
    }
  }
}
TEST_F(DBTest, PreShutdownFlush) {
  Options options = CurrentOptions();
  options.max_background_flushes = 0;
  CreateAndReopenWithCF({"pikachu"}, options);
  ASSERT_OK(Put(1, "key", "value"));
  CancelAllBackgroundWork(db_);
  Status s =
      db_->CompactRange(CompactRangeOptions(), handles_[1], nullptr, nullptr);
  ASSERT_TRUE(s.IsShutdownInProgress());
}
TEST_F(DBTest, PreShutdownMultipleCompaction) {
  const int kTestKeySize = 16;
  const int kTestValueSize = 984;
  const int kEntrySize = kTestKeySize + kTestValueSize;
  const int kEntriesPerBuffer = 40;
  const int kNumL0Files = 4;
  const int kHighPriCount = 3;
  const int kLowPriCount = 5;
  env_->SetBackgroundThreads(kHighPriCount, Env::HIGH);
  env_->SetBackgroundThreads(kLowPriCount, Env::LOW);
  Options options;
  options.create_if_missing = true;
  options.write_buffer_size = kEntrySize * kEntriesPerBuffer;
  options.compaction_style = kCompactionStyleLevel;
  options.target_file_size_base = options.write_buffer_size;
  options.max_bytes_for_level_base =
      options.target_file_size_base * kNumL0Files;
  options.compression = kNoCompression;
  options = CurrentOptions(options);
  options.env = env_;
  options.enable_thread_tracking = true;
  options.level0_file_num_compaction_trigger = kNumL0Files;
  options.max_bytes_for_level_multiplier = 2;
  options.max_background_compactions = kLowPriCount;
  options.level0_stop_writes_trigger = 1 << 10;
  options.level0_slowdown_writes_trigger = 1 << 10;
  TryReopen(options);
  Random rnd(301);
  std::vector<ThreadStatus> thread_list;
  rocksdb::SyncPoint::GetInstance()->LoadDependency(
      {{"FlushJob::FlushJob()", "CompactionJob::Run():Start"},
       {"CompactionJob::Run():Start",
        "DBTest::PreShutdownMultipleCompaction:Preshutdown"},
        {"CompactionJob::Run():Start",
        "DBTest::PreShutdownMultipleCompaction:VerifyCompaction"},
       {"DBTest::PreShutdownMultipleCompaction:Preshutdown",
        "CompactionJob::Run():End"},
       {"CompactionJob::Run():End",
        "DBTest::PreShutdownMultipleCompaction:VerifyPreshutdown"}});
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  int key = 0;
  int operation_count[ThreadStatus::NUM_OP_TYPES] = {0};
  for (int file = 0; file < 16 * kNumL0Files; ++file) {
    for (int k = 0; k < kEntriesPerBuffer; ++k) {
      ASSERT_OK(Put(ToString(key++), RandomString(&rnd, kTestValueSize)));
    }
    Status s = env_->GetThreadList(&thread_list);
    for (auto thread : thread_list) {
      operation_count[thread.operation_type]++;
    }
    if (operation_count[ThreadStatus::OP_FLUSH] > 1 &&
        operation_count[ThreadStatus::OP_COMPACTION] >
            0.6 * options.max_background_compactions) {
      break;
    }
    if (file == 15 * kNumL0Files) {
      TEST_SYNC_POINT("DBTest::PreShutdownMultipleCompaction:Preshutdown");
    }
  }
  TEST_SYNC_POINT("DBTest::PreShutdownMultipleCompaction:Preshutdown");
  ASSERT_GE(operation_count[ThreadStatus::OP_COMPACTION], 1);
  CancelAllBackgroundWork(db_);
  TEST_SYNC_POINT("DBTest::PreShutdownMultipleCompaction:VerifyPreshutdown");
  dbfull()->TEST_WaitForCompact();
  for (int i = 0; i < ThreadStatus::NUM_OP_TYPES; ++i) {
    operation_count[i] = 0;
  }
  Status s = env_->GetThreadList(&thread_list);
  for (auto thread : thread_list) {
    operation_count[thread.operation_type]++;
  }
  ASSERT_EQ(operation_count[ThreadStatus::OP_COMPACTION], 0);
}
TEST_F(DBTest, PreShutdownCompactionMiddle) {
  const int kTestKeySize = 16;
  const int kTestValueSize = 984;
  const int kEntrySize = kTestKeySize + kTestValueSize;
  const int kEntriesPerBuffer = 40;
  const int kNumL0Files = 4;
  const int kHighPriCount = 3;
  const int kLowPriCount = 5;
  env_->SetBackgroundThreads(kHighPriCount, Env::HIGH);
  env_->SetBackgroundThreads(kLowPriCount, Env::LOW);
  Options options;
  options.create_if_missing = true;
  options.write_buffer_size = kEntrySize * kEntriesPerBuffer;
  options.compaction_style = kCompactionStyleLevel;
  options.target_file_size_base = options.write_buffer_size;
  options.max_bytes_for_level_base =
      options.target_file_size_base * kNumL0Files;
  options.compression = kNoCompression;
  options = CurrentOptions(options);
  options.env = env_;
  options.enable_thread_tracking = true;
  options.level0_file_num_compaction_trigger = kNumL0Files;
  options.max_bytes_for_level_multiplier = 2;
  options.max_background_compactions = kLowPriCount;
  options.level0_stop_writes_trigger = 1 << 10;
  options.level0_slowdown_writes_trigger = 1 << 10;
  TryReopen(options);
  Random rnd(301);
  std::vector<ThreadStatus> thread_list;
  rocksdb::SyncPoint::GetInstance()->LoadDependency(
      {{"DBTest::PreShutdownCompactionMiddle:Preshutdown",
        "CompactionJob::Run():Inprogress"},
        {"CompactionJob::Run():Start",
        "DBTest::PreShutdownCompactionMiddle:VerifyCompaction"},
       {"CompactionJob::Run():Inprogress", "CompactionJob::Run():End"},
       {"CompactionJob::Run():End",
        "DBTest::PreShutdownCompactionMiddle:VerifyPreshutdown"}});
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  int key = 0;
  int operation_count[ThreadStatus::NUM_OP_TYPES] = {0};
  for (int file = 0; file < 16 * kNumL0Files; ++file) {
    for (int k = 0; k < kEntriesPerBuffer; ++k) {
      ASSERT_OK(Put(ToString(key++), RandomString(&rnd, kTestValueSize)));
    }
    Status s = env_->GetThreadList(&thread_list);
    for (auto thread : thread_list) {
      operation_count[thread.operation_type]++;
    }
    if (operation_count[ThreadStatus::OP_FLUSH] > 1 &&
        operation_count[ThreadStatus::OP_COMPACTION] >
            0.6 * options.max_background_compactions) {
      break;
    }
    if (file == 15 * kNumL0Files) {
      TEST_SYNC_POINT("DBTest::PreShutdownCompactionMiddle:VerifyCompaction");
    }
  }
  ASSERT_GE(operation_count[ThreadStatus::OP_COMPACTION], 1);
  CancelAllBackgroundWork(db_);
  TEST_SYNC_POINT("DBTest::PreShutdownCompactionMiddle:Preshutdown");
  TEST_SYNC_POINT("DBTest::PreShutdownCompactionMiddle:VerifyPreshutdown");
  dbfull()->TEST_WaitForCompact();
  for (int i = 0; i < ThreadStatus::NUM_OP_TYPES; ++i) {
    operation_count[i] = 0;
  }
  Status s = env_->GetThreadList(&thread_list);
  for (auto thread : thread_list) {
    operation_count[thread.operation_type]++;
  }
  ASSERT_EQ(operation_count[ThreadStatus::OP_COMPACTION], 0);
}
#endif
TEST_F(DBTest, FlushOnDestroy) {
  WriteOptions wo;
  wo.disableWAL = true;
  ASSERT_OK(Put("foo", "v1", wo));
  CancelAllBackgroundWork(db_);
}
TEST_F(DBTest, DynamicLevelMaxBytesBase) {
  if (!Snappy_Supported() || !LZ4_Supported()) {
    return;
  }
  unique_ptr<Env> env(new MockEnv(env_));
  const int kNKeys = 1000;
  int keys[kNKeys];
  auto verify_func = [&]() {
    for (int i = 0; i < kNKeys; i++) {
      ASSERT_NE("NOT_FOUND", Get(Key(i)));
      ASSERT_NE("NOT_FOUND", Get(Key(kNKeys * 2 + i)));
      if (i < kNKeys / 10) {
        ASSERT_EQ("NOT_FOUND", Get(Key(kNKeys + keys[i])));
      } else {
        ASSERT_NE("NOT_FOUND", Get(Key(kNKeys + keys[i])));
      }
    }
  };
  Random rnd(301);
  for (int ordered_insert = 0; ordered_insert <= 1; ordered_insert++) {
    for (int i = 0; i < kNKeys; i++) {
      keys[i] = i;
    }
    if (ordered_insert == 0) {
      std::random_shuffle(std::begin(keys), std::end(keys));
    }
    for (int max_background_compactions = 1; max_background_compactions < 4;
         max_background_compactions += 2) {
      Options options;
      options.env = env.get();
      options.create_if_missing = true;
      options.db_write_buffer_size = 2048;
      options.write_buffer_size = 2048;
      options.max_write_buffer_number = 2;
      options.level0_file_num_compaction_trigger = 2;
      options.level0_slowdown_writes_trigger = 2;
      options.level0_stop_writes_trigger = 2;
      options.target_file_size_base = 2048;
      options.level_compaction_dynamic_level_bytes = true;
      options.max_bytes_for_level_base = 10240;
      options.max_bytes_for_level_multiplier = 4;
      options.soft_rate_limit = 1.1;
      options.max_background_compactions = max_background_compactions;
      options.num_levels = 5;
      options.compression_per_level.resize(3);
      options.compression_per_level[0] = kNoCompression;
      options.compression_per_level[1] = kLZ4Compression;
      options.compression_per_level[2] = kSnappyCompression;
      DestroyAndReopen(options);
      for (int i = 0; i < kNKeys; i++) {
        int key = keys[i];
        ASSERT_OK(Put(Key(kNKeys + key), RandomString(&rnd, 102)));
        ASSERT_OK(Put(Key(key), RandomString(&rnd, 102)));
        ASSERT_OK(Put(Key(kNKeys * 2 + key), RandomString(&rnd, 102)));
        ASSERT_OK(Delete(Key(kNKeys + keys[i / 10])));
        env_->SleepForMicroseconds(5000);
      }
      uint64_t int_prop;
      ASSERT_TRUE(db_->GetIntProperty("rocksdb.background-errors", &int_prop));
      ASSERT_EQ(0U, int_prop);
      for (int j = 0; j < 2; j++) {
        verify_func();
        if (j == 0) {
          Reopen(options);
        }
      }
      dbfull()->CompactRange(CompactRangeOptions(), nullptr, nullptr);
      ColumnFamilyMetaData cf_meta;
      db_->GetColumnFamilyMetaData(&cf_meta);
      ASSERT_EQ(5U, cf_meta.levels.size());
      for (int i = 0; i < 4; i++) {
        ASSERT_EQ(0U, cf_meta.levels[i].files.size());
      }
      ASSERT_GT(cf_meta.levels[4U].files.size(), 0U);
      verify_func();
      Close();
    }
  }
  env_->SetBackgroundThreads(1, Env::LOW);
  env_->SetBackgroundThreads(1, Env::HIGH);
}
TEST_F(DBTest, DynamicLevelMaxBytesBase2) {
  Random rnd(301);
  int kMaxKey = 1000000;
  Options options = CurrentOptions();
  options.create_if_missing = true;
  options.db_write_buffer_size = 2048;
  options.write_buffer_size = 2048;
  options.max_write_buffer_number = 2;
  options.level0_file_num_compaction_trigger = 2;
  options.level0_slowdown_writes_trigger = 9999;
  options.level0_stop_writes_trigger = 9999;
  options.target_file_size_base = 2048;
  options.level_compaction_dynamic_level_bytes = true;
  options.max_bytes_for_level_base = 10240;
  options.max_bytes_for_level_multiplier = 4;
  options.max_background_compactions = 2;
  options.num_levels = 5;
  options.expanded_compaction_factor = 0;
  BlockBasedTableOptions table_options;
  table_options.block_size = 1024;
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  DestroyAndReopen(options);
  ASSERT_OK(dbfull()->SetOptions({
      {"disable_auto_compactions", "true"},
  }));
  uint64_t int_prop;
  std::string str_prop;
  ASSERT_TRUE(db_->GetIntProperty("rocksdb.base-level", &int_prop));
  ASSERT_EQ(4U, int_prop);
  for (int i = 0; i < 70; i++) {
    ASSERT_OK(Put(Key(static_cast<int>(rnd.Uniform(kMaxKey))),
                  RandomString(&rnd, 80)));
  }
  ASSERT_OK(dbfull()->SetOptions({
      {"disable_auto_compactions", "false"},
  }));
  Flush();
  dbfull()->TEST_WaitForCompact();
  ASSERT_TRUE(db_->GetIntProperty("rocksdb.base-level", &int_prop));
  ASSERT_EQ(4U, int_prop);
  ASSERT_OK(dbfull()->SetOptions({
      {"disable_auto_compactions", "true"},
  }));
  for (int i = 0; i < 70; i++) {
    ASSERT_OK(Put(Key(static_cast<int>(rnd.Uniform(kMaxKey))),
                  RandomString(&rnd, 80)));
  }
  ASSERT_OK(dbfull()->SetOptions({
      {"disable_auto_compactions", "false"},
  }));
  Flush();
  dbfull()->TEST_WaitForCompact();
  ASSERT_TRUE(db_->GetIntProperty("rocksdb.base-level", &int_prop));
  ASSERT_EQ(3U, int_prop);
  ASSERT_TRUE(db_->GetProperty("rocksdb.num-files-at-level1", &str_prop));
  ASSERT_EQ("0", str_prop);
  ASSERT_TRUE(db_->GetProperty("rocksdb.num-files-at-level2", &str_prop));
  ASSERT_EQ("0", str_prop);
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "CompactionJob::Run():Start",
      [&](void* arg) { env_->SleepForMicroseconds(100000); });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  ASSERT_OK(dbfull()->SetOptions({
      {"disable_auto_compactions", "true"},
  }));
  for (int i = 0; i < 100; i++) {
    ASSERT_OK(Put(Key(static_cast<int>(rnd.Uniform(kMaxKey))),
                  RandomString(&rnd, 80)));
  }
  ASSERT_OK(dbfull()->SetOptions({
      {"disable_auto_compactions", "false"},
  }));
  Flush();
  env_->SleepForMicroseconds(200000);
  dbfull()->TEST_WaitForCompact();
  ASSERT_TRUE(db_->GetIntProperty("rocksdb.base-level", &int_prop));
  ASSERT_EQ(3U, int_prop);
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
  ASSERT_OK(dbfull()->SetOptions({
      {"disable_auto_compactions", "true"},
  }));
  for (int i = 0; i < 1350; i++) {
    ASSERT_OK(Put(Key(static_cast<int>(rnd.Uniform(kMaxKey))),
                  RandomString(&rnd, 80)));
  }
  ASSERT_OK(dbfull()->SetOptions({
      {"disable_auto_compactions", "false"},
  }));
  Flush();
  dbfull()->TEST_WaitForCompact();
  ASSERT_TRUE(db_->GetIntProperty("rocksdb.base-level", &int_prop));
  ASSERT_EQ(2U, int_prop);
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  for (int attempt = 0; attempt <= 20; attempt++) {
    for (int i = 0; i < 50; i++) {
      ASSERT_OK(Put(Key(static_cast<int>(rnd.Uniform(kMaxKey))),
                    RandomString(&rnd, 80)));
    }
    Flush();
    ASSERT_TRUE(db_->GetIntProperty("rocksdb.base-level", &int_prop));
    if (int_prop == 2U) {
      env_->SleepForMicroseconds(50000);
    } else {
      break;
    }
  }
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
  rocksdb::SyncPoint::GetInstance()->ClearAllCallBacks();
  env_->SleepForMicroseconds(200000);
  ASSERT_TRUE(db_->GetIntProperty("rocksdb.base-level", &int_prop));
  ASSERT_EQ(1U, int_prop);
}
TEST_F(DBTest, DynamicLevelMaxBytesCompactRange) {
  Random rnd(301);
  int kMaxKey = 1000000;
  Options options = CurrentOptions();
  options.create_if_missing = true;
  options.db_write_buffer_size = 2048;
  options.write_buffer_size = 2048;
  options.max_write_buffer_number = 2;
  options.level0_file_num_compaction_trigger = 2;
  options.level0_slowdown_writes_trigger = 9999;
  options.level0_stop_writes_trigger = 9999;
  options.target_file_size_base = 2;
  options.level_compaction_dynamic_level_bytes = true;
  options.max_bytes_for_level_base = 10240;
  options.max_bytes_for_level_multiplier = 4;
  options.max_background_compactions = 1;
  const int kNumLevels = 5;
  options.num_levels = kNumLevels;
  options.expanded_compaction_factor = 0;
  BlockBasedTableOptions table_options;
  table_options.block_size = 1024;
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  DestroyAndReopen(options);
  dbfull()->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  uint64_t int_prop;
  std::string str_prop;
  ASSERT_TRUE(db_->GetIntProperty("rocksdb.base-level", &int_prop));
  ASSERT_EQ(4U, int_prop);
  for (int i = 0; i < 140; i++) {
    ASSERT_OK(Put(Key(static_cast<int>(rnd.Uniform(kMaxKey))),
                  RandomString(&rnd, 80)));
  }
  Flush();
  dbfull()->TEST_WaitForCompact();
  if (NumTableFilesAtLevel(0) == 0) {
    ASSERT_OK(Put(Key(static_cast<int>(rnd.Uniform(kMaxKey))),
                  RandomString(&rnd, 80)));
    Flush();
  }
  ASSERT_TRUE(db_->GetIntProperty("rocksdb.base-level", &int_prop));
  ASSERT_EQ(3U, int_prop);
  ASSERT_TRUE(db_->GetProperty("rocksdb.num-files-at-level1", &str_prop));
  ASSERT_EQ("0", str_prop);
  ASSERT_TRUE(db_->GetProperty("rocksdb.num-files-at-level2", &str_prop));
  ASSERT_EQ("0", str_prop);
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
  rocksdb::SyncPoint::GetInstance()->ClearAllCallBacks();
  std::set<int> output_levels;
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "CompactionPicker::CompactRange:Return", [&](void* arg) {
        Compaction* compaction = reinterpret_cast<Compaction*>(arg);
        output_levels.insert(compaction->output_level());
      });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  dbfull()->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  ASSERT_EQ(output_levels.size(), 2);
  ASSERT_TRUE(output_levels.find(3) != output_levels.end());
  ASSERT_TRUE(output_levels.find(4) != output_levels.end());
  ASSERT_TRUE(db_->GetProperty("rocksdb.num-files-at-level0", &str_prop));
  ASSERT_EQ("0", str_prop);
  ASSERT_TRUE(db_->GetProperty("rocksdb.num-files-at-level3", &str_prop));
  ASSERT_EQ("0", str_prop);
  ASSERT_TRUE(db_->GetIntProperty("rocksdb.base-level", &int_prop));
  ASSERT_EQ(3U, int_prop);
}
TEST_F(DBTest, DynamicLevelMaxBytesBaseInc) {
  Options options = CurrentOptions();
  options.create_if_missing = true;
  options.db_write_buffer_size = 2048;
  options.write_buffer_size = 2048;
  options.max_write_buffer_number = 2;
  options.level0_file_num_compaction_trigger = 2;
  options.level0_slowdown_writes_trigger = 2;
  options.level0_stop_writes_trigger = 2;
  options.target_file_size_base = 2048;
  options.level_compaction_dynamic_level_bytes = true;
  options.max_bytes_for_level_base = 10240;
  options.max_bytes_for_level_multiplier = 4;
  options.soft_rate_limit = 1.1;
  options.max_background_compactions = 2;
  options.num_levels = 5;
  DestroyAndReopen(options);
  int non_trivial = 0;
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:NonTrivial",
      [&](void* arg) { non_trivial++; });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  Random rnd(301);
  const int total_keys = 3000;
  const int random_part_size = 100;
  for (int i = 0; i < total_keys; i++) {
    std::string value = RandomString(&rnd, random_part_size);
    PutFixed32(&value, static_cast<uint32_t>(i));
    ASSERT_OK(Put(Key(i), value));
  }
  Flush();
  dbfull()->TEST_WaitForCompact();
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
  ASSERT_EQ(non_trivial, 0);
  for (int i = 0; i < total_keys; i++) {
    std::string value = Get(Key(i));
    ASSERT_EQ(DecodeFixed32(value.c_str() + random_part_size),
              static_cast<uint32_t>(i));
  }
  env_->SetBackgroundThreads(1, Env::LOW);
  env_->SetBackgroundThreads(1, Env::HIGH);
}
TEST_F(DBTest, MigrateToDynamicLevelMaxBytesBase) {
  Random rnd(301);
  const int kMaxKey = 2000;
  Options options;
  options.create_if_missing = true;
  options.db_write_buffer_size = 2048;
  options.write_buffer_size = 2048;
  options.max_write_buffer_number = 8;
  options.level0_file_num_compaction_trigger = 4;
  options.level0_slowdown_writes_trigger = 4;
  options.level0_stop_writes_trigger = 8;
  options.target_file_size_base = 2048;
  options.level_compaction_dynamic_level_bytes = false;
  options.max_bytes_for_level_base = 10240;
  options.max_bytes_for_level_multiplier = 4;
  options.soft_rate_limit = 1.1;
  options.num_levels = 8;
  DestroyAndReopen(options);
  auto verify_func = [&](int num_keys, bool if_sleep) {
    for (int i = 0; i < num_keys; i++) {
      ASSERT_NE("NOT_FOUND", Get(Key(kMaxKey + i)));
      if (i < num_keys / 10) {
        ASSERT_EQ("NOT_FOUND", Get(Key(i)));
      } else {
        ASSERT_NE("NOT_FOUND", Get(Key(i)));
      }
      if (if_sleep && i % 1000 == 0) {
        env_->SleepForMicroseconds(1);
      }
    }
  };
  int total_keys = 1000;
  for (int i = 0; i < total_keys; i++) {
    ASSERT_OK(Put(Key(i), RandomString(&rnd, 102)));
    ASSERT_OK(Put(Key(kMaxKey + i), RandomString(&rnd, 102)));
    ASSERT_OK(Delete(Key(i / 10)));
  }
  verify_func(total_keys, false);
  dbfull()->TEST_WaitForCompact();
  options.level_compaction_dynamic_level_bytes = true;
  options.disable_auto_compactions = true;
  Reopen(options);
  verify_func(total_keys, false);
  std::atomic_bool compaction_finished;
  compaction_finished = false;
  std::thread t([&]() {
    CompactRangeOptions compact_options;
    compact_options.change_level = true;
    compact_options.target_level = options.num_levels - 1;
    dbfull()->CompactRange(compact_options, nullptr, nullptr);
    compaction_finished.store(true);
  });
  do {
    verify_func(total_keys, true);
  } while (!compaction_finished.load());
  t.join();
  ASSERT_OK(dbfull()->SetOptions({
      {"disable_auto_compactions", "false"},
  }));
  int total_keys2 = 2000;
  for (int i = total_keys; i < total_keys2; i++) {
    ASSERT_OK(Put(Key(i), RandomString(&rnd, 102)));
    ASSERT_OK(Put(Key(kMaxKey + i), RandomString(&rnd, 102)));
    ASSERT_OK(Delete(Key(i / 10)));
  }
  verify_func(total_keys2, false);
  dbfull()->TEST_WaitForCompact();
  verify_func(total_keys2, false);
  ASSERT_EQ(NumTableFilesAtLevel(1), 0);
  ASSERT_EQ(NumTableFilesAtLevel(2), 0);
}
namespace {
class OnFileDeletionListener : public EventListener {
 public:
  OnFileDeletionListener() :
      matched_count_(0),
      expected_file_name_("") {}
  void SetExpectedFileName(
      const std::string file_name) {
    expected_file_name_ = file_name;
  }
  void VerifyMatchedCount(size_t expected_value) {
    ASSERT_EQ(matched_count_, expected_value);
  }
  void OnTableFileDeleted(
      const TableFileDeletionInfo& info) override {
    if (expected_file_name_ != "") {
      ASSERT_EQ(expected_file_name_, info.file_path);
      expected_file_name_ = "";
      matched_count_++;
    }
  }
 private:
  size_t matched_count_;
  std::string expected_file_name_;
};
}
TEST_F(DBTest, DynamicLevelCompressionPerLevel) {
  if (!Snappy_Supported()) {
    return;
  }
  const int kNKeys = 120;
  int keys[kNKeys];
  for (int i = 0; i < kNKeys; i++) {
    keys[i] = i;
  }
  std::random_shuffle(std::begin(keys), std::end(keys));
  Random rnd(301);
  Options options;
  options.create_if_missing = true;
  options.db_write_buffer_size = 20480;
  options.write_buffer_size = 20480;
  options.max_write_buffer_number = 2;
  options.level0_file_num_compaction_trigger = 2;
  options.level0_slowdown_writes_trigger = 2;
  options.level0_stop_writes_trigger = 2;
  options.target_file_size_base = 2048;
  options.level_compaction_dynamic_level_bytes = true;
  options.max_bytes_for_level_base = 102400;
  options.max_bytes_for_level_multiplier = 4;
  options.max_background_compactions = 1;
  options.num_levels = 5;
  options.compression_per_level.resize(3);
  options.compression_per_level[0] = kNoCompression;
  options.compression_per_level[1] = kNoCompression;
  options.compression_per_level[2] = kSnappyCompression;
  OnFileDeletionListener* listener = new OnFileDeletionListener();
  options.listeners.emplace_back(listener);
  DestroyAndReopen(options);
  for (int i = 0; i < 20; i++) {
    ASSERT_OK(Put(Key(keys[i]), CompressibleString(&rnd, 4000)));
  }
  Flush();
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ(NumTableFilesAtLevel(1), 0);
  ASSERT_EQ(NumTableFilesAtLevel(2), 0);
  ASSERT_EQ(NumTableFilesAtLevel(3), 0);
  ASSERT_GT(SizeAtLevel(0) + SizeAtLevel(4), 20U * 4000U);
  for (int i = 21; i < 120; i++) {
    ASSERT_OK(Put(Key(keys[i]), CompressibleString(&rnd, 4000)));
  }
  Flush();
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ(NumTableFilesAtLevel(1), 0);
  ASSERT_EQ(NumTableFilesAtLevel(2), 0);
  ASSERT_LT(SizeAtLevel(0) + SizeAtLevel(3) + SizeAtLevel(4), 120U * 4000U);
  ASSERT_OK(dbfull()->SetOptions({
      {"disable_auto_compactions", "true"},
  }));
  ColumnFamilyMetaData cf_meta;
  db_->GetColumnFamilyMetaData(&cf_meta);
  for (auto file : cf_meta.levels[4].files) {
    listener->SetExpectedFileName(dbname_ + file.name);
    ASSERT_OK(dbfull()->DeleteFile(file.name));
  }
  listener->VerifyMatchedCount(cf_meta.levels[4].files.size());
  int num_keys = 0;
  std::unique_ptr<Iterator> iter(db_->NewIterator(ReadOptions()));
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    num_keys++;
  }
  ASSERT_OK(iter->status());
  ASSERT_GT(SizeAtLevel(0) + SizeAtLevel(3), num_keys * 4000U);
}
TEST_F(DBTest, DynamicLevelCompressionPerLevel2) {
  if (!Snappy_Supported() || !LZ4_Supported() || !Zlib_Supported()) {
    return;
  }
  const int kNKeys = 500;
  int keys[kNKeys];
  for (int i = 0; i < kNKeys; i++) {
    keys[i] = i;
  }
  std::random_shuffle(std::begin(keys), std::end(keys));
  Random rnd(301);
  Options options;
  options.create_if_missing = true;
  options.db_write_buffer_size = 6000;
  options.write_buffer_size = 6000;
  options.max_write_buffer_number = 2;
  options.level0_file_num_compaction_trigger = 2;
  options.level0_slowdown_writes_trigger = 2;
  options.level0_stop_writes_trigger = 2;
  options.soft_rate_limit = 1.1;
  options.target_file_size_base = 10;
  options.target_file_size_multiplier = 2;
  options.level_compaction_dynamic_level_bytes = true;
  options.max_bytes_for_level_base = 200;
  options.max_bytes_for_level_multiplier = 8;
  options.max_background_compactions = 1;
  options.num_levels = 5;
  std::shared_ptr<mock::MockTableFactory> mtf(new mock::MockTableFactory);
  options.table_factory = mtf;
  options.compression_per_level.resize(3);
  options.compression_per_level[0] = kNoCompression;
  options.compression_per_level[1] = kLZ4Compression;
  options.compression_per_level[2] = kZlibCompression;
  DestroyAndReopen(options);
  std::atomic<int> num_zlib(0);
  std::atomic<int> num_lz4(0);
  std::atomic<int> num_no(0);
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "LevelCompactionPicker::PickCompaction:Return", [&](void* arg) {
        Compaction* compaction = reinterpret_cast<Compaction*>(arg);
        if (compaction->output_level() == 4) {
          ASSERT_TRUE(compaction->output_compression() == kLZ4Compression);
          num_lz4.fetch_add(1);
        }
      });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "FlushJob::WriteLevel0Table:output_compression", [&](void* arg) {
        auto* compression = reinterpret_cast<CompressionType*>(arg);
        ASSERT_TRUE(*compression == kNoCompression);
        num_no.fetch_add(1);
      });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  for (int i = 0; i < 100; i++) {
    ASSERT_OK(Put(Key(keys[i]), RandomString(&rnd, 200)));
  }
  Flush();
  dbfull()->TEST_WaitForCompact();
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
  rocksdb::SyncPoint::GetInstance()->ClearAllCallBacks();
  ASSERT_EQ(NumTableFilesAtLevel(1), 0);
  ASSERT_EQ(NumTableFilesAtLevel(2), 0);
  ASSERT_EQ(NumTableFilesAtLevel(3), 0);
  ASSERT_GT(NumTableFilesAtLevel(4), 0);
  ASSERT_GT(num_no.load(), 2);
  ASSERT_GT(num_lz4.load(), 0);
  int prev_num_files_l4 = NumTableFilesAtLevel(4);
  num_lz4.store(0);
  num_no.store(0);
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "LevelCompactionPicker::PickCompaction:Return", [&](void* arg) {
        Compaction* compaction = reinterpret_cast<Compaction*>(arg);
        if (compaction->output_level() == 4 && compaction->start_level() == 3) {
          ASSERT_TRUE(compaction->output_compression() == kZlibCompression);
          num_zlib.fetch_add(1);
        } else {
          ASSERT_TRUE(compaction->output_compression() == kLZ4Compression);
          num_lz4.fetch_add(1);
        }
      });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "FlushJob::WriteLevel0Table:output_compression", [&](void* arg) {
        auto* compression = reinterpret_cast<CompressionType*>(arg);
        ASSERT_TRUE(*compression == kNoCompression);
        num_no.fetch_add(1);
      });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  for (int i = 101; i < 500; i++) {
    ASSERT_OK(Put(Key(keys[i]), RandomString(&rnd, 200)));
    if (i % 100 == 99) {
      Flush();
      dbfull()->TEST_WaitForCompact();
    }
  }
  rocksdb::SyncPoint::GetInstance()->ClearAllCallBacks();
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
  ASSERT_EQ(NumTableFilesAtLevel(1), 0);
  ASSERT_EQ(NumTableFilesAtLevel(2), 0);
  ASSERT_GT(NumTableFilesAtLevel(3), 0);
  ASSERT_GT(NumTableFilesAtLevel(4), prev_num_files_l4);
  ASSERT_GT(num_no.load(), 2);
  ASSERT_GT(num_lz4.load(), 0);
  ASSERT_GT(num_zlib.load(), 0);
}
TEST_F(DBTest, DynamicCompactionOptions) {
  const uint64_t k32KB = 1 << 15;
  const uint64_t k64KB = 1 << 16;
  const uint64_t k128KB = 1 << 17;
  const uint64_t k1MB = 1 << 20;
  const uint64_t k4KB = 1 << 12;
  Options options;
  options.env = env_;
  options.create_if_missing = true;
  options.compression = kNoCompression;
  options.soft_rate_limit = 1.1;
  options.write_buffer_size = k64KB;
  options.max_write_buffer_number = 2;
  options.level0_file_num_compaction_trigger = 3;
  options.level0_slowdown_writes_trigger = 4;
  options.level0_stop_writes_trigger = 8;
  options.max_grandparent_overlap_factor = 10;
  options.expanded_compaction_factor = 25;
  options.source_compaction_factor = 1;
  options.target_file_size_base = k64KB;
  options.target_file_size_multiplier = 1;
  options.max_bytes_for_level_base = k128KB;
  options.max_bytes_for_level_multiplier = 4;
  env_->SetBackgroundThreads(1, Env::LOW);
  env_->SetBackgroundThreads(1, Env::HIGH);
  DestroyAndReopen(options);
  auto gen_l0_kb = [this](int start, int size, int stride) {
    Random rnd(301);
    for (int i = 0; i < size; i++) {
      ASSERT_OK(Put(Key(start + stride * i), RandomString(&rnd, 1024)));
    }
    dbfull()->TEST_WaitForFlushMemTable();
  };
  gen_l0_kb(0, 64, 1);
  ASSERT_EQ(NumTableFilesAtLevel(0), 1);
  gen_l0_kb(0, 64, 1);
  ASSERT_EQ(NumTableFilesAtLevel(0), 2);
  gen_l0_kb(0, 64, 1);
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ("0,1", FilesPerLevel());
  std::vector<LiveFileMetaData> metadata;
  db_->GetLiveFilesMetaData(&metadata);
  ASSERT_EQ(1U, metadata.size());
  ASSERT_LE(metadata[0].size, k64KB + k4KB);
  ASSERT_GE(metadata[0].size, k64KB - k4KB);
  ASSERT_OK(dbfull()->SetOptions({
    {"level0_file_num_compaction_trigger", "2"},
    {"target_file_size_base", ToString(k32KB) }
  }));
  gen_l0_kb(0, 64, 1);
  ASSERT_EQ("1,1", FilesPerLevel());
  gen_l0_kb(0, 64, 1);
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ("0,2", FilesPerLevel());
  metadata.clear();
  db_->GetLiveFilesMetaData(&metadata);
  ASSERT_EQ(2U, metadata.size());
  ASSERT_LE(metadata[0].size, k32KB + k4KB);
  ASSERT_GE(metadata[0].size, k32KB - k4KB);
  ASSERT_LE(metadata[1].size, k32KB + k4KB);
  ASSERT_GE(metadata[1].size, k32KB - k4KB);
  ASSERT_OK(dbfull()->SetOptions({
    {"max_bytes_for_level_base", ToString(k1MB) }
  }));
  for (int i = 0; i < 96; ++i) {
    gen_l0_kb(i, 64, 96);
  }
  dbfull()->TEST_WaitForCompact();
  ASSERT_GT(SizeAtLevel(1), k1MB / 2);
  ASSERT_LT(SizeAtLevel(1), k1MB + k1MB / 2);
  ASSERT_GT(SizeAtLevel(2), 2 * k1MB);
  ASSERT_LT(SizeAtLevel(2), 6 * k1MB);
  ASSERT_OK(dbfull()->SetOptions({
    {"max_bytes_for_level_multiplier", "2"},
    {"max_bytes_for_level_base", ToString(k128KB) }
  }));
  for (int i = 0; i < 20; ++i) {
    gen_l0_kb(i, 64, 32);
  }
  dbfull()->TEST_WaitForCompact();
  uint64_t total_size =
    SizeAtLevel(1) + SizeAtLevel(2) + SizeAtLevel(3);
  ASSERT_TRUE(total_size < k128KB * 7 * 1.5);
  dbfull()->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  SleepingBackgroundTask sleeping_task_low1;
  env_->Schedule(&SleepingBackgroundTask::DoSleepTask, &sleeping_task_low1,
                 Env::Priority::LOW);
  ASSERT_EQ(NumTableFilesAtLevel(0), 0);
  int count = 0;
  Random rnd(301);
  WriteOptions wo;
  wo.timeout_hint_us = 10000;
  while (Put(Key(count), RandomString(&rnd, 1024), wo).ok() && count < 64) {
    dbfull()->TEST_FlushMemTable(true);
    count++;
  }
  ASSERT_EQ(count, 8);
  sleeping_task_low1.WakeUp();
  sleeping_task_low1.WaitUntilDone();
  ASSERT_OK(dbfull()->SetOptions({
    {"level0_stop_writes_trigger", "6"}
  }));
  dbfull()->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  ASSERT_EQ(NumTableFilesAtLevel(0), 0);
  SleepingBackgroundTask sleeping_task_low2;
  env_->Schedule(&SleepingBackgroundTask::DoSleepTask, &sleeping_task_low2,
                 Env::Priority::LOW);
  count = 0;
  while (Put(Key(count), RandomString(&rnd, 1024), wo).ok() && count < 64) {
    dbfull()->TEST_FlushMemTable(true);
    count++;
  }
  ASSERT_EQ(count, 6);
  sleeping_task_low2.WakeUp();
  sleeping_task_low2.WaitUntilDone();
  ASSERT_OK(dbfull()->SetOptions({
    {"disable_auto_compactions", "true"}
  }));
  dbfull()->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  ASSERT_EQ(NumTableFilesAtLevel(0), 0);
  for (int i = 0; i < 4; ++i) {
    ASSERT_OK(Put(Key(i), RandomString(&rnd, 1024)));
    dbfull()->TEST_FlushMemTable(true);
  }
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ(NumTableFilesAtLevel(0), 4);
  ASSERT_OK(dbfull()->SetOptions({
    {"disable_auto_compactions", "false"}
  }));
  dbfull()->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  ASSERT_EQ(NumTableFilesAtLevel(0), 0);
  for (int i = 0; i < 4; ++i) {
    ASSERT_OK(Put(Key(i), RandomString(&rnd, 1024)));
    dbfull()->TEST_FlushMemTable(true);
  }
  dbfull()->TEST_WaitForCompact();
  ASSERT_LT(NumTableFilesAtLevel(0), 4);
  options.max_background_compactions = 1;
  options.max_background_flushes = 0;
  options.max_mem_compaction_level = 2;
  DestroyAndReopen(options);
  ASSERT_EQ(NumTableFilesAtLevel(0), 0);
  ASSERT_EQ(NumTableFilesAtLevel(1), 0);
  ASSERT_EQ(NumTableFilesAtLevel(2), 0);
  ASSERT_OK(Put("max_mem_compaction_level_key", RandomString(&rnd, 8)));
  dbfull()->TEST_FlushMemTable(true);
  ASSERT_EQ(NumTableFilesAtLevel(0), 0);
  ASSERT_EQ(NumTableFilesAtLevel(1), 0);
  ASSERT_EQ(NumTableFilesAtLevel(2), 1);
  ASSERT_TRUE(Put("max_mem_compaction_level_key",
              RandomString(&rnd, 8)).ok());
  ASSERT_OK(dbfull()->SetOptions({
    {"max_mem_compaction_level", "1"}
  }));
  dbfull()->TEST_FlushMemTable(true);
  ASSERT_EQ(NumTableFilesAtLevel(0), 0);
  ASSERT_EQ(NumTableFilesAtLevel(1), 1);
  ASSERT_EQ(NumTableFilesAtLevel(2), 1);
  ASSERT_TRUE(Put("max_mem_compaction_level_key",
              RandomString(&rnd, 8)).ok());
  ASSERT_OK(dbfull()->SetOptions({
    {"max_mem_compaction_level", "0"}
  }));
  dbfull()->TEST_FlushMemTable(true);
  ASSERT_EQ(NumTableFilesAtLevel(0), 1);
  ASSERT_EQ(NumTableFilesAtLevel(1), 1);
  ASSERT_EQ(NumTableFilesAtLevel(2), 1);
}
TEST_F(DBTest, FileCreationRandomFailure) {
  Options options;
  options.env = env_;
  options.create_if_missing = true;
  options.write_buffer_size = 100000;
  options.target_file_size_base = 200000;
  options.max_bytes_for_level_base = 1000000;
  options.max_bytes_for_level_multiplier = 2;
  DestroyAndReopen(options);
  Random rnd(301);
  const int kTestSize = kCDTKeysPerBuffer * 4096;
  const int kTotalIteration = 100;
  const int kRandomFailureTest = kTotalIteration / 2;
  std::vector<std::string> values;
  for (int i = 0; i < kTestSize; ++i) {
    values.push_back("NOT_FOUND");
  }
  for (int j = 0; j < kTotalIteration; ++j) {
    if (j == kRandomFailureTest) {
      env_->non_writeable_rate_.store(90);
    }
    for (int k = 0; k < kTestSize; ++k) {
      std::string value = RandomString(&rnd, 100);
      Status s = Put(Key(k), Slice(value));
      if (s.ok()) {
        values[k] = value;
      }
      if (j < kRandomFailureTest) {
        ASSERT_OK(s);
      }
    }
  }
  dbfull()->TEST_WaitForFlushMemTable();
  dbfull()->TEST_WaitForCompact();
  for (int k = 0; k < kTestSize; ++k) {
    auto v = Get(Key(k));
    ASSERT_EQ(v, values[k]);
  }
  env_->non_writeable_rate_.store(0);
  Reopen(options);
  for (int k = 0; k < kTestSize; ++k) {
    auto v = Get(Key(k));
    ASSERT_EQ(v, values[k]);
  }
}
TEST_F(DBTest, PartialCompactionFailure) {
  Options options;
  const int kKeySize = 16;
  const int kKvSize = 1000;
  const int kKeysPerBuffer = 100;
  const int kNumL1Files = 5;
  options.create_if_missing = true;
  options.write_buffer_size = kKeysPerBuffer * kKvSize;
  options.max_write_buffer_number = 2;
  options.target_file_size_base =
      options.write_buffer_size *
      (options.max_write_buffer_number - 1);
  options.level0_file_num_compaction_trigger = kNumL1Files;
  options.max_bytes_for_level_base =
      options.level0_file_num_compaction_trigger *
      options.target_file_size_base;
  options.max_bytes_for_level_multiplier = 2;
  options.compression = kNoCompression;
  env_->SetBackgroundThreads(1, Env::HIGH);
  env_->SetBackgroundThreads(1, Env::LOW);
  SleepingBackgroundTask sleeping_task_low;
  env_->Schedule(&SleepingBackgroundTask::DoSleepTask, &sleeping_task_low,
                 Env::Priority::LOW);
  options.env = env_;
  DestroyAndReopen(options);
  const int kNumInsertedKeys =
      options.level0_file_num_compaction_trigger *
      (options.max_write_buffer_number - 1) *
      kKeysPerBuffer;
  Random rnd(301);
  std::vector<std::string> keys;
  std::vector<std::string> values;
  for (int k = 0; k < kNumInsertedKeys; ++k) {
    keys.emplace_back(RandomString(&rnd, kKeySize));
    values.emplace_back(RandomString(&rnd, kKvSize - kKeySize));
    ASSERT_OK(Put(Slice(keys[k]), Slice(values[k])));
  }
  dbfull()->TEST_FlushMemTable(true);
  ASSERT_GE(NumTableFilesAtLevel(0),
            options.level0_file_num_compaction_trigger);
  auto previous_num_level0_files = NumTableFilesAtLevel(0);
  env_->non_writable_count_ = 1;
  sleeping_task_low.WakeUp();
  sleeping_task_low.WaitUntilDone();
  ASSERT_TRUE(!dbfull()->TEST_WaitForCompact().ok());
  ASSERT_EQ(NumTableFilesAtLevel(1), 0);
  ASSERT_EQ(NumTableFilesAtLevel(0), previous_num_level0_files);
  for (int k = 0; k < kNumInsertedKeys; ++k) {
    ASSERT_EQ(values[k], Get(keys[k]));
  }
  env_->non_writable_count_ = 0;
  Reopen(options);
  for (int k = 0; k < kNumInsertedKeys; ++k) {
    ASSERT_EQ(values[k], Get(keys[k]));
  }
}
TEST_F(DBTest, DynamicMiscOptions) {
  Options options;
  options.env = env_;
  options.create_if_missing = true;
  options.max_sequential_skip_in_iterations = 16;
  options.compression = kNoCompression;
  options.statistics = rocksdb::CreateDBStatistics();
  DestroyAndReopen(options);
  auto assert_reseek_count = [this, &options](int key_start, int num_reseek) {
    int key0 = key_start;
    int key1 = key_start + 1;
    int key2 = key_start + 2;
    Random rnd(301);
    ASSERT_OK(Put(Key(key0), RandomString(&rnd, 8)));
    for (int i = 0; i < 10; ++i) {
      ASSERT_OK(Put(Key(key1), RandomString(&rnd, 8)));
    }
    ASSERT_OK(Put(Key(key2), RandomString(&rnd, 8)));
    std::unique_ptr<Iterator> iter(db_->NewIterator(ReadOptions()));
    iter->Seek(Key(key1));
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().compare(Key(key1)), 0);
    iter->Next();
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().compare(Key(key2)), 0);
    ASSERT_EQ(num_reseek,
              TestGetTickerCount(options, NUMBER_OF_RESEEKS_IN_ITERATION));
  };
  assert_reseek_count(100, 0);
  ASSERT_OK(dbfull()->SetOptions({
    {"max_sequential_skip_in_iterations", "4"}
  }));
  dbfull()->TEST_FlushMemTable(true);
  assert_reseek_count(200, 1);
  ASSERT_OK(dbfull()->SetOptions({
    {"max_sequential_skip_in_iterations", "16"}
  }));
  dbfull()->TEST_FlushMemTable(true);
  assert_reseek_count(300, 1);
}
TEST_F(DBTest, DontDeletePendingOutputs) {
  Options options;
  options.env = env_;
  options.create_if_missing = true;
  DestroyAndReopen(options);
  std::function<void()> purge_obsolete_files_function = [&]() {
    JobContext job_context(0);
    dbfull()->TEST_LockMutex();
    dbfull()->FindObsoleteFiles(&job_context, true );
    dbfull()->TEST_UnlockMutex();
    dbfull()->PurgeObsoleteFiles(job_context);
    job_context.Clean();
  };
  env_->table_write_callback_ = &purge_obsolete_files_function;
  for (int i = 0; i < 2; ++i) {
    ASSERT_OK(Put("a", "begin"));
    ASSERT_OK(Put("z", "end"));
    ASSERT_OK(Flush());
  }
  Compact("a", "b");
}
TEST_F(DBTest, DontDeleteMovedFile) {
  Options options = CurrentOptions();
  options.env = env_;
  options.create_if_missing = true;
  options.max_bytes_for_level_base = 1024 * 1024;
  options.level0_file_num_compaction_trigger =
      2;
  DestroyAndReopen(options);
  Random rnd(301);
  for (int i = 0; i < 2; ++i) {
    for (int j = 0; j < 100; ++j) {
      ASSERT_OK(Put(Key(i * 50 + j), RandomString(&rnd, 10 * 1024)));
    }
    ASSERT_OK(Flush());
  }
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ("0,0,1", FilesPerLevel(0));
  Reopen(options);
}
TEST_F(DBTest, DeleteMovedFileAfterCompaction) {
  for (int iter = 0; iter < 2; ++iter) {
    Options options = CurrentOptions();
    options.env = env_;
    if (iter == 1) {
      options.delete_obsolete_files_period_micros = 0;
    }
    options.create_if_missing = true;
    options.level0_file_num_compaction_trigger =
        2;
    OnFileDeletionListener* listener = new OnFileDeletionListener();
    options.listeners.emplace_back(listener);
    DestroyAndReopen(options);
    Random rnd(301);
    for (int i = 0; i < 2; ++i) {
      for (int j = 0; j < 100; ++j) {
        ASSERT_OK(Put(Key(i * 50 + j), RandomString(&rnd, 10 * 1024)));
      }
      ASSERT_OK(Flush());
    }
    dbfull()->TEST_WaitForCompact();
    ASSERT_EQ("0,1", FilesPerLevel(0));
    SleepingBackgroundTask sleeping_task;
    env_->Schedule(&SleepingBackgroundTask::DoSleepTask, &sleeping_task,
                   Env::Priority::LOW);
    options.max_bytes_for_level_base = 1024 * 1024;
    Reopen(options);
    std::unique_ptr<Iterator> iterator(db_->NewIterator(ReadOptions()));
    ASSERT_EQ("0,1", FilesPerLevel(0));
    sleeping_task.WakeUp();
    sleeping_task.WaitUntilDone();
    dbfull()->TEST_WaitForCompact();
    ASSERT_EQ("0,0,1", FilesPerLevel(0));
    std::vector<LiveFileMetaData> metadata;
    db_->GetLiveFilesMetaData(&metadata);
    ASSERT_EQ(metadata.size(), 1U);
    auto moved_file_name = metadata[0].name;
    for (int i = 0; i < 2; ++i) {
      for (int j = 0; j < 100; ++j) {
        ASSERT_OK(Put(Key(i * 50 + j + 100), RandomString(&rnd, 10 * 1024)));
      }
      ASSERT_OK(Flush());
    }
    dbfull()->TEST_WaitForCompact();
    ASSERT_EQ("0,0,2", FilesPerLevel(0));
    ASSERT_TRUE(env_->FileExists(dbname_ + moved_file_name));
    listener->SetExpectedFileName(dbname_ + moved_file_name);
    iterator.reset();
    ASSERT_TRUE(!env_->FileExists(dbname_ + moved_file_name));
    listener->VerifyMatchedCount(1);
  }
}
TEST_F(DBTest, OptimizeFiltersForHits) {
  Options options = CurrentOptions();
  options.write_buffer_size = 256 * 1024;
  options.target_file_size_base = 256 * 1024;
  options.level0_file_num_compaction_trigger = 2;
  options.level0_slowdown_writes_trigger = 2;
  options.level0_stop_writes_trigger = 4;
  options.max_bytes_for_level_base = 256 * 1024;
  options.max_write_buffer_number = 2;
  options.max_background_compactions = 8;
  options.max_background_flushes = 8;
  options.compaction_style = kCompactionStyleLevel;
  BlockBasedTableOptions bbto;
  bbto.filter_policy.reset(NewBloomFilterPolicy(10, true));
  bbto.whole_key_filtering = true;
  options.table_factory.reset(NewBlockBasedTableFactory(bbto));
  options.optimize_filters_for_hits = true;
  options.statistics = rocksdb::CreateDBStatistics();
  CreateAndReopenWithCF({"mypikachu"}, options);
  int numkeys = 200000;
  for (int i = 0; i < 20; i += 2) {
    for (int j = i; j < numkeys; j += 20) {
      ASSERT_OK(Put(1, Key(j), "val"));
    }
  }
  ASSERT_OK(Flush(1));
  dbfull()->TEST_WaitForCompact();
  for (int i = 1; i < numkeys; i += 2) {
    ASSERT_EQ(Get(1, Key(i)), "NOT_FOUND");
  }
  ASSERT_EQ(0, TestGetTickerCount(options, GET_HIT_L0));
  ASSERT_EQ(0, TestGetTickerCount(options, GET_HIT_L1));
  ASSERT_EQ(0, TestGetTickerCount(options, GET_HIT_L2_AND_UP));
  ASSERT_GT(90000, TestGetTickerCount(options, BLOOM_FILTER_USEFUL));
  for (int i = 0; i < numkeys; i += 2) {
    ASSERT_EQ(Get(1, Key(i)), "val");
  }
}
TEST_F(DBTest, L0L1L2AndUpHitCounter) {
  Options options = CurrentOptions();
  options.write_buffer_size = 32 * 1024;
  options.target_file_size_base = 32 * 1024;
  options.level0_file_num_compaction_trigger = 2;
  options.level0_slowdown_writes_trigger = 2;
  options.level0_stop_writes_trigger = 4;
  options.max_bytes_for_level_base = 64 * 1024;
  options.max_write_buffer_number = 2;
  options.max_background_compactions = 8;
  options.max_background_flushes = 8;
  options.statistics = rocksdb::CreateDBStatistics();
  CreateAndReopenWithCF({"mypikachu"}, options);
  int numkeys = 20000;
  for (int i = 0; i < numkeys; i++) {
    ASSERT_OK(Put(1, Key(i), "val"));
  }
  ASSERT_EQ(0, TestGetTickerCount(options, GET_HIT_L0));
  ASSERT_EQ(0, TestGetTickerCount(options, GET_HIT_L1));
  ASSERT_EQ(0, TestGetTickerCount(options, GET_HIT_L2_AND_UP));
  ASSERT_OK(Flush(1));
  dbfull()->TEST_WaitForCompact();
  for (int i = 0; i < numkeys; i++) {
    ASSERT_EQ(Get(1, Key(i)), "val");
  }
  ASSERT_GT(TestGetTickerCount(options, GET_HIT_L0), 100);
  ASSERT_GT(TestGetTickerCount(options, GET_HIT_L1), 100);
  ASSERT_GT(TestGetTickerCount(options, GET_HIT_L2_AND_UP), 100);
  ASSERT_EQ(numkeys, TestGetTickerCount(options, GET_HIT_L0) +
                         TestGetTickerCount(options, GET_HIT_L1) +
                         TestGetTickerCount(options, GET_HIT_L2_AND_UP));
}
TEST_F(DBTest, EncodeDecompressedBlockSizeTest) {
  CompressionType compressions[] = {kZlibCompression, kBZip2Compression,
                                    kLZ4Compression, kLZ4HCCompression};
  for (int iter = 0; iter < 4; ++iter) {
    if (!CompressionTypeSupported(compressions[iter])) {
      continue;
    }
    for (int first_table_version = 1; first_table_version <= 2;
         ++first_table_version) {
      BlockBasedTableOptions table_options;
      table_options.format_version = first_table_version;
      table_options.filter_policy.reset(NewBloomFilterPolicy(10));
      Options options = CurrentOptions();
      options.table_factory.reset(NewBlockBasedTableFactory(table_options));
      options.create_if_missing = true;
      options.compression = compressions[iter];
      DestroyAndReopen(options);
      int kNumKeysWritten = 100000;
      Random rnd(301);
      for (int i = 0; i < kNumKeysWritten; ++i) {
        ASSERT_OK(Put(Key(i), RandomString(&rnd, 128) + std::string(128, 'a')));
      }
      table_options.format_version = first_table_version == 1 ? 2 : 1;
      options.table_factory.reset(NewBlockBasedTableFactory(table_options));
      Reopen(options);
      for (int i = 0; i < kNumKeysWritten; ++i) {
        auto r = Get(Key(i));
        ASSERT_EQ(r.substr(128), std::string(128, 'a'));
      }
    }
  }
}
TEST_F(DBTest, MutexWaitStats) {
  Options options = CurrentOptions();
  options.create_if_missing = true;
  options.statistics = rocksdb::CreateDBStatistics();
  CreateAndReopenWithCF({"pikachu"}, options);
  const int64_t kMutexWaitDelay = 100;
  ThreadStatusUtil::TEST_SetStateDelay(
      ThreadStatus::STATE_MUTEX_WAIT, kMutexWaitDelay);
  ASSERT_OK(Put("hello", "rocksdb"));
  ASSERT_GE(TestGetTickerCount(
            options, DB_MUTEX_WAIT_MICROS), kMutexWaitDelay);
  ThreadStatusUtil::TEST_SetStateDelay(
      ThreadStatus::STATE_MUTEX_WAIT, 0);
}
TEST_F(DBTest, DeleteObsoleteFilesPendingOutputs) {
  Options options = CurrentOptions();
  options.env = env_;
  options.write_buffer_size = 2 * 1024 * 1024;
  options.max_bytes_for_level_base = 1024 * 1024;
  options.level0_file_num_compaction_trigger =
      2;
  options.max_background_flushes = 2;
  options.max_background_compactions = 2;
  OnFileDeletionListener* listener = new OnFileDeletionListener();
  options.listeners.emplace_back(listener);
  Reopen(options);
  Random rnd(301);
  for (int i = 0; i < 2; ++i) {
    for (int j = 0; j < 100; ++j) {
      ASSERT_OK(Put(Key(i * 50 + j), RandomString(&rnd, 10 * 1024)));
    }
    ASSERT_OK(Flush());
  }
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ("0,0,1", FilesPerLevel(0));
  SleepingBackgroundTask blocking_thread;
  port::Mutex mutex_;
  bool already_blocked(false);
  std::function<void()> block_first_time = [&]() {
    bool blocking = false;
    {
      MutexLock l(&mutex_);
      if (!already_blocked) {
        blocking = true;
        already_blocked = true;
      }
    }
    if (blocking) {
      blocking_thread.DoSleep();
    }
  };
  env_->table_write_callback_ = &block_first_time;
  for (int j = 0; j < 256; ++j) {
    ASSERT_OK(Put(Key(j), RandomString(&rnd, 10 * 1024)));
  }
  ASSERT_OK(dbfull()->TEST_CompactRange(2, nullptr, nullptr));
  ASSERT_EQ("0,0,0,1", FilesPerLevel(0));
  std::vector<LiveFileMetaData> metadata;
  db_->GetLiveFilesMetaData(&metadata);
  ASSERT_EQ(metadata.size(), 1U);
  auto file_on_L2 = metadata[0].name;
  listener->SetExpectedFileName(dbname_ + file_on_L2);
  ASSERT_OK(dbfull()->TEST_CompactRange(3, nullptr, nullptr, nullptr,
                                        true ));
  ASSERT_EQ("0,0,0,0,1", FilesPerLevel(0));
  blocking_thread.WakeUp();
  blocking_thread.WaitUntilDone();
  dbfull()->TEST_WaitForFlushMemTable();
  ASSERT_EQ("1,0,0,0,1", FilesPerLevel(0));
  metadata.clear();
  db_->GetLiveFilesMetaData(&metadata);
  ASSERT_EQ(metadata.size(), 2U);
  ASSERT_TRUE(!env_->FileExists(dbname_ + file_on_L2));
  listener->VerifyMatchedCount(1);
}
TEST_F(DBTest, CloseSpeedup) {
  Options options = CurrentOptions();
  options.compaction_style = kCompactionStyleLevel;
  options.write_buffer_size = 100 << 10;
  options.level0_file_num_compaction_trigger = 2;
  options.num_levels = 4;
  options.max_bytes_for_level_base = 400 * 1024;
  options.max_write_buffer_number = 16;
  env_->SetBackgroundThreads(1, Env::LOW);
  env_->SetBackgroundThreads(1, Env::HIGH);
  SleepingBackgroundTask sleeping_task_low;
  env_->Schedule(&SleepingBackgroundTask::DoSleepTask, &sleeping_task_low,
                 Env::Priority::LOW);
  SleepingBackgroundTask sleeping_task_high;
  env_->Schedule(&SleepingBackgroundTask::DoSleepTask, &sleeping_task_high,
                 Env::Priority::HIGH);
  std::vector<std::string> filenames;
  env_->GetChildren(dbname_, &filenames);
  for (size_t i = 0; i < filenames.size(); ++i) {
    env_->DeleteFile(dbname_ + "/" + filenames[i]);
  }
  env_->DeleteDir(dbname_);
  DestroyAndReopen(options);
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  env_->SetBackgroundThreads(1, Env::LOW);
  env_->SetBackgroundThreads(1, Env::HIGH);
  Random rnd(301);
  int key_idx = 0;
  for (int num = 0; num < 5; num++) {
    GenerateNewFile(&rnd, &key_idx, true);
  }
  ASSERT_EQ(0, GetSstFileCount(dbname_));
  Close();
  ASSERT_EQ(0, GetSstFileCount(dbname_));
  sleeping_task_high.WakeUp();
  sleeping_task_high.WaitUntilDone();
  sleeping_task_low.WakeUp();
  sleeping_task_low.WaitUntilDone();
  Destroy(options);
}
class DelayedMergeOperator : public AssociativeMergeOperator {
 private:
  DBTest* db_test_;
 public:
  explicit DelayedMergeOperator(DBTest* d) : db_test_(d) {}
  virtual bool Merge(const Slice& key, const Slice* existing_value,
                     const Slice& value, std::string* new_value,
                     Logger* logger) const override {
    db_test_->env_->addon_time_.fetch_add(1000);
    return true;
  }
  virtual const char* Name() const override { return "DelayedMergeOperator"; }
};
TEST_F(DBTest, MergeTestTime) {
  std::string one, two, three;
  PutFixed64(&one, 1);
  PutFixed64(&two, 2);
  PutFixed64(&three, 3);
  SetPerfLevel(kEnableTime);
  this->env_->addon_time_.store(0);
  Options options;
  options = CurrentOptions(options);
  options.statistics = rocksdb::CreateDBStatistics();
  options.merge_operator.reset(new DelayedMergeOperator(this));
  DestroyAndReopen(options);
  ASSERT_EQ(TestGetTickerCount(options, MERGE_OPERATION_TOTAL_TIME), 0);
  db_->Put(WriteOptions(), "foo", one);
  ASSERT_OK(Flush());
  ASSERT_OK(db_->Merge(WriteOptions(), "foo", two));
  ASSERT_OK(Flush());
  ASSERT_OK(db_->Merge(WriteOptions(), "foo", three));
  ASSERT_OK(Flush());
  ReadOptions opt;
  opt.verify_checksums = true;
  opt.snapshot = nullptr;
  std::string result;
  db_->Get(opt, "foo", &result);
  ASSERT_LT(TestGetTickerCount(options, MERGE_OPERATION_TOTAL_TIME), 2800000);
  ASSERT_GT(TestGetTickerCount(options, MERGE_OPERATION_TOTAL_TIME), 1200000);
  ReadOptions read_options;
  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  int count = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ASSERT_OK(iter->status());
    ++count;
  }
  ASSERT_EQ(1, count);
  ASSERT_LT(TestGetTickerCount(options, MERGE_OPERATION_TOTAL_TIME), 6000000);
  ASSERT_GT(TestGetTickerCount(options, MERGE_OPERATION_TOTAL_TIME), 3200000);
}
TEST_F(DBTest, MergeCompactionTimeTest) {
  SetPerfLevel(kEnableTime);
  Options options;
  options = CurrentOptions(options);
  options.compaction_filter_factory = std::make_shared<KeepFilterFactory>();
  options.statistics = rocksdb::CreateDBStatistics();
  options.merge_operator.reset(new DelayedMergeOperator(this));
  options.compaction_style = kCompactionStyleUniversal;
  DestroyAndReopen(options);
  for (int i = 0; i < 1000; i++) {
    ASSERT_OK(db_->Merge(WriteOptions(), "foo", "TEST"));
    ASSERT_OK(Flush());
  }
  dbfull()->TEST_WaitForFlushMemTable();
  dbfull()->TEST_WaitForCompact();
  ASSERT_NE(TestGetTickerCount(options, MERGE_OPERATION_TOTAL_TIME), 0);
}
TEST_F(DBTest, FilterCompactionTimeTest) {
  Options options;
  options.compaction_filter_factory =
      std::make_shared<DelayFilterFactory>(this);
  options.disable_auto_compactions = true;
  options.create_if_missing = true;
  options.statistics = rocksdb::CreateDBStatistics();
  options = CurrentOptions(options);
  DestroyAndReopen(options);
  for (int table = 0; table < 4; ++table) {
    for (int i = 0; i < 10 + table; ++i) {
      Put(ToString(table * 100 + i), "val");
    }
    Flush();
  }
  ASSERT_OK(db_->CompactRange(CompactRangeOptions(), nullptr, nullptr));
  ASSERT_EQ(0U, CountLiveFiles());
  Reopen(options);
  Iterator* itr = db_->NewIterator(ReadOptions());
  itr->SeekToFirst();
  ASSERT_NE(TestGetTickerCount(options, FILTER_OPERATION_TOTAL_TIME), 0);
  delete itr;
}
TEST_F(DBTest, TestLogCleanup) {
  Options options = CurrentOptions();
  options.write_buffer_size = 64 * 1024;
  options.max_write_buffer_number = 2;
  Reopen(options);
  for (int i = 0; i < 100000; ++i) {
    Put(Key(i), "val");
    ASSERT_LT(dbfull()->TEST_LogsToFreeSize(), static_cast<size_t>(3));
  }
}
TEST_F(DBTest, EmptyCompactedDB) {
  Options options;
  options.max_open_files = -1;
  options = CurrentOptions(options);
  Close();
  ASSERT_OK(ReadOnlyReopen(options));
  Status s = Put("new", "value");
  ASSERT_TRUE(s.IsNotSupported());
  Close();
}
TEST_F(DBTest, CompressLevelCompaction) {
  if (!Zlib_Supported()) {
    return;
  }
  Options options = CurrentOptions();
  options.compaction_style = kCompactionStyleLevel;
  options.write_buffer_size = 100 << 10;
  options.level0_file_num_compaction_trigger = 2;
  options.num_levels = 4;
  options.max_bytes_for_level_base = 400 * 1024;
  options.compression_per_level = {kNoCompression, kNoCompression,
                                   kZlibCompression};
  int matches = 0, didnt_match = 0, trivial_move = 0, non_trivial = 0;
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "Compaction::InputCompressionMatchesOutput:Matches",
      [&](void* arg) { matches++; });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "Compaction::InputCompressionMatchesOutput:DidntMatch",
      [&](void* arg) { didnt_match++; });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:NonTrivial",
      [&](void* arg) { non_trivial++; });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:TrivialMove",
      [&](void* arg) { trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  Reopen(options);
  Random rnd(301);
  int key_idx = 0;
  for (int num = 0; num < 3; num++) {
    GenerateNewFile(&rnd, &key_idx);
  }
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ(4, GetSstFileCount(dbname_));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4", FilesPerLevel(0));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,1", FilesPerLevel(0));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,2", FilesPerLevel(0));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,3", FilesPerLevel(0));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,4", FilesPerLevel(0));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,5", FilesPerLevel(0));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,6", FilesPerLevel(0));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,7", FilesPerLevel(0));
  GenerateNewFile(&rnd, &key_idx);
  ASSERT_EQ("1,4,8", FilesPerLevel(0));
  ASSERT_EQ(matches, 12);
  const int kCallsToInputCompressionMatch = 2;
  ASSERT_EQ(didnt_match, 8 * kCallsToInputCompressionMatch);
  ASSERT_EQ(trivial_move, 12);
  ASSERT_EQ(non_trivial, 8);
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
  for (int i = 0; i < key_idx; i++) {
    auto v = Get(Key(i));
    ASSERT_NE(v, "NOT_FOUND");
    ASSERT_TRUE(v.size() == 1 || v.size() == 10000);
  }
  Reopen(options);
  for (int i = 0; i < key_idx; i++) {
    auto v = Get(Key(i));
    ASSERT_NE(v, "NOT_FOUND");
    ASSERT_TRUE(v.size() == 1 || v.size() == 10000);
  }
  Destroy(options);
}
class CountingDeleteTabPropCollector : public TablePropertiesCollector {
 public:
  const char* Name() const override { return "CountingDeleteTabPropCollector"; }
  Status AddUserKey(const Slice& user_key, const Slice& value, EntryType type,
                    SequenceNumber seq, uint64_t file_size) override {
    if (type == kEntryDelete) {
      num_deletes_++;
    }
    return Status::OK();
  }
  bool NeedCompact() const override { return num_deletes_ > 10; }
  UserCollectedProperties GetReadableProperties() const override {
    return UserCollectedProperties{};
  }
  Status Finish(UserCollectedProperties* properties) override {
    *properties =
        UserCollectedProperties{{"num_delete", ToString(num_deletes_)}};
    return Status::OK();
  }
 private:
  uint32_t num_deletes_ = 0;
};
class CountingDeleteTabPropCollectorFactory
    : public TablePropertiesCollectorFactory {
 public:
  virtual TablePropertiesCollector* CreateTablePropertiesCollector() override {
    return new CountingDeleteTabPropCollector();
  }
  const char* Name() const override {
    return "CountingDeleteTabPropCollectorFactory";
  }
};
TEST_F(DBTest, TablePropertiesNeedCompactTest) {
  Random rnd(301);
  Options options;
  options.create_if_missing = true;
  options.write_buffer_size = 4096;
  options.max_write_buffer_number = 8;
  options.level0_file_num_compaction_trigger = 2;
  options.level0_slowdown_writes_trigger = 2;
  options.level0_stop_writes_trigger = 4;
  options.target_file_size_base = 2048;
  options.max_bytes_for_level_base = 10240;
  options.max_bytes_for_level_multiplier = 4;
  options.soft_rate_limit = 1.1;
  options.num_levels = 8;
  std::shared_ptr<TablePropertiesCollectorFactory> collector_factory(
      new CountingDeleteTabPropCollectorFactory);
  options.table_properties_collector_factories.resize(1);
  options.table_properties_collector_factories[0] = collector_factory;
  DestroyAndReopen(options);
  const int kMaxKey = 1000;
  for (int i = 0; i < kMaxKey; i++) {
    ASSERT_OK(Put(Key(i), RandomString(&rnd, 102)));
    ASSERT_OK(Put(Key(kMaxKey + i), RandomString(&rnd, 102)));
  }
  Flush();
  dbfull()->TEST_WaitForCompact();
  if (NumTableFilesAtLevel(0) == 1) {
    ASSERT_OK(Put(Key(0), ""));
    ASSERT_OK(Put(Key(kMaxKey * 2), ""));
    Flush();
    dbfull()->TEST_WaitForCompact();
  }
  ASSERT_EQ(NumTableFilesAtLevel(0), 0);
  {
    int c = 0;
    std::unique_ptr<Iterator> iter(db_->NewIterator(ReadOptions()));
    iter->Seek(Key(kMaxKey - 100));
    while (iter->Valid() && iter->key().compare(Key(kMaxKey + 100)) < 0) {
      iter->Next();
      ++c;
    }
    ASSERT_EQ(c, 200);
  }
  Delete(Key(0));
  for (int i = kMaxKey - 100; i < kMaxKey + 100; i++) {
    Delete(Key(i));
  }
  Delete(Key(kMaxKey * 2));
  Flush();
  dbfull()->TEST_WaitForCompact();
  {
    SetPerfLevel(kEnableCount);
    perf_context.Reset();
    int c = 0;
    std::unique_ptr<Iterator> iter(db_->NewIterator(ReadOptions()));
    iter->Seek(Key(kMaxKey - 100));
    while (iter->Valid() && iter->key().compare(Key(kMaxKey + 100)) < 0) {
      iter->Next();
    }
    ASSERT_EQ(c, 0);
    ASSERT_LT(perf_context.internal_delete_skipped_count, 30u);
    ASSERT_LT(perf_context.internal_key_skipped_count, 30u);
    SetPerfLevel(kDisable);
  }
}
TEST_F(DBTest, SuggestCompactRangeTest) {
  class CompactionFilterFactoryGetContext : public CompactionFilterFactory {
   public:
    virtual std::unique_ptr<CompactionFilter> CreateCompactionFilter(
        const CompactionFilter::Context& context) override {
      saved_context = context;
      std::unique_ptr<CompactionFilter> empty_filter;
      return empty_filter;
    }
    const char* Name() const override {
      return "CompactionFilterFactoryGetContext";
    }
    static bool IsManual(CompactionFilterFactory* compaction_filter_factory) {
      return reinterpret_cast<CompactionFilterFactoryGetContext*>(
                 compaction_filter_factory)->saved_context.is_manual_compaction;
    }
    CompactionFilter::Context saved_context;
  };
  Options options = CurrentOptions();
  options.compaction_style = kCompactionStyleLevel;
  options.compaction_filter_factory.reset(
      new CompactionFilterFactoryGetContext());
  options.write_buffer_size = 110 << 10;
  options.level0_file_num_compaction_trigger = 4;
  options.num_levels = 4;
  options.compression = kNoCompression;
  options.max_bytes_for_level_base = 450 << 10;
  options.target_file_size_base = 98 << 10;
  options.max_grandparent_overlap_factor = 1 << 20;
  Reopen(options);
  Random rnd(301);
  for (int num = 0; num < 3; num++) {
    GenerateNewRandomFile(&rnd);
  }
  GenerateNewRandomFile(&rnd);
  ASSERT_EQ("0,4", FilesPerLevel(0));
  ASSERT_TRUE(!CompactionFilterFactoryGetContext::IsManual(
                   options.compaction_filter_factory.get()));
  GenerateNewRandomFile(&rnd);
  ASSERT_EQ("1,4", FilesPerLevel(0));
  GenerateNewRandomFile(&rnd);
  ASSERT_EQ("2,4", FilesPerLevel(0));
  GenerateNewRandomFile(&rnd);
  ASSERT_EQ("3,4", FilesPerLevel(0));
  GenerateNewRandomFile(&rnd);
  ASSERT_EQ("0,4,4", FilesPerLevel(0));
  GenerateNewRandomFile(&rnd);
  ASSERT_EQ("1,4,4", FilesPerLevel(0));
  GenerateNewRandomFile(&rnd);
  ASSERT_EQ("2,4,4", FilesPerLevel(0));
  GenerateNewRandomFile(&rnd);
  ASSERT_EQ("3,4,4", FilesPerLevel(0));
  GenerateNewRandomFile(&rnd);
  ASSERT_EQ("0,4,8", FilesPerLevel(0));
  GenerateNewRandomFile(&rnd);
  ASSERT_EQ("1,4,8", FilesPerLevel(0));
  for (int i = 0; i < 3; ++i) {
    ASSERT_OK(experimental::SuggestCompactRange(db_, nullptr, nullptr));
    dbfull()->TEST_WaitForCompact();
  }
  ASSERT_EQ("0,0,13", FilesPerLevel(0));
  GenerateNewRandomFile(&rnd);
  ASSERT_EQ("1,0,13", FilesPerLevel(0));
  Slice start("a"), end("b");
  ASSERT_OK(experimental::SuggestCompactRange(db_, &start, &end));
  dbfull()->TEST_WaitForCompact();
  ASSERT_EQ("1,0,13", FilesPerLevel(0));
  start = Slice("j");
  end = Slice("m");
  ASSERT_OK(experimental::SuggestCompactRange(db_, &start, &end));
  dbfull()->TEST_WaitForCompact();
  ASSERT_TRUE(CompactionFilterFactoryGetContext::IsManual(
      options.compaction_filter_factory.get()));
  ASSERT_EQ("0,1,13", FilesPerLevel(0));
}
TEST_F(DBTest, PromoteL0) {
  Options options = CurrentOptions();
  options.disable_auto_compactions = true;
  options.write_buffer_size = 10 * 1024 * 1024;
  DestroyAndReopen(options);
  std::vector<std::pair<int32_t, int32_t>> ranges = {
      {81, 160}, {0, 80}, {161, 240}, {241, 320}};
  int32_t value_size = 10 * 1024;
  Random rnd(301);
  std::map<int32_t, std::string> values;
  for (const auto& range : ranges) {
    for (int32_t j = range.first; j < range.second; j++) {
      values[j] = RandomString(&rnd, value_size);
      ASSERT_OK(Put(Key(j), values[j]));
    }
    ASSERT_OK(Flush());
  }
  int32_t level0_files = NumTableFilesAtLevel(0, 0);
  ASSERT_EQ(level0_files, ranges.size());
  ASSERT_EQ(NumTableFilesAtLevel(1, 0), 0);
  ASSERT_OK(experimental::PromoteL0(db_, db_->DefaultColumnFamily(), 2));
  ASSERT_EQ(NumTableFilesAtLevel(0, 0), 0);
  ASSERT_EQ(NumTableFilesAtLevel(2, 0), level0_files);
  for (const auto& kv : values) {
    ASSERT_EQ(Get(Key(kv.first)), kv.second);
  }
}
TEST_F(DBTest, PromoteL0Failure) {
  Options options = CurrentOptions();
  options.disable_auto_compactions = true;
  options.write_buffer_size = 10 * 1024 * 1024;
  DestroyAndReopen(options);
  ASSERT_OK(Put(Key(0), ""));
  ASSERT_OK(Put(Key(3), ""));
  ASSERT_OK(Flush());
  ASSERT_OK(Put(Key(1), ""));
  ASSERT_OK(Flush());
  Status status;
  status = experimental::PromoteL0(db_, db_->DefaultColumnFamily());
  ASSERT_TRUE(status.IsInvalidArgument());
  ASSERT_OK(db_->CompactRange(CompactRangeOptions(), nullptr, nullptr));
  ASSERT_GE(NumTableFilesAtLevel(1, 0), 1);
  ASSERT_OK(Put(Key(5), ""));
  ASSERT_OK(Flush());
  status = experimental::PromoteL0(db_, db_->DefaultColumnFamily());
  ASSERT_TRUE(status.IsInvalidArgument());
}
TEST_F(DBTest, HugeNumberOfLevels) {
  Options options = CurrentOptions();
  options.write_buffer_size = 2 * 1024 * 1024;
  options.max_bytes_for_level_base = 2 * 1024 * 1024;
  options.num_levels = 12;
  options.max_background_compactions = 10;
  options.max_bytes_for_level_multiplier = 2;
  options.level_compaction_dynamic_level_bytes = true;
  DestroyAndReopen(options);
  Random rnd(301);
  for (int i = 0; i < 300000; ++i) {
    ASSERT_OK(Put(Key(i), RandomString(&rnd, 1024)));
  }
  ASSERT_OK(db_->CompactRange(CompactRangeOptions(), nullptr, nullptr));
}
TEST_F(DBTest, LargeBatchWithColumnFamilies) {
  Options options;
  options.env = env_;
  options = CurrentOptions(options);
  options.write_buffer_size = 100000;
  CreateAndReopenWithCF({"pikachu"}, options);
  int64_t j = 0;
  for (int i = 0; i < 5; i++) {
    for (int pass = 1; pass <= 3; pass++) {
      WriteBatch batch;
      size_t write_size = 1024 * 1024 * (5 + i);
      fprintf(stderr, "prepare: %ld MB, pass:%d\n", (write_size / 1024 / 1024),
              pass);
      for (;;) {
        std::string data(3000, j++ % 127 + 20);
        data += ToString(j);
        batch.Put(handles_[0], Slice(data), Slice(data));
        if (batch.GetDataSize() > write_size) {
          break;
        }
      }
      fprintf(stderr, "write: %ld MB\n", (batch.GetDataSize() / 1024 / 1024));
      ASSERT_OK(dbfull()->Write(WriteOptions(), &batch));
      fprintf(stderr, "done\n");
    }
  }
  ASSERT_OK(TryReopenWithColumnFamilies({"default", "pikachu"}, options));
}
TEST_F(DBTest, FlushesInParallelWithCompactRange) {
  for (int iter = 0; iter < 3; ++iter) {
    Options options = CurrentOptions();
    if (iter < 2) {
      options.compaction_style = kCompactionStyleLevel;
    } else {
      options.compaction_style = kCompactionStyleUniversal;
    }
    options.write_buffer_size = 110 << 10;
    options.level0_file_num_compaction_trigger = 4;
    options.num_levels = 4;
    options.compression = kNoCompression;
    options.max_bytes_for_level_base = 450 << 10;
    options.target_file_size_base = 98 << 10;
    options.max_write_buffer_number = 2;
    DestroyAndReopen(options);
    Random rnd(301);
    for (int num = 0; num < 14; num++) {
      GenerateNewRandomFile(&rnd);
    }
    if (iter == 1) {
    rocksdb::SyncPoint::GetInstance()->LoadDependency(
        {{"DBImpl::RunManualCompaction()::1",
          "DBTest::FlushesInParallelWithCompactRange:1"},
         {"DBTest::FlushesInParallelWithCompactRange:2",
          "DBImpl::RunManualCompaction()::2"}});
    } else {
      rocksdb::SyncPoint::GetInstance()->LoadDependency(
          {{"CompactionJob::Run():Start",
            "DBTest::FlushesInParallelWithCompactRange:1"},
           {"DBTest::FlushesInParallelWithCompactRange:2",
            "CompactionJob::Run():End"}});
    }
    rocksdb::SyncPoint::GetInstance()->EnableProcessing();
    std::vector<std::thread> threads;
    threads.emplace_back([&]() { Compact("a", "z"); });
    TEST_SYNC_POINT("DBTest::FlushesInParallelWithCompactRange:1");
    for (int num = 0; num < 3; num++) {
      GenerateNewRandomFile(&rnd, true);
    }
    TEST_SYNC_POINT("DBTest::FlushesInParallelWithCompactRange:2");
    for (auto& t : threads) {
      t.join();
    }
    rocksdb::SyncPoint::GetInstance()->DisableProcessing();
  }
}
TEST_F(DBTest, UniversalCompactionTargetLevel) {
  Options options;
  options.compaction_style = kCompactionStyleUniversal;
  options.write_buffer_size = 100 << 10;
  options.num_levels = 7;
  options.disable_auto_compactions = true;
  options = CurrentOptions(options);
  DestroyAndReopen(options);
  Random rnd(301);
  for (int i = 0; i < 210; i++) {
    ASSERT_OK(Put(Key(i), RandomString(&rnd, 100)));
  }
  ASSERT_OK(Flush());
  for (int i = 200; i < 300; i++) {
    ASSERT_OK(Put(Key(i), RandomString(&rnd, 100)));
  }
  ASSERT_OK(Flush());
  for (int i = 250; i < 260; i++) {
    ASSERT_OK(Put(Key(i), RandomString(&rnd, 100)));
  }
  ASSERT_OK(Flush());
  ASSERT_EQ("3", FilesPerLevel(0));
  CompactRangeOptions compact_options;
  compact_options.change_level = true;
  compact_options.target_level = 4;
  db_->CompactRange(compact_options, nullptr, nullptr);
  ASSERT_EQ("0,0,0,0,1", FilesPerLevel(0));
}
TEST_F(DBTest, SuggestCompactRangeNoTwoLevel0Compactions) {
  Options options = CurrentOptions();
  options.compaction_style = kCompactionStyleLevel;
  options.write_buffer_size = 110 << 10;
  options.level0_file_num_compaction_trigger = 4;
  options.num_levels = 4;
  options.compression = kNoCompression;
  options.max_bytes_for_level_base = 450 << 10;
  options.target_file_size_base = 98 << 10;
  options.max_write_buffer_number = 2;
  options.max_background_compactions = 2;
  DestroyAndReopen(options);
  Random rnd(301);
  for (int num = 0; num < 10; num++) {
    GenerateNewRandomFile(&rnd);
  }
  db_->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  rocksdb::SyncPoint::GetInstance()->LoadDependency(
      {{"CompactionJob::Run():Start",
        "DBTest::SuggestCompactRangeNoTwoLevel0Compactions:1"},
       {"DBTest::SuggestCompactRangeNoTwoLevel0Compactions:2",
        "CompactionJob::Run():End"}});
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  for (int num = 0; num < options.level0_file_num_compaction_trigger + 1;
       num++) {
    GenerateNewRandomFile(&rnd, true);
  }
  TEST_SYNC_POINT("DBTest::SuggestCompactRangeNoTwoLevel0Compactions:1");
  GenerateNewRandomFile(&rnd, true);
  dbfull()->TEST_WaitForFlushMemTable();
  ASSERT_OK(experimental::SuggestCompactRange(db_, nullptr, nullptr));
  for (int num = 0; num < options.level0_file_num_compaction_trigger + 1;
       num++) {
    GenerateNewRandomFile(&rnd, true);
  }
  TEST_SYNC_POINT("DBTest::SuggestCompactRangeNoTwoLevel0Compactions:2");
}
TEST_F(DBTest, DelayedWriteRate) {
  Options options;
  options.env = env_;
  env_->no_sleep_ = true;
  options = CurrentOptions(options);
  options.write_buffer_size = 100000;
  options.max_write_buffer_number = 256;
  options.disable_auto_compactions = true;
  options.level0_file_num_compaction_trigger = 3;
  options.level0_slowdown_writes_trigger = 3;
  options.level0_stop_writes_trigger = 999999;
  options.delayed_write_rate = 200000;
  CreateAndReopenWithCF({"pikachu"}, options);
  for (int i = 0; i < 3; i++) {
    Put(Key(i), std::string(10000, 'x'));
    Flush();
  }
  size_t estimated_total_size = 0;
  Random rnd(301);
  for (int i = 0; i < 3000; i++) {
    auto rand_num = rnd.Uniform(20);
    size_t entry_size = rand_num * rand_num * rand_num;
    WriteOptions wo;
    if (rnd.Uniform(20) == 6) {
      wo.timeout_hint_us = 1500;
    }
    Put(Key(i), std::string(entry_size, 'x'), wo);
    estimated_total_size += entry_size + 20;
    if (rnd.Uniform(20) == 6) {
      env_->SleepForMicroseconds(2666);
    }
  }
  uint64_t estimated_sleep_time =
      estimated_total_size / options.delayed_write_rate * 1000000U;
  ASSERT_GT(env_->addon_time_.load(), estimated_sleep_time * 0.8);
  ASSERT_LT(env_->addon_time_.load(), estimated_sleep_time * 1.1);
  env_->no_sleep_ = false;
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}
TEST_F(DBTest, SoftLimit) {
  Options options;
  options.env = env_;
  options = CurrentOptions(options);
  options.write_buffer_size = 100000;
  options.max_write_buffer_number = 256;
  options.level0_file_num_compaction_trigger = 3;
  options.level0_slowdown_writes_trigger = 3;
  options.level0_stop_writes_trigger = 999999;
  options.delayed_write_rate = 200000;
  options.soft_rate_limit = 1.1;
  options.target_file_size_base = 99999999;
  options.max_bytes_for_level_base = 50000;
  options.compression = kNoCompression;
  Reopen(options);
  Put(Key(0), "");
  port::Mutex mut;
  port::CondVar cv(&mut);
  std::atomic<int> compaction_cnt(0);
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "VersionSet::LogAndApply:WriteManifest", [&](void* arg) {
        MutexLock l(&mut);
        while (compaction_cnt.load() >= 8) {
          cv.Wait();
        }
        compaction_cnt.fetch_add(1);
      });
  std::atomic<int> sleep_count(0);
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::DelayWrite:Sleep", [&](void* arg) { sleep_count.fetch_add(1); });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  for (int i = 0; i < 3; i++) {
    Put(Key(i), std::string(5000, 'x'));
    Put(Key(100 - i), std::string(5000, 'x'));
    Flush();
  }
  while (compaction_cnt.load() < 4 || NumTableFilesAtLevel(0) > 0) {
    env_->SleepForMicroseconds(1000);
  }
  ASSERT_EQ(NumTableFilesAtLevel(1), 1);
  ASSERT_EQ(sleep_count.load(), 0);
  for (int i = 0; i < 3; i++) {
    Put(Key(10 + i), std::string(5000, 'x'));
    Put(Key(90 - i), std::string(5000, 'x'));
    Flush();
  }
  while (compaction_cnt.load() < 8 || NumTableFilesAtLevel(0) > 0) {
    env_->SleepForMicroseconds(1000);
  }
  ASSERT_EQ(NumTableFilesAtLevel(1), 1);
  ASSERT_EQ(sleep_count.load(), 0);
  for (int i = 0; i < 10; i++) {
    Put(Key(i), std::string(100, 'x'));
  }
  ASSERT_GT(sleep_count.load(), 0);
  {
    MutexLock l(&mut);
    compaction_cnt.store(7);
    cv.SignalAll();
  }
  while (NumTableFilesAtLevel(1) > 0) {
    env_->SleepForMicroseconds(1000);
  }
  sleep_count.store(0);
  for (int i = 0; i < 10; i++) {
    Put(Key(i), std::string(100, 'x'));
  }
  ASSERT_EQ(sleep_count.load(), 0);
  ASSERT_OK(dbfull()->SetOptions({
      {"max_bytes_for_level_base", "5000"},
  }));
  compaction_cnt.store(7);
  Flush();
  while (NumTableFilesAtLevel(0) == 0) {
    env_->SleepForMicroseconds(1000);
  }
  for (int i = 0; i < 10; i++) {
    Put(Key(i), std::string(100, 'x'));
  }
  ASSERT_GT(sleep_count.load(), 0);
  {
    MutexLock l(&mut);
    compaction_cnt.store(7);
    cv.SignalAll();
  }
  while (NumTableFilesAtLevel(2) != 0) {
    env_->SleepForMicroseconds(1000);
  }
  sleep_count.store(0);
  for (int i = 0; i < 10; i++) {
    Put(Key(i), std::string(100, 'x'));
  }
  ASSERT_EQ(sleep_count.load(), 0);
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}
TEST_F(DBTest, ForceBottommostLevelCompaction) {
  int32_t trivial_move = 0;
  int32_t non_trivial_move = 0;
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:TrivialMove",
      [&](void* arg) { trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::BackgroundCompaction:NonTrivial",
      [&](void* arg) { non_trivial_move++; });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  Options options;
  options.write_buffer_size = 100000000;
  options = CurrentOptions(options);
  DestroyAndReopen(options);
  int32_t value_size = 10 * 1024;
  Random rnd(301);
  std::vector<std::string> values;
  for (int i = 0; i < 100; i++) {
    values.push_back(RandomString(&rnd, value_size));
    ASSERT_OK(Put(Key(i), values[i]));
  }
  ASSERT_OK(Flush());
  ASSERT_EQ("1", FilesPerLevel(0));
  CompactRangeOptions compact_options;
  compact_options.change_level = true;
  compact_options.target_level = 3;
  ASSERT_OK(db_->CompactRange(compact_options, nullptr, nullptr));
  ASSERT_EQ("0,0,0,1", FilesPerLevel(0));
  ASSERT_EQ(trivial_move, 1);
  ASSERT_EQ(non_trivial_move, 0);
  for (int i = 100; i < 200; i++) {
    values.push_back(RandomString(&rnd, value_size));
    ASSERT_OK(Put(Key(i), values[i]));
  }
  ASSERT_OK(Flush());
  ASSERT_EQ("1,0,0,1", FilesPerLevel(0));
  compact_options = CompactRangeOptions();
  compact_options.bottommost_level_compaction =
      BottommostLevelCompaction::kForce;
  ASSERT_OK(db_->CompactRange(compact_options, nullptr, nullptr));
  ASSERT_EQ("0,0,0,1", FilesPerLevel(0));
  ASSERT_EQ(trivial_move, 4);
  ASSERT_EQ(non_trivial_move, 1);
  for (int i = 200; i < 300; i++) {
    values.push_back(RandomString(&rnd, value_size));
    ASSERT_OK(Put(Key(i), values[i]));
  }
  ASSERT_OK(Flush());
  ASSERT_EQ("1,0,0,1", FilesPerLevel(0));
  trivial_move = 0;
  non_trivial_move = 0;
  compact_options = CompactRangeOptions();
  compact_options.bottommost_level_compaction =
      BottommostLevelCompaction::kSkip;
  ASSERT_OK(db_->CompactRange(compact_options, nullptr, nullptr));
  ASSERT_EQ("0,0,0,2", FilesPerLevel(0));
  ASSERT_EQ(trivial_move, 3);
  ASSERT_EQ(non_trivial_move, 0);
  for (int i = 0; i < 300; i++) {
    ASSERT_EQ(Get(Key(i)), values[i]);
  }
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}
TEST_F(DBTest, FailWhenCompressionNotSupportedTest) {
  CompressionType compressions[] = {kZlibCompression, kBZip2Compression,
                                    kLZ4Compression, kLZ4HCCompression};
  for (int iter = 0; iter < 4; ++iter) {
    if (!CompressionTypeSupported(compressions[iter])) {
      Options options = CurrentOptions();
      options.compression = compressions[iter];
      ASSERT_TRUE(!TryReopen(options).ok());
      options.compression = kNoCompression;
      ASSERT_OK(TryReopen(options));
      ColumnFamilyOptions cf_options(options);
      cf_options.compression = compressions[iter];
      ColumnFamilyHandle* handle;
      ASSERT_TRUE(!db_->CreateColumnFamily(cf_options, "name", &handle).ok());
    }
  }
}
TEST_F(DBTest, RowCache) {
  Options options = CurrentOptions();
  options.statistics = rocksdb::CreateDBStatistics();
  options.row_cache = NewLRUCache(8192);
  DestroyAndReopen(options);
  ASSERT_OK(Put("foo", "bar"));
  ASSERT_OK(Flush());
  ASSERT_EQ(TestGetTickerCount(options, ROW_CACHE_HIT), 0);
  ASSERT_EQ(TestGetTickerCount(options, ROW_CACHE_MISS), 0);
  ASSERT_EQ(Get("foo"), "bar");
  ASSERT_EQ(TestGetTickerCount(options, ROW_CACHE_HIT), 0);
  ASSERT_EQ(TestGetTickerCount(options, ROW_CACHE_MISS), 1);
  ASSERT_EQ(Get("foo"), "bar");
  ASSERT_EQ(TestGetTickerCount(options, ROW_CACHE_HIT), 1);
  ASSERT_EQ(TestGetTickerCount(options, ROW_CACHE_MISS), 1);
}
TEST_F(DBTest, DISABLED_PrevAfterMerge) {
  Options options;
  options.create_if_missing = true;
  options.merge_operator = MergeOperators::CreatePutOperator();
  DestroyAndReopen(options);
  WriteOptions wopts;
  db_->Merge(wopts, "1", "data1");
  db_->Merge(wopts, "2", "data2");
  db_->Merge(wopts, "3", "data3");
  std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions()));
  it->Seek("2");
  ASSERT_TRUE(it->Valid());
  ASSERT_EQ("2", it->key().ToString());
  it->Prev();
  ASSERT_TRUE(it->Valid());
  ASSERT_EQ("1", it->key().ToString());
}
TEST_F(DBTest, DeletingOldWalAfterDrop) {
  rocksdb::SyncPoint::GetInstance()->LoadDependency(
      { { "Test:AllowFlushes", "DBImpl::BGWorkFlush" },
        { "DBImpl::BGWorkFlush:done", "Test:WaitForFlush"} });
  rocksdb::SyncPoint::GetInstance()->ClearTrace();
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
  Options options = CurrentOptions();
  options.max_total_wal_size = 8192;
  options.compression = kNoCompression;
  options.write_buffer_size = 1 << 20;
  options.level0_file_num_compaction_trigger = (1<<30);
  options.level0_slowdown_writes_trigger = (1<<30);
  options.level0_stop_writes_trigger = (1<<30);
  options.disable_auto_compactions = true;
  DestroyAndReopen(options);
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  CreateColumnFamilies({"cf1", "cf2"}, options);
  ASSERT_OK(Put(0, "key1", DummyString(8192)));
  ASSERT_OK(Put(0, "key2", DummyString(8192)));
  ASSERT_OK(db_->DropColumnFamily(handles_[0]));
  TEST_SYNC_POINT("Test:AllowFlushes");
  TEST_SYNC_POINT("Test:WaitForFlush");
  uint64_t lognum1 = dbfull()->TEST_LogfileNumber();
  ASSERT_OK(Put(1, "key3", DummyString(8192)));
  ASSERT_OK(Put(1, "key4", DummyString(8192)));
  uint64_t lognum2 = dbfull()->TEST_LogfileNumber();
  EXPECT_GT(lognum2, lognum1);
}
}
#endif
int main(int argc, char** argv) {
#if !(defined NDEBUG) || !defined(OS_WIN)
  rocksdb::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
#else
  return 0;
#endif
}
