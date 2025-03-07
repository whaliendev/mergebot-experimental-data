#include "utilities/backupable_db.h"
#include "db/filename.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "rocksdb/transaction_log.h"
#define __STDC_FORMAT_MACROS 
#include <inttypes.h>
#include <algorithm>
#include <vector>
#include <map>
#include <string>
#include <limits>
#include <atomic>
#include <unordered_map>
namespace rocksdb {
void BackupableDBOptions::Dump(Logger* logger) const {
  Log(logger, "       Options.backup_dir: %s", backup_dir.c_str());
  Log(logger, "       Options.backup_env: %p", backup_env);
  Log(logger, "Options.share_table_files: %d",
      static_cast<int>(share_table_files));
  Log(logger, "         Options.info_log: %p", info_log);
  Log(logger, "             Options.sync: %d", static_cast<int>(sync));
  Log(logger, " Options.destroy_old_data: %d",
      static_cast<int>(destroy_old_data));
  Log(logger, " Options.backup_log_files: %d",
      static_cast<int>(backup_log_files));
}
class BackupEngineImpl : public BackupEngine {
 public:
  BackupEngineImpl(Env* db_env, const BackupableDBOptions& options);
  ~BackupEngineImpl();
  Status CreateNewBackup(DB* db, bool flush_before_backup = false);
  Status PurgeOldBackups(uint32_t num_backups_to_keep);
  Status DeleteBackup(BackupID backup_id);
  void StopBackup() {
    stop_backup_.store(true, std::memory_order_release);
  }
  void GetBackupInfo(std::vector<BackupInfo>* backup_info);
  Status RestoreDBFromBackup(BackupID backup_id, const std::string& db_dir,
                             const std::string& wal_dir,
                             const RestoreOptions& restore_options =
                                 RestoreOptions());
  Status RestoreDBFromLatestBackup(const std::string& db_dir,
                                   const std::string& wal_dir,
                                   const RestoreOptions& restore_options =
                                       RestoreOptions()) {
    return RestoreDBFromBackup(latest_backup_id_, db_dir, wal_dir,
                               restore_options);
  }
 private:
  void DeleteChildren(const std::string& dir, uint32_t file_type_filter = 0);
  struct FileInfo {
    FileInfo(const std::string& fname, uint64_t sz, uint32_t checksum)
      : refs(0), filename(fname), size(sz), checksum_value(checksum) {}
    int refs;
    const std::string filename;
    const uint64_t size;
    uint32_t checksum_value;
  };
  class BackupMeta {
   public:
    BackupMeta(const std::string& meta_filename,
        std::unordered_map<std::string, FileInfo>* file_infos, Env* env)
      : timestamp_(0), size_(0), meta_filename_(meta_filename),
        file_infos_(file_infos), env_(env) {}
    ~BackupMeta() {}
    void RecordTimestamp() {
      env_->GetCurrentTime(&timestamp_);
    }
    int64_t GetTimestamp() const {
      return timestamp_;
    }
    uint64_t GetSize() const {
      return size_;
    }
    void SetSequenceNumber(uint64_t sequence_number) {
      sequence_number_ = sequence_number;
    }
    uint64_t GetSequenceNumber() {
      return sequence_number_;
    }
    Status AddFile(const FileInfo& file_info);
    void Delete();
    bool Empty() {
      return files_.empty();
    }
    const std::vector<std::string>& GetFiles() {
      return files_;
    }
    Status LoadFromFile(const std::string& backup_dir);
    Status StoreToFile(bool sync);
   private:
    int64_t timestamp_;
    uint64_t sequence_number_;
    uint64_t size_;
    std::string const meta_filename_;
    std::vector<std::string> files_;
    std::unordered_map<std::string, FileInfo>* file_infos_;
    Env* env_;
    static const size_t max_backup_meta_file_size_ = 10 * 1024 * 1024;
  };
  inline std::string GetAbsolutePath(
      const std::string &relative_path = "") const {
    assert(relative_path.size() == 0 || relative_path[0] != '/');
    return options_.backup_dir + "/" + relative_path;
  }
  inline std::string GetPrivateDirRel() const {
    return "private";
  }
  inline std::string GetPrivateFileRel(BackupID backup_id,
                                       bool tmp = false,
                                       const std::string& file = "") const {
    assert(file.size() == 0 || file[0] != '/');
    return GetPrivateDirRel() + "/" + std::to_string(backup_id) +
           (tmp ? ".tmp" : "") + "/" + file;
  }
  inline std::string GetSharedFileRel(const std::string& file = "",
                                      bool tmp = false) const {
    assert(file.size() == 0 || file[0] != '/');
    return "shared/" + file + (tmp ? ".tmp" : "");
  }
  inline std::string GetLatestBackupFile(bool tmp = false) const {
    return GetAbsolutePath(std::string("LATEST_BACKUP") + (tmp ? ".tmp" : ""));
  }
  inline std::string GetBackupMetaDir() const {
    return GetAbsolutePath("meta");
  }
  inline std::string GetBackupMetaFile(BackupID backup_id) const {
    return GetBackupMetaDir() + "/" + std::to_string(backup_id);
  }
  Status GetLatestBackupFileContents(uint32_t* latest_backup);
  Status PutLatestBackupFileContents(uint32_t latest_backup);
  Status CopyFile(const std::string& src,
                  const std::string& dst,
                  Env* src_env,
                  Env* dst_env,
                  bool sync,
                  uint64_t* size = nullptr,
                  uint32_t* checksum_value = nullptr,
                  uint64_t size_limit = 0);
  Status BackupFile(BackupID backup_id,
                    BackupMeta* backup,
                    bool shared,
                    const std::string& src_dir,
                    const std::string& src_fname,
                    uint64_t size_limit = 0);
  Status CalculateChecksum(const std::string& src,
                           Env* src_env,
                           uint64_t size_limit,
                           uint32_t* checksum_value);
  void GarbageCollection(bool full_scan);
  BackupID latest_backup_id_;
  std::map<BackupID, BackupMeta> backups_;
  std::unordered_map<std::string, FileInfo> backuped_file_infos_;
  std::vector<BackupID> obsolete_backups_;
  std::atomic<bool> stop_backup_;
  BackupableDBOptions options_;
  Env* db_env_;
  Env* backup_env_;
  unique_ptr<Directory> backup_directory_;
  unique_ptr<Directory> shared_directory_;
  unique_ptr<Directory> meta_directory_;
  unique_ptr<Directory> private_directory_;
  static const size_t copy_file_buffer_size_ = 5 * 1024 * 1024LL;
};
BackupEngine* BackupEngine::NewBackupEngine(
    Env* db_env, const BackupableDBOptions& options) {
  return new BackupEngineImpl(db_env, options);
}
BackupEngineImpl::BackupEngineImpl(Env* db_env,
                                   const BackupableDBOptions& options)
    : stop_backup_(false),
      options_(options),
      db_env_(db_env),
      backup_env_(options.backup_env != nullptr ? options.backup_env
                                                : db_env_) {
  options_.Dump(options_.info_log);
  backup_env_->CreateDirIfMissing(GetAbsolutePath());
  backup_env_->NewDirectory(GetAbsolutePath(), &backup_directory_);
  if (options_.share_table_files) {
    backup_env_->CreateDirIfMissing(GetAbsolutePath(GetSharedFileRel()));
    backup_env_->NewDirectory(GetAbsolutePath(GetSharedFileRel()),
                              &shared_directory_);
  }
  backup_env_->CreateDirIfMissing(GetAbsolutePath(GetPrivateDirRel()));
  backup_env_->NewDirectory(GetAbsolutePath(GetPrivateDirRel()),
                            &private_directory_);
  backup_env_->CreateDirIfMissing(GetBackupMetaDir());
  backup_env_->NewDirectory(GetBackupMetaDir(), &meta_directory_);
  std::vector<std::string> backup_meta_files;
  backup_env_->GetChildren(GetBackupMetaDir(), &backup_meta_files);
  for (auto& file : backup_meta_files) {
    BackupID backup_id = 0;
    sscanf(file.c_str(), "%u", &backup_id);
    if (backup_id == 0 || file != std::to_string(backup_id)) {
      backup_env_->DeleteFile(GetBackupMetaDir() + "/" + file);
      continue;
    }
    assert(backups_.find(backup_id) == backups_.end());
    backups_.insert(std::make_pair(
        backup_id, BackupMeta(GetBackupMetaFile(backup_id),
                              &backuped_file_infos_, backup_env_)));
  }
  if (options_.destroy_old_data) {
    for (auto& backup : backups_) {
      backup.second.Delete();
      obsolete_backups_.push_back(backup.first);
    }
    backups_.clear();
    latest_backup_id_ = 0;
  } else {
    for (auto& backup : backups_) {
      Status s = backup.second.LoadFromFile(options_.backup_dir);
      if (!s.ok()) {
        Log(options_.info_log, "Backup %u corrupted - deleting -- %s",
            backup.first, s.ToString().c_str());
        backup.second.Delete();
        obsolete_backups_.push_back(backup.first);
      }
    }
    for (auto ob : obsolete_backups_) {
      backups_.erase(ob);
    }
    Status s = GetLatestBackupFileContents(&latest_backup_id_);
    if (!s.ok() ||
        backups_.find(latest_backup_id_) == backups_.end()) {
      auto itr = backups_.end();
      latest_backup_id_ = (itr == backups_.begin()) ? 0 : (--itr)->first;
    }
  }
  for (auto itr = backups_.upper_bound(latest_backup_id_);
       itr != backups_.end();) {
    itr->second.Delete();
    obsolete_backups_.push_back(itr->first);
    itr = backups_.erase(itr);
  }
  PutLatestBackupFileContents(latest_backup_id_);
  GarbageCollection(true);
  Log(options_.info_log,
      "Initialized BackupEngine, the latest backup is %u.",
      latest_backup_id_);
}
BackupEngineImpl::~BackupEngineImpl() { LogFlush(options_.info_log); }
Status BackupEngineImpl::CreateNewBackup(DB* db, bool flush_before_backup) {
  Status s;
  std::vector<std::string> live_files;
  VectorLogPtr live_wal_files;
  uint64_t manifest_file_size = 0;
  uint64_t sequence_number = db->GetLatestSequenceNumber();
  s = db->DisableFileDeletions();
  if (s.ok()) {
    s = db->GetLiveFiles(live_files, &manifest_file_size, flush_before_backup);
  }
  if (s.ok() && !flush_before_backup && options_.backup_log_files) {
    s = db->GetSortedWalFiles(live_wal_files);
  }
  if (!s.ok()) {
    db->EnableFileDeletions();
    return s;
  }
  BackupID new_backup_id = latest_backup_id_ + 1;
  assert(backups_.find(new_backup_id) == backups_.end());
  auto ret = backups_.insert(std::make_pair(
      new_backup_id, BackupMeta(GetBackupMetaFile(new_backup_id),
                                &backuped_file_infos_, backup_env_)));
  assert(ret.second == true);
  auto& new_backup = ret.first->second;
  new_backup.RecordTimestamp();
  new_backup.SetSequenceNumber(sequence_number);
  Log(options_.info_log, "Started the backup process -- creating backup %u",
      new_backup_id);
  s = backup_env_->CreateDir(
      GetAbsolutePath(GetPrivateFileRel(new_backup_id, true)));
  for (size_t i = 0; s.ok() && i < live_files.size(); ++i) {
    uint64_t number;
    FileType type;
    bool ok = ParseFileName(live_files[i], &number, &type);
    if (!ok) {
      assert(false);
      return Status::Corruption("Can't parse file name. This is very bad");
    }
    assert(type == kTableFile || type == kDescriptorFile ||
           type == kCurrentFile);
    s = BackupFile(new_backup_id,
                   &new_backup,
                   options_.share_table_files && type == kTableFile,
                   db->GetName(),
                   live_files[i],
                   (type == kDescriptorFile) ? manifest_file_size : 0);
  }
  for (size_t i = 0; s.ok() && i < live_wal_files.size(); ++i) {
    if (live_wal_files[i]->Type() == kAliveLogFile) {
      s = BackupFile(new_backup_id,
                     &new_backup,
                     false,
                     db->GetOptions().wal_dir,
                     live_wal_files[i]->PathName());
    }
  }
  db->EnableFileDeletions();
  if (s.ok()) {
    s = backup_env_->RenameFile(
        GetAbsolutePath(GetPrivateFileRel(new_backup_id, true)),
        GetAbsolutePath(GetPrivateFileRel(new_backup_id, false)));
  }
  if (s.ok()) {
    s = new_backup.StoreToFile(options_.sync);
  }
  if (s.ok()) {
    s = PutLatestBackupFileContents(new_backup_id);
  }
  if (s.ok() && options_.sync) {
    unique_ptr<Directory> backup_private_directory;
    backup_env_->NewDirectory(
        GetAbsolutePath(GetPrivateFileRel(new_backup_id, false)),
        &backup_private_directory);
    if (backup_private_directory != nullptr) {
      backup_private_directory->Fsync();
    }
    if (private_directory_ != nullptr) {
      private_directory_->Fsync();
    }
    if (meta_directory_ != nullptr) {
      meta_directory_->Fsync();
    }
    if (shared_directory_ != nullptr) {
      shared_directory_->Fsync();
    }
    if (backup_directory_ != nullptr) {
      backup_directory_->Fsync();
    }
  }
  if (!s.ok()) {
    Log(options_.info_log, "Backup failed -- %s", s.ToString().c_str());
    backups_.erase(new_backup_id);
    GarbageCollection(true);
    return s;
  }
  latest_backup_id_ = new_backup_id;
  Log(options_.info_log, "Backup DONE. All is good");
  return s;
}
Status BackupEngineImpl::PurgeOldBackups(uint32_t num_backups_to_keep) {
  Log(options_.info_log, "Purging old backups, keeping %u",
      num_backups_to_keep);
  while (num_backups_to_keep < backups_.size()) {
    Log(options_.info_log, "Deleting backup %u", backups_.begin()->first);
    backups_.begin()->second.Delete();
    obsolete_backups_.push_back(backups_.begin()->first);
    backups_.erase(backups_.begin());
  }
  GarbageCollection(false);
  return Status::OK();
}
Status BackupEngineImpl::DeleteBackup(BackupID backup_id) {
  Log(options_.info_log, "Deleting backup %u", backup_id);
  auto backup = backups_.find(backup_id);
  if (backup == backups_.end()) {
    return Status::NotFound("Backup not found");
  }
  backup->second.Delete();
  obsolete_backups_.push_back(backup_id);
  backups_.erase(backup);
  GarbageCollection(false);
  return Status::OK();
}
void BackupEngineImpl::GetBackupInfo(std::vector<BackupInfo>* backup_info) {
  backup_info->reserve(backups_.size());
  for (auto& backup : backups_) {
    if (!backup.second.Empty()) {
      backup_info->push_back(BackupInfo(
          backup.first, backup.second.GetTimestamp(), backup.second.GetSize()));
    }
  }
}
Status BackupEngineImpl::RestoreDBFromBackup(
    BackupID backup_id, const std::string& db_dir, const std::string& wal_dir,
    const RestoreOptions& restore_options) {
  auto backup_itr = backups_.find(backup_id);
  if (backup_itr == backups_.end()) {
    return Status::NotFound("Backup not found");
  }
  auto& backup = backup_itr->second;
  if (backup.Empty()) {
    return Status::NotFound("Backup not found");
  }
  Log(options_.info_log, "Restoring backup id %u\n", backup_id);
  Log(options_.info_log, "keep_log_files: %d\n",
      static_cast<int>(restore_options.keep_log_files));
  db_env_->CreateDirIfMissing(db_dir);
  db_env_->CreateDirIfMissing(wal_dir);
  if (restore_options.keep_log_files) {
    DeleteChildren(db_dir, 1 << kLogFile);
    std::string archive_dir = ArchivalDirectory(wal_dir);
    std::vector<std::string> archive_files;
    db_env_->GetChildren(archive_dir, &archive_files);
    for (const auto& f : archive_files) {
      uint64_t number;
      FileType type;
      bool ok = ParseFileName(f, &number, &type);
      if (ok && type == kLogFile) {
        Log(options_.info_log, "Moving log file from archive/ to wal_dir: %s",
            f.c_str());
        Status s =
            db_env_->RenameFile(archive_dir + "/" + f, wal_dir + "/" + f);
        if (!s.ok()) {
          return s;
        }
      }
    }
  } else {
    DeleteChildren(wal_dir);
    DeleteChildren(ArchivalDirectory(wal_dir));
    DeleteChildren(db_dir);
  }
  Status s;
  for (auto& file : backup.GetFiles()) {
    std::string dst;
    size_t slash = file.find_last_of('/');
    assert(slash != std::string::npos);
    dst = file.substr(slash + 1);
    uint64_t number;
    FileType type;
    bool ok = ParseFileName(dst, &number, &type);
    if (!ok) {
      return Status::Corruption("Backup corrupted");
    }
    dst = ((type == kLogFile) ? wal_dir : db_dir) +
      "/" + dst;
    Log(options_.info_log, "Restoring %s to %s\n", file.c_str(), dst.c_str());
    uint32_t checksum_value;
    s = CopyFile(GetAbsolutePath(file), dst, backup_env_, db_env_, false,
                 nullptr , &checksum_value);
    if (!s.ok()) {
      break;
    }
    const auto iter = backuped_file_infos_.find(file);
    assert(iter != backuped_file_infos_.end());
    if (iter->second.checksum_value != checksum_value) {
      s = Status::Corruption("Checksum check failed");
      break;
    }
  }
  Log(options_.info_log, "Restoring done -- %s\n", s.ToString().c_str());
  return s;
}
Status BackupEngineImpl::GetLatestBackupFileContents(uint32_t* latest_backup) {
  Status s;
  unique_ptr<SequentialFile> file;
  s = backup_env_->NewSequentialFile(GetLatestBackupFile(),
                                     &file,
                                     EnvOptions());
  if (!s.ok()) {
    return s;
  }
  char buf[11];
  Slice data;
  s = file->Read(10, &data, buf);
  if (!s.ok() || data.size() == 0) {
    return s.ok() ? Status::Corruption("Latest backup file corrupted") : s;
  }
  buf[data.size()] = 0;
  *latest_backup = 0;
  sscanf(data.data(), "%u", latest_backup);
  if (backup_env_->FileExists(GetBackupMetaFile(*latest_backup)) == false) {
    s = Status::Corruption("Latest backup file corrupted");
  }
  return Status::OK();
}
Status BackupEngineImpl::PutLatestBackupFileContents(uint32_t latest_backup) {
  Status s;
  unique_ptr<WritableFile> file;
  EnvOptions env_options;
  env_options.use_mmap_writes = false;
  s = backup_env_->NewWritableFile(GetLatestBackupFile(true),
                                   &file,
                                   env_options);
  if (!s.ok()) {
    backup_env_->DeleteFile(GetLatestBackupFile(true));
    return s;
  }
  char file_contents[10];
  int len = sprintf(file_contents, "%u\n", latest_backup);
  s = file->Append(Slice(file_contents, len));
  if (s.ok() && options_.sync) {
    file->Sync();
  }
  if (s.ok()) {
    s = file->Close();
  }
  if (s.ok()) {
    s = backup_env_->RenameFile(GetLatestBackupFile(true),
                                GetLatestBackupFile(false));
  }
  return s;
}
Status BackupEngineImpl::CopyFile(const std::string& src,
                                  const std::string& dst, Env* src_env,
                                  Env* dst_env, bool sync, uint64_t* size,
                                  uint32_t* checksum_value,
                                  uint64_t size_limit) {
  Status s;
  unique_ptr<WritableFile> dst_file;
  unique_ptr<SequentialFile> src_file;
  EnvOptions env_options;
  env_options.use_mmap_writes = false;
  env_options.use_os_buffer = false;
  if (size != nullptr) {
    *size = 0;
  }
  if (checksum_value != nullptr) {
    *checksum_value = 0;
  }
  if (size_limit == 0) {
    size_limit = std::numeric_limits<uint64_t>::max();
  }
  s = src_env->NewSequentialFile(src, &src_file, env_options);
  if (s.ok()) {
    s = dst_env->NewWritableFile(dst, &dst_file, env_options);
  }
  if (!s.ok()) {
    return s;
  }
  unique_ptr<char[]> buf(new char[copy_file_buffer_size_]);
  Slice data;
  do {
    if (stop_backup_.load(std::memory_order_acquire)) {
      return Status::Incomplete("Backup stopped");
    }
    size_t buffer_to_read = (copy_file_buffer_size_ < size_limit) ?
      copy_file_buffer_size_ : size_limit;
    s = src_file->Read(buffer_to_read, &data, buf.get());
    size_limit -= data.size();
    if (!s.ok()) {
      return s;
    }
    if (size != nullptr) {
      *size += data.size();
    }
    if (checksum_value != nullptr) {
      *checksum_value = crc32c::Extend(*checksum_value, data.data(),
                                       data.size());
    }
    s = dst_file->Append(data);
  } while (s.ok() && data.size() > 0 && size_limit > 0);
  if (s.ok() && sync) {
    s = dst_file->Sync();
  }
  return s;
}
Status BackupEngineImpl::BackupFile(BackupID backup_id, BackupMeta* backup,
                                    bool shared, const std::string& src_dir,
                                    const std::string& src_fname,
                                    uint64_t size_limit) {
  assert(src_fname.size() > 0 && src_fname[0] == '/');
  std::string dst_relative = src_fname.substr(1);
  std::string dst_relative_tmp;
  if (shared) {
    dst_relative_tmp = GetSharedFileRel(dst_relative, true);
    dst_relative = GetSharedFileRel(dst_relative, false);
  } else {
    dst_relative_tmp = GetPrivateFileRel(backup_id, true, dst_relative);
    dst_relative = GetPrivateFileRel(backup_id, false, dst_relative);
  }
  std::string dst_path = GetAbsolutePath(dst_relative);
  std::string dst_path_tmp = GetAbsolutePath(dst_relative_tmp);
  Status s;
  uint64_t size;
  uint32_t checksum_value = 0;
  if (shared && backup_env_->FileExists(dst_path)) {
    backup_env_->GetFileSize(dst_path, &size);
    Log(options_.info_log, "%s already present, calculate checksum",
        src_fname.c_str());
    s = CalculateChecksum(src_dir + src_fname,
                          db_env_,
                          size_limit,
                          &checksum_value);
  } else {
    Log(options_.info_log, "Copying %s", src_fname.c_str());
    s = CopyFile(src_dir + src_fname,
                 dst_path_tmp,
                 db_env_,
                 backup_env_,
                 options_.sync,
                 &size,
                 &checksum_value,
                 size_limit);
    if (s.ok() && shared) {
      s = backup_env_->RenameFile(dst_path_tmp, dst_path);
    }
  }
  if (s.ok()) {
    s = backup->AddFile(FileInfo(dst_relative, size, checksum_value));
  }
  return s;
}
Status BackupEngineImpl::CalculateChecksum(const std::string& src, Env* src_env,
                                           uint64_t size_limit,
                                           uint32_t* checksum_value) {
  *checksum_value = 0;
  if (size_limit == 0) {
    size_limit = std::numeric_limits<uint64_t>::max();
  }
  EnvOptions env_options;
  env_options.use_mmap_writes = false;
  env_options.use_os_buffer = false;
  std::unique_ptr<SequentialFile> src_file;
  Status s = src_env->NewSequentialFile(src, &src_file, env_options);
  if (!s.ok()) {
    return s;
  }
  std::unique_ptr<char[]> buf(new char[copy_file_buffer_size_]);
  Slice data;
  do {
    if (stop_backup_.load(std::memory_order_acquire)) {
      return Status::Incomplete("Backup stopped");
    }
    size_t buffer_to_read = (copy_file_buffer_size_ < size_limit) ?
      copy_file_buffer_size_ : size_limit;
    s = src_file->Read(buffer_to_read, &data, buf.get());
    if (!s.ok()) {
      return s;
    }
    size_limit -= data.size();
    *checksum_value = crc32c::Extend(*checksum_value, data.data(), data.size());
  } while (data.size() > 0 && size_limit > 0);
  return s;
}
void BackupEngineImpl::DeleteChildren(const std::string& dir,
                                      uint32_t file_type_filter) {
  std::vector<std::string> children;
  db_env_->GetChildren(dir, &children);
  for (const auto& f : children) {
    uint64_t number;
    FileType type;
    bool ok = ParseFileName(f, &number, &type);
    if (ok && (file_type_filter & (1 << type))) {
      continue;
    }
    db_env_->DeleteFile(dir + "/" + f);
  }
}
void BackupEngineImpl::GarbageCollection(bool full_scan) {
  Log(options_.info_log, "Starting garbage collection");
  std::vector<std::string> to_delete;
  for (auto& itr : backuped_file_infos_) {
    if (itr.second.refs == 0) {
      Status s = backup_env_->DeleteFile(GetAbsolutePath(itr.first));
      Log(options_.info_log, "Deleting %s -- %s", itr.first.c_str(),
          s.ToString().c_str());
      to_delete.push_back(itr.first);
    }
  }
  for (auto& td : to_delete) {
    backuped_file_infos_.erase(td);
  }
  if (!full_scan) {
    for (auto backup_id : obsolete_backups_) {
      std::string private_dir = GetPrivateFileRel(backup_id);
      Status s = backup_env_->DeleteDir(GetAbsolutePath(private_dir));
      Log(options_.info_log, "Deleting private dir %s -- %s",
          private_dir.c_str(), s.ToString().c_str());
    }
  }
  obsolete_backups_.clear();
  if (full_scan) {
    Log(options_.info_log, "Starting full scan garbage collection");
    std::vector<std::string> shared_children;
    backup_env_->GetChildren(GetAbsolutePath(GetSharedFileRel()),
                             &shared_children);
    for (auto& child : shared_children) {
      std::string rel_fname = GetSharedFileRel(child);
      if (backuped_file_infos_.find(rel_fname) == backuped_file_infos_.end()) {
        Status s = backup_env_->DeleteFile(GetAbsolutePath(rel_fname));
        if (s.ok()) {
          Log(options_.info_log, "Deleted %s", rel_fname.c_str());
        }
      }
    }
    std::vector<std::string> private_children;
    backup_env_->GetChildren(GetAbsolutePath(GetPrivateDirRel()),
                             &private_children);
    for (auto& child : private_children) {
      BackupID backup_id = 0;
      bool tmp_dir = child.find(".tmp") != std::string::npos;
      sscanf(child.c_str(), "%u", &backup_id);
      if (!tmp_dir &&
          (backup_id == 0 || backups_.find(backup_id) != backups_.end())) {
        continue;
      }
      std::string full_private_path =
          GetAbsolutePath(GetPrivateFileRel(backup_id, tmp_dir));
      std::vector<std::string> subchildren;
      backup_env_->GetChildren(full_private_path, &subchildren);
      for (auto& subchild : subchildren) {
        Status s = backup_env_->DeleteFile(full_private_path + subchild);
        if (s.ok()) {
          Log(options_.info_log, "Deleted %s",
              (full_private_path + subchild).c_str());
        }
      }
      Status s = backup_env_->DeleteDir(full_private_path);
      Log(options_.info_log, "Deleted dir %s -- %s", full_private_path.c_str(),
          s.ToString().c_str());
    }
  }
}
Status BackupEngineImpl::BackupMeta::AddFile(const FileInfo& file_info) {
  size_ += file_info.size;
  files_.push_back(file_info.filename);
  auto itr = file_infos_->find(file_info.filename);
  if (itr == file_infos_->end()) {
    auto ret = file_infos_->insert({file_info.filename, file_info});
    if (ret.second) {
      ret.first->second.refs = 1;
    } else {
      return Status::Corruption("In memory metadata insertion error");
    }
  } else {
    if (itr->second.checksum_value != file_info.checksum_value) {
      return Status::Corruption("Checksum mismatch for existing backup file");
    }
    ++itr->second.refs;
  }
  return Status::OK();
}
void BackupEngineImpl::BackupMeta::Delete() {
  for (const auto& file : files_) {
    auto itr = file_infos_->find(file);
    assert(itr != file_infos_->end());
    --(itr->second.refs);
  }
  files_.clear();
  env_->DeleteFile(meta_filename_);
  timestamp_ = 0;
}
Status BackupEngineImpl::BackupMeta::LoadFromFile(
    const std::string& backup_dir) {
  assert(Empty());
  Status s;
  unique_ptr<SequentialFile> backup_meta_file;
  s = env_->NewSequentialFile(meta_filename_, &backup_meta_file, EnvOptions());
  if (!s.ok()) {
    return s;
  }
  unique_ptr<char[]> buf(new char[max_backup_meta_file_size_ + 1]);
  Slice data;
  s = backup_meta_file->Read(max_backup_meta_file_size_, &data, buf.get());
  if (!s.ok() || data.size() == max_backup_meta_file_size_) {
    return s.ok() ? Status::Corruption("File size too big") : s;
  }
  buf[data.size()] = 0;
  uint32_t num_files = 0;
  int bytes_read = 0;
  sscanf(data.data(), "%" PRId64 "%n", &timestamp_, &bytes_read);
  data.remove_prefix(bytes_read + 1);
  sscanf(data.data(), "%" PRIu64 "%n", &sequence_number_, &bytes_read);
  data.remove_prefix(bytes_read + 1);
  sscanf(data.data(), "%u%n", &num_files, &bytes_read);
  data.remove_prefix(bytes_read + 1);
  std::vector<FileInfo> files;
  for (uint32_t i = 0; s.ok() && i < num_files; ++i) {
    auto line = GetSliceUntil(&data, '\n');
    std::string filename = GetSliceUntil(&line, ' ').ToString();
    uint64_t size;
    s = env_->GetFileSize(backup_dir + "/" + filename, &size);
    if (!s.ok()) {
      return s;
    }
    if (line.empty()) {
      return Status::Corruption("File checksum is missing");
    }
    uint32_t checksum_value = 0;
    if (line.starts_with("crc32 ")) {
      line.remove_prefix(6);
      sscanf(line.data(), "%u", &checksum_value);
      if (memcmp(line.data(), std::to_string(checksum_value).c_str(),
                 line.size() - 1) != 0) {
        return Status::Corruption("Invalid checksum value");
      }
    } else {
      return Status::Corruption("Unknown checksum type");
    }
    files.emplace_back(filename, size, checksum_value);
  }
  if (s.ok() && data.size() > 0) {
    s = Status::Corruption("Tailing data in backup meta file");
  }
  if (s.ok()) {
    for (const auto& file_info : files) {
      s = AddFile(file_info);
      if (!s.ok()) {
        break;
      }
    }
  }
  return s;
}
Status BackupEngineImpl::BackupMeta::StoreToFile(bool sync) {
  Status s;
  unique_ptr<WritableFile> backup_meta_file;
  EnvOptions env_options;
  env_options.use_mmap_writes = false;
  s = env_->NewWritableFile(meta_filename_ + ".tmp", &backup_meta_file,
                            env_options);
  if (!s.ok()) {
    return s;
  }
  unique_ptr<char[]> buf(new char[max_backup_meta_file_size_]);
  int len = 0, buf_size = max_backup_meta_file_size_;
  len += snprintf(buf.get(), buf_size, "%" PRId64 "\n", timestamp_);
  len += snprintf(buf.get() + len, buf_size - len, "%" PRIu64 "\n",
                  sequence_number_);
  len += snprintf(buf.get() + len, buf_size - len, "%zu\n", files_.size());
  for (const auto& file : files_) {
    const auto& iter = file_infos_->find(file);
    assert(iter != file_infos_->end());
    len += snprintf(buf.get() + len, buf_size - len, "%s crc32 %u\n",
                    file.c_str(), iter->second.checksum_value);
  }
  s = backup_meta_file->Append(Slice(buf.get(), (size_t)len));
  if (s.ok() && sync) {
    s = backup_meta_file->Sync();
  }
  if (s.ok()) {
    s = backup_meta_file->Close();
  }
  if (s.ok()) {
    s = env_->RenameFile(meta_filename_ + ".tmp", meta_filename_);
  }
  return s;
}
BackupableDB::BackupableDB(DB* db, const BackupableDBOptions& options)
    : StackableDB(db),
      backup_engine_(new BackupEngineImpl(db->GetEnv(), options)) {}
BackupableDB::~BackupableDB() {
  delete backup_engine_;
}
Status BackupableDB::CreateNewBackup(bool flush_before_backup) {
  return backup_engine_->CreateNewBackup(this, flush_before_backup);
}
void BackupableDB::GetBackupInfo(std::vector<BackupInfo>* backup_info) {
  backup_engine_->GetBackupInfo(backup_info);
}
Status BackupableDB::PurgeOldBackups(uint32_t num_backups_to_keep) {
  return backup_engine_->PurgeOldBackups(num_backups_to_keep);
}
Status BackupableDB::DeleteBackup(BackupID backup_id) {
  return backup_engine_->DeleteBackup(backup_id);
}
void BackupableDB::StopBackup() {
  backup_engine_->StopBackup();
}
RestoreBackupableDB::RestoreBackupableDB(Env* db_env,
                                         const BackupableDBOptions& options)
    : backup_engine_(new BackupEngineImpl(db_env, options)) {}
RestoreBackupableDB::~RestoreBackupableDB() {
  delete backup_engine_;
}
void
RestoreBackupableDB::GetBackupInfo(std::vector<BackupInfo>* backup_info) {
  backup_engine_->GetBackupInfo(backup_info);
}
Status RestoreBackupableDB::RestoreDBFromBackup(
    BackupID backup_id, const std::string& db_dir, const std::string& wal_dir,
    const RestoreOptions& restore_options) {
  return backup_engine_->RestoreDBFromBackup(backup_id, db_dir, wal_dir,
                                             restore_options);
}
Status RestoreBackupableDB::RestoreDBFromLatestBackup(
    const std::string& db_dir, const std::string& wal_dir,
    const RestoreOptions& restore_options) {
  return backup_engine_->RestoreDBFromLatestBackup(db_dir, wal_dir,
                                                   restore_options);
}
Status RestoreBackupableDB::PurgeOldBackups(uint32_t num_backups_to_keep) {
  return backup_engine_->PurgeOldBackups(num_backups_to_keep);
}
Status RestoreBackupableDB::DeleteBackup(BackupID backup_id) {
  return backup_engine_->DeleteBackup(backup_id);
}
}
