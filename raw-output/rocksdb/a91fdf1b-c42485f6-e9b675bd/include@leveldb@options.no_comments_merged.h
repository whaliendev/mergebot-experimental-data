#ifndef STORAGE_LEVELDB_INCLUDE_OPTIONS_H_
#define STORAGE_LEVELDB_INCLUDE_OPTIONS_H_ 
#include <stddef.h>
#include <string>
#include <memory>
#include <vector>
#include <stdint.h>
#include "leveldb/slice.h"
#include "leveldb/statistics.h"
<<<<<<< HEAD
#include "leveldb/universal_compaction.h"
||||||| e9b675bd9
=======
#include "leveldb/memtablerep.h"
>>>>>>> c42485f6
namespace leveldb {
class Cache;
class Comparator;
class Env;
class FilterPolicy;
class Logger;
class MergeOperator;
class Snapshot;
class CompactionFilter;
using std::shared_ptr;
enum CompressionType {
  kNoCompression = 0x0,
  kSnappyCompression = 0x1,
  kZlibCompression = 0x2,
  kBZip2Compression = 0x3
};
enum CompactionStyle {
  kCompactionStyleLevel = 0x0,
  kCompactionStyleUniversal = 0x1
};
struct CompressionOptions {
  int window_bits;
  int level;
  int strategy;
  CompressionOptions():window_bits(-14),
                       level(-1),
                       strategy(0){}
  CompressionOptions(int wbits, int lev, int strategy):window_bits(wbits),
                                                       level(lev),
                                                       strategy(strategy){}
};
struct Options {
  const Comparator* comparator;
  const MergeOperator* merge_operator;
  const CompactionFilter* compaction_filter;
  bool create_if_missing;
  bool error_if_exists;
  bool paranoid_checks;
  Env* env;
  shared_ptr<Logger> info_log;
  size_t write_buffer_size;
  int max_write_buffer_number;
  int min_write_buffer_number_to_merge;
  int max_open_files;
  shared_ptr<Cache> block_cache;
  size_t block_size;
  int block_restart_interval;
  CompressionType compression;
  std::vector<CompressionType> compression_per_level;
  CompressionOptions compression_opts;
  const FilterPolicy* filter_policy;
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
  shared_ptr<Statistics> statistics;
  bool disableDataSync;
  bool use_fsync;
  int db_stats_log_interval;
  std::string db_log_dir;
  bool disable_seek_compaction;
  uint64_t delete_obsolete_files_period_micros;
  int max_background_compactions;
  size_t max_log_file_size;
  size_t log_file_time_to_roll;
  size_t keep_log_file_num;
  double rate_limit;
  unsigned int rate_limit_delay_milliseconds;
  uint64_t max_manifest_file_size;
  bool no_block_cache;
  int table_cache_numshardbits;
  size_t arena_block_size;
  Options();
  void Dump(Logger* log) const;
  Options* PrepareForBulkLoad();
  bool disable_auto_compactions;
  uint64_t WAL_ttl_seconds;
  size_t manifest_preallocation_size;
  bool purge_redundant_kvs_while_flush;
  bool allow_os_buffer;
  bool allow_mmap_reads;
  bool allow_mmap_writes;
  bool is_fd_close_on_exec;
  bool skip_log_error_on_recovery;
  unsigned int stats_dump_period_sec;
  int block_size_deviation;
  bool advise_random_on_open;
  enum { NONE, NORMAL, SEQUENTIAL, WILLNEED } access_hint_on_compaction_start;
  bool use_adaptive_mutex;
  uint64_t bytes_per_sync;
<<<<<<< HEAD
  CompactionStyle compaction_style;
  CompactionOptionsUniversal compaction_options_universal;
||||||| e9b675bd9
=======
>>>>>>> c42485f6
<<<<<<< HEAD
  bool deletes_check_filter_first;
||||||| e9b675bd9
  bool deletes_check_filter_first;
=======
  bool filter_deletes;
  std::shared_ptr<MemTableRepFactory> memtable_factory;
>>>>>>> c42485f6
};
struct ReadOptions {
  bool verify_checksums;
  bool fill_cache;
  const Snapshot* snapshot;
  ReadOptions()
      : verify_checksums(false),
        fill_cache(true),
        snapshot(nullptr) {
  }
  ReadOptions(bool cksum, bool cache) :
              verify_checksums(cksum), fill_cache(cache),
              snapshot(nullptr) {
  }
};
struct WriteOptions {
  bool sync;
  bool disableWAL;
  WriteOptions()
      : sync(false),
        disableWAL(false) {
  }
};
struct FlushOptions {
  bool wait;
  FlushOptions()
      : wait(true) {
  }
};
}
#endif
