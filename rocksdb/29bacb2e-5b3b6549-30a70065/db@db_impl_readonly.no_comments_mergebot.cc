#include "db/db_impl_readonly.h"
#include "db/db_impl.h"
#include <algorithm>
#include <set>
#include <string>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/merge_context.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/status.h"
#include "rocksdb/table.h"
#include "rocksdb/merge_operator.h"
#include "port/port.h"
#include "table/block.h"
#include "table/merger.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "util/logging.h"
#include "util/build_version.h"
namespace rocksdb {
DBImplReadOnly::DBImplReadOnly(const Options& options,
                               const std::string& dbname)
    : DBImpl(options, dbname) {
  Log(options_.info_log, "Opening the db in read only mode");
}
DBImplReadOnly::~DBImplReadOnly() {}
Status DBImplReadOnly::Get(const ReadOptions& options,
                           const ColumnFamilyHandle& column_family,
                           const Slice& key, std::string* value) {
  Status s;
<<<<<<< HEAD
  MemTable* mem = GetDefaultColumnFamily()->mem();
  Version* current = GetDefaultColumnFamily()->current();
||||||| 30a700657
  MemTable* mem = GetMemTable();
  Version* current = versions_->current();
=======
>>>>>>> 5b3b6549d68b61e65c1614ad5f4da115a06a94f0
  SequenceNumber snapshot = versions_->LastSequence();
  SuperVersion* super_version = GetSuperVersion();
  MergeContext merge_context;
  LookupKey lkey(key, snapshot);
  if (super_version->mem->Get(lkey, value, &s, merge_context, options_)) {
  } else {
    Version::GetStats stats;
    super_version->current->Get(options, lkey, value, &s, &merge_context,
                                &stats, options_);
  }
  return s;
}
Iterator* DBImplReadOnly::NewIterator(const ReadOptions& options) {
  SequenceNumber latest_snapshot;
  Iterator* internal_iter = NewInternalIterator(options, &latest_snapshot);
  return NewDBIterator(
      &dbname_, env_, options_, user_comparator(), internal_iter,
      (options.snapshot != nullptr
           ? reinterpret_cast<const SnapshotImpl*>(options.snapshot)->number_
           : latest_snapshot));
}
Status DB::OpenForReadOnly(const Options& options, const std::string& dbname,
                           DB** dbptr, bool error_if_log_file_exist) {
  *dbptr = nullptr;
  DBImplReadOnly* impl = new DBImplReadOnly(options, dbname);
  impl->mutex_.Lock();
<<<<<<< HEAD
  DBOptions db_options(options);
  ColumnFamilyOptions cf_options(options);
  std::vector<ColumnFamilyDescriptor> column_families;
  column_families.push_back(
      ColumnFamilyDescriptor(default_column_family_name, cf_options));
  Status s = impl->Recover(column_families, true ,
                           error_if_log_file_exist);
||||||| 30a700657
  Status s = impl->Recover(true , error_if_log_file_exist);
=======
  Status s = impl->Recover(true , error_if_log_file_exist);
  if (s.ok()) {
    delete impl->InstallSuperVersion(new DBImpl::SuperVersion());
  }
>>>>>>> 5b3b6549d68b61e65c1614ad5f4da115a06a94f0
  impl->mutex_.Unlock();
  if (s.ok()) {
    *dbptr = impl;
  } else {
    delete impl;
  }
  return s;
}
}
