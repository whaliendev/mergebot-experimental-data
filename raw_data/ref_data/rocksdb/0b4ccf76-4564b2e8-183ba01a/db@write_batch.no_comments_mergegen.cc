#include "rocksdb/write_batch.h"
#include "rocksdb/options.h"
#include "rocksdb/merge_operator.h"
#include "db/dbformat.h"
#include "db/db_impl.h"
#include "db/memtable.h"
#include "db/snapshot.h"
#include "db/write_batch_internal.h"
#include "util/coding.h"
#include "util/statistics.h"
#include <stdexcept>
namespace rocksdb {
static const size_t kHeader = 12;
WriteBatch::WriteBatch(size_t reserved_bytes) {
  rep_.reserve((reserved_bytes > kHeader) ? reserved_bytes : kHeader);
  Clear();
}
WriteBatch::~WriteBatch() { }
WriteBatch::Handler::~Handler() { }
void WriteBatch::Handler::Put(const Slice& key, const Slice& value) {
  throw std::runtime_error("Handler::Put not implemented!");
}
void WriteBatch::Handler::Merge(const Slice& key, const Slice& value) {
  throw std::runtime_error("Handler::Merge not implemented!");
}
void WriteBatch::Handler::Delete(const Slice& key) {
  throw std::runtime_error("Handler::Delete not implemented!");
}
void WriteBatch::Handler::LogData(const Slice& blob) {
}
bool WriteBatch::Handler::Continue() {
  return true;
}
void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(kHeader);
}
int WriteBatch::Count() const {
  return WriteBatchInternal::Count(this);
}
Status WriteBatch::Iterate(Handler* handler) const {
  Slice input(rep_);
  if (input.size() < kHeader) {
    return Status::Corruption("malformed WriteBatch (too small)");
  }
  input.remove_prefix(kHeader);
  Slice key, value, blob;
  int found = 0;
  while (!input.empty() && handler->Continue()) {
    char tag = input[0];
    input.remove_prefix(1);
    uint32_t column_family = 0;
    switch (tag) {
      case kTypeColumnFamilyValue:
        if (!GetVarint32(&input, &column_family)) {
          return Status::Corruption("bad WriteBatch Put");
        }
      case kTypeValue:
        if (GetLengthPrefixedSlice(&input, &key) &&
            GetLengthPrefixedSlice(&input, &value)) {
          handler->PutCF(column_family, key, value);
          found++;
        } else {
          return Status::Corruption("bad WriteBatch Put");
        }
        break;
      case kTypeColumnFamilyDeletion:
        if (!GetVarint32(&input, &column_family)) {
          return Status::Corruption("bad WriteBatch Delete");
        }
      case kTypeDeletion:
        if (GetLengthPrefixedSlice(&input, &key)) {
          handler->DeleteCF(column_family, key);
          found++;
        } else {
          return Status::Corruption("bad WriteBatch Delete");
        }
        break;
      case kTypeColumnFamilyMerge:
        if (!GetVarint32(&input, &column_family)) {
          return Status::Corruption("bad WriteBatch Merge");
        }
      case kTypeMerge:
        if (GetLengthPrefixedSlice(&input, &key) &&
            GetLengthPrefixedSlice(&input, &value)) {
          handler->MergeCF(column_family, key, value);
          found++;
        } else {
          return Status::Corruption("bad WriteBatch Merge");
        }
        break;
      case kTypeLogData:
        if (GetLengthPrefixedSlice(&input, &blob)) {
          handler->LogData(blob);
        } else {
          return Status::Corruption("bad WriteBatch Blob");
        }
        break;
      default:
        return Status::Corruption("unknown WriteBatch tag");
    }
  }
 if (found != WriteBatchInternal::Count(this)) {
    return Status::Corruption("WriteBatch has wrong count");
  } else {
    return Status::OK();
  }
}
int WriteBatchInternal::Count(const WriteBatch* b) {
  return DecodeFixed32(b->rep_.data() + 8);
}
void WriteBatchInternal::SetCount(WriteBatch* b, int n) {
  EncodeFixed32(&b->rep_[8], n);
}
SequenceNumber WriteBatchInternal::Sequence(const WriteBatch* b) {
  return SequenceNumber(DecodeFixed64(b->rep_.data()));
}
void WriteBatchInternal::SetSequence(WriteBatch* b, SequenceNumber seq) {
  EncodeFixed64(&b->rep_[0], seq);
}
void WriteBatch::Put(uint32_t column_family_id, const Slice& key,
                     const Slice& value) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  if (column_family_id == 0) {
    rep_.push_back(static_cast<char>(kTypeValue));
  } else {
    rep_.push_back(static_cast<char>(kTypeColumnFamilyValue));
    PutVarint32(&rep_, column_family_id);
  }
  PutLengthPrefixedSlice(&rep_, key);
  PutLengthPrefixedSlice(&rep_, value);
}
void WriteBatch::Put(uint32_t column_family_id, const SliceParts& key,
                     const SliceParts& value) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  if (column_family_id == 0) {
    rep_.push_back(static_cast<char>(kTypeValue));
  } else {
    rep_.push_back(static_cast<char>(kTypeColumnFamilyValue));
    PutVarint32(&rep_, column_family_id);
  }
  PutLengthPrefixedSliceParts(&rep_, key);
  PutLengthPrefixedSliceParts(&rep_, value);
}
void WriteBatch::Delete(uint32_t column_family_id, const Slice& key) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  if (column_family_id == 0) {
    rep_.push_back(static_cast<char>(kTypeDeletion));
  } else {
    rep_.push_back(static_cast<char>(kTypeColumnFamilyDeletion));
    PutVarint32(&rep_, column_family_id);
  }
  PutLengthPrefixedSlice(&rep_, key);
}
void WriteBatch::Merge(uint32_t column_family_id, const Slice& key,
                       const Slice& value) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  if (column_family_id == 0) {
    rep_.push_back(static_cast<char>(kTypeMerge));
  } else {
    rep_.push_back(static_cast<char>(kTypeColumnFamilyMerge));
    PutVarint32(&rep_, column_family_id);
  }
  PutLengthPrefixedSlice(&rep_, key);
  PutLengthPrefixedSlice(&rep_, value);
}
void WriteBatch::PutLogData(const Slice& blob) {
  rep_.push_back(static_cast<char>(kTypeLogData));
  PutLengthPrefixedSlice(&rep_, blob);
}
namespace {
class MemTableInserter : public WriteBatch::Handler {
 public:
  SequenceNumber sequence_;
  ColumnFamilyMemTables* cf_mems_;
  uint64_t log_number_;
  DBImpl* db_;
  const bool dont_filter_deletes_;
  MemTableInserter(SequenceNumber sequence, ColumnFamilyMemTables* cf_mems,
                   uint64_t log_number, DB* db, const bool dont_filter_deletes)
      : sequence_(sequence),
        cf_mems_(cf_mems),
        log_number_(log_number),
        db_(reinterpret_cast<DBImpl*>(db)),
        dont_filter_deletes_(dont_filter_deletes) {
    assert(cf_mems);
    if (!dont_filter_deletes_) {
      assert(db_);
    }
  }
bool IgnoreUpdate() { return log_number_ != 0 && log_number_ < cf_mems_->GetLogNumber(); } virtual void PutCF(uint32_t column_family_id, const Slice& key, const Slice& value) { bool found = cf_mems_->Seek(column_family_id);
    } else {
if (mem_->UpdateCallback(sequence_, key, value, *options_)) { } else {
    }
    sequence_++;
  }
virtual void MergeCF(uint32_t column_family_id, const Slice& key, const Slice& value) { bool found = cf_mems_->Seek(column_family_id); if (!found || IgnoreUpdate()) { return; } MemTable* mem = cf_mems_->GetMemTable(); const Options* options = cf_mems_->GetFullOptions();
    bool perform_merge = false;
    if (options->max_successive_merges > 0 && db_ != nullptr) {
      LookupKey lkey(key, sequence_);
      size_t num_merges = mem->CountSuccessiveMergeEntries(lkey);
      if (num_merges >= options->max_successive_merges) {
        perform_merge = true;
      }
    }
    if (perform_merge) {
      std::string get_value;
      SnapshotImpl read_from_snapshot;
      read_from_snapshot.number_ = sequence_;
      ReadOptions read_options;
      read_options.snapshot = &read_from_snapshot;
      db_->Get(read_options, cf_mems_->GetColumnFamilyHandle(), key,
               &get_value);
      Slice get_value_slice = Slice(get_value);
      auto merge_operator = options->merge_operator.get();
      assert(merge_operator);
      std::deque<std::string> operands;
      operands.push_front(value.ToString());
      std::string new_value;
      if (!merge_operator->FullMerge(key, &get_value_slice, operands,
                                     &new_value, options->info_log.get())) {
        RecordTick(options->statistics.get(), NUMBER_MERGE_FAILURES);
          perform_merge = false;
      } else {
        mem->Add(sequence_, kTypeValue, key, new_value);
      }
    }
    if (!perform_merge) {
      mem->Add(sequence_, kTypeMerge, key, value);
    }
    sequence_++;
  }
  virtual void DeleteCF(uint32_t column_family_id, const Slice& key) {
    bool found = cf_mems_->Seek(column_family_id);
    if (!found || IgnoreUpdate()) {
      return;
    }
    MemTable* mem = cf_mems_->GetMemTable();
    const Options* options = cf_mems_->GetFullOptions();
    if (!dont_filter_deletes_ && options->filter_deletes) {
      SnapshotImpl read_from_snapshot;
      read_from_snapshot.number_ = sequence_;
      ReadOptions ropts;
      ropts.snapshot = &read_from_snapshot;
      std::string value;
      if (!db_->KeyMayExist(ropts, cf_mems_->GetColumnFamilyHandle(), key,
                            &value)) {
        RecordTick(options->statistics.get(), NUMBER_FILTERED_DELETES);
        return;
      }
    }
    mem->Add(sequence_, kTypeDeletion, key, Slice());
    sequence_++;
  }
};
}
Status WriteBatchInternal::InsertInto(const WriteBatch* b,
                                      ColumnFamilyMemTables* memtables,
                                      uint64_t log_number, DB* db,
                                      const bool dont_filter_deletes) {
  MemTableInserter inserter(WriteBatchInternal::Sequence(b), memtables,
                            log_number, db, dont_filter_deletes);
  return b->Iterate(&inserter);
}
void WriteBatchInternal::SetContents(WriteBatch* b, const Slice& contents) {
  assert(contents.size() >= kHeader);
  b->rep_.assign(contents.data(), contents.size());
}
void WriteBatchInternal::Append(WriteBatch* dst, const WriteBatch* src) {
  SetCount(dst, Count(dst) + Count(src));
  assert(src->rep_.size() >= kHeader);
  dst->rep_.append(src->rep_.data() + kHeader, src->rep_.size() - kHeader);
}
}
