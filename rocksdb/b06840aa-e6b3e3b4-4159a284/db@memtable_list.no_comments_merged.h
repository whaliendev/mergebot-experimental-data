       
#include <string>
#include <list>
#include <vector>
#include <set>
#include <deque>
#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/iterator.h"
#include "db/dbformat.h"
#include "db/memtable.h"
#include "db/skiplist.h"
#include "db/memtable.h"
#include "rocksdb/db.h"
#include "rocksdb/iterator.h"
#include "rocksdb/options.h"
#include "util/autovector.h"
namespace rocksdb {
class ColumnFamilyData;
class InternalKeyComparator;
class Mutex;
class MemTableListVersion {
 public:
  explicit MemTableListVersion(MemTableListVersion* old = nullptr);
  void Ref();
  void Unref(autovector<MemTable*>* to_delete = nullptr);
  int size() const;
  bool Get(const LookupKey& key, std::string* value, Status* s,
           MergeContext& merge_context, const Options& options);
  void AddIterators(const ReadOptions& options,
                    std::vector<Iterator*>* iterator_list);
 private:
  void Add(MemTable* m);
  void Remove(MemTable* m);
  friend class MemTableList;
  std::list<MemTable*> memlist_;
  int size_ = 0;
  int refs_ = 0;
};
class MemTableList {
 public:
  explicit MemTableList(int min_write_buffer_number_to_merge)
      : min_write_buffer_number_to_merge_(min_write_buffer_number_to_merge),
        current_(new MemTableListVersion()),
        num_flush_not_started_(0),
        commit_in_progress_(false),
        flush_requested_(false) {
    imm_flush_needed.Release_Store(nullptr);
    current_->Ref();
  }
  ~MemTableList() {}
  MemTableListVersion* current() { return current_; }
  port::AtomicPointer imm_flush_needed;
  int size() const;
  bool IsFlushPending();
  void PickMemtablesToFlush(autovector<MemTable*>* mems);
  void RollbackMemtableFlush(const autovector<MemTable*>& mems,
                             uint64_t file_number,
                             std::set<uint64_t>* pending_outputs);
  Status InstallMemtableFlushResults(ColumnFamilyData* cfd,
                                     const autovector<MemTable*>& m,
                                     VersionSet* vset, port::Mutex* mu,
                                     Logger* info_log, uint64_t file_number,
                                     std::set<uint64_t>& pending_outputs,
                                     autovector<MemTable*>* to_delete,
                                     Directory* db_directory);
  void Add(MemTable* m);
  size_t ApproximateMemoryUsage();
  void FlushRequested() { flush_requested_ = true; }
 private:
  void InstallNewVersion();
  int min_write_buffer_number_to_merge_;
  MemTableListVersion* current_;
  int num_flush_not_started_;
  bool commit_in_progress_;
  bool flush_requested_;
};
}
