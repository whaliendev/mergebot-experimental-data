       
#include <set>
#include <utility>
#include <vector>
#include <string>
#include "rocksdb/cache.h"
#include "db/dbformat.h"
namespace rocksdb {
class VersionSet;
struct FileMetaData {
  int refs;
  int allowed_seeks;
  uint64_t number;
  uint64_t file_size;
  InternalKey smallest;
  InternalKey largest;
  bool being_compacted;
  SequenceNumber smallest_seqno;
  SequenceNumber largest_seqno;
  Cache::Handle* table_reader_handle;
  FileMetaData(uint64_t number, uint64_t file_size)
      : refs(0),
        allowed_seeks(1 << 30),
        number(number),
        file_size(file_size),
        being_compacted(false),
        table_reader_handle(nullptr) {}
  FileMetaData() : FileMetaData(0, 0) {}
};
class VersionEdit {
 public:
  VersionEdit() { Clear(); }
  ~VersionEdit() { }
  void Clear();
  void SetComparatorName(const Slice& name) {
    has_comparator_ = true;
    comparator_ = name.ToString();
  }
  void SetLogNumber(uint64_t num) {
    has_log_number_ = true;
    log_number_ = num;
  }
  void SetPrevLogNumber(uint64_t num) {
    has_prev_log_number_ = true;
    prev_log_number_ = num;
  }
  void SetNextFile(uint64_t num) {
    has_next_file_number_ = true;
    next_file_number_ = num;
  }
  void SetLastSequence(SequenceNumber seq) {
    has_last_sequence_ = true;
    last_sequence_ = seq;
  }
  void SetMaxColumnFamily(uint32_t max_column_family) {
    has_max_column_family_ = true;
    max_column_family_ = max_column_family;
  }
  void AddFile(int level, uint64_t file,
               uint64_t file_size,
               const InternalKey& smallest,
               const InternalKey& largest,
               const SequenceNumber& smallest_seqno,
               const SequenceNumber& largest_seqno) {
    assert(smallest_seqno <= largest_seqno);
    FileMetaData f;
    f.number = file;
    f.file_size = file_size;
    f.smallest = smallest;
    f.largest = largest;
    f.smallest_seqno = smallest_seqno;
    f.largest_seqno = largest_seqno;
    new_files_.push_back(std::make_pair(level, f));
  }
  void DeleteFile(int level, uint64_t file) {
    deleted_files_.insert({level, file});
  }
  int NumEntries() {
    return new_files_.size() + deleted_files_.size();
  }
  bool IsColumnFamilyManipulation() {
    return is_column_family_add_ || is_column_family_drop_;
  }
  void SetColumnFamily(uint32_t column_family_id) {
    column_family_ = column_family_id;
  }
  void AddColumnFamily(const std::string& name) {
    assert(!is_column_family_drop_);
    assert(!is_column_family_add_);
    assert(NumEntries() == 0);
    is_column_family_add_ = true;
    column_family_name_ = name;
  }
  void DropColumnFamily() {
    assert(!is_column_family_drop_);
    assert(!is_column_family_add_);
    assert(NumEntries() == 0);
    is_column_family_drop_ = true;
  }
  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(const Slice& src);
  std::string DebugString(bool hex_key = false) const;
 private:
  friend class VersionSet;
  typedef std::set< std::pair<int, uint64_t>> DeletedFileSet;
  bool GetLevel(Slice* input, int* level, const char** msg);
  int max_level_;
  std::string comparator_;
  uint64_t log_number_;
  uint64_t prev_log_number_;
  uint64_t next_file_number_;
  uint32_t max_column_family_;
  SequenceNumber last_sequence_;
  bool has_comparator_;
  bool has_log_number_;
  bool has_prev_log_number_;
  bool has_next_file_number_;
  bool has_last_sequence_;
  bool has_max_column_family_;
  DeletedFileSet deleted_files_;
<<<<<<< HEAD
  std::vector<std::pair<int, FileMetaData>> new_files_;
  uint32_t column_family_;
  bool is_column_family_drop_;
  bool is_column_family_add_;
  std::string column_family_name_;
  enum {
    kManifestVersion = 1
  };
||||||| b9ce156e3
  std::vector<std::pair<int, FileMetaData> > new_files_;
  enum {
    kManifestVersion = 1
  };
=======
  std::vector<std::pair<int, FileMetaData> > new_files_;
>>>>>>> 577556d5
};
}
