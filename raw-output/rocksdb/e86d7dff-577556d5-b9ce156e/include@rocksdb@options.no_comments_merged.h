#ifndef STORAGE_ROCKSDB_INCLUDE_OPTIONS_H_
#define STORAGE_ROCKSDB_INCLUDE_OPTIONS_H_ 
#include <stddef.h>
#include <string>
#include <memory>
#include <vector>
#include <stdint.h>
#include "rocksdb/universal_compaction.h"
namespace rocksdb {
class Cache;
class CompactionFilter;
class CompactionFilterFactory;
class CompactionFilterFactoryV2;
class Comparator;
class Env;
enum InfoLogLevel : unsigned char;
class FilterPolicy;
class Logger;
class MergeOperator;
class Snapshot;
class TableFactory;
class MemTableRepFactory;
class TablePropertiesCollector;
class Slice;
class SliceTransform;
class Statistics;
class InternalKeyComparator;
using std::shared_ptr;
enum CompressionType : char {
  kNoCompression = 0x0, kSnappyCompression = 0x1, kZlibCompression = 0x2,
  kBZip2Compression = 0x3, kLZ4Compression = 0x4, kLZ4HCCompression = 0x5
};
enum CompactionStyle : char {
  kCompactionStyleLevel = 0x0,
  kCompactionStyleUniversal = 0x1
};
struct CompressionOptions {
  int window_bits;
  int level;
  int strategy;
  CompressionOptions() : window_bits(-14), level(-1), strategy(0) {}
  CompressionOptions(int wbits, int lev, int strategy)
      : window_bits(wbits), level(lev), strategy(strategy) {}
};
enum UpdateStatus {
  UPDATE_FAILED = 0,
  UPDATED_INPLACE = 1,
  UPDATED = 2,
};
struct Options;
struct ColumnFamilyOptions {
  const Comparator* comparator;
  shared_ptr<MergeOperator> merge_operator;
  const CompactionFilter* compaction_filter;
  std::shared_ptr<CompactionFilterFactory> compaction_filter_factory;
  std::shared_ptr<CompactionFilterFactoryV2> compaction_filter_factory_v2;
<<<<<<< HEAD
||||||| b9ce156e3
  bool create_if_missing;
  bool error_if_exists;
  bool paranoid_checks;
  Env* env;
  shared_ptr<Logger> info_log;
  InfoLogLevel info_log_level;
=======
  bool create_if_missing;
  bool error_if_exists;
  bool paranoid_checks;
  Env* env;
  shared_ptr<Logger> info_log;
  InfoLogLevel info_log_level;
>>>>>>> 577556d5
  size_t write_buffer_size;
  int max_write_buffer_number;
  int min_write_buffer_number_to_merge;
<<<<<<< HEAD
||||||| b9ce156e3
  int max_open_files;
=======
  int max_open_files;
>>>>>>> 577556d5
  shared_ptr<Cache> block_cache;
  shared_ptr<Cache> block_cache_compressed;
  size_t block_size;
  int block_restart_interval;
  CompressionType compression;
  std::vector<CompressionType> compression_per_level;
  CompressionOptions compression_opts;
  const FilterPolicy* filter_policy;
  std::shared_ptr<const SliceTransform> prefix_extractor;
  bool whole_key_filtering;
  int num_levels;
  int level0_file_num_compaction_trigger;
  int level0_slowdown_writes_trigger;
  int level0_stop_writes_trigger;
  int max_mem_compaction_level;
  int target_file_size_base;
  int target_file_size_multiplier;
  uint64_t max_bytes_for_level_base;
  int max_bytes_for_level_multiplier;
  std::vector<int> max_bytes_for_level_multiplier_additional;
  int expanded_compaction_factor;
  int source_compaction_factor;
  int max_grandparent_overlap_factor;
  bool disable_seek_compaction;
<<<<<<< HEAD
||||||| b9ce156e3
  uint64_t delete_obsolete_files_period_micros;
  int max_background_compactions;
  int max_background_flushes;
  size_t max_log_file_size;
  size_t log_file_time_to_roll;
  size_t keep_log_file_num;
=======
  uint64_t delete_obsolete_files_period_micros;
  int max_background_compactions;
  int max_background_flushes;
  size_t max_log_file_size;
  size_t log_file_time_to_roll;
  size_t keep_log_file_num;
>>>>>>> 577556d5
  double soft_rate_limit;
  double hard_rate_limit;
  unsigned int rate_limit_delay_max_milliseconds;
  bool no_block_cache;
  size_t arena_block_size;
  bool disable_auto_compactions;
  bool purge_redundant_kvs_while_flush;
<<<<<<< HEAD
||||||| b9ce156e3
  bool allow_os_buffer;
  bool allow_mmap_reads;
  bool allow_mmap_writes;
  bool is_fd_close_on_exec;
  bool skip_log_error_on_recovery;
  unsigned int stats_dump_period_sec;
=======
  bool allow_os_buffer;
  bool allow_mmap_reads;
  bool allow_mmap_writes;
  bool is_fd_close_on_exec;
  bool skip_log_error_on_recovery;
  unsigned int stats_dump_period_sec;
>>>>>>> 577556d5
  int block_size_deviation;
  CompactionStyle compaction_style;
  bool verify_checksums_in_compaction;
  CompactionOptionsUniversal compaction_options_universal;
  bool filter_deletes;
  uint64_t max_sequential_skip_in_iterations;
  std::shared_ptr<MemTableRepFactory> memtable_factory;
  std::shared_ptr<TableFactory> table_factory;
  typedef std::vector<std::shared_ptr<TablePropertiesCollector>>
      TablePropertiesCollectors;
  TablePropertiesCollectors table_properties_collectors;
  bool inplace_update_support;
  size_t inplace_update_num_locks;
  UpdateStatus (*inplace_callback)(char* existing_value,
                                   uint32_t* existing_value_size,
                                   Slice delta_value,
                                   std::string* merged_value);
  uint32_t memtable_prefix_bloom_bits;
  uint32_t memtable_prefix_bloom_probes;
  uint32_t bloom_locality;
  size_t max_successive_merges;
  uint32_t min_partial_merge_operands;
  ColumnFamilyOptions();
  explicit ColumnFamilyOptions(const Options& options);
  void Dump(Logger* log) const;
};
struct DBOptions {
  bool create_if_missing;
  bool error_if_exists;
  bool paranoid_checks;
  Env* env;
  shared_ptr<Logger> info_log;
  InfoLogLevel info_log_level;
  int max_open_files;
  shared_ptr<Statistics> statistics;
  bool disableDataSync;
  bool use_fsync;
  int db_stats_log_interval;
  std::string db_log_dir;
  std::string wal_dir;
  uint64_t delete_obsolete_files_period_micros;
  int max_background_compactions;
  int max_background_flushes;
  size_t max_log_file_size;
  size_t log_file_time_to_roll;
  size_t keep_log_file_num;
  uint64_t max_manifest_file_size;
  int table_cache_numshardbits;
  int table_cache_remove_scan_count_limit;
  uint64_t WAL_ttl_seconds;
  uint64_t WAL_size_limit_MB;
  size_t manifest_preallocation_size;
  bool allow_os_buffer;
  bool allow_mmap_reads;
  bool allow_mmap_writes;
  bool is_fd_close_on_exec;
  bool skip_log_error_on_recovery;
  unsigned int stats_dump_period_sec;
  bool advise_random_on_open;
  enum {
    NONE,
    NORMAL,
    SEQUENTIAL,
    WILLNEED
  } access_hint_on_compaction_start;
  bool use_adaptive_mutex;
  uint64_t bytes_per_sync;
  bool allow_thread_local;
  DBOptions();
  explicit DBOptions(const Options& options);
  void Dump(Logger* log) const;
};
struct Options : public DBOptions, public ColumnFamilyOptions {
  Options() :
    DBOptions(),
    ColumnFamilyOptions() {}
  Options(const DBOptions& db_options,
          const ColumnFamilyOptions& column_family_options)
      : DBOptions(db_options), ColumnFamilyOptions(column_family_options) {}
  void Dump(Logger* log) const;
  Options* PrepareForBulkLoad();
};
enum ReadTier {
  kReadAllTier = 0x0,
  kBlockCacheTier = 0x1
};
struct ReadOptions {
  bool verify_checksums;
  bool fill_cache;
  bool prefix_seek;
  const Snapshot* snapshot;
  const Slice* prefix;
  ReadTier read_tier;
  bool tailing;
  ReadOptions()
      : verify_checksums(true),
        fill_cache(true),
        prefix_seek(false),
        snapshot(nullptr),
        prefix(nullptr),
        read_tier(kReadAllTier),
        tailing(false) {}
  ReadOptions(bool cksum, bool cache)
      : verify_checksums(cksum),
        fill_cache(cache),
        prefix_seek(false),
        snapshot(nullptr),
        prefix(nullptr),
        read_tier(kReadAllTier),
        tailing(false) {}
};
struct WriteOptions {
  bool sync;
  bool disableWAL;
  WriteOptions() : sync(false), disableWAL(false) {}
};
struct FlushOptions {
  bool wait;
  FlushOptions() : wait(true) {}
};
}
#endif
