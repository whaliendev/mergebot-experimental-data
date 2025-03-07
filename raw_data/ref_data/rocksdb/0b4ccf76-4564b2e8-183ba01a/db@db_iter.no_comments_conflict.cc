#include "db/db_iter.h"
#include <stdexcept>
#include <deque>
#include "db/filename.h"
#include "db/dbformat.h"
#include "rocksdb/env.h"
#include "rocksdb/options.h"
#include "rocksdb/iterator.h"
#include "rocksdb/merge_operator.h"
#include "port/port.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/perf_context_imp.h"
namespace rocksdb {
#if 0
static void DumpInternalIter(Iterator* iter) {
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ParsedInternalKey k;
    if (!ParseInternalKey(iter->key(), &k)) {
      fprintf(stderr, "Corrupt '%s'\n", EscapeString(iter->key()).c_str());
    } else {
      fprintf(stderr, "@ '%s'\n", k.DebugString().c_str());
    }
  }
}
#endif
namespace {
class DBIter: public Iterator {
 public:
  enum Direction {
    kForward,
    kReverse
  };
  DBIter(const std::string* dbname, Env* env, const Options& options,
         const Comparator* cmp, Iterator* iter, SequenceNumber s)
      : dbname_(dbname),
        env_(env),
        logger_(options.info_log.get()),
        user_comparator_(cmp),
        user_merge_operator_(options.merge_operator.get()),
        iter_(iter),
        sequence_(s),
        direction_(kForward),
        valid_(false),
        current_entry_is_merged_(false),
        statistics_(options.statistics.get()) {
    RecordTick(statistics_, NO_ITERATORS, 1);
    max_skip_ = options.max_sequential_skip_in_iterations;
  }
  virtual ~DBIter() {
    RecordTick(statistics_, NO_ITERATORS, -1);
    delete iter_;
  }
  virtual bool Valid() const { return valid_; }
  virtual Slice key() const {
    assert(valid_);
    return saved_key_;
  }
  virtual Slice value() const {
    assert(valid_);
    return (direction_ == kForward && !current_entry_is_merged_) ?
      iter_->value() : saved_value_;
  }
  virtual Status status() const {
    if (status_.ok()) {
      return iter_->status();
    } else {
      return status_;
    }
  }
  virtual void Next();
  virtual void Prev();
  virtual void Seek(const Slice& target);
  virtual void SeekToFirst();
  virtual void SeekToLast();
 private:
  inline void FindNextUserEntry(bool skipping);
  void FindNextUserEntryInternal(bool skipping);
  void FindPrevUserEntry();
  bool ParseKey(ParsedInternalKey* key);
  void MergeValuesNewToOld();
  inline void SaveKey(const Slice& k, std::string* dst) {
    dst->assign(k.data(), k.size());
  }
  inline void ClearSavedValue() {
    if (saved_value_.capacity() > 1048576) {
      std::string empty;
      swap(empty, saved_value_);
    } else {
      saved_value_.clear();
    }
  }
  const std::string* const dbname_;
  Env* const env_;
  Logger* logger_;
  const Comparator* const user_comparator_;
  const MergeOperator* const user_merge_operator_;
  Iterator* const iter_;
  SequenceNumber const sequence_;
  Status status_;
  std::string saved_key_;
  std::string saved_value_;
  std::string skip_key_;
  Direction direction_;
  bool valid_;
  bool current_entry_is_merged_;
  Statistics* statistics_;
  uint64_t max_skip_;
  DBIter(const DBIter&);
  void operator=(const DBIter&);
};
inline bool DBIter::ParseKey(ParsedInternalKey* ikey) {
  if (!ParseInternalKey(iter_->key(), ikey)) {
    status_ = Status::Corruption("corrupted internal key in DBIter");
    Log(logger_, "corrupted internal key in DBIter: %s",
        iter_->key().ToString(true).c_str());
    return false;
  } else {
    return true;
  }
}
void DBIter::Next() {
  assert(valid_);
  if (direction_ == kReverse) {
    direction_ = kForward;
    if (!iter_->Valid()) {
      iter_->SeekToFirst();
    } else {
      iter_->Next();
    }
    if (!iter_->Valid()) {
      valid_ = false;
      saved_key_.clear();
      return;
    }
  }
  if (!iter_->Valid()) {
    valid_ = false;
    return;
  }
  FindNextUserEntry(true );
}
inline void DBIter::FindNextUserEntry(bool skipping) {
  StopWatchNano timer(env_, false);
  StartPerfTimer(&timer);
  FindNextUserEntryInternal(skipping);
  BumpPerfTime(&perf_context.find_next_user_entry_time, &timer);
}
void DBIter::FindNextUserEntryInternal(bool skipping) {
  assert(iter_->Valid());
  assert(direction_ == kForward);
  current_entry_is_merged_ = false;
  uint64_t num_skipped = 0;
  do {
    ParsedInternalKey ikey;
    if (ParseKey(&ikey) && ikey.sequence <= sequence_) {
      if (skipping &&
          user_comparator_->Compare(ikey.user_key, saved_key_) <= 0) {
        num_skipped++;
        BumpPerfCount(&perf_context.internal_key_skipped_count);
      } else {
        skipping = false;
        switch (ikey.type) {
          case kTypeDeletion:
            SaveKey(ikey.user_key, &saved_key_);
            skipping = true;
            num_skipped = 0;
            BumpPerfCount(&perf_context.internal_delete_skipped_count);
            break;
          case kTypeValue:
            valid_ = true;
            SaveKey(ikey.user_key, &saved_key_);
            return;
          case kTypeMerge:
            SaveKey(ikey.user_key, &saved_key_);
            current_entry_is_merged_ = true;
            valid_ = true;
            MergeValuesNewToOld();
            return;
<<<<<<< HEAD
          case kTypeColumnFamilyDeletion:
          case kTypeColumnFamilyValue:
          case kTypeColumnFamilyMerge:
          case kTypeLogData:
||||||| 183ba01a0
          case kTypeLogData:
=======
          default:
>>>>>>> 4564b2e8
            assert(false);
            break;
        }
      }
    }
    if (skipping && num_skipped > max_skip_) {
      num_skipped = 0;
      std::string last_key;
      AppendInternalKey(&last_key,
        ParsedInternalKey(Slice(saved_key_), 0, kValueTypeForSeek));
      iter_->Seek(last_key);
      RecordTick(statistics_, NUMBER_OF_RESEEKS_IN_ITERATION);
    } else {
      iter_->Next();
    }
  } while (iter_->Valid());
  valid_ = false;
}
void DBIter::MergeValuesNewToOld() {
  if (!user_merge_operator_) {
    Log(logger_, "Options::merge_operator is null.");
    throw std::logic_error("DBIter::MergeValuesNewToOld() with"
                           " Options::merge_operator null");
  }
  std::deque<std::string> operands;
  operands.push_front(iter_->value().ToString());
  std::string merge_result;
  ParsedInternalKey ikey;
  for (iter_->Next(); iter_->Valid(); iter_->Next()) {
    if (!ParseKey(&ikey)) {
      continue;
    }
    if (user_comparator_->Compare(ikey.user_key, saved_key_) != 0) {
      break;
    }
    if (kTypeDeletion == ikey.type) {
      iter_->Next();
      break;
    }
    if (kTypeValue == ikey.type) {
      const Slice value = iter_->value();
      user_merge_operator_->FullMerge(ikey.user_key, &value, operands,
                                      &saved_value_, logger_);
      iter_->Next();
      return;
    }
    if (kTypeMerge == ikey.type) {
      const Slice& value = iter_->value();
      operands.push_front(value.ToString());
      while(operands.size() >= 2) {
        if (user_merge_operator_->PartialMerge(ikey.user_key,
                                               Slice(operands[0]),
                                               Slice(operands[1]),
                                               &merge_result,
                                               logger_)) {
          operands.pop_front();
          swap(operands.front(), merge_result);
        } else {
          break;
        }
      }
    }
  }
  user_merge_operator_->FullMerge(saved_key_, nullptr, operands,
                                  &saved_value_, logger_);
}
void DBIter::Prev() {
  assert(valid_);
  if (user_merge_operator_) {
    Log(logger_, "Prev not supported yet if merge_operator is provided");
    throw std::logic_error("DBIter::Prev backward iteration not supported"
                           " if merge_operator is provided");
  }
  if (direction_ == kForward) {
    assert(iter_->Valid());
    SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
    while (true) {
      iter_->Prev();
      if (!iter_->Valid()) {
        valid_ = false;
        saved_key_.clear();
        ClearSavedValue();
        return;
      }
      if (user_comparator_->Compare(ExtractUserKey(iter_->key()),
                                    saved_key_) < 0) {
        break;
      }
    }
    direction_ = kReverse;
  }
  FindPrevUserEntry();
}
void DBIter::FindPrevUserEntry() {
  assert(direction_ == kReverse);
  uint64_t num_skipped = 0;
  ValueType value_type = kTypeDeletion;
  if (iter_->Valid()) {
    do {
      ParsedInternalKey ikey;
      bool saved_key_cleared = false;
      if (ParseKey(&ikey) && ikey.sequence <= sequence_) {
        if ((value_type != kTypeDeletion) &&
            user_comparator_->Compare(ikey.user_key, saved_key_) < 0) {
          break;
        }
        value_type = ikey.type;
        if (value_type == kTypeDeletion) {
          saved_key_.clear();
          ClearSavedValue();
          saved_key_cleared = true;
        } else {
          Slice raw_value = iter_->value();
          if (saved_value_.capacity() > raw_value.size() + 1048576) {
            std::string empty;
            swap(empty, saved_value_);
          }
          SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
          saved_value_.assign(raw_value.data(), raw_value.size());
        }
      }
      num_skipped++;
      if (!saved_key_cleared && num_skipped > max_skip_) {
        num_skipped = 0;
        std::string last_key;
        AppendInternalKey(&last_key,
          ParsedInternalKey(Slice(saved_key_), kMaxSequenceNumber,
                            kValueTypeForSeek));
        iter_->Seek(last_key);
        RecordTick(statistics_, NUMBER_OF_RESEEKS_IN_ITERATION);
      } else {
        iter_->Prev();
      }
    } while (iter_->Valid());
  }
  if (value_type == kTypeDeletion) {
    valid_ = false;
    saved_key_.clear();
    ClearSavedValue();
    direction_ = kForward;
  } else {
    valid_ = true;
  }
}
void DBIter::Seek(const Slice& target) {
  saved_key_.clear();
  AppendInternalKey(
      &saved_key_, ParsedInternalKey(target, sequence_, kValueTypeForSeek));
  StopWatchNano internal_seek_timer(env_, false);
  StartPerfTimer(&internal_seek_timer);
  iter_->Seek(saved_key_);
  BumpPerfTime(&perf_context.seek_internal_seek_time, &internal_seek_timer);
  if (iter_->Valid()) {
    direction_ = kForward;
    ClearSavedValue();
    FindNextUserEntry(false );
  } else {
    valid_ = false;
  }
}
void DBIter::SeekToFirst() {
  direction_ = kForward;
  ClearSavedValue();
  StopWatchNano internal_seek_timer(env_, false);
  StartPerfTimer(&internal_seek_timer);
  iter_->SeekToFirst();
  BumpPerfTime(&perf_context.seek_internal_seek_time, &internal_seek_timer);
  if (iter_->Valid()) {
    FindNextUserEntry(false );
  } else {
    valid_ = false;
  }
}
void DBIter::SeekToLast() {
  if (user_merge_operator_) {
    Log(logger_, "SeekToLast not supported yet if merge_operator is provided");
    throw std::logic_error("DBIter::SeekToLast: backward iteration not"
                           " supported if merge_operator is provided");
  }
  direction_ = kReverse;
  ClearSavedValue();
  StopWatchNano internal_seek_timer(env_, false);
  StartPerfTimer(&internal_seek_timer);
  iter_->SeekToLast();
  BumpPerfTime(&perf_context.seek_internal_seek_time, &internal_seek_timer);
  FindPrevUserEntry();
}
}
Iterator* NewDBIterator(
    const std::string* dbname,
    Env* env,
    const Options& options,
    const Comparator *user_key_comparator,
    Iterator* internal_iter,
    const SequenceNumber& sequence) {
  return new DBIter(dbname, env, options, user_key_comparator,
                    internal_iter, sequence);
}
}
