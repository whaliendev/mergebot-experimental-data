       
#include <atomic>
#include <deque>
#include <set>
#include <utility>
#include <vector>
#include <string>
#include "db/dbformat.h"
#include "db/log_writer.h"
#include "db/snapshot.h"
#include "db/column_family.h"
#include "db/version_edit.h"
#include "memtable_list.h"
#include "port/port.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/memtablerep.h"
#include "rocksdb/transaction_log.h"
#include "util/autovector.h"
#include "util/stats_logger.h"
#include "util/thread_local.h"
#include "db/internal_stats.h"
namespace rocksdb {
class MemTable;
class TableCache;
class Version;
class VersionEdit;
class VersionSet;
class DBImpl : public DB {
 public:
  DBImpl(const DBOptions& options, const std::string& dbname);
  virtual ~DBImpl();
  using DB::Put;
  virtual Status Put(const WriteOptions& options,
                     ColumnFamilyHandle* column_family, const Slice& key,
                     const Slice& value);
  using DB::Merge;
  virtual Status Merge(const WriteOptions& options,
                       ColumnFamilyHandle* column_family, const Slice& key,
                       const Slice& value);
  using DB::Delete;
  virtual Status Delete(const WriteOptions& options,
                        ColumnFamilyHandle* column_family, const Slice& key);
  using DB::Write;
  virtual Status Write(const WriteOptions& options, WriteBatch* updates);
  using DB::Get;
  virtual Status Get(const ReadOptions& options,
                     ColumnFamilyHandle* column_family, const Slice& key,
                     std::string* value);
  using DB::MultiGet;
  virtual std::vector<Status> MultiGet(
      const ReadOptions& options,
      const std::vector<ColumnFamilyHandle*>& column_family,
      const std::vector<Slice>& keys, std::vector<std::string>* values);
  virtual Status CreateColumnFamily(const ColumnFamilyOptions& options,
                                    const std::string& column_family,
                                    ColumnFamilyHandle** handle);
  virtual Status DropColumnFamily(ColumnFamilyHandle* column_family);
  using DB::KeyMayExist;
  virtual bool KeyMayExist(const ReadOptions& options,
                           ColumnFamilyHandle* column_family, const Slice& key,
                           std::string* value, bool* value_found = nullptr);
  using DB::NewIterator;
  virtual Iterator* NewIterator(const ReadOptions& options,
                                ColumnFamilyHandle* column_family);
  virtual Status NewIterators(
      const ReadOptions& options,
      const std::vector<ColumnFamilyHandle*>& column_families,
      std::vector<Iterator*>* iterators);
  virtual const Snapshot* GetSnapshot();
  virtual void ReleaseSnapshot(const Snapshot* snapshot);
  using DB::GetProperty;
  virtual bool GetProperty(ColumnFamilyHandle* column_family,
                           const Slice& property, std::string* value);
  using DB::GetApproximateSizes;
  virtual void GetApproximateSizes(ColumnFamilyHandle* column_family,
                                   const Range* range, int n, uint64_t* sizes);
  using DB::CompactRange;
  virtual Status CompactRange(ColumnFamilyHandle* column_family,
                              const Slice* begin, const Slice* end,
                              bool reduce_level = false, int target_level = -1);
  using DB::NumberLevels;
  virtual int NumberLevels(ColumnFamilyHandle* column_family);
  using DB::MaxMemCompactionLevel;
  virtual int MaxMemCompactionLevel(ColumnFamilyHandle* column_family);
  using DB::Level0StopWriteTrigger;
  virtual int Level0StopWriteTrigger(ColumnFamilyHandle* column_family);
  virtual const std::string& GetName() const;
  virtual Env* GetEnv() const;
  using DB::GetOptions;
  virtual const Options& GetOptions(ColumnFamilyHandle* column_family) const;
  using DB::Flush;
  virtual Status Flush(const FlushOptions& options,
                       ColumnFamilyHandle* column_family);
  virtual Status DisableFileDeletions();
  virtual Status EnableFileDeletions(bool force);
  virtual Status GetLiveFiles(std::vector<std::string>&,
                              uint64_t* manifest_file_size,
                              bool flush_memtable = true);
  virtual Status GetSortedWalFiles(VectorLogPtr& files);
  virtual SequenceNumber GetLatestSequenceNumber() const;
  virtual Status GetUpdatesSince(
      SequenceNumber seq_number, unique_ptr<TransactionLogIterator>* iter,
      const TransactionLogIterator::ReadOptions&
          read_options = TransactionLogIterator::ReadOptions());
  virtual Status DeleteFile(std::string name);
  virtual void GetLiveFilesMetaData(std::vector<LiveFileMetaData>* metadata);
  virtual Status GetDbIdentity(std::string& identity);
  Status RunManualCompaction(ColumnFamilyData* cfd, int input_level,
                             int output_level, const Slice* begin,
                             const Slice* end);
  Status TEST_CompactRange(int level, const Slice* begin, const Slice* end,
                           ColumnFamilyHandle* column_family = nullptr);
  Status TEST_FlushMemTable();
  Status TEST_WaitForFlushMemTable(ColumnFamilyHandle* column_family = nullptr);
  Status TEST_WaitForCompact();
  Iterator* TEST_NewInternalIterator(ColumnFamilyHandle* column_family =
                                         nullptr);
  int64_t TEST_MaxNextLevelOverlappingBytes(ColumnFamilyHandle* column_family =
                                                nullptr);
  void TEST_Destroy_DBImpl();
  uint64_t TEST_Current_Manifest_FileNo();
  void TEST_PurgeObsoleteteWAL();
  uint64_t TEST_GetLevel0TotalSize();
  void TEST_SetDefaultTimeToCheck(uint64_t default_interval_to_delete_obsolete_WAL)
  {
    default_interval_to_delete_obsolete_WAL_ = default_interval_to_delete_obsolete_WAL;
  }
  void TEST_GetFilesMetaData(ColumnFamilyHandle* column_family,
                             std::vector<std::vector<FileMetaData>>* metadata);
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
    autovector<SuperVersion*> superversions_to_free;
    SuperVersion* new_superversion;
    uint64_t manifest_file_number, log_number, prev_log_number;
    explicit DeletionState(bool create_superversion = false) {
      manifest_file_number = 0;
      log_number = 0;
      prev_log_number = 0;
      new_superversion = create_superversion ? new SuperVersion() : nullptr;
    }
    ~DeletionState() {
      for (auto m : memtables_to_free) {
        delete m;
      }
      for (auto s : superversions_to_free) {
        delete s;
      }
      delete new_superversion;
    }
  };
  void FindObsoleteFiles(DeletionState& deletion_state,
                         bool force,
                         bool no_full_scan = false);
  void PurgeObsoleteFiles(DeletionState& deletion_state);
  ColumnFamilyHandle* DefaultColumnFamily() const;
 protected:
  Env* const env_;
  const std::string dbname_;
  unique_ptr<VersionSet> versions_;
  const DBOptions options_;
  Iterator* NewInternalIterator(const ReadOptions&, ColumnFamilyData* cfd,
                                SuperVersion* super_version);
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
  Status FlushMemTableToOutputFile(ColumnFamilyData* cfd, bool* madeProgress,
                                   DeletionState& deletion_state);
  Status RecoverLogFile(uint64_t log_number, SequenceNumber* max_sequence,
                        bool read_only);
  Status WriteLevel0TableForRecovery(ColumnFamilyData* cfd, MemTable* mem,
                                     VersionEdit* edit);
  Status WriteLevel0Table(ColumnFamilyData* cfd, autovector<MemTable*>& mems,
                          VersionEdit* edit, uint64_t* filenumber);
  uint64_t SlowdownAmount(int n, double bottom, double top);
  Status MakeRoomForWrite(ColumnFamilyData* cfd,
                          bool force );
  void BuildBatchGroup(Writer** last_writer,
                       autovector<WriteBatch*>* write_batch_group);
  Status FlushMemTable(ColumnFamilyData* cfd, const FlushOptions& options);
  Status WaitForFlushMemTable(ColumnFamilyData* cfd);
  void MaybeScheduleLogDBDeployStats();
  static void BGLogDBDeployStats(void* db);
  void LogDBDeployStats();
  void MaybeScheduleFlushOrCompaction();
  static void BGWorkCompaction(void* db);
  static void BGWorkFlush(void* db);
  void BackgroundCallCompaction();
  void BackgroundCallFlush();
  Status BackgroundCompaction(bool* madeProgress, DeletionState& deletion_state,
                              LogBuffer* log_buffer);
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
  int FindMinimumEmptyLevelFitting(ColumnFamilyData* cfd, int level);
  Status ReFitLevel(ColumnFamilyData* cfd, int level, int target_level = -1);
  std::pair<Iterator*, Iterator*> GetTailingIteratorPair(
      const ReadOptions& options, ColumnFamilyData* cfd,
      uint64_t* superversion_number);
  std::shared_ptr<Cache> table_cache_;
  FileLock* db_lock_;
  port::Mutex mutex_;
  port::AtomicPointer shutting_down_;
  port::CondVar bg_cv_;
  uint64_t logfile_number_;
  unique_ptr<log::Writer> log_;
  ColumnFamilyHandleImpl* default_cf_handle_;
  unique_ptr<ColumnFamilyMemTablesImpl> column_family_memtables_;
  std::deque<uint64_t> alive_log_files_;
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
    ColumnFamilyData* cfd;
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
  static const int KEEP_LOG_FILE_NUM = 1000;
  std::string db_absolute_path_;
  int delayed_writes_;
  const EnvOptions storage_options_;
  bool bg_work_gate_closed_;
  bool refitting_level_;
  bool opened_successfully_;
  DBImpl(const DBImpl&);
  void operator=(const DBImpl&);
  void DelayLoggingAndReset();
  inline SequenceNumber findEarliestVisibleSnapshot(
    SequenceNumber in,
    std::vector<SequenceNumber>& snapshots,
    SequenceNumber* prev_snapshot);
  void InstallSuperVersion(ColumnFamilyData* cfd,
                           DeletionState& deletion_state);
  using DB::GetPropertiesOfAllTables;
  virtual Status GetPropertiesOfAllTables(ColumnFamilyHandle* column_family,
                                          TablePropertiesCollection* props)
      override;
  Status GetImpl(const ReadOptions& options, ColumnFamilyHandle* column_family,
                 const Slice& key, std::string* value,
                 bool* value_found = nullptr);
};
extern Options SanitizeOptions(const std::string& db,
                               const InternalKeyComparator* icmp,
                               const InternalFilterPolicy* ipolicy,
                               const Options& src);
extern DBOptions SanitizeOptions(const std::string& db, const DBOptions& src);
CompressionType GetCompressionType(const Options& options, int level,
                                   const bool enable_compression);
CompressionType GetCompressionFlush(const Options& options);
}
