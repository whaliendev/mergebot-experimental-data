//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

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
    memset((void*)&ikey, 0, sizeof(ikey));
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
>>>>>>> 4564b2e8f9a2da07fa84fb5f3bba007469052dfb
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

class WriteBatchTest {};

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

}  // namespace

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

namespace {
struct TestHandler : public WriteBatch::Handler {
  std::string seen;
  virtual void Put(const Slice& key, const Slice& value) {
    seen += "Put(" + key.ToString() + ", " + value.ToString() + ")";
  }
  virtual void Merge(const Slice& key, const Slice& value) {
    seen += "Merge(" + key.ToString() + ", " + value.ToString() + ")";
  }
  virtual void LogData(const Slice& blob) {
    seen += "LogData(" + blob.ToString() + ")";
  }
  virtual void Delete(const Slice& key) {
    seen += "Delete(" + key.ToString() + ")";
  }
};

}  // namespace

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

}  // namespace rocksdb

int main(int argc, char** argv) { return rocksdb::test::RunAllTests(); }