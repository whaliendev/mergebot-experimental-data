#include "rocksdb/db.h"
#include <memory>
#include "db/memtable.h"
#include "db/write_batch_internal.h"
#include "rocksdb/env.h"
#include "rocksdb/memtablerep.h"
#include "util/logging.h"
#include "util/testharness.h"
namespace rocksdb {
static std::string PrintContents(WriteBatch* b) {
  InternalKeyComparator cmp(BytewiseComparator());
  auto factory = std::make_shared<SkipListFactory>();
  Options options;
  options.memtable_factory = factory;
  MemTable* mem = new MemTable(cmp, ColumnFamilyOptions(options));
  mem->Ref();
  std::string state;
  ColumnFamilyMemTablesDefault cf_mems_default(mem, &options);
  Status s = WriteBatchInternal::InsertInto(b, &cf_mems_default);
  int count = 0;
  Iterator* iter = mem->NewIterator();
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ParsedInternalKey ikey;
    memset((void *)&ikey, 0, sizeof(ikey));
    ASSERT_TRUE(ParseInternalKey(iter->key(), &ikey));
    switch (ikey.type) {
      case kTypeValue:
        state.append("Put(");
        state.append(ikey.user_key.ToString());
        state.append(", ");
        state.append(iter->value().ToString());
        state.append(")");
        count++;
        break;
      case kTypeMerge:
        state.append("Merge(");
        state.append(ikey.user_key.ToString());
        state.append(", ");
        state.append(iter->value().ToString());
        state.append(")");
        count++;
        break;
      case kTypeDeletion:
        state.append("Delete(");
        state.append(ikey.user_key.ToString());
        state.append(")");
        count++;
        break;
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
    state.append("@");
    state.append(NumberToString(ikey.sequence));
  }
  delete iter;
  if (!s.ok()) {
    state.append(s.ToString());
  } else if (count != WriteBatchInternal::Count(b)) {
    state.append("CountMismatch()");
  }
  delete mem->Unref();
  return state;
}
class WriteBatchTest { };
TEST(WriteBatchTest, Empty) {
  WriteBatch batch;
  ASSERT_EQ("", PrintContents(&batch));
  ASSERT_EQ(0, WriteBatchInternal::Count(&batch));
  ASSERT_EQ(0, batch.Count());
}
TEST(WriteBatchTest, Multiple) {
  WriteBatch batch;
  batch.Put(Slice("foo"), Slice("bar"));
  batch.Delete(Slice("box"));
  batch.Put(Slice("baz"), Slice("boo"));
  WriteBatchInternal::SetSequence(&batch, 100);
  ASSERT_EQ(100U, WriteBatchInternal::Sequence(&batch));
  ASSERT_EQ(3, WriteBatchInternal::Count(&batch));
  ASSERT_EQ("Put(baz, boo)@102"
            "Delete(box)@101"
            "Put(foo, bar)@100",
            PrintContents(&batch));
  ASSERT_EQ(3, batch.Count());
}
TEST(WriteBatchTest, Corruption) {
  WriteBatch batch;
  batch.Put(Slice("foo"), Slice("bar"));
  batch.Delete(Slice("box"));
  WriteBatchInternal::SetSequence(&batch, 200);
  Slice contents = WriteBatchInternal::Contents(&batch);
  WriteBatchInternal::SetContents(&batch,
                                  Slice(contents.data(),contents.size()-1));
  ASSERT_EQ("Put(foo, bar)@200"
            "Corruption: bad WriteBatch Delete",
            PrintContents(&batch));
}
TEST(WriteBatchTest, Append) {
  WriteBatch b1, b2;
  WriteBatchInternal::SetSequence(&b1, 200);
  WriteBatchInternal::SetSequence(&b2, 300);
  WriteBatchInternal::Append(&b1, &b2);
  ASSERT_EQ("",
            PrintContents(&b1));
  ASSERT_EQ(0, b1.Count());
  b2.Put("a", "va");
  WriteBatchInternal::Append(&b1, &b2);
  ASSERT_EQ("Put(a, va)@200",
            PrintContents(&b1));
  ASSERT_EQ(1, b1.Count());
  b2.Clear();
  b2.Put("b", "vb");
  WriteBatchInternal::Append(&b1, &b2);
  ASSERT_EQ("Put(a, va)@200"
            "Put(b, vb)@201",
            PrintContents(&b1));
  ASSERT_EQ(2, b1.Count());
  b2.Delete("foo");
  WriteBatchInternal::Append(&b1, &b2);
  ASSERT_EQ("Put(a, va)@200"
            "Put(b, vb)@202"
            "Put(b, vb)@201"
            "Delete(foo)@203",
            PrintContents(&b1));
  ASSERT_EQ(4, b1.Count());
}
namespace {
  struct TestHandler : public WriteBatch::Handler {
    std::string seen;
    virtual void PutCF(uint32_t column_family_id, const Slice& key,
                       const Slice& value) {
      if (column_family_id == 0) {
        seen += "Put(" + key.ToString() + ", " + value.ToString() + ")";
      } else {
        seen += "PutCF(" + std::to_string(column_family_id) + ", " +
                key.ToString() + ", " + value.ToString() + ")";
      }
    }
    virtual void MergeCF(uint32_t column_family_id, const Slice& key,
                         const Slice& value) {
      if (column_family_id == 0) {
        seen += "Merge(" + key.ToString() + ", " + value.ToString() + ")";
      } else {
        seen += "MergeCF(" + std::to_string(column_family_id) + ", " +
                key.ToString() + ", " + value.ToString() + ")";
      }
    }
    virtual void LogData(const Slice& blob) {
      seen += "LogData(" + blob.ToString() + ")";
    }
    virtual void DeleteCF(uint32_t column_family_id, const Slice& key) {
      if (column_family_id == 0) {
        seen += "Delete(" + key.ToString() + ")";
      } else {
        seen += "DeleteCF(" + std::to_string(column_family_id) + ", " +
                key.ToString() + ")";
      }
    }
  };
}
TEST(WriteBatchTest, Blob) {
  WriteBatch batch;
  batch.Put(Slice("k1"), Slice("v1"));
  batch.Put(Slice("k2"), Slice("v2"));
  batch.Put(Slice("k3"), Slice("v3"));
  batch.PutLogData(Slice("blob1"));
  batch.Delete(Slice("k2"));
  batch.PutLogData(Slice("blob2"));
  batch.Merge(Slice("foo"), Slice("bar"));
  ASSERT_EQ(5, batch.Count());
  ASSERT_EQ("Merge(foo, bar)@4"
            "Put(k1, v1)@0"
            "Delete(k2)@3"
            "Put(k2, v2)@1"
            "Put(k3, v3)@2",
            PrintContents(&batch));
  TestHandler handler;
  batch.Iterate(&handler);
  ASSERT_EQ(
            "Put(k1, v1)"
            "Put(k2, v2)"
            "Put(k3, v3)"
            "LogData(blob1)"
            "Delete(k2)"
            "LogData(blob2)"
            "Merge(foo, bar)",
            handler.seen);
}
TEST(WriteBatchTest, Continue) {
  WriteBatch batch;
  struct Handler : public TestHandler {
    int num_seen = 0;
    virtual void PutCF(uint32_t column_family_id, const Slice& key,
                       const Slice& value) {
      ++num_seen;
      TestHandler::PutCF(column_family_id, key, value);
    }
    virtual void MergeCF(uint32_t column_family_id, const Slice& key,
                         const Slice& value) {
      ++num_seen;
      TestHandler::MergeCF(column_family_id, key, value);
    }
    virtual void LogData(const Slice& blob) {
      ++num_seen;
      TestHandler::LogData(blob);
    }
    virtual void DeleteCF(uint32_t column_family_id, const Slice& key) {
      ++num_seen;
      TestHandler::DeleteCF(column_family_id, key);
    }
    virtual bool Continue() override {
      return num_seen < 3;
    }
  } handler;
  batch.Put(Slice("k1"), Slice("v1"));
  batch.PutLogData(Slice("blob1"));
  batch.Delete(Slice("k1"));
  batch.PutLogData(Slice("blob2"));
  batch.Merge(Slice("foo"), Slice("bar"));
  batch.Iterate(&handler);
  ASSERT_EQ(
            "Put(k1, v1)"
            "LogData(blob1)"
            "Delete(k1)",
            handler.seen);
}
TEST(WriteBatchTest, PutGatherSlices) {
  WriteBatch batch;
  batch.Put(Slice("foo"), Slice("bar"));
  {
    Slice key_slice("baz");
    Slice value_slices[2] = { Slice("header"), Slice("payload") };
    batch.Put(SliceParts(&key_slice, 1),
              SliceParts(value_slices, 2));
  }
  {
    Slice key_slices[3] = { Slice("key"), Slice("part2"), Slice("part3") };
    Slice value_slice("value");
    batch.Put(SliceParts(key_slices, 3),
              SliceParts(&value_slice, 1));
  }
  WriteBatchInternal::SetSequence(&batch, 100);
  ASSERT_EQ("Put(baz, headerpayload)@101"
            "Put(foo, bar)@100"
            "Put(keypart2part3, value)@102",
            PrintContents(&batch));
  ASSERT_EQ(3, batch.Count());
}
TEST(WriteBatchTest, ColumnFamiliesBatchTest) {
  WriteBatch batch;
  batch.Put(0, Slice("foo"), Slice("bar"));
  batch.Put(2, Slice("twofoo"), Slice("bar2"));
  batch.Put(8, Slice("eightfoo"), Slice("bar8"));
  batch.Delete(8, Slice("eightfoo"));
  batch.Merge(3, Slice("threethree"), Slice("3three"));
  batch.Put(0, Slice("foo"), Slice("bar"));
  batch.Merge(Slice("omom"), Slice("nom"));
  TestHandler handler;
  batch.Iterate(&handler);
  ASSERT_EQ(
      "Put(foo, bar)"
      "PutCF(2, twofoo, bar2)"
      "PutCF(8, eightfoo, bar8)"
      "DeleteCF(8, eightfoo)"
      "MergeCF(3, threethree, 3three)"
      "Put(foo, bar)"
      "Merge(omom, nom)",
      handler.seen);
}
}
int main(int argc, char** argv) {
  return rocksdb::test::RunAllTests();
}
