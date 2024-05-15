#include "db/tailing_iter.h"
#include <string>
#include <utility>
#include "db/db_impl.h"
#include "db/column_family.h"
#include "rocksdb/slice.h"
#include "rocksdb/slice_transform.h"
namespace rocksdb {
CompactionPicker::~CompactionPicker() {}
Compaction* UniversalCompactionPicker::PickCompactionUniversalSizeAmp(
    Version* version, double score, LogBuffer* log_buffer) {
  int level = 0;
  uint64_t ratio =
      options_->compaction_options_universal.max_size_amplification_percent;
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
Compaction* UniversalCompactionPicker::PickCompactionUniversalSizeAmp(
    Version* version, double score, LogBuffer* log_buffer) {
  int level = 0;
  uint64_t ratio =
      options_->compaction_options_universal.max_size_amplification_percent;
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
Compaction* UniversalCompactionPicker::PickCompactionUniversalSizeAmp(
    Version* version, double score, LogBuffer* log_buffer) {
  int level = 0;
  uint64_t ratio =
      options_->compaction_options_universal.max_size_amplification_percent;
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
Compaction* UniversalCompactionPicker::PickCompactionUniversalSizeAmp(
    Version* version, double score, LogBuffer* log_buffer) {
  int level = 0;
  uint64_t ratio =
      options_->compaction_options_universal.max_size_amplification_percent;
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
Compaction* UniversalCompactionPicker::PickCompactionUniversalSizeAmp(
    Version* version, double score, LogBuffer* log_buffer) {
  int level = 0;
  uint64_t ratio =
      options_->compaction_options_universal.max_size_amplification_percent;
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
Compaction* UniversalCompactionPicker::PickCompactionUniversalSizeAmp(
    Version* version, double score, LogBuffer* log_buffer) {
  int level = 0;
  uint64_t ratio =
      options_->compaction_options_universal.max_size_amplification_percent;
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
Compaction* UniversalCompactionPicker::PickCompactionUniversalSizeAmp(
    Version* version, double score, LogBuffer* log_buffer) {
  int level = 0;
  uint64_t ratio =
      options_->compaction_options_universal.max_size_amplification_percent;
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
Compaction* UniversalCompactionPicker::PickCompactionUniversalSizeAmp(
    Version* version, double score, LogBuffer* log_buffer) {
  int level = 0;
  uint64_t ratio =
      options_->compaction_options_universal.max_size_amplification_percent;
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
Compaction* UniversalCompactionPicker::PickCompactionUniversalSizeAmp(
    Version* version, double score, LogBuffer* log_buffer) {
  int level = 0;
  uint64_t ratio =
      options_->compaction_options_universal.max_size_amplification_percent;
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
Compaction* UniversalCompactionPicker::PickCompactionUniversalSizeAmp(
    Version* version, double score, LogBuffer* log_buffer) {
  int level = 0;
  uint64_t ratio =
      options_->compaction_options_universal.max_size_amplification_percent;
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
Compaction* UniversalCompactionPicker::PickCompactionUniversalSizeAmp(
    Version* version, double score, LogBuffer* log_buffer) {
  int level = 0;
  uint64_t ratio =
      options_->compaction_options_universal.max_size_amplification_percent;
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
Compaction* UniversalCompactionPicker::PickCompactionUniversalSizeAmp(
    Version* version, double score, LogBuffer* log_buffer) {
  int level = 0;
  uint64_t ratio =
      options_->compaction_options_universal.max_size_amplification_percent;
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
Compaction* UniversalCompactionPicker::PickCompactionUniversalSizeAmp(
    Version* version, double score, LogBuffer* log_buffer) {
  int level = 0;
  uint64_t ratio =
      options_->compaction_options_universal.max_size_amplification_percent;
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
Compaction* UniversalCompactionPicker::PickCompactionUniversalSizeAmp(
    Version* version, double score, LogBuffer* log_buffer) {
  int level = 0;
  uint64_t ratio =
      options_->compaction_options_universal.max_size_amplification_percent;
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
