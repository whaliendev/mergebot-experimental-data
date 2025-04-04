#include <algorithm>
#include "leveldb/db.h"
#include "leveldb/filter_policy.h"
#include "db/db_impl.h"
#include "db/filename.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include "leveldb/cache.h"
#include "leveldb/env.h"
#include "leveldb/table.h"
#include "util/hash.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/testharness.h"
#include "util/testutil.h"
namespace leveldb {
static std::string RandomString(Random* rnd, int len) {
  std::string r;
  test::RandomString(rnd, len, &r);
  return r;
}
namespace {
class AtomicCounter {
private:
  port::Mutex mu_;
  int count_;
public:
  AtomicCounter() : count_(0) { }
  void Increment() {
    MutexLock l(&mu_);
    count_++;
  }
  int Read() {
    MutexLock l(&mu_);
    return count_;
  }
  void Reset() {
    MutexLock l(&mu_);
    count_ = 0;
  }
};
}
class SpecialEnv : public EnvWrapper {
public:
  port::AtomicPointer delay_sstable_sync_;
  port::AtomicPointer no_space_;
  port::AtomicPointer non_writable_;
  bool count_random_reads_;
  AtomicCounter random_read_counter_;
  AtomicCounter sleep_counter_;
  explicit SpecialEnv(Env* base) : EnvWrapper(base) {
    delay_sstable_sync_.Release_Store(NULL);
    no_space_.Release_Store(NULL);
    non_writable_.Release_Store(NULL);
    count_random_reads_ = false;
  }
  Status NewWritableFile(const std::string& f, WritableFile** r) {
    class SSTableFile : public WritableFile {
     private:
      SpecialEnv* env_;
      WritableFile* base_;
     public:
      SSTableFile(SpecialEnv* env, WritableFile* base)
          : env_(env),
            base_(base) {
      }
      ~SSTableFile() { delete base_; }
      Status Append(const Slice& data) {
        if (env_->no_space_.Acquire_Load() != NULL) {
          return Status::OK();
        } else {
          return base_->Append(data);
        }
      }
      Status Close() { return base_->Close(); }
      Status Flush() { return base_->Flush(); }
      Status Sync() {
        while (env_->delay_sstable_sync_.Acquire_Load() != NULL) {
          env_->SleepForMicroseconds(100000);
        }
        return base_->Sync();
      }
    };
    if (non_writable_.Acquire_Load() != NULL) {
       return Status::IOError("simulated write error");
     }
    Status s = target()->NewWritableFile(f, r);
    if (s.ok()) {
      if (strstr(f.c_str(), ".sst") != NULL) {
        *r = new SSTableFile(this, *r);
      }
    }
    return s;
  }
  Status NewRandomAccessFile(const std::string& f, RandomAccessFile** r) {
    class CountingFile : public RandomAccessFile {
     private:
      RandomAccessFile* target_;
      AtomicCounter* counter_;
     public:
      CountingFile(RandomAccessFile* target, AtomicCounter* counter)
          : target_(target), counter_(counter) {
      }
      virtual ~CountingFile() { delete target_; }
      virtual Status Read(uint64_t offset, size_t n, Slice* result,
                          char* scratch) const {
        counter_->Increment();
        return target_->Read(offset, n, result, scratch);
      }
    };
    Status s = target()->NewRandomAccessFile(f, r);
    if (s.ok() && count_random_reads_) {
      *r = new CountingFile(*r, &random_read_counter_);
    }
    return s;
  }
  virtual void SleepForMicroseconds(int micros) {
    sleep_counter_.Increment();
    target()->SleepForMicroseconds(micros);
  }
};
class DBTest {
private:
  const FilterPolicy* filter_policy_;
  enum OptionConfig {
    kDefault,
    kFilter,
    kUncompressed,
    kNumLevel_3,
    kDBLogDir,
    kEnd
  };
  int option_config_;
public:
  std::string dbname_;
  SpecialEnv* env_;
  DB* db_;
  Options last_options_;
  DBTest() : option_config_(kDefault),
             env_(new SpecialEnv(Env::Default())) {
    filter_policy_ = NewBloomFilterPolicy(10);
    dbname_ = test::TmpDir() + "/db_test";
    DestroyDB(dbname_, Options());
    db_ = NULL;
    Reopen();
  }
  ~DBTest() {
    delete db_;
    DestroyDB(dbname_, Options());
    delete env_;
    delete filter_policy_;
  }{
    delete db_;
    DestroyDB(dbname_, Options());
    delete env_;
    delete filter_policy_;
  }
  bool ChangeOptions() {
    option_config_++;
    if (option_config_ >= kEnd) {
      return false;
    } else {
      DestroyAndReopen();
      return true;
    }
  }
  Options CurrentOptions() {
    Options options;
    switch (option_config_) {
      case kFilter:
        options.filter_policy = filter_policy_;
        break;
      case kUncompressed:
        options.compression = kNoCompression;
        break;
      case kNumLevel_3:
        options.num_levels = 3;
        break;
      case kDBLogDir:
        options.db_log_dir = test::TmpDir();
        break;
      default:
        break;
    }
    return options;
  }
  DBImpl* dbfull() {
    return reinterpret_cast<DBImpl*>(db_);
  }
  void Reopen(Options* options = NULL) {
    ASSERT_OK(TryReopen(options));
  }
  void Close() {
    delete db_;
    db_ = NULL;
  }
  void DestroyAndReopen(Options* options = NULL) {
    delete db_;
    db_ = NULL;
    DestroyDB(dbname_, Options());
    ASSERT_OK(TryReopen(options));
  }
  Status PureReopen(Options* options, DB** db) {
    return DB::Open(*options, dbname_, db);
  }
  Status TryReopen(Options* options) {
    delete db_;
    db_ = NULL;
    Options opts;
    if (options != NULL) {
      opts = *options;
    } else {
      opts = CurrentOptions();
      opts.create_if_missing = true;
    }
    last_options_ = opts;
    return DB::Open(opts, dbname_, &db_);
  }
  Status Put(const std::string& k, const std::string& v) {
    return db_->Put(WriteOptions(), k, v);
  }
  Status Delete(const std::string& k) {
    return db_->Delete(WriteOptions(), k);
  }
  std::string Get(const std::string& k, const Snapshot* snapshot = NULL) {
    ReadOptions options;
    options.snapshot = snapshot;
    std::string result;
    Status s = db_->Get(options, k, &result);
    if (s.IsNotFound()) {
      result = "NOT_FOUND";
    } else if (!s.ok()) {
      result = s.ToString();
    }
    return result;
  }
  std::string Contents() {
    std::vector<std::string> forward;
    std::string result;
    Iterator* iter = db_->NewIterator(ReadOptions());
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      std::string s = IterStatus(iter);
      result.push_back('(');
      result.append(s);
      result.push_back(')');
      forward.push_back(s);
    }
    int matched = 0;
    for (iter->SeekToLast(); iter->Valid(); iter->Prev()) {
      ASSERT_LT(matched, forward.size());
      ASSERT_EQ(IterStatus(iter), forward[forward.size() - matched - 1]);
      matched++;
    }
    ASSERT_EQ(matched, forward.size());
    delete iter;
    return result;
  }
  std::string AllEntriesFor(const Slice& user_key) {
    Iterator* iter = dbfull()->TEST_NewInternalIterator();
    InternalKey target(user_key, kMaxSequenceNumber, kTypeValue);
    iter->Seek(target.Encode());
    std::string result;
    if (!iter->status().ok()) {
      result = iter->status().ToString();
    } else {
      result = "[ ";
      bool first = true;
      while (iter->Valid()) {
        ParsedInternalKey ikey;
        if (!ParseInternalKey(iter->key(), &ikey)) {
          result += "CORRUPTED";
        } else {
          if (last_options_.comparator->Compare(ikey.user_key, user_key) != 0) {
            break;
          }
          if (!first) {
            result += ", ";
          }
          first = false;
          switch (ikey.type) {
            case kTypeValue:
              result += iter->value().ToString();
              break;
            case kTypeDeletion:
              result += "DEL";
              break;
          }
        }
        iter->Next();
      }
      if (!first) {
        result += " ";
      }
      result += "]";
    }
    delete iter;
    return result;
  }
  int NumTableFilesAtLevel(int level) {
    std::string property;
    ASSERT_TRUE(
        db_->GetProperty("leveldb.num-files-at-level" + NumberToString(level),
                         &property));
    return atoi(property.c_str());
  }
  int TotalTableFiles() {
    int result = 0;
    for (int level = 0; level < db_->NumberLevels(); level++) {
      result += NumTableFilesAtLevel(level);
    }
    return result;
  }
  std::string FilesPerLevel() {
    std::string result;
    int last_non_zero_offset = 0;
    for (int level = 0; level < db_->NumberLevels(); level++) {
      int f = NumTableFilesAtLevel(level);
      char buf[100];
      snprintf(buf, sizeof(buf), "%s%d", (level ? "," : ""), f);
      result += buf;
      if (f > 0) {
        last_non_zero_offset = result.size();
      }
    }
    result.resize(last_non_zero_offset);
    return result;
  }
  int CountFiles() {
    std::vector<std::string> files;
    env_->GetChildren(dbname_, &files);
    return static_cast<int>(files.size());
  }
  int CountLiveFiles() {
    std::vector<std::string> files;
    uint64_t manifest_file_size;
    db_->GetLiveFiles(files, &manifest_file_size);
    return files.size();
  }
  uint64_t Size(const Slice& start, const Slice& limit) {
    Range r(start, limit);
    uint64_t size;
    db_->GetApproximateSizes(&r, 1, &size);
    return size;
  }
  void Compact(const Slice& start, const Slice& limit) {
    db_->CompactRange(&start, &limit);
  }
  void MakeTables(int n, const std::string& small, const std::string& large) {
    for (int i = 0; i < n; i++) {
      Put(small, "begin");
      Put(large, "end");
      dbfull()->TEST_CompactMemTable();
    }
  }
  void FillLevels(const std::string& smallest, const std::string& largest) {
    MakeTables(db_->NumberLevels(), smallest, largest);
  }
  void DumpFileCounts(const char* label) {
    fprintf(stderr, "---\n%s:\n", label);
    fprintf(stderr, "maxoverlap: %lld\n",
            static_cast<long long>(
                dbfull()->TEST_MaxNextLevelOverlappingBytes()));
    for (int level = 0; level < db_->NumberLevels(); level++) {
      int num = NumTableFilesAtLevel(level);
      if (num > 0) {
        fprintf(stderr, "  level %3d : %d files\n", level, num);
      }
    }
  }
  std::string DumpSSTableList() {
    std::string property;
    db_->GetProperty("leveldb.sstables", &property);
    return property;
  }
  std::string IterStatus(Iterator* iter) {
    std::string result;
    if (iter->Valid()) {
      result = iter->key().ToString() + "->" + iter->value().ToString();
    } else {
      result = "(invalid)";
    }
    return result;
  }
};
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
static std::string Key(int i) {
  char buf[100];
  snprintf(buf, sizeof(buf), "key%06d", i);
  return std::string(buf);
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
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
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
namespace {
static const int kNumThreads = 4;
static const int kTestSeconds = 10;
static const int kNumKeys = 1000;
struct MTState {
  DBTest* test;
  port::AtomicPointer stop;
  port::AtomicPointer thread_done[kNumThreads];
  port::AtomicPointer thread_done[kNumThreads];
};
struct MTThread {
  MTState* state;
  int id;
};
static void MTThreadBody(void* arg) {
  MTThread* t = reinterpret_cast<MTThread*>(arg);
  int id = t->id;
  DB* db = t->state->test->db_;
  uintptr_t counter = 0;
  fprintf(stderr, "... starting thread %d\n", id);
  Random rnd(1000 + id);
  std::string value;
  char valbuf[1500];
  while (t->state->stop.Acquire_Load() == NULL) {
    t->state->counter[id].Release_Store(reinterpret_cast<void*>(counter));
    int key = rnd.Uniform(kNumKeys);
    char keybuf[20];
    snprintf(keybuf, sizeof(keybuf), "%016d", key);
    if (rnd.OneIn(2)) {
      snprintf(valbuf, sizeof(valbuf), "%d.%d.%-1000d",
               key, id, static_cast<int>(counter));
      ASSERT_OK(db->Put(WriteOptions(), Slice(keybuf), Slice(valbuf)));
    } else {
      Status s = db->Get(ReadOptions(), Slice(keybuf), &value);
      if (s.IsNotFound()) {
      } else {
        ASSERT_OK(s);
        int k, w, c;
        ASSERT_EQ(3, sscanf(value.c_str(), "%d.%d.%d", &k, &w, &c)) << value;
        ASSERT_EQ(k, key);
        ASSERT_GE(w, 0);
        ASSERT_LT(w, kNumThreads);
        ASSERT_LE(c, reinterpret_cast<uintptr_t>(
            t->state->counter[w].Acquire_Load()));
      }
    }
    counter++;
  }
  t->state->thread_done[id].Release_Store(t);
  fprintf(stderr, "... stopping thread %d after %d ops\n", id, int(counter));
}
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
namespace {
typedef std::map<std::string, std::string> KVMap;
}
class ModelDB: public DB {
public:
  class ModelSnapshot : public Snapshot {
   public:
    KVMap map_;
  };
  explicit ModelDB(const Options& options): options_(options) { }
  ~ModelDB() { }
  virtual Status Put(const WriteOptions& o, const Slice& k, const Slice& v) {
    return DB::Put(o, k, v);
  }
  virtual Status Delete(const WriteOptions& o, const Slice& key) {
    return DB::Delete(o, key);
  }
  virtual Status Get(const ReadOptions& options,
                     const Slice& key, std::string* value) {
    assert(false);
    return Status::NotFound(key);
  }
  virtual Iterator* NewIterator(const ReadOptions& options) {
    if (options.snapshot == NULL) {
      KVMap* saved = new KVMap;
      *saved = map_;
      return new ModelIter(saved, true);
    } else {
      const KVMap* snapshot_state =
          &(reinterpret_cast<const ModelSnapshot*>(options.snapshot)->map_);
      return new ModelIter(snapshot_state, false);
    }
  }
  virtual const Snapshot* GetSnapshot() {
    ModelSnapshot* snapshot = new ModelSnapshot;
    snapshot->map_ = map_;
    return snapshot;
  }
  virtual void ReleaseSnapshot(const Snapshot* snapshot) {
    delete reinterpret_cast<const ModelSnapshot*>(snapshot);
  }
  virtual Status Write(const WriteOptions& options, WriteBatch* batch) {
    class Handler : public WriteBatch::Handler {
     public:
      KVMap* map_;
      virtual void Put(const Slice& key, const Slice& value) {
        (*map_)[key.ToString()] = value.ToString();
      }
      virtual void Delete(const Slice& key) {
        map_->erase(key.ToString());
      }
    };
    Handler handler;
    handler.map_ = &map_;
    return batch->Iterate(&handler);
  }
  virtual bool GetProperty(const Slice& property, std::string* value) {
    return false;
  }
  virtual void GetApproximateSizes(const Range* r, int n, uint64_t* sizes) {
    for (int i = 0; i < n; i++) {
      sizes[i] = 0;
    }
  }
  virtual void CompactRange(const Slice* start, const Slice* end) {
  }
  virtual int NumberLevels()
  {
  return 1;
  }
  virtual int MaxMemCompactionLevel()
  {
  return 1;
  }
  virtual int Level0StopWriteTrigger()
  {
  return -1;
  }
  virtual Status Flush(const leveldb::FlushOptions& options) {
    Status ret;
    return ret;
  }
  virtual Status DisableFileDeletions() {
    return Status::OK();
  }
  virtual Status EnableFileDeletions() {
    return Status::OK();
  }
  virtual Status GetLiveFiles(std::vector<std::string>&, uint64_t* size) {
    return Status::OK();
  }
private:
  class ModelIter: public Iterator {
   public:
    ModelIter(const KVMap* map, bool owned)
        : map_(map), owned_(owned), iter_(map_->end()) {
    }
    ~ModelIter() {
      if (owned_) delete map_;
    }
    virtual bool Valid() const { return iter_ != map_->end(); }
    virtual void SeekToFirst() { iter_ = map_->begin(); }
    virtual void SeekToLast() {
      if (map_->empty()) {
        iter_ = map_->end();
      } else {
        iter_ = map_->find(map_->rbegin()->first);
      }
    }
    virtual void Seek(const Slice& k) {
      iter_ = map_->lower_bound(k.ToString());
    }
    virtual void Next() { ++iter_; }
    virtual void Prev() { --iter_; }
    virtual Slice key() const { return iter_->first; }
    virtual Slice value() const { return iter_->second; }
    virtual Status status() const { return Status::OK(); }
   private:
    const KVMap* const map_;
    const bool owned_;
    KVMap::const_iterator iter_;
  };
  const Options options_;
  KVMap map_;
};
static std::string RandomKey(Random* rnd) {
  int len = (rnd->OneIn(3)
             ? 1
             : (rnd->OneIn(100) ? rnd->Skewed(10) : rnd->Uniform(10)));
  return test::RandomKey(rnd, len);
}
static bool CompareIterators(int step,
                             DB* model,
                             DB* db,
                             const Snapshot* model_snap,
                             const Snapshot* db_snap) {
  ReadOptions options;
  options.snapshot = model_snap;
  Iterator* miter = model->NewIterator(options);
  options.snapshot = db_snap;
  Iterator* dbiter = db->NewIterator(options);
  bool ok = true;
  int count = 0;
  for (miter->SeekToFirst(), dbiter->SeekToFirst();
       ok && miter->Valid() && dbiter->Valid();
       miter->Next(), dbiter->Next()) {
    count++;
    if (miter->key().compare(dbiter->key()) != 0) {
      fprintf(stderr, "step %d: Key mismatch: '%s' vs. '%s'\n",
              step,
              EscapeString(miter->key()).c_str(),
              EscapeString(dbiter->key()).c_str());
      ok = false;
      break;
    }
    if (miter->value().compare(dbiter->value()) != 0) {
      fprintf(stderr, "step %d: Value mismatch for key '%s': '%s' vs. '%s'\n",
              step,
              EscapeString(miter->key()).c_str(),
              EscapeString(miter->value()).c_str(),
              EscapeString(miter->value()).c_str());
      ok = false;
    }
  }
  if (ok) {
    if (miter->Valid() != dbiter->Valid()) {
      fprintf(stderr, "step %d: Mismatch at end of iterators: %d vs. %d\n",
              step, miter->Valid(), dbiter->Valid());
      ok = false;
    }
  }
  fprintf(stderr, "%d entries compared: ok=%d\n", count, ok);
  delete miter;
  delete dbiter;
  return ok;
}
TEST(DBTest, Randomized) {
  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      int p = rnd.Uniform(100);
      if (p < 45) {
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));
      } else if (p < 90) {
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));
      } else {
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }
      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());
}
std::string MakeKey(unsigned int num) {
  char buf[30];
  snprintf(buf, sizeof(buf), "%016u", num);
  return std::string(buf);
}
void BM_LogAndApply(int iters, int num_base_files) {
  std::string dbname = test::TmpDir() + "/leveldb_test_benchmark";
  DestroyDB(dbname, Options());
  DB* db = NULL;
  Options opts;
  opts.create_if_missing = true;
  Status s = DB::Open(opts, dbname, &db);
  ASSERT_OK(s);
  ASSERT_TRUE(db != NULL);
  delete db;
  db = NULL;
  Env* env = Env::Default();
  port::Mutex mu;
  MutexLock l(&mu);
  InternalKeyComparator cmp(BytewiseComparator());
  Options options;
  VersionSet vset(dbname, &options, NULL, &cmp);
  ASSERT_OK(vset.Recover());
  VersionEdit vbase(vset.NumberLevels());
  uint64_t fnum = 1;
  for (int i = 0; i < num_base_files; i++) {
    InternalKey start(MakeKey(2*fnum), 1, kTypeValue);
    InternalKey limit(MakeKey(2*fnum+1), 1, kTypeDeletion);
    vbase.AddFile(2, fnum++, 1 , start, limit);
  }
  ASSERT_OK(vset.LogAndApply(&vbase, &mu));
  uint64_t start_micros = env->NowMicros();
  for (int i = 0; i < iters; i++) {
    VersionEdit vedit(vset.NumberLevels());
    vedit.DeleteFile(2, fnum);
    InternalKey start(MakeKey(2*fnum), 1, kTypeValue);
    InternalKey limit(MakeKey(2*fnum+1), 1, kTypeDeletion);
    vedit.AddFile(2, fnum++, 1 , start, limit);
    vset.LogAndApply(&vedit, &mu);
  }
  uint64_t stop_micros = env->NowMicros();
  unsigned int us = stop_micros - start_micros;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", num_base_files);
  fprintf(stderr,
          "BM_LogAndApply/%-6s   %8d iters : %9u us (%7.0f us / iter)\n",
          buf, iters, us, ((float)us) / iters);
}
}
int main(int argc, char** argv) {
  FLAGS_write_buffer_size = leveldb::Options().write_buffer_size;
  FLAGS_max_write_buffer_number = leveldb::Options().max_write_buffer_number;
  FLAGS_open_files = leveldb::Options().max_open_files;
  FLAGS_max_background_compactions =
    leveldb::Options().max_background_compactions;
  FLAGS_block_size = leveldb::Options().block_size;
  std::string default_db_path;
  for (int i = 1; i < argc; i++) {
    double d;
    int n;
    long l;
    char junk;
    char hdfsname[2048];
    if (leveldb::Slice(argv[i]).starts_with("--benchmarks=")) {
      FLAGS_benchmarks = argv[i] + strlen("--benchmarks=");
    } else if (sscanf(argv[i], "--compression_ratio=%lf%c", &d, &junk) == 1) {
      FLAGS_compression_ratio = d;
    } else if (sscanf(argv[i], "--histogram=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_histogram = n;
    } else if (sscanf(argv[i], "--use_existing_db=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_use_existing_db = n;
    } else if (sscanf(argv[i], "--num=%ld%c", &l, &junk) == 1) {
      FLAGS_num = l;
    } else if (sscanf(argv[i], "--reads=%d%c", &n, &junk) == 1) {
      FLAGS_reads = n;
    } else if (sscanf(argv[i], "--threads=%d%c", &n, &junk) == 1) {
      FLAGS_threads = n;
    } else if (sscanf(argv[i], "--value_size=%d%c", &n, &junk) == 1) {
      FLAGS_value_size = n;
    } else if (sscanf(argv[i], "--write_buffer_size=%d%c", &n, &junk) == 1) {
      FLAGS_write_buffer_size = n;
    } else if (sscanf(argv[i], "--max_write_buffer_number=%d%c", &n, &junk) == 1) {
      FLAGS_max_write_buffer_number = n;
    } else if (sscanf(argv[i], "--max_background_compactions=%d%c", &n, &junk) == 1) {
      FLAGS_max_background_compactions = n;
    } else if (sscanf(argv[i], "--cache_size=%ld%c", &l, &junk) == 1) {
      FLAGS_cache_size = l;
    } else if (sscanf(argv[i], "--block_size=%d%c", &n, &junk) == 1) {
      FLAGS_block_size = n;
    } else if (sscanf(argv[i], "--cache_numshardbits=%d%c", &n, &junk) == 1) {
      if (n < 20) {
        FLAGS_cache_numshardbits = n;
      } else {
        fprintf(stderr, "The cache cannot be sharded into 2**%d pieces\n", n);
        exit(1);
      }
    } else if (sscanf(argv[i], "--bloom_bits=%d%c", &n, &junk) == 1) {
      FLAGS_bloom_bits = n;
    } else if (sscanf(argv[i], "--open_files=%d%c", &n, &junk) == 1) {
      FLAGS_open_files = n;
    } else if (strncmp(argv[i], "--db=", 5) == 0) {
      FLAGS_db = argv[i] + 5;
    } else if (sscanf(argv[i], "--verify_checksum=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_verify_checksum = n;
    } else if (sscanf(argv[i], "--bufferedio=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      useOsBuffer = n;
    } else if (sscanf(argv[i], "--mmap_read=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      useMmapRead = n;
    } else if (sscanf(argv[i], "--mmap_write=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      useMmapWrite = n;
    } else if (sscanf(argv[i], "--readahead=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      useFsReadAhead = n;
    } else if (sscanf(argv[i], "--statistics=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      if (n == 1) {
        dbstats = new leveldb::DBStatistics();
        FLAGS_statistics = true;
      }
    } else if (sscanf(argv[i], "--writes=%d%c", &n, &junk) == 1) {
      FLAGS_writes = n;
    } else if (sscanf(argv[i], "--sync=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_sync = n;
    } else if (sscanf(argv[i], "--readwritepercent=%d%c", &n, &junk) == 1 &&
               n > 0 && n < 100) {
      FLAGS_readwritepercent = n;
    } else if (sscanf(argv[i], "--disable_data_sync=%d%c", &n, &junk) == 1 &&
        (n == 0 || n == 1)) {
      FLAGS_disable_data_sync = n;
    } else if (sscanf(argv[i], "--use_fsync=%d%c", &n, &junk) == 1 &&
        (n == 0 || n == 1)) {
      FLAGS_use_fsync = n;
    } else if (sscanf(argv[i], "--disable_wal=%d%c", &n, &junk) == 1 &&
        (n == 0 || n == 1)) {
      FLAGS_disable_wal = n;
    } else if (sscanf(argv[i], "--hdfs=%s", hdfsname) == 1) {
      FLAGS_env = new leveldb::HdfsEnv(hdfsname);
    } else if (sscanf(argv[i], "--num_levels=%d%c",
        &n, &junk) == 1) {
      FLAGS_num_levels = n;
    } else if (sscanf(argv[i], "--target_file_size_base=%d%c",
        &n, &junk) == 1) {
      FLAGS_target_file_size_base = n;
    } else if ( sscanf(argv[i], "--target_file_size_multiplier=%d%c",
        &n, &junk) == 1) {
      FLAGS_target_file_size_multiplier = n;
    } else if (
        sscanf(argv[i], "--max_bytes_for_level_base=%d%c", &n, &junk) == 1) {
      FLAGS_max_bytes_for_level_base = n;
    } else if (sscanf(argv[i], "--max_bytes_for_level_multiplier=%d%c",
        &n, &junk) == 1) {
      FLAGS_max_bytes_for_level_multiplier = n;
    } else if (sscanf(argv[i],"--level0_stop_writes_trigger=%d%c",
        &n, &junk) == 1) {
      FLAGS_level0_stop_writes_trigger = n;
    } else if (sscanf(argv[i],"--level0_slowdown_writes_trigger=%d%c",
        &n, &junk) == 1) {
      FLAGS_level0_slowdown_writes_trigger = n;
    } else if (sscanf(argv[i],"--level0_file_num_compaction_trigger=%d%c",
        &n, &junk) == 1) {
      FLAGS_level0_file_num_compaction_trigger = n;
    } else if (strncmp(argv[i], "--compression_type=", 19) == 0) {
      const char* ctype = argv[i] + 19;
      if (!strcasecmp(ctype, "none"))
        FLAGS_compression_type = leveldb::kNoCompression;
      else if (!strcasecmp(ctype, "snappy"))
        FLAGS_compression_type = leveldb::kSnappyCompression;
      else if (!strcasecmp(ctype, "zlib"))
        FLAGS_compression_type = leveldb::kZlibCompression;
      else if (!strcasecmp(ctype, "bzip2"))
        FLAGS_compression_type = leveldb::kBZip2Compression;
      else {
        fprintf(stdout, "Cannot parse %s\n", argv[i]);
      }
    } else if (sscanf(argv[i], "--disable_seek_compaction=%d%c", &n, &junk) == 1
        && (n == 0 || n == 1)) {
      FLAGS_disable_seek_compaction = n;
    } else if (sscanf(argv[i], "--delete_obsolete_files_period_micros=%ld%c",
                      &l, &junk) == 1) {
      FLAGS_delete_obsolete_files_period_micros = l;
    } else if (sscanf(argv[i], "--stats_interval=%d%c", &n, &junk) == 1 &&
               n >= 0 && n < 2000000000) {
      FLAGS_stats_interval = n;
    } else {
      fprintf(stderr, "Invalid flag '%s'\n", argv[i]);
      exit(1);
    }
  }
  FLAGS_env->SetBackgroundThreads(FLAGS_max_background_compactions);
  if (FLAGS_db == NULL) {
      leveldb::Env::Default()->GetTestDirectory(&default_db_path);
      default_db_path += "/dbbench";
      FLAGS_db = default_db_path.c_str();
  }
  leveldb::Benchmark benchmark;
  benchmark.Run();
  return 0;
}
