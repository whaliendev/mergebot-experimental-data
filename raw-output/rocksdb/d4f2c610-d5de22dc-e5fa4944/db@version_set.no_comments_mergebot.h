       
#include <map>
#include <memory>
#include <set>
#include <vector>
#include <deque>
#include <atomic>
#include <limits>
#include "db/dbformat.h"
#include "db/version_edit.h"
#include "port/port.h"
#include "db/table_cache.h"
#include "db/compaction.h"
#include "db/compaction_picker.h"
#include "db/column_family.h"
#include "db/log_reader.h"
namespace rocksdb {
namespace log { class Writer;}
class Compaction;
class CompactionPicker;
class Iterator;
class LogBuffer;
class LookupKey;
class MemTable;
class MergeContext;
class ColumnFamilyData;
class ColumnFamilySet;
class TableCache;
class Version;
class VersionSet;
extern int FindFile(const InternalKeyComparator& icmp,
                    const std::vector<FileMetaData*>& files,
                    const Slice& key);
extern bool SomeFileOverlapsRange(
    const InternalKeyComparator& icmp,
    bool disjoint_sorted_files,
    const std::vector<FileMetaData*>& files,
    const Slice* smallest_user_key,
    const Slice* largest_user_key);
class Version {
public:
  void AddIterators(const ReadOptions&, const EnvOptions& soptions,
                    std::vector<Iterator*>* iters);
  struct GetStats {
    FileMetaData* seek_file;
    int seek_file_level;
  };
  void Get(const ReadOptions&, const LookupKey& key, std::string* val,
           Status* status, MergeContext* merge_context,
           GetStats* stats, const Options& db_option,
           bool* value_found = nullptr);
  bool UpdateStats(const GetStats& stats);
  void Finalize(std::vector<uint64_t>& size_being_compacted);
  void Ref();
  bool Unref();
  bool NeedsCompaction() const;
  double MaxCompactionScore() const { return max_compaction_score_; }
  int MaxCompactionScoreLevel() const { return max_compaction_score_level_; }
void GetOverlappingInputs(
      int level,
      const InternalKey* begin,
      const InternalKey* end,
      std::vector<FileMetaData*>* inputs,
      int hint_index = -1,
      int* file_index = nullptr);
void GetOverlappingInputsBinarySearch(
      int level,
      const Slice& begin,
      const Slice& end,
      std::vector<FileMetaData*>* inputs,
      int hint_index,
      int* file_index);
void ExtendOverlappingInputs(
      int level,
      const Slice& begin,
      const Slice& end,
      std::vector<FileMetaData*>* inputs,
      unsigned int index);
  bool OverlapInLevel(int level,
                      const Slice* smallest_user_key,
                      const Slice* largest_user_key);
  bool HasOverlappingUserKey(const std::vector<FileMetaData*>* inputs,
                             int level);
  int PickLevelForMemTableOutput(const Slice& smallest_user_key,
                                 const Slice& largest_user_key);
  int NumberLevels() const { return num_levels_; }
  int NumLevelFiles(int level) const { return files_[level].size(); }
  int64_t NumLevelBytes(int level) const;
  struct LevelSummaryStorage {
    char buffer[100];
  };
  struct FileSummaryStorage {
    char buffer[1000];
  };
  const char* LevelSummary(LevelSummaryStorage* scratch) const;
  const char* LevelFileSummary(FileSummaryStorage* scratch, int level) const;
  int64_t MaxNextLevelOverlappingBytes();
  void AddLiveFiles(std::set<uint64_t>* live);
  std::string DebugString(bool hex = false) const;
  uint64_t GetVersionNumber() const { return version_number_; }
  Status GetPropertiesOfAllTables(TablePropertiesCollection* props);
  struct Fsize {
    int index;
    FileMetaData* file;
  };
private:
  friend class Compaction;
  friend class VersionSet;
  friend class DBImpl;
  friend class ColumnFamilyData;
  friend class CompactionPicker;
  friend class LevelCompactionPicker;
  friend class UniversalCompactionPicker;
  class LevelFileNumIterator;
  Iterator* NewConcatenatingIterator(const ReadOptions&,
                                     const EnvOptions& soptions,
                                     int level) const;
  bool PrefixMayMatch(const ReadOptions& options, const EnvOptions& soptions,
                      const Slice& internal_prefix, Iterator* level_iter) const;
  void UpdateFilesBySize();
ColumnFamilyData* cfd_;
VersionSet* vset_;
Version* next_;
Version* prev_;
int refs_;
int num_levels_;
  std::vector<FileMetaData*>* files_;
  std::vector<std::vector<int>> files_by_size_;
  std::vector<int> next_file_to_compact_by_size_;
  static const int number_of_files_to_sort_ = 50;
  FileMetaData* file_to_compact_;
  int file_to_compact_level_;
  std::vector<double> compaction_score_;
  std::vector<int> compaction_level_;
double max_compaction_score_;
int max_compaction_score_level_;
  uint64_t version_number_;
  Version(ColumnFamilyData* cfd, VersionSet* vset, uint64_t version_number = 0);
  ~Version();
  void ResetNextCompactionIndex(int level) {
    next_file_to_compact_by_size_[level] = 0;
  }
  Version(const Version&);
  void operator=(const Version&);
};
class VersionSet {
public:
  VersionSet(const std::string& dbname, const DBOptions* options,
             const EnvOptions& storage_options, Cache* table_cache);
  ~VersionSet();
  Status LogAndApply(ColumnFamilyData* column_family_data, VersionEdit* edit,
                     port::Mutex* mu, Directory* db_directory = nullptr,
                     bool new_descriptor_log = false,
                     const ColumnFamilyOptions* column_family_options =
                         nullptr);
  static Status ReduceNumberOfLevels(const std::string& dbname,
                                     const Options* options,
                                     const EnvOptions& storage_options,
                                     int new_levels);
  uint64_t ManifestFileNumber() const { return manifest_file_number_; }
  uint64_t NewFileNumber() { return next_file_number_++; }
  void ReuseFileNumber(uint64_t file_number) {
    if (next_file_number_ == file_number + 1) {
      next_file_number_ = file_number;
    }
  }
  uint64_t LastSequence() const {
    return last_sequence_.load(std::memory_order_acquire);
  }
  void SetLastSequence(uint64_t s) {
    assert(s >= last_sequence_);
    last_sequence_.store(s, std::memory_order_release);
  }
  void MarkFileNumberUsed(uint64_t number);
  uint64_t PrevLogNumber() const { return prev_log_number_; }
  uint64_t MinLogNumber() const {
    uint64_t min_log_num = std::numeric_limits<uint64_t>::max();
    for (auto cfd : *column_family_set_) {
      if (min_log_num > cfd->GetLogNumber()) {
        min_log_num = cfd->GetLogNumber();
      }
    }
    return min_log_num;
  }
  Iterator* MakeInputIterator(Compaction* c);
  void AddLiveFiles(std::vector<uint64_t>* live_list);
  uint64_t ApproximateOffsetOf(Version* v, const InternalKey& key);
  Status DumpManifest(Options& options, std::string& manifestFileName,
                      bool verbose, bool hex = false);
  uint64_t ManifestFileSize() const { return manifest_file_size_; }
  bool VerifyCompactionFileConsistency(Compaction* c);
  Status GetMetadataForFile(uint64_t number, int* filelevel,
                            FileMetaData** metadata, ColumnFamilyData** cfd);
  void GetLiveFilesMetaData(
    std::vector<LiveFileMetaData> *metadata);
  void GetObsoleteFiles(std::vector<FileMetaData*>* files);
  ColumnFamilySet* GetColumnFamilySet() { return column_family_set_.get(); }
private:
  class Builder;
  struct ManifestWriter;
  friend class Compaction;
  friend class Version;
  struct LogReporter : public log::Reader::Reporter {
    Status* status;
    virtual void Corruption(size_t bytes, const Status& s) {
      if (this->status->ok()) *this->status = s;
    }
  };
  Status WriteSnapshot(log::Writer* log);
  void AppendVersion(ColumnFamilyData* column_family_data, Version* v);
  bool ManifestContains(const std::string& record) const;
  ColumnFamilyData* CreateColumnFamily(const ColumnFamilyOptions& options,
                                       VersionEdit* edit);
  std::unique_ptr<ColumnFamilySet> column_family_set_;
  Env* const env_;
  const std::string dbname_;
  const DBOptions* const options_;
  uint64_t next_file_number_;
  uint64_t manifest_file_number_;
  std::atomic<uint64_t> last_sequence_;
uint64_t prev_log_number_;
  unique_ptr<log::Writer> descriptor_log_;
  uint64_t current_version_number_;
  std::deque<ManifestWriter*> manifest_writers_;
  uint64_t manifest_file_size_;
  std::vector<FileMetaData*> obsolete_files_;
  const EnvOptions& storage_options_;
  const EnvOptions storage_options_compactions_;
  VersionSet(const VersionSet&);
  void operator=(const VersionSet&);
  void LogAndApplyHelper(ColumnFamilyData* cfd, Builder* b, Version* v,
                         VersionEdit* edit, port::Mutex* mu);
public:
  Status Recover(const std::vector<ColumnFamilyDescriptor>& column_families);
  static Status ListColumnFamilies(std::vector<std::string>* column_families,
                                   const std::string& dbname, Env* env);
};
}
