//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
#include <cstdlib>
#include "db/db_test_util.h"
#include "port/stack_trace.h"
#include "rocksdb/wal_filter.h"

namespace rocksdb {

class DBTest2 : public DBTestBase {
 public:
  DBTest2() : DBTestBase("/db_test2") {}
};

TEST_F(DBTest2, IteratorPropertyVersionNumber) {
  Put("", "");
  Iterator* iter1 = db_->NewIterator(ReadOptions());
  std::string prop_value;
  ASSERT_OK(
      iter1->GetProperty("rocksdb.iterator.super-version-number", &prop_value));
  uint64_t version_number1 =
      static_cast<uint64_t>(std::atoi(prop_value.c_str()));

  Put("", "");
  Flush();

  Iterator* iter2 = db_->NewIterator(ReadOptions());
  ASSERT_OK(
      iter2->GetProperty("rocksdb.iterator.super-version-number", &prop_value));
  uint64_t version_number2 =
      static_cast<uint64_t>(std::atoi(prop_value.c_str()));

  ASSERT_GT(version_number2, version_number1);

  Put("", "");

  Iterator* iter3 = db_->NewIterator(ReadOptions());
  ASSERT_OK(
      iter3->GetProperty("rocksdb.iterator.super-version-number", &prop_value));
  uint64_t version_number3 =
      static_cast<uint64_t>(std::atoi(prop_value.c_str()));

  ASSERT_EQ(version_number2, version_number3);

  iter1->SeekToFirst();
  ASSERT_OK(
      iter1->GetProperty("rocksdb.iterator.super-version-number", &prop_value));
  uint64_t version_number1_new =
      static_cast<uint64_t>(std::atoi(prop_value.c_str()));
  ASSERT_EQ(version_number1, version_number1_new);

  delete iter1;
  delete iter2;
  delete iter3;
}

TEST_F(DBTest2, CacheIndexAndFilterWithDBRestart) {
  Options options = CurrentOptions();
  options.create_if_missing = true;
  options.statistics = rocksdb::CreateDBStatistics();
  BlockBasedTableOptions table_options;
  table_options.cache_index_and_filter_blocks = true;
  table_options.filter_policy.reset(NewBloomFilterPolicy(20));
  options.table_factory.reset(new BlockBasedTableFactory(table_options));
  CreateAndReopenWithCF({"pikachu"}, options);

  Put(1, "a", "begin");
  Put(1, "z", "end");
  ASSERT_OK(Flush(1));
  TryReopenWithColumnFamilies({"default", "pikachu"}, options);

  std::string value;
  value = Get(1, "a");
}

#ifndef ROCKSDB_LITE namespace { void ValidateKeyExistence(DB* db, const std::vector<Slice>& keys_must_exist, const std::vector<Slice>& keys_must_not_exist) { // Ensure that expected keys exist std::vector<std::string> values; if (keys_must_exist.size() > 0) { std::vector<Status> status_list = db->MultiGet(ReadOptions(), keys_must_exist, &values); for (size_t i = 0; i < keys_must_exist.size(); i++) { ASSERT_OK(status_list[i]); } } // Ensure that given keys don't exist if (keys_must_not_exist.size() > 0) { std::vector<Status> status_list = db->MultiGet(ReadOptions(), keys_must_not_exist, &values); for (size_t i = 0; i < keys_must_not_exist.size(); i++) { ASSERT_TRUE(status_list[i].IsNotFound()); } } } // namespace TEST_F(DBTest2, WalFilterTest) { class TestWalFilter : public WalFilter { private: // Processing option that is requested to be applied at the given index WalFilter::WalProcessingOption wal_processing_option_; // Index at which to apply wal_processing_option_ // At other indexes default wal_processing_option::kContinueProcessing is // returned. size_t apply_option_ // At other indexes default wal_processing_option::kContinueProcessing is // returned. size_t apply_option_ // At other indexes default wal_processing_option::kContinueProcessing is // returned. size_t apply_option_ // At other indexes default wal_processing_option::kContinueProcessing is // returned.
}  // namespace rocksdb

int main(int argc, char** argv) {
  rocksdb::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
