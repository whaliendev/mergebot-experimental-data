//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <algorithm>
#include <map>
#include <string>
#include <memory>
#include <vector>
#include "db/dbformat.h"
#include "rocksdb/statistics.h"
#include "util/statistics.h"
#include "db/memtable.h"
#include "db/write_batch_internal.h"
#include "rocksdb/cache.h"
#include "rocksdb/db.h"
#include "rocksdb/plain_table_factory.h"
#include "rocksdb/env.h"
#include "rocksdb/iterator.h"
#include "rocksdb/memtablerep.h"
#include "table/meta_blocks.h"
#include "table/block_based_table_builder.h"
#include "table/block_based_table_factory.h"
#include "table/block_based_table_reader.h"
#include "table/block_builder.h"
#include "table/block.h"
#include "table/format.h"
#include "util/random.h"
#include "util/testharness.h"
#include "util/testutil.h"

namespace rocksdb {

namespace {
// Return reverse of "key".
// Used to test non-lexicographic comparators.
std::string Reverse(const Slice& key) {
  auto rev = key.ToString();
  std::reverse(rev.begin(), rev.end());
  return rev;
}

class ReverseKeyComparator : public Comparator {
public:
  virtual const char* Name() const {
    return "rocksdb.ReverseBytewiseComparator";
  }
  
  virtual int Compare(const Slice& a, const Slice& b) const {
    return BytewiseComparator()->Compare(Reverse(a), Reverse(b));
  }
  
  virtual void FindShortestSeparator(
      std::string* start,
      const Slice& limit) const {
    std::string s = Reverse(*start);
    std::string l = Reverse(limit);
    BytewiseComparator()->FindShortestSeparator(&s, l);
    *start = Reverse(s);
  }
  
  virtual void FindShortSuccessor(std::string* key) const {
    std::string s = Reverse(*key);
    BytewiseComparator()->FindShortSuccessor(&s);
    *key = Reverse(s);
  }
};

ReverseKeyComparator reverse_key_comparator;

void Increment(const Comparator* cmp, std::string* key) {
  if (cmp == BytewiseComparator()) {
    key->push_back('\0');
  } else {
    assert(cmp == &reverse_key_comparator);
    std::string rev = Reverse(*key);
    rev.push_back('\0');
    *key = Reverse(rev);
  }
}

// An STL comparator that uses a Comparator
struct STLLessThan {
  const Comparator* cmp;
  
  STLLessThan(): cmp(BytewiseComparator()) { }
  explicitSTLLessThan(const Comparator* c): cmp(c) { }
  bool operator()(const std::string& a, const std::string& b) const {
    return cmp->Compare(Slice(a), Slice(b)) < 0;
  }
};

} // namespace

class StringSink: public WritableFile {
public:
  ~StringSink() { }
  
  const std::string& contents() const { return contents_; }
  
  virtual Status Close() { return Status::OK(); }
  virtual Status Flush() { return Status::OK(); }
  virtual Status Sync() { return Status::OK(); }
  
  virtual Status Append(const Slice& data) {
    contents_.append(data.data(), data.size());
    return Status::OK();
  }
  
private:
  std::string contents_;
};

class StringSource: public RandomAccessFile {
public:
  StringSource(const Slice& contents, uint64_t uniq_id, bool mmap)
      : contents_(contents.data(), contents.size()), uniq_id_(uniq_id),
        mmap_(mmap) {
  }
  
  virtual ~StringSource() { }
  
  uint64_t Size() const { return contents_.size(); }
  
  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                       char* scratch) const {
    if (offset > contents_.size()) {
      return Status::InvalidArgument("invalid Read offset");
    }
    if (offset + n > contents_.size()) {
      n = contents_.size() - offset;
    }
    if (!mmap_) {
      memcpy(scratch, &contents_[offset], n);
      *result = Slice(scratch, n);
    } else {
      *result = Slice(&contents_[offset], n);
    }
    return Status::OK();
  }
  
  virtual size_t GetUniqueId(char* id, size_t max_size) const {
    if (max_size < 20) {
      return 0;
    }
  
    char* rid = id;
    rid = EncodeVarint64(rid, uniq_id_);
    rid = EncodeVarint64(rid, 0);
    return static_cast<size_t>(rid-id);
  }
  
private:
  std::string contents_;
  uint64_t uniq_id_;
  bool mmap_;
};

typedef std::map<std::string, std::string, STLLessThan> KVMap;

// Helper class for tests to unify the interface between
// BlockBuilder/TableBuilder and Block/Table.
class Constructor {
public:
  explicit Constructor(const Comparator* cmp) : data_(STLLessThan(cmp)) {}
  virtual ~Constructor() { }
  
  void Add(const std::string& key, const Slice& value) {
    data_[key] = value.ToString();
  }
  
  // Finish constructing the data structure with all the keys that have
  // been added so far.  Returns the keys in sorted order in "*keys"
  // and stores the key/value pairs in "*kvmap"
  void Finish(const Options& options,
              std::vector<std::string>* keys,
              KVMap* kvmap) {
    *kvmap = data_;
    keys->clear();
    for (KVMap::const_iterator it = data_.begin();
         it != data_.end();
         ++it) {
      keys->push_back(it->first);
    }
    data_.clear();
    Status s = FinishImpl(options, *kvmap);
    ASSERT_TRUE(s.ok()) << s.ToString();
  }
  
  // Construct the data structure from the data in "data"
  virtual Status FinishImpl(const Options& options, const KVMap& data) = 0;
  
  virtual Iterator* NewIterator() const = 0;
  
  virtual const KVMap& data() { return data_; }
  
  // Returns nullptr if not running against a DB
  DB* db() const { return constructor_->db(){ return constructor_->db(); }
  
                                              // Overridden in DBConstructor
private:
  KVMap data_;
};

class BlockConstructor: public Constructor {
public:
  explicit BlockConstructor(const Comparator* cmp)
      : Constructor(cmp),
        comparator_(cmp),
        block_(nullptr) { }
  ~BlockConstructor() {
    delete block_;
  }{
    delete block_;
  }
  virtual Status FinishImpl(const Options& options, const KVMap& data) {
    delete block_;
    block_ = nullptr;
    BlockBuilder builder(options);
  
    for (KVMap::const_iterator it = data.begin();
         it != data.end();
         ++it) {
      builder.Add(it->first, it->second);
    }
    // Open the block
    data_ = builder.Finish().ToString();
    BlockContents contents;
    contents.data = data_;
    contents.cachable = false;
    contents.heap_allocated = false;
    block_ = new Block(contents);
    return Status::OK();
  }
  virtual Iterator* NewIterator() const {
    return block_->NewIterator(comparator_);
  }
  
private:
  const Comparator* comparator_;
  std::string data_;
  Block* block_;
  
  BlockConstructor();
};

class TableConstructor: public Constructor {
public:
  explicitTableConstructor(const Comparator* cmp, bool convert_to_internal_key = false): Constructor(cmp), convert_to_internal_key_(convert_to_internal_key) {
  }
  ~TableConstructor() {
    Reset();
  }
  
  virtual Status FinishImpl(const Options& options, const KVMap& data) {
    Reset();
    sink_.reset(new StringSink());
    unique_ptr<TableBuilder> builder;
    builder.reset(
        options.table_factory->GetTableBuilder(options, sink_.get(),
                                               options.compression));
  
    for (KVMap::const_iterator it = data.begin();
         it != data.end();
         ++it) {
      if (convert_to_internal_key_) {
        ParsedInternalKey ikey(it->first, kMaxSequenceNumber, kTypeValue);
        std::string encoded;
        AppendInternalKey(&encoded, ikey);
        builder->Add(encoded, it->second);
      } else {
        builder->Add(it->first, it->second);
      }
      ASSERT_TRUE(builder->status().ok());
    }
    Status s = builder->Finish();
    ASSERT_TRUE(s.ok()) << s.ToString();
  
    ASSERT_EQ(sink_->contents().size(), builder->FileSize());
  
    // Open the table
    uniq_id_ = cur_uniq_id_++;
<<<<<<< HEAD
    source_.reset(
        new StringSource(sink_->contents(), uniq_id_,
                         options.allow_mmap_reads));
    unique_ptr<TableFactory> table_factory;
||||||| 4e91f27c3
    source_.reset(new StringSource(sink_->contents(), uniq_id_));
    unique_ptr<TableFactory> table_factory;
=======
    source_.reset(new StringSource(sink_->contents(), uniq_id_));
>>>>>>> 9dc29414
    return options.table_factory->GetTableReader(options, soptions,
                                                 std::move(source_),
                                                 sink_->contents().size(),
                                                 &table_reader_);
  }
  
  virtual Iterator* NewIterator() const {
    Iterator* iter = table_reader_->NewIterator(ReadOptions());
    if (convert_to_internal_key_) {
      return new KeyConvertingIterator(iter);
    } else {
      return iter;
    }
  }
  
  uint64_t ApproximateOffsetOf(const Slice& key) const {
    return table_reader_->ApproximateOffsetOf(key);
  }
  
  virtual Status Reopen(const Options& options) {
    source_.reset(
        new StringSource(sink_->contents(), uniq_id_,
                         options.allow_mmap_reads));
    return options.table_factory->GetTableReader(options, soptions,
                                                 std::move(source_),
                                                 sink_->contents().size(),
                                                 &table_reader_);
  }
  
  virtual TableReader* table_reader() {
    return table_reader_.get();
  }
  
private:
  void Reset() {
    uniq_id_ = 0;
    table_reader_.reset();
    sink_.reset();
    source_.reset();
  }
  
  bool convert_to_internal_key_;
  
  uint64_t uniq_id_;
  unique_ptr<StringSink> sink_;
  unique_ptr<StringSource> source_;
  unique_ptr<TableReader> table_reader_;
  
  TableConstructor();
  
  static uint64_t cur_uniq_id_;
  const EnvOptions soptions;
};

// A helper class that converts internal format keys into user keys
class KeyConvertingIterator: public Iterator {
public:
  explicit KeyConvertingIterator(Iterator* iter) : iter_(iter) { }
  virtual ~KeyConvertingIterator() { delete iter_; }{ delete iter_; }
  virtual bool Valid() const { return iter_->Valid(){ return iter_->Valid(); }
  virtual void Seek(const Slice& target) {
    ParsedInternalKey ikey(target, kMaxSequenceNumber, kTypeValue);
    std::string encoded;
    AppendInternalKey(&encoded, ikey);
    iter_->Seek(encoded);
  }
  virtual void SeekToFirst() { iter_->SeekToFirst(){ iter_->SeekToFirst(); }
  virtual void SeekToLast() { iter_->SeekToLast(){ iter_->SeekToLast(); }
  virtual void Next() { iter_->Next(){ iter_->Next(); }
  virtual void Prev() { iter_->Prev(){ iter_->Prev(); }
  
  virtual Slice key() const {
    assert(Valid());
    ParsedInternalKey key;
    if (!ParseInternalKey(iter_->key(), &key)) {
      status_ = Status::Corruption("malformed internal key");
      return Slice("corrupted key");
    }
    return key.user_key;
  }
  
  virtual Slice value() const { return iter_->value(){ return iter_->value(); }
  virtual Status status() const {
    return status_.ok() ? iter_->status() : status_;
  }
  
private:
  mutable Status status_;
  Iterator* iter_;
  
  // No copying allowed
  KeyConvertingIterator(const KeyConvertingIterator&);
  void operator=(const KeyConvertingIterator&);
};

uint64_t TableConstructor::cur_uniq_id_ = 1;

class MemTableConstructor: public Constructor {
public:
  explicit MemTableConstructor(const Comparator* cmp)
      : Constructor(cmp),
        internal_comparator_(cmp),
        table_factory_(new SkipListFactory) {
    Options options;
    options.memtable_factory = table_factory_;
    memtable_ = new MemTable(internal_comparator_, options);
    memtable_->Ref();
  }
  ~MemTableConstructor() {
    delete memtable_->Unref();
  }{
    delete memtable_->Unref();
  }
  virtual Status FinishImpl(const Options& options, const KVMap& data) {
    delete memtable_->Unref();
    Options memtable_options;
    memtable_options.memtable_factory = table_factory_;
    memtable_ = new MemTable(internal_comparator_, memtable_options);
    memtable_->Ref();
    int seq = 1;
    for (KVMap::const_iterator it = data.begin();
         it != data.end();
         ++it) {
      memtable_->Add(seq, kTypeValue, it->first, it->second);
      seq++;
    }
    return Status::OK();
  }
  virtual Iterator* NewIterator() const {
    return new KeyConvertingIterator(memtable_->NewIterator());
  }
  
private:
  InternalKeyComparator internal_comparator_;
  MemTable* memtable_;
  std::shared_ptr<SkipListFactory> table_factory_;
};

class DBConstructor: public Constructor {
public:
  explicit DBConstructor(const Comparator* cmp)
      : Constructor(cmp),
        comparator_(cmp) {
    db_ = nullptr;
    NewDB();
  }
  ~DBConstructor() {
    delete db_;
  }{
    delete db_;
  }
  virtual Status FinishImpl(const Options& options, const KVMap& data) {
    delete db_;
    db_ = nullptr;
    NewDB();
    for (KVMap::const_iterator it = data.begin();
         it != data.end();
         ++it) {
      WriteBatch batch;
      batch.Put(it->first, it->second);
      ASSERT_TRUE(db_->Write(WriteOptions(), &batch).ok());
    }
    return Status::OK();
  }
  virtual Iterator* NewIterator() const {
    return db_->NewIterator(ReadOptions());
  }
  
  virtual DB* db() const { return db_; }
  
private:
  void NewDB() {
    std::string name = test::TmpDir() + "/table_testdb";
  
    Options options;
    options.comparator = comparator_;
    Status status = DestroyDB(name, options);
    ASSERT_TRUE(status.ok()) << status.ToString();
  
    options.create_if_missing = true;
    options.error_if_exists = true;
    options.write_buffer_size = 10000;  // Something small to force merging
    status = DB::Open(options, name, &db_);
    ASSERT_TRUE(status.ok()) << status.ToString();
  }
  
  const Comparator* comparator_;
  DB* db_;
};

static bool SnappyCompressionSupported() {
  std::string out;
  Slice in = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  return port::Snappy_Compress(Options().compression_opts,
                               in.data(), in.size(),
                               &out);
}

static bool ZlibCompressionSupported() {
  std::string out;
  Slice in = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  return port::Zlib_Compress(Options().compression_opts,
                             in.data(), in.size(),
                             &out);
}

#ifdef BZIP2
static bool BZip2CompressionSupported() {
  std::string out;
  Slice in = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  return port::BZip2_Compress(Options().compression_opts,
                              in.data(), in.size(),
                              &out);
}
#endif

enum TestType {
BLOCK_TEST,MEMTABLE_TEST,  DB_TEST
BLOCK_BASED_TABLE_TEST,PLAIN_TABLE_SEMI_FIXED_PREFIX,PLAIN_TABLE_FULL_STR_PREFIX,};

struct TestArgs {
  TestType type;
  bool reverse_compare;
  int restart_interval;
  CompressionType compression;
};

static std::vector<TestArgs> GenerateArgList() {
  std::vector<TestArgs> test_args;
  std::vector<TestType> test_types = {
      BLOCK_BASED_TABLE_TEST,      PLAIN_TABLE_SEMI_FIXED_PREFIX,
      PLAIN_TABLE_FULL_STR_PREFIX, BLOCK_TEST,
      MEMTABLE_TEST,               DB_TEST};
  std::vector<bool> reverse_compare_types = {false, true};
  std::vector<int> restart_intervals = {16, 1, 1024};

  // Only add compression if it is supported
  std::vector<CompressionType> compression_types = {kNoCompression};
#ifdef SNAPPY
  if (SnappyCompressionSupported()) {
    compression_types.push_back(kSnappyCompression);
  }
#endif

#ifdef ZLIB
  if (ZlibCompressionSupported()) {
    compression_types.push_back(kZlibCompression);
  }
#endif

#ifdef BZIP2
  if (BZip2CompressionSupported()) {
    compression_types.push_back(kBZip2Compression);
  }
#endif

  for (auto test_type : test_types) {
    for (auto reverse_compare : reverse_compare_types) {
      if (test_type == PLAIN_TABLE_SEMI_FIXED_PREFIX ||
          test_type == PLAIN_TABLE_FULL_STR_PREFIX) {
        // Plain table doesn't use restart index or compression.
        TestArgs one_arg;
        one_arg.type = test_type;
        one_arg.reverse_compare = reverse_compare;
        one_arg.restart_interval = restart_intervals[0];
        one_arg.compression = compression_types[0];
        test_args.push_back(one_arg);
        continue;
      }

      for (auto restart_interval : restart_intervals) {
        for (auto compression_type : compression_types) {
          TestArgs one_arg;
          one_arg.type = test_type;
          one_arg.reverse_compare = reverse_compare;
          one_arg.restart_interval = restart_interval;
          one_arg.compression = compression_type;
          test_args.push_back(one_arg);
        }
      }
    }
  }
  return test_args;
}

// In order to make all tests run for plain table format, including
// those operating on empty keys, create a new prefix transformer which
// return fixed prefix if the slice is not shorter than the prefix length,
// and the full slice if it is shorter.
class FixedOrLessPrefixTransform : public SliceTransform {
private:
  const size_t prefix_len_;
  
public:
  explicitFixedOrLessPrefixTransform(size_t prefix_len): prefix_len_(prefix_len) {
  }
  
  virtual const char* Name() const {
    return "rocksdb.FixedPrefix";
  }
  
  virtual Slice Transform(const Slice& src) const {
    assert(InDomain(src));
    if (src.size() < prefix_len_) {
      return src;
    }
    return Slice(src.data(), prefix_len_);
  }
  
  virtual bool InDomain(const Slice& src) const {
    return true;
  }
  
  virtual bool InRange(const Slice& dst) const {
    return (dst.size() <= prefix_len_);
  }
};

class Harness {
public:
  Harness() : constructor_(nullptr) { }
  
  void Init(const TestArgs& args) {
    delete constructor_;
    constructor_ = nullptr;
    options_ = Options();
  
    options_.block_restart_interval = args.restart_interval;
    options_.compression = args.compression;
    // Use shorter block size for tests to exercise block boundary
    // conditions more.
    options_.block_size = 256;
    if (args.reverse_compare) {
      options_.comparator = &reverse_key_comparator;
    }
    internal_comparator_.reset(new InternalKeyComparator(options_.comparator));
    support_prev_ = true;
    only_support_prefix_seek_ = false;
    BlockBasedTableFactory::TableOptions table_options;
    switch (args.type) {
      case BLOCK_BASED_TABLE_TEST:
        table_options.flush_block_policy_factory.reset(
            new FlushBlockBySizePolicyFactory(options_.block_size,
                                              options_.block_size_deviation));
        options_.table_factory.reset(new BlockBasedTableFactory(table_options));
        constructor_ = new TableConstructor(options_.comparator);
        break;
      case PLAIN_TABLE_SEMI_FIXED_PREFIX:
        support_prev_ = false;
        only_support_prefix_seek_ = true;
        options_.prefix_extractor = prefix_transform.get();
        options_.allow_mmap_reads = true;
        options_.table_factory.reset(new PlainTableFactory());
        constructor_ = new TableConstructor(options_.comparator, true);
        options_.comparator = internal_comparator_.get();
        break;
      case PLAIN_TABLE_FULL_STR_PREFIX:
        support_prev_ = false;
        only_support_prefix_seek_ = true;
        options_.prefix_extractor = noop_transform.get();
        options_.allow_mmap_reads = true;
        options_.table_factory.reset(new PlainTableFactory());
        constructor_ = new TableConstructor(options_.comparator, true);
        options_.comparator = internal_comparator_.get();
        break;
      case BLOCK_TEST:
        constructor_ = new BlockConstructor(options_.comparator);
        break;
      case MEMTABLE_TEST:
        constructor_ = new MemTableConstructor(options_.comparator);
        break;
      case DB_TEST:
        constructor_ = new DBConstructor(options_.comparator);
        break;
    }
  }
  
  ~Harness() {
    delete constructor_;
  }{
    delete constructor_;
  }
  
  void Add(const std::string& key, const std::string& value) {
    constructor_->Add(key, value);
  }
  
  void Test(Random* rnd) {
    std::vector<std::string> keys;
    KVMap data;
    constructor_->Finish(options_, &keys, &data);
  
    TestForwardScan(keys, data);
    if (support_prev_) {
      TestBackwardScan(keys, data);
    }
    TestRandomAccess(rnd, keys, data);
  }
  
  void TestForwardScan(const std::vector<std::string>& keys,
                       const KVMap& data) {
    Iterator* iter = constructor_->NewIterator();
    ASSERT_TRUE(!iter->Valid());
    iter->SeekToFirst();
    for (KVMap::const_iterator model_iter = data.begin();
         model_iter != data.end();
         ++model_iter) {
      ASSERT_EQ(ToString(data, model_iter), ToString(iter));
      iter->Next();
    }
    ASSERT_TRUE(!iter->Valid());
    delete iter;
  }
  
  void TestBackwardScan(const std::vector<std::string>& keys,
                        const KVMap& data) {
    Iterator* iter = constructor_->NewIterator();
    ASSERT_TRUE(!iter->Valid());
    iter->SeekToLast();
    for (KVMap::const_reverse_iterator model_iter = data.rbegin();
         model_iter != data.rend();
         ++model_iter) {
      ASSERT_EQ(ToString(data, model_iter), ToString(iter));
      iter->Prev();
    }
    ASSERT_TRUE(!iter->Valid());
    delete iter;
  }
  
  void TestRandomAccess(Random* rnd,
                        const std::vector<std::string>& keys,
                        const KVMap& data) {
    static const bool kVerbose = false;
    Iterator* iter = constructor_->NewIterator();
    ASSERT_TRUE(!iter->Valid());
    KVMap::const_iterator model_iter = data.begin();
    if (kVerbose) fprintf(stderr, "---\n");
    for (int i = 0; i < 200; i++) {
      const int toss = rnd->Uniform(support_prev_ ? 5 : 3);
      switch (toss) {
        case 0: {
          if (iter->Valid()) {
            if (kVerbose) fprintf(stderr, "Next\n");
            iter->Next();
            ++model_iter;
            ASSERT_EQ(ToString(data, model_iter), ToString(iter));
          }
          break;
        }
  
        case 1: {
          if (kVerbose) fprintf(stderr, "SeekToFirst\n");
          iter->SeekToFirst();
          model_iter = data.begin();
          ASSERT_EQ(ToString(data, model_iter), ToString(iter));
          break;
        }
  
        case 2: {
          std::string key = PickRandomKey(rnd, keys);
          model_iter = data.lower_bound(key);
          if (kVerbose) fprintf(stderr, "Seek '%s'\n",
                                EscapeString(key).c_str());
          iter->Seek(Slice(key));
          ASSERT_EQ(ToString(data, model_iter), ToString(iter));
          break;
        }
  
        case 3: {
          if (iter->Valid()) {
            if (kVerbose) fprintf(stderr, "Prev\n");
            iter->Prev();
            if (model_iter == data.begin()) {
              model_iter = data.end();   // Wrap around to invalid value
            } else {
              --model_iter;
            }
            ASSERT_EQ(ToString(data, model_iter), ToString(iter));
          }
          break;
        }
  
        case 4: {
          if (kVerbose) fprintf(stderr, "SeekToLast\n");
          iter->SeekToLast();
          if (keys.empty()) {
            model_iter = data.end();
          } else {
            std::string last = data.rbegin()->first;
            model_iter = data.lower_bound(last);
          }
          ASSERT_EQ(ToString(data, model_iter), ToString(iter));
          break;
        }
      }
    }
    delete iter;
  }
  
  std::string ToString(const KVMap& data,
                       const KVMap::const_reverse_iterator& it) {
    if (it == data.rend()) {
      return "END";
    } else {
      return "'" + it->first + "->" + it->second + "'";
    }
  }
  
  std::string ToString(const KVMap& data,
                       const KVMap::const_reverse_iterator& it) {
    if (it == data.rend()) {
      return "END";
    } else {
      return "'" + it->first + "->" + it->second + "'";
    }
  }
  
  std::string ToString(const Iterator* it) {
    if (!it->Valid()) {
      return "END";
    } else {
      return "'" + it->key().ToString() + "->" + it->value().ToString() + "'";
    }
  }
  
  std::string PickRandomKey(Random* rnd, const std::vector<std::string>& keys) {
    if (keys.empty()) {
      return "foo";
    } else {
      const int index = rnd->Uniform(keys.size());
      std::string result = keys[index];
      switch (rnd->Uniform(support_prev_ ? 3 : 1)) {
        case 0:
          // Return an existing key
          break;
        case 1: {
          // Attempt to return something smaller than an existing key
          if (result.size() > 0 && result[result.size() - 1] > '\0'
              && (!only_support_prefix_seek_
                  || options_.prefix_extractor->Transform(result).size()
                  < result.size())) {
            result[result.size() - 1]--;
          }
          break;
      }
        case 2: {
          // Return something larger than an existing key
          Increment(options_.comparator, &result);
          break;
        }
      }
      return result;
    }
  }
  
  // Returns nullptr if not running against a DB
  DB* db() const { return constructor_->db(){ return constructor_->db(); }
  
private:
  Options options_ = Options();
  Constructor* constructor_;
  bool support_prev_;
  bool only_support_prefix_seek_;
  shared_ptr<Comparator> internal_comparator_;
  static std::unique_ptr<const SliceTransform> noop_transform;
  static std::unique_ptr<const SliceTransform> prefix_transform;
public:
  std::string ToString(const KVMap& data, const KVMap::const_iterator& it) {
    if (it == data.end()) {
      return "END";
    } else {
      return "'" + it->first + "->" + it->second + "'";
    }
  }
  
};

std::unique_ptr<const SliceTransform> Harness::prefix_transform(
    new FixedOrLessPrefixTransform(2));

TEST(DBTest, TailingIteratorPrefixSeek) {
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
  Options options = CurrentOptions();
  options.env = env_;
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
  options.create_if_missing = true;
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

<<<<<<< HEAD
  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
  // no prefix specified: 11 RAND I/Os
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
=======
  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
}

extern const uint64_t kPlainTableMagicNumber;
TEST(DBTest, TailingIteratorPrefixSeek) {
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
  Options options = CurrentOptions();
  options.env = env_;
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
  options.create_if_missing = true;
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

<<<<<<< HEAD
  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
  // no prefix specified: 11 RAND I/Os
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
=======
  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
}

TEST(DBTest, TailingIteratorPrefixSeek) {
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
  Options options = CurrentOptions();
  options.env = env_;
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
  options.create_if_missing = true;
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

<<<<<<< HEAD
  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
  // no prefix specified: 11 RAND I/Os
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
=======
  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
}

TEST(DBTest, TailingIteratorPrefixSeek) {
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
  Options options = CurrentOptions();
  options.env = env_;
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
  options.create_if_missing = true;
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

<<<<<<< HEAD
  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
  // no prefix specified: 11 RAND I/Os
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
=======
  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
}

static bool Between(uint64_t val, uint64_t low, uint64_t high) {
  bool result = (val >= low) && (val <= high);
  if (!result) {
    fprintf(stderr, "Value %llu is not in range [%llu, %llu]\n",
            (unsigned long long)(val),
            (unsigned long long)(low),
            (unsigned long long)(high));
  }
  return result;
}

// Tests against all kinds of tables
class GeneralTableTest {};

class BlockBasedTableTest {};

class PlainTableTest {};

TEST(DBTest, TailingIteratorPrefixSeek) {
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
  Options options = CurrentOptions();
  options.env = env_;
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
  options.create_if_missing = true;
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

<<<<<<< HEAD
  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
  // no prefix specified: 11 RAND I/Os
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
=======
  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
}

TEST(DBTest, TailingIteratorPrefixSeek) {
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
  Options options = CurrentOptions();
  options.env = env_;
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
  options.create_if_missing = true;
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

<<<<<<< HEAD
  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
  // no prefix specified: 11 RAND I/Os
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
=======
  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
}

static std::string RandomString(Random* rnd, int len) {
  std::string r;
  test::RandomString(rnd, len, &r);
  return r;
}

TEST(DBTest, TailingIteratorPrefixSeek) {
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
  Options options = CurrentOptions();
  options.env = env_;
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
  options.create_if_missing = true;
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

<<<<<<< HEAD
  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
  // no prefix specified: 11 RAND I/Os
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
=======
  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
}

TEST(DBTest, TailingIteratorPrefixSeek) {
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
  Options options = CurrentOptions();
  options.env = env_;
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
  options.create_if_missing = true;
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

<<<<<<< HEAD
  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
  // no prefix specified: 11 RAND I/Os
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
=======
  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
}

class BlockCacheProperties {
public:
  explicit BlockCacheProperties(Statistics* statistics) {
    block_cache_miss = statistics->getTickerCount(BLOCK_CACHE_MISS);
    block_cache_hit = statistics->getTickerCount(BLOCK_CACHE_HIT);
    index_block_cache_miss = statistics->getTickerCount(BLOCK_CACHE_INDEX_MISS);
    index_block_cache_hit = statistics->getTickerCount(BLOCK_CACHE_INDEX_HIT);
    data_block_cache_miss = statistics->getTickerCount(BLOCK_CACHE_DATA_MISS);
    data_block_cache_hit = statistics->getTickerCount(BLOCK_CACHE_DATA_HIT);
  }
  
  // Check if the fetched props matches the expected ones.
  void AssertEqual(
      long index_block_cache_miss,
      long index_block_cache_hit,
      long data_block_cache_miss,
      long data_block_cache_hit) const {
    ASSERT_EQ(index_block_cache_miss, this->index_block_cache_miss);
    ASSERT_EQ(index_block_cache_hit, this->index_block_cache_hit);
    ASSERT_EQ(data_block_cache_miss, this->data_block_cache_miss);
    ASSERT_EQ(data_block_cache_hit, this->data_block_cache_hit);
    ASSERT_EQ(
        index_block_cache_miss + data_block_cache_miss,
        this->block_cache_miss
    );
    ASSERT_EQ(
        index_block_cache_hit + data_block_cache_hit,
        this->block_cache_hit
    );
  }
  
private:
  long block_cache_miss = 0;
  long block_cache_hit = 0;
  long index_block_cache_miss = 0;
  long index_block_cache_hit = 0;
  long data_block_cache_miss = 0;
  long data_block_cache_hit = 0;
};

TEST(DBTest, TailingIteratorPrefixSeek) {
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
  Options options = CurrentOptions();
  options.env = env_;
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
  options.create_if_missing = true;
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

<<<<<<< HEAD
  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
  // no prefix specified: 11 RAND I/Os
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
=======
  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
}

TEST(DBTest, TailingIteratorPrefixSeek) {
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
  Options options = CurrentOptions();
  options.env = env_;
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
  options.create_if_missing = true;
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

<<<<<<< HEAD
  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
  // no prefix specified: 11 RAND I/Os
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
=======
  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
}

static void DoCompressionTest(CompressionType comp) {
  Random rnd(301);
  TableConstructor c(BytewiseComparator());
  std::string tmp;
  c.Add("k01", "hello");
  c.Add("k02", test::CompressibleString(&rnd, 0.25, 10000, &tmp));
  c.Add("k03", "hello3");
  c.Add("k04", test::CompressibleString(&rnd, 0.25, 10000, &tmp));
  std::vector<std::string> keys;
  KVMap kvmap;
  Options options;
  options.block_size = 1024;
  options.compression = comp;
  c.Finish(options, &keys, &kvmap);

  ASSERT_TRUE(Between(c.ApproximateOffsetOf("abc"),       0,      0));
  ASSERT_TRUE(Between(c.ApproximateOffsetOf("k01"),       0,      0));
  ASSERT_TRUE(Between(c.ApproximateOffsetOf("k02"),       0,      0));
  ASSERT_TRUE(Between(c.ApproximateOffsetOf("k03"),    2000,   3000));
  ASSERT_TRUE(Between(c.ApproximateOffsetOf("k04"),    2000,   3000));
  ASSERT_TRUE(Between(c.ApproximateOffsetOf("xyz"),    4000,   6100));
}

TEST(DBTest, TailingIteratorPrefixSeek) {
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
  Options options = CurrentOptions();
  options.env = env_;
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
  options.create_if_missing = true;
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

<<<<<<< HEAD
  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
  // no prefix specified: 11 RAND I/Os
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
=======
  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
}

TEST(DBTest, TailingIteratorPrefixSeek) {
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
  Options options = CurrentOptions();
  options.env = env_;
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
  options.create_if_missing = true;
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

<<<<<<< HEAD
  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
  // no prefix specified: 11 RAND I/Os
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
=======
  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
}

TEST(DBTest, TailingIteratorPrefixSeek) {
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
  Options options = CurrentOptions();
  options.env = env_;
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
  options.create_if_missing = true;
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

<<<<<<< HEAD
  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
  // no prefix specified: 11 RAND I/Os
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
=======
  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
}

TEST(DBTest, TailingIteratorPrefixSeek) {
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
  Options options = CurrentOptions();
  options.env = env_;
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
  options.create_if_missing = true;
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

<<<<<<< HEAD
  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
  // no prefix specified: 11 RAND I/Os
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
=======
  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
}

class MemTableTest {};

TEST(DBTest, TailingIteratorPrefixSeek) {
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
<<<<<<< HEAD
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  // db configs
  env_->count_random_reads_ = true;
||||||| 4e91f27c3
  ReadOptions ro = ReadOptions();
  int count;
  Slice prefix;
  Slice key;
  char buf[100];
  Iterator* iter;
  snprintf(buf, sizeof(buf), "03______:");
  prefix = Slice(buf, 8);
  key = Slice(buf, 9);
  auto prefix_extractor = NewFixedPrefixTransform(8);
  // db configs
  env_->count_random_reads_ = true;
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
=======
  ReadOptions read_options;
  read_options.tailing = true;
  read_options.prefix_seek = true;

  auto prefix_extractor = NewFixedPrefixTransform(2);

>>>>>>> 9dc29414
  Options options = CurrentOptions();
  options.env = env_;
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.no_block_cache = true;
  options.filter_policy = NewBloomFilterPolicy(10);
  options.prefix_extractor = NewFixedPrefixTransform(8);
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
||||||| 4e91f27c3
  options.no_block_cache = true;
  options.filter_policy =  NewBloomFilterPolicy(10);
  options.prefix_extractor = prefix_extractor;
  options.whole_key_filtering = false;
  options.disable_auto_compactions = true;
  options.max_background_compactions = 2;
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
=======
>>>>>>> 9dc29414
  options.create_if_missing = true;
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->Seek(key); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

<<<<<<< HEAD
  // no prefix specified: 11 RAND I/Os
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
||||||| 4e91f27c3
  // no prefix specified: 11 RAND I/Os
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  ro.prefix = &prefix;
  iter = db_->NewIterator(ro);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    assert(iter->key().starts_with(prefix));
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 2);

  // prefix specified, with blooms: 2 RAND I/Os
  // Seek
||||||| 4e91f27c3
<<<<<<< HEAD
  options.disable_seek_compaction = true;
  // Tricky: options.prefix_extractor will be released by
  // NewHashSkipListRepFactory after use.
  options.memtable_factory.reset(
      NewHashSkipListRepFactory(options.prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
||||||| 4e91f27c3
  options.disable_seek_compaction = true;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));

  // prefix specified, with blooms: 2 RAND I/Os
  // SeekToFirst
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
>>>>>>> 9dc29414
  DestroyAndReopen(&options);
  PrefixScanInit(this);
  count = 0;
  env_->random_read_counter_.Reset();
  iter = db_->NewIterator(ReadOptions());
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    if (! iter->key().starts_with(prefix)) {
      break;
    }
    count++;
  }
  ASSERT_OK(iter->status());
  delete iter;
  ASSERT_EQ(count, 2);
  ASSERT_EQ(env_->random_read_counter_.Read(), 11);
  Close();
  delete options.filter_policy;
=======
  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
=======
  options.disable_auto_compactions = true;
  options.prefix_extractor = prefix_extractor;
  options.memtable_factory.reset(NewHashSkipListRepFactory(prefix_extractor));
  DestroyAndReopen(&options);

  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  ASSERT_OK(db_->Put(WriteOptions(), "0101", "test"));

  dbfull()->TEST_FlushMemTable();

  ASSERT_OK(db_->Put(WriteOptions(), "0202", "test"));

  // Seek(0102) shouldn't find any records since 0202 has a different prefix
  iter->Seek("0102");
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("0202");
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "0202");

  iter->Next();
  ASSERT_TRUE(!iter->Valid());
>>>>>>> 9dc29414
}

} // namespace rocksdb

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--benchmark") {
    rocksdb::BM_LogAndApply(1000, 1);
    rocksdb::BM_LogAndApply(1000, 100);
    rocksdb::BM_LogAndApply(1000, 10000);
    rocksdb::BM_LogAndApply(100, 100000);
    return 0;
  }

  return rocksdb::test::RunAllTests();
}