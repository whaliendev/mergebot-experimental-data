#include "db/db_impl.h"
#include <algorithm>
#include <climits>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <stdint.h>
#include <string>
#include <unordered_set>
#include <vector>
#include "db/builder.h"
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/memtable_list.h"
#include "db/merge_context.h"
#include "db/merge_helper.h"
#include "db/prefix_filter_iterator.h"
#include "db/table_cache.h"
#include "db/table_properties_collector.h"
#include "db/transaction_log_impl.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include "port/port.h"
#include "rocksdb/compaction_filter.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/merge_operator.h"
#include "rocksdb/statistics.h"
#include "rocksdb/status.h"
#include "rocksdb/table.h"
#include "table/block.h"
#include "table/block_based_table_factory.h"
#include "table/merger.h"
#include "table/two_level_iterator.h"
#include "util/autovector.h"
#include "util/auto_roll_logger.h"
#include "util/build_version.h"
#include "util/coding.h"
#include "util/hash_skiplist_rep.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/perf_context_imp.h"
#include "util/stop_watch.h"
#include "util/autovector.h"
namespace rocksdb {
void DumpLeveldbBuildVersion(Logger * log);
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
  std::vector<SequenceNumber> existing_snapshots;
  struct Output {
    uint64_t number;
    uint64_t file_size;
    InternalKey smallest, largest;
    SequenceNumber smallest_seqno, largest_seqno;
  };
  std::vector<Output> outputs;
  std::list<uint64_t> allocated_file_numbers;
  unique_ptr<WritableFile> outfile;
  unique_ptr<TableBuilder> builder;
  uint64_t total_bytes;
  Output* current_output() { return &outputs[outputs.size()-1]; }
  explicit CompactionState(Compaction* c)
      : compaction(c),
        total_bytes(0) {
  }
  CompactionFilter::Context GetFilterContext() {
    CompactionFilter::Context context;
    context.is_full_compaction = compaction->IsFullCompaction();
    return context;
  }
};
template <class T, class V>
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
  result.filter_policy = (src.filter_policy != nullptr) ? ipolicy : nullptr;
  if (result.max_open_files != -1) {
    ClipToRange(&result.max_open_files, 20, 1000000);
  }
  ClipToRange(&result.write_buffer_size, ((size_t)64)<<10,
                                                 ((size_t)64)<<30);
  ClipToRange(&result.block_size, 1<<10, 4<<20);
  if (result.arena_block_size <= 0) {
    result.arena_block_size = result.write_buffer_size / 10;
  }
  result.min_write_buffer_number_to_merge = std::min(
    result.min_write_buffer_number_to_merge, result.max_write_buffer_number-1);
  if (result.info_log == nullptr) {
    Status s = CreateLoggerFromOptions(dbname, result.db_log_dir, src.env,
                                       result, &result.info_log);
    if (!s.ok()) {
      result.info_log = nullptr;
    }
  }
  if (result.block_cache == nullptr && !result.no_block_cache) {
    result.block_cache = NewLRUCache(8 << 20);
  }
  result.compression_per_level = src.compression_per_level;
  if (result.block_size_deviation < 0 || result.block_size_deviation > 100) {
    result.block_size_deviation = 0;
  }
  if (result.max_mem_compaction_level >= result.num_levels) {
    result.max_mem_compaction_level = result.num_levels - 1;
  }
  if (result.soft_rate_limit > result.hard_rate_limit) {
    result.soft_rate_limit = result.hard_rate_limit;
  }
  if (result.compaction_filter) {
    Log(result.info_log, "Compaction filter specified, ignore factory");
  }
  if (result.prefix_extractor) {
    auto factory = dynamic_cast<HashSkipListRepFactory*>(
      result.memtable_factory.get());
    if (factory &&
        factory->GetTransform() != result.prefix_extractor) {
      Log(result.info_log, "A prefix hash representation factory was supplied "
          "whose prefix extractor does not match options.prefix_extractor. "
          "Falling back to skip list representation factory");
      result.memtable_factory = std::make_shared<SkipListFactory>();
    } else if (factory) {
      Log(result.info_log, "Prefix hash memtable rep is in use.");
    }
  }
  if (result.wal_dir.empty()) {
    result.wal_dir = dbname;
  }
  auto& collectors = result.table_properties_collectors;
  for (size_t i = 0; i < result.table_properties_collectors.size(); ++i) {
    assert(collectors[i]);
    collectors[i] =
      std::make_shared<UserKeyTablePropertiesCollector>(collectors[i]);
  }
  collectors.push_back(
      std::make_shared<InternalKeyPropertiesCollector>()
  );
  return result;
}
CompressionType GetCompressionType(const Options& options, int level,
                                   const bool enable_compression) {
  if (!enable_compression) {
    return kNoCompression;
  }
  if (!options.compression_per_level.empty()) {
    const int n = options.compression_per_level.size() - 1;
    return options.compression_per_level[std::max(0, std::min(level, n))];
  } else {
    return options.compression;
  }
}
CompressionType GetCompressionFlush(const Options& options) {
  bool can_compress;
  if (options.compaction_style == kCompactionStyleUniversal) {
    can_compress =
        (options.compaction_options_universal.compression_size_percent < 0);
  } else {
    can_compress = (GetCompressionType(options, 0, true) != kNoCompression);
  }
  if (can_compress) {
    return options.compression;
  } else {
    return kNoCompression;
  }
}
DBImpl::DBImpl(const Options& options, const std::string& dbname)
    : env_(options.env),
      dbname_(dbname),
      internal_comparator_(options.comparator),
      options_(SanitizeOptions(dbname, &internal_comparator_,
                               &internal_filter_policy_, options)),
      internal_filter_policy_(options.filter_policy),
      owns_info_log_(options_.info_log != options.info_log),
      db_lock_(nullptr),
      mutex_(options.use_adaptive_mutex),
      shutting_down_(nullptr),
      bg_cv_(&mutex_),
      mem_rep_factory_(options_.memtable_factory.get()),
      mem_(new MemTable(internal_comparator_, options_)),
      logfile_number_(0),
      super_version_(nullptr),
      tmp_batch_(),
      bg_compaction_scheduled_(0),
      bg_manual_only_(0),
      bg_flush_scheduled_(0),
      bg_logstats_scheduled_(false),
      manual_compaction_(nullptr),
      logger_(nullptr),
      disable_delete_obsolete_files_(0),
      delete_obsolete_files_last_run_(options.env->NowMicros()),
      purge_wal_files_last_run_(0),
      last_stats_dump_time_microsec_(0),
      default_interval_to_delete_obsolete_WAL_(600),
      stall_level0_slowdown_(0),
      stall_memtable_compaction_(0),
      stall_level0_num_files_(0),
      stall_level0_slowdown_count_(0),
      stall_memtable_compaction_count_(0),
      stall_level0_num_files_count_(0),
      started_at_(options.env->NowMicros()),
      flush_on_destroy_(false),
      stats_(options.num_levels),
      delayed_writes_(0),
      storage_options_(options),
      bg_work_gate_closed_(false),
      refitting_level_(false) {
  mem_->Ref();
  env_->GetAbsolutePath(dbname, &db_absolute_path_);
  stall_leveln_slowdown_.resize(options.num_levels);
  stall_leveln_slowdown_count_.resize(options.num_levels);
  for (int i = 0; i < options.num_levels; ++i) {
    stall_leveln_slowdown_[i] = 0;
    stall_leveln_slowdown_count_[i] = 0;
  }
  const int table_cache_size =
      (options_.max_open_files == -1) ?
          4194304 : options_.max_open_files - 10;
  table_cache_.reset(new TableCache(dbname_, &options_,
                                    storage_options_, table_cache_size));
  versions_.reset(new VersionSet(dbname_, &options_, storage_options_,
                                 table_cache_.get(), &internal_comparator_));
  DumpLeveldbBuildVersion(options_.info_log.get());
  options_.Dump(options_.info_log.get());
  char name[100];
  Status s = env_->GetHostName(name, 100L);
  if (s.ok()) {
    host_name_ = name;
  } else {
    Log(options_.info_log, "Can't get hostname, use localhost as host name.");
    host_name_ = "localhost";
  }
  last_log_ts = 0;
  LogFlush(options_.info_log);
}
DBImpl::~DBImpl() {
  autovector<MemTable*> to_delete;
  if (flush_on_destroy_ && mem_->GetFirstSequenceNumber() != 0) {
    FlushMemTable(FlushOptions());
  }
  mutex_.Lock();
  shutting_down_.Release_Store(this);
  while (bg_compaction_scheduled_ ||
         bg_flush_scheduled_ ||
         bg_logstats_scheduled_) {
    bg_cv_.Wait();
  }
  if (super_version_ != nullptr) {
    bool is_last_reference __attribute__((unused));
    is_last_reference = super_version_->Unref();
    assert(is_last_reference);
    super_version_->Cleanup();
    delete super_version_;
  }
  mutex_.Unlock();
  if (db_lock_ != nullptr) {
    env_->UnlockFile(db_lock_);
  }
  if (mem_ != nullptr) {
    delete mem_->Unref();
  }
  imm_.UnrefAll(&to_delete);
  for (MemTable* m: to_delete) {
    delete m;
  }
  versions_.reset();
  LogFlush(options_.info_log);
}
void DBImpl::TEST_Destroy_DBImpl() {
  flush_on_destroy_ = false;
  mutex_.Lock();
  while (bg_compaction_scheduled_ ||
         bg_flush_scheduled_ ||
         bg_logstats_scheduled_) {
    bg_cv_.Wait();
  }
  if (super_version_ != nullptr) {
    bool is_last_reference __attribute__((unused));
    is_last_reference = super_version_->Unref();
    assert(is_last_reference);
    super_version_->Cleanup();
    delete super_version_;
  }
  bg_work_gate_closed_ = true;
  const int LargeNumber = 10000000;
  bg_compaction_scheduled_ += LargeNumber;
  mutex_.Unlock();
  LogFlush(options_.info_log);
  if (db_lock_ != nullptr) {
    env_->UnlockFile(db_lock_);
  }
  log_.reset();
  versions_.reset();
  table_cache_.reset();
}
uint64_t DBImpl::TEST_Current_Manifest_FileNo() {
  return versions_->ManifestFileNumber();
}
Status DBImpl::NewDB() {
  VersionEdit new_db;
  new_db.SetComparatorName(user_comparator()->Name());
  new_db.SetLogNumber(0);
  new_db.SetNextFile(2);
  new_db.SetLastSequence(0);
  const std::string manifest = DescriptorFileName(dbname_, 1);
  unique_ptr<WritableFile> file;
  Status s = env_->NewWritableFile(manifest, &file, storage_options_);
  if (!s.ok()) {
    return s;
  }
  file->SetPreallocationBlockSize(options_.manifest_preallocation_size);
  {
    log::Writer log(std::move(file));
    std::string record;
    new_db.EncodeTo(&record);
    s = log.AddRecord(record);
  }
  if (s.ok()) {
    s = SetCurrentFile(env_, dbname_, 1);
  } else {
    env_->DeleteFile(manifest);
  }
  return s;
}
void DBImpl::MaybeIgnoreError(Status* s) const {
  if (s->ok() || options_.paranoid_checks) {
  } else {
    Log(options_.info_log, "Ignoring error %s", s->ToString().c_str());
    *s = Status::OK();
  }
}
const Status DBImpl::CreateArchivalDirectory() {
  if (options_.WAL_ttl_seconds > 0 || options_.WAL_size_limit_MB > 0) {
    std::string archivalPath = ArchivalDirectory(options_.wal_dir);
    return env_->CreateDirIfMissing(archivalPath);
  }
  return Status::OK();
}
void DBImpl::PrintStatistics() {
  auto dbstats = options_.statistics.get();
  if (dbstats) {
    Log(options_.info_log,
        "STATISTCS:\n %s",
        dbstats->ToString().c_str());
  }
}
void DBImpl::MaybeDumpStats() {
  if (options_.stats_dump_period_sec == 0) return;
  const uint64_t now_micros = env_->NowMicros();
  if (last_stats_dump_time_microsec_ +
      options_.stats_dump_period_sec * 1000000
      <= now_micros) {
    last_stats_dump_time_microsec_ = now_micros;
    std::string stats;
    GetProperty("rocksdb.stats", &stats);
    Log(options_.info_log, "%s", stats.c_str());
    PrintStatistics();
  }
}
DBImpl::SuperVersion::~SuperVersion() {
  for (auto td : to_delete) {
    delete td;
  }
}
DBImpl::SuperVersion* DBImpl::SuperVersion::Ref() {
  refs.fetch_add(1, std::memory_order_relaxed);
  return this;
}
bool DBImpl::SuperVersion::Unref() {
  assert(refs > 0);
  return refs.fetch_sub(1, std::memory_order_relaxed) == 1;
}
void DBImpl::SuperVersion::Cleanup() {
  assert(refs.load(std::memory_order_relaxed) == 0);
  imm.UnrefAll(&to_delete);
  MemTable* m = mem->Unref();
  if (m != nullptr) {
    to_delete.push_back(m);
  }
  current->Unref();
}
void DBImpl::SuperVersion::Init(MemTable* new_mem, const MemTableList& new_imm,
                                Version* new_current) {
  mem = new_mem;
  imm = new_imm;
  current = new_current;
  mem->Ref();
  imm.RefAll();
  current->Ref();
  refs.store(1, std::memory_order_relaxed);
}
void DBImpl::FindObsoleteFiles(DeletionState& deletion_state,
                               bool force,
                               bool no_full_scan) {
  mutex_.AssertHeld();
  if (disable_delete_obsolete_files_ > 0) {
    return;
  }
  bool doing_the_full_scan = false;
  if (no_full_scan) {
    doing_the_full_scan = false;
  } else if (force || options_.delete_obsolete_files_period_micros == 0) {
    doing_the_full_scan = true;
  } else {
    const uint64_t now_micros = env_->NowMicros();
    if (delete_obsolete_files_last_run_ +
        options_.delete_obsolete_files_period_micros < now_micros) {
      doing_the_full_scan = true;
      delete_obsolete_files_last_run_ = now_micros;
    }
  }
  versions_->GetObsoleteFiles(&deletion_state.sst_delete_files);
  deletion_state.manifest_file_number = versions_->ManifestFileNumber();
  deletion_state.log_number = versions_->LogNumber();
  deletion_state.prev_log_number = versions_->PrevLogNumber();
  if (!doing_the_full_scan && !deletion_state.HaveSomethingToDelete()) {
    return;
  }
  deletion_state.sst_live.assign(pending_outputs_.begin(),
                                 pending_outputs_.end());
  versions_->AddLiveFiles(&deletion_state.sst_live);
  if (doing_the_full_scan) {
    env_->GetChildren(
        dbname_, &deletion_state.candidate_files
    );
    if (options_.wal_dir != dbname_) {
      std::vector<std::string> log_files;
      env_->GetChildren(options_.wal_dir, &log_files);
      deletion_state.candidate_files.insert(
        deletion_state.candidate_files.end(),
        log_files.begin(),
        log_files.end()
      );
    }
  }
}
void DBImpl::PurgeObsoleteFiles(DeletionState& state) {
  if (state.candidate_files.empty() &&
      state.sst_delete_files.empty() &&
      state.log_delete_files.empty()) {
    return;
  }
  if (state.manifest_file_number == 0) {
    return;
  }
  std::vector<std::string> old_log_files;
  std::unordered_set<uint64_t> sst_live(
      state.sst_live.begin(), state.sst_live.end()
  );
  auto& candidate_files = state.candidate_files;
  candidate_files.reserve(
      candidate_files.size() +
      state.sst_delete_files.size() +
      state.log_delete_files.size());
  const char* kDumbDbName = "";
  for (auto file : state.sst_delete_files) {
    candidate_files.push_back(
        TableFileName(kDumbDbName, file->number).substr(1)
    );
    delete file;
  }
  for (auto file_num : state.log_delete_files) {
    if (file_num > 0) {
      candidate_files.push_back(
          LogFileName(kDumbDbName, file_num).substr(1)
      );
    }
  }
  sort(candidate_files.begin(), candidate_files.end());
  candidate_files.erase(
      unique(candidate_files.begin(), candidate_files.end()),
      candidate_files.end()
  );
  for (const auto& to_delete : candidate_files) {
    uint64_t number;
    FileType type;
    if (!ParseFileName(to_delete, &number, &type)) {
      continue;
    }
    bool keep = true;
    switch (type) {
      case kLogFile:
        keep = ((number >= state.log_number) ||
                (number == state.prev_log_number));
        break;
      case kDescriptorFile:
        keep = (number >= state.manifest_file_number);
        break;
      case kTableFile:
        keep = (sst_live.find(number) != sst_live.end());
        break;
      case kTempFile:
        keep = (sst_live.find(number) != sst_live.end());
        break;
      case kInfoLogFile:
        keep = true;
        if (number != 0) {
          old_log_files.push_back(to_delete);
        }
        break;
      case kCurrentFile:
      case kDBLockFile:
      case kIdentityFile:
      case kMetaDatabase:
        keep = true;
        break;
    }
    if (keep) {
      continue;
    }
    if (type == kTableFile) {
      table_cache_->Evict(number);
    }
    std::string fname = ((type == kLogFile) ? options_.wal_dir : dbname_) +
        "/" + to_delete;
    Log(options_.info_log,
        "Delete type=%d #%lu",
        int(type),
        (unsigned long)number);
    if (type == kLogFile &&
        (options_.WAL_ttl_seconds > 0 || options_.WAL_size_limit_MB > 0)) {
      Status s = env_->RenameFile(fname,
          ArchivedLogFileName(options_.wal_dir, number));
      if (!s.ok()) {
        Log(options_.info_log,
            "RenameFile logfile #%lu FAILED -- %s\n",
            (unsigned long)number, s.ToString().c_str());
      }
    } else {
      Status s = env_->DeleteFile(fname);
      if (!s.ok()) {
        Log(options_.info_log, "Delete type=%d #%lu FAILED -- %s\n",
            int(type), (unsigned long)number, s.ToString().c_str());
      }
    }
  }
  size_t old_log_file_count = old_log_files.size();
  if (old_log_file_count >= options_.keep_log_file_num &&
      options_.db_log_dir.empty()) {
    std::sort(old_log_files.begin(), old_log_files.end());
    size_t end = old_log_file_count - options_.keep_log_file_num;
    for (unsigned int i = 0; i <= end; i++) {
      std::string& to_delete = old_log_files.at(i);
      env_->DeleteFile(dbname_ + "/" + to_delete);
    }
  }
  PurgeObsoleteWALFiles();
  LogFlush(options_.info_log);
}
void DBImpl::DeleteObsoleteFiles() {
  mutex_.AssertHeld();
  DeletionState deletion_state;
  FindObsoleteFiles(deletion_state, true);
  PurgeObsoleteFiles(deletion_state);
}
void DBImpl::PurgeObsoleteWALFiles() {
  bool const ttl_enabled = options_.WAL_ttl_seconds > 0;
  bool const size_limit_enabled = options_.WAL_size_limit_MB > 0;
  if (!ttl_enabled && !size_limit_enabled) {
    return;
  }
  int64_t current_time;
  Status s = env_->GetCurrentTime(&current_time);
  if (!s.ok()) {
    Log(options_.info_log, "Can't get current time: %s", s.ToString().c_str());
    assert(false);
    return;
  }
  uint64_t const now_seconds = static_cast<uint64_t>(current_time);
  uint64_t const time_to_check = (ttl_enabled && !size_limit_enabled) ?
    options_.WAL_ttl_seconds / 2 : default_interval_to_delete_obsolete_WAL_;
  if (purge_wal_files_last_run_ + time_to_check > now_seconds) {
    return;
  }
  purge_wal_files_last_run_ = now_seconds;
  std::string archival_dir = ArchivalDirectory(options_.wal_dir);
  std::vector<std::string> files;
  s = env_->GetChildren(archival_dir, &files);
  if (!s.ok()) {
    Log(options_.info_log, "Can't get archive files: %s", s.ToString().c_str());
    assert(false);
    return;
  }
  size_t log_files_num = 0;
  uint64_t log_file_size = 0;
  for (auto& f : files) {
    uint64_t number;
    FileType type;
    if (ParseFileName(f, &number, &type) && type == kLogFile) {
      std::string const file_path = archival_dir + "/" + f;
      if (ttl_enabled) {
        uint64_t file_m_time;
        Status const s = env_->GetFileModificationTime(file_path,
          &file_m_time);
        if (!s.ok()) {
          Log(options_.info_log, "Can't get file mod time: %s: %s",
              file_path.c_str(), s.ToString().c_str());
          continue;
        }
        if (now_seconds - file_m_time > options_.WAL_ttl_seconds) {
          Status const s = env_->DeleteFile(file_path);
          if (!s.ok()) {
            Log(options_.info_log, "Can't delete file: %s: %s",
                file_path.c_str(), s.ToString().c_str());
            continue;
          }
          continue;
        }
      }
      if (size_limit_enabled) {
        uint64_t file_size;
        Status const s = env_->GetFileSize(file_path, &file_size);
        if (!s.ok()) {
          Log(options_.info_log, "Can't get file size: %s: %s",
              file_path.c_str(), s.ToString().c_str());
          return;
        } else {
          if (file_size > 0) {
            log_file_size = std::max(log_file_size, file_size);
            ++log_files_num;
          } else {
            Status s = env_->DeleteFile(file_path);
            if (!s.ok()) {
              Log(options_.info_log, "Can't delete file: %s: %s",
                  file_path.c_str(), s.ToString().c_str());
              continue;
            }
          }
        }
      }
    }
  }
  if (0 == log_files_num || !size_limit_enabled) {
    return;
  }
  size_t const files_keep_num = options_.WAL_size_limit_MB *
    1024 * 1024 / log_file_size;
  if (log_files_num <= files_keep_num) {
    return;
  }
  size_t files_del_num = log_files_num - files_keep_num;
  VectorLogPtr archived_logs;
  AppendSortedWalsOfType(archival_dir, archived_logs, kArchivedLogFile);
  if (files_del_num > archived_logs.size()) {
    Log(options_.info_log, "Trying to delete more archived log files than "
        "exist. Deleting all");
    files_del_num = archived_logs.size();
  }
  for (size_t i = 0; i < files_del_num; ++i) {
    std::string const file_path = archived_logs[i]->PathName();
    Status const s = DeleteFile(file_path);
    if (!s.ok()) {
      Log(options_.info_log, "Can't delete file: %s: %s",
          file_path.c_str(), s.ToString().c_str());
      continue;
    }
  }
}
Status DBImpl::Recover(bool read_only, bool error_if_log_file_exist) {
  mutex_.AssertHeld();
  assert(db_lock_ == nullptr);
  if (!read_only) {
    Status s = env_->CreateDirIfMissing(dbname_);
    if (!s.ok()) {
      return s;
    }
    s = env_->LockFile(LockFileName(dbname_), &db_lock_);
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
    if (!env_->FileExists(IdentityFileName(dbname_))) {
      s = SetIdentityFile(env_, dbname_);
      if (!s.ok()) {
        return s;
      }
    }
  }
  Status s = versions_->Recover();
  if (s.ok()) {
    SequenceNumber max_sequence(0);
    const uint64_t min_log = versions_->LogNumber();
    const uint64_t prev_log = versions_->PrevLogNumber();
    std::vector<std::string> filenames;
    s = env_->GetChildren(options_.wal_dir, &filenames);
    if (!s.ok()) {
      return s;
    }
    std::vector<uint64_t> logs;
    for (size_t i = 0; i < filenames.size(); i++) {
      uint64_t number;
      FileType type;
      if (ParseFileName(filenames[i], &number, &type)
          && type == kLogFile
          && ((number >= min_log) || (number == prev_log))) {
        logs.push_back(number);
      }
    }
    if (logs.size() > 0 && error_if_log_file_exist) {
      return Status::Corruption(""
          "The db was opened in readonly mode with error_if_log_file_exist"
          "flag but a log file already exists");
    }
    std::sort(logs.begin(), logs.end());
<<<<<<< HEAD
    for (const auto& log : logs) {
      s = RecoverLogFile(log, edit, &max_sequence, external_table);
||||||| 0f4a75b71
    for (size_t i = 0; i < logs.size(); i++) {
      s = RecoverLogFile(logs[i], edit, &max_sequence, external_table);
=======
    for (size_t i = 0; s.ok() && i < logs.size(); i++) {
>>>>>>> 4e91f27c
<<<<<<< HEAD
      versions_->MarkFileNumberUsed(log);
||||||| 0f4a75b71
      versions_->MarkFileNumberUsed(logs[i]);
=======
      versions_->MarkFileNumberUsed(logs[i]);
      s = RecoverLogFile(logs[i], &max_sequence, read_only);
>>>>>>> 4e91f27c
    }
    if (s.ok()) {
      if (versions_->LastSequence() < max_sequence) {
        versions_->SetLastSequence(max_sequence);
      }
      SetTickerCount(options_.statistics.get(), SEQUENCE_NUMBER,
                     versions_->LastSequence());
    }
  }
  return s;
}
Status DBImpl::RecoverLogFile(uint64_t log_number, SequenceNumber* max_sequence,
                              bool read_only) {
  struct LogReporter : public log::Reader::Reporter {
    Env* env;
    Logger* info_log;
    const char* fname;
    Status* status;
    virtual void Corruption(size_t bytes, const Status& s) {
      Log(info_log, "%s%s: dropping %d bytes; %s",
          (this->status == nullptr ? "(ignoring error) " : ""),
          fname, static_cast<int>(bytes), s.ToString().c_str());
      if (this->status != nullptr && this->status->ok()) *this->status = s;
    }
  };
  mutex_.AssertHeld();
  VersionEdit edit;
  std::string fname = LogFileName(options_.wal_dir, log_number);
  unique_ptr<SequentialFile> file;
  Status status = env_->NewSequentialFile(fname, &file, storage_options_);
  if (!status.ok()) {
    MaybeIgnoreError(&status);
    return status;
  }
  LogReporter reporter;
  reporter.env = env_;
  reporter.info_log = options_.info_log.get();
  reporter.fname = fname.c_str();
  reporter.status = (options_.paranoid_checks &&
                     !options_.skip_log_error_on_recovery ? &status : nullptr);
  log::Reader reader(std::move(file), &reporter, true ,
                     0 );
  Log(options_.info_log, "Recovering log #%lu",
      (unsigned long) log_number);
  std::string scratch;
  Slice record;
  WriteBatch batch;
  bool memtable_empty = true;
  while (reader.ReadRecord(&record, &scratch)) {
    if (record.size() < 12) {
      reporter.Corruption(
          record.size(), Status::Corruption("log record too small"));
      continue;
    }
    WriteBatchInternal::SetContents(&batch, record);
    status = WriteBatchInternal::InsertInto(&batch, mem_, &options_);
    memtable_empty = false;
    MaybeIgnoreError(&status);
    if (!status.ok()) {
      return status;
    }
    const SequenceNumber last_seq =
        WriteBatchInternal::Sequence(&batch) +
        WriteBatchInternal::Count(&batch) - 1;
    if (last_seq > *max_sequence) {
      *max_sequence = last_seq;
    }
    if (!read_only &&
        mem_->ApproximateMemoryUsage() > options_.write_buffer_size) {
      status = WriteLevel0TableForRecovery(mem_, &edit);
      delete mem_->Unref();
      mem_ = new MemTable(internal_comparator_, options_);
      mem_->Ref();
      memtable_empty = true;
      if (!status.ok()) {
        return status;
      }
    }
  }
  if (!memtable_empty && !read_only) {
    status = WriteLevel0TableForRecovery(mem_, &edit);
    delete mem_->Unref();
    mem_ = new MemTable(internal_comparator_, options_);
    mem_->Ref();
    if (!status.ok()) {
      return status;
    }
  }
  if (edit.NumEntries() > 0) {
    assert(!read_only);
    edit.SetLogNumber(log_number + 1);
    status = versions_->LogAndApply(&edit, &mutex_);
  }
  return status;
}
Status DBImpl::WriteLevel0TableForRecovery(MemTable* mem, VersionEdit* edit) {
  mutex_.AssertHeld();
  const uint64_t start_micros = env_->NowMicros();
  FileMetaData meta;
  meta.number = versions_->NewFileNumber();
  pending_outputs_.insert(meta.number);
  Iterator* iter = mem->NewIterator();
  const SequenceNumber newest_snapshot = snapshots_.GetNewest();
  const SequenceNumber earliest_seqno_in_memtable =
    mem->GetFirstSequenceNumber();
  Log(options_.info_log, "Level-0 table #%lu: started",
      (unsigned long) meta.number);
  Status s;
  {
    mutex_.Unlock();
    s = BuildTable(dbname_, env_, options_, storage_options_,
                   table_cache_.get(), iter, &meta,
                   user_comparator(), newest_snapshot,
                   earliest_seqno_in_memtable,
                   GetCompressionFlush(options_));
    LogFlush(options_.info_log);
    mutex_.Lock();
  }
  Log(options_.info_log, "Level-0 table #%lu: %lu bytes %s",
      (unsigned long) meta.number,
      (unsigned long) meta.file_size,
      s.ToString().c_str());
  delete iter;
  pending_outputs_.erase(meta.number);
  int level = 0;
  if (s.ok() && meta.file_size > 0) {
    edit->AddFile(level, meta.number, meta.file_size,
                  meta.smallest, meta.largest,
                  meta.smallest_seqno, meta.largest_seqno);
  }
  CompactionStats stats;
  stats.micros = env_->NowMicros() - start_micros;
  stats.bytes_written = meta.file_size;
  stats.files_out_levelnp1 = 1;
  stats_[level].Add(stats);
  RecordTick(options_.statistics.get(), COMPACT_WRITE_BYTES, meta.file_size);
  return s;
}
Status DBImpl::WriteLevel0Table(autovector<MemTable*>& mems, VersionEdit* edit,
                                uint64_t* filenumber) {
  mutex_.AssertHeld();
  const uint64_t start_micros = env_->NowMicros();
  FileMetaData meta;
  meta.number = versions_->NewFileNumber();
  *filenumber = meta.number;
  pending_outputs_.insert(meta.number);
  const SequenceNumber newest_snapshot = snapshots_.GetNewest();
  const SequenceNumber earliest_seqno_in_memtable =
    mems[0]->GetFirstSequenceNumber();
  Version* base = versions_->current();
  base->Ref();
  Status s;
  {
    mutex_.Unlock();
    std::vector<Iterator*> memtables;
    for (MemTable* m : mems) {
      Log(options_.info_log,
          "Flushing memtable with log file: %lu\n",
          (unsigned long)m->GetLogNumber());
      memtables.push_back(m->NewIterator());
    }
    Iterator* iter = NewMergingIterator(
        env_, &internal_comparator_, &memtables[0], memtables.size());
    Log(options_.info_log,
        "Level-0 flush table #%lu: started",
        (unsigned long)meta.number);
    s = BuildTable(dbname_, env_, options_, storage_options_,
                   table_cache_.get(), iter, &meta,
                   user_comparator(), newest_snapshot,
                   earliest_seqno_in_memtable, GetCompressionFlush(options_));
    LogFlush(options_.info_log);
    delete iter;
    Log(options_.info_log, "Level-0 flush table #%lu: %lu bytes %s",
        (unsigned long) meta.number,
        (unsigned long) meta.file_size,
        s.ToString().c_str());
    mutex_.Lock();
  }
  base->Unref();
  base = versions_->current();
  int level = 0;
  if (s.ok() && meta.file_size > 0) {
    const Slice min_user_key = meta.smallest.user_key();
    const Slice max_user_key = meta.largest.user_key();
    if (base != nullptr && options_.max_background_compactions <= 1 &&
        options_.compaction_style == kCompactionStyleLevel) {
      level = base->PickLevelForMemTableOutput(min_user_key, max_user_key);
    }
    edit->AddFile(level, meta.number, meta.file_size,
                  meta.smallest, meta.largest,
                  meta.smallest_seqno, meta.largest_seqno);
  }
  CompactionStats stats;
  stats.micros = env_->NowMicros() - start_micros;
  stats.bytes_written = meta.file_size;
  stats_[level].Add(stats);
  RecordTick(options_.statistics.get(), COMPACT_WRITE_BYTES, meta.file_size);
  return s;
}
Status DBImpl::FlushMemTableToOutputFile(bool* madeProgress,
                                         DeletionState& deletion_state) {
  mutex_.AssertHeld();
  assert(imm_.size() != 0);
  if (!imm_.IsFlushPending(options_.min_write_buffer_number_to_merge)) {
    Log(options_.info_log, "FlushMemTableToOutputFile already in progress");
    Status s = Status::IOError("FlushMemTableToOutputFile already in progress");
    return s;
  }
  uint64_t file_number;
  autovector<MemTable*> mems;
  imm_.PickMemtablesToFlush(&mems);
  if (mems.empty()) {
    Log(options_.info_log, "Nothing in memstore to flush");
    Status s = Status::IOError("Nothing in memstore to flush");
    return s;
  }
  MemTable* m = mems[0];
  VersionEdit* edit = m->GetEdits();
  edit->SetPrevLogNumber(0);
  edit->SetLogNumber(
      mems.back()->GetNextLogNumber()
  );
  std::vector<uint64_t> logs_to_delete;
  for (auto mem : mems) {
    logs_to_delete.push_back(mem->GetLogNumber());
  }
  Status s = WriteLevel0Table(mems, edit, &file_number);
  if (s.ok() && shutting_down_.Acquire_Load()) {
    s = Status::IOError(
      "Database shutdown started during memtable compaction"
    );
  }
  s = imm_.InstallMemtableFlushResults(
    mems, versions_.get(), s, &mutex_, options_.info_log.get(),
    file_number, pending_outputs_, &deletion_state.memtables_to_free);
  if (s.ok()) {
    InstallSuperVersion(deletion_state);
    if (madeProgress) {
      *madeProgress = 1;
    }
    MaybeScheduleLogDBDeployStats();
    if (disable_delete_obsolete_files_ == 0) {
      deletion_state.log_delete_files.insert(
          deletion_state.log_delete_files.end(),
          logs_to_delete.begin(),
          logs_to_delete.end());
    }
  }
  return s;
}
void DBImpl::CompactRange(const Slice* begin,
                          const Slice* end,
                          bool reduce_level,
                          int target_level) {
  FlushMemTable(FlushOptions());
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
  for (int level = 0; level <= max_level_with_files; level++) {
    if (options_.compaction_style == kCompactionStyleUniversal ||
        level == max_level_with_files) {
      RunManualCompaction(level, level, begin, end);
    } else {
      RunManualCompaction(level, level + 1, begin, end);
    }
  }
  if (reduce_level) {
    ReFitLevel(max_level_with_files, target_level);
  }
  LogFlush(options_.info_log);
}
int DBImpl::FindMinimumEmptyLevelFitting(int level) {
  mutex_.AssertHeld();
  Version* current = versions_->current();
  int minimum_level = level;
  for (int i = level - 1; i > 0; --i) {
    if (current->NumLevelFiles(i) > 0) break;
    if (versions_->MaxBytesForLevel(i) < current->NumLevelBytes(level)) break;
    minimum_level = i;
  }
  return minimum_level;
}
void DBImpl::ReFitLevel(int level, int target_level) {
  assert(level < NumberLevels());
  SuperVersion* superversion_to_free = nullptr;
  SuperVersion* new_superversion = new SuperVersion();
  mutex_.Lock();
  if (refitting_level_) {
    mutex_.Unlock();
    Log(options_.info_log, "ReFitLevel: another thread is refitting");
    delete new_superversion;
    return;
  }
  refitting_level_ = true;
  bg_work_gate_closed_ = true;
  while (bg_compaction_scheduled_ > 0 || bg_flush_scheduled_) {
    Log(options_.info_log,
        "RefitLevel: waiting for background threads to stop: %d %d",
        bg_compaction_scheduled_, bg_flush_scheduled_);
    bg_cv_.Wait();
  }
  int to_level = target_level;
  if (target_level < 0) {
    to_level = FindMinimumEmptyLevelFitting(level);
  }
  assert(to_level <= level);
  if (to_level < level) {
    Log(options_.info_log, "Before refitting:\n%s",
        versions_->current()->DebugString().data());
    VersionEdit edit;
    for (const auto& f : versions_->current()->files_[level]) {
      edit.DeleteFile(level, f->number);
      edit.AddFile(to_level, f->number, f->file_size, f->smallest, f->largest,
                   f->smallest_seqno, f->largest_seqno);
    }
    Log(options_.info_log, "Apply version edit:\n%s",
        edit.DebugString().data());
    auto status = versions_->LogAndApply(&edit, &mutex_);
    superversion_to_free = InstallSuperVersion(new_superversion);
    new_superversion = nullptr;
    Log(options_.info_log, "LogAndApply: %s\n", status.ToString().data());
    if (status.ok()) {
      Log(options_.info_log, "After refitting:\n%s",
          versions_->current()->DebugString().data());
    }
  }
  refitting_level_ = false;
  bg_work_gate_closed_ = false;
  mutex_.Unlock();
  delete superversion_to_free;
  delete new_superversion;
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
  return FlushMemTable(options);
}
SequenceNumber DBImpl::GetLatestSequenceNumber() const {
  return versions_->LastSequence();
}
Status DBImpl::GetUpdatesSince(SequenceNumber seq,
                               unique_ptr<TransactionLogIterator>* iter) {
  RecordTick(options_.statistics.get(), GET_UPDATES_SINCE_CALLS);
  if (seq > versions_->LastSequence()) {
    return Status::IOError("Requested sequence not yet written in the db");
  }
  std::unique_ptr<VectorLogPtr> wal_files(new VectorLogPtr);
  Status s = GetSortedWalFiles(*wal_files);
  if (!s.ok()) {
    return s;
  }
  s = RetainProbableWalFiles(*wal_files, seq);
  if (!s.ok()) {
    return s;
  }
  iter->reset(
    new TransactionLogIteratorImpl(options_.wal_dir,
                                   &options_,
                                   storage_options_,
                                   seq,
                                   std::move(wal_files),
                                   this));
  return (*iter)->status();
}
Status DBImpl::RetainProbableWalFiles(VectorLogPtr& all_logs,
                                      const SequenceNumber target) {
  long start = 0;
  long end = static_cast<long>(all_logs.size()) - 1;
  while (end >= start) {
    long mid = start + (end - start) / 2;
    SequenceNumber current_seq_num = all_logs.at(mid)->StartSequence();
    if (current_seq_num == target) {
      end = mid;
      break;
    } else if (current_seq_num < target) {
      start = mid + 1;
    } else {
      end = mid - 1;
    }
  }
  size_t start_index = std::max(0l, end);
  all_logs.erase(all_logs.begin(), all_logs.begin() + start_index);
  return Status::OK();
}
bool DBImpl::CheckWalFileExistsAndEmpty(const WalFileType type,
                                        const uint64_t number) {
  const std::string fname = (type == kAliveLogFile) ?
    LogFileName(options_.wal_dir, number) :
    ArchivedLogFileName(options_.wal_dir, number);
  uint64_t file_size;
  Status s = env_->GetFileSize(fname, &file_size);
  return (s.ok() && (file_size == 0));
}
Status DBImpl::ReadFirstRecord(const WalFileType type, const uint64_t number,
                               WriteBatch* const result) {
  if (type == kAliveLogFile) {
    std::string fname = LogFileName(options_.wal_dir, number);
    Status status = ReadFirstLine(fname, result);
    if (!status.ok()) {
      std::string archived_file =
        ArchivedLogFileName(options_.wal_dir, number);
      Status s = ReadFirstLine(archived_file, result);
      if (!s.ok()) {
        return Status::IOError("Log File has been deleted: " + archived_file);
      }
    }
    return Status::OK();
  } else if (type == kArchivedLogFile) {
    std::string fname = ArchivedLogFileName(options_.wal_dir, number);
    Status status = ReadFirstLine(fname, result);
    return status;
  }
  return Status::NotSupported("File Type Not Known: " + std::to_string(type));
}
Status DBImpl::ReadFirstLine(const std::string& fname,
                             WriteBatch* const batch) {
  struct LogReporter : public log::Reader::Reporter {
    Env* env;
    Logger* info_log;
    const char* fname;
    Status* status;
    virtual void Corruption(size_t bytes, const Status& s) {
      Log(info_log, "%s%s: dropping %d bytes; %s",
          (this->status == nullptr ? "(ignoring error) " : ""),
          fname, static_cast<int>(bytes), s.ToString().c_str());
      if (this->status != nullptr && this->status->ok()) *this->status = s;
    }
  };
  unique_ptr<SequentialFile> file;
  Status status = env_->NewSequentialFile(fname, &file, storage_options_);
  if (!status.ok()) {
    return status;
  }
  LogReporter reporter;
  reporter.env = env_;
  reporter.info_log = options_.info_log.get();
  reporter.fname = fname.c_str();
  reporter.status = (options_.paranoid_checks ? &status : nullptr);
  log::Reader reader(std::move(file), &reporter, true ,
                     0 );
  std::string scratch;
  Slice record;
  if (reader.ReadRecord(&record, &scratch) && status.ok()) {
    if (record.size() < 12) {
      reporter.Corruption(
          record.size(), Status::Corruption("log record too small"));
      return Status::IOError("Corruption noted");
    }
    WriteBatchInternal::SetContents(batch, record);
    return Status::OK();
  }
  return Status::IOError("Error reading from file " + fname);
}
struct CompareLogByPointer {
  bool operator() (const unique_ptr<LogFile>& a,
                   const unique_ptr<LogFile>& b) {
    LogFileImpl* a_impl = dynamic_cast<LogFileImpl*>(a.get());
    LogFileImpl* b_impl = dynamic_cast<LogFileImpl*>(b.get());
    return *a_impl < *b_impl;
  }
};
Status DBImpl::AppendSortedWalsOfType(const std::string& path,
    VectorLogPtr& log_files, WalFileType log_type) {
  std::vector<std::string> all_files;
  const Status status = env_->GetChildren(path, &all_files);
  if (!status.ok()) {
    return status;
  }
  log_files.reserve(log_files.size() + all_files.size());
  VectorLogPtr::iterator pos_start;
  if (!log_files.empty()) {
    pos_start = log_files.end() - 1;
  } else {
    pos_start = log_files.begin();
  }
  for (const auto& f : all_files) {
    uint64_t number;
    FileType type;
    if (ParseFileName(f, &number, &type) && type == kLogFile){
      WriteBatch batch;
      Status s = ReadFirstRecord(log_type, number, &batch);
      if (!s.ok()) {
        if (CheckWalFileExistsAndEmpty(log_type, number)) {
          continue;
        }
        return s;
      }
      uint64_t size_bytes;
      s = env_->GetFileSize(LogFileName(path, number), &size_bytes);
      if (!s.ok()) {
        return s;
      }
      log_files.push_back(std::move(unique_ptr<LogFile>(new LogFileImpl(
        number, log_type, WriteBatchInternal::Sequence(&batch), size_bytes))));
    }
  }
  CompareLogByPointer compare_log_files;
  std::sort(pos_start, log_files.end(), compare_log_files);
  return status;
}
void DBImpl::RunManualCompaction(int input_level,
                                 int output_level,
                                 const Slice* begin,
                                 const Slice* end) {
  assert(input_level >= 0);
  InternalKey begin_storage, end_storage;
  ManualCompaction manual;
  manual.input_level = input_level;
  manual.output_level = output_level;
  manual.done = false;
  manual.in_progress = false;
  if (begin == nullptr ||
      options_.compaction_style == kCompactionStyleUniversal) {
    manual.begin = nullptr;
  } else {
    begin_storage = InternalKey(*begin, kMaxSequenceNumber, kValueTypeForSeek);
    manual.begin = &begin_storage;
  }
  if (end == nullptr ||
      options_.compaction_style == kCompactionStyleUniversal) {
    manual.end = nullptr;
  } else {
    end_storage = InternalKey(*end, 0, static_cast<ValueType>(0));
    manual.end = &end_storage;
  }
  MutexLock l(&mutex_);
  ++bg_manual_only_;
  while (bg_compaction_scheduled_ > 0) {
    Log(options_.info_log,
        "Manual compaction waiting for all other scheduled background "
        "compactions to finish");
    bg_cv_.Wait();
  }
  Log(options_.info_log, "Manual compaction starting");
  while (!manual.done && !shutting_down_.Acquire_Load() && bg_error_.ok()) {
    assert(bg_manual_only_ > 0);
    if (manual_compaction_ != nullptr) {
      bg_cv_.Wait();
    } else {
      manual_compaction_ = &manual;
      MaybeScheduleFlushOrCompaction();
    }
  }
  assert(!manual.in_progress);
  assert(bg_manual_only_ > 0);
  --bg_manual_only_;
}
void DBImpl::TEST_CompactRange(int level,
                               const Slice* begin,
                               const Slice* end) {
  int output_level = (options_.compaction_style == kCompactionStyleUniversal)
                         ? level
                         : level + 1;
  RunManualCompaction(level, output_level, begin, end);
}
Status DBImpl::FlushMemTable(const FlushOptions& options) {
  Status s = Write(WriteOptions(), nullptr);
  if (s.ok() && options.wait) {
    s = WaitForFlushMemTable();
  }
  return s;
}
Status DBImpl::WaitForFlushMemTable() {
  Status s;
  MutexLock l(&mutex_);
  while (imm_.size() > 0 && bg_error_.ok()) {
    bg_cv_.Wait();
  }
  if (imm_.size() != 0) {
    s = bg_error_;
  }
  return s;
}
Status DBImpl::TEST_FlushMemTable() {
  return FlushMemTable(FlushOptions());
}
Status DBImpl::TEST_WaitForFlushMemTable() {
  return WaitForFlushMemTable();
}
Status DBImpl::TEST_WaitForCompact() {
  MutexLock l(&mutex_);
  while ((bg_compaction_scheduled_ || bg_flush_scheduled_) &&
         bg_error_.ok()) {
    bg_cv_.Wait();
  }
  return bg_error_;
}
void DBImpl::MaybeScheduleFlushOrCompaction() {
  mutex_.AssertHeld();
  if (bg_work_gate_closed_) {
  } else if (shutting_down_.Acquire_Load()) {
  } else {
    bool is_flush_pending =
      imm_.IsFlushPending(options_.min_write_buffer_number_to_merge);
    if (is_flush_pending &&
        (bg_flush_scheduled_ < options_.max_background_flushes)) {
      bg_flush_scheduled_++;
      env_->Schedule(&DBImpl::BGWorkFlush, this, Env::Priority::HIGH);
    }
    if ((manual_compaction_ ||
         versions_->NeedsCompaction() ||
         (is_flush_pending && (options_.max_background_flushes <= 0))) &&
        bg_compaction_scheduled_ < options_.max_background_compactions &&
        (!bg_manual_only_ || manual_compaction_)) {
      bg_compaction_scheduled_++;
      env_->Schedule(&DBImpl::BGWorkCompaction, this, Env::Priority::LOW);
    }
  }
}
void DBImpl::BGWorkFlush(void* db) {
  reinterpret_cast<DBImpl*>(db)->BackgroundCallFlush();
}
void DBImpl::BGWorkCompaction(void* db) {
  reinterpret_cast<DBImpl*>(db)->BackgroundCallCompaction();
}
Status DBImpl::BackgroundFlush(bool* madeProgress,
                               DeletionState& deletion_state) {
  Status stat;
  while (stat.ok() &&
         imm_.IsFlushPending(options_.min_write_buffer_number_to_merge)) {
    Log(options_.info_log,
        "BackgroundCallFlush doing FlushMemTableToOutputFile, flush slots available %d",
        options_.max_background_flushes - bg_flush_scheduled_);
    stat = FlushMemTableToOutputFile(madeProgress, deletion_state);
  }
  return stat;
}
void DBImpl::BackgroundCallFlush() {
  bool madeProgress = false;
  DeletionState deletion_state(true);
  assert(bg_flush_scheduled_);
  MutexLock l(&mutex_);
  Status s;
  if (!shutting_down_.Acquire_Load()) {
    s = BackgroundFlush(&madeProgress, deletion_state);
    if (!s.ok()) {
      bg_cv_.SignalAll();
      Log(options_.info_log, "Waiting after background flush error: %s",
          s.ToString().c_str());
      mutex_.Unlock();
      LogFlush(options_.info_log);
      env_->SleepForMicroseconds(1000000);
      mutex_.Lock();
    }
  }
  FindObsoleteFiles(deletion_state, !s.ok());
  if (deletion_state.HaveSomethingToDelete()) {
    mutex_.Unlock();
    PurgeObsoleteFiles(deletion_state);
    mutex_.Lock();
  }
  bg_flush_scheduled_--;
  if (madeProgress) {
    MaybeScheduleFlushOrCompaction();
  }
  bg_cv_.SignalAll();
}
void DBImpl::TEST_PurgeObsoleteteWAL() {
  PurgeObsoleteWALFiles();
}
uint64_t DBImpl::TEST_GetLevel0TotalSize() {
  MutexLock l(&mutex_);
  return versions_->current()->NumLevelBytes(0);
}
void DBImpl::BackgroundCallCompaction() {
  bool madeProgress = false;
  DeletionState deletion_state(true);
  MaybeDumpStats();
  MutexLock l(&mutex_);
  assert(bg_compaction_scheduled_);
  Status s;
  if (!shutting_down_.Acquire_Load()) {
    s = BackgroundCompaction(&madeProgress, deletion_state);
    if (!s.ok()) {
      bg_cv_.SignalAll();
      Log(options_.info_log, "Waiting after background compaction error: %s",
          s.ToString().c_str());
      mutex_.Unlock();
      LogFlush(options_.info_log);
      env_->SleepForMicroseconds(1000000);
      mutex_.Lock();
    }
  }
  FindObsoleteFiles(deletion_state, !s.ok());
  if (deletion_state.HaveSomethingToDelete()) {
    mutex_.Unlock();
    PurgeObsoleteFiles(deletion_state);
    mutex_.Lock();
  }
  bg_compaction_scheduled_--;
  MaybeScheduleLogDBDeployStats();
  if (madeProgress) {
    MaybeScheduleFlushOrCompaction();
  }
  bg_cv_.SignalAll();
}
Status DBImpl::BackgroundCompaction(bool* madeProgress,
                                    DeletionState& deletion_state) {
  *madeProgress = false;
  mutex_.AssertHeld();
  while (imm_.IsFlushPending(options_.min_write_buffer_number_to_merge)) {
    Log(options_.info_log,
        "BackgroundCompaction doing FlushMemTableToOutputFile, compaction slots "
        "available %d",
        options_.max_background_compactions - bg_compaction_scheduled_);
    Status stat = FlushMemTableToOutputFile(madeProgress, deletion_state);
    if (!stat.ok()) {
      return stat;
    }
  }
  unique_ptr<Compaction> c;
  bool is_manual = (manual_compaction_ != nullptr) &&
                   (manual_compaction_->in_progress == false);
  InternalKey manual_end_storage;
  InternalKey* manual_end = &manual_end_storage;
  if (is_manual) {
    ManualCompaction* m = manual_compaction_;
    assert(!m->in_progress);
    m->in_progress = true;
    c.reset(versions_->CompactRange(
        m->input_level, m->output_level, m->begin, m->end, &manual_end));
    if (!c) {
      m->done = true;
    }
    Log(options_.info_log,
        "Manual compaction from level-%d to level-%d from %s .. %s; will stop "
        "at %s\n",
        m->input_level,
        m->output_level,
        (m->begin ? m->begin->DebugString().c_str() : "(begin)"),
        (m->end ? m->end->DebugString().c_str() : "(end)"),
        ((m->done || manual_end == nullptr)
             ? "(end)"
             : manual_end->DebugString().c_str()));
  } else if (!options_.disable_auto_compactions) {
    c.reset(versions_->PickCompaction());
  }
  Status status;
  if (!c) {
    Log(options_.info_log, "Compaction nothing to do");
  } else if (!is_manual && c->IsTrivialMove()) {
    assert(c->num_input_files(0) == 1);
    FileMetaData* f = c->input(0, 0);
    c->edit()->DeleteFile(c->level(), f->number);
    c->edit()->AddFile(c->level() + 1, f->number, f->file_size,
                       f->smallest, f->largest,
                       f->smallest_seqno, f->largest_seqno);
    status = versions_->LogAndApply(c->edit(), &mutex_);
    InstallSuperVersion(deletion_state);
    Version::LevelSummaryStorage tmp;
    Log(options_.info_log, "Moved #%lld to level-%d %lld bytes %s: %s\n",
        static_cast<unsigned long long>(f->number), c->level() + 1,
        static_cast<unsigned long long>(f->file_size),
        status.ToString().c_str(), versions_->current()->LevelSummary(&tmp));
    versions_->ReleaseCompactionFiles(c.get(), status);
    *madeProgress = true;
  } else {
    MaybeScheduleFlushOrCompaction();
    CompactionState* compact = new CompactionState(c.get());
    status = DoCompactionWork(compact, deletion_state);
    CleanupCompaction(compact, status);
    versions_->ReleaseCompactionFiles(c.get(), status);
    c->ReleaseInputs();
    *madeProgress = true;
  }
  c.reset();
  if (status.ok()) {
  } else if (shutting_down_.Acquire_Load()) {
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
    if (manual_end == nullptr) {
      m->done = true;
    }
    if (!m->done) {
      assert(options_.compaction_style != kCompactionStyleUniversal);
      m->tmp_storage = *manual_end;
      m->begin = &m->tmp_storage;
    }
    m->in_progress = false;
    manual_compaction_ = nullptr;
  }
  return status;
}
void DBImpl::CleanupCompaction(CompactionState* compact, Status status) {
  mutex_.AssertHeld();
  if (compact->builder != nullptr) {
    compact->builder->Abandon();
    compact->builder.reset();
  } else {
    assert(compact->outfile == nullptr);
  }
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    const CompactionState::Output& out = compact->outputs[i];
    pending_outputs_.erase(out.number);
    if (!status.ok()) {
      table_cache_->Evict(out.number);
    }
  }
  delete compact;
}
void DBImpl::AllocateCompactionOutputFileNumbers(CompactionState* compact) {
  mutex_.AssertHeld();
  assert(compact != nullptr);
  assert(compact->builder == nullptr);
  int filesNeeded = compact->compaction->num_input_files(1);
  for (int i = 0; i < std::max(filesNeeded, 1); i++) {
    uint64_t file_number = versions_->NewFileNumber();
    pending_outputs_.insert(file_number);
    compact->allocated_file_numbers.push_back(file_number);
  }
}
void DBImpl::ReleaseCompactionUnusedFileNumbers(CompactionState* compact) {
  mutex_.AssertHeld();
  for (const auto file_number : compact->allocated_file_numbers) {
    pending_outputs_.erase(file_number);
  }
}
Status DBImpl::OpenCompactionOutputFile(CompactionState* compact) {
  assert(compact != nullptr);
  assert(compact->builder == nullptr);
  uint64_t file_number;
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
  out.smallest_seqno = out.largest_seqno = 0;
  compact->outputs.push_back(out);
  std::string fname = TableFileName(dbname_, file_number);
  Status s = env_->NewWritableFile(fname, &compact->outfile, storage_options_);
  if (s.ok()) {
    compact->outfile->SetPreallocationBlockSize(
      1.1 * versions_->MaxFileSizeForLevel(compact->compaction->output_level()));
    CompressionType compression_type = GetCompressionType(
        options_, compact->compaction->output_level(),
        compact->compaction->enable_compression());
    compact->builder.reset(
        GetTableBuilder(options_, compact->outfile.get(), compression_type));
  }
  LogFlush(options_.info_log);
  return s;
}
Status DBImpl::FinishCompactionOutputFile(CompactionState* compact,
                                          Iterator* input) {
  assert(compact != nullptr);
  assert(compact->outfile);
  assert(compact->builder != nullptr);
  const uint64_t output_number = compact->current_output()->number;
  assert(output_number != 0);
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
  compact->builder.reset();
  if (s.ok() && !options_.disableDataSync) {
    if (options_.use_fsync) {
      StopWatch sw(env_, options_.statistics.get(),
                   COMPACTION_OUTFILE_SYNC_MICROS, false);
      s = compact->outfile->Fsync();
    } else {
      StopWatch sw(env_, options_.statistics.get(),
                   COMPACTION_OUTFILE_SYNC_MICROS, false);
      s = compact->outfile->Sync();
    }
  }
  if (s.ok()) {
    s = compact->outfile->Close();
  }
  compact->outfile.reset();
  if (s.ok() && current_entries > 0) {
    FileMetaData meta(output_number, current_bytes);
    Iterator* iter = table_cache_->NewIterator(ReadOptions(),
                                               storage_options_,
                                               meta);
    s = iter->status();
    delete iter;
    if (s.ok()) {
      Log(options_.info_log,
          "Generated table #%lu: %lu keys, %lu bytes",
          (unsigned long) output_number,
          (unsigned long) current_entries,
          (unsigned long) current_bytes);
    }
  }
  return s;
}
Status DBImpl::InstallCompactionResults(CompactionState* compact) {
  mutex_.AssertHeld();
  if (!versions_->VerifyCompactionFileConsistency(compact->compaction)) {
    Log(options_.info_log, "Compaction %d@%d + %d@%d files aborted",
      compact->compaction->num_input_files(0),
      compact->compaction->level(),
      compact->compaction->num_input_files(1),
      compact->compaction->level() + 1);
    return Status::IOError("Compaction input files inconsistent");
  }
  Log(options_.info_log, "Compacted %d@%d + %d@%d files => %lld bytes",
      compact->compaction->num_input_files(0),
      compact->compaction->level(),
      compact->compaction->num_input_files(1),
      compact->compaction->level() + 1,
      static_cast<long long>(compact->total_bytes));
  compact->compaction->AddInputDeletions(compact->compaction->edit());
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    const CompactionState::Output& out = compact->outputs[i];
    compact->compaction->edit()->AddFile(
        compact->compaction->output_level(), out.number, out.file_size,
        out.smallest, out.largest, out.smallest_seqno, out.largest_seqno);
  }
  return versions_->LogAndApply(compact->compaction->edit(), &mutex_);
}
inline SequenceNumber DBImpl::findEarliestVisibleSnapshot(
  SequenceNumber in, std::vector<SequenceNumber>& snapshots,
  SequenceNumber* prev_snapshot) {
  SequenceNumber prev __attribute__((unused)) = 0;
  for (const auto cur : snapshots) {
    assert(prev <= cur);
    if (cur >= in) {
      *prev_snapshot = prev;
      return cur;
    }
    prev = cur;
    assert(prev);
  }
  Log(options_.info_log,
      "Looking for seqid %lu but maxseqid is %lu",
      (unsigned long)in,
      (unsigned long)snapshots[snapshots.size()-1]);
  assert(0);
  return 0;
}
Status DBImpl::DoCompactionWork(CompactionState* compact,
                                DeletionState& deletion_state) {
  assert(compact);
  int64_t imm_micros = 0;
  Log(options_.info_log,
      "Compacting %d@%d + %d@%d files, score %.2f slots available %d",
      compact->compaction->num_input_files(0),
      compact->compaction->level(),
      compact->compaction->num_input_files(1),
      compact->compaction->output_level(),
      compact->compaction->score(),
      options_.max_background_compactions - bg_compaction_scheduled_);
  char scratch[256];
  compact->compaction->Summary(scratch, sizeof(scratch));
  Log(options_.info_log, "Compaction start summary: %s\n", scratch);
  assert(versions_->current()->NumLevelFiles(compact->compaction->level()) > 0);
  assert(compact->builder == nullptr);
  assert(!compact->outfile);
  SequenceNumber visible_at_tip = 0;
  SequenceNumber earliest_snapshot;
  SequenceNumber latest_snapshot = 0;
  snapshots_.getAll(compact->existing_snapshots);
  if (compact->existing_snapshots.size() == 0) {
    visible_at_tip = versions_->LastSequence();
    earliest_snapshot = visible_at_tip;
  } else {
    latest_snapshot = compact->existing_snapshots.back();
    compact->existing_snapshots.push_back(versions_->LastSequence());
    earliest_snapshot = compact->existing_snapshots[0];
  }
  bool bottommost_level = compact->compaction->BottomMostLevel();
  AllocateCompactionOutputFileNumbers(compact);
  mutex_.Unlock();
  const uint64_t start_micros = env_->NowMicros();
  unique_ptr<Iterator> input(versions_->MakeInputIterator(compact->compaction));
  input->SeekToFirst();
  Status status;
  ParsedInternalKey ikey;
  std::string current_user_key;
  bool has_current_user_key = false;
  SequenceNumber last_sequence_for_key __attribute__((unused)) =
    kMaxSequenceNumber;
  SequenceNumber visible_in_snapshot = kMaxSequenceNumber;
  std::string compaction_filter_value;
  std::vector<char> delete_key;
  MergeHelper merge(user_comparator(), options_.merge_operator.get(),
                    options_.info_log.get(),
                    false );
  auto compaction_filter = options_.compaction_filter;
  std::unique_ptr<CompactionFilter> compaction_filter_from_factory = nullptr;
  if (!compaction_filter) {
    auto context = compact->GetFilterContext();
    compaction_filter_from_factory =
      options_.compaction_filter_factory->CreateCompactionFilter(context);
    compaction_filter = compaction_filter_from_factory.get();
  }
  for (; input->Valid() && !shutting_down_.Acquire_Load(); ) {
    if (imm_.imm_flush_needed.NoBarrier_Load() != nullptr) {
      const uint64_t imm_start = env_->NowMicros();
      LogFlush(options_.info_log);
      mutex_.Lock();
      if (imm_.IsFlushPending(options_.min_write_buffer_number_to_merge)) {
        FlushMemTableToOutputFile(nullptr, deletion_state);
        bg_cv_.SignalAll();
      }
      mutex_.Unlock();
      imm_micros += (env_->NowMicros() - imm_start);
    }
    Slice key = input->key();
    Slice value = input->value();
    if (compact->compaction->ShouldStopBefore(key) &&
        compact->builder != nullptr) {
      status = FinishCompactionOutputFile(compact, input.get());
      if (!status.ok()) {
        break;
      }
    }
    bool drop = false;
    bool current_entry_is_merging = false;
    if (!ParseInternalKey(key, &ikey)) {
      current_user_key.clear();
      has_current_user_key = false;
      last_sequence_for_key = kMaxSequenceNumber;
      visible_in_snapshot = kMaxSequenceNumber;
    } else {
      if (!has_current_user_key ||
          user_comparator()->Compare(ikey.user_key,
                                     Slice(current_user_key)) != 0) {
        current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
        has_current_user_key = true;
        last_sequence_for_key = kMaxSequenceNumber;
        visible_in_snapshot = kMaxSequenceNumber;
        if (compaction_filter &&
            ikey.type == kTypeValue &&
            (visible_at_tip || ikey.sequence > latest_snapshot)) {
          bool value_changed = false;
          compaction_filter_value.clear();
          bool to_delete =
            compaction_filter->Filter(compact->compaction->level(),
                                               ikey.user_key, value,
                                               &compaction_filter_value,
                                               &value_changed);
          if (to_delete) {
            delete_key.assign(key.data(), key.data() + key.size());
            UpdateInternalKey(&delete_key[0], delete_key.size(),
                              ikey.sequence, kTypeDeletion);
            key = Slice(&delete_key[0], delete_key.size());
            ParseInternalKey(key, &ikey);
            value.clear();
            RecordTick(options_.statistics.get(), COMPACTION_KEY_DROP_USER);
          } else if (value_changed) {
            value = compaction_filter_value;
          }
        }
      }
      SequenceNumber prev_snapshot = 0;
      SequenceNumber visible = visible_at_tip ?
        visible_at_tip :
        findEarliestVisibleSnapshot(ikey.sequence,
                                    compact->existing_snapshots,
                                    &prev_snapshot);
      if (visible_in_snapshot == visible) {
        assert(last_sequence_for_key >= ikey.sequence);
        drop = true;
        RecordTick(options_.statistics.get(), COMPACTION_KEY_DROP_NEWER_ENTRY);
      } else if (ikey.type == kTypeDeletion &&
                 ikey.sequence <= earliest_snapshot &&
                 compact->compaction->IsBaseLevelForKey(ikey.user_key)) {
        drop = true;
        RecordTick(options_.statistics.get(), COMPACTION_KEY_DROP_OBSOLETE);
      } else if (ikey.type == kTypeMerge) {
        merge.MergeUntil(input.get(), prev_snapshot, bottommost_level,
                         options_.statistics.get());
        current_entry_is_merging = true;
        if (merge.IsSuccess()) {
          key = merge.key();
          ParseInternalKey(key, &ikey);
          value = merge.value();
        } else {
          assert(!merge.keys().empty());
          assert(merge.keys().size() == merge.values().size());
          ParseInternalKey(merge.keys().front(), &ikey);
        }
      }
      last_sequence_for_key = ikey.sequence;
      visible_in_snapshot = visible;
    }
#if 0
    Log(options_.info_log,
        "  Compact: %s, seq %d, type: %d %d, drop: %d, is_base: %d, "
        "%d smallest_snapshot: %d level: %d bottommost %d",
        ikey.user_key.ToString().c_str(),
        (int)ikey.sequence, ikey.type, kTypeValue, drop,
        compact->compaction->IsBaseLevelForKey(ikey.user_key),
        (int)last_sequence_for_key, (int)earliest_snapshot,
        compact->compaction->level(), bottommost_level);
#endif
    if (!drop) {
      bool has_merge_list = current_entry_is_merging && !merge.IsSuccess();
      const std::deque<std::string>* keys = nullptr;
      const std::deque<std::string>* values = nullptr;
      std::deque<std::string>::const_reverse_iterator key_iter;
      std::deque<std::string>::const_reverse_iterator value_iter;
      if (has_merge_list) {
        keys = &merge.keys();
        values = &merge.values();
        key_iter = keys->rbegin();
        value_iter = values->rbegin();
        key = Slice(*key_iter);
        value = Slice(*value_iter);
      }
      while (true) {
        char* kptr = (char*)key.data();
        std::string kstr;
        if (options_.compaction_style == kCompactionStyleLevel &&
            bottommost_level && ikey.sequence < earliest_snapshot &&
            ikey.type != kTypeMerge) {
          assert(ikey.type != kTypeDeletion);
          kstr.assign(key.data(), key.size());
          kptr = (char *)kstr.c_str();
          UpdateInternalKey(kptr, key.size(), (uint64_t)0, ikey.type);
        }
        Slice newkey(kptr, key.size());
        assert((key.clear(), 1));
        if (compact->builder == nullptr) {
          status = OpenCompactionOutputFile(compact);
          if (!status.ok()) {
            break;
          }
        }
        SequenceNumber seqno = GetInternalKeySeqno(newkey);
        if (compact->builder->NumEntries() == 0) {
          compact->current_output()->smallest.DecodeFrom(newkey);
          compact->current_output()->smallest_seqno = seqno;
        } else {
          compact->current_output()->smallest_seqno =
            std::min(compact->current_output()->smallest_seqno, seqno);
        }
        compact->current_output()->largest.DecodeFrom(newkey);
        compact->builder->Add(newkey, value);
        compact->current_output()->largest_seqno =
          std::max(compact->current_output()->largest_seqno, seqno);
        if (compact->builder->FileSize() >=
            compact->compaction->MaxOutputFileSize()) {
          status = FinishCompactionOutputFile(compact, input.get());
          if (!status.ok()) {
            break;
          }
        }
        if (has_merge_list) {
          ++key_iter;
          ++value_iter;
          if (key_iter == keys->rend() || value_iter == values->rend()) {
            assert(key_iter == keys->rend() && value_iter == values->rend());
            break;
          }
          key = Slice(*key_iter);
          value = Slice(*value_iter);
          ParseInternalKey(key, &ikey);
        } else{
          break;
        }
      }
    }
    if (!current_entry_is_merging) {
      input->Next();
    }
  }
  if (status.ok() && shutting_down_.Acquire_Load()) {
    status = Status::IOError("Database shutdown started during compaction");
  }
  if (status.ok() && compact->builder != nullptr) {
    status = FinishCompactionOutputFile(compact, input.get());
  }
  if (status.ok()) {
    status = input->status();
  }
  input.reset();
  CompactionStats stats;
  stats.micros = env_->NowMicros() - start_micros - imm_micros;
  MeasureTime(options_.statistics.get(), COMPACTION_TIME, stats.micros);
  stats.files_in_leveln = compact->compaction->num_input_files(0);
  stats.files_in_levelnp1 = compact->compaction->num_input_files(1);
  int num_output_files = compact->outputs.size();
  if (compact->builder != nullptr) {
    assert(num_output_files > 0);
    --num_output_files;
  }
  stats.files_out_levelnp1 = num_output_files;
  for (int i = 0; i < compact->compaction->num_input_files(0); i++) {
    stats.bytes_readn += compact->compaction->input(0, i)->file_size;
    RecordTick(options_.statistics.get(), COMPACT_READ_BYTES,
               compact->compaction->input(0, i)->file_size);
  }
  for (int i = 0; i < compact->compaction->num_input_files(1); i++) {
    stats.bytes_readnp1 += compact->compaction->input(1, i)->file_size;
    RecordTick(options_.statistics.get(), COMPACT_READ_BYTES,
               compact->compaction->input(1, i)->file_size);
  }
  for (int i = 0; i < num_output_files; i++) {
    stats.bytes_written += compact->outputs[i].file_size;
    RecordTick(options_.statistics.get(), COMPACT_WRITE_BYTES,
               compact->outputs[i].file_size);
  }
  LogFlush(options_.info_log);
  mutex_.Lock();
  stats_[compact->compaction->output_level()].Add(stats);
  ReleaseCompactionUnusedFileNumbers(compact);
  if (status.ok()) {
    status = InstallCompactionResults(compact);
    InstallSuperVersion(deletion_state);
  }
  Version::LevelSummaryStorage tmp;
  Log(options_.info_log,
      "compacted to: %s, %.1f MB/sec, level %d, files in(%d, %d) out(%d) "
      "MB in(%.1f, %.1f) out(%.1f), read-write-amplify(%.1f) "
      "write-amplify(%.1f) %s\n",
      versions_->current()->LevelSummary(&tmp),
      (stats.bytes_readn + stats.bytes_readnp1 + stats.bytes_written) /
          (double)stats.micros,
      compact->compaction->output_level(), stats.files_in_leveln,
      stats.files_in_levelnp1, stats.files_out_levelnp1,
      stats.bytes_readn / 1048576.0, stats.bytes_readnp1 / 1048576.0,
      stats.bytes_written / 1048576.0,
      (stats.bytes_written + stats.bytes_readnp1 + stats.bytes_readn) /
          (double)stats.bytes_readn,
      stats.bytes_written / (double)stats.bytes_readn,
      status.ToString().c_str());
  return status;
}
namespace {
struct IterState {
  port::Mutex* mu;
  Version* version;
  autovector<MemTable*> mem;
  DBImpl *db;
};
static void CleanupIteratorState(void* arg1, void* arg2) {
  IterState* state = reinterpret_cast<IterState*>(arg1);
  DBImpl::DeletionState deletion_state;
  state->mu->Lock();
  auto mems_size = state->mem.size();
  for (size_t i = 0; i < mems_size; i++) {
    MemTable* m = state->mem[i]->Unref();
    if (m != nullptr) {
      deletion_state.memtables_to_free.push_back(m);
    }
  }
  if (state->version->Unref()) {
    state->db->FindObsoleteFiles(deletion_state, false, true);
  }
  state->mu->Unlock();
  state->db->PurgeObsoleteFiles(deletion_state);
  delete state;
}
}
Iterator* DBImpl::NewInternalIterator(const ReadOptions& options,
                                      SequenceNumber* latest_snapshot) {
  IterState* cleanup = new IterState;
  MemTable* mutable_mem;
  autovector<MemTable*> immutables;
  Version* version;
  mutex_.Lock();
  *latest_snapshot = versions_->LastSequence();
  mem_->Ref();
  mutable_mem = mem_;
  imm_.GetMemTables(&immutables);
  for (unsigned int i = 0; i < immutables.size(); i++) {
    immutables[i]->Ref();
  }
  versions_->current()->Ref();
  version = versions_->current();
  mutex_.Unlock();
  std::vector<Iterator*> memtables;
  memtables.push_back(mutable_mem->NewIterator(options));
  cleanup->mem.push_back(mutable_mem);
  for (MemTable* m : immutables) {
    memtables.push_back(m->NewIterator(options));
    cleanup->mem.push_back(m);
  }
  version->AddIterators(options, storage_options_, &memtables);
  Iterator* internal_iter = NewMergingIterator(
      env_, &internal_comparator_, memtables.data(), memtables.size()
  );
  cleanup->version = version;
  cleanup->mu = &mutex_;
  cleanup->db = this;
  internal_iter->RegisterCleanup(CleanupIteratorState, cleanup, nullptr);
  return internal_iter;
}
Iterator* DBImpl::TEST_NewInternalIterator() {
  SequenceNumber ignored;
  return NewInternalIterator(ReadOptions(), &ignored);
}
int64_t DBImpl::TEST_MaxNextLevelOverlappingBytes() {
  MutexLock l(&mutex_);
  return versions_->current()->MaxNextLevelOverlappingBytes();
}
Status DBImpl::Get(const ReadOptions& options,
                   const Slice& key,
                   std::string* value) {
  return GetImpl(options, key, value);
}
void DBImpl::InstallSuperVersion(DeletionState& deletion_state) {
  SuperVersion* new_superversion =
    (deletion_state.new_superversion != nullptr) ?
    deletion_state.new_superversion : new SuperVersion();
  SuperVersion* old_superversion = InstallSuperVersion(new_superversion);
  deletion_state.new_superversion = nullptr;
  if (deletion_state.superversion_to_free != nullptr) {
    delete old_superversion;
  } else {
    deletion_state.superversion_to_free = old_superversion;
  }
}
DBImpl::SuperVersion* DBImpl::InstallSuperVersion(
    SuperVersion* new_superversion) {
  mutex_.AssertHeld();
  new_superversion->Init(mem_, imm_, versions_->current());
  SuperVersion* old_superversion = super_version_;
  super_version_ = new_superversion;
  if (old_superversion != nullptr && old_superversion->Unref()) {
    old_superversion->Cleanup();
    return old_superversion;
  }
  return nullptr;
}
Status DBImpl::GetImpl(const ReadOptions& options,
                       const Slice& key,
                       std::string* value,
                       bool* value_found) {
  Status s;
  StopWatch sw(env_, options_.statistics.get(), DB_GET, false);
  StopWatchNano snapshot_timer(env_, false);
  StartPerfTimer(&snapshot_timer);
  SequenceNumber snapshot;
  if (options.snapshot != nullptr) {
    snapshot = reinterpret_cast<const SnapshotImpl*>(options.snapshot)->number_;
  } else {
    snapshot = versions_->LastSequence();
  }
  mutex_.Lock();
  SuperVersion* get_version = super_version_->Ref();
  mutex_.Unlock();
  bool have_stat_update = false;
  Version::GetStats stats;
  MergeContext merge_context;
  LookupKey lkey(key, snapshot);
  BumpPerfTime(&perf_context.get_snapshot_time, &snapshot_timer);
  if (get_version->mem->Get(lkey, value, &s, merge_context, options_)) {
    RecordTick(options_.statistics.get(), MEMTABLE_HIT);
  } else if (get_version->imm.Get(lkey, value, &s, merge_context, options_)) {
    RecordTick(options_.statistics.get(), MEMTABLE_HIT);
  } else {
    StopWatchNano from_files_timer(env_, false);
    StartPerfTimer(&from_files_timer);
    get_version->current->Get(options, lkey, value, &s, &merge_context, &stats,
                              options_, value_found);
    have_stat_update = true;
    BumpPerfTime(&perf_context.get_from_output_files_time, &from_files_timer);
    RecordTick(options_.statistics.get(), MEMTABLE_MISS);
  }
  StopWatchNano post_process_timer(env_, false);
  StartPerfTimer(&post_process_timer);
  bool delete_get_version = false;
  if (!options_.disable_seek_compaction && have_stat_update) {
    mutex_.Lock();
    if (get_version->current->UpdateStats(stats)) {
      MaybeScheduleFlushOrCompaction();
    }
    if (get_version->Unref()) {
      get_version->Cleanup();
      delete_get_version = true;
    }
    mutex_.Unlock();
  } else {
    if (get_version->Unref()) {
      mutex_.Lock();
      get_version->Cleanup();
      mutex_.Unlock();
      delete_get_version = true;
    }
  }
  if (delete_get_version) {
    delete get_version;
  }
  RecordTick(options_.statistics.get(), NUMBER_KEYS_READ);
  RecordTick(options_.statistics.get(), BYTES_READ, value->size());
  BumpPerfTime(&perf_context.get_post_process_time, &post_process_timer);
  return s;
}
std::vector<Status> DBImpl::MultiGet(const ReadOptions& options,
                                     const std::vector<Slice>& keys,
                                     std::vector<std::string>* values) {
  StopWatch sw(env_, options_.statistics.get(), DB_MULTIGET, false);
  StopWatchNano snapshot_timer(env_, false);
  StartPerfTimer(&snapshot_timer);
  SequenceNumber snapshot;
  autovector<MemTable*> to_delete;
  mutex_.Lock();
  if (options.snapshot != nullptr) {
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
  mutex_.Unlock();
  bool have_stat_update = false;
  Version::GetStats stats;
  MergeContext merge_context;
  int numKeys = keys.size();
  std::vector<Status> statList(numKeys);
  values->resize(numKeys);
  uint64_t bytesRead = 0;
  BumpPerfTime(&perf_context.get_snapshot_time, &snapshot_timer);
  for (int i=0; i<numKeys; ++i) {
    merge_context.Clear();
    Status& s = statList[i];
    std::string* value = &(*values)[i];
    LookupKey lkey(keys[i], snapshot);
    if (mem->Get(lkey, value, &s, merge_context, options_)) {
    } else if (imm.Get(lkey, value, &s, merge_context, options_)) {
    } else {
      current->Get(options, lkey, value, &s, &merge_context, &stats, options_);
      have_stat_update = true;
    }
    if (s.ok()) {
      bytesRead += value->size();
    }
  }
  StopWatchNano post_process_timer(env_, false);
  StartPerfTimer(&post_process_timer);
  mutex_.Lock();
  if (!options_.disable_seek_compaction &&
      have_stat_update && current->UpdateStats(stats)) {
    MaybeScheduleFlushOrCompaction();
  }
  MemTable* m = mem->Unref();
  imm.UnrefAll(&to_delete);
  current->Unref();
  mutex_.Unlock();
  delete m;
  for (MemTable* v: to_delete) delete v;
  RecordTick(options_.statistics.get(), NUMBER_MULTIGET_CALLS);
  RecordTick(options_.statistics.get(), NUMBER_MULTIGET_KEYS_READ, numKeys);
  RecordTick(options_.statistics.get(), NUMBER_MULTIGET_BYTES_READ, bytesRead);
  BumpPerfTime(&perf_context.get_post_process_time, &post_process_timer);
  return statList;
}
bool DBImpl::KeyMayExist(const ReadOptions& options,
                         const Slice& key,
                         std::string* value,
                         bool* value_found) {
  if (value_found != nullptr) {
    *value_found = true;
  }
  ReadOptions roptions = options;
  roptions.read_tier = kBlockCacheTier;
  auto s = GetImpl(roptions, key, value, value_found);
  return s.ok() || s.IsIncomplete();
}
Iterator* DBImpl::NewIterator(const ReadOptions& options) {
  SequenceNumber latest_snapshot;
  Iterator* iter = NewInternalIterator(options, &latest_snapshot);
  iter = NewDBIterator(
             &dbname_, env_, options_, user_comparator(), iter,
             (options.snapshot != nullptr
              ? reinterpret_cast<const SnapshotImpl*>(options.snapshot)->number_
              : latest_snapshot));
  if (options.prefix) {
    iter = new PrefixFilterIterator(iter, *options.prefix,
                                    options_.prefix_extractor);
  }
  return iter;
}
const Snapshot* DBImpl::GetSnapshot() {
  MutexLock l(&mutex_);
  return snapshots_.New(versions_->LastSequence());
}
void DBImpl::ReleaseSnapshot(const Snapshot* s) {
  MutexLock l(&mutex_);
  snapshots_.Delete(reinterpret_cast<const SnapshotImpl*>(s));
}
Status DBImpl::Put(const WriteOptions& o, const Slice& key, const Slice& val) {
  return DB::Put(o, key, val);
}
Status DBImpl::Merge(const WriteOptions& o, const Slice& key,
                     const Slice& val) {
  if (!options_.merge_operator) {
    return Status::NotSupported("Provide a merge_operator when opening DB");
  } else {
    return DB::Merge(o, key, val);
  }
}
Status DBImpl::Delete(const WriteOptions& options, const Slice& key) {
  return DB::Delete(options, key);
}
Status DBImpl::Write(const WriteOptions& options, WriteBatch* my_batch) {
  StopWatchNano pre_post_process_timer(env_, false);
  StartPerfTimer(&pre_post_process_timer);
  Writer w(&mutex_);
  w.batch = my_batch;
  w.sync = options.sync;
  w.disableWAL = options.disableWAL;
  w.done = false;
  StopWatch sw(env_, options_.statistics.get(), DB_WRITE, false);
  mutex_.Lock();
  writers_.push_back(&w);
  while (!w.done && &w != writers_.front()) {
    w.cv.Wait();
  }
  if (!options.disableWAL) {
    RecordTick(options_.statistics.get(), WRITE_WITH_WAL, 1);
  }
  if (w.done) {
    mutex_.Unlock();
    RecordTick(options_.statistics.get(), WRITE_DONE_BY_OTHER, 1);
    return w.status;
  } else {
    RecordTick(options_.statistics.get(), WRITE_DONE_BY_SELF, 1);
  }
  SuperVersion* superversion_to_free = nullptr;
  Status status = MakeRoomForWrite(my_batch == nullptr, &superversion_to_free);
  uint64_t last_sequence = versions_->LastSequence();
  Writer* last_writer = &w;
  if (status.ok() && my_batch != nullptr) {
    autovector<WriteBatch*> write_batch_group;
    BuildBatchGroup(&last_writer, &write_batch_group);
    {
      mutex_.Unlock();
      WriteBatch* updates = nullptr;
      if (write_batch_group.size() == 1) {
        updates = write_batch_group[0];
      } else {
        updates = &tmp_batch_;
        for (size_t i = 0; i < write_batch_group.size(); ++i) {
          WriteBatchInternal::Append(updates, write_batch_group[i]);
        }
      }
      const SequenceNumber current_sequence = last_sequence + 1;
      WriteBatchInternal::SetSequence(updates, current_sequence);
      int my_batch_count = WriteBatchInternal::Count(updates);
      last_sequence += my_batch_count;
      RecordTick(options_.statistics.get(),
                 NUMBER_KEYS_WRITTEN, my_batch_count);
      RecordTick(options_.statistics.get(),
                 BYTES_WRITTEN,
                 WriteBatchInternal::ByteSize(updates));
      if (options.disableWAL) {
        flush_on_destroy_ = true;
      }
      BumpPerfTime(&perf_context.write_pre_and_post_process_time,
                   &pre_post_process_timer);
      if (!options.disableWAL) {
        StopWatchNano timer(env_);
        StartPerfTimer(&timer);
        Slice log_entry = WriteBatchInternal::Contents(updates);
        status = log_->AddRecord(log_entry);
        RecordTick(options_.statistics.get(), WAL_FILE_SYNCED, 1);
        RecordTick(options_.statistics.get(), WAL_FILE_BYTES, log_entry.size());
        if (status.ok() && options.sync) {
          if (options_.use_fsync) {
            StopWatch(env_, options_.statistics.get(), WAL_FILE_SYNC_MICROS);
            status = log_->file()->Fsync();
          } else {
            StopWatch(env_, options_.statistics.get(), WAL_FILE_SYNC_MICROS);
            status = log_->file()->Sync();
          }
        }
        BumpPerfTime(&perf_context.write_wal_time, &timer);
      }
      if (status.ok()) {
        StopWatchNano write_memtable_timer(env_, false);
        StartPerfTimer(&write_memtable_timer);
        status = WriteBatchInternal::InsertInto(updates, mem_, &options_, this,
                                                options_.filter_deletes);
        BumpPerfTime(&perf_context.write_memtable_time, &write_memtable_timer);
        if (!status.ok()) {
          throw std::runtime_error("In memory WriteBatch corruption!");
        }
        SetTickerCount(options_.statistics.get(), SEQUENCE_NUMBER,
                       last_sequence);
      }
      StartPerfTimer(&pre_post_process_timer);
      if (updates == &tmp_batch_) tmp_batch_.Clear();
      mutex_.Lock();
      if (status.ok()) {
        versions_->SetLastSequence(last_sequence);
      }
    }
  }
  if (options_.paranoid_checks && !status.ok() && bg_error_.ok()) {
    bg_error_ = status;
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
  if (!writers_.empty()) {
    writers_.front()->cv.Signal();
  }
  mutex_.Unlock();
  delete superversion_to_free;
  BumpPerfTime(&perf_context.write_pre_and_post_process_time,
               &pre_post_process_timer);
  return status;
}
void DBImpl::BuildBatchGroup(Writer** last_writer,
                             autovector<WriteBatch*>* write_batch_group) {
  assert(!writers_.empty());
  Writer* first = writers_.front();
  assert(first->batch != nullptr);
  size_t size = WriteBatchInternal::ByteSize(first->batch);
  write_batch_group->push_back(first->batch);
  size_t max_size = 1 << 20;
  if (size <= (128<<10)) {
    max_size = size + (128<<10);
  }
  *last_writer = first;
  std::deque<Writer*>::iterator iter = writers_.begin();
  ++iter;
  for (; iter != writers_.end(); ++iter) {
    Writer* w = *iter;
    if (w->sync && !first->sync) {
      break;
    }
    if (!w->disableWAL && first->disableWAL) {
      break;
    }
    if (w->batch != nullptr) {
      size += WriteBatchInternal::ByteSize(w->batch);
      if (size > max_size) {
        break;
      }
      write_batch_group->push_back(w->batch);
    }
    *last_writer = w;
  }
}
uint64_t DBImpl::SlowdownAmount(int n, double bottom, double top) {
  uint64_t delay;
  if (n >= top) {
    delay = 1000;
  }
  else if (n < bottom) {
    delay = 0;
  }
  else {
    double how_much =
      (double) (n - bottom) /
              (top - bottom);
    delay = std::max(how_much * how_much * 1000, 100.0);
  }
  assert(delay <= 1000);
  return delay;
}
Status DBImpl::MakeRoomForWrite(bool force,
                                SuperVersion** superversion_to_free) {
  mutex_.AssertHeld();
  assert(!writers_.empty());
  bool allow_delay = !force;
  bool allow_hard_rate_limit_delay = !force;
  bool allow_soft_rate_limit_delay = !force;
  uint64_t rate_limit_delay_millis = 0;
  Status s;
  double score;
  *superversion_to_free = nullptr;
  while (true) {
    if (!bg_error_.ok()) {
      s = bg_error_;
      break;
    } else if (allow_delay && versions_->NeedSlowdownForNumLevel0Files()) {
      uint64_t slowdown =
          SlowdownAmount(versions_->current()->NumLevelFiles(0),
                         options_.level0_slowdown_writes_trigger,
                         options_.level0_stop_writes_trigger);
      mutex_.Unlock();
      uint64_t delayed;
      {
        StopWatch sw(env_, options_.statistics.get(), STALL_L0_SLOWDOWN_COUNT);
        env_->SleepForMicroseconds(slowdown);
        delayed = sw.ElapsedMicros();
      }
      RecordTick(options_.statistics.get(), STALL_L0_SLOWDOWN_MICROS, delayed);
      stall_level0_slowdown_ += delayed;
      stall_level0_slowdown_count_++;
      allow_delay = false;
      mutex_.Lock();
      delayed_writes_++;
    } else if (!force &&
               (mem_->ApproximateMemoryUsage() <= options_.write_buffer_size)) {
      if (allow_delay) {
        DelayLoggingAndReset();
      }
      break;
    } else if (imm_.size() == options_.max_write_buffer_number - 1) {
      DelayLoggingAndReset();
      Log(options_.info_log, "wait for memtable compaction...\n");
      uint64_t stall;
      {
        StopWatch sw(env_, options_.statistics.get(),
          STALL_MEMTABLE_COMPACTION_COUNT);
        bg_cv_.Wait();
        stall = sw.ElapsedMicros();
      }
      RecordTick(options_.statistics.get(),
                 STALL_MEMTABLE_COMPACTION_MICROS, stall);
      stall_memtable_compaction_ += stall;
      stall_memtable_compaction_count_++;
    } else if (versions_->current()->NumLevelFiles(0) >=
               options_.level0_stop_writes_trigger) {
      DelayLoggingAndReset();
      Log(options_.info_log, "wait for fewer level0 files...\n");
      uint64_t stall;
      {
        StopWatch sw(env_, options_.statistics.get(),
                     STALL_L0_NUM_FILES_COUNT);
        bg_cv_.Wait();
        stall = sw.ElapsedMicros();
      }
      RecordTick(options_.statistics.get(), STALL_L0_NUM_FILES_MICROS, stall);
      stall_level0_num_files_ += stall;
      stall_level0_num_files_count_++;
    } else if (
        allow_hard_rate_limit_delay &&
        options_.hard_rate_limit > 1.0 &&
        (score = versions_->MaxCompactionScore()) > options_.hard_rate_limit) {
      int max_level = versions_->MaxCompactionScoreLevel();
      mutex_.Unlock();
      uint64_t delayed;
      {
        StopWatch sw(env_, options_.statistics.get(),
                     HARD_RATE_LIMIT_DELAY_COUNT);
        env_->SleepForMicroseconds(1000);
        delayed = sw.ElapsedMicros();
      }
      stall_leveln_slowdown_[max_level] += delayed;
      stall_leveln_slowdown_count_[max_level]++;
      uint64_t rate_limit = std::max((delayed / 1000), (uint64_t) 1);
      rate_limit_delay_millis += rate_limit;
      RecordTick(options_.statistics.get(),
                 RATE_LIMIT_DELAY_MILLIS, rate_limit);
      if (options_.rate_limit_delay_max_milliseconds > 0 &&
          rate_limit_delay_millis >=
          (unsigned)options_.rate_limit_delay_max_milliseconds) {
        allow_hard_rate_limit_delay = false;
      }
      mutex_.Lock();
    } else if (
        allow_soft_rate_limit_delay &&
        options_.soft_rate_limit > 0.0 &&
        (score = versions_->MaxCompactionScore()) > options_.soft_rate_limit) {
      mutex_.Unlock();
      {
        StopWatch sw(env_, options_.statistics.get(),
                     SOFT_RATE_LIMIT_DELAY_COUNT);
        env_->SleepForMicroseconds(SlowdownAmount(
          score,
          options_.soft_rate_limit,
          options_.hard_rate_limit)
        );
        rate_limit_delay_millis += sw.ElapsedMicros();
      }
      allow_soft_rate_limit_delay = false;
      mutex_.Lock();
    } else {
      unique_ptr<WritableFile> lfile;
      MemTable* new_mem = nullptr;
      assert(versions_->PrevLogNumber() == 0);
      uint64_t new_log_number = versions_->NewFileNumber();
      SuperVersion* new_superversion = nullptr;
      mutex_.Unlock();
      {
        EnvOptions soptions(storage_options_);
        soptions.use_mmap_writes = false;
        DelayLoggingAndReset();
        s = env_->NewWritableFile(LogFileName(options_.wal_dir, new_log_number),
                                  &lfile, soptions);
        if (s.ok()) {
          lfile->SetPreallocationBlockSize(1.1 * options_.write_buffer_size);
          new_mem = new MemTable(internal_comparator_, options_);
          new_superversion = new SuperVersion();
        }
      }
      mutex_.Lock();
      if (!s.ok()) {
        versions_->ReuseFileNumber(new_log_number);
        assert (!new_mem);
        break;
      }
      logfile_number_ = new_log_number;
      log_.reset(new log::Writer(std::move(lfile)));
      mem_->SetNextLogNumber(logfile_number_);
      imm_.Add(mem_);
      if (force) {
        imm_.FlushRequested();
      }
      mem_ = new_mem;
      mem_->Ref();
      Log(options_.info_log,
          "New memtable created with log file: #%lu\n",
          (unsigned long)logfile_number_);
      mem_->SetLogNumber(logfile_number_);
      force = false;
      MaybeScheduleFlushOrCompaction();
      *superversion_to_free = InstallSuperVersion(new_superversion);
    }
  }
  return s;
}
const std::string& DBImpl::GetName() const {
  return dbname_;
}
Env* DBImpl::GetEnv() const {
  return env_;
}
const Options& DBImpl::GetOptions() const {
  return options_;
}
bool DBImpl::GetProperty(const Slice& property, std::string* value) {
  value->clear();
  MutexLock l(&mutex_);
  Version* current = versions_->current();
  Slice in = property;
  Slice prefix("rocksdb.");
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
               current->NumLevelFiles(static_cast<int>(level)));
      *value = buf;
      return true;
    }
  } else if (in == "levelstats") {
    char buf[1000];
    snprintf(buf, sizeof(buf),
             "Level Files Size(MB)\n"
             "--------------------\n");
    value->append(buf);
    for (int level = 0; level < NumberLevels(); level++) {
      snprintf(buf, sizeof(buf),
               "%3d %8d %8.0f\n",
               level,
               current->NumLevelFiles(level),
               current->NumLevelBytes(level) / 1048576.0);
      value->append(buf);
    }
    return true;
  } else if (in == "stats") {
    char buf[1000];
    uint64_t wal_bytes = 0;
    uint64_t wal_synced = 0;
    uint64_t user_bytes_written = 0;
    uint64_t write_other = 0;
    uint64_t write_self = 0;
    uint64_t write_with_wal = 0;
    uint64_t total_bytes_written = 0;
    uint64_t total_bytes_read = 0;
    uint64_t micros_up = env_->NowMicros() - started_at_;
    double seconds_up = (micros_up + 1) / 1000000.0;
    uint64_t total_slowdown = 0;
    uint64_t total_slowdown_count = 0;
    uint64_t interval_bytes_written = 0;
    uint64_t interval_bytes_read = 0;
    uint64_t interval_bytes_new = 0;
    double interval_seconds_up = 0;
    Statistics* s = options_.statistics.get();
    if (s) {
      wal_bytes = s->getTickerCount(WAL_FILE_BYTES);
      wal_synced = s->getTickerCount(WAL_FILE_SYNCED);
      user_bytes_written = s->getTickerCount(BYTES_WRITTEN);
      write_other = s->getTickerCount(WRITE_DONE_BY_OTHER);
      write_self = s->getTickerCount(WRITE_DONE_BY_SELF);
      write_with_wal = s->getTickerCount(WRITE_WITH_WAL);
    }
    snprintf(buf, sizeof(buf),
             "                               Compactions\n"
             "Level  Files Size(MB) Score Time(sec)  Read(MB) Write(MB)    Rn(MB)  Rnp1(MB)  Wnew(MB) RW-Amplify Read(MB/s) Write(MB/s)      Rn     Rnp1     Wnp1     NewW    Count  Ln-stall Stall-cnt\n"
             "--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------\n"
             );
    value->append(buf);
    for (int level = 0; level < current->NumberLevels(); level++) {
      int files = current->NumLevelFiles(level);
      if (stats_[level].micros > 0 || files > 0) {
        int64_t bytes_read = stats_[level].bytes_readn +
                             stats_[level].bytes_readnp1;
        int64_t bytes_new = stats_[level].bytes_written -
                            stats_[level].bytes_readnp1;
        double amplify = (stats_[level].bytes_readn == 0)
            ? 0.0
            : (stats_[level].bytes_written +
               stats_[level].bytes_readnp1 +
               stats_[level].bytes_readn) /
                (double) stats_[level].bytes_readn;
        total_bytes_read += bytes_read;
        total_bytes_written += stats_[level].bytes_written;
        snprintf(
            buf, sizeof(buf),
            "%3d %8d %8.0f %5.1f %9.0f %9.0f %9.0f %9.0f %9.0f %9.0f %10.1f %9.1f %11.1f %8d %8d %8d %8d %8d %9.1f %9lu\n",
            level,
            files,
            current->NumLevelBytes(level) / 1048576.0,
            current->NumLevelBytes(level) /
                versions_->MaxBytesForLevel(level),
            stats_[level].micros / 1e6,
            bytes_read / 1048576.0,
            stats_[level].bytes_written / 1048576.0,
            stats_[level].bytes_readn / 1048576.0,
            stats_[level].bytes_readnp1 / 1048576.0,
            bytes_new / 1048576.0,
            amplify,
            (bytes_read / 1048576.0) / ((stats_[level].micros+1) / 1000000.0),
            (stats_[level].bytes_written / 1048576.0) /
                ((stats_[level].micros+1) / 1000000.0),
            stats_[level].files_in_leveln,
            stats_[level].files_in_levelnp1,
            stats_[level].files_out_levelnp1,
            stats_[level].files_out_levelnp1 - stats_[level].files_in_levelnp1,
            stats_[level].count,
            stall_leveln_slowdown_[level] / 1000000.0,
            (unsigned long) stall_leveln_slowdown_count_[level]);
        total_slowdown += stall_leveln_slowdown_[level];
        total_slowdown_count += stall_leveln_slowdown_count_[level];
        value->append(buf);
      }
    }
    interval_bytes_new = user_bytes_written - last_stats_.ingest_bytes_;
    interval_bytes_read = total_bytes_read - last_stats_.compaction_bytes_read_;
    interval_bytes_written =
        total_bytes_written - last_stats_.compaction_bytes_written_;
    interval_seconds_up = seconds_up - last_stats_.seconds_up_;
    snprintf(buf, sizeof(buf), "Uptime(secs): %.1f total, %.1f interval\n",
             seconds_up, interval_seconds_up);
    value->append(buf);
    snprintf(buf, sizeof(buf),
             "Writes cumulative: %llu total, %llu batches, "
             "%.1f per batch, %.2f ingest GB\n",
             (unsigned long long) (write_other + write_self),
             (unsigned long long) write_self,
             (write_other + write_self) / (double) (write_self + 1),
             user_bytes_written / (1048576.0 * 1024));
    value->append(buf);
    snprintf(buf, sizeof(buf),
             "WAL cumulative: %llu WAL writes, %llu WAL syncs, "
             "%.2f writes per sync, %.2f GB written\n",
             (unsigned long long) write_with_wal,
             (unsigned long long ) wal_synced,
             write_with_wal / (double) (wal_synced + 1),
             wal_bytes / (1048576.0 * 1024));
    value->append(buf);
    snprintf(buf, sizeof(buf),
             "Compaction IO cumulative (GB): "
             "%.2f new, %.2f read, %.2f write, %.2f read+write\n",
             user_bytes_written / (1048576.0 * 1024),
             total_bytes_read / (1048576.0 * 1024),
             total_bytes_written / (1048576.0 * 1024),
             (total_bytes_read + total_bytes_written) / (1048576.0 * 1024));
    value->append(buf);
    snprintf(buf, sizeof(buf),
             "Compaction IO cumulative (MB/sec): "
             "%.1f new, %.1f read, %.1f write, %.1f read+write\n",
             user_bytes_written / 1048576.0 / seconds_up,
             total_bytes_read / 1048576.0 / seconds_up,
             total_bytes_written / 1048576.0 / seconds_up,
             (total_bytes_read + total_bytes_written) / 1048576.0 / seconds_up);
    value->append(buf);
    snprintf(buf, sizeof(buf),
             "Amplification cumulative: %.1f write, %.1f compaction\n",
             (double) (total_bytes_written + wal_bytes)
                 / (user_bytes_written + 1),
             (double) (total_bytes_written + total_bytes_read + wal_bytes)
                 / (user_bytes_written + 1));
    value->append(buf);
    uint64_t interval_write_other = write_other - last_stats_.write_other_;
    uint64_t interval_write_self = write_self - last_stats_.write_self_;
    snprintf(buf, sizeof(buf),
             "Writes interval: %llu total, %llu batches, "
             "%.1f per batch, %.1f ingest MB\n",
             (unsigned long long) (interval_write_other + interval_write_self),
             (unsigned long long) interval_write_self,
             (double) (interval_write_other + interval_write_self)
                 / (interval_write_self + 1),
             (user_bytes_written - last_stats_.ingest_bytes_) / 1048576.0);
    value->append(buf);
    uint64_t interval_write_with_wal =
        write_with_wal - last_stats_.write_with_wal_;
    uint64_t interval_wal_synced = wal_synced - last_stats_.wal_synced_;
    uint64_t interval_wal_bytes = wal_bytes - last_stats_.wal_bytes_;
    snprintf(buf, sizeof(buf),
             "WAL interval: %llu WAL writes, %llu WAL syncs, "
             "%.2f writes per sync, %.2f MB written\n",
             (unsigned long long) interval_write_with_wal,
             (unsigned long long ) interval_wal_synced,
             interval_write_with_wal / (double) (interval_wal_synced + 1),
             interval_wal_bytes / (1048576.0 * 1024));
    value->append(buf);
    snprintf(buf, sizeof(buf),
             "Compaction IO interval (MB): "
             "%.2f new, %.2f read, %.2f write, %.2f read+write\n",
             interval_bytes_new / 1048576.0,
             interval_bytes_read/ 1048576.0,
             interval_bytes_written / 1048576.0,
             (interval_bytes_read + interval_bytes_written) / 1048576.0);
    value->append(buf);
    snprintf(buf, sizeof(buf),
             "Compaction IO interval (MB/sec): "
             "%.1f new, %.1f read, %.1f write, %.1f read+write\n",
             interval_bytes_new / 1048576.0 / interval_seconds_up,
             interval_bytes_read / 1048576.0 / interval_seconds_up,
             interval_bytes_written / 1048576.0 / interval_seconds_up,
             (interval_bytes_read + interval_bytes_written)
                 / 1048576.0 / interval_seconds_up);
    value->append(buf);
    snprintf(buf, sizeof(buf),
             "Amplification interval: %.1f write, %.1f compaction\n",
             (double) (interval_bytes_written + wal_bytes)
                 / (interval_bytes_new + 1),
             (double) (interval_bytes_written + interval_bytes_read + wal_bytes)
                 / (interval_bytes_new + 1));
    value->append(buf);
    snprintf(buf, sizeof(buf),
            "Stalls(secs): %.3f level0_slowdown, %.3f level0_numfiles, "
            "%.3f memtable_compaction, %.3f leveln_slowdown\n",
            stall_level0_slowdown_ / 1000000.0,
            stall_level0_num_files_ / 1000000.0,
            stall_memtable_compaction_ / 1000000.0,
            total_slowdown / 1000000.0);
    value->append(buf);
    snprintf(buf, sizeof(buf),
            "Stalls(count): %lu level0_slowdown, %lu level0_numfiles, "
            "%lu memtable_compaction, %lu leveln_slowdown\n",
            (unsigned long) stall_level0_slowdown_count_,
            (unsigned long) stall_level0_num_files_count_,
            (unsigned long) stall_memtable_compaction_count_,
            (unsigned long) total_slowdown_count);
    value->append(buf);
    last_stats_.compaction_bytes_read_ = total_bytes_read;
    last_stats_.compaction_bytes_written_ = total_bytes_written;
    last_stats_.ingest_bytes_ = user_bytes_written;
    last_stats_.seconds_up_ = seconds_up;
    last_stats_.wal_bytes_ = wal_bytes;
    last_stats_.wal_synced_ = wal_synced;
    last_stats_.write_with_wal_ = write_with_wal;
    last_stats_.write_other_ = write_other;
    last_stats_.write_self_ = write_self;
    return true;
  } else if (in == "sstables") {
    *value = versions_->current()->DebugString();
    return true;
  } else if (in == "num-immutable-mem-table") {
    *value = std::to_string(imm_.size());
    return true;
  }
  return false;
}
void DBImpl::GetApproximateSizes(
    const Range* range, int n,
    uint64_t* sizes) {
  Version* v;
  {
    MutexLock l(&mutex_);
    versions_->current()->Ref();
    v = versions_->current();
  }
  for (int i = 0; i < n; i++) {
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
Status DBImpl::DeleteFile(std::string name) {
  uint64_t number;
  FileType type;
  WalFileType log_type;
  if (!ParseFileName(name, &number, &type, &log_type) ||
      (type != kTableFile && type != kLogFile)) {
    Log(options_.info_log, "DeleteFile %s failed.\n", name.c_str());
    return Status::InvalidArgument("Invalid file name");
  }
  Status status;
  if (type == kLogFile) {
    if (log_type != kArchivedLogFile) {
      Log(options_.info_log, "DeleteFile %s failed.\n", name.c_str());
      return Status::NotSupported("Delete only supported for archived logs");
    }
    status = env_->DeleteFile(options_.wal_dir + "/" + name.c_str());
    if (!status.ok()) {
      Log(options_.info_log, "DeleteFile %s failed.\n", name.c_str());
    }
    return status;
  }
  int level;
  FileMetaData* metadata;
  int maxlevel = NumberLevels();
  VersionEdit edit;
  DeletionState deletion_state(true);
  {
    MutexLock l(&mutex_);
    status = versions_->GetMetadataForFile(number, &level, &metadata);
    if (!status.ok()) {
      Log(options_.info_log, "DeleteFile %s failed. File not found\n",
                             name.c_str());
      return Status::InvalidArgument("File not found");
    }
    assert((level > 0) && (level < maxlevel));
    if (metadata->being_compacted) {
      Log(options_.info_log,
          "DeleteFile %s Skipped. File about to be compacted\n", name.c_str());
      return Status::OK();
    }
    for (int i = level + 1; i < maxlevel; i++) {
      if (versions_->current()->NumLevelFiles(i) != 0) {
        Log(options_.info_log,
            "DeleteFile %s FAILED. File not in last level\n", name.c_str());
        return Status::InvalidArgument("File not in last level");
      }
    }
    edit.DeleteFile(level, number);
    status = versions_->LogAndApply(&edit, &mutex_);
    if (status.ok()) {
      InstallSuperVersion(deletion_state);
    }
    FindObsoleteFiles(deletion_state, false);
  }
  LogFlush(options_.info_log);
  PurgeObsoleteFiles(deletion_state);
  return status;
}
void DBImpl::GetLiveFilesMetaData(std::vector<LiveFileMetaData> *metadata) {
  MutexLock l(&mutex_);
  return versions_->GetLiveFilesMetaData(metadata);
}
Status DBImpl::GetDbIdentity(std::string& identity) {
  std::string idfilename = IdentityFileName(dbname_);
  unique_ptr<SequentialFile> idfile;
  const EnvOptions soptions;
  Status s = env_->NewSequentialFile(idfilename, &idfile, soptions);
  if (!s.ok()) {
    return s;
  }
  uint64_t file_size;
  s = env_->GetFileSize(idfilename, &file_size);
  if (!s.ok()) {
    return s;
  }
  char buffer[file_size];
  Slice id;
  s = idfile->Read(file_size, &id, buffer);
  if (!s.ok()) {
    return s;
  }
  identity.assign(id.ToString());
  if (identity.size() > 0 && identity.back() == '\n') {
    identity.pop_back();
  }
  return s;
}
Status DB::Put(const WriteOptions& opt, const Slice& key, const Slice& value) {
  WriteBatch batch(key.size() + value.size() + 24);
  batch.Put(key, value);
  return Write(opt, &batch);
}
Status DB::Delete(const WriteOptions& opt, const Slice& key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(opt, &batch);
}
Status DB::Merge(const WriteOptions& opt, const Slice& key,
                 const Slice& value) {
  WriteBatch batch;
  batch.Merge(key, value);
  return Write(opt, &batch);
}
DB::~DB() { }
Status DB::Open(const Options& options, const std::string& dbname, DB** dbptr) {
  *dbptr = nullptr;
  EnvOptions soptions;
  if (options.block_cache != nullptr && options.no_block_cache) {
    return Status::InvalidArgument(
        "no_block_cache is true while block_cache is not nullptr");
  }
  DBImpl* impl = new DBImpl(options, dbname);
  Status s = impl->env_->CreateDirIfMissing(impl->options_.wal_dir);
  if (!s.ok()) {
    delete impl;
    return s;
  }
  s = impl->CreateArchivalDirectory();
  if (!s.ok()) {
    delete impl;
    return s;
  }
<<<<<<< HEAD
  impl->mutex_.Lock();
  VersionEdit edit;
  s = impl->Recover(&edit);
||||||| 0f4a75b71
  impl->mutex_.Lock();
  VersionEdit edit;
  s = impl->Recover(&edit);
=======
  impl->mutex_.Lock();
  s = impl->Recover();
>>>>>>> 4e91f27c
  if (s.ok()) {
    uint64_t new_log_number = impl->versions_->NewFileNumber();
    unique_ptr<WritableFile> lfile;
    soptions.use_mmap_writes = false;
    s = impl->options_.env->NewWritableFile(
      LogFileName(impl->options_.wal_dir, new_log_number),
      &lfile,
      soptions
    );
    if (s.ok()) {
      lfile->SetPreallocationBlockSize(1.1 * impl->options_.write_buffer_size);
      VersionEdit edit;
      edit.SetLogNumber(new_log_number);
      impl->logfile_number_ = new_log_number;
      impl->log_.reset(new log::Writer(std::move(lfile)));
      s = impl->versions_->LogAndApply(&edit, &impl->mutex_);
    }
    if (s.ok()) {
      delete impl->InstallSuperVersion(new DBImpl::SuperVersion());
      impl->mem_->SetLogNumber(impl->logfile_number_);
      impl->DeleteObsoleteFiles();
      impl->MaybeScheduleFlushOrCompaction();
      impl->MaybeScheduleLogDBDeployStats();
    }
  }
  if (s.ok() && impl->options_.compaction_style == kCompactionStyleUniversal) {
    Version* current = impl->versions_->current();
    for (int i = 1; i < impl->NumberLevels(); i++) {
      int num_files = current->NumLevelFiles(i);
      if (num_files > 0) {
        s = Status::InvalidArgument("Not all files are at level 0. Cannot "
          "open with universal compaction style.");
        break;
      }
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
  const InternalKeyComparator comparator(options.comparator);
  const InternalFilterPolicy filter_policy(options.filter_policy);
  const Options& soptions(SanitizeOptions(
    dbname, &comparator, &filter_policy, options));
  Env* env = soptions.env;
  std::vector<std::string> filenames;
  std::vector<std::string> archiveFiles;
  std::string archivedir = ArchivalDirectory(dbname);
  env->GetChildren(dbname, &filenames);
  if (dbname != soptions.wal_dir) {
    std::vector<std::string> logfilenames;
    env->GetChildren(soptions.wal_dir, &logfilenames);
    filenames.insert(filenames.end(), logfilenames.begin(), logfilenames.end());
    archivedir = ArchivalDirectory(soptions.wal_dir);
  }
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
          type != kDBLockFile) {
        Status del;
        if (type == kMetaDatabase) {
          del = DestroyDB(dbname + "/" + filenames[i], options);
        } else if (type == kLogFile) {
          del = env->DeleteFile(soptions.wal_dir + "/" + filenames[i]);
        } else {
          del = env->DeleteFile(dbname + "/" + filenames[i]);
        }
        if (result.ok() && !del.ok()) {
          result = del;
        }
      }
    }
    env->GetChildren(archivedir, &archiveFiles);
    for (size_t i = 0; i < archiveFiles.size(); ++i) {
      if (ParseFileName(archiveFiles[i], &number, &type) &&
          type == kLogFile) {
        Status del = env->DeleteFile(archivedir + "/" + archiveFiles[i]);
        if (result.ok() && !del.ok()) {
          result = del;
        }
      }
    }
    env->DeleteDir(archivedir);
    env->UnlockFile(lock);
    env->DeleteFile(lockname);
    env->DeleteDir(dbname);
    env->DeleteDir(soptions.wal_dir);
  }
  return result;
}
void DumpLeveldbBuildVersion(Logger * log) {
  Log(log, "Git sha %s", rocksdb_build_git_sha);
  Log(log, "Compile time %s %s",
      rocksdb_build_compile_time, rocksdb_build_compile_date);
}
}
