       
#include "rocksdb/statistics.h"
#include "util/statistics.h"
#include "db/version_set.h"
#include <vector>
#include <string>
class ColumnFamilyData;
namespace rocksdb {
class MemTableList;
enum DBPropertyType {
  kNumFilesAtLevel,
  kLevelStats,
  kStats,
  kSsTables,
  kNumImmutableMemTable,
  kMemtableFlushPending,
  kCompactionPending,
  kBackgroundErrors,
  kUnknown,
};
extern DBPropertyType GetPropertyType(const Slice& property);
class InternalStats {
 public:
  enum WriteStallType {
    LEVEL0_SLOWDOWN,
    MEMTABLE_COMPACTION,
    LEVEL0_NUM_FILES,
    WRITE_STALLS_ENUM_MAX,
  };
  InternalStats(int num_levels, Env* env, Statistics* statistics)
      : compaction_stats_(num_levels),
        stall_micros_(WRITE_STALLS_ENUM_MAX, 0),
        stall_counts_(WRITE_STALLS_ENUM_MAX, 0),
        stall_leveln_slowdown_(num_levels, 0),
        stall_leveln_slowdown_count_(num_levels, 0),
        bg_error_count_(0),
        number_levels_(num_levels),
        statistics_(statistics),
        env_(env),
        started_at_(env->NowMicros()) {}
  struct CompactionStats {
    uint64_t micros;
    int64_t bytes_readn;
    int64_t bytes_readnp1;
    int64_t bytes_written;
    int files_in_leveln;
    int files_in_levelnp1;
    int files_out_levelnp1;
    int count;
    CompactionStats()
        : micros(0),
          bytes_readn(0),
          bytes_readnp1(0),
          bytes_written(0),
          files_in_leveln(0),
          files_in_levelnp1(0),
          files_out_levelnp1(0),
          count(0) {}
    void Add(const CompactionStats& c) {
      this->micros += c.micros;
      this->bytes_readn += c.bytes_readn;
      this->bytes_readnp1 += c.bytes_readnp1;
      this->bytes_written += c.bytes_written;
      this->files_in_leveln += c.files_in_leveln;
      this->files_in_levelnp1 += c.files_in_levelnp1;
      this->files_out_levelnp1 += c.files_out_levelnp1;
      this->count += 1;
    }
  };
  void AddCompactionStats(int level, const CompactionStats& stats) {
    compaction_stats_[level].Add(stats);
  }
  void RecordWriteStall(WriteStallType write_stall_type, uint64_t micros) {
    stall_micros_[write_stall_type] += micros;
    stall_counts_[write_stall_type]++;
  }
  void RecordLevelNSlowdown(int level, uint64_t micros) {
    stall_leveln_slowdown_[level] += micros;
    stall_leveln_slowdown_count_[level] += micros;
  }
<<<<<<< HEAD
  bool GetProperty(const Slice& property, std::string* value,
                   ColumnFamilyData* cfd);
||||||| 758fa8c35
  bool GetProperty(const Slice& property, std::string* value,
                   VersionSet* version_set, int immsize);
=======
  uint64_t GetBackgroundErrorCount() const { return bg_error_count_; }
  uint64_t BumpAndGetBackgroundErrorCount() { return ++bg_error_count_; }
  bool GetProperty(DBPropertyType property_type, const Slice& property,
                   std::string* value, VersionSet* version_set,
                   const MemTableList& imm);
>>>>>>> fcd5c5e8
 private:
  std::vector<CompactionStats> compaction_stats_;
  struct StatsSnapshot {
    uint64_t compaction_bytes_read_;
    uint64_t compaction_bytes_written_;
    uint64_t ingest_bytes_;
    uint64_t wal_bytes_;
    uint64_t wal_synced_;
    uint64_t write_with_wal_;
    uint64_t write_other_;
    uint64_t write_self_;
    double seconds_up_;
    StatsSnapshot()
        : compaction_bytes_read_(0),
          compaction_bytes_written_(0),
          ingest_bytes_(0),
          wal_bytes_(0),
          wal_synced_(0),
          write_with_wal_(0),
          write_other_(0),
          write_self_(0),
          seconds_up_(0) {}
  };
  StatsSnapshot last_stats_;
  std::vector<uint64_t> stall_micros_;
  std::vector<uint64_t> stall_counts_;
  std::vector<uint64_t> stall_leveln_slowdown_;
  std::vector<uint64_t> stall_leveln_slowdown_count_;
  uint64_t bg_error_count_;
  int number_levels_;
  Statistics* statistics_;
  Env* env_;
  uint64_t started_at_;
};
}
