       
#include <stdio.h>
#include "rocksdb/comparator.h"
#include "rocksdb/db.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/slice.h"
#include "rocksdb/table.h"
#include "rocksdb/types.h"
#include "util/coding.h"
#include "util/logging.h"
namespace rocksdb {
class InternalKey;
enum ValueType : unsigned char {
  kTypeDeletion = 0x0,
  kTypeValue = 0x1,
  kTypeMerge = 0x2,
<<<<<<< HEAD
  kTypeLogData = 0x3,
  kTypeColumnFamilyDeletion = 0x4,
  kTypeColumnFamilyValue = 0x5,
  kTypeColumnFamilyMerge = 0x6,
||||||| 183ba01a0
  kTypeLogData = 0x3
=======
  kTypeLogData = 0x3,
  kMaxValue = 0x7F
>>>>>>> 4564b2e8
};
static const ValueType kValueTypeForSeek = kTypeMerge;
static const SequenceNumber kMaxSequenceNumber =
    ((0x1ull << 56) - 1);
struct ParsedInternalKey {
  Slice user_key;
  SequenceNumber sequence;
  ValueType type;
  ParsedInternalKey() { }
  ParsedInternalKey(const Slice& u, const SequenceNumber& seq, ValueType t)
      : user_key(u), sequence(seq), type(t) { }
  std::string DebugString(bool hex = false) const;
};
inline size_t InternalKeyEncodingLength(const ParsedInternalKey& key) {
  return key.user_key.size() + 8;
}
extern void AppendInternalKey(std::string* result,
                              const ParsedInternalKey& key);
extern bool ParseInternalKey(const Slice& internal_key,
                             ParsedInternalKey* result);
inline Slice ExtractUserKey(const Slice& internal_key) {
  assert(internal_key.size() >= 8);
  return Slice(internal_key.data(), internal_key.size() - 8);
}
inline ValueType ExtractValueType(const Slice& internal_key) {
  assert(internal_key.size() >= 8);
  const size_t n = internal_key.size();
  uint64_t num = DecodeFixed64(internal_key.data() + n - 8);
  unsigned char c = num & 0xff;
  return static_cast<ValueType>(c);
}
class InternalKeyComparator : public Comparator {
 private:
  const Comparator* user_comparator_;
  std::string name_;
 public:
  explicit InternalKeyComparator(const Comparator* c) : user_comparator_(c),
    name_("rocksdb.InternalKeyComparator:" +
          std::string(user_comparator_->Name())) {
  }
  virtual ~InternalKeyComparator() {}
  virtual const char* Name() const;
  virtual int Compare(const Slice& a, const Slice& b) const;
  virtual void FindShortestSeparator(
      std::string* start,
      const Slice& limit) const;
  virtual void FindShortSuccessor(std::string* key) const;
  const Comparator* user_comparator() const { return user_comparator_; }
  int Compare(const InternalKey& a, const InternalKey& b) const;
  int Compare(const ParsedInternalKey& a, const ParsedInternalKey& b) const;
};
class InternalFilterPolicy : public FilterPolicy {
 private:
  const FilterPolicy* const user_policy_;
 public:
  explicit InternalFilterPolicy(const FilterPolicy* p) : user_policy_(p) { }
  virtual const char* Name() const;
  virtual void CreateFilter(const Slice* keys, int n, std::string* dst) const;
  virtual bool KeyMayMatch(const Slice& key, const Slice& filter) const;
};
class InternalKey {
 private:
  std::string rep_;
 public:
  InternalKey() { }
  InternalKey(const Slice& user_key, SequenceNumber s, ValueType t) {
    AppendInternalKey(&rep_, ParsedInternalKey(user_key, s, t));
  }
  void DecodeFrom(const Slice& s) { rep_.assign(s.data(), s.size()); }
  Slice Encode() const {
    assert(!rep_.empty());
    return rep_;
  }
  Slice user_key() const { return ExtractUserKey(rep_); }
  void SetFrom(const ParsedInternalKey& p) {
    rep_.clear();
    AppendInternalKey(&rep_, p);
  }
  void Clear() { rep_.clear(); }
  std::string DebugString(bool hex = false) const;
};
inline int InternalKeyComparator::Compare(
    const InternalKey& a, const InternalKey& b) const {
  return Compare(a.Encode(), b.Encode());
}
inline bool ParseInternalKey(const Slice& internal_key,
                             ParsedInternalKey* result) {
  const size_t n = internal_key.size();
  if (n < 8) return false;
  uint64_t num = DecodeFixed64(internal_key.data() + n - 8);
  unsigned char c = num & 0xff;
  result->sequence = num >> 8;
  result->type = static_cast<ValueType>(c);
  assert(result->type <= ValueType::kMaxValue);
  result->user_key = Slice(internal_key.data(), n - 8);
  return (c <= static_cast<unsigned char>(kValueTypeForSeek));
}
inline void UpdateInternalKey(char* internal_key,
                              const size_t internal_key_size,
                              uint64_t seq, ValueType t) {
  assert(internal_key_size >= 8);
  char* seqtype = internal_key + internal_key_size - 8;
  uint64_t newval = (seq << 8) | t;
  EncodeFixed64(seqtype, newval);
}
inline uint64_t GetInternalKeySeqno(const Slice& internal_key) {
  const size_t n = internal_key.size();
  assert(n >= 8);
  uint64_t num = DecodeFixed64(internal_key.data() + n - 8);
  return num >> 8;
}
class LookupKey {
 public:
  LookupKey(const Slice& user_key, SequenceNumber sequence);
  ~LookupKey();
  Slice memtable_key() const { return Slice(start_, end_ - start_); }
  Slice internal_key() const { return Slice(kstart_, end_ - kstart_); }
  Slice user_key() const { return Slice(kstart_, end_ - kstart_ - 8); }
 private:
  const char* start_;
  const char* kstart_;
  const char* end_;
  char space_[200];
  LookupKey(const LookupKey&);
  void operator=(const LookupKey&);
};
inline LookupKey::~LookupKey() {
  if (start_ != space_) delete[] start_;
}
}
