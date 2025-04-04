       
#include <atomic>
#include <deque>
#include <set>
#include <utility>
#include <vector>
#include "db/dbformat.h"
#include "db/log_writer.h"
#include "db/snapshot.h"
#include "db/version_edit.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/memtablerep.h"
#include "rocksdb/transaction_log.h"
#include "port/port.h"
#include "util/stats_logger.h"
#include "memtablelist.h"
#include "util/autovector.h"
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
  using DB::Put;
  virtual Status Put(const WriteOptions& options,
                     const ColumnFamilyHandle& column_family, const Slice& key,
                     const Slice& value);
  using DB::Merge;
  virtual Status Merge(const WriteOptions& options,
                       const ColumnFamilyHandle& column_family,
                       const Slice& key, const Slice& value);
  using DB::Delete;
  virtual Status Delete(const WriteOptions& options,
                        const ColumnFamilyHandle& column_family,
                        const Slice& key);
  using DB::Write;
  virtual Status Write(const WriteOptions& options, WriteBatch* updates);
  using DB::Get;
  virtual Status Get(const ReadOptions& options,
                     const ColumnFamilyHandle& column_family, const Slice& key,
                     std::string* value);
  using DB::MultiGet;
  virtual std::vector<Status> MultiGet(
      const ReadOptions& options,
      const std::vector<ColumnFamilyHandle>& column_family,
      const std::vector<Slice>& keys, std::vector<std::string>* values);
  virtual Status CreateColumnFamily(const ColumnFamilyOptions& options,
                                    const std::string& column_family,
                                    ColumnFamilyHandle* handle);
  virtual Status DropColumnFamily(const ColumnFamilyHandle& column_family);
  using DB::KeyMayExist;
  virtual bool KeyMayExist(const ReadOptions& options,
                           const ColumnFamilyHandle& column_family,
                           const Slice& key, std::string* value,
                           bool* value_found = nullptr);
  using DB::NewIterator;
  virtual Iterator* NewIterator(const ReadOptions& options,
                                const ColumnFamilyHandle& column_family);
  virtual Status NewIterators(
      const ReadOptions& options,
      const std::vector<ColumnFamilyHandle>& column_family,
      std::vector<Iterator*>* iterators);
  virtual const Snapshot* GetSnapshot();
  virtual void ReleaseSnapshot(const Snapshot* snapshot);
<<<<<<< HEAD
  using DB::GetProperty;
  virtual bool GetProperty(const ColumnFamilyHandle& column_family,
                           const Slice& property, std::string* value);
  using DB::GetApproximateSizes;
  virtual void GetApproximateSizes(const ColumnFamilyHandle& column_family,
                                   const Range* range, int n, uint64_t* sizes);
  using DB::CompactRange;
  virtual void CompactRange(const ColumnFamilyHandle& column_family,
                            const Slice* begin, const Slice* end,
                            bool reduce_level = false, int target_level = -1);
  using DB::NumberLevels;
  virtual int NumberLevels(const ColumnFamilyHandle& column_family);
  using DB::MaxMemCompactionLevel;
  virtual int MaxMemCompactionLevel(const ColumnFamilyHandle& column_family);
  using DB::Level0StopWriteTrigger;
  virtual int Level0StopWriteTrigger(const ColumnFamilyHandle& column_family);
||||||| fb01755aa
  virtual bool GetProperty(const Slice& property, std::string* value);
  virtual void GetApproximateSizes(const Range* range, int n, uint64_t* sizes);
  virtual void CompactRange(const Slice* begin, const Slice* end,
                            bool reduce_level = false, int target_level = -1);
  virtual int NumberLevels();
  virtual int MaxMemCompactionLevel();
  virtual int Level0StopWriteTrigger();
=======
  virtual bool GetProperty(const Slice& property, std::string* value);
  virtual void GetApproximateSizes(const Range* range, int n, uint64_t* sizes);
  virtual Status CompactRange(const Slice* begin, const Slice* end,
                              bool reduce_level = false, int target_level = -1);
  virtual int NumberLevels();
  virtual int MaxMemCompactionLevel();
  virtual int Level0StopWriteTrigger();
>>>>>>> aba2acb5
  virtual const std::string& GetName() const;
  virtual Env* GetEnv() const;
  using DB::GetOptions;
  virtual const Options& GetOptions(const ColumnFamilyHandle& column_family)
      const;
  using DB::Flush;
  virtual Status Flush(const FlushOptions& options,
                       const ColumnFamilyHandle& column_family);
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
  virtual void GetLiveFilesMetaData(std::vector<LiveFileMetaData>* metadata);
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
    MemTableList imm;
    Version* current;
    std::atomic<uint32_t> refs;
    std::vector<MemTable*> to_delete;
    explicit SuperVersion(const int num_memtables = 0);
    ~SuperVersion();
    SuperVersion* Ref();
    bool Unref();
    void Cleanup();
    void Init(MemTable* new_mem, const MemTableList& new_imm,
              Version* new_current);
  };
  struct DeletionState {
    inline bool HaveSomethingToDelete() const {
      return all_files.size() ||
        sst_delete_files.size() ||
        log_delete_files.size();
    }
    std::vector<std::string> all_files;
    std::vector<uint64_t> sst_live;
    std::vector<FileMetaData*> sst_delete_files;
    std::vector<uint64_t> log_delete_files;
    std::vector<MemTable *> memtables_to_free;
    SuperVersion* superversion_to_free;
    SuperVersion* new_superversion;
    uint64_t manifest_file_number, log_number, prev_log_number;
    explicit DeletionState(const int num_memtables = 0,
                           bool create_superversion = false) {
      manifest_file_number = 0;
      log_number = 0;
      prev_log_number = 0;
      memtables_to_free.reserve(num_memtables);
      superversion_to_free = nullptr;
      new_superversion =
          create_superversion ? new SuperVersion(num_memtables) : nullptr;
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
  Status Recover(const std::vector<ColumnFamilyDescriptor>& column_families,
                 bool read_only = false, bool error_if_log_file_exist = false);
  void MaybeIgnoreError(Status* s) const;
  const Status CreateArchivalDirectory();
  void DeleteObsoleteFiles();
  Status FlushMemTableToOutputFile(bool* madeProgress,
                                   DeletionState& deletion_state);
  Status RecoverLogFile(uint64_t log_number, SequenceNumber* max_sequence,
                        bool read_only);
  Status WriteLevel0TableForRecovery(MemTable* mem, VersionEdit* edit);
  Status WriteLevel0Table(std::vector<MemTable*> &mems, VersionEdit* edit,
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
  MemTableRepFactory* mem_rep_factory_;
  MemTable* mem_;
  MemTableList imm_;
  uint64_t logfile_number_;
  unique_ptr<log::Writer> log_;
  SuperVersion* super_version_;
  std::atomic<uint64_t> super_version_number_;
  std::string host_name_;
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
  uint64_t stall_level0_slowdown_;
  uint64_t stall_memtable_compaction_;
  uint64_t stall_level0_num_files_;
  std::vector<uint64_t> stall_leveln_slowdown_;
  uint64_t stall_level0_slowdown_count_;
  uint64_t stall_memtable_compaction_count_;
  uint64_t stall_level0_num_files_count_;
  std::vector<uint64_t> stall_leveln_slowdown_count_;
  const uint64_t started_at_;
  bool flush_on_destroy_;
  struct CompactionStats {
    uint64_t micros;
    int64_t bytes_readn;
    int64_t bytes_readnp1;
    int64_t bytes_written;
    int files_in_leveln;
    int files_in_levelnp1;
    int files_out_levelnp1;
    int count;
    CompactionStats() : micros(0), bytes_readn(0), bytes_readnp1(0),
                        bytes_written(0), files_in_leveln(0),
                        files_in_levelnp1(0), files_out_levelnp1(0),
                        count(0) { }
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
  std::vector<CompactionStats> stats_;
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
    StatsSnapshot() : compaction_bytes_read_(0), compaction_bytes_written_(0),
                      ingest_bytes_(0), wal_bytes_(0), wal_synced_(0),
                      write_with_wal_(0), write_other_(0), write_self_(0),
                      seconds_up_(0) {}
  };
  StatsSnapshot last_stats_;
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
