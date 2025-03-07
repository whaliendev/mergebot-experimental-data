//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// The test uses an array to compare against values written to the database.
// Keys written to the array are in 1:1 correspondence to the actual values in
// the database according to the formula in the function GenerateValue.

// Space is reserved in the array from 0 to FLAGS_max_key and values are
// randomly written/deleted/read from those positions. During verification we
// compare all the positions in the array. To shorten/elongate the running
// time, you could change the settings: FLAGS_max_key, FLAGS_ops_per_thread,
// (sometimes also FLAGS_threads).
//
// NOTE that if FLAGS_test_batches_snapshots is set, the test will have
// different behavior. See comment of the flag for details.

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <gflags/gflags.h>
#include "db/db_impl.h"
#include "db/version_set.h"
#include "rocksdb/statistics.h"
#include "rocksdb/cache.h"
#include "utilities/utility_db.h"
#include "rocksdb/env.h"
#include "rocksdb/write_batch.h"
#include "rocksdb/slice.h"
#include "rocksdb/slice_transform.h"
#include "rocksdb/statistics.h"
#include "port/port.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/histogram.h"
#include "util/mutexlock.h"
#include "util/random.h"
#include "util/testutil.h"
#include "util/logging.h"
#include "utilities/ttl/db_ttl.h"
#include "hdfs/env_hdfs.h"
#include "utilities/merge_operators.h"

static const long KB = 1024;


static bool ValidateUint32Range(const char* flagname, uint64_t value) {
  if (value > std::numeric_limits<uint32_t>::max()) {
    fprintf(stderr,
            "Invalid value for --%s: %lu, overflow\n",
            flagname,
            (unsigned long)value);
    return false;
  }
  return true;
}
DEFINE_uint64(seed, 2341234, "Seed for PRNG");
static const bool FLAGS_seed_dummy __attribute__((unused)) =
    google::RegisterFlagValidator(&FLAGS_seed, &ValidateUint32Range);

DEFINE_int64(max_key, 1 * KB* KB,
             "Max number of key/values to place in database");

DEFINE_int32(column_families, 10, "Number of column families");

DEFINE_bool(test_batches_snapshots, false,
            "If set, the test uses MultiGet(), Multiut() and MultiDelete()"
            " which read/write/delete multiple keys in a batch. In this mode,"
            " we do not verify db content by comparing the content with the "
            "pre-allocated array. Instead, we do partial verification inside"
            " MultiGet() by checking various values in a batch. Benefit of"
            " this mode:\n"
            "\t(a) No need to acquire mutexes during writes (less cache "
            "flushes in multi-core leading to speed up)\n"
            "\t(b) No long validation at the end (more speed up)\n"
            "\t(c) Test snapshot and atomicity of batch writes");

DEFINE_int32(threads, 32, "Number of concurrent threads to run.");

DEFINE_int32(ttl, -1,
             "Opens the db with this ttl value if this is not -1. "
             "Carefully specify a large value such that verifications on "
             "deleted values don't fail");

DEFINE_int32(value_size_mult, 8,
             "Size of value will be this number times rand_int(1,3) bytes");

DEFINE_bool(verify_before_write, false, "Verify before write");

DEFINE_bool(histogram, false, "Print histogram of operation timings");

DEFINE_bool(destroy_db_initially, true,
            "Destroys the database dir before start if this is true");

DEFINE_bool (verbose, false, "Verbose");

DEFINE_int32(write_buffer_size, rocksdb::Options().write_buffer_size,
             "Number of bytes to buffer in memtable before compacting");

DEFINE_int32(max_write_buffer_number,
             rocksdb::Options().max_write_buffer_number,
             "The number of in-memory memtables. "
             "Each memtable is of size FLAGS_write_buffer_size.");

DEFINE_int32(min_write_buffer_number_to_merge,
             rocksdb::Options().min_write_buffer_number_to_merge,
             "The minimum number of write buffers that will be merged together "
             "before writing to storage. This is cheap because it is an "
             "in-memory merge. If this feature is not enabled, then all these "
             "write buffers are flushed to L0 as separate files and this "
             "increases read amplification because a get request has to check "
             "in all of these files. Also, an in-memory merge may result in "
             "writing less data to storage if there are duplicate records in"
             " each of these individual write buffers.");

DEFINE_int32(open_files, rocksdb::Options().max_open_files,
             "Maximum number of files to keep open at the same time "
             "(use default if == 0)");

DEFINE_int64(compressed_cache_size, -1,
             "Number of bytes to use as a cache of compressed data."
             " Negative means use default settings.");

DEFINE_int32(compaction_style, rocksdb::Options().compaction_style, "");

DEFINE_int32(level0_file_num_compaction_trigger,
             rocksdb::Options().level0_file_num_compaction_trigger,
             "Level0 compaction start trigger");

DEFINE_int32(level0_slowdown_writes_trigger,
             rocksdb::Options().level0_slowdown_writes_trigger,
             "Number of files in level-0 that will slow down writes");

DEFINE_int32(level0_stop_writes_trigger,
             rocksdb::Options().level0_stop_writes_trigger,
             "Number of files in level-0 that will trigger put stop.");

DEFINE_int32(block_size, rocksdb::Options().block_size,
             "Number of bytes in a block.");

DEFINE_int32(max_background_compactions,
             rocksdb::Options().max_background_compactions,
             "The maximum number of concurrent background compactions "
             "that can occur in parallel.");

DEFINE_int32(max_background_flushes, rocksdb::Options().max_background_flushes,
             "The maximum number of concurrent background flushes "
             "that can occur in parallel.");

DEFINE_int32(universal_size_ratio, 0, "The ratio of file sizes that trigger"
             " compaction in universal style");

DEFINE_int32(universal_min_merge_width, 0, "The minimum number of files to "
             "compact in universal style compaction");

DEFINE_int32(universal_max_merge_width, 0, "The max number of files to compact"
             " in universal style compaction");

DEFINE_int32(universal_max_size_amplification_percent, 0,
             "The max size amplification for universal style compaction");

DEFINE_int32(clear_column_family_one_in, 1000000,
             "With a chance of 1/N, delete a column family and then recreate "
             "it again. If N == 0, never drop/create column families. "
             "When test_batches_snapshots is true, this flag has no effect");

DEFINE_int64(cache_size, 2 * KB * KB * KB,
             "Number of bytes to use as a cache of uncompressed data.");

static bool ValidateInt32Positive(const char* flagname, int32_t value) {
  if (value < 0) {
    fprintf(stderr, "Invalid value for --%s: %d, must be >=0\n",
            flagname, value);
    return false;
  }
  return true;
}
DEFINE_int32(reopen, 10, "Number of times database reopens");
static const bool FLAGS_reopen_dummy __attribute__((unused)) =
    google::RegisterFlagValidator(&FLAGS_reopen, &ValidateInt32Positive);

DEFINE_int32(bloom_bits, 10, "Bloom filter bits per key. "
             "Negative means use default settings.");

DEFINE_string(db, "", "Use the db with the following name.");

DEFINE_bool(verify_checksum, false,
            "Verify checksum for every block read from storage");

DEFINE_bool(mmap_read, rocksdb::EnvOptions().use_mmap_reads,
            "Allow reads to occur via mmap-ing files");

// Database statistics
static std::shared_ptr<rocksdb::Statistics> dbstats;
DEFINE_bool(statistics, false, "Create database statistics");

DEFINE_bool(sync, false, "Sync all writes to disk");

DEFINE_bool(disable_data_sync, false,
            "If true, do not wait until data is synced to disk.");

DEFINE_bool(use_fsync, false, "If true, issue fsync instead of fdatasync");

DEFINE_int32(kill_random_test, 0,
             "If non-zero, kill at various points in source code with "
             "probability 1/this");
static const bool FLAGS_kill_random_test_dummy __attribute__((unused)) =
    google::RegisterFlagValidator(&FLAGS_kill_random_test,
                                  &ValidateInt32Positive);
extern int rocksdb_kill_odds;

DEFINE_bool(disable_wal, false, "If true, do not write WAL for write.");

DEFINE_int32(target_file_size_base, 64 * KB,
             "Target level-1 file size for compaction");

DEFINE_int32(target_file_size_multiplier, 1,
             "A multiplier to compute targe level-N file size (N >= 2)");

DEFINE_uint64(max_bytes_for_level_base, 256 * KB, "Max bytes for level-1");

DEFINE_int32(max_bytes_for_level_multiplier, 2,
             "A multiplier to compute max bytes for level-N (N >= 2)");

static bool ValidateInt32Percent(const char* flagname, int32_t value) {
  if (value < 0 || value>100) {
    fprintf(stderr, "Invalid value for --%s: %d, 0<= pct <=100 \n",
            flagname, value);
    return false;
  }
  return true;
}
DEFINE_int32(readpercent, 10,
             "Ratio of reads to total workload (expressed as a percentage)");
static const bool FLAGS_readpercent_dummy __attribute__((unused)) =
    google::RegisterFlagValidator(&FLAGS_readpercent, &ValidateInt32Percent);

DEFINE_int32(prefixpercent, 20,
             "Ratio of prefix iterators to total workload (expressed as a"
             " percentage)");
static const bool FLAGS_prefixpercent_dummy __attribute__((unused)) =
    google::RegisterFlagValidator(&FLAGS_prefixpercent, &ValidateInt32Percent);

DEFINE_int32(writepercent, 45,
             " Ratio of deletes to total workload (expressed as a percentage)");
static const bool FLAGS_writepercent_dummy __attribute__((unused)) =
    google::RegisterFlagValidator(&FLAGS_writepercent, &ValidateInt32Percent);

DEFINE_int32(delpercent, 15,
             "Ratio of deletes to total workload (expressed as a percentage)");
static const bool FLAGS_delpercent_dummy __attribute__((unused)) =
    google::RegisterFlagValidator(&FLAGS_delpercent, &ValidateInt32Percent);

DEFINE_int32(iterpercent, 10, "Ratio of iterations to total workload"
             " (expressed as a percentage)");
static const bool FLAGS_iterpercent_dummy __attribute__((unused)) =
    google::RegisterFlagValidator(&FLAGS_iterpercent, &ValidateInt32Percent);

DEFINE_uint64(num_iterations, 10, "Number of iterations per MultiIterate run");
static const bool FLAGS_num_iterations_dummy __attribute__((unused)) =
    google::RegisterFlagValidator(&FLAGS_num_iterations, &ValidateUint32Range);

DEFINE_bool(disable_seek_compaction, false,
            "Option to disable compation triggered by read.");

enum rocksdb::CompressionType StringToCompressionType(const char* ctype) {
  assert(ctype);

  if (!strcasecmp(ctype, "none"))
    return rocksdb::kNoCompression;
  else if (!strcasecmp(ctype, "snappy"))
    return rocksdb::kSnappyCompression;
  else if (!strcasecmp(ctype, "zlib"))
    return rocksdb::kZlibCompression;
  else if (!strcasecmp(ctype, "bzip2"))
    return rocksdb::kBZip2Compression;
  else if (!strcasecmp(ctype, "lz4"))
    return rocksdb::kLZ4Compression;
  else if (!strcasecmp(ctype, "lz4hc"))
    return rocksdb::kLZ4HCCompression;

  fprintf(stdout, "Cannot parse compression type '%s'\n", ctype);
  return rocksdb::kSnappyCompression; //default value
}
DEFINE_string(compression_type, "snappy",
              "Algorithm to use to compress the database");
static enum rocksdb::CompressionType FLAGS_compression_type_e =
    rocksdb::kSnappyCompression;

DEFINE_string(hdfs, "", "Name of hdfs environment");
// posix or hdfs environment
static rocksdb::Env* FLAGS_env = rocksdb::Env::Default();

DEFINE_uint64(ops_per_thread, 1200000, "Number of operations per thread.");
static const bool FLAGS_ops_per_thread_dummy __attribute__((unused)) =
    google::RegisterFlagValidator(&FLAGS_ops_per_thread, &ValidateUint32Range);

DEFINE_uint64(log2_keys_per_lock, 2, "Log2 of number of keys per lock");
static const bool FLAGS_log2_keys_per_lock_dummy __attribute__((unused)) =
    google::RegisterFlagValidator(&FLAGS_log2_keys_per_lock,
                                  &ValidateUint32Range);

DEFINE_int32(purge_redundant_percent, 50,
             "Percentage of times we want to purge redundant keys in memory "
             "before flushing");
static const bool FLAGS_purge_redundant_percent_dummy __attribute__((unused)) =
    google::RegisterFlagValidator(&FLAGS_purge_redundant_percent,
                                  &ValidateInt32Percent);

DEFINE_bool(filter_deletes, false, "On true, deletes use KeyMayExist to drop"
            " the delete if key not present");

enum RepFactory {
  kSkipList,
  kHashSkipList,
  kVectorRep
};
enum RepFactory StringToRepFactory(const char* ctype) {
  assert(ctype);

  if (!strcasecmp(ctype, "skip_list"))
    return kSkipList;
  else if (!strcasecmp(ctype, "prefix_hash"))
    return kHashSkipList;
  else if (!strcasecmp(ctype, "vector"))
    return kVectorRep;

  fprintf(stdout, "Cannot parse memreptable %s\n", ctype);
  return kSkipList;
}
static enum RepFactory FLAGS_rep_factory;
DEFINE_string(memtablerep, "prefix_hash", "");

static bool ValidatePrefixSize(const char* flagname, int32_t value) {
  if (value < 0 || value > 8) {
    fprintf(stderr, "Invalid value for --%s: %d. 0 <= PrefixSize <= 8\n",
            flagname, value);
    return false;
  }
  return true;
}
DEFINE_int32(prefix_size, 7, "Control the prefix size for HashSkipListRep");
static const bool FLAGS_prefix_size_dummy =
  google::RegisterFlagValidator(&FLAGS_prefix_size, &ValidatePrefixSize);

DEFINE_bool(use_merge, false, "On true, replaces all writes with a Merge "
            "that behaves like a Put");


namespace rocksdb {

// convert long to a big-endian slice key
static std::string Key(long val) {
  std::string little_endian_key;
  std::string big_endian_key;
  PutFixed64(&little_endian_key, val);
  assert(little_endian_key.size() == sizeof(val));
  big_endian_key.resize(sizeof(val));
  for (int i=0; i<(int)sizeof(val); i++) {
    big_endian_key[i] = little_endian_key[sizeof(val) - 1 - i];
  }
  return big_endian_key;
}

class StressTest;
namespace {

class Stats {
 private:
  double start_;
  double finish_;
  double seconds_;
  long done_;
  long gets_;
  long prefixes_;
  long writes_;
  long deletes_;
  long iterator_size_sums_;
  long founds_;
  long iterations_;
  long errors_;
  int next_report_;
  size_t bytes_;
  double last_op_finish_;
  HistogramImpl hist_;

 public:
  Stats() { }

  void Start() {
    next_report_ = 100;
    hist_.Clear();
    done_ = 0;
    gets_ = 0;
    prefixes_ = 0;
    writes_ = 0;
    deletes_ = 0;
    iterator_size_sums_ = 0;
    founds_ = 0;
    iterations_ = 0;
    errors_ = 0;
    bytes_ = 0;
    seconds_ = 0;
    start_ = FLAGS_env->NowMicros();
    last_op_finish_ = start_;
    finish_ = start_;
  }

  void Merge(const Stats& other) {
    hist_.Merge(other.hist_);
    done_ += other.done_;
    gets_ += other.gets_;
    prefixes_ += other.prefixes_;
    writes_ += other.writes_;
    deletes_ += other.deletes_;
    iterator_size_sums_ += other.iterator_size_sums_;
    founds_ += other.founds_;
    iterations_ += other.iterations_;
    errors_ += other.errors_;
    bytes_ += other.bytes_;
    seconds_ += other.seconds_;
    if (other.start_ < start_) start_ = other.start_;
    if (other.finish_ > finish_) finish_ = other.finish_;
  }

  void Stop() {
    finish_ = FLAGS_env->NowMicros();
    seconds_ = (finish_ - start_) * 1e-6;
  }

  void FinishedSingleOp() {
    if (FLAGS_histogram) {
      double now = FLAGS_env->NowMicros();
      double micros = now - last_op_finish_;
      hist_.Add(micros);
      if (micros > 20000) {
        fprintf(stdout, "long op: %.1f micros%30s\r", micros, "");
      }
      last_op_finish_ = now;
    }

    done_++;
    if (done_ >= next_report_) {
      if      (next_report_ < 1000)   next_report_ += 100;
      else if (next_report_ < 5000)   next_report_ += 500;
      else if (next_report_ < 10000)  next_report_ += 1000;
      else if (next_report_ < 50000)  next_report_ += 5000;
      else if (next_report_ < 100000) next_report_ += 10000;
      else if (next_report_ < 500000) next_report_ += 50000;
      else                            next_report_ += 100000;
      fprintf(stdout, "... finished %ld ops%30s\r", done_, "");
    }
  }

  void AddBytesForWrites(int nwrites, size_t nbytes) {
    writes_ += nwrites;
    bytes_ += nbytes;
  }

  void AddGets(int ngets, int nfounds) {
    founds_ += nfounds;
    gets_ += ngets;
  }

  void AddPrefixes(int nprefixes, int count) {
    prefixes_ += nprefixes;
    iterator_size_sums_ += count;
  }

  void AddIterations(int n) {
    iterations_ += n;
  }

  void AddDeletes(int n) {
    deletes_ += n;
  }

  void AddErrors(int n) {
    errors_ += n;
  }

  void Report(const char* name) {
    std::string extra;
    if (bytes_ < 1 || done_ < 1) {
      fprintf(stderr, "No writes or ops?\n");
      return;
    }

    double elapsed = (finish_ - start_) * 1e-6;
    double bytes_mb = bytes_ / 1048576.0;
    double rate = bytes_mb / elapsed;
    double throughput = (double)done_/elapsed;

    fprintf(stdout, "%-12s: ", name);
    fprintf(stdout, "%.3f micros/op %ld ops/sec\n",
            seconds_ * 1e6 / done_, (long)throughput);
    fprintf(stdout, "%-12s: Wrote %.2f MB (%.2f MB/sec) (%ld%% of %ld ops)\n",
            "", bytes_mb, rate, (100*writes_)/done_, done_);
    fprintf(stdout, "%-12s: Wrote %ld times\n", "", writes_);
    fprintf(stdout, "%-12s: Deleted %ld times\n", "", deletes_);
    fprintf(stdout, "%-12s: %ld read and %ld found the key\n", "",
            gets_, founds_);
    fprintf(stdout, "%-12s: Prefix scanned %ld times\n", "", prefixes_);
    fprintf(stdout, "%-12s: Iterator size sum is %ld\n", "",
            iterator_size_sums_);
    fprintf(stdout, "%-12s: Iterated %ld times\n", "", iterations_);
    fprintf(stdout, "%-12s: Got errors %ld times\n", "", errors_);

    if (FLAGS_histogram) {
      fprintf(stdout, "Microseconds per op:\n%s\n", hist_.ToString().c_str());
    }
    fflush(stdout);
  }
};

// State shared by all concurrent executions of the same benchmark.
class SharedState {
 public:
  static const uint32_t SENTINEL;

  explicit SharedState(StressTest* stress_test) :
      cv_(&mu_),
      seed_(FLAGS_seed),
      max_key_(FLAGS_max_key),
      log2_keys_per_lock_(FLAGS_log2_keys_per_lock),
      num_threads_(FLAGS_threads),
      num_initialized_(0),
      num_populated_(0),
      vote_reopen_(0),
      num_done_(0),
      start_(false),
      start_verify_(false),
      stress_test_(stress_test) {
    if (FLAGS_test_batches_snapshots) {
      fprintf(stdout, "No lock creation because test_batches_snapshots set\n");
      return;
    }
    values_.resize(FLAGS_column_families);

    for (int i = 0; i < FLAGS_column_families; ++i) {
      values_[i] = std::vector<uint32_t>(max_key_, SENTINEL);
    }

    long num_locks = (max_key_ >> log2_keys_per_lock_);
    if (max_key_ & ((1 << log2_keys_per_lock_) - 1)) {
      num_locks++;
    }
    fprintf(stdout, "Creating %ld locks\n", num_locks * FLAGS_column_families);
    key_locks_.resize(FLAGS_column_families);
    for (int i = 0; i < FLAGS_column_families; ++i) {
      key_locks_[i] = std::vector<port::Mutex>(num_locks);
    }
  }

  ~SharedState() {}

  port::Mutex* GetMutex() {
    return &mu_;
  }

  port::CondVar* GetCondVar() {
    return &cv_;
  }

  StressTest* GetStressTest() const {
    return stress_test_;
  }

  long GetMaxKey() const {
    return max_key_;
  }

  uint32_t GetNumThreads() const {
    return num_threads_;
  }

  void IncInitialized() {
    num_initialized_++;
  }

  void IncOperated() {
    num_populated_++;
  }

  void IncDone() {
    num_done_++;
  }

  void IncVotedReopen() {
    vote_reopen_ = (vote_reopen_ + 1) % num_threads_;
  }

  bool AllInitialized() const {
    return num_initialized_ >= num_threads_;
  }

  bool AllOperated() const {
    return num_populated_ >= num_threads_;
  }

  bool AllDone() const {
    return num_done_ >= num_threads_;
  }

  bool AllVotedReopen() {
    return (vote_reopen_ == 0);
  }

  void SetStart() {
    start_ = true;
  }

  void SetStartVerify() {
    start_verify_ = true;
  }

  bool Started() const {
    return start_;
  }

  bool VerifyStarted() const {
    return start_verify_;
  }

  port::Mutex* GetMutexForKey(int cf, long key) {
    return &key_locks_[cf][key >> log2_keys_per_lock_];
  }

  void LockColumnFamily(int cf) {
    for (auto& mutex : key_locks_[cf]) {
      mutex.Lock();
    }
  }

  void UnlockColumnFamily(int cf) {
    for (auto& mutex : key_locks_[cf]) {
      mutex.Unlock();
    }
  }

  void ClearColumnFamily(int cf) {
    std::fill(values_[cf].begin(), values_[cf].end(), SENTINEL);
  }

  void Put(int cf, long key, uint32_t value_base) {
    values_[cf][key] = value_base;
  }

  uint32_t Get(int cf, long key) const { return values_[cf][key]; }

  void Delete(int cf, long key) { values_[cf][key] = SENTINEL; }

  uint32_t GetSeed() const { return seed_; }

 private:
  port::Mutex mu_;
  port::CondVar cv_;
  const uint32_t seed_;
  const long max_key_;
  const uint32_t log2_keys_per_lock_;
  const int num_threads_;
  long num_initialized_;
  long num_populated_;
  long vote_reopen_;
  long num_done_;
  bool start_;
  bool start_verify_;
  StressTest* stress_test_;

  std::vector<std::vector<uint32_t>> values_;
  std::vector<std::vector<port::Mutex>> key_locks_;
};

const uint32_t SharedState::SENTINEL = 0xffffffff;

// Per-thread state for concurrent executions of the same benchmark.
struct ThreadState {
  uint32_t tid; // 0..n-1
  Random rand;  // Has different seeds for different threads
  SharedState* shared;
  Stats stats;

  ThreadState(uint32_t index, SharedState *shared)
      : tid(index),
        rand(1000 + index + shared->GetSeed()),
        shared(shared) {
  }
};

}  // namespace

class StressTest {
 public:
  StressTest()
      : cache_(NewLRUCache(FLAGS_cache_size)),
        compressed_cache_(FLAGS_compressed_cache_size >= 0
                              ? NewLRUCache(FLAGS_compressed_cache_size)
                              : nullptr),
        filter_policy_(FLAGS_bloom_bits >= 0
                           ? NewBloomFilterPolicy(FLAGS_bloom_bits)
                           : nullptr),
        db_(nullptr),
        new_column_family_name_(0),
        num_times_reopened_(0) {
    if (FLAGS_destroy_db_initially) {
      std::vector<std::string> files;
      FLAGS_env->GetChildren(FLAGS_db, &files);
      for (unsigned int i = 0; i < files.size(); i++) {
        if (Slice(files[i]).starts_with("heap-")) {
          FLAGS_env->DeleteFile(FLAGS_db + "/" + files[i]);
        }
      }
      DestroyDB(FLAGS_db, Options());
    }
  }

  ~StressTest() {
    for (auto cf : column_families_) {
      delete cf;
    }
    column_families_.clear();
    delete db_;
    delete filter_policy_;
  }

  void Run() {
    PrintEnv();
    Open();
    SharedState shared(this);
    uint32_t n = shared.GetNumThreads();

    std::vector<ThreadState*> threads(n);
    for (uint32_t i = 0; i < n; i++) {
      threads[i] = new ThreadState(i, &shared);
      FLAGS_env->StartThread(ThreadBody, threads[i]);
    }
    // Each thread goes through the following states:
    // initializing -> wait for others to init -> read/populate/depopulate
    // wait for others to operate -> verify -> done

    {
      MutexLock l(shared.GetMutex());
      while (!shared.AllInitialized()) {
        shared.GetCondVar()->Wait();
      }

      double now = FLAGS_env->NowMicros();
      fprintf(stdout, "%s Starting database operations\n",
              FLAGS_env->TimeToString((uint64_t) now/1000000).c_str());

      shared.SetStart();
      shared.GetCondVar()->SignalAll();
      while (!shared.AllOperated()) {
        shared.GetCondVar()->Wait();
      }

      now = FLAGS_env->NowMicros();
      if (FLAGS_test_batches_snapshots) {
        fprintf(stdout, "%s Limited verification already done during gets\n",
                FLAGS_env->TimeToString((uint64_t) now/1000000).c_str());
      } else {
        fprintf(stdout, "%s Starting verification\n",
                FLAGS_env->TimeToString((uint64_t) now/1000000).c_str());
      }

      shared.SetStartVerify();
      shared.GetCondVar()->SignalAll();
      while (!shared.AllDone()) {
        shared.GetCondVar()->Wait();
      }
    }

    for (unsigned int i = 1; i < n; i++) {
      threads[0]->stats.Merge(threads[i]->stats);
    }
    threads[0]->stats.Report("Stress Test");

    for (unsigned int i = 0; i < n; i++) {
      delete threads[i];
      threads[i] = nullptr;
    }
    double now = FLAGS_env->NowMicros();
    if (!FLAGS_test_batches_snapshots) {
      fprintf(stdout, "%s Verification successful\n",
              FLAGS_env->TimeToString((uint64_t) now/1000000).c_str());
    }
    PrintStatistics();
  }

 private:

  static void ThreadBody(void* v) {
    ThreadState* thread = reinterpret_cast<ThreadState*>(v);
    SharedState* shared = thread->shared;

    {
      MutexLock l(shared->GetMutex());
      shared->IncInitialized();
      if (shared->AllInitialized()) {
        shared->GetCondVar()->SignalAll();
      }
      while (!shared->Started()) {
        shared->GetCondVar()->Wait();
      }
    }
    thread->shared->GetStressTest()->OperateDb(thread);

    {
      MutexLock l(shared->GetMutex());
      shared->IncOperated();
      if (shared->AllOperated()) {
        shared->GetCondVar()->SignalAll();
      }
      while (!shared->VerifyStarted()) {
        shared->GetCondVar()->Wait();
      }
    }

    if (!FLAGS_test_batches_snapshots) {
      thread->shared->GetStressTest()->VerifyDb(thread);
    }

    {
      MutexLock l(shared->GetMutex());
      shared->IncDone();
      if (shared->AllDone()) {
        shared->GetCondVar()->SignalAll();
      }
    }

  }

  // Given a key K and value V, this puts ("0"+K, "0"+V), ("1"+K, "1"+V), ...
  // ("9"+K, "9"+V) in DB atomically i.e in a single batch.
  // Also refer MultiGet.
  Status MultiPut(ThreadState* thread, const WriteOptions& writeoptions,
                  ColumnFamilyHandle* column_family, const Slice& key,
                  const Slice& value, size_t sz) {
    std::string keys[10] = {"9", "8", "7", "6", "5",
                            "4", "3", "2", "1", "0"};
    std::string values[10] = {"9", "8", "7", "6", "5",
                              "4", "3", "2", "1", "0"};
    Slice value_slices[10];
    WriteBatch batch;
    Status s;
    for (int i = 0; i < 10; i++) {
      keys[i] += key.ToString();
      values[i] += value.ToString();
      value_slices[i] = values[i];
      if (FLAGS_use_merge) {
        batch.Merge(column_family, keys[i], value_slices[i]);
      } else {
        batch.Put(column_family, keys[i], value_slices[i]);
      }
    }

    s = db_->Write(writeoptions, &batch);
    if (!s.ok()) {
      fprintf(stderr, "multiput error: %s\n", s.ToString().c_str());
      thread->stats.AddErrors(1);
    } else {
      // we did 10 writes each of size sz + 1
      thread->stats.AddBytesForWrites(10, (sz + 1) * 10);
    }

    return s;
  }

  // Given a key K, this deletes ("0"+K), ("1"+K),... ("9"+K)
  // in DB atomically i.e in a single batch. Also refer MultiGet.
  Status MultiDelete(ThreadState* thread, const WriteOptions& writeoptions,
                     ColumnFamilyHandle* column_family, const Slice& key) {
    std::string keys[10] = {"9", "7", "5", "3", "1",
                            "8", "6", "4", "2", "0"};

    WriteBatch batch;
    Status s;
    for (int i = 0; i < 10; i++) {
      keys[i] += key.ToString();
      batch.Delete(column_family, keys[i]);
    }

    s = db_->Write(writeoptions, &batch);
    if (!s.ok()) {
      fprintf(stderr, "multidelete error: %s\n", s.ToString().c_str());
      thread->stats.AddErrors(1);
    } else {
      thread->stats.AddDeletes(10);
    }

    return s;
  }

  // Given a key K, this gets values for "0"+K, "1"+K,..."9"+K
  // in the same snapshot, and verifies that all the values are of the form
  // "0"+V, "1"+V,..."9"+V.
  // ASSUMES that MultiPut was used to put (K, V) into the DB.
  Status MultiGet(ThreadState* thread, const ReadOptions& readoptions,
                  ColumnFamilyHandle* column_family, const Slice& key,
                  std::string* value) {
    std::string keys[10] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    Slice key_slices[10];
    std::string values[10];
    ReadOptions readoptionscopy = readoptions;
    readoptionscopy.snapshot = db_->GetSnapshot();
    Status s;
    for (int i = 0; i < 10; i++) {
      keys[i] += key.ToString();
      key_slices[i] = keys[i];
      s = db_->Get(readoptionscopy, column_family, key_slices[i], value);
      if (!s.ok() && !s.IsNotFound()) {
        fprintf(stderr, "get error: %s\n", s.ToString().c_str());
        values[i] = "";
        thread->stats.AddErrors(1);
        // we continue after error rather than exiting so that we can
        // find more errors if any
      } else if (s.IsNotFound()) {
        values[i] = "";
        thread->stats.AddGets(1, 0);
      } else {
        values[i] = *value;

        char expected_prefix = (keys[i])[0];
        char actual_prefix = (values[i])[0];
        if (actual_prefix != expected_prefix) {
          fprintf(stderr, "error expected prefix = %c actual = %c\n",
                  expected_prefix, actual_prefix);
        }
        (values[i])[0] = ' '; // blank out the differing character
        thread->stats.AddGets(1, 1);
      }
    }
    db_->ReleaseSnapshot(readoptionscopy.snapshot);

    // Now that we retrieved all values, check that they all match
    for (int i = 1; i < 10; i++) {
      if (values[i] != values[0]) {
        fprintf(stderr, "error : inconsistent values for key %s: %s, %s\n",
                key.ToString().c_str(), values[0].c_str(),
                values[i].c_str());
      // we continue after error rather than exiting so that we can
      // find more errors if any
      }
    }

    return s;
  }

  // Given a key, this does prefix scans for "0"+P, "1"+P,..."9"+P
  // in the same snapshot where P is the first FLAGS_prefix_size - 1 bytes
  // of the key. Each of these 10 scans returns a series of values;
  // each series should be the same length, and it is verified for each
  // index i that all the i'th values are of the form "0"+V, "1"+V,..."9"+V.
  // ASSUMES that MultiPut was used to put (K, V)
  Status MultiPrefixScan(ThreadState* thread, const ReadOptions& readoptions,
                         ColumnFamilyHandle* column_family,
                         const Slice& key) {
    std::string prefixes[10] = {"0", "1", "2", "3", "4",
                                "5", "6", "7", "8", "9"};
    Slice prefix_slices[10];
    ReadOptions readoptionscopy[10];
    const Snapshot* snapshot = db_->GetSnapshot();
    Iterator* iters[10];
    Status s = Status::OK();
    for (int i = 0; i < 10; i++) {
      prefixes[i] += key.ToString();
      prefixes[i].resize(FLAGS_prefix_size);
      prefix_slices[i] = Slice(prefixes[i]);
      readoptionscopy[i] = readoptions;
      readoptionscopy[i].prefix_seek = true;
      readoptionscopy[i].snapshot = snapshot;
      iters[i] = db_->NewIterator(readoptionscopy[i], column_family);
      iters[i]->Seek(prefix_slices[i]);
    }

    int count = 0;
    while (iters[0]->Valid()) {
      count++;
      std::string values[10];
      // get list of all values for this iteration
      for (int i = 0; i < 10; i++) {
        // no iterator should finish before the first one
        assert(iters[i]->Valid());
        values[i] = iters[i]->value().ToString();

        char expected_first = (prefixes[i])[0];
        char actual_first = (values[i])[0];

        if (actual_first != expected_first) {
          fprintf(stderr, "error expected first = %c actual = %c\n",
                  expected_first, actual_first);
        }
        (values[i])[0] = ' '; // blank out the differing character
      }
      // make sure all values are equivalent
      for (int i = 0; i < 10; i++) {
        if (values[i] != values[0]) {
          fprintf(stderr, "error : inconsistent values for prefix %s: %s, %s\n",
                  prefixes[i].c_str(), values[0].c_str(),
                  values[i].c_str());
          // we continue after error rather than exiting so that we can
          // find more errors if any
        }
        iters[i]->Next();
      }
    }

    // cleanup iterators and snapshot
    for (int i = 0; i < 10; i++) {
      // if the first iterator finished, they should have all finished
      assert(!iters[i]->Valid());
      assert(iters[i]->status().ok());
      delete iters[i];
    }
    db_->ReleaseSnapshot(snapshot);

    if (s.ok()) {
      thread->stats.AddPrefixes(1, count);
    } else {
      thread->stats.AddErrors(1);
    }

    return s;
  }

  // Given a key K, this creates an iterator which scans to K and then
  // does a random sequence of Next/Prev operations.
  Status MultiIterate(ThreadState* thread, const ReadOptions& readoptions,
                      ColumnFamilyHandle* column_family, const Slice& key) {
    Status s;
    const Snapshot* snapshot = db_->GetSnapshot();
    ReadOptions readoptionscopy = readoptions;
    readoptionscopy.snapshot = snapshot;
    readoptionscopy.prefix_seek = FLAGS_prefix_size > 0;
    unique_ptr<Iterator> iter(db_->NewIterator(readoptionscopy, column_family));

    iter->Seek(key);
    for (uint64_t i = 0; i < FLAGS_num_iterations && iter->Valid(); i++) {
      if (thread->rand.OneIn(2)) {
        iter->Next();
      } else {
        iter->Prev();
      }
    }

    if (s.ok()) {
      thread->stats.AddIterations(1);
    } else {
      thread->stats.AddErrors(1);
    }

    db_->ReleaseSnapshot(snapshot);

    return s;
  }

  void OperateDb(ThreadState* thread) {
    ReadOptions read_opts(FLAGS_verify_checksum, true);
    WriteOptions write_opts;
    char value[100];
    long max_key = thread->shared->GetMaxKey();
    std::string from_db;
    if (FLAGS_sync) {
      write_opts.sync = true;
    }
    write_opts.disableWAL = FLAGS_disable_wal;
    const int prefixBound = (int)FLAGS_readpercent + (int)FLAGS_prefixpercent;
    const int writeBound = prefixBound + (int)FLAGS_writepercent;
    const int delBound = writeBound + (int)FLAGS_delpercent;

    thread->stats.Start();
    for (uint64_t i = 0; i < FLAGS_ops_per_thread; i++) {
      if(i != 0 && (i % (FLAGS_ops_per_thread / (FLAGS_reopen + 1))) == 0) {
        {
          thread->stats.FinishedSingleOp();
          MutexLock l(thread->shared->GetMutex());
          thread->shared->IncVotedReopen();
          if (thread->shared->AllVotedReopen()) {
            thread->shared->GetStressTest()->Reopen();
            thread->shared->GetCondVar()->SignalAll();
          }
          else {
            thread->shared->GetCondVar()->Wait();
          }
          // Commenting this out as we don't want to reset stats on each open.
          // thread->stats.Start();
        }
      }

      if (!FLAGS_test_batches_snapshots &&
          FLAGS_clear_column_family_one_in != 0) {
        if (thread->rand.OneIn(FLAGS_clear_column_family_one_in)) {
          // drop column family and then create it again (can't drop default)
          int cf = thread->rand.Next() % (FLAGS_column_families - 1) + 1;
          std::string new_name =
              std::to_string(new_column_family_name_.fetch_add(1));
          {
            MutexLock l(thread->shared->GetMutex());
            fprintf(
                stdout,
                "[CF %d] Dropping and recreating column family. new name: %s\n",
                cf, new_name.c_str());
          }
          thread->shared->LockColumnFamily(cf);
          Status s __attribute__((unused));
          s = db_->DropColumnFamily(column_families_[cf]);
          delete column_families_[cf];
          assert(s.ok());
          s = db_->CreateColumnFamily(ColumnFamilyOptions(options_), new_name,
                                      &column_families_[cf]);
          column_family_names_[cf] = new_name;
          thread->shared->ClearColumnFamily(cf);
          assert(s.ok());
          thread->shared->UnlockColumnFamily(cf);
        }
      }

      long rand_key = thread->rand.Next() % max_key;
      int rand_column_family = thread->rand.Next() % FLAGS_column_families;
      std::string keystr = Key(rand_key);
      Slice key = keystr;
      int prob_op = thread->rand.Uniform(100);
      std::unique_ptr<MutexLock> l;
      if (!FLAGS_test_batches_snapshots) {
        l.reset(new MutexLock(
            thread->shared->GetMutexForKey(rand_column_family, rand_key)));
      }
      auto column_family = column_families_[rand_column_family];

      if (prob_op >= 0 && prob_op < (int)FLAGS_readpercent) {
        // OPERATION read
        if (!FLAGS_test_batches_snapshots) {
          Status s = db_->Get(read_opts, column_family, key, &from_db);
          if (s.ok()) {
            // found case
            thread->stats.AddGets(1, 1);
          } else if (s.IsNotFound()) {
            // not found case
            thread->stats.AddGets(1, 0);
          } else {
            // errors case
            thread->stats.AddErrors(1);
          }
        } else {
          MultiGet(thread, read_opts, column_family, key, &from_db);
        }
      } else if ((int)FLAGS_readpercent <= prob_op && prob_op < prefixBound) {
        // OPERATION prefix scan
        // keys are 8 bytes long, prefix size is FLAGS_prefix_size. There are
        // (8 - FLAGS_prefix_size) bytes besides the prefix. So there will
        // be 2 ^ ((8 - FLAGS_prefix_size) * 8) possible keys with the same
        // prefix
        if (!FLAGS_test_batches_snapshots) {
          Slice prefix = Slice(key.data(), FLAGS_prefix_size);
          read_opts.prefix_seek = true;
          Iterator* iter = db_->NewIterator(read_opts, column_family);
          int64_t count = 0;
          for (iter->Seek(prefix);
               iter->Valid() && iter->key().starts_with(prefix); iter->Next()) {
            ++count;
          }
          assert(count <=
                 (static_cast<int64_t>(1) << ((8 - FLAGS_prefix_size) * 8)));
          if (iter->status().ok()) {
            thread->stats.AddPrefixes(1, count);
          } else {
            thread->stats.AddErrors(1);
          }
          delete iter;
        } else {
          MultiPrefixScan(thread, read_opts, column_family, key);
        }
      } else if (prefixBound <= prob_op && prob_op < writeBound) {
        // OPERATION write
        uint32_t value_base = thread->rand.Next();
        size_t sz = GenerateValue(value_base, value, sizeof(value));
        Slice v(value, sz);
        if (!FLAGS_test_batches_snapshots) {
          if (FLAGS_verify_before_write) {
            std::string keystr2 = Key(rand_key);
            Slice k = keystr2;
            Status s = db_->Get(read_opts, column_family, k, &from_db);
            VerifyValue(rand_column_family, rand_key, read_opts,
                        *(thread->shared), from_db, s, true);
          }
          thread->shared->Put(rand_column_family, rand_key, value_base);
          if (FLAGS_use_merge) {
            db_->Merge(write_opts, column_family, key, v);
          } else {
            db_->Put(write_opts, column_family, key, v);
          }
          thread->stats.AddBytesForWrites(1, sz);
        } else {
          MultiPut(thread, write_opts, column_family, key, v, sz);
        }
        PrintKeyValue(rand_column_family, rand_key, value, sz);
      } else if (writeBound <= prob_op && prob_op < delBound) {
        // OPERATION delete
        if (!FLAGS_test_batches_snapshots) {
          thread->shared->Delete(rand_column_family, rand_key);
          db_->Delete(write_opts, column_family, key);
          thread->stats.AddDeletes(1);
        } else {
          MultiDelete(thread, write_opts, column_family, key);
        }
      } else {
        // OPERATION iterate
        MultiIterate(thread, read_opts, column_family, key);
      }
      thread->stats.FinishedSingleOp();
    }

    thread->stats.Stop();
  }

  void VerifyDb(ThreadState* thread) const {
    ReadOptions options(FLAGS_verify_checksum, true);
    const SharedState& shared = *(thread->shared);
    static const long max_key = shared.GetMaxKey();
    static const long keys_per_thread = max_key / shared.GetNumThreads();
    long start = keys_per_thread * thread->tid;
    long end = start + keys_per_thread;
    if (thread->tid == shared.GetNumThreads() - 1) {
      end = max_key;
    }
    for (size_t cf = 0; cf < column_families_.size(); ++cf) {
      if (!thread->rand.OneIn(2)) {
        // Use iterator to verify this range
        options.prefix_seek = FLAGS_prefix_size > 0;
        unique_ptr<Iterator> iter(
            db_->NewIterator(options, column_families_[cf]));
        iter->Seek(Key(start));
        for (long i = start; i < end; i++) {
          // TODO(ljin): update "long" to uint64_t
          // Reseek when the prefix changes
          if (i % (static_cast<int64_t>(1) << 8 * (8 - FLAGS_prefix_size)) ==
              0) {
            iter->Seek(Key(i));
          }
          std::string from_db;
          std::string keystr = Key(i);
          Slice k = keystr;
          Status s = iter->status();
          if (iter->Valid()) {
            if (iter->key().compare(k) > 0) {
              s = Status::NotFound(Slice());
            } else if (iter->key().compare(k) == 0) {
              from_db = iter->value().ToString();
              iter->Next();
            } else if (iter->key().compare(k) < 0) {
              VerificationAbort("An out of range key was found", cf, i);
            }
          } else {
            // The iterator found no value for the key in question, so do not
            // move to the next item in the iterator
            s = Status::NotFound(Slice());
          }
          VerifyValue(cf, i, options, shared, from_db, s, true);
          if (from_db.length()) {
            PrintKeyValue(cf, i, from_db.data(), from_db.length());
          }
        }
      } else {
        // Use Get to verify this range
        for (long i = start; i < end; i++) {
          std::string from_db;
          std::string keystr = Key(i);
          Slice k = keystr;
          Status s = db_->Get(options, column_families_[cf], k, &from_db);
          VerifyValue(cf, i, options, shared, from_db, s, true);
          if (from_db.length()) {
            PrintKeyValue(cf, i, from_db.data(), from_db.length());
          }
        }
      }
    }
  }

  void VerificationAbort(std::string msg, int cf, long key) const {
    fprintf(stderr, "Verification failed for column family %d key %ld: %s\n",
            cf, key, msg.c_str());
    exit(1);
  }

  void VerifyValue(int cf, long key, const ReadOptions& opts,
                   const SharedState& shared, const std::string& value_from_db,
                   Status s, bool strict = false) const {
    // compare value_from_db with the value in the shared state
    char value[100];
    uint32_t value_base = shared.Get(cf, key);
    if (value_base == SharedState::SENTINEL && !strict) {
      return;
    }

    if (s.ok()) {
      if (value_base == SharedState::SENTINEL) {
        VerificationAbort("Unexpected value found", cf, key);
      }
      size_t sz = GenerateValue(value_base, value, sizeof(value));
      if (value_from_db.length() != sz) {
        VerificationAbort("Length of value read is not equal", cf, key);
      }
      if (memcmp(value_from_db.data(), value, sz) != 0) {
        VerificationAbort("Contents of value read don't match", cf, key);
      }
    } else {
      if (value_base != SharedState::SENTINEL) {
        VerificationAbort("Value not found", cf, key);
      }
    }
  }

  static void PrintKeyValue(int cf, uint32_t key, const char* value,
                            size_t sz) {
    if (!FLAGS_verbose) {
      return;
    }
    fprintf(stdout, "[CF %d] %u ==> (%u) ", cf, key, (unsigned int)sz);
    for (size_t i = 0; i < sz; i++) {
      fprintf(stdout, "%X", value[i]);
    }
    fprintf(stdout, "\n");
  }

  static size_t GenerateValue(uint32_t rand, char *v, size_t max_sz) {
    size_t value_sz = ((rand % 3) + 1) * FLAGS_value_size_mult;
    assert(value_sz <= max_sz && value_sz >= sizeof(uint32_t));
    *((uint32_t*)v) = rand;
    for (size_t i=sizeof(uint32_t); i < value_sz; i++) {
      v[i] = (char)(rand ^ i);
    }
    v[value_sz] = '\0';
    return value_sz; // the size of the value set.
  }

  void PrintEnv() const {
    fprintf(stdout, "RocksDB version     : %d.%d\n", kMajorVersion,
            kMinorVersion);
    fprintf(stdout, "Column families     : %d\n", FLAGS_column_families);
    if (!FLAGS_test_batches_snapshots) {
      fprintf(stdout, "Clear CFs one in    : %d\n",
              FLAGS_clear_column_family_one_in);
    }
    fprintf(stdout, "Number of threads   : %d\n", FLAGS_threads);
    fprintf(stdout,
            "Ops per thread      : %lu\n",
            (unsigned long)FLAGS_ops_per_thread);
    std::string ttl_state("unused");
    if (FLAGS_ttl > 0) {
      ttl_state = NumberToString(FLAGS_ttl);
    }
    fprintf(stdout, "Time to live(sec)   : %s\n", ttl_state.c_str());
    fprintf(stdout, "Read percentage     : %d%%\n", FLAGS_readpercent);
    fprintf(stdout, "Prefix percentage   : %d%%\n", FLAGS_prefixpercent);
    fprintf(stdout, "Write percentage    : %d%%\n", FLAGS_writepercent);
    fprintf(stdout, "Delete percentage   : %d%%\n", FLAGS_delpercent);
    fprintf(stdout, "Iterate percentage  : %d%%\n", FLAGS_iterpercent);
    fprintf(stdout, "Write-buffer-size   : %d\n", FLAGS_write_buffer_size);
    fprintf(stdout,
            "Iterations          : %lu\n",
            (unsigned long)FLAGS_num_iterations);
    fprintf(stdout,
            "Max key             : %lu\n",
            (unsigned long)FLAGS_max_key);
    fprintf(stdout, "Ratio #ops/#keys    : %f\n",
            (1.0 * FLAGS_ops_per_thread * FLAGS_threads)/FLAGS_max_key);
    fprintf(stdout, "Num times DB reopens: %d\n", FLAGS_reopen);
    fprintf(stdout, "Batches/snapshots   : %d\n",
            FLAGS_test_batches_snapshots);
    fprintf(stdout, "Purge redundant %%   : %d\n",
            FLAGS_purge_redundant_percent);
    fprintf(stdout, "Deletes use filter  : %d\n",
            FLAGS_filter_deletes);
    fprintf(stdout, "Num keys per lock   : %d\n",
            1 << FLAGS_log2_keys_per_lock);

    const char* compression = "";
    switch (FLAGS_compression_type_e) {
      case rocksdb::kNoCompression:
        compression = "none";
        break;
      case rocksdb::kSnappyCompression:
        compression = "snappy";
        break;
      case rocksdb::kZlibCompression:
        compression = "zlib";
        break;
      case rocksdb::kBZip2Compression:
        compression = "bzip2";
        break;
      case rocksdb::kLZ4Compression:
        compression = "lz4";
      case rocksdb::kLZ4HCCompression:
        compression = "lz4hc";
        break;
      }

    fprintf(stdout, "Compression         : %s\n", compression);

    const char* memtablerep = "";
    switch (FLAGS_rep_factory) {
      case kSkipList:
        memtablerep = "skip_list";
        break;
      case kHashSkipList:
        memtablerep = "prefix_hash";
        break;
      case kVectorRep:
        memtablerep = "vector";
        break;
    }

    fprintf(stdout, "Memtablerep         : %s\n", memtablerep);

    fprintf(stdout, "------------------------------------------------\n");
  }

  void Open() {
    assert(db_ == nullptr);
    options_.block_cache = cache_;
    options_.block_cache_compressed = compressed_cache_;
    options_.write_buffer_size = FLAGS_write_buffer_size;
    options_.max_write_buffer_number = FLAGS_max_write_buffer_number;
    options_.min_write_buffer_number_to_merge =
        FLAGS_min_write_buffer_number_to_merge;
    options_.max_background_compactions = FLAGS_max_background_compactions;
    options_.max_background_flushes = FLAGS_max_background_flushes;
    options_.compaction_style =
        static_cast<rocksdb::CompactionStyle>(FLAGS_compaction_style);
    options_.block_size = FLAGS_block_size;
    options_.filter_policy = filter_policy_;
    options_.prefix_extractor.reset(NewFixedPrefixTransform(FLAGS_prefix_size));
    options_.max_open_files = FLAGS_open_files;
    options_.statistics = dbstats;
    options_.env = FLAGS_env;
    options_.disableDataSync = FLAGS_disable_data_sync;
    options_.use_fsync = FLAGS_use_fsync;
    options_.allow_mmap_reads = FLAGS_mmap_read;
    rocksdb_kill_odds = FLAGS_kill_random_test;
    options_.target_file_size_base = FLAGS_target_file_size_base;
    options_.target_file_size_multiplier = FLAGS_target_file_size_multiplier;
    options_.max_bytes_for_level_base = FLAGS_max_bytes_for_level_base;
    options_.max_bytes_for_level_multiplier =
        FLAGS_max_bytes_for_level_multiplier;
    options_.level0_stop_writes_trigger = FLAGS_level0_stop_writes_trigger;
    options_.level0_slowdown_writes_trigger =
        FLAGS_level0_slowdown_writes_trigger;
    options_.level0_file_num_compaction_trigger =
        FLAGS_level0_file_num_compaction_trigger;
    options_.compression = FLAGS_compression_type_e;
    options_.create_if_missing = true;
    options_.disable_seek_compaction = FLAGS_disable_seek_compaction;
    options_.max_manifest_file_size = 10 * 1024;
    options_.filter_deletes = FLAGS_filter_deletes;
    if ((FLAGS_prefix_size == 0) == (FLAGS_rep_factory == kHashSkipList)) {
      fprintf(stderr,
            "prefix_size should be non-zero iff memtablerep == prefix_hash\n");
      exit(1);
    }
    switch (FLAGS_rep_factory) {
      case kHashSkipList:
        options_.memtable_factory.reset(NewHashSkipListRepFactory());
        break;
      case kSkipList:
        // no need to do anything
        break;
      case kVectorRep:
        options_.memtable_factory.reset(new VectorRepFactory());
        break;
    }
    static Random purge_percent(1000); // no benefit from non-determinism here
    if (static_cast<int32_t>(purge_percent.Uniform(100)) <
        FLAGS_purge_redundant_percent - 1) {
      options_.purge_redundant_kvs_while_flush = false;
    }

    if (FLAGS_use_merge) {
      options_.merge_operator = MergeOperators::CreatePutOperator();
    }

    // set universal style compaction configurations, if applicable
    if (FLAGS_universal_size_ratio != 0) {
      options_.compaction_options_universal.size_ratio =
          FLAGS_universal_size_ratio;
    }
    if (FLAGS_universal_min_merge_width != 0) {
      options_.compaction_options_universal.min_merge_width =
          FLAGS_universal_min_merge_width;
    }
    if (FLAGS_universal_max_merge_width != 0) {
      options_.compaction_options_universal.max_merge_width =
          FLAGS_universal_max_merge_width;
    }
    if (FLAGS_universal_max_size_amplification_percent != 0) {
      options_.compaction_options_universal.max_size_amplification_percent =
          FLAGS_universal_max_size_amplification_percent;
    }

    fprintf(stdout, "DB path: [%s]\n", FLAGS_db.c_str());

    Status s;
    if (FLAGS_ttl == -1) {
      std::vector<std::string> existing_column_families;
      s = DB::ListColumnFamilies(DBOptions(options_), FLAGS_db,
                                 &existing_column_families);  // ignore errors
      if (!s.ok()) {
        // DB doesn't exist
        assert(existing_column_families.empty());
        assert(column_family_names_.empty());
        column_family_names_.push_back(default_column_family_name);
      } else if (column_family_names_.empty()) {
        // this is the first call to the function Open()
        column_family_names_ = existing_column_families;
      } else {
        // this is a reopen. just assert that existing column_family_names are
        // equivalent to what we remember
        auto sorted_cfn = column_family_names_;
        sort(sorted_cfn.begin(), sorted_cfn.end());
        sort(existing_column_families.begin(), existing_column_families.end());
        if (sorted_cfn != existing_column_families) {
          fprintf(stderr,
                  "Expected column families differ from the existing:\n");
          printf("Expected: {");
          for (auto cf : sorted_cfn) {
            printf("%s ", cf.c_str());
          }
          printf("}\n");
          printf("Existing: {");
          for (auto cf : existing_column_families) {
            printf("%s ", cf.c_str());
          }
          printf("}\n");
        }
        assert(sorted_cfn == existing_column_families);
      }
      std::vector<ColumnFamilyDescriptor> cf_descriptors;
      for (auto name : column_family_names_) {
        if (name != default_column_family_name) {
          new_column_family_name_ =
              std::max(new_column_family_name_.load(), std::stoi(name) + 1);
        }
        cf_descriptors.emplace_back(name, ColumnFamilyOptions(options_));
      }
      s = DB::Open(DBOptions(options_), FLAGS_db, cf_descriptors,
                   &column_families_, &db_);
      if (s.ok()) {
        while (s.ok() &&
               column_families_.size() < (size_t)FLAGS_column_families) {
          ColumnFamilyHandle* cf = nullptr;
          std::string name = std::to_string(new_column_family_name_.load());
          new_column_family_name_++;
          s = db_->CreateColumnFamily(ColumnFamilyOptions(options_), name, &cf);
          column_families_.push_back(cf);
          column_family_names_.push_back(name);
        }
      }
      assert(!s.ok() || column_families_.size() ==
                            static_cast<size_t>(FLAGS_column_families));
    } else {
      StackableDB* sdb;
      s = UtilityDB::OpenTtlDB(options_, FLAGS_db, &sdb, FLAGS_ttl);
      db_ = sdb;
    }
    if (!s.ok()) {
      fprintf(stderr, "open error: %s\n", s.ToString().c_str());
      exit(1);
    }
  }

  void Reopen() {
    for (auto cf : column_families_) {
      delete cf;
    }
    column_families_.clear();
    delete db_;
    db_ = nullptr;

    num_times_reopened_++;
    double now = FLAGS_env->NowMicros();
    fprintf(stdout, "%s Reopening database for the %dth time\n",
            FLAGS_env->TimeToString((uint64_t) now/1000000).c_str(),
            num_times_reopened_);
    Open();
  }

  void PrintStatistics() {
    if (dbstats) {
      fprintf(stdout, "STATISTICS:\n%s\n", dbstats->ToString().c_str());
    }
  }

 private:
  shared_ptr<Cache> cache_;
  shared_ptr<Cache> compressed_cache_;
  const FilterPolicy* filter_policy_;
  DB* db_;
  Options options_;
  std::vector<ColumnFamilyHandle*> column_families_;
  std::vector<std::string> column_family_names_;
  std::atomic<int> new_column_family_name_;
  int num_times_reopened_;
};

}  // namespace rocksdb

int main(int argc, char** argv) {
  google::SetUsageMessage(std::string("\nUSAGE:\n") + std::string(argv[0]) +
                          " [OPTIONS]...");
  google::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_statistics) {
    dbstats = rocksdb::CreateDBStatistics();
  }
  FLAGS_compression_type_e =
    StringToCompressionType(FLAGS_compression_type.c_str());
  if (!FLAGS_hdfs.empty()) {
    FLAGS_env  = new rocksdb::HdfsEnv(FLAGS_hdfs);
  }
  FLAGS_rep_factory = StringToRepFactory(FLAGS_memtablerep.c_str());

  // The number of background threads should be at least as much the
  // max number of concurrent compactions.
  FLAGS_env->SetBackgroundThreads(FLAGS_max_background_compactions);

  if (FLAGS_prefixpercent > 0 && FLAGS_prefix_size <= 0) {
    fprintf(stderr,
            "Error: prefixpercent is non-zero while prefix_size is "
            "not positive!\n");
    exit(1);
  }
  if (FLAGS_test_batches_snapshots && FLAGS_prefix_size <= 0) {
    fprintf(stderr,
            "Error: please specify prefix_size for "
            "test_batches_snapshots test!\n");
    exit(1);
  }
  if ((FLAGS_readpercent + FLAGS_prefixpercent +
       FLAGS_writepercent + FLAGS_delpercent + FLAGS_iterpercent) != 100) {
      fprintf(stderr,
              "Error: Read+Prefix+Write+Delete+Iterate percents != 100!\n");
      exit(1);
  }
  if (FLAGS_disable_wal == 1 && FLAGS_reopen > 0) {
      fprintf(stderr, "Error: Db cannot reopen safely with disable_wal set!\n");
      exit(1);
  }
  if ((unsigned)FLAGS_reopen >= FLAGS_ops_per_thread) {
      fprintf(stderr,
              "Error: #DB-reopens should be < ops_per_thread\n"
              "Provided reopens = %d and ops_per_thread = %lu\n",
              FLAGS_reopen,
              (unsigned long)FLAGS_ops_per_thread);
      exit(1);
  }

  // Choose a location for the test database if none given with --db=<path>
  if (FLAGS_db.empty()) {
      std::string default_db_path;
      rocksdb::Env::Default()->GetTestDirectory(&default_db_path);
      default_db_path += "/dbstress";
      FLAGS_db = default_db_path;
  }

  rocksdb::StressTest stress;
  stress.Run();
  return 0;
}
