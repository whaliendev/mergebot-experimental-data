#ifndef STORAGE_LEVELDB_DB_DB_IMPL_H_
#define STORAGE_LEVELDB_DB_DB_IMPL_H_ 
#include <deque>
#include <set>
#include "db/dbformat.h"
#include "db/log_writer.h"
#include "db/snapshot.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "port/port.h"
#include "util/stats_logger.h"
#include "memtablelist.h"
#ifdef USE_SCRIBE
#include "scribe/scribe_logger.h"
#endif
namespace leveldb {
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
  virtual Status Delete(const WriteOptions&, const Slice& key);
  virtual Status Write(const WriteOptions& options, WriteBatch* updates);
  virtual Status Get(const ReadOptions& options,
                     const Slice& key,
                     std::string* value);
  virtual Iterator* NewIterator(const ReadOptions&);
  virtual const Snapshot* GetSnapshot();
  virtual void ReleaseSnapshot(const Snapshot* snapshot);
  virtual bool GetProperty(const Slice& property, std::string* value);
  virtual void GetApproximateSizes(const Range* range, int n, uint64_t* sizes);
  virtual void CompactRange(const Slice* begin, const Slice* end);
  virtual int NumberLevels();
  virtual int MaxMemCompactionLevel();
  virtual int Level0StopWriteTrigger();
  virtual Status Flush(const FlushOptions& options);
  virtual Status DisableFileDeletions();
  virtual Status EnableFileDeletions();
  virtual Status GetLiveFiles(std::vector<std::string>&,
                              uint64_t* manifest_file_size);
  void TEST_CompactRange(int level, const Slice* begin, const Slice* end);
  Status TEST_CompactMemTable();
  Status TEST_WaitForCompactMemTable();
  Status TEST_WaitForCompact();
  Iterator* TEST_NewInternalIterator();
  int64_t TEST_MaxNextLevelOverlappingBytes();
 private:
  friend class DB;
  struct CompactionState;
  struct Writer;
  struct DeletionState;
  Iterator* NewInternalIterator(const ReadOptions&,
                                SequenceNumber* latest_snapshot);
  Status NewDB();
  Status Recover(VersionEdit* edit);
  void MaybeIgnoreError(Status* s) const;
  void DeleteObsoleteFiles();
  Status CompactMemTable(bool* madeProgress = NULL);
  Status RecoverLogFile(uint64_t log_number,
                        VersionEdit* edit,
                        SequenceNumber* max_sequence);
  Status WriteLevel0TableForRecovery(MemTable* mem, VersionEdit* edit);
  Status WriteLevel0Table(MemTable* mem, VersionEdit* edit,
                                uint64_t* filenumber);
  Status MakeRoomForWrite(bool force );
  WriteBatch* BuildBatchGroup(Writer** last_writer);
  Status FlushMemTable(const FlushOptions& options);
  Status WaitForCompactMemTable();
  void MaybeScheduleLogDBDeployStats();
  static void BGLogDBDeployStats(void* db);
  void LogDBDeployStats();
  void MaybeScheduleCompaction();
  static void BGWork(void* db);
  void BackgroundCall();
  Status BackgroundCompaction(bool* madeProgress, DeletionState& deletion_state);
  void CleanupCompaction(CompactionState* compact);
  Status DoCompactionWork(CompactionState* compact);
  Status OpenCompactionOutputFile(CompactionState* compact);
  Status FinishCompactionOutputFile(CompactionState* compact, Iterator* input);
  Status InstallCompactionResults(CompactionState* compact);
  void AllocateCompactionOutputFileNumbers(CompactionState* compact);
  void ReleaseCompactionUnusedFileNumbers(CompactionState* compact);
  void FindObsoleteFiles(DeletionState& deletion_state);
  void PurgeObsoleteFiles(DeletionState& deletion_state);
  void EvictObsoleteFiles(DeletionState& deletion_state);
  Env* const env_;
  const InternalKeyComparator internal_comparator_;
  const InternalFilterPolicy internal_filter_policy_;
  const Options options_;
  bool owns_info_log_;
  bool owns_cache_;
  const std::string dbname_;
  TableCache* table_cache_;
  FileLock* db_lock_;
  port::Mutex mutex_;
  port::AtomicPointer shutting_down_;
  port::CondVar bg_cv_;
  MemTable* mem_;
  MemTableList imm_;
  WritableFile* logfile_;
  uint64_t logfile_number_;
  log::Writer* log_;
  std::string host_name_;
  std::deque<Writer*> writers_;
  WriteBatch* tmp_batch_;
  SnapshotList snapshots_;
  std::set<uint64_t> pending_outputs_;
  int bg_compaction_scheduled_;
  bool bg_logstats_scheduled_;
  struct ManualCompaction {
    int level;
    bool done;
    bool in_progress;
    const InternalKey* begin;
    const InternalKey* end;
    InternalKey tmp_storage;
  };
  ManualCompaction* manual_compaction_;
  VersionSet* versions_;
  Status bg_error_;
  StatsLogger* logger_;
  int64_t volatile last_log_ts;
  bool disable_delete_obsolete_files_;
  uint64_t delete_obsolete_files_last_run_;
  struct CompactionStats {
    int64_t micros;
    int64_t bytes_read;
    int64_t bytes_written;
    CompactionStats() : micros(0), bytes_read(0), bytes_written(0) { }
    void Add(const CompactionStats& c) {
      this->micros += c.micros;
      this->bytes_read += c.bytes_read;
      this->bytes_written += c.bytes_written;
    }
  };
  CompactionStats* stats_;
  static const int KEEP_LOG_FILE_NUM = 1000;
  std::string db_absolute_path_;
  int delayed_writes_;
  DBImpl(const DBImpl&);
  void operator=(const DBImpl&);
  const Comparator* user_comparator() const {
    return internal_comparator_.user_comparator();
  }
  void DelayLoggingAndReset();
};
extern Options SanitizeOptions(const std::string& db,
                               const InternalKeyComparator* icmp,
                               const InternalFilterPolicy* ipolicy,
                               const Options& src);
}
#endif
