#include "db/compaction_picker.h"
#include <limits>
<<<<<<< HEAD
||||||| e5fa4944f
#include "util/statistics.h"
=======
#include "util/log_buffer.h"
#include "util/statistics.h"
>>>>>>> d5de22dc
namespace rocksdb {
namespace {
uint64_t TotalFileSize(const std::vector<FileMetaData*>& files) {
  uint64_t sum = 0;
  for (size_t i = 0; i < files.size() && files[i]; i++) {
    sum += files[i]->file_size;
  }
  return sum;
}
uint64_t MultiplyCheckOverflow(uint64_t op1, int op2) {
  if (op1 == 0) {
    return 0;
  }
  if (op2 <= 0) {
    return op1;
  }
  uint64_t casted_op2 = (uint64_t) op2;
  if (std::numeric_limits<uint64_t>::max() / op1 < casted_op2) {
    return op1;
  }
  return op1 * casted_op2;
}
}
CompactionPicker::CompactionPicker(const ColumnFamilyOptions* options,
                                   const InternalKeyComparator* icmp,
                                   Logger* logger)
    : compactions_in_progress_(options->num_levels),
      logger_(logger),
      options_(options),
      num_levels_(options->num_levels),
      icmp_(icmp) {
  max_file_size_.reset(new uint64_t[NumberLevels()]);
  level_max_bytes_.reset(new uint64_t[NumberLevels()]);
  int target_file_size_multiplier = options_->target_file_size_multiplier;
  int max_bytes_multiplier = options_->max_bytes_for_level_multiplier;
  for (int i = 0; i < NumberLevels(); i++) {
    if (i == 0 && options_->compaction_style == kCompactionStyleUniversal) {
      max_file_size_[i] = ULLONG_MAX;
      level_max_bytes_[i] = options_->max_bytes_for_level_base;
    } else if (i > 1) {
      max_file_size_[i] = MultiplyCheckOverflow(max_file_size_[i - 1],
                                                target_file_size_multiplier);
      level_max_bytes_[i] = MultiplyCheckOverflow(
          MultiplyCheckOverflow(level_max_bytes_[i - 1], max_bytes_multiplier),
          options_->max_bytes_for_level_multiplier_additional[i - 1]);
    } else {
      max_file_size_[i] = options_->target_file_size_base;
      level_max_bytes_[i] = options_->max_bytes_for_level_base;
    }
  }
}
CompactionPicker::~CompactionPicker() {}
void CompactionPicker::SizeBeingCompacted(std::vector<uint64_t>& sizes) {
  for (int level = 0; level < NumberLevels() - 1; level++) {
    uint64_t total = 0;
    for (auto c : compactions_in_progress_[level]) {
      assert(c->level() == level);
      for (int i = 0; i < c->num_input_files(0); i++) {
        total += c->input(0,i)->file_size;
      }
    }
    sizes[level] = total;
  }
}
void CompactionPicker::ReleaseCompactionFiles(Compaction* c, Status status) {
  c->MarkFilesBeingCompacted(false);
  compactions_in_progress_[c->level()].erase(c);
  if (!status.ok()) {
    c->ResetNextCompactionIndex();
  }
}
uint64_t CompactionPicker::MaxFileSizeForLevel(int level) const {
  assert(level >= 0);
  assert(level < NumberLevels());
  return max_file_size_[level];
}
uint64_t CompactionPicker::MaxGrandParentOverlapBytes(int level) {
  uint64_t result = MaxFileSizeForLevel(level);
  result *= options_->max_grandparent_overlap_factor;
  return result;
}
double CompactionPicker::MaxBytesForLevel(int level) {
  assert(level >= 0);
  assert(level < NumberLevels());
  return level_max_bytes_[level];
}
void CompactionPicker::GetRange(const std::vector<FileMetaData*>& inputs,
                                InternalKey* smallest, InternalKey* largest) {
  assert(!inputs.empty());
  smallest->Clear();
  largest->Clear();
  for (size_t i = 0; i < inputs.size(); i++) {
    FileMetaData* f = inputs[i];
    if (i == 0) {
      *smallest = f->smallest;
      *largest = f->largest;
    } else {
      if (icmp_->Compare(f->smallest, *smallest) < 0) {
        *smallest = f->smallest;
      }
      if (icmp_->Compare(f->largest, *largest) > 0) {
        *largest = f->largest;
      }
    }
  }
}
void CompactionPicker::GetRange(const std::vector<FileMetaData*>& inputs1,
                                const std::vector<FileMetaData*>& inputs2,
                                InternalKey* smallest, InternalKey* largest) {
  std::vector<FileMetaData*> all = inputs1;
  all.insert(all.end(), inputs2.begin(), inputs2.end());
  GetRange(all, smallest, largest);
}
bool CompactionPicker::ExpandWhileOverlapping(Compaction* c) {
  if (!c || c->inputs_[0].empty()) {
    return true;
  }
  if (c->level() == 0) {
    return true;
  }
  const int level = c->level();
  InternalKey smallest, largest;
  int hint_index = -1;
  size_t old_size;
  do {
    old_size = c->inputs_[0].size();
    GetRange(c->inputs_[0], &smallest, &largest);
    c->inputs_[0].clear();
    c->input_version_->GetOverlappingInputs(
        level, &smallest, &largest, &c->inputs_[0], hint_index, &hint_index);
  } while(c->inputs_[0].size() > old_size);
  GetRange(c->inputs_[0], &smallest, &largest);
  int parent_index = -1;
  if (FilesInCompaction(c->inputs_[0]) ||
      (c->level() != c->output_level() &&
       ParentRangeInCompaction(c->input_version_, &smallest, &largest, level,
                               &parent_index))) {
    c->inputs_[0].clear();
    c->inputs_[1].clear();
    return false;
  }
  return true;
}
uint64_t CompactionPicker::ExpandedCompactionByteSizeLimit(int level) {
  uint64_t result = MaxFileSizeForLevel(level);
  result *= options_->expanded_compaction_factor;
  return result;
}
bool CompactionPicker::FilesInCompaction(std::vector<FileMetaData*>& files) {
  for (unsigned int i = 0; i < files.size(); i++) {
    if (files[i]->being_compacted) {
      return true;
    }
  }
  return false;
}
bool CompactionPicker::ParentRangeInCompaction(Version* version,
                                               const InternalKey* smallest,
                                               const InternalKey* largest,
                                               int level, int* parent_index) {
  std::vector<FileMetaData*> inputs;
  assert(level + 1 < NumberLevels());
  version->GetOverlappingInputs(level + 1, smallest, largest, &inputs,
                                *parent_index, parent_index);
  return FilesInCompaction(inputs);
}
void CompactionPicker::SetupOtherInputs(Compaction* c) {
  if (c->inputs_[0].empty() || c->level() == c->output_level()) {
    return;
  }
  const int level = c->level();
  InternalKey smallest, largest;
  GetRange(c->inputs_[0], &smallest, &largest);
  c->input_version_->GetOverlappingInputs(level + 1, &smallest, &largest,
                                          &c->inputs_[1], c->parent_index_,
                                          &c->parent_index_);
  InternalKey all_start, all_limit;
  GetRange(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);
  if (!c->inputs_[1].empty()) {
    std::vector<FileMetaData*> expanded0;
    c->input_version_->GetOverlappingInputs(
        level, &all_start, &all_limit, &expanded0, c->base_index_, nullptr);
    const uint64_t inputs0_size = TotalFileSize(c->inputs_[0]);
    const uint64_t inputs1_size = TotalFileSize(c->inputs_[1]);
    const uint64_t expanded0_size = TotalFileSize(expanded0);
    uint64_t limit = ExpandedCompactionByteSizeLimit(level);
    if (expanded0.size() > c->inputs_[0].size() &&
        inputs1_size + expanded0_size < limit &&
        !FilesInCompaction(expanded0) &&
        !c->input_version_->HasOverlappingUserKey(&expanded0, level)) {
      InternalKey new_start, new_limit;
      GetRange(expanded0, &new_start, &new_limit);
      std::vector<FileMetaData*> expanded1;
      c->input_version_->GetOverlappingInputs(level + 1, &new_start, &new_limit,
                                              &expanded1, c->parent_index_,
                                              &c->parent_index_);
      if (expanded1.size() == c->inputs_[1].size() &&
          !FilesInCompaction(expanded1)) {
        Log(logger_,
            "Expanding@%lu %lu+%lu (%lu+%lu bytes) to %lu+%lu (%lu+%lu bytes)"
            "\n",
            (unsigned long)level, (unsigned long)(c->inputs_[0].size()),
            (unsigned long)(c->inputs_[1].size()), (unsigned long)inputs0_size,
            (unsigned long)inputs1_size, (unsigned long)(expanded0.size()),
            (unsigned long)(expanded1.size()), (unsigned long)expanded0_size,
            (unsigned long)inputs1_size);
        smallest = new_start;
        largest = new_limit;
        c->inputs_[0] = expanded0;
        c->inputs_[1] = expanded1;
        GetRange(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);
      }
    }
  }
  if (level + 2 < NumberLevels()) {
    c->input_version_->GetOverlappingInputs(level + 2, &all_start, &all_limit,
                                            &c->grandparents_);
  }
}
Compaction* CompactionPicker::CompactRange(Version* version, int input_level,
                                           int output_level,
                                           const InternalKey* begin,
                                           const InternalKey* end,
                                           InternalKey** compaction_end) {
  std::vector<FileMetaData*> inputs;
  bool covering_the_whole_range = true;
  if (options_->compaction_style == kCompactionStyleUniversal) {
    begin = nullptr;
    end = nullptr;
  }
  version->GetOverlappingInputs(input_level, begin, end, &inputs);
  if (inputs.empty()) {
    return nullptr;
  }
  if (input_level > 0) {
    const uint64_t limit =
        MaxFileSizeForLevel(input_level) * options_->source_compaction_factor;
    uint64_t total = 0;
    for (size_t i = 0; i + 1 < inputs.size(); ++i) {
      uint64_t s = inputs[i]->file_size;
      total += s;
      if (total >= limit) {
        **compaction_end = inputs[i + 1]->smallest;
        covering_the_whole_range = false;
        inputs.resize(i + 1);
        break;
      }
    }
  }
  Compaction* c = new Compaction(version, input_level, output_level,
                                 MaxFileSizeForLevel(output_level),
                                 MaxGrandParentOverlapBytes(input_level));
  c->inputs_[0] = inputs;
  if (ExpandWhileOverlapping(c) == false) {
    delete c;
    Log(logger_, "Could not compact due to expansion failure.\n");
    return nullptr;
  }
  SetupOtherInputs(c);
  if (covering_the_whole_range) {
    *compaction_end = nullptr;
  }
  c->MarkFilesBeingCompacted(true);
  c->SetupBottomMostLevel(true);
  c->is_manual_compaction_ = true;
  return c;
}
Compaction* LevelCompactionPicker::PickCompaction(Version* version,
                                                  LogBuffer* log_buffer) {
  Compaction* c = nullptr;
  int level = -1;
  std::vector<uint64_t> size_being_compacted(NumberLevels() - 1);
  SizeBeingCompacted(size_being_compacted);
  version->Finalize(size_being_compacted);
  for (int i = 0; i < NumberLevels() - 1; i++) {
    assert(i == 0 ||
           version->compaction_score_[i] <= version->compaction_score_[i - 1]);
    level = version->compaction_level_[i];
    if ((version->compaction_score_[i] >= 1)) {
      c = PickCompactionBySize(version, level, version->compaction_score_[i]);
      if (ExpandWhileOverlapping(c) == false) {
        delete c;
        c = nullptr;
      } else {
        break;
      }
    }
  }
  FileMetaData* f = version->file_to_compact_;
  if (c == nullptr && f != nullptr && !f->being_compacted) {
    level = version->file_to_compact_level_;
    int parent_index = -1;
    if (level != 0 || compactions_in_progress_[0].empty()) {
      if (!ParentRangeInCompaction(version, &f->smallest, &f->largest, level,
                                   &parent_index)) {
        c = new Compaction(version, level, level + 1,
                           MaxFileSizeForLevel(level + 1),
                           MaxGrandParentOverlapBytes(level), true);
        c->inputs_[0].push_back(f);
        c->parent_index_ = parent_index;
        c->input_version_->file_to_compact_ = nullptr;
        if (ExpandWhileOverlapping(c) == false) {
          return nullptr;
        }
      }
    }
  }
  if (c == nullptr) {
    return nullptr;
  }
  if (level == 0) {
    assert(compactions_in_progress_[0].empty());
    InternalKey smallest, largest;
    GetRange(c->inputs_[0], &smallest, &largest);
    c->inputs_[0].clear();
    c->input_version_->GetOverlappingInputs(0, &smallest, &largest,
                                            &c->inputs_[0]);
    GetRange(c->inputs_[0], &smallest, &largest);
    if (ParentRangeInCompaction(c->input_version_, &smallest, &largest, level,
                                &c->parent_index_)) {
      delete c;
      return nullptr;
    }
    assert(!c->inputs_[0].empty());
  }
  SetupOtherInputs(c);
  c->MarkFilesBeingCompacted(true);
  c->SetupBottomMostLevel(false);
  compactions_in_progress_[level].insert(c);
  return c;
}
Compaction* LevelCompactionPicker::PickCompactionBySize(Version* version,
                                                        int level,
                                                        double score) {
  Compaction* c = nullptr;
  if (level == 0 && compactions_in_progress_[level].size() == 1) {
    return nullptr;
  }
  assert(level >= 0);
  assert(level + 1 < NumberLevels());
  c = new Compaction(version, level, level + 1, MaxFileSizeForLevel(level + 1),
                     MaxGrandParentOverlapBytes(level));
  c->score_ = score;
  std::vector<int>& file_size = c->input_version_->files_by_size_[level];
  int nextIndex = -1;
  for (unsigned int i = c->input_version_->next_file_to_compact_by_size_[level];
       i < file_size.size(); i++) {
    int index = file_size[i];
    FileMetaData* f = c->input_version_->files_[level][index];
    assert((i == file_size.size() - 1) ||
           (i >= Version::number_of_files_to_sort_ - 1) ||
           (f->file_size >=
            c->input_version_->files_[level][file_size[i + 1]]->file_size));
    if (f->being_compacted) {
      continue;
    }
    if (nextIndex == -1) {
      nextIndex = i;
    }
    int parent_index = -1;
    if (ParentRangeInCompaction(c->input_version_, &f->smallest, &f->largest,
                                level, &parent_index)) {
      continue;
    }
    c->inputs_[0].push_back(f);
    c->base_index_ = index;
    c->parent_index_ = parent_index;
    break;
  }
  if (c->inputs_[0].empty()) {
    delete c;
    c = nullptr;
  }
  version->next_file_to_compact_by_size_[level] = nextIndex;
  return c;
}
Compaction* UniversalCompactionPicker::PickCompaction(Version* version,
                                                      LogBuffer* log_buffer) {
  int level = 0;
  double score = version->compaction_score_[0];
  if ((version->files_[level].size() <
       (unsigned int)options_->level0_file_num_compaction_trigger)) {
    LogToBuffer(log_buffer, "Universal: nothing to do\n");
    return nullptr;
  }
  Version::FileSummaryStorage tmp;
  LogToBuffer(log_buffer, "Universal: candidate files(%zu): %s\n",
              version->files_[level].size(),
              version->LevelFileSummary(&tmp, 0));
  Compaction* c;
  if ((c = PickCompactionUniversalSizeAmp(version, score, log_buffer)) !=
      nullptr) {
    LogToBuffer(log_buffer, "Universal: compacting for size amp\n");
  } else {
    unsigned int ratio = options_->compaction_options_universal.size_ratio;
    if ((c = PickCompactionUniversalReadAmp(version, score, ratio, UINT_MAX,
                                            log_buffer)) != nullptr) {
      LogToBuffer(log_buffer, "Universal: compacting for size ratio\n");
    } else {
      unsigned int num_files = version->files_[level].size() -
                               options_->level0_file_num_compaction_trigger;
      if ((c = PickCompactionUniversalReadAmp(
               version, score, UINT_MAX, num_files, log_buffer)) != nullptr) {
        LogToBuffer(log_buffer, "Universal: compacting for file num\n");
      }
    }
  }
  if (c == nullptr) {
    return nullptr;
  }
  assert(c->inputs_[0].size() > 1);
  FileMetaData* newerfile __attribute__((unused)) = nullptr;
  for (unsigned int i = 0; i < c->inputs_[0].size(); i++) {
    FileMetaData* f = c->inputs_[0][i];
    assert (f->smallest_seqno <= f->largest_seqno);
    assert(newerfile == nullptr ||
           newerfile->smallest_seqno > f->largest_seqno);
    newerfile = f;
  }
  std::vector<int>& file_by_time = c->input_version_->files_by_size_[level];
  int last_index = file_by_time[file_by_time.size()-1];
  FileMetaData* last_file = c->input_version_->files_[level][last_index];
  if (c->inputs_[0][c->inputs_[0].size()-1] == last_file) {
    c->bottommost_level_ = true;
  }
  c->MarkFilesBeingCompacted(true);
  compactions_in_progress_[level].insert(c);
  c->is_full_compaction_ =
      (c->inputs_[0].size() == c->input_version_->files_[0].size());
  return c;
}
Compaction* UniversalCompactionPicker::PickCompactionUniversalReadAmp(
    Version* version, double score, unsigned int ratio,
    unsigned int max_number_of_files_to_compact, LogBuffer* log_buffer) {
  int level = 0;
  unsigned int min_merge_width =
    options_->compaction_options_universal.min_merge_width;
  unsigned int max_merge_width =
    options_->compaction_options_universal.max_merge_width;
  std::vector<int>& file_by_time = version->files_by_size_[level];
  FileMetaData* f = nullptr;
  bool done = false;
  int start_index = 0;
  unsigned int candidate_count;
  assert(file_by_time.size() == version->files_[level].size());
  unsigned int max_files_to_compact = std::min(max_merge_width,
                                       max_number_of_files_to_compact);
  min_merge_width = std::max(min_merge_width, 2U);
  for (unsigned int loop = 0; loop < file_by_time.size(); loop++) {
    candidate_count = 0;
    for (f = nullptr; loop < file_by_time.size(); loop++) {
      int index = file_by_time[loop];
      f = version->files_[level][index];
      if (!f->being_compacted) {
        candidate_count = 1;
        break;
      }
      LogToBuffer(log_buffer,
                  "Universal: file %lu[%d] being compacted, skipping",
                  (unsigned long)f->number, loop);
      f = nullptr;
    }
    uint64_t candidate_size = f != nullptr? f->file_size : 0;
    if (f != nullptr) {
      LogToBuffer(log_buffer, "Universal: Possible candidate file %lu[%d].",
                  (unsigned long)f->number, loop);
    }
    for (unsigned int i = loop+1;
         candidate_count < max_files_to_compact && i < file_by_time.size();
         i++) {
      int index = file_by_time[i];
      FileMetaData* f = version->files_[level][index];
      if (f->being_compacted) {
        break;
      }
      uint64_t sz = (candidate_size * (100L + ratio)) /100;
      if (sz < f->file_size) {
        break;
      }
      if (options_->compaction_options_universal.stop_style == kCompactionStopStyleSimilarSize) {
        sz = (f->file_size * (100L + ratio)) / 100;
        if (sz < candidate_size) {
          break;
        }
        candidate_size = f->file_size;
      } else {
        candidate_size += f->file_size;
      }
      candidate_count++;
    }
    if (candidate_count >= (unsigned int)min_merge_width) {
      start_index = loop;
      done = true;
      break;
    } else {
      for (unsigned int i = loop;
           i < loop + candidate_count && i < file_by_time.size(); i++) {
       int index = file_by_time[i];
       FileMetaData* f = version->files_[level][index];
       LogToBuffer(log_buffer,
                   "Universal: Skipping file %lu[%d] with size %lu %d\n",
                   (unsigned long)f->number, i, (unsigned long)f->file_size,
                   f->being_compacted);
      }
    }
  }
  if (!done || candidate_count <= 1) {
    return nullptr;
  }
  unsigned int first_index_after = start_index + candidate_count;
  bool enable_compression = true;
  int ratio_to_compress =
      options_->compaction_options_universal.compression_size_percent;
  if (ratio_to_compress >= 0) {
    uint64_t total_size = version->NumLevelBytes(level);
    uint64_t older_file_size = 0;
    for (unsigned int i = file_by_time.size() - 1; i >= first_index_after;
        i--) {
      older_file_size += version->files_[level][file_by_time[i]]->file_size;
      if (older_file_size * 100L >= total_size * (long) ratio_to_compress) {
        enable_compression = false;
        break;
      }
    }
  }
  Compaction* c =
      new Compaction(version, level, level, MaxFileSizeForLevel(level),
                     LLONG_MAX, false, enable_compression);
  c->score_ = score;
  for (unsigned int i = start_index; i < first_index_after; i++) {
    int index = file_by_time[i];
    FileMetaData* f = c->input_version_->files_[level][index];
    c->inputs_[0].push_back(f);
    LogToBuffer(log_buffer, "Universal: Picking file %lu[%d] with size %lu\n",
                (unsigned long)f->number, i, (unsigned long)f->file_size);
  }
  return c;
}
Compaction* UniversalCompactionPicker::PickCompactionUniversalSizeAmp(
    Version* version, double score, LogBuffer* log_buffer) {
  int level = 0;
  uint64_t ratio = options_->compaction_options_universal.
                     max_size_amplification_percent;
  std::vector<int>& file_by_time = version->files_by_size_[level];
  assert(file_by_time.size() == version->files_[level].size());
  unsigned int candidate_count = 0;
  uint64_t candidate_size = 0;
  unsigned int start_index = 0;
  FileMetaData* f = nullptr;
  for (unsigned int loop = 0; loop < file_by_time.size() - 1; loop++) {
    int index = file_by_time[loop];
    f = version->files_[level][index];
    if (!f->being_compacted) {
      start_index = loop;
      break;
    }
    LogToBuffer(log_buffer, "Universal: skipping file %lu[%d] compacted %s",
                (unsigned long)f->number, loop,
                " cannot be a candidate to reduce size amp.\n");
    f = nullptr;
  }
  if (f == nullptr) {
    return nullptr;
  }
  LogToBuffer(log_buffer, "Universal: First candidate file %lu[%d] %s",
              (unsigned long)f->number, start_index, " to reduce size amp.\n");
  for (unsigned int loop = start_index; loop < file_by_time.size() - 1;
       loop++) {
    int index = file_by_time[loop];
    f = version->files_[level][index];
    if (f->being_compacted) {
      LogToBuffer(
          log_buffer, "Universal: Possible candidate file %lu[%d] %s.",
          (unsigned long)f->number, loop,
          " is already being compacted. No size amp reduction possible.\n");
      return nullptr;
    }
    candidate_size += f->file_size;
    candidate_count++;
  }
  if (candidate_count == 0) {
    return nullptr;
  }
  int index = file_by_time[file_by_time.size() - 1];
  uint64_t earliest_file_size = version->files_[level][index]->file_size;
  if (candidate_size * 100 < ratio * earliest_file_size) {
    LogToBuffer(log_buffer,
                "Universal: size amp not needed. newer-files-total-size %lu "
                "earliest-file-size %lu",
                (unsigned long)candidate_size,
                (unsigned long)earliest_file_size);
    return nullptr;
  } else {
    LogToBuffer(log_buffer,
                "Universal: size amp needed. newer-files-total-size %lu "
                "earliest-file-size %lu",
                (unsigned long)candidate_size,
                (unsigned long)earliest_file_size);
  }
  assert(start_index >= 0 && start_index < file_by_time.size() - 1);
  Compaction* c =
      new Compaction(version, level, level, MaxFileSizeForLevel(level),
                     LLONG_MAX, false, true);
  c->score_ = score;
  for (unsigned int loop = start_index; loop < file_by_time.size(); loop++) {
    int index = file_by_time[loop];
    f = c->input_version_->files_[level][index];
    c->inputs_[0].push_back(f);
    LogToBuffer(log_buffer,
                "Universal: size amp picking file %lu[%d] with size %lu",
                (unsigned long)f->number, index, (unsigned long)f->file_size);
  }
  return c;
}
}
