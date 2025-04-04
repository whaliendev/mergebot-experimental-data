// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/db_impl.h"

#include <algorithm>
#include <set>
#include <string>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include <algorithm>
#include "db/builder.h"
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/memtablelist.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/status.h"
#include "leveldb/table.h"
#include "leveldb/table_builder.h"
#include "port/port.h"
#include "table/block.h"
#include "table/merger.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/build_version.h"
#include "util/auto_split_logger.h"

namespace leveldb {

void dumpLeveldbBuildVersion(Logger * log);

static Status NewLogger(const std::string& dbname,
                        const std::string& db_log_dir,
                        Env* env,
                        size_t max_log_file_size,
                        Logger** logger) {
  std::string db_absolute_path;
  env->GetAbsolutePath(dbname, &db_absolute_path);

  if (max_log_file_size > 0) { // need to auto split the log file?
    AutoSplitLogger<Logger>* auto_split_logger =
      new AutoSplitLogger<Logger>(env, dbname, db_log_dir, max_log_file_size);
    Status s = auto_split_logger->GetStatus();
    if (!s.ok()) {
      delete auto_split_logger;
    } else {
      *logger = auto_split_logger;
    }
    return s;
  } else {
    // Open a log file in the same directory as the db
    env->CreateDir(dbname);  // In case it does not exist
    std::string fname = InfoLogFileName(dbname, db_absolute_path, db_log_dir);
    env->RenameFile(fname, OldInfoLogFileName(dbname, env->NowMicros(),
                                              db_absolute_path, db_log_dir));
    return env->NewLogger(fname, logger);
  }
}

// Information kept for every waiting writer
struct DBImpl::Writer {
  Status status;
  WriteBatch* batch;
  bool sync;
  bool disableWAL;
  bool done;
  port::CondVar cv;

  explicit Writer(port::Mutex* mu) : cv(mu) { }
};

struct DBImpl::CompactionState {
  Compaction* const compaction;

  // Sequence numbers < smallest_snapshot are not significant since we
  // will never have to service a snapshot below smallest_snapshot.
  // Therefore if we have seen a sequence number S <= smallest_snapshot,
  // we can drop all entries for the same key with sequence numbers < S.
  SequenceNumber smallest_snapshot;

  // Files produced by compaction
  struct Output {
    uint64_t number;
    uint64_t file_size;
    InternalKey smallest, largest;
  };
  std::vector<Output> outputs;
  std::list<uint64_t> allocated_file_numbers;

  // State kept for output being generated
  WritableFile* outfile;
  TableBuilder* builder;

  uint64_t total_bytes;

  Output* current_output() { return &outputs[outputs.size()-1]; }

  explicit CompactionState(Compaction* c)
      : compaction(c),
        outfile(NULL),
        builder(NULL),
        total_bytes(0) {
  }
};

struct DBImpl::DeletionState {

  // the set of all live files that cannot be deleted
  std::set<uint64_t> live;

  // a list of all siles that exists in the db directory
  std::vector<std::string> allfiles;

  // the current filenumber, lognumber and prevlognumber
  // that corresponds to the set of files in 'live'.
  uint64_t filenumber, lognumber, prevlognumber;

  // the list of all files to be evicted from the table cahce
  std::vector<uint64_t> files_to_evict;
};

// Fix user-supplied options to be reasonable
template <class T,class V>
static void ClipToRange(T* ptr, V minvalue, V maxvalue) {
  if (static_cast<V>(*ptr) > maxvalue) *ptr = maxvalue;
  if (static_cast<V>(*ptr) < minvalue) *ptr = minvalue;
}
Options SanitizeOptions(const std::string& dbname,
                        const InternalKeyComparator* icmp,
                        const InternalFilterPolicy* ipolicy,
                        const Options& src) {
  Options result = src;
  result.comparator = icmp;
  result.filter_policy = (src.filter_policy != NULL) ? ipolicy : NULL;
  ClipToRange(&result.max_open_files,            20,     50000);
  ClipToRange(&result.write_buffer_size,         64<<10, 1<<30);
  ClipToRange(&result.block_size,                1<<10,  4<<20);
  if (result.info_log == NULL) {
    Status s = NewLogger(dbname, result.db_log_dir, src.env,
                         result.max_log_file_size, &result.info_log);
    if (!s.ok()) {
      // No place suitable for logging
      result.info_log = NULL;
    }
  }
  if (result.block_cache == NULL && !result.no_block_cache) {
    result.block_cache = NewLRUCache(8 << 20);
  }
  if (src.compression_per_level != NULL) {
    result.compression_per_level = new CompressionType[src.num_levels];
    for (unsigned int i = 0; i < src.num_levels; i++) {
      result.compression_per_level[i] = src.compression_per_level[i];
    }
  }
  return result;
}

DBImpl::DBImpl(const Options& options, const std::string& dbname)
    : env_(options.env),
      internal_comparator_(options.comparator),
      internal_filter_policy_(options.filter_policy),
      options_(SanitizeOptions(
          dbname, &internal_comparator_, &internal_filter_policy_, options)),
      owns_info_log_(options_.info_log != options.info_log),
      owns_cache_(options_.block_cache != options.block_cache),
      dbname_(dbname),
      db_lock_(NULL),
      shutting_down_(NULL),
      bg_cv_(&mutex_),
      mem_(new MemTable(internal_comparator_, NumberLevels())),
      logfile_(NULL),
      logfile_number_(0),
      log_(NULL),
      tmp_batch_(new WriteBatch),
      bg_compaction_scheduled_(0),
      bg_logstats_scheduled_(false),
      manual_compaction_(NULL),
      logger_(NULL),
      disable_delete_obsolete_files_(false),
      delete_obsolete_files_last_run_(0),
      stall_level0_slowdown_(0),
      stall_leveln_slowdown_(0),
      stall_memtable_compaction_(0),
      stall_level0_num_files_(0),
      started_at_(options.env->NowMicros()),
      delayed_writes_(0) {
  mem_->Ref();

  env_->GetAbsolutePath(dbname, &db_absolute_path_);
  stats_ = new CompactionStats[options.num_levels];
  // Reserve ten files or so for other uses and give the rest to TableCache.
  const int table_cache_size = options_.max_open_files - 10;
  table_cache_ = new TableCache(dbname_, &options_, table_cache_size);

  versions_ = new VersionSet(dbname_, &options_, table_cache_,
                             &internal_comparator_);

  dumpLeveldbBuildVersion(options_.info_log);
  options_.Dump(options_.info_log);

#ifdef USE_SCRIBE
  logger_ = new ScribeLogger("localhost", 1456);
#endif

  char name[100];
  Status st = env_->GetHostName(name, 100L);
  if(st.ok()) {
    host_name_ = name;
  } else {
    Log(options_.info_log, "Can't get hostname, use localhost as host name.");
    host_name_ = "localhost";
  }
  last_log_ts = 0;
}

DBImpl::~DBImpl() {
  // Wait for background work to finish
  mutex_.Lock();
  shutting_down_.Release_Store(this);  // Any non-NULL value is ok
  while (bg_compaction_scheduled_ || bg_logstats_scheduled_) {
    bg_cv_.Wait();
  }
  mutex_.Unlock();

  if (db_lock_ != NULL) {
    env_->UnlockFile(db_lock_);
  }

  delete versions_;
  if (mem_ != NULL) mem_->Unref();
  imm_.UnrefAll();
  delete tmp_batch_;
  delete log_;
  delete logfile_;
  delete table_cache_;
  delete[] stats_;

  if (owns_info_log_) {
    delete options_.info_log;
  }
  if (owns_cache_) {
    delete options_.block_cache;
  }
  if (options_.compression_per_level != NULL) {
    delete options_.compression_per_level;
  }

  delete logger_;
}

Status DBImpl::NewDB() {
  VersionEdit new_db(NumberLevels());
  new_db.SetComparatorName(user_comparator()->Name());
  new_db.SetLogNumber(0);
  new_db.SetNextFile(2);
  new_db.SetLastSequence(0);

  const std::string manifest = DescriptorFileName(dbname_, 1);
  WritableFile* file;
  Status s = env_->NewWritableFile(manifest, &file);
  if (!s.ok()) {
    return s;
  }
  {
    log::Writer log(file);
    std::string record;
    new_db.EncodeTo(&record);
    s = log.AddRecord(record);
    if (s.ok()) {
      s = file->Close();
    }
  }
  delete file;
  if (s.ok()) {
    // Make "CURRENT" file that points to the new manifest file.
    s = SetCurrentFile(env_, dbname_, 1);
  } else {
    env_->DeleteFile(manifest);
  }
  return s;
}

void DBImpl::MaybeIgnoreError(Status* s) const {
  if (s->ok() || options_.paranoid_checks) {
    // No change needed
  } else {
    Log(options_.info_log, "Ignoring error %s", s->ToString().c_str());
    *s = Status::OK();
  }
}

// Returns the list of live files in 'live' and the list
// of all files in the filesystem in 'allfiles'.
void DBImpl::FindObsoleteFiles(DeletionState& deletion_state) {
  mutex_.AssertHeld();

  // if deletion is disabled, do nothing
  if (disable_delete_obsolete_files_) {
    return;
  }

  // This method is costly when the number of files is large.
  // Do not allow it to trigger more often than once in
  // delete_obsolete_files_period_micros.
  if (options_.delete_obsolete_files_period_micros != 0) {
    const uint64_t now_micros = env_->NowMicros();
    if (delete_obsolete_files_last_run_ + 
        options_.delete_obsolete_files_period_micros > now_micros) {
      return;
    }
    delete_obsolete_files_last_run_ = now_micros;
  }

  // Make a set of all of the live files
  deletion_state.live = pending_outputs_;
  versions_->AddLiveFiles(&deletion_state.live);

  // set of all files in the directory
  env_->GetChildren(dbname_, &deletion_state.allfiles); // Ignore errors

  // store the current filenum, lognum, etc
  deletion_state.filenumber = versions_->ManifestFileNumber();
  deletion_state.lognumber = versions_->LogNumber();
  deletion_state.prevlognumber = versions_->PrevLogNumber();
}

// Diffs the files listed in filenames and those that do not
// belong to live files are posibly removed. If the removed file
// is a sst file, then it returns the file number in files_to_evict.
// It is not necesary to hold the mutex when invoking this method.
void DBImpl::PurgeObsoleteFiles(DeletionState& state) {
  uint64_t number;
  FileType type;
  std::vector<std::string> old_log_files;
  for (size_t i = 0; i < state.allfiles.size(); i++) {
    if (ParseFileName(state.allfiles[i], &number, &type)) {
      bool keep = true;
      switch (type) {
        case kLogFile:
          keep = ((number >= state.lognumber) ||
                  (number == state.prevlognumber));
          break;
        case kDescriptorFile:
          // Keep my manifest file, and any newer incarnations'
          // (in case there is a race that allows other incarnations)
          keep = (number >= state.filenumber);
          break;
        case kTableFile:
          keep = (state.live.find(number) != state.live.end());
          break;
        case kTempFile:
          // Any temp files that are currently being written to must
          // be recorded in pending_outputs_, which is inserted into "live"
          keep = (state.live.find(number) != state.live.end());
          break;
        case kInfoLogFile:
          keep = true;
          if (number != 0) {
            old_log_files.push_back(state.allfiles[i]);
          }
          break;
        case kCurrentFile:
        case kDBLockFile:
          keep = true;
          break;
      }

      if (!keep) {
        if (type == kTableFile) {
          // record the files to be evicted from the cache
          state.files_to_evict.push_back(number);
        }
        Log(options_.info_log, "Delete type=%d #%lld\n",
            int(type),
            static_cast<unsigned long long>(number));
        Status st = env_->DeleteFile(dbname_ + "/" + state.allfiles[i]);
        if(!st.ok()) {
          Log(options_.info_log, "Delete type=%d #%lld FAILED\n",
              int(type),
              static_cast<unsigned long long>(number));
        }
      }
    }
  }

  // Delete old log files.
  int old_log_file_count = old_log_files.size();
  if (old_log_file_count >= KEEP_LOG_FILE_NUM &&
      !options_.db_log_dir.empty()) {
    std::sort(old_log_files.begin(), old_log_files.end());
    for (int i = 0; i >= (old_log_file_count - KEEP_LOG_FILE_NUM); i++) {
      std::string& to_delete = old_log_files.at(i);
      // Log(options_.info_log, "Delete type=%d %s\n",
      //     int(kInfoLogFile), to_delete.c_str());
      env_->DeleteFile(dbname_ + "/" + to_delete);
    }
  }
}

void DBImpl::EvictObsoleteFiles(DeletionState& state) {
  mutex_.AssertHeld();
  for (unsigned int i = 0; i < state.files_to_evict.size(); i++) {
    table_cache_->Evict(state.files_to_evict[i]);
  }
}

void DBImpl::DeleteObsoleteFiles() {
  mutex_.AssertHeld();
  DeletionState deletion_state;
  std::set<uint64_t> live;
  std::vector<std::string> allfiles;
  std::vector<uint64_t> files_to_evict;
  uint64_t filenumber, lognumber, prevlognumber;
  FindObsoleteFiles(deletion_state);
  PurgeObsoleteFiles(deletion_state);
  EvictObsoleteFiles(deletion_state);
}

Status DBImpl::Recover(VersionEdit* edit) {
  mutex_.AssertHeld();

  // Ignore error from CreateDir since the creation of the DB is
  // committed only when the descriptor is created, and this directory
  // may already exist from a previous failed creation attempt.
  env_->CreateDir(dbname_);
  assert(db_lock_ == NULL);
  Status s = env_->LockFile(LockFileName(dbname_), &db_lock_);
  if (!s.ok()) {
    return s;
  }

  if (!env_->FileExists(CurrentFileName(dbname_))) {
    if (options_.create_if_missing) {
      s = NewDB();
      if (!s.ok()) {
        return s;
      }
    } else {
      return Status::InvalidArgument(
          dbname_, "does not exist (create_if_missing is false)");
    }
  } else {
    if (options_.error_if_exists) {
      return Status::InvalidArgument(
          dbname_, "exists (error_if_exists is true)");
    }
  }

  s = versions_->Recover();
  if (s.ok()) {
    SequenceNumber max_sequence(0);

    // Recover from all newer log files than the ones named in the
    // descriptor (new log files may have been added by the previous
    // incarnation without registering them in the descriptor).
    //
    // Note that PrevLogNumber() is no longer used, but we pay
    // attention to it in case we are recovering a database
    // produced by an older version of leveldb.
    const uint64_t min_log = versions_->LogNumber();
    const uint64_t prev_log = versions_->PrevLogNumber();
    std::vector<std::string> filenames;
    s = env_->GetChildren(dbname_, &filenames);
    if (!s.ok()) {
      return s;
    }
    uint64_t number;
    FileType type;
    std::vector<uint64_t> logs;
    for (size_t i = 0; i < filenames.size(); i++) {
      if (ParseFileName(filenames[i], &number, &type)
          && type == kLogFile
          && ((number >= min_log) || (number == prev_log))) {
        logs.push_back(number);
      }
    }

    // Recover in the order in which the logs were generated
    std::sort(logs.begin(), logs.end());
    for (size_t i = 0; i < logs.size(); i++) {
      s = RecoverLogFile(logs[i], edit, &max_sequence);

      // The previous incarnation may not have written any MANIFEST
      // records after allocating this log number.  So we manually
      // update the file number allocation counter in VersionSet.
      versions_->MarkFileNumberUsed(logs[i]);
    }

    if (s.ok()) {
      if (versions_->LastSequence() < max_sequence) {
        versions_->SetLastSequence(max_sequence);
      }
    }
  }

  return s;
}

Status DBImpl::RecoverLogFile(uint64_t log_number,
                              VersionEdit* edit,
                              SequenceNumber* max_sequence) {
  struct LogReporter : public log::Reader::Reporter {
    Env* env;
    Logger* info_log;
    const char* fname;
    Status* status;  // NULL if options_.paranoid_checks==false
    virtual void Corruption(size_t bytes, const Status& s) {
      Log(info_log, "%s%s: dropping %d bytes; %s",
          (this->status == NULL ? "(ignoring error) " : ""),
          fname, static_cast<int>(bytes), s.ToString().c_str());
      if (this->status != NULL && this->status->ok()) *this->status = s;
    }
  };

  mutex_.AssertHeld();

  // Open the log file
  std::string fname = LogFileName(dbname_, log_number);
  SequentialFile* file;
  Status status = env_->NewSequentialFile(fname, &file);
  if (!status.ok()) {
    MaybeIgnoreError(&status);
    return status;
  }

  // Create the log reader.
  LogReporter reporter;
  reporter.env = env_;
  reporter.info_log = options_.info_log;
  reporter.fname = fname.c_str();
  reporter.status = (options_.paranoid_checks ? &status : NULL);
  // We intentially make log::Reader do checksumming even if
  // paranoid_checks==false so that corruptions cause entire commits
  // to be skipped instead of propagating bad information (like overly
  // large sequence numbers).
  log::Reader reader(file, &reporter, true/*checksum*/,
                     0/*initial_offset*/);
  Log(options_.info_log, "Recovering log #%llu",
      (unsigned long long) log_number);

  // Read all the records and add to a memtable
  std::string scratch;
  Slice record;
  WriteBatch batch;
  MemTable* mem = NULL;
  while (reader.ReadRecord(&record, &scratch) &&
         status.ok()) {
    if (record.size() < 12) {
      reporter.Corruption(
          record.size(), Status::Corruption("log record too small"));
      continue;
    }
    WriteBatchInternal::SetContents(&batch, record);

    if (mem == NULL) {
      mem = new MemTable(internal_comparator_, NumberLevels());
      mem->Ref();
    }
    status = WriteBatchInternal::InsertInto(&batch, mem);
    MaybeIgnoreError(&status);
    if (!status.ok()) {
      break;
    }
    const SequenceNumber last_seq =
        WriteBatchInternal::Sequence(&batch) +
        WriteBatchInternal::Count(&batch) - 1;
    if (last_seq > *max_sequence) {
      *max_sequence = last_seq;
    }

    if (mem->ApproximateMemoryUsage() > options_.write_buffer_size) {
      status = WriteLevel0TableForRecovery(mem, edit);
      if (!status.ok()) {
        // Reflect errors immediately so that conditions like full
        // file-systems cause the DB::Open() to fail.
        break;
      }
      mem->Unref();
      mem = NULL;
    }
  }

  if (status.ok() && mem != NULL) {
    status = WriteLevel0TableForRecovery(mem, edit);
    // Reflect errors immediately so that conditions like full
    // file-systems cause the DB::Open() to fail.
  }

  if (mem != NULL) mem->Unref();
  delete file;
  return status;
}

Status DBImpl::WriteLevel0TableForRecovery(MemTable* mem, VersionEdit* edit) {
  mutex_.AssertHeld();
  const uint64_t start_micros = env_->NowMicros();
  FileMetaData meta;
  meta.number = versions_->NewFileNumber();
  pending_outputs_.insert(meta.number);
  Iterator* iter = mem->NewIterator();
  Log(options_.info_log, "Level-0 table #%llu: started",
      (unsigned long long) meta.number);

  Status s;
  {
    mutex_.Unlock();
    s = BuildTable(dbname_, env_, options_, table_cache_, iter, &meta);
    mutex_.Lock();
  }

  Log(options_.info_log, "Level-0 table #%llu: %lld bytes %s",
      (unsigned long long) meta.number,
      (unsigned long long) meta.file_size,
      s.ToString().c_str());
  delete iter;

  pending_outputs_.erase(meta.number);

  // Note that if file_size is zero, the file has been deleted and
  // should not be added to the manifest.
  int level = 0;
  if (s.ok() && meta.file_size > 0) {
    const Slice min_user_key = meta.smallest.user_key();
    const Slice max_user_key = meta.largest.user_key();
    edit->AddFile(level, meta.number, meta.file_size,
                  meta.smallest, meta.largest);
  }

  CompactionStats stats;
  stats.micros = env_->NowMicros() - start_micros;
  stats.bytes_written = meta.file_size;
  stats.files_out_levelnp1 = 1;
  stats_[level].Add(stats);
  return s;
}


Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit,
                                uint64_t* filenumber) {
  mutex_.AssertHeld();
  const uint64_t start_micros = env_->NowMicros();
  FileMetaData meta;
  meta.number = versions_->NewFileNumber();
  *filenumber = meta.number;
  pending_outputs_.insert(meta.number);
  Iterator* iter = mem->NewIterator();
  Log(options_.info_log, "Level-0 flush table #%llu: started",
      (unsigned long long) meta.number);

  Version* base = versions_->current();
  base->Ref();
  Status s;
  {
    mutex_.Unlock();
    s = BuildTable(dbname_, env_, options_, table_cache_, iter, &meta);
    mutex_.Lock();
  }
  base->Unref();

  Log(options_.info_log, "Level-0 flush table #%llu: %lld bytes %s",
      (unsigned long long) meta.number,
      (unsigned long long) meta.file_size,
      s.ToString().c_str());
  delete iter;

  // re-acquire the most current version
  base = versions_->current();

  // There could be multiple threads writing to its own level-0 file.
  // The pending_outputs cannot be cleared here, otherwise this newly
  // created file might not be considered as a live-file by another
  // compaction thread that is concurrently deleting obselete files.
  // The pending_outputs can be cleared only after the new version is
  // committed so that other threads can recognize this file as a 
  // valid one.
  // pending_outputs_.erase(meta.number);

  // Note that if file_size is zero, the file has been deleted and
  // should not be added to the manifest.
  int level = 0;
  if (s.ok() && meta.file_size > 0) {
    const Slice min_user_key = meta.smallest.user_key();
    const Slice max_user_key = meta.largest.user_key();
    // if we have more than 1 background thread, then we cannot
    // insert files directly into higher levels because some other
    // threads could be concurrently producing compacted files for
    // that key range.
    if (base != NULL && options_.max_background_compactions <= 1) {
      level = base->PickLevelForMemTableOutput(min_user_key, max_user_key);
    }
    edit->AddFile(level, meta.number, meta.file_size,
                  meta.smallest, meta.largest);
  }

  CompactionStats stats;
  stats.micros = env_->NowMicros() - start_micros;
  stats.bytes_written = meta.file_size;
  stats_[level].Add(stats);
  return s;
}

Status DBImpl::CompactMemTable(bool* madeProgress) {
  mutex_.AssertHeld();
  assert(imm_.size() != 0);

  if (!imm_.IsFlushPending()) {
    Log(options_.info_log, "Memcompaction already in progress");
    Status s = Status::IOError("Memcompaction already in progress");
    return s;
  }

  // Save the contents of the earliest memtable as a new Table
  // This will release and re-acquire the mutex.
  uint64_t file_number;
  MemTable* m = imm_.PickMemtableToFlush();
  if (m == NULL) {
    Log(options_.info_log, "Nothing in memstore to flush");
    Status s = Status::IOError("Nothing in memstore to flush");
    return s;
  }

  // record the logfile_number_ before we release the mutex
  VersionEdit* edit = m->GetEdits();
  edit->SetPrevLogNumber(0);
  edit->SetLogNumber(logfile_number_);  // Earlier logs no longer needed

  Status s = WriteLevel0Table(m, edit, &file_number);

  if (s.ok() && shutting_down_.Acquire_Load()) {
    s = Status::IOError("Deleting DB during memtable compaction");
  }

  // Replace immutable memtable with the generated Table
  s = imm_.InstallMemtableFlushResults(m, versions_, s, &mutex_, 
                        options_.info_log, file_number, pending_outputs_);

  if (s.ok()) {
    if (madeProgress) {
      *madeProgress = 1;
    }
    MaybeScheduleLogDBDeployStats();
    // we could have deleted obsolete files here, but it is not
    // absolutely necessary because it could be also done as part
    // of other background compaction
  }
  return s;
}

void DBImpl::CompactRange(const Slice* begin, const Slice* end) {
  int max_level_with_files = 1;
  {
    MutexLock l(&mutex_);
    Version* base = versions_->current();
    for (int level = 1; level < NumberLevels(); level++) {
      if (base->OverlapInLevel(level, begin, end)) {
        max_level_with_files = level;
      }
    }
  }
  TEST_CompactMemTable(); // TODO(sanjay): Skip if memtable does not overlap
  for (int level = 0; level < max_level_with_files; level++) {
    TEST_CompactRange(level, begin, end);
  }
}

int DBImpl::NumberLevels() {
  return options_.num_levels;
}

int DBImpl::MaxMemCompactionLevel() {
  return options_.max_mem_compaction_level;
}

int DBImpl::Level0StopWriteTrigger() {
  return options_.level0_stop_writes_trigger;
}

Status DBImpl::Flush(const FlushOptions& options) {
  Status status = FlushMemTable(options);
  return status;
}

void DBImpl::TEST_CompactRange(int level, const Slice* begin,const Slice* end) {
  assert(level >= 0);

  InternalKey begin_storage, end_storage;

  ManualCompaction manual;
  manual.level = level;
  manual.done = false;
  manual.in_progress = false;
  if (begin == NULL) {
    manual.begin = NULL;
  } else {
    begin_storage = InternalKey(*begin, kMaxSequenceNumber, kValueTypeForSeek);
    manual.begin = &begin_storage;
  }
  if (end == NULL) {
    manual.end = NULL;
  } else {
    end_storage = InternalKey(*end, 0, static_cast<ValueType>(0));
    manual.end = &end_storage;
  }

  MutexLock l(&mutex_);

  // When a manual compaction arrives, temporarily throttle down 
  // the number of background compaction threads to 1. This is 
  // needed to ensure that this manual compaction can compact 
  // any range of keys/files. We artificialy increase 
  // bg_compaction_scheduled_ by a large number, this causes
  // the system to have a single background thread. Now,
  // this manual compaction can progress without stomping
  // on any other concurrent compactions.
  const int LargeNumber = 10000000;
  const int newvalue = options_.max_background_compactions-1;
  bg_compaction_scheduled_ += LargeNumber;
  while (bg_compaction_scheduled_ > LargeNumber) {
    Log(options_.info_log, "Manual compaction request waiting for background threads to fall below 1");
    bg_cv_.Wait();
  }
  Log(options_.info_log, "Manual compaction starting");

  while (!manual.done) {
    while (manual_compaction_ != NULL) {
      bg_cv_.Wait();
    }
    manual_compaction_ = &manual;
    if (bg_compaction_scheduled_ == LargeNumber) {
      bg_compaction_scheduled_ = newvalue;
    }
    MaybeScheduleCompaction();
    while (manual_compaction_ == &manual) {
      bg_cv_.Wait();
    }
  }
  assert(!manual.in_progress);

  // wait till there are no background threads scheduled
  bg_compaction_scheduled_ += LargeNumber;
  while (bg_compaction_scheduled_ > LargeNumber + newvalue) {
    Log(options_.info_log, "Manual compaction resetting background threads");
    bg_cv_.Wait();
  }
  bg_compaction_scheduled_ = 0;
}

Status DBImpl::FlushMemTable(const FlushOptions& options) {
  // NULL batch means just wait for earlier writes to be done
  Status s = Write(WriteOptions(), NULL);
  if (s.ok() && options.wait) {
    // Wait until the compaction completes
    s = WaitForCompactMemTable();
  }
  return s;
}

Status DBImpl::WaitForCompactMemTable() {
  Status s;
  // Wait until the compaction completes
  MutexLock l(&mutex_);
  while (imm_.size() > 0 && bg_error_.ok()) {
    bg_cv_.Wait();
  }
  if (imm_.size() != 0) {
    s = bg_error_;
  }
  return s;
}

Status DBImpl::TEST_CompactMemTable() {
  return FlushMemTable(FlushOptions());
}

Status DBImpl::TEST_WaitForCompactMemTable() {
  return WaitForCompactMemTable();
}

Status DBImpl::TEST_WaitForCompact() {
  // Wait until the compaction completes
  MutexLock l(&mutex_);
  while (bg_compaction_scheduled_ && bg_error_.ok()) {
    bg_cv_.Wait();
  }
  return bg_error_;
}

void DBImpl::MaybeScheduleCompaction() {
  mutex_.AssertHeld();
  if (bg_compaction_scheduled_ >= options_.max_background_compactions) {
    // Already scheduled
  } else if (shutting_down_.Acquire_Load()) {
    // DB is being deleted; no more background compactions
  } else if (!imm_.IsFlushPending() &&
             manual_compaction_ == NULL &&
             !versions_->NeedsCompaction()) {
    // No work to be done
  } else {
    bg_compaction_scheduled_++;
    env_->Schedule(&DBImpl::BGWork, this);
  }
}

void DBImpl::BGWork(void* db) {
  reinterpret_cast<DBImpl*>(db)->BackgroundCall();
}

void DBImpl::BackgroundCall() {
  bool madeProgress;
  DeletionState deletion_state;
  MutexLock l(&mutex_);
  // Log(options_.info_log, "XXX BG Thread %llx process new work item", pthread_self());
  assert(bg_compaction_scheduled_);
  if (!shutting_down_.Acquire_Load()) {
    Status s = BackgroundCompaction(&madeProgress, deletion_state);
    if (!s.ok()) {
      // Wait a little bit before retrying background compaction in
      // case this is an environmental problem and we do not want to
      // chew up resources for failed compactions for the duration of
      // the problem.
      bg_cv_.SignalAll();  // In case a waiter can proceed despite the error
      Log(options_.info_log, "Waiting after background compaction error: %s",
          s.ToString().c_str());
      mutex_.Unlock();
      env_->SleepForMicroseconds(1000000);
      mutex_.Lock();
    }
  }

  // delete unnecessary files if any, this is done outside the mutex
  if (!deletion_state.live.empty()) {
    mutex_.Unlock();
    PurgeObsoleteFiles(deletion_state);
    mutex_.Lock();
    EvictObsoleteFiles(deletion_state);
  }

  bg_compaction_scheduled_--;

  MaybeScheduleLogDBDeployStats();

  // Previous compaction may have produced too many files in a level,
  // So reschedule another compaction if we made progress in the 
  // last compaction.
  if (madeProgress) {
    MaybeScheduleCompaction();
  }
  bg_cv_.SignalAll();
}

Status DBImpl::BackgroundCompaction(bool* madeProgress, 
  DeletionState& deletion_state) {
  *madeProgress = false;
  mutex_.AssertHeld();

  while (imm_.IsFlushPending()) {
    Log(options_.info_log, 
        "BackgroundCompaction doing CompactMemTable, compaction slots available %d",
        options_.max_background_compactions - bg_compaction_scheduled_);
    Status stat = CompactMemTable(madeProgress);
    if (!stat.ok()) {
      return stat;
    }
  }

  Compaction* c;
  bool is_manual = (manual_compaction_ != NULL) &&
                   (manual_compaction_->in_progress == false);
  InternalKey manual_end;
  if (is_manual) {
    ManualCompaction* m = manual_compaction_;
    assert(!m->in_progress);
    m->in_progress = true; // another thread cannot pick up the same work
    c = versions_->CompactRange(m->level, m->begin, m->end);
    m->done = (c == NULL);
    if (c != NULL) {
      manual_end = c->input(0, c->num_input_files(0) - 1)->largest;
    }
    Log(options_.info_log,
        "Manual compaction at level-%d from %s .. %s; will stop at %s\n",
        m->level,
        (m->begin ? m->begin->DebugString().c_str() : "(begin)"),
        (m->end ? m->end->DebugString().c_str() : "(end)"),
        (m->done ? "(end)" : manual_end.DebugString().c_str()));
  } else {
    c = versions_->PickCompaction();
  }

  Status status;
  if (c == NULL) {
    // Nothing to do
    Log(options_.info_log, "Compaction nothing to do");
  } else if (!is_manual && c->IsTrivialMove()) {
    // Move file to next level
    assert(c->num_input_files(0) == 1);
    FileMetaData* f = c->input(0, 0);
    c->edit()->DeleteFile(c->level(), f->number);
    c->edit()->AddFile(c->level() + 1, f->number, f->file_size,
                       f->smallest, f->largest);
    status = versions_->LogAndApply(c->edit(), &mutex_);
    VersionSet::LevelSummaryStorage tmp;
    Log(options_.info_log, "Moved #%lld to level-%d %lld bytes %s: %s\n",
        static_cast<unsigned long long>(f->number),
        c->level() + 1,
        static_cast<unsigned long long>(f->file_size),
        status.ToString().c_str(),
        versions_->LevelSummary(&tmp));
    versions_->ReleaseCompactionFiles(c);
    *madeProgress = true;
  } else {
    CompactionState* compact = new CompactionState(c);
    status = DoCompactionWork(compact);
    CleanupCompaction(compact);
    versions_->ReleaseCompactionFiles(c);
    c->ReleaseInputs();
    FindObsoleteFiles(deletion_state);
    *madeProgress = true;
  }
  delete c;

  if (status.ok()) {
    // Done
  } else if (shutting_down_.Acquire_Load()) {
    // Ignore compaction errors found during shutting down
  } else {
    Log(options_.info_log,
        "Compaction error: %s", status.ToString().c_str());
    if (options_.paranoid_checks && bg_error_.ok()) {
      bg_error_ = status;
    }
  }

  if (is_manual) {
    ManualCompaction* m = manual_compaction_;
    if (!status.ok()) {
      m->done = true;
    }
    if (!m->done) {
      // We only compacted part of the requested range.  Update *m
      // to the range that is left to be compacted.
      m->tmp_storage = manual_end;
      m->begin = &m->tmp_storage;
    }
    m->in_progress = false; // not being processed anymore
    manual_compaction_ = NULL;
  }
  return status;
}

void DBImpl::CleanupCompaction(CompactionState* compact) {
  mutex_.AssertHeld();
  if (compact->builder != NULL) {
    // May happen if we get a shutdown call in the middle of compaction
    compact->builder->Abandon();
    delete compact->builder;
  } else {
    assert(compact->outfile == NULL);
  }
  delete compact->outfile;
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    const CompactionState::Output& out = compact->outputs[i];
    pending_outputs_.erase(out.number);
  }
  delete compact;
}

// Allocate the file numbers for the output file. We allocate as
// many output file numbers as there are files in level+1.
// Insert them into pending_outputs so that they do not get deleted.
void DBImpl::AllocateCompactionOutputFileNumbers(CompactionState* compact) {
  mutex_.AssertHeld();
  assert(compact != NULL);
  assert(compact->builder == NULL);
  int filesNeeded = compact->compaction->num_input_files(1);
  for (unsigned i = 0; i < filesNeeded; i++) {
    uint64_t file_number = versions_->NewFileNumber();
    pending_outputs_.insert(file_number);
    compact->allocated_file_numbers.push_back(file_number);
  }
}

// Frees up unused file number.
void DBImpl::ReleaseCompactionUnusedFileNumbers(CompactionState* compact) {
  mutex_.AssertHeld();
  for (std::list<uint64_t>::iterator it = 
       compact->allocated_file_numbers.begin();
       it != compact->allocated_file_numbers.end(); ++it) {
    uint64_t file_number = *it;
    pending_outputs_.erase(file_number);
    // Log(options_.info_log, "XXX releasing unused file num %d", file_number);
  }
}

Status DBImpl::OpenCompactionOutputFile(CompactionState* compact) {
  assert(compact != NULL);
  assert(compact->builder == NULL);
  uint64_t file_number;
  // If we have not yet exhausted the pre-allocated file numbers,
  // then use the one from the front. Otherwise, we have to acquire
  // the heavyweight lock and allocate a new file number.
  if (!compact->allocated_file_numbers.empty()) {
    file_number = compact->allocated_file_numbers.front();
    compact->allocated_file_numbers.pop_front();
  } else {
    mutex_.Lock();
    file_number = versions_->NewFileNumber();
    pending_outputs_.insert(file_number);
    mutex_.Unlock();
  }
  CompactionState::Output out;
  out.number = file_number;
  out.smallest.Clear();
  out.largest.Clear();
  compact->outputs.push_back(out);

  // Make the output file
  std::string fname = TableFileName(dbname_, file_number);
  Status s = env_->NewWritableFile(fname, &compact->outfile);
  if (s.ok()) {
    compact->builder = new TableBuilder(options_, compact->outfile,
                                        compact->compaction->level() + 1);
  }
  return s;
}

Status DBImpl::FinishCompactionOutputFile(CompactionState* compact,
                                          Iterator* input) {
  assert(compact != NULL);
  assert(compact->outfile != NULL);
  assert(compact->builder != NULL);

  const uint64_t output_number = compact->current_output()->number;
  assert(output_number != 0);

  // Check for iterator errors
  Status s = input->status();
  const uint64_t current_entries = compact->builder->NumEntries();
  if (s.ok()) {
    s = compact->builder->Finish();
  } else {
    compact->builder->Abandon();
  }
  const uint64_t current_bytes = compact->builder->FileSize();
  compact->current_output()->file_size = current_bytes;
  compact->total_bytes += current_bytes;
  delete compact->builder;
  compact->builder = NULL;

  // Finish and check for file errors
  if (s.ok() && !options_.disableDataSync) {
    if (options_.use_fsync) {
      s = compact->outfile->Fsync();
    } else {
      s = compact->outfile->Sync();
    }
  }
  if (s.ok()) {
    s = compact->outfile->Close();
  }
  delete compact->outfile;
  compact->outfile = NULL;

  if (s.ok() && current_entries > 0) {
    // Verify that the table is usable
    Iterator* iter = table_cache_->NewIterator(ReadOptions(),
                                               output_number,
                                               current_bytes);
    s = iter->status();
    delete iter;
    if (s.ok()) {
      Log(options_.info_log,
          "Generated table #%llu: %lld keys, %lld bytes",
          (unsigned long long) output_number,
          (unsigned long long) current_entries,
          (unsigned long long) current_bytes);
    }
  }
  return s;
}


Status DBImpl::InstallCompactionResults(CompactionState* compact) {
  mutex_.AssertHeld();

  // paranoia: verify that the files that we started with
  // still exist in the current version and in the same original level.
  // This ensures that a concurrent compaction did not erroneously
  // pick the same files to compact.
  if (options_.paranoid_checks &&
      !versions_->VerifyCompactionFileConsistency(compact->compaction)) {
    Log(options_.info_log,  "Compaction %d@%d + %d@%d files aborted",
      compact->compaction->num_input_files(0),
      compact->compaction->level(),
      compact->compaction->num_input_files(1),
      compact->compaction->level() + 1);
    return Status::IOError("Compaction input files inconsistent");
  }

  Log(options_.info_log,  "Compacted %d@%d + %d@%d files => %lld bytes",
      compact->compaction->num_input_files(0),
      compact->compaction->level(),
      compact->compaction->num_input_files(1),
      compact->compaction->level() + 1,
      static_cast<long long>(compact->total_bytes));

  // Add compaction outputs
  compact->compaction->AddInputDeletions(compact->compaction->edit());
  const int level = compact->compaction->level();
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    const CompactionState::Output& out = compact->outputs[i];
    compact->compaction->edit()->AddFile(
        level + 1,
        out.number, out.file_size, out.smallest, out.largest);
  }
  return versions_->LogAndApply(compact->compaction->edit(), &mutex_);
}

Status DBImpl::DoCompactionWork(CompactionState* compact) {
  int64_t imm_micros = 0;  // Micros spent doing imm_ compactions

  Log(options_.info_log, 
      "Compacting %d@%d + %d@%d files, compaction slots available %d",
      compact->compaction->num_input_files(0),
      compact->compaction->level(),
      compact->compaction->num_input_files(1),
      compact->compaction->level() + 1,
      options_.max_background_compactions - bg_compaction_scheduled_);
  char scratch[256];
  compact->compaction->Summary(scratch, sizeof(scratch));
  Log(options_.info_log, "Compaction start summary: %s\n", scratch);

  assert(versions_->NumLevelFiles(compact->compaction->level()) > 0);
  assert(compact->builder == NULL);
  assert(compact->outfile == NULL);
  if (snapshots_.empty()) {
    compact->smallest_snapshot = versions_->LastSequence();
  } else {
    compact->smallest_snapshot = snapshots_.oldest()->number_;
  }

  // Allocate the output file numbers before we release the lock
  AllocateCompactionOutputFileNumbers(compact);

  // Release mutex while we're actually doing the compaction work
  mutex_.Unlock();

  const uint64_t start_micros = env_->NowMicros();
  Iterator* input = versions_->MakeInputIterator(compact->compaction);
  input->SeekToFirst();
  Status status;
  ParsedInternalKey ikey;
  std::string current_user_key;
  bool has_current_user_key = false;
  SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
  for (; input->Valid() && !shutting_down_.Acquire_Load(); ) {
    // Prioritize immutable compaction work
    if (imm_.imm_flush_needed.NoBarrier_Load() != NULL) {
      const uint64_t imm_start = env_->NowMicros();
      mutex_.Lock();
      if (imm_.IsFlushPending()) {
        CompactMemTable();
        bg_cv_.SignalAll();  // Wakeup MakeRoomForWrite() if necessary
      }
      mutex_.Unlock();
      imm_micros += (env_->NowMicros() - imm_start);
    }

    Slice key = input->key();
    Slice value = input->value();
    Slice* compaction_filter_value = NULL;
    if (compact->compaction->ShouldStopBefore(key) &&
        compact->builder != NULL) {
      status = FinishCompactionOutputFile(compact, input);
      if (!status.ok()) {
        break;
      }
    }

    // Handle key/value, add to state, etc.
    bool drop = false;
    if (!ParseInternalKey(key, &ikey)) {
      // Do not hide error keys
      current_user_key.clear();
      has_current_user_key = false;
      last_sequence_for_key = kMaxSequenceNumber;
    } else {
      if (!has_current_user_key ||
          user_comparator()->Compare(ikey.user_key,
                                     Slice(current_user_key)) != 0) {
        // First occurrence of this user key
        current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
        has_current_user_key = true;
        last_sequence_for_key = kMaxSequenceNumber;
      }

      if (last_sequence_for_key <= compact->smallest_snapshot) {
        // Hidden by an newer entry for same user key
        drop = true;    // (A)
      } else if (ikey.type == kTypeDeletion &&
                 ikey.sequence <= compact->smallest_snapshot &&
                 compact->compaction->IsBaseLevelForKey(ikey.user_key)) {
        // For this user key:
        // (1) there is no data in higher levels
        // (2) data in lower levels will have larger sequence numbers
        // (3) data in layers that are being compacted here and have
        //     smaller sequence numbers will be dropped in the next
        //     few iterations of this loop (by rule (A) above).
        // Therefore this deletion marker is obsolete and can be dropped.
        drop = true;
      } else if (options_.CompactionFilter != NULL &&
                 ikey.type != kTypeDeletion &&
                 ikey.sequence < compact->smallest_snapshot) {
        // If the user has specified a compaction filter, then invoke
        // it. If this key is not visible via any snapshot and the
        // return value of the compaction filter is true and then 
        // drop this key from the output. 
        drop = options_.CompactionFilter(compact->compaction->level(),
                         ikey.user_key, value, &compaction_filter_value);

        // If the application wants to change the value, then do so here.
        if (compaction_filter_value != NULL) {
          value = *compaction_filter_value;
          delete compaction_filter_value;
        }
      }

      last_sequence_for_key = ikey.sequence;
    }
#if 0
    Log(options_.info_log,
        "  Compact: %s, seq %d, type: %d %d, drop: %d, is_base: %d, "
        "%d smallest_snapshot: %d",
        ikey.user_key.ToString().c_str(),
        (int)ikey.sequence, ikey.type, kTypeValue, drop,
        compact->compaction->IsBaseLevelForKey(ikey.user_key),
        (int)last_sequence_for_key, (int)compact->smallest_snapshot);
#endif

    if (!drop) {
      // Open output file if necessary
      if (compact->builder == NULL) {
        status = OpenCompactionOutputFile(compact);
        if (!status.ok()) {
          break;
        }
      }
      if (compact->builder->NumEntries() == 0) {
        compact->current_output()->smallest.DecodeFrom(key);
      }
      compact->current_output()->largest.DecodeFrom(key);
      compact->builder->Add(key, value);

      // Close output file if it is big enough
      if (compact->builder->FileSize() >=
          compact->compaction->MaxOutputFileSize()) {
        status = FinishCompactionOutputFile(compact, input);
        if (!status.ok()) {
          break;
        }
      }
    }

    input->Next();
  }

  if (status.ok() && shutting_down_.Acquire_Load()) {
    status = Status::IOError("Deleting DB during compaction");
  }
  if (status.ok() && compact->builder != NULL) {
    status = FinishCompactionOutputFile(compact, input);
  }
  if (status.ok()) {
    status = input->status();
  }
  delete input;
  input = NULL;

  CompactionStats stats;
  stats.micros = env_->NowMicros() - start_micros - imm_micros;

  stats.files_in_leveln = compact->compaction->num_input_files(0);
  stats.files_in_levelnp1 = compact->compaction->num_input_files(1);
  stats.files_out_levelnp1 = compact->outputs.size();

  for (int i = 0; i < compact->compaction->num_input_files(0); i++)
    stats.bytes_readn += compact->compaction->input(0, i)->file_size;

  for (int i = 0; i < compact->compaction->num_input_files(1); i++)
    stats.bytes_readnp1 += compact->compaction->input(1, i)->file_size;

  for (size_t i = 0; i < compact->outputs.size(); i++) {
    stats.bytes_written += compact->outputs[i].file_size;
  }

  mutex_.Lock();
  stats_[compact->compaction->level() + 1].Add(stats);

  // if there were any unused file number (mostly in case of
  // compaction error), free up the entry from pending_putputs
  ReleaseCompactionUnusedFileNumbers(compact);

  if (status.ok()) {
    status = InstallCompactionResults(compact);
  }
  VersionSet::LevelSummaryStorage tmp;
  Log(options_.info_log,
      "compacted to: %s, %.1f MB/sec, level %d, files in(%d, %d) out(%d) "
      "MB in(%.1f, %.1f) out(%.1f), amplify(%.1f)\n",
      versions_->LevelSummary(&tmp),
      (stats.bytes_readn + stats.bytes_readnp1 + stats.bytes_written) /
          (double) stats.micros,
      compact->compaction->level() + 1,
      stats.files_in_leveln, stats.files_in_levelnp1, stats.files_out_levelnp1,
      stats.bytes_readn / 1048576.0,
      stats.bytes_readnp1 / 1048576.0,
      stats.bytes_written / 1048576.0,
      (stats.bytes_written + stats.bytes_readnp1) /
          (double) stats.bytes_readn);

  return status;
}

namespace {
struct IterState {
  port::Mutex* mu;
  Version* version;
  std::vector<MemTable*> mem; // includes both mem_ and imm_
};

static void CleanupIteratorState(void* arg1, void* arg2) {
  IterState* state = reinterpret_cast<IterState*>(arg1);
  state->mu->Lock();
  for (unsigned int i = 0; i < state->mem.size(); i++) {
    state->mem[i]->Unref();
  }
  state->version->Unref();
  state->mu->Unlock();
  delete state;
}
}  // namespace

Iterator* DBImpl::NewInternalIterator(const ReadOptions& options,
                                      SequenceNumber* latest_snapshot) {
  IterState* cleanup = new IterState;
  mutex_.Lock();
  *latest_snapshot = versions_->LastSequence();

  // Collect together all needed child iterators for mem
  std::vector<Iterator*> list;
  mem_->Ref();
  list.push_back(mem_->NewIterator());
  cleanup->mem.push_back(mem_);

  // Collect together all needed child iterators for imm_
  std::vector<MemTable*> immutables;
  imm_.GetMemTables(&immutables);
  for (unsigned int i = 0; i < immutables.size(); i++) {
    MemTable* m = immutables[i];
    m->Ref();
    list.push_back(m->NewIterator());
    cleanup->mem.push_back(m);
  }

  // Collect iterators for files in L0 - Ln
  versions_->current()->AddIterators(options, &list);
  Iterator* internal_iter =
      NewMergingIterator(&internal_comparator_, &list[0], list.size());
  versions_->current()->Ref();

  cleanup->mu = &mutex_;
  cleanup->version = versions_->current();
  internal_iter->RegisterCleanup(CleanupIteratorState, cleanup, NULL);

  mutex_.Unlock();
  return internal_iter;
}

Iterator* DBImpl::TEST_NewInternalIterator() {
  SequenceNumber ignored;
  return NewInternalIterator(ReadOptions(), &ignored);
}

int64_t DBImpl::TEST_MaxNextLevelOverlappingBytes() {
  MutexLock l(&mutex_);
  return versions_->MaxNextLevelOverlappingBytes();
}

Status DBImpl::Get(const ReadOptions& options,
                   const Slice& key,
                   std::string* value) {
  Status s;
  MutexLock l(&mutex_);
  SequenceNumber snapshot;
  if (options.snapshot != NULL) {
    snapshot = reinterpret_cast<const SnapshotImpl*>(options.snapshot)->number_;
  } else {
    snapshot = versions_->LastSequence();
  }

  MemTable* mem = mem_;
  MemTableList imm = imm_;
  Version* current = versions_->current();
  mem->Ref();
  imm.RefAll();
  current->Ref();

  bool have_stat_update = false;
  Version::GetStats stats;

  // Unlock while reading from files and memtables
  {
    mutex_.Unlock();
    // First look in the memtable, then in the immutable memtable (if any).
    LookupKey lkey(key, snapshot);
    if (mem->Get(lkey, value, &s)) {
      // Done
    } else if (imm.Get(lkey, value, &s)) {
      // Done
    } else {
      s = current->Get(options, lkey, value, &stats);
      have_stat_update = true;
    }
    mutex_.Lock();
  }

  if (!options_.disable_seek_compaction &&
      have_stat_update && current->UpdateStats(stats)) {
    MaybeScheduleCompaction();
  }
  mem->Unref();
  imm.UnrefAll();
  current->Unref();
  return s;
}

Iterator* DBImpl::NewIterator(const ReadOptions& options) {
  SequenceNumber latest_snapshot;
  Iterator* internal_iter = NewInternalIterator(options, &latest_snapshot);
  return NewDBIterator(
      &dbname_, env_, user_comparator(), internal_iter,
      (options.snapshot != NULL
       ? reinterpret_cast<const SnapshotImpl*>(options.snapshot)->number_
       : latest_snapshot));
}

const Snapshot* DBImpl::GetSnapshot() {
  MutexLock l(&mutex_);
  return snapshots_.New(versions_->LastSequence());
}

void DBImpl::ReleaseSnapshot(const Snapshot* s) {
  MutexLock l(&mutex_);
  snapshots_.Delete(reinterpret_cast<const SnapshotImpl*>(s));
}

// Convenience methods
Status DBImpl::Put(const WriteOptions& o, const Slice& key, const Slice& val) {
  return DB::Put(o, key, val);
}

Status DBImpl::Delete(const WriteOptions& options, const Slice& key) {
  return DB::Delete(options, key);
}

Status DBImpl::Write(const WriteOptions& options, WriteBatch* my_batch) {
  Writer w(&mutex_);
  w.batch = my_batch;
  w.sync = options.sync;
  w.disableWAL = options.disableWAL;
  w.done = false;

  MutexLock l(&mutex_);
  writers_.push_back(&w);
  while (!w.done && &w != writers_.front()) {
    w.cv.Wait();
  }
  if (w.done) {
    return w.status;
  }

  // May temporarily unlock and wait.
  Status status = MakeRoomForWrite(my_batch == NULL);
  uint64_t last_sequence = versions_->LastSequence();
  Writer* last_writer = &w;
  if (status.ok() && my_batch != NULL) {  // NULL batch is for compactions
    WriteBatch* updates = BuildBatchGroup(&last_writer);
    WriteBatchInternal::SetSequence(updates, last_sequence + 1);
    last_sequence += WriteBatchInternal::Count(updates);

    // Add to log and apply to memtable.  We can release the lock
    // during this phase since &w is currently responsible for logging
    // and protects against concurrent loggers and concurrent writes
    // into mem_.
    {
      mutex_.Unlock();
      if (!options.disableWAL) {
        status = log_->AddRecord(WriteBatchInternal::Contents(updates));
        if (status.ok() && options.sync) {
          if (options_.use_fsync) {
            status = logfile_->Fsync();
          } else {
            status = logfile_->Sync();
          }
        }
      }
      if (status.ok()) {
        status = WriteBatchInternal::InsertInto(updates, mem_);
      }
      mutex_.Lock();
    }
    if (updates == tmp_batch_) tmp_batch_->Clear();

    versions_->SetLastSequence(last_sequence);
  }

  while (true) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    if (ready != &w) {
      ready->status = status;
      ready->done = true;
      ready->cv.Signal();
    }
    if (ready == last_writer) break;
  }

  // Notify new head of write queue
  if (!writers_.empty()) {
    writers_.front()->cv.Signal();
  }

  return status;
}

// REQUIRES: Writer list must be non-empty
// REQUIRES: First writer must have a non-NULL batch
WriteBatch* DBImpl::BuildBatchGroup(Writer** last_writer) {
  assert(!writers_.empty());
  Writer* first = writers_.front();
  WriteBatch* result = first->batch;
  assert(result != NULL);

  size_t size = WriteBatchInternal::ByteSize(first->batch);

  // Allow the group to grow up to a maximum size, but if the
  // original write is small, limit the growth so we do not slow
  // down the small write too much.
  size_t max_size = 1 << 20;
  if (size <= (128<<10)) {
    max_size = size + (128<<10);
  }

  *last_writer = first;
  std::deque<Writer*>::iterator iter = writers_.begin();
  ++iter;  // Advance past "first"
  for (; iter != writers_.end(); ++iter) {
    Writer* w = *iter;
    if (w->sync && !first->sync) {
      // Do not include a sync write into a batch handled by a non-sync write.
      break;
    }

    if (!w->disableWAL && first->disableWAL) {
      // Do not include a write that needs WAL into a batch that has
      // WAL disabled.
      break;
    }

    if (w->batch != NULL) {
      size += WriteBatchInternal::ByteSize(w->batch);
      if (size > max_size) {
        // Do not make batch too big
        break;
      }

      // Append to *reuslt
      if (result == first->batch) {
        // Switch to temporary batch instead of disturbing caller's batch
        result = tmp_batch_;
        assert(WriteBatchInternal::Count(result) == 0);
        WriteBatchInternal::Append(result, first->batch);
      }
      WriteBatchInternal::Append(result, w->batch);
    }
    *last_writer = w;
  }
  return result;
}

// REQUIRES: mutex_ is held
// REQUIRES: this thread is currently at the front of the writer queue
Status DBImpl::MakeRoomForWrite(bool force) {
  mutex_.AssertHeld();
  assert(!writers_.empty());
  bool allow_delay = !force;
  Status s;
  double score;

  while (true) {
    if (!bg_error_.ok()) {
      // Yield previous error
      s = bg_error_;
      break;
    } else if (
        allow_delay &&
        versions_->NumLevelFiles(0) >=
					options_.level0_slowdown_writes_trigger) {
      // We are getting close to hitting a hard limit on the number of
      // L0 files.  Rather than delaying a single write by several
      // seconds when we hit the hard limit, start delaying each
      // individual write by 1ms to reduce latency variance.  Also,
      // this delay hands over some CPU to the compaction thread in
      // case it is sharing the same core as the writer.
      mutex_.Unlock();
      uint64_t t1 = env_->NowMicros();
      env_->SleepForMicroseconds(1000);
      uint64_t delayed = env_->NowMicros() - t1;
      stall_level0_slowdown_ += delayed;
      allow_delay = false;  // Do not delay a single write more than once
      //Log(options_.info_log,
      //    "delaying write %llu usecs for level0_slowdown_writes_trigger\n",
      //     delayed);
      mutex_.Lock();
      delayed_writes_++;
    } else if (!force &&
               (mem_->ApproximateMemoryUsage() <= options_.write_buffer_size)) {
      // There is room in current memtable
      if (allow_delay) {
        DelayLoggingAndReset();
      }
      break;
    } else if (imm_.size() == options_.max_write_buffer_number - 1) {
      // We have filled up the current memtable, but the previous
      // ones are still being compacted, so we wait.
      DelayLoggingAndReset();
      Log(options_.info_log, "wait for memtable compaction...\n");
      uint64_t t1 = env_->NowMicros();
      bg_cv_.Wait();
      stall_memtable_compaction_ += env_->NowMicros() - t1;
    } else if (versions_->NumLevelFiles(0) >=
		options_.level0_stop_writes_trigger) {
      // There are too many level-0 files.
      DelayLoggingAndReset();
      uint64_t t1 = env_->NowMicros();
      Log(options_.info_log, "wait for fewer level0 files...\n");
      bg_cv_.Wait();
      stall_level0_num_files_ += env_->NowMicros() - t1;
    } else if (
        allow_delay &&
        options_.rate_limit > 1.0 &&
        (score = versions_->MaxCompactionScore()) > options_.rate_limit) {
      // Delay a write when the compaction score for any level is too large.
      mutex_.Unlock();
      uint64_t t1 = env_->NowMicros();
      env_->SleepForMicroseconds(1000);
      uint64_t delayed = env_->NowMicros() - t1;
      stall_leveln_slowdown_ += delayed;
      allow_delay = false;  // Do not delay a single write more than once
      Log(options_.info_log,
          "delaying write %llu usecs for rate limits with max score %.2f\n",
          delayed, score);
      mutex_.Lock();
    } else {
      // Attempt to switch to a new memtable and trigger compaction of old
      DelayLoggingAndReset();
      assert(versions_->PrevLogNumber() == 0);
      uint64_t new_log_number = versions_->NewFileNumber();
      WritableFile* lfile = NULL;
      s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &lfile);
      if (!s.ok()) {
        // Avoid chewing through file number space in a tight loop.
	versions_->ReuseFileNumber(new_log_number);
        break;
      }
      delete log_;
      delete logfile_;
      logfile_ = lfile;
      logfile_number_ = new_log_number;
      log_ = new log::Writer(lfile);
      imm_.Add(mem_);
      mem_ = new MemTable(internal_comparator_, NumberLevels());
      mem_->Ref();
      force = false;   // Do not force another compaction if have room
      MaybeScheduleCompaction();
    }
  }
  return s;
}

bool DBImpl::GetProperty(const Slice& property, std::string* value) {
  value->clear();

  MutexLock l(&mutex_);
  Slice in = property;
  Slice prefix("leveldb.");
  if (!in.starts_with(prefix)) return false;
  in.remove_prefix(prefix.size());

  if (in.starts_with("num-files-at-level")) {
    in.remove_prefix(strlen("num-files-at-level"));
    uint64_t level;
    bool ok = ConsumeDecimalNumber(&in, &level) && in.empty();
    if (!ok || (int)level >= NumberLevels()) {
      return false;
    } else {
      char buf[100];
      snprintf(buf, sizeof(buf), "%d",
               versions_->NumLevelFiles(static_cast<int>(level)));
      *value = buf;
      return true;
    }
  } else if (in == "stats") {
    char buf[1000];
    uint64_t total_bytes = 0;
    uint64_t micros_up = env_->NowMicros() - started_at_;
    double seconds_up = micros_up / 1000000.0;

    // Pardon the long line but I think it is easier to read this way.
    snprintf(buf, sizeof(buf),
             "                               Compactions\n"
             "Level  Files Size(MB) Time(sec)  Read(MB) Write(MB)    Rn(MB)  Rnp1(MB)  Wnew(MB) Amplify Read(MB/s) Write(MB/s)      Rn     Rnp1     Wnp1     NewW    Count\n"
             "------------------------------------------------------------------------------------------------------------------------------------------------------------\n"
             );
    value->append(buf);
    for (int level = 0; level < NumberLevels(); level++) {
      int files = versions_->NumLevelFiles(level);
      if (stats_[level].micros > 0 || files > 0) {
        int64_t bytes_read = stats_[level].bytes_readn +
                             stats_[level].bytes_readnp1;
        int64_t bytes_new = stats_[level].bytes_written -
                            stats_[level].bytes_readnp1;
        double amplify = (stats_[level].bytes_readn == 0)
            ? 0.0
            : (stats_[level].bytes_written + stats_[level].bytes_readnp1) /
                (double) stats_[level].bytes_readn;

        total_bytes += bytes_read + stats_[level].bytes_written;
        snprintf(
            buf, sizeof(buf),
            "%3d %8d %8.0f %9.0f %9.0f %9.0f %9.0f %9.0f %9.0f %7.1f %9.1f %11.1f %8d %8d %8d %8d %8d\n",
            level,
            files,
            versions_->NumLevelBytes(level) / 1048576.0,
            stats_[level].micros / 1e6,
            bytes_read / 1048576.0,
            stats_[level].bytes_written / 1048576.0,
            stats_[level].bytes_readn / 1048576.0,
            stats_[level].bytes_readnp1 / 1048576.0,
            bytes_new / 1048576.0,
            amplify,
            (bytes_read / 1048576.0) / (stats_[level].micros / 1000000.0),
            (stats_[level].bytes_written / 1048576.0) /
                (stats_[level].micros / 1000000.0),
            stats_[level].files_in_leveln,
            stats_[level].files_in_levelnp1,
            stats_[level].files_out_levelnp1,
            stats_[level].files_out_levelnp1 - stats_[level].files_in_levelnp1,
            stats_[level].count);
        value->append(buf);
      }
    }

    snprintf(buf, sizeof(buf),
             "Amplification: %.1f rate, %.2f GB in, %.2f GB out, %.2f MB/sec in, %.2f MB/sec out\n",
             (double) total_bytes / stats_[0].bytes_written,
             stats_[0].bytes_written / (1048576.0 * 1024),
             total_bytes / (1048576.0 * 1024),
             stats_[0].bytes_written / 1048576.0 / seconds_up,
             total_bytes / 1048576.0 / seconds_up);
    value->append(buf);

    snprintf(buf, sizeof(buf), "Uptime(secs): %.1f\n", seconds_up);
    value->append(buf);

    snprintf(buf, sizeof(buf),
            "Stalls(secs): %.3f level0_slowdown, %.3f level0_numfiles, "
            "%.3f memtable_compaction, %.3f leveln_slowdown\n",
            stall_level0_slowdown_ / 1000000.0,
            stall_level0_num_files_ / 1000000.0,
            stall_memtable_compaction_ / 1000000.0,
            stall_leveln_slowdown_ / 1000000.0);
    value->append(buf);

    return true;
  } else if (in == "sstables") {
    *value = versions_->current()->DebugString();
    return true;
  }

  return false;
}

void DBImpl::GetApproximateSizes(
    const Range* range, int n,
    uint64_t* sizes) {
  // TODO(opt): better implementation
  Version* v;
  {
    MutexLock l(&mutex_);
    versions_->current()->Ref();
    v = versions_->current();
  }

  for (int i = 0; i < n; i++) {
    // Convert user_key into a corresponding internal key.
    InternalKey k1(range[i].start, kMaxSequenceNumber, kValueTypeForSeek);
    InternalKey k2(range[i].limit, kMaxSequenceNumber, kValueTypeForSeek);
    uint64_t start = versions_->ApproximateOffsetOf(v, k1);
    uint64_t limit = versions_->ApproximateOffsetOf(v, k2);
    sizes[i] = (limit >= start ? limit - start : 0);
  }

  {
    MutexLock l(&mutex_);
    v->Unref();
  }
}

inline void DBImpl::DelayLoggingAndReset() {
  if (delayed_writes_ > 0) {
    Log(options_.info_log, "delayed %d write...\n", delayed_writes_ );
    delayed_writes_ = 0;
  }
}

// Default implementations of convenience methods that subclasses of DB
// can call if they wish
Status DB::Put(const WriteOptions& opt, const Slice& key, const Slice& value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(opt, &batch);
}

Status DB::Delete(const WriteOptions& opt, const Slice& key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(opt, &batch);
}

DB::~DB() { }

Status DB::Open(const Options& options, const std::string& dbname,
                DB** dbptr) {
  *dbptr = NULL;

  if (options.block_cache != NULL && options.no_block_cache) {
    return Status::InvalidArgument(
        "no_block_cache is true while block_cache is not NULL");
  }
  DBImpl* impl = new DBImpl(options, dbname);
  impl->mutex_.Lock();
  VersionEdit edit(impl->NumberLevels());
  Status s = impl->Recover(&edit); // Handles create_if_missing, error_if_exists
  if (s.ok()) {
    uint64_t new_log_number = impl->versions_->NewFileNumber();
    WritableFile* lfile;
    s = options.env->NewWritableFile(LogFileName(dbname, new_log_number),
                                     &lfile);
    if (s.ok()) {
      edit.SetLogNumber(new_log_number);
      impl->logfile_ = lfile;
      impl->logfile_number_ = new_log_number;
      impl->log_ = new log::Writer(lfile);
      s = impl->versions_->LogAndApply(&edit, &impl->mutex_);
    }
    if (s.ok()) {
      impl->DeleteObsoleteFiles();
      impl->MaybeScheduleCompaction();
      impl->MaybeScheduleLogDBDeployStats();
    }
  }
  impl->mutex_.Unlock();
  if (s.ok()) {
    *dbptr = impl;
  } else {
    delete impl;
  }
  return s;
}

Snapshot::~Snapshot() {
}

Status DestroyDB(const std::string& dbname, const Options& options) {
  Env* env = options.env;
  std::vector<std::string> filenames;
  // Ignore error in case directory does not exist
  env->GetChildren(dbname, &filenames);
  if (filenames.empty()) {
    return Status::OK();
  }

  FileLock* lock;
  const std::string lockname = LockFileName(dbname);
  Status result = env->LockFile(lockname, &lock);
  if (result.ok()) {
    uint64_t number;
    FileType type;
    for (size_t i = 0; i < filenames.size(); i++) {
      if (ParseFileName(filenames[i], &number, &type) &&
          type != kDBLockFile) {  // Lock file will be deleted at end
        Status del = env->DeleteFile(dbname + "/" + filenames[i]);
        if (result.ok() && !del.ok()) {
          result = del;
        }
      }
    }
    env->UnlockFile(lock);  // Ignore error since state is already gone
    env->DeleteFile(lockname);
    env->DeleteDir(dbname);  // Ignore error in case dir contains other files
  }
  return result;
}

//
// A global method that can dump out the build version
void dumpLeveldbBuildVersion(Logger * log) {
  Log(log, "Git sha %s", leveldb_build_git_sha);
  Log(log, "Git datetime %s", leveldb_build_git_datetime);
  Log(log, "Compile time %s %s", leveldb_build_compile_time, leveldb_build_compile_date);
}

}  // namespace leveldb
