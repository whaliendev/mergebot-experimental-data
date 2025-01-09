       
#include "db/version_set.h"
#include "db/compaction.h"
#include "rocksdb/status.h"
#include "rocksdb/options.h"
#include "rocksdb/env.h"
#include <vector>
#include <memory>
#include <set>
namespace rocksdb {
class LogBuffer;
class Compaction;
class Version;
class CompactionPicker {
 public:
  CompactionPicker(const ColumnFamilyOptions* options,
                   const InternalKeyComparator* icmp, Logger* logger);
  virtual ~CompactionPicker();
  virtual Compaction* PickCompaction(Version* version,
                                     LogBuffer* log_buffer) = 0;
  Compaction* CompactRange(Version* version, int input_level, int output_level,
                           const InternalKey* begin, const InternalKey* end,
                           InternalKey** compaction_end);
  void ReleaseCompactionFiles(Compaction* c, Status status);
  void SizeBeingCompacted(std::vector<uint64_t>& sizes);
  uint64_t MaxGrandParentOverlapBytes(int level);
  double MaxBytesForLevel(int level);
  uint64_t MaxFileSizeForLevel(int level) const;
 protected:
  int NumberLevels() const { return num_levels_; }
  void GetRange(const std::vector<FileMetaData*>& inputs, InternalKey* smallest,
                InternalKey* largest);
  void GetRange(const std::vector<FileMetaData*>& inputs1,
                const std::vector<FileMetaData*>& inputs2,
                InternalKey* smallest, InternalKey* largest);
  bool ExpandWhileOverlapping(Compaction* c);
  uint64_t ExpandedCompactionByteSizeLimit(int level);
  bool FilesInCompaction(std::vector<FileMetaData*>& files);
  bool ParentRangeInCompaction(Version* version, const InternalKey* smallest,
                               const InternalKey* largest, int level,
                               int* index);
  void SetupOtherInputs(Compaction* c);
  std::vector<std::set<Compaction*>> compactions_in_progress_;
  std::unique_ptr<uint64_t[]> max_file_size_;
  std::unique_ptr<uint64_t[]> level_max_bytes_;
  Logger* logger_;
  const ColumnFamilyOptions* const options_;
 private:
  int num_levels_;
  const InternalKeyComparator* const icmp_;
};
class UniversalCompactionPicker : public CompactionPicker {
 public:
 percentalCompactionPicker(const ColumnFamilyOptions* options, const InternalKeyComparator* icmp, Logger* logger) : CompactionPicker(options, icmp, logger) {} virtual Compaction* PickCompaction(Version* version, LogBuffer* log_buffer) override;
 private:
  Compaction* PickCompactionUniversalReadAmp(Version* version, double score,
                                             unsigned int ratio,
                                             unsigned int num_files,
                                             LogBuffer* log_buffer);
  Compaction* PickCompactionUniversalSizeAmp(Version* version, double score,
                                             LogBuffer* log_buffer);
};
class LevelCompactionPicker : public CompactionPicker {
 public:
streamsactionPicker(const ColumnFamilyOptions* options, const InternalKeyComparator* icmp, Logger* logger) : CompactionPicker(options, icmp, logger) {} virtual Compaction* PickCompaction(Version* version, LogBuffer* log_buffer) override;
 private:
  Compaction* PickCompactionBySize(Version* version, int level, double score);
};
}
