       
#include <atomic>
#include <deque>
#include <set>
#include <utility>
#include <vector>
#include "db/dbformat.h"
#include "db/log_writer.h"
#include "db/snapshot.h"
#include "db/version_edit.h"
#include "memtable_list.h"
#include "port/port.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/memtablerep.h"
#include "rocksdb/transaction_log.h"
#include "util/autovector.h"
#include "util/stats_logger.h"
#include "db/internal_stats.h"
namespace rocksdb {
class MemTable;
class TableCache;
class Version;
class VersionEdit;
class VersionSet;
class DBImpl : public DB {
 public:
  DBImpl(const Options& options, const std::string& dbname);
  virtual ~DBImpl();
  virtual Status Put(const WriteOptions&, const Slice& key, const Slice& value);
  virtual Status Merge(const WriteOptions&, const Slice& key,
                       const Slice& value);
  virtual Status Delete(const WriteOptions&, const Slice& key);
  virtual Status Write(const WriteOptions& options, WriteBatch* updates);
  virtual Status Get(const ReadOptions& options,
                     const Slice& key,
                     std::string* value);
  virtual std::vector<Status> MultiGet(const ReadOptions& options,
                                       const std::vector<Slice>& keys,
                                       std::vector<std::string>* values);
  virtual bool KeyMayExist(const ReadOptions& options,
                           const Slice& key,
                           std::string* value,
                           bool* value_found = nullptr);
  virtual Iterator* NewIterator(const ReadOptions&);
  virtual const Snapshot* GetSnapshot();
  virtual void ReleaseSnapshot(const Snapshot* snapshot);
  virtual bool GetProperty(const Slice& property, std::string* value);
  virtual void GetApproximateSizes(const Range* range, int n, uint64_t* sizes);
  virtual Status CompactRange(const Slice* begin, const Slice* end,
                              bool reduce_level = false, int target_level = -1);
  virtual int NumberLevels();
  virtual int MaxMemCompactionLevel();
  virtual int Level0StopWriteTrigger();
  virtual const std::string& GetName() const;
  virtual Env* GetEnv() const;
  virtual const Options& GetOptions() const;
  virtual Status Flush(const FlushOptions& options);
  virtual Status DisableFileDeletions();
  virtual Status EnableFileDeletions(bool force);
  virtual Status GetLiveFiles(std::vector<std::string>&,
                              uint64_t* manifest_file_size,
                              bool flush_memtable = true);
  virtual Status GetSortedWalFiles(VectorLogPtr& files);
  virtual SequenceNumber GetLatestSequenceNumber() const;
  virtual Status GetUpdatesSince(SequenceNumber seq_number,
                                 unique_ptr<TransactionLogIterator>* iter);
  virtual Status DeleteFile(std::string name);
  virtual void GetLiveFilesMetaData(
    std::vector<LiveFileMetaData> *metadata);
  virtual Status GetDbIdentity(std::string& identity);
  Status RunManualCompaction(int input_level,
                             int output_level,
                             const Slice* begin,
                             const Slice* end);
  Status TEST_CompactRange(int level,
                           const Slice* begin,
                           const Slice* end);
  Status TEST_FlushMemTable();
  Status TEST_WaitForFlushMemTable();
  Status TEST_WaitForCompact();
  Iterator* TEST_NewInternalIterator();
  int64_t TEST_MaxNextLevelOverlappingBytes();
  void TEST_Destroy_DBImpl();
  uint64_t TEST_Current_Manifest_FileNo();
  void TEST_PurgeObsoleteteWAL();
  uint64_t TEST_GetLevel0TotalSize();
  void TEST_SetDefaultTimeToCheck(uint64_t default_interval_to_delete_obsolete_WAL)
  {
    default_interval_to_delete_obsolete_WAL_ = default_interval_to_delete_obsolete_WAL;
  }
  struct SuperVersion {
    MemTable* mem;
    MemTableListVersion* imm;
    Version* current;
    std::atomic<uint32_t> refs;
    autovector<MemTable*> to_delete;
    SuperVersion() = default;
    ~SuperVersion();
    SuperVersion* Ref();
    bool Unref();
    void Cleanup();
    void Init(MemTable* new_mem, MemTableListVersion* new_imm,
              Version* new_current);
  };
  struct DeletionState {
    inline bool HaveSomethingToDelete() const {
      return candidate_files.size() ||
        sst_delete_files.size() ||
        log_delete_files.size();
    }
    std::vector<std::string> candidate_files;
    std::vector<uint64_t> sst_live;
    std::vector<FileMetaData*> sst_delete_files;
    std::vector<uint64_t> log_delete_files;
    autovector<MemTable*> memtables_to_free;
    SuperVersion* superversion_to_free;
    SuperVersion* new_superversion;
    uint64_t manifest_file_number, log_number, prev_log_number;
    explicit DeletionState(bool create_superversion = false) {
      manifest_file_number = 0;
      log_number = 0;
      prev_log_number = 0;
      superversion_to_free = nullptr;
      new_superversion =
          create_superversion ? new SuperVersion() : nullptr;
    }
    ~DeletionState() {
      for (auto m : memtables_to_free) {
        delete m;
      }
      delete superversion_to_free;
      delete new_superversion;
    }
  };
  void FindObsoleteFiles(DeletionState& deletion_state,
                         bool force,
                         bool no_full_scan = false);
  void PurgeObsoleteFiles(DeletionState& deletion_state);
 protected:
  Env* const env_;
  const std::string dbname_;
  unique_ptr<VersionSet> versions_;
  const InternalKeyComparator internal_comparator_;
  const Options options_;
  const Comparator* user_comparator() const {
    return internal_comparator_.user_comparator();
  }
  MemTable* GetMemTable() {
    return mem_;
  }
  Iterator* NewInternalIterator(const ReadOptions&,
                                SequenceNumber* latest_snapshot);
 private:
  friend class DB;
  friend class TailingIterator;
  struct CompactionState;
  struct Writer;
  Status NewDB();
  Status Recover(bool read_only = false, bool error_if_log_file_exist = false);
  void MaybeIgnoreError(Status* s) const;
  const Status CreateArchivalDirectory();
  void DeleteObsoleteFiles();
  Status FlushMemTableToOutputFile(bool* madeProgress,
                                   DeletionState& deletion_state);
  Status RecoverLogFile(uint64_t log_number, SequenceNumber* max_sequence,
                        bool read_only);
  Status WriteLevel0TableForRecovery(MemTable* mem, VersionEdit* edit);
  Status WriteLevel0Table(autovector<MemTable*>& mems, VersionEdit* edit,
                                uint64_t* filenumber);
  uint64_t SlowdownAmount(int n, double bottom, double top);
  Status MakeRoomForWrite(bool force ,
                          SuperVersion** superversion_to_free);
  void BuildBatchGroup(Writer** last_writer,
                       autovector<WriteBatch*>* write_batch_group);
  Status FlushMemTable(const FlushOptions& options);
  Status WaitForFlushMemTable();
  void MaybeScheduleLogDBDeployStats();
  static void BGLogDBDeployStats(void* db);
  void LogDBDeployStats();
  void MaybeScheduleFlushOrCompaction();
  static void BGWorkCompaction(void* db);
  static void BGWorkFlush(void* db);
  void BackgroundCallCompaction();
  void BackgroundCallFlush();
  Status BackgroundCompaction(bool* madeProgress,DeletionState& deletion_state);
  Status BackgroundFlush(bool* madeProgress, DeletionState& deletion_state);
  void CleanupCompaction(CompactionState* compact, Status status);
  Status DoCompactionWork(CompactionState* compact,
                          DeletionState& deletion_state);
  Status OpenCompactionOutputFile(CompactionState* compact);
  Status FinishCompactionOutputFile(CompactionState* compact, Iterator* input);
  Status InstallCompactionResults(CompactionState* compact);
  void AllocateCompactionOutputFileNumbers(CompactionState* compact);
  void ReleaseCompactionUnusedFileNumbers(CompactionState* compact);
  void PurgeObsoleteWALFiles();
  Status AppendSortedWalsOfType(const std::string& path,
                                VectorLogPtr& log_files,
                                WalFileType type);
  Status RetainProbableWalFiles(VectorLogPtr& all_logs,
                                const SequenceNumber target);
  bool CheckWalFileExistsAndEmpty(const WalFileType type,
                                  const uint64_t number);
  Status ReadFirstRecord(const WalFileType type, const uint64_t number,
                         WriteBatch* const result);
  Status ReadFirstLine(const std::string& fname, WriteBatch* const batch);
  void PrintStatistics();
  void MaybeDumpStats();
  int FindMinimumEmptyLevelFitting(int level);
  Status ReFitLevel(int level, int target_level = -1);
  uint64_t CurrentVersionNumber() const;
  std::pair<Iterator*, Iterator*> GetTailingIteratorPair(
    const ReadOptions& options,
    uint64_t* superversion_number);
  const InternalFilterPolicy internal_filter_policy_;
  bool owns_info_log_;
  unique_ptr<TableCache> table_cache_;
  FileLock* db_lock_;
  port::Mutex mutex_;
  port::AtomicPointer shutting_down_;
  port::CondVar bg_cv_;
  MemTable* mem_;
  MemTableList imm_;
  uint64_t logfile_number_;
  unique_ptr<log::Writer> log_;
  SuperVersion* super_version_;
  std::atomic<uint64_t> super_version_number_;
  std::string host_name_;
  std::unique_ptr<Directory> db_directory_;
  std::deque<Writer*> writers_;
  WriteBatch tmp_batch_;
  SnapshotList snapshots_;
  std::set<uint64_t> pending_outputs_;
  int bg_compaction_scheduled_;
  int bg_manual_only_;
  int bg_flush_scheduled_;
  bool bg_logstats_scheduled_;
  struct ManualCompaction {
    int input_level;
    int output_level;
    bool done;
    Status status;
    bool in_progress;
    const InternalKey* begin;
    const InternalKey* end;
    InternalKey tmp_storage;
  };
  ManualCompaction* manual_compaction_;
  Status bg_error_;
  std::unique_ptr<StatsLogger> logger_;
  int64_t volatile last_log_ts;
  int disable_delete_obsolete_files_;
  uint64_t delete_obsolete_files_last_run_;
  uint64_t purge_wal_files_last_run_;
  std::atomic<uint64_t> last_stats_dump_time_microsec_;
  uint64_t default_interval_to_delete_obsolete_WAL_;
  bool flush_on_destroy_;
  InternalStats internal_stats_;
  static const int KEEP_LOG_FILE_NUM = 1000;
  std::string db_absolute_path_;
  int delayed_writes_;
  const EnvOptions storage_options_;
  bool bg_work_gate_closed_;
  bool refitting_level_;
  DBImpl(const DBImpl&);
  void operator=(const DBImpl&);
  void DelayLoggingAndReset();
  inline SequenceNumber findEarliestVisibleSnapshot(
    SequenceNumber in,
    std::vector<SequenceNumber>& snapshots,
    SequenceNumber* prev_snapshot);
  SuperVersion* InstallSuperVersion(SuperVersion* new_superversion);
  void InstallSuperVersion(DeletionState& deletion_state);
  Status GetImpl(const ReadOptions& options,
                 const Slice& key,
                 std::string* value,
                 bool* value_found = nullptr);
};
extern Options SanitizeOptions(const std::string& db,
                               const InternalKeyComparator* icmp,
                               const InternalFilterPolicy* ipolicy,
                               const Options& src);
CompressionType GetCompressionType(const Options& options, int level,
                                   const bool enable_compression);
CompressionType GetCompressionFlush(const Options& options);
}
