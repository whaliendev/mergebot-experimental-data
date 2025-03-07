#include "db/version_edit.h"
#include "db/version_set.h"
#include "util/coding.h"
#include "rocksdb/slice.h"
namespace rocksdb {
enum Tag {
  kComparator = 1,
  kLogNumber = 2,
  kNextFileNumber = 3,
  kLastSequence = 4,
  kCompactPointer = 5,
  kDeletedFile = 6,
  kNewFile = 7,
  kPrevLogNumber = 9,
<<<<<<< HEAD
  kNewFile2 = 100,
  kColumnFamily = 200,
  kColumnFamilyAdd = 201,
  kColumnFamilyDrop = 202,
  kMaxColumnFamily = 203,
||||||| 5601bc461
  kNewFile2 = 100
=======
  kNewFile2 = 100,
  kVersionNumber = 101,
>>>>>>> 63cef900
};
void VersionEdit::Clear() {
  version_number_ = 0;
  comparator_.clear();
  max_level_ = 0;
  log_number_ = 0;
  prev_log_number_ = 0;
  last_sequence_ = 0;
  next_file_number_ = 0;
<<<<<<< HEAD
  max_column_family_ = 0;
||||||| 5601bc461
=======
  has_version_number_ = false;
>>>>>>> 63cef900
  has_comparator_ = false;
  has_log_number_ = false;
  has_prev_log_number_ = false;
  has_next_file_number_ = false;
  has_last_sequence_ = false;
  has_max_column_family_ = false;
  deleted_files_.clear();
  new_files_.clear();
  column_family_ = 0;
  is_column_family_add_ = 0;
  is_column_family_drop_ = 0;
  column_family_name_.clear();
}
void VersionEdit::EncodeTo(std::string* dst) const {
  if (has_version_number_) {
    PutVarint32(dst, kVersionNumber);
    PutVarint32(dst, version_number_);
  }
  if (has_comparator_) {
    PutVarint32(dst, kComparator);
    PutLengthPrefixedSlice(dst, comparator_);
  }
  if (has_log_number_) {
    PutVarint32(dst, kLogNumber);
    PutVarint64(dst, log_number_);
  }
  if (has_prev_log_number_) {
    PutVarint32(dst, kPrevLogNumber);
    PutVarint64(dst, prev_log_number_);
  }
  if (has_next_file_number_) {
    PutVarint32(dst, kNextFileNumber);
    PutVarint64(dst, next_file_number_);
  }
  if (has_last_sequence_) {
    PutVarint32(dst, kLastSequence);
    PutVarint64(dst, last_sequence_);
  }
  if (has_max_column_family_) {
    PutVarint32(dst, kMaxColumnFamily);
    PutVarint32(dst, max_column_family_);
  }
  for (const auto& deleted : deleted_files_) {
    PutVarint32(dst, kDeletedFile);
    PutVarint32(dst, deleted.first );
    PutVarint64(dst, deleted.second );
  }
  for (size_t i = 0; i < new_files_.size(); i++) {
    const FileMetaData& f = new_files_[i].second;
    PutVarint32(dst, kNewFile2);
    PutVarint32(dst, new_files_[i].first);
    PutVarint64(dst, f.number);
    PutVarint64(dst, f.file_size);
    PutLengthPrefixedSlice(dst, f.smallest.Encode());
    PutLengthPrefixedSlice(dst, f.largest.Encode());
    PutVarint64(dst, f.smallest_seqno);
    PutVarint64(dst, f.largest_seqno);
  }
  if (column_family_ != 0) {
    PutVarint32(dst, kColumnFamily);
    PutVarint32(dst, column_family_);
  }
  if (is_column_family_add_) {
    PutVarint32(dst, kColumnFamilyAdd);
    PutLengthPrefixedSlice(dst, Slice(column_family_name_));
  }
  if (is_column_family_drop_) {
    PutVarint32(dst, kColumnFamilyDrop);
  }
}
static bool GetInternalKey(Slice* input, InternalKey* dst) {
  Slice str;
  if (GetLengthPrefixedSlice(input, &str)) {
    dst->DecodeFrom(str);
    return true;
  } else {
    return false;
  }
}
bool VersionEdit::GetLevel(Slice* input, int* level, const char** msg) {
  uint32_t v;
  if (GetVarint32(input, &v)) {
    *level = v;
    if (max_level_ < *level) {
      max_level_ = *level;
    }
    return true;
  } else {
    return false;
  }
}
Status VersionEdit::DecodeFrom(const Slice& src) {
  Clear();
  Slice input = src;
  const char* msg = nullptr;
  uint32_t tag;
  int level;
  uint64_t number;
  FileMetaData f;
  Slice str;
  InternalKey key;
  while (msg == nullptr && GetVarint32(&input, &tag)) {
    switch (tag) {
      case kVersionNumber:
        if (GetVarint32(&input, &version_number_)) {
          has_version_number_ = true;
        } else {
          msg = "version number";
        }
        break;
      case kComparator:
        if (GetLengthPrefixedSlice(&input, &str)) {
          comparator_ = str.ToString();
          has_comparator_ = true;
        } else {
          msg = "comparator name";
        }
        break;
      case kLogNumber:
        if (GetVarint64(&input, &log_number_)) {
          has_log_number_ = true;
        } else {
          msg = "log number";
        }
        break;
      case kPrevLogNumber:
        if (GetVarint64(&input, &prev_log_number_)) {
          has_prev_log_number_ = true;
        } else {
          msg = "previous log number";
        }
        break;
      case kNextFileNumber:
        if (GetVarint64(&input, &next_file_number_)) {
          has_next_file_number_ = true;
        } else {
          msg = "next file number";
        }
        break;
      case kLastSequence:
        if (GetVarint64(&input, &last_sequence_)) {
          has_last_sequence_ = true;
        } else {
          msg = "last sequence number";
        }
        break;
      case kMaxColumnFamily:
        if (GetVarint32(&input, &max_column_family_)) {
          has_max_column_family_ = true;
        } else {
          msg = "max column family";
        }
        break;
      case kCompactPointer:
        if (GetLevel(&input, &level, &msg) &&
            GetInternalKey(&input, &key)) {
        } else {
          if (!msg) {
            msg = "compaction pointer";
          }
        }
        break;
      case kDeletedFile:
        if (GetLevel(&input, &level, &msg) &&
            GetVarint64(&input, &number)) {
          deleted_files_.insert(std::make_pair(level, number));
        } else {
          if (!msg) {
            msg = "deleted file";
          }
        }
        break;
      case kNewFile:
        if (GetLevel(&input, &level, &msg) &&
            GetVarint64(&input, &f.number) &&
            GetVarint64(&input, &f.file_size) &&
            GetInternalKey(&input, &f.smallest) &&
            GetInternalKey(&input, &f.largest)) {
          new_files_.push_back(std::make_pair(level, f));
        } else {
          if (!msg) {
            msg = "new-file entry";
          }
        }
        break;
      case kNewFile2:
        if (GetLevel(&input, &level, &msg) &&
            GetVarint64(&input, &f.number) &&
            GetVarint64(&input, &f.file_size) &&
            GetInternalKey(&input, &f.smallest) &&
            GetInternalKey(&input, &f.largest) &&
            GetVarint64(&input, &f.smallest_seqno) &&
            GetVarint64(&input, &f.largest_seqno) ) {
          new_files_.push_back(std::make_pair(level, f));
        } else {
          if (!msg) {
            msg = "new-file2 entry";
          }
        }
        break;
      case kColumnFamily:
        if (!GetVarint32(&input, &column_family_)) {
          if (!msg) {
            msg = "set column family id";
          }
        }
        break;
      case kColumnFamilyAdd:
        if (GetLengthPrefixedSlice(&input, &str)) {
          is_column_family_add_ = true;
          column_family_name_ = str.ToString();
        } else {
          if (!msg) {
            msg = "column family add";
          }
        }
        break;
      case kColumnFamilyDrop:
        is_column_family_drop_ = true;
        break;
      default:
        msg = "unknown tag";
        break;
    }
  }
  if (msg == nullptr && !input.empty()) {
    msg = "invalid tag";
  }
  Status result;
  if (msg != nullptr) {
    result = Status::Corruption("VersionEdit", msg);
  }
  return result;
}
std::string VersionEdit::DebugString(bool hex_key) const {
  std::string r;
  r.append("VersionEdit {");
  if (has_comparator_) {
    r.append("\n  Comparator: ");
    r.append(comparator_);
  }
  if (has_log_number_) {
    r.append("\n  LogNumber: ");
    AppendNumberTo(&r, log_number_);
  }
  if (has_prev_log_number_) {
    r.append("\n  PrevLogNumber: ");
    AppendNumberTo(&r, prev_log_number_);
  }
  if (has_next_file_number_) {
    r.append("\n  NextFile: ");
    AppendNumberTo(&r, next_file_number_);
  }
  if (has_last_sequence_) {
    r.append("\n  LastSeq: ");
    AppendNumberTo(&r, last_sequence_);
  }
  for (DeletedFileSet::const_iterator iter = deleted_files_.begin();
       iter != deleted_files_.end();
       ++iter) {
    r.append("\n  DeleteFile: ");
    AppendNumberTo(&r, iter->first);
    r.append(" ");
    AppendNumberTo(&r, iter->second);
  }
  for (size_t i = 0; i < new_files_.size(); i++) {
    const FileMetaData& f = new_files_[i].second;
    r.append("\n  AddFile: ");
    AppendNumberTo(&r, new_files_[i].first);
    r.append(" ");
    AppendNumberTo(&r, f.number);
    r.append(" ");
    AppendNumberTo(&r, f.file_size);
    r.append(" ");
    r.append(f.smallest.DebugString(hex_key));
    r.append(" .. ");
    r.append(f.largest.DebugString(hex_key));
  }
  r.append("\n  ColumnFamily: ");
  AppendNumberTo(&r, column_family_);
  if (is_column_family_add_) {
    r.append("\n  ColumnFamilyAdd: ");
    r.append(column_family_name_);
  }
  if (is_column_family_drop_) {
    r.append("\n  ColumnFamilyDrop");
  }
  if (has_max_column_family_) {
    r.append("\n  MaxColumnFamily: ");
    AppendNumberTo(&r, max_column_family_);
  }
  r.append("\n}\n");
  return r;
}
}
