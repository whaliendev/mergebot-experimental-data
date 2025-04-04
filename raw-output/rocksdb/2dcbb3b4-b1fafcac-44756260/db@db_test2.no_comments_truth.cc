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
#ifndef ROCKSDB_LITE
namespace {
  void ValidateKeyExistence(DB* db, const std::vector<Slice>& keys_must_exist,
    const std::vector<Slice>& keys_must_not_exist) {
    std::vector<std::string> values;
    if (keys_must_exist.size() > 0) {
      std::vector<Status> status_list =
        db->MultiGet(ReadOptions(), keys_must_exist, &values);
      for (size_t i = 0; i < keys_must_exist.size(); i++) {
        ASSERT_OK(status_list[i]);
      }
    }
    if (keys_must_not_exist.size() > 0) {
      std::vector<Status> status_list =
        db->MultiGet(ReadOptions(), keys_must_not_exist, &values);
      for (size_t i = 0; i < keys_must_not_exist.size(); i++) {
        ASSERT_TRUE(status_list[i].IsNotFound());
      }
    }
  }
}
TEST_F(DBTest2, WalFilterTest) {
  class TestWalFilter : public WalFilter {
  private:
    WalFilter::WalProcessingOption wal_processing_option_;
    size_t apply_option_at_record_index_;
    size_t current_record_index_;
  public:
    TestWalFilter(WalFilter::WalProcessingOption wal_processing_option,
      size_t apply_option_for_record_index)
      : wal_processing_option_(wal_processing_option),
      apply_option_at_record_index_(apply_option_for_record_index),
      current_record_index_(0) {}
    virtual WalProcessingOption LogRecord(const WriteBatch& batch,
      WriteBatch* new_batch,
      bool* batch_changed) const override {
      WalFilter::WalProcessingOption option_to_return;
      if (current_record_index_ == apply_option_at_record_index_) {
        option_to_return = wal_processing_option_;
      }
      else {
        option_to_return = WalProcessingOption::kContinueProcessing;
      }
      (const_cast<TestWalFilter*>(this)->current_record_index_)++;
      return option_to_return;
    }
    virtual const char* Name() const override { return "TestWalFilter"; }
  };
  std::vector<std::vector<std::string>> batch_keys(3);
  batch_keys[0].push_back("key1");
  batch_keys[0].push_back("key2");
  batch_keys[1].push_back("key3");
  batch_keys[1].push_back("key4");
  batch_keys[2].push_back("key5");
  batch_keys[2].push_back("key6");
  for (int option = 0;
    option < static_cast<int>(
    WalFilter::WalProcessingOption::kWalProcessingOptionMax);
  option++) {
    Options options = OptionsForLogIterTest();
    DestroyAndReopen(options);
    CreateAndReopenWithCF({ "pikachu" }, options);
    for (size_t i = 0; i < batch_keys.size(); i++) {
      WriteBatch batch;
      for (size_t j = 0; j < batch_keys[i].size(); j++) {
        batch.Put(handles_[0], batch_keys[i][j], DummyString(1024));
      }
      dbfull()->Write(WriteOptions(), &batch);
    }
    WalFilter::WalProcessingOption wal_processing_option =
      static_cast<WalFilter::WalProcessingOption>(option);
    size_t apply_option_for_record_index = 1;
    TestWalFilter test_wal_filter(wal_processing_option,
      apply_option_for_record_index);
    options = OptionsForLogIterTest();
    options.wal_filter = &test_wal_filter;
    Status status =
      TryReopenWithColumnFamilies({ "default", "pikachu" }, options);
    if (wal_processing_option ==
      WalFilter::WalProcessingOption::kCorruptedRecord) {
      assert(!status.ok());
      options.paranoid_checks = false;
      ReopenWithColumnFamilies({ "default", "pikachu" }, options);
    }
    else {
      assert(status.ok());
    }
    std::vector<Slice> keys_must_exist;
    std::vector<Slice> keys_must_not_exist;
    switch (wal_processing_option) {
    case WalFilter::WalProcessingOption::kCorruptedRecord:
    case WalFilter::WalProcessingOption::kContinueProcessing: {
      fprintf(stderr, "Testing with complete WAL processing\n");
      for (size_t i = 0; i < batch_keys.size(); i++) {
        for (size_t j = 0; j < batch_keys[i].size(); j++) {
          keys_must_exist.push_back(Slice(batch_keys[i][j]));
        }
      }
      break;
    }
    case WalFilter::WalProcessingOption::kIgnoreCurrentRecord: {
      fprintf(stderr,
        "Testing with ignoring record %" ROCKSDB_PRIszt " only\n",
        apply_option_for_record_index);
      for (size_t i = 0; i < batch_keys.size(); i++) {
        for (size_t j = 0; j < batch_keys[i].size(); j++) {
          if (i == apply_option_for_record_index) {
            keys_must_not_exist.push_back(Slice(batch_keys[i][j]));
          }
          else {
            keys_must_exist.push_back(Slice(batch_keys[i][j]));
          }
        }
      }
      break;
    }
    case WalFilter::WalProcessingOption::kStopReplay: {
      fprintf(stderr,
        "Testing with stopping replay from record %" ROCKSDB_PRIszt
        "\n",
        apply_option_for_record_index);
      for (size_t i = 0; i < batch_keys.size(); i++) {
        for (size_t j = 0; j < batch_keys[i].size(); j++) {
          if (i >= apply_option_for_record_index) {
            keys_must_not_exist.push_back(Slice(batch_keys[i][j]));
          }
          else {
            keys_must_exist.push_back(Slice(batch_keys[i][j]));
          }
        }
      }
      break;
    }
    default:
      assert(false);
    }
    bool checked_after_reopen = false;
    while (true) {
      ValidateKeyExistence(db_, keys_must_exist, keys_must_not_exist);
      if (checked_after_reopen) {
        break;
      }
      options = OptionsForLogIterTest();
      ReopenWithColumnFamilies({ "default", "pikachu" }, options);
      checked_after_reopen = true;
    }
  }
}
TEST_F(DBTest2, WalFilterTestWithChangeBatch) {
  class ChangeBatchHandler : public WriteBatch::Handler {
  private:
    WriteBatch* new_write_batch_;
    size_t num_keys_to_add_in_new_batch_;
    size_t num_keys_added_;
  public:
    ChangeBatchHandler(WriteBatch* new_write_batch,
      size_t num_keys_to_add_in_new_batch)
      : new_write_batch_(new_write_batch),
      num_keys_to_add_in_new_batch_(num_keys_to_add_in_new_batch),
      num_keys_added_(0) {}
    virtual void Put(const Slice& key, const Slice& value) override {
      if (num_keys_added_ < num_keys_to_add_in_new_batch_) {
        new_write_batch_->Put(key, value);
        ++num_keys_added_;
      }
    }
  };
  class TestWalFilterWithChangeBatch : public WalFilter {
  private:
    size_t change_records_from_index_;
    size_t num_keys_to_add_in_new_batch_;
    size_t current_record_index_;
  public:
    TestWalFilterWithChangeBatch(size_t change_records_from_index,
      size_t num_keys_to_add_in_new_batch)
      : change_records_from_index_(change_records_from_index),
      num_keys_to_add_in_new_batch_(num_keys_to_add_in_new_batch),
      current_record_index_(0) {}
    virtual WalProcessingOption LogRecord(const WriteBatch& batch,
      WriteBatch* new_batch,
      bool* batch_changed) const override {
      if (current_record_index_ >= change_records_from_index_) {
        ChangeBatchHandler handler(new_batch, num_keys_to_add_in_new_batch_);
        batch.Iterate(&handler);
        *batch_changed = true;
      }
      (const_cast<TestWalFilterWithChangeBatch*>(this)
        ->current_record_index_)++;
      return WalProcessingOption::kContinueProcessing;
    }
    virtual const char* Name() const override {
      return "TestWalFilterWithChangeBatch";
    }
  };
  std::vector<std::vector<std::string>> batch_keys(3);
  batch_keys[0].push_back("key1");
  batch_keys[0].push_back("key2");
  batch_keys[1].push_back("key3");
  batch_keys[1].push_back("key4");
  batch_keys[2].push_back("key5");
  batch_keys[2].push_back("key6");
  Options options = OptionsForLogIterTest();
  DestroyAndReopen(options);
  CreateAndReopenWithCF({ "pikachu" }, options);
  for (size_t i = 0; i < batch_keys.size(); i++) {
    WriteBatch batch;
    for (size_t j = 0; j < batch_keys[i].size(); j++) {
      batch.Put(handles_[0], batch_keys[i][j], DummyString(1024));
    }
    dbfull()->Write(WriteOptions(), &batch);
  }
  size_t change_records_from_index = 1;
  size_t num_keys_to_add_in_new_batch = 1;
  TestWalFilterWithChangeBatch test_wal_filter_with_change_batch(
    change_records_from_index, num_keys_to_add_in_new_batch);
  options = OptionsForLogIterTest();
  options.wal_filter = &test_wal_filter_with_change_batch;
  ReopenWithColumnFamilies({ "default", "pikachu" }, options);
  std::vector<Slice> keys_must_exist;
  std::vector<Slice> keys_must_not_exist;
  for (size_t i = 0; i < batch_keys.size(); i++) {
    for (size_t j = 0; j < batch_keys[i].size(); j++) {
      if (i >= change_records_from_index && j >=
        num_keys_to_add_in_new_batch) {
        keys_must_not_exist.push_back(Slice(batch_keys[i][j]));
      }
      else {
        keys_must_exist.push_back(Slice(batch_keys[i][j]));
      }
    }
  }
  bool checked_after_reopen = false;
  while (true) {
    ValidateKeyExistence(db_, keys_must_exist, keys_must_not_exist);
    if (checked_after_reopen) {
      break;
    }
    options = OptionsForLogIterTest();
    ReopenWithColumnFamilies({ "default", "pikachu" }, options);
    checked_after_reopen = true;
  }
}
TEST_F(DBTest2, WalFilterTestWithChangeBatchExtraKeys) {
  class TestWalFilterWithChangeBatchAddExtraKeys : public WalFilter {
  public:
    virtual WalProcessingOption LogRecord(const WriteBatch& batch,
      WriteBatch* new_batch,
      bool* batch_changed) const override {
      *new_batch = batch;
      new_batch->Put("key_extra", "value_extra");
      *batch_changed = true;
      return WalProcessingOption::kContinueProcessing;
    }
    virtual const char* Name() const override {
      return "WalFilterTestWithChangeBatchExtraKeys";
    }
  };
  std::vector<std::vector<std::string>> batch_keys(3);
  batch_keys[0].push_back("key1");
  batch_keys[0].push_back("key2");
  batch_keys[1].push_back("key3");
  batch_keys[1].push_back("key4");
  batch_keys[2].push_back("key5");
  batch_keys[2].push_back("key6");
  Options options = OptionsForLogIterTest();
  DestroyAndReopen(options);
  CreateAndReopenWithCF({ "pikachu" }, options);
  for (size_t i = 0; i < batch_keys.size(); i++) {
    WriteBatch batch;
    for (size_t j = 0; j < batch_keys[i].size(); j++) {
      batch.Put(handles_[0], batch_keys[i][j], DummyString(1024));
    }
    dbfull()->Write(WriteOptions(), &batch);
  }
  TestWalFilterWithChangeBatchAddExtraKeys test_wal_filter_extra_keys;
  options = OptionsForLogIterTest();
  options.wal_filter = &test_wal_filter_extra_keys;
  Status status = TryReopenWithColumnFamilies({ "default", "pikachu" },
    options);
  ASSERT_TRUE(status.IsNotSupported());
  options = OptionsForLogIterTest();
  ReopenWithColumnFamilies({ "default", "pikachu" }, options);
  std::vector<Slice> keys_must_exist;
  std::vector<Slice> keys_must_not_exist;
  for (size_t i = 0; i < batch_keys.size(); i++) {
    for (size_t j = 0; j < batch_keys[i].size(); j++) {
      keys_must_exist.push_back(Slice(batch_keys[i][j]));
    }
  }
  ValidateKeyExistence(db_, keys_must_exist, keys_must_not_exist);
}
TEST_F(DBTest2, WalFilterTestWithColumnFamilies) {
  class TestWalFilterWithColumnFamilies : public WalFilter {
  private:
    std::map<uint32_t, uint64_t> cf_log_number_map_;
    std::map<std::string, uint32_t> cf_name_id_map_;
    std::map<uint32_t, std::vector<std::string>> cf_wal_keys_;
  public:
    virtual void ColumnFamilyLogNumberMap(
      const std::map<uint32_t, uint64_t>& cf_lognumber_map,
      const std::map<std::string, uint32_t>& cf_name_id_map) override {
      cf_log_number_map_ = cf_lognumber_map;
      cf_name_id_map_ = cf_name_id_map;
    }
    virtual WalProcessingOption LogRecord(unsigned long long log_number,
      const std::string& log_file_name,
      const WriteBatch& batch,
      WriteBatch* new_batch,
      bool* batch_changed) override {
      class LogRecordBatchHandler : public WriteBatch::Handler {
      private:
        const std::map<uint32_t, uint64_t> & cf_log_number_map_;
        std::map<uint32_t, std::vector<std::string>> & cf_wal_keys_;
        unsigned long long log_number_;
      public:
        LogRecordBatchHandler(unsigned long long current_log_number,
          const std::map<uint32_t, uint64_t> & cf_log_number_map,
          std::map<uint32_t, std::vector<std::string>> & cf_wal_keys) :
          cf_log_number_map_(cf_log_number_map),
          cf_wal_keys_(cf_wal_keys),
          log_number_(current_log_number){}
        virtual Status PutCF(uint32_t column_family_id, const Slice& key,
          const Slice& ) override {
          auto it = cf_log_number_map_.find(column_family_id);
          assert(it != cf_log_number_map_.end());
          unsigned long long log_number_for_cf = it->second;
          if (log_number_ >= log_number_for_cf) {
            cf_wal_keys_[column_family_id].push_back(std::string(key.data(),
              key.size()));
          }
          return Status::OK();
        }
      } handler(log_number, cf_log_number_map_, cf_wal_keys_);
      batch.Iterate(&handler);
      return WalProcessingOption::kContinueProcessing;
    }
    virtual const char* Name() const override {
      return "WalFilterTestWithColumnFamilies";
    }
    const std::map<uint32_t, std::vector<std::string>> &
    GetColumnFamilyKeys() {
      return cf_wal_keys_;
    }
    const std::map<std::string, uint32_t> & GetColumnFamilyNameIdMap() {
      return cf_name_id_map_;
    }
  };
  std::vector<std::vector<std::string>> batch_keys_pre_flush(3);
  batch_keys_pre_flush[0].push_back("key1");
  batch_keys_pre_flush[0].push_back("key2");
  batch_keys_pre_flush[1].push_back("key3");
  batch_keys_pre_flush[1].push_back("key4");
  batch_keys_pre_flush[2].push_back("key5");
  batch_keys_pre_flush[2].push_back("key6");
  Options options = OptionsForLogIterTest();
  DestroyAndReopen(options);
  CreateAndReopenWithCF({ "pikachu" }, options);
  for (size_t i = 0; i < batch_keys_pre_flush.size(); i++) {
    WriteBatch batch;
    for (size_t j = 0; j < batch_keys_pre_flush[i].size(); j++) {
      batch.Put(handles_[0], batch_keys_pre_flush[i][j], DummyString(1024));
      batch.Put(handles_[1], batch_keys_pre_flush[i][j], DummyString(1024));
    }
    dbfull()->Write(WriteOptions(), &batch);
  }
  db_->Flush(FlushOptions(), handles_[0]);
  std::vector<std::vector<std::string>> batch_keys_post_flush(3);
  batch_keys_post_flush[0].push_back("key7");
  batch_keys_post_flush[0].push_back("key8");
  batch_keys_post_flush[1].push_back("key9");
  batch_keys_post_flush[1].push_back("key10");
  batch_keys_post_flush[2].push_back("key11");
  batch_keys_post_flush[2].push_back("key12");
  for (size_t i = 0; i < batch_keys_post_flush.size(); i++) {
    WriteBatch batch;
    for (size_t j = 0; j < batch_keys_post_flush[i].size(); j++) {
      batch.Put(handles_[0], batch_keys_post_flush[i][j], DummyString(1024));
      batch.Put(handles_[1], batch_keys_post_flush[i][j], DummyString(1024));
    }
    dbfull()->Write(WriteOptions(), &batch);
  }
  TestWalFilterWithColumnFamilies test_wal_filter_column_families;
  options = OptionsForLogIterTest();
  options.wal_filter = &test_wal_filter_column_families;
  Status status =
    TryReopenWithColumnFamilies({ "default", "pikachu" }, options);
  ASSERT_TRUE(status.ok());
  auto cf_wal_keys = test_wal_filter_column_families.GetColumnFamilyKeys();
  auto name_id_map =
    test_wal_filter_column_families.GetColumnFamilyNameIdMap();
  size_t index = 0;
  auto keys_cf = cf_wal_keys[name_id_map[kDefaultColumnFamilyName]];
  for (size_t i = 0; i < batch_keys_post_flush.size(); i++) {
    for (size_t j = 0; j < batch_keys_post_flush[i].size(); j++) {
      Slice key_from_the_log(keys_cf[index++]);
      Slice batch_key(batch_keys_post_flush[i][j]);
      ASSERT_TRUE(key_from_the_log.compare(batch_key) == 0);
    }
  }
  ASSERT_TRUE(index == keys_cf.size());
  index = 0;
  keys_cf = cf_wal_keys[name_id_map["pikachu"]];
  for (size_t i = 0; i < batch_keys_pre_flush.size(); i++) {
    for (size_t j = 0; j < batch_keys_pre_flush[i].size(); j++) {
      Slice key_from_the_log(keys_cf[index++]);
      Slice batch_key(batch_keys_pre_flush[i][j]);
      ASSERT_TRUE(key_from_the_log.compare(batch_key) == 0);
    }
  }
  for (size_t i = 0; i < batch_keys_post_flush.size(); i++) {
    for (size_t j = 0; j < batch_keys_post_flush[i].size(); j++) {
      Slice key_from_the_log(keys_cf[index++]);
      Slice batch_key(batch_keys_post_flush[i][j]);
      ASSERT_TRUE(key_from_the_log.compare(batch_key) == 0);
    }
  }
  ASSERT_TRUE(index == keys_cf.size());
}
#endif
TEST_F(DBTest2, DISABLED_FirstSnapshotTest) {
  Options options;
  options.write_buffer_size = 100000;
  options = CurrentOptions(options);
  CreateAndReopenWithCF({"pikachu"}, options);
  const Snapshot* s1 = db_->GetSnapshot();
  Put(1, "k1", std::string(100000, 'x'));
  Put(1, "k2", std::string(100000, 'y'));
  db_->ReleaseSnapshot(s1);
}
}
int main(int argc, char** argv) {
  rocksdb::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
