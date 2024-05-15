#include "db/memtable_list.h"
#include <string>
#include "rocksdb/db.h"
#include "db/memtable.h"
#include "db/version_set.h"
#include "rocksdb/env.h"
#include "rocksdb/iterator.h"
#include "util/coding.h"
namespace rocksdb {
class InternalKeyComparator;
class Mutex;
class VersionSet;
MemTableListVersion::MemTableListVersion(MemTableListVersion* old) {
  if (old != nullptr) {
    memlist_ = old->memlist_;
    size_ = old->size_;
    for (auto& m : memlist_) {
      m->Ref();
    }
  }
}
void MemTableListVersion::Ref() { ++refs_; }
void MemTableListVersion::Unref(autovector<MemTable*>* to_delete) {
  assert(refs_ >= 1);
  --refs_;
  if (refs_ == 0) {
    assert(to_delete != nullptr);
    for (const auto& m : memlist_) {
      MemTable* x = m->Unref();
      if (x != nullptr) {
        to_delete->push_back(x);
      }
    }
    delete this;
  }
}
int MemTableListVersion::size() const { return size_; }
int MemTableList::size() const {
  assert(num_flush_not_started_ <= current_->size_);
  return current_->size_;
}
bool MemTableListVersion::Get(const LookupKey& key, std::string* value,
                              Status* s, MergeContext& merge_context,
                              const Options& options) {
  for (auto& memtable : memlist_) {
    if (memtable->Get(key, value, s, merge_context, options)) {
      return true;
    }
  }
  return false;
}
void MemTableListVersion::AddIterators(const ReadOptions& options,
                                       std::vector<Iterator*>* iterator_list) {
  for (auto& m : memlist_) {
    iterator_list->push_back(m->NewIterator(options));
  }
}
void MemTableListVersion::Add(MemTable* m) {
  assert(refs_ == 1);
  memlist_.push_front(m);
  ++size_;
}
void MemTableListVersion::Remove(MemTable* m) {
  assert(refs_ == 1);
  memlist_.remove(m);
  --size_;
}
bool MemTableList::IsFlushPending() {
  if ((flush_requested_ && num_flush_not_started_ >= 1) ||
      (num_flush_not_started_ >= min_write_buffer_number_to_merge_)) {
    assert(imm_flush_needed.NoBarrier_Load() != nullptr);
    return true;
  }
  return false;
}
void MemTableList::PickMemtablesToFlush(autovector<MemTable*>* ret) {
  const auto& memlist = current_->memlist_;
  for (auto it = memlist.rbegin(); it != memlist.rend(); ++it) {
    MemTable* m = *it;
    if (!m->flush_in_progress_) {
      assert(!m->flush_completed_);
      num_flush_not_started_--;
      if (num_flush_not_started_ == 0) {
        imm_flush_needed.Release_Store(nullptr);
      }
      m->flush_in_progress_ = true;
      ret->push_back(m);
    }
  }
  flush_requested_ = false;
}
void MemTableList::RollbackMemtableFlush(const autovector<MemTable*>& mems,
                                         uint64_t file_number,
                                         std::set<uint64_t>* pending_outputs) {
  assert(!mems.empty());
  for (MemTable* m : mems) {
    assert(m->flush_in_progress_);
    assert(m->file_number_ == 0);
    m->flush_in_progress_ = false;
    m->flush_completed_ = false;
    m->edit_.Clear();
    num_flush_not_started_++;
  }
  pending_outputs->erase(file_number);
  imm_flush_needed.Release_Store(reinterpret_cast<void*>(1));
}
Status MemTableList::InstallMemtableFlushResults(
    ColumnFamilyData* cfd, const autovector<MemTable*>& mems, VersionSet* vset,
    Status flushStatus, port::Mutex* mu, Logger* info_log, uint64_t file_number,
    std::set<uint64_t>& pending_outputs, autovector<MemTable*>* to_delete,
    Directory* db_directory) {
  mu->AssertHeld();
  for (size_t i = 0; i < mems.size(); ++i) {
    assert(i == 0 || mems[i]->GetEdits()->NumEntries() == 0);
    mems[i]->flush_completed_ = true;
    mems[i]->file_number_ = file_number;
  }
  Status s;
  if (commit_in_progress_) {
    return s;
  }
  commit_in_progress_ = true;
  while (!current_->memlist_.empty() && s.ok()) {
    MemTable* m = current_->memlist_.back();
    if (!m->flush_completed_) {
      break;
    }
    Log(info_log, "Level-0 commit table #%lu started",
        (unsigned long)m->file_number_);
    s = vset->LogAndApply(cfd, &m->edit_, mu, db_directory);
    InstallNewVersion();
    uint64_t mem_id = 1;
    do {
      if (s.ok()) {
        Log(info_log, "Level-0 commit table #%lu: memtable #%lu done",
            (unsigned long)m->file_number_, (unsigned long)mem_id);
        current_->Remove(m);
        assert(m->file_number_ > 0);
        pending_outputs.erase(m->file_number_);
        if (m->Unref() != nullptr) {
          to_delete->push_back(m);
        }
      } else {
        Log(info_log, "Level-0 commit table #%lu: memtable #%lu failed",
            (unsigned long)m->file_number_, (unsigned long)mem_id);
        m->flush_completed_ = false;
        m->flush_in_progress_ = false;
        m->edit_.Clear();
        num_flush_not_started_++;
        pending_outputs.erase(m->file_number_);
        m->file_number_ = 0;
        imm_flush_needed.Release_Store((void*)1);
      }
      ++mem_id;
    } while (!current_->memlist_.empty() && (m = current_->memlist_.back()) &&
             m->file_number_ == file_number);
  }
  commit_in_progress_ = false;
  return s;
}
void MemTableList::Add(MemTable* m) {
  assert(current_->size_ >= num_flush_not_started_);
  InstallNewVersion();
  current_->Add(m);
  m->MarkImmutable();
  num_flush_not_started_++;
  if (num_flush_not_started_ == 1) {
    imm_flush_needed.Release_Store((void*)1);
  }
}
size_t MemTableList::ApproximateMemoryUsage() {
  size_t size = 0;
  for (auto& memtable : current_->memlist_) {
    size += memtable->ApproximateMemoryUsage();
  }
  return size;
}
void MemTableList::InstallNewVersion() {
  if (current_->refs_ == 1) {
  } else {
    MemTableListVersion* version = current_;
    current_ = new MemTableListVersion(current_);
    current_->Ref();
    version->Unref();
  }
}
}
