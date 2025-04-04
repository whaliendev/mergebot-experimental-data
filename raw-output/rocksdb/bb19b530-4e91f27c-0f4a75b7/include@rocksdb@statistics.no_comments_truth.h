#ifndef STORAGE_ROCKSDB_INCLUDE_STATISTICS_H_
#define STORAGE_ROCKSDB_INCLUDE_STATISTICS_H_ 
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <memory>
#include <vector>
namespace rocksdb {
enum Tickers {
  BLOCK_CACHE_MISS,
  BLOCK_CACHE_HIT,
  BLOCK_CACHE_ADD,
  BLOCK_CACHE_INDEX_MISS,
  BLOCK_CACHE_INDEX_HIT,
  BLOCK_CACHE_FILTER_MISS,
  BLOCK_CACHE_FILTER_HIT,
  BLOCK_CACHE_DATA_MISS,
  BLOCK_CACHE_DATA_HIT,
  BLOOM_FILTER_USEFUL,
  MEMTABLE_HIT,
  MEMTABLE_MISS,
  COMPACTION_KEY_DROP_NEWER_ENTRY,
  COMPACTION_KEY_DROP_OBSOLETE,
  COMPACTION_KEY_DROP_USER,
  NUMBER_KEYS_WRITTEN,
  NUMBER_KEYS_READ,
  NUMBER_KEYS_UPDATED,
  BYTES_WRITTEN,
  BYTES_READ,
  NO_FILE_CLOSES,
  NO_FILE_OPENS,
  NO_FILE_ERRORS,
  STALL_L0_SLOWDOWN_MICROS,
  STALL_MEMTABLE_COMPACTION_MICROS,
  STALL_L0_NUM_FILES_MICROS,
  RATE_LIMIT_DELAY_MILLIS,
  NO_ITERATORS,
  NUMBER_MULTIGET_CALLS,
  NUMBER_MULTIGET_KEYS_READ,
  NUMBER_MULTIGET_BYTES_READ,
  NUMBER_FILTERED_DELETES,
  NUMBER_MERGE_FAILURES,
  SEQUENCE_NUMBER,
  BLOOM_FILTER_PREFIX_CHECKED,
  BLOOM_FILTER_PREFIX_USEFUL,
  NUMBER_OF_RESEEKS_IN_ITERATION,
  GET_UPDATES_SINCE_CALLS,
  BLOCK_CACHE_COMPRESSED_MISS,
  BLOCK_CACHE_COMPRESSED_HIT,
  WAL_FILE_SYNCED,
  WAL_FILE_BYTES,
  WRITE_DONE_BY_SELF,
  WRITE_DONE_BY_OTHER,
  WRITE_WITH_WAL,
  COMPACT_READ_BYTES,
  COMPACT_WRITE_BYTES,
  TICKER_ENUM_MAX
};
const std::vector<std::pair<Tickers, std::string>> TickersNameMap = {
  { BLOCK_CACHE_MISS, "rocksdb.block.cache.miss" },
  { BLOCK_CACHE_HIT, "rocksdb.block.cache.hit" },
  { BLOCK_CACHE_ADD, "rocksdb.block.cache.add" },
  { BLOCK_CACHE_INDEX_MISS, "rocksdb.block.cache.index.miss" },
  { BLOCK_CACHE_INDEX_HIT, "rocksdb.block.cache.index.hit" },
  { BLOCK_CACHE_FILTER_MISS, "rocksdb.block.cache.filter.miss" },
  { BLOCK_CACHE_FILTER_HIT, "rocksdb.block.cache.filter.hit" },
  { BLOCK_CACHE_DATA_MISS, "rocksdb.block.cache.data.miss" },
  { BLOCK_CACHE_DATA_HIT, "rocksdb.block.cache.data.hit" },
  { BLOOM_FILTER_USEFUL, "rocksdb.bloom.filter.useful" },
  { MEMTABLE_HIT, "rocksdb.memtable.hit" },
  { MEMTABLE_MISS, "rocksdb.memtable.miss" },
  { COMPACTION_KEY_DROP_NEWER_ENTRY, "rocksdb.compaction.key.drop.new" },
  { COMPACTION_KEY_DROP_OBSOLETE, "rocksdb.compaction.key.drop.obsolete" },
  { COMPACTION_KEY_DROP_USER, "rocksdb.compaction.key.drop.user" },
  { NUMBER_KEYS_WRITTEN, "rocksdb.number.keys.written" },
  { NUMBER_KEYS_READ, "rocksdb.number.keys.read" },
  { NUMBER_KEYS_UPDATED, "rocksdb.number.keys.updated" },
  { BYTES_WRITTEN, "rocksdb.bytes.written" },
  { BYTES_READ, "rocksdb.bytes.read" },
  { NO_FILE_CLOSES, "rocksdb.no.file.closes" },
  { NO_FILE_OPENS, "rocksdb.no.file.opens" },
  { NO_FILE_ERRORS, "rocksdb.no.file.errors" },
  { STALL_L0_SLOWDOWN_MICROS, "rocksdb.l0.slowdown.micros" },
  { STALL_MEMTABLE_COMPACTION_MICROS, "rocksdb.memtable.compaction.micros" },
  { STALL_L0_NUM_FILES_MICROS, "rocksdb.l0.num.files.stall.micros" },
  { RATE_LIMIT_DELAY_MILLIS, "rocksdb.rate.limit.delay.millis" },
  { NO_ITERATORS, "rocksdb.num.iterators" },
  { NUMBER_MULTIGET_CALLS, "rocksdb.number.multiget.get" },
  { NUMBER_MULTIGET_KEYS_READ, "rocksdb.number.multiget.keys.read" },
  { NUMBER_MULTIGET_BYTES_READ, "rocksdb.number.multiget.bytes.read" },
  { NUMBER_FILTERED_DELETES, "rocksdb.number.deletes.filtered" },
  { NUMBER_MERGE_FAILURES, "rocksdb.number.merge.failures" },
  { SEQUENCE_NUMBER, "rocksdb.sequence.number" },
  { BLOOM_FILTER_PREFIX_CHECKED, "rocksdb.bloom.filter.prefix.checked" },
  { BLOOM_FILTER_PREFIX_USEFUL, "rocksdb.bloom.filter.prefix.useful" },
  { NUMBER_OF_RESEEKS_IN_ITERATION, "rocksdb.number.reseeks.iteration" },
  { GET_UPDATES_SINCE_CALLS, "rocksdb.getupdatessince.calls" },
  { BLOCK_CACHE_COMPRESSED_MISS, "rocksdb.block.cachecompressed.miss" },
  { BLOCK_CACHE_COMPRESSED_HIT, "rocksdb.block.cachecompressed.hit" },
  { WAL_FILE_SYNCED, "rocksdb.wal.synced" },
  { WAL_FILE_BYTES, "rocksdb.wal.bytes" },
  { WRITE_DONE_BY_SELF, "rocksdb.write.self" },
  { WRITE_DONE_BY_OTHER, "rocksdb.write.other" },
  { WRITE_WITH_WAL, "rocksdb.write.wal" },
  { COMPACT_READ_BYTES, "rocksdb.compact.read.bytes" },
  { COMPACT_WRITE_BYTES, "rocksdb.compact.write.bytes" },
};
enum Histograms {
  DB_GET,
  DB_WRITE,
  COMPACTION_TIME,
  TABLE_SYNC_MICROS,
  COMPACTION_OUTFILE_SYNC_MICROS,
  WAL_FILE_SYNC_MICROS,
  MANIFEST_FILE_SYNC_MICROS,
  TABLE_OPEN_IO_MICROS,
  DB_MULTIGET,
  READ_BLOCK_COMPACTION_MICROS,
  READ_BLOCK_GET_MICROS,
  WRITE_RAW_BLOCK_MICROS,
  STALL_L0_SLOWDOWN_COUNT,
  STALL_MEMTABLE_COMPACTION_COUNT,
  STALL_L0_NUM_FILES_COUNT,
  HARD_RATE_LIMIT_DELAY_COUNT,
  SOFT_RATE_LIMIT_DELAY_COUNT,
  NUM_FILES_IN_SINGLE_COMPACTION,
  HISTOGRAM_ENUM_MAX,
};
const std::vector<std::pair<Histograms, std::string>> HistogramsNameMap = {
  { DB_GET, "rocksdb.db.get.micros" },
  { DB_WRITE, "rocksdb.db.write.micros" },
  { COMPACTION_TIME, "rocksdb.compaction.times.micros" },
  { TABLE_SYNC_MICROS, "rocksdb.table.sync.micros" },
  { COMPACTION_OUTFILE_SYNC_MICROS, "rocksdb.compaction.outfile.sync.micros" },
  { WAL_FILE_SYNC_MICROS, "rocksdb.wal.file.sync.micros" },
  { MANIFEST_FILE_SYNC_MICROS, "rocksdb.manifest.file.sync.micros" },
  { TABLE_OPEN_IO_MICROS, "rocksdb.table.open.io.micros" },
  { DB_MULTIGET, "rocksdb.db.multiget.micros" },
  { READ_BLOCK_COMPACTION_MICROS, "rocksdb.read.block.compaction.micros" },
  { READ_BLOCK_GET_MICROS, "rocksdb.read.block.get.micros" },
  { WRITE_RAW_BLOCK_MICROS, "rocksdb.write.raw.block.micros" },
  { STALL_L0_SLOWDOWN_COUNT, "rocksdb.l0.slowdown.count"},
  { STALL_MEMTABLE_COMPACTION_COUNT, "rocksdb.memtable.compaction.count"},
  { STALL_L0_NUM_FILES_COUNT, "rocksdb.num.files.stall.count"},
  { HARD_RATE_LIMIT_DELAY_COUNT, "rocksdb.hard.rate.limit.delay.count"},
  { SOFT_RATE_LIMIT_DELAY_COUNT, "rocksdb.soft.rate.limit.delay.count"},
  { NUM_FILES_IN_SINGLE_COMPACTION, "rocksdb.numfiles.in.singlecompaction" },
};
struct HistogramData {
  double median;
  double percentile95;
  double percentile99;
  double average;
  double standard_deviation;
};
class Statistics {
 public:
  virtual ~Statistics() {}
  virtual long getTickerCount(Tickers tickerType) = 0;
  virtual void recordTick(Tickers tickerType, uint64_t count = 0) = 0;
  virtual void setTickerCount(Tickers tickerType, uint64_t count) = 0;
  virtual void measureTime(Histograms histogramType, uint64_t time) = 0;
  virtual void histogramData(Histograms type, HistogramData* const data) = 0;
  std::string ToString();
};
std::shared_ptr<Statistics> CreateDBStatistics();
}
#endif
