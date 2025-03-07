#include "db/tailing_iter.h"
#include <string>
#include <utility>
#include "db/db_impl.h"
#include "db/column_family.h"
#include "rocksdb/slice.h"
#include "rocksdb/slice_transform.h"
namespace rocksdb {
TailingIterator::TailingIterator(DBImpl* db, const ReadOptions& options,
                                 ColumnFamilyData* cfd)
    : db_(db),
      options_(options),
      cfd_(cfd),
      version_number_(0),
      current_(nullptr),
      status_(Status::InvalidArgument("Seek() not called on this iterator")) {}
bool TailingIterator::Valid() const {
  return current_ != nullptr;
}
void TailingIterator::SeekToFirst() {
  if (!IsCurrentVersion()) {
    CreateIterators();
  }
  mutable_->SeekToFirst();
  immutable_->SeekToFirst();
  UpdateCurrent();
}
void TailingIterator::Seek(const Slice& target) {
  if (!IsCurrentVersion()) {
    CreateIterators();
  }
  mutable_->Seek(target);
  const Comparator* cmp = cfd_->user_comparator();
  if (!is_prev_set_ || cmp->Compare(prev_key_, target) >= !is_prev_inclusive_ ||
      (immutable_->Valid() && cmp->Compare(target, immutable_->key()) > 0) ||
      (options_.prefix_seek && !IsSamePrefix(target))) {
    SeekImmutable(target);
  }
  UpdateCurrent();
}
void TailingIterator::Next() {
  assert(Valid());
  if (!IsCurrentVersion()) {
    std::string current_key = key().ToString();
    Slice key_slice(current_key.data(), current_key.size());
    CreateIterators();
    Seek(key_slice);
    if (!Valid() || key().compare(key_slice) != 0) {
      return;
    }
  } else if (current_ == immutable_.get()) {
    prev_key_ = key().ToString();
    is_prev_inclusive_ = false;
    is_prev_set_ = true;
  }
  current_->Next();
  UpdateCurrent();
}
Slice TailingIterator::key() const {
  assert(Valid());
  return current_->key();
}
Slice TailingIterator::value() const {
  assert(Valid());
  return current_->value();
}
Status TailingIterator::status() const {
  if (!status_.ok()) {
    return status_;
  } else if (!mutable_->status().ok()) {
    return mutable_->status();
  } else {
    return immutable_->status();
  }
}
void TailingIterator::Prev() {
  status_ = Status::NotSupported("This iterator doesn't support Prev()");
}
void TailingIterator::SeekToLast() {
  status_ = Status::NotSupported("This iterator doesn't support SeekToLast()");
}
void TailingIterator::CreateIterators() {
  std::pair<Iterator*, Iterator*> iters =
      db_->GetTailingIteratorPair(options_, cfd_, &version_number_);
  assert(iters.first && iters.second);
  mutable_.reset(iters.first);
  immutable_.reset(iters.second);
  current_ = nullptr;
  is_prev_set_ = false;
}
void TailingIterator::UpdateCurrent() {
  current_ = nullptr;
  if (mutable_->Valid()) {
    current_ = mutable_.get();
  }
  const Comparator* cmp = cfd_->user_comparator();
  if (immutable_->Valid() &&
      (current_ == nullptr ||
       cmp->Compare(immutable_->key(), current_->key()) < 0)) {
    current_ = immutable_.get();
  }
  if (!status_.ok()) {
    status_ = Status::OK();
  }
}
bool TailingIterator::IsCurrentVersion() const {
  return mutable_ != nullptr && immutable_ != nullptr &&
         version_number_ == cfd_->GetSuperVersionNumber();
}
bool TailingIterator::IsSamePrefix(const Slice& target) const {
  const SliceTransform* extractor = cfd_->options()->prefix_extractor.get();
  assert(extractor);
  assert(is_prev_set_);
  return extractor->Transform(target)
    .compare(extractor->Transform(prev_key_)) == 0;
}
void TailingIterator::SeekImmutable(const Slice& target) {
  prev_key_ = target.ToString();
  is_prev_inclusive_ = true;
  is_prev_set_ = true;
  immutable_->Seek(target);
}
}
