//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Thread-safe (provides internal synchronization)

#pragma once
#include <string>
#include <stdint.h>

#include "db/dbformat.h"
#include "port/port.h"
#include "rocksdb/cache.h"
#include "rocksdb/env.h"
#include "rocksdb/table.h"
#include "table/table_reader.h"

namespace rocksdb {

class Env;
struct FileMetaData;

class TableCache {
 public:
  TableCache(const std::string& dbname, const Options* options,
             const EnvOptions& storage_options, Cache* cache);
  ~TableCache();

  // Return an iterator for the specified file number (the corresponding
  // file length must be exactly "file_size" bytes).  If "tableptr" is
  // non-nullptr, also sets "*tableptr" to point to the Table object
  // underlying the returned iterator, or nullptr if no Table object underlies
  // the returned iterator.  The returned "*tableptr" object is owned by
  // the cache and should not be deleted, and is valid for as long as the
  // returned iterator is live.
  Iterator* NewIterator(const ReadOptions& options, const EnvOptions& toptions,
                        const InternalKeyComparator& internal_comparator,
                        const FileMetaData& file_meta,
                        TableReader** table_reader_ptr = nullptr,
                        bool for_compaction = false);

  // If a seek to internal key "k" in specified file finds an entry,
  // call (*handle_result)(arg, found_key, found_value) repeatedly until
  // it returns false.
  Status Get(const ReadOptions& options,
             const InternalKeyComparator& internal_comparator,
             const FileMetaData& file_meta, const Slice& k, void* arg,
             bool (*handle_result)(void*, const ParsedInternalKey&,
                                   const Slice&, bool),
             bool* table_io, void (*mark_key_may_exist)(void*) = nullptr);

  // Determine whether the table may contain the specified prefix.  If
  // the table index or blooms are not in memory, this may cause an I/O
  bool PrefixMayMatch(const ReadOptions& options,
                      const InternalKeyComparator& internal_comparator,
                      uint64_t file_number, uint64_t file_size,
                      const Slice& internal_prefix, bool* table_io);

  // Evict any entry for the specified file number
  static void Evict(Cache* cache, uint64_t file_number);

  // Find table reader
  Status FindTable(const EnvOptions& toptions,
                   const InternalKeyComparator& internal_comparator,
                   uint64_t file_number, uint64_t file_size, Cache::Handle**,
                   bool* table_io = nullptr, const bool no_io = false);

  // Get TableReader from a cache handle.
  TableReader* GetTableReaderFromHandle(Cache::Handle* handle);

  // Release the handle from a cache
  void ReleaseHandle(Cache::Handle* handle);

 private:
  Env* const env_;
  const std::string dbname_;
  const Options* options_;
  const EnvOptions& storage_options_;

const EnvOptions& storage_options_; Cache* const cache_;
};

}  // namespace rocksdb
