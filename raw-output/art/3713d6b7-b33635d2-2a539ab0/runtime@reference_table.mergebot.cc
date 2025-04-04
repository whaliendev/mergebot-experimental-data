/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "reference_table.h"
#include "base/mutex.h"
#include "indirect_reference_table.h"
#include "mirror/array.h"
#include "mirror/array-inl.h"
#include "mirror/class.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/string-inl.h"
#include "runtime-inl.h"
#include "thread.h"
#include "utils.h"

namespace art {

ReferenceTable::ReferenceTable(const char* name, size_t initial_size, size_t max_size)
    : name_(name), max_size_(max_size) {
  CHECK_LE(initial_size, max_size);
  entries_.reserve(initial_size);
}

ReferenceTable::~ReferenceTable() {
}

void ReferenceTable::Add(ObjPtr<mirror::Object> obj) {
  DCHECK(obj != nullptr);
  VerifyObject(obj);
  if (entries_.size() >= max_size_) {
    LOG(FATAL) << "ReferenceTable '" << name_ << "' "
               << "overflowed (" << max_size_ << " entries)";
  }
  entries_.push_back(GcRoot<mirror::Object>(obj));
}

void ReferenceTable::Remove(ObjPtr<mirror::Object> obj) {
  // We iterate backwards on the assumption that references are LIFO.
  for (int i = entries_.size() - 1; i >= 0; --i) {
    ObjPtr<mirror::Object> entry = entries_[i].Read();
    if (entry == obj) {
      entries_.erase(entries_.begin() + i);
      return;
    }
  }
}

void ReferenceTable::VisitRoots(RootVisitor* visitor, const RootInfo& root_info) {
  BufferedRootVisitor<kDefaultBufferedRootCount> buffered_visitor(visitor, root_info);
  for (GcRoot<mirror::Object>& root : entries_) {
    buffered_visitor.VisitRoot(root);
  }
}

// Log an object with some additional info.
//
// Pass in the number of elements in the array (or 0 if this is not an
// array object), and the number of additional objects that are identical
// or equivalent to the original.
static void DumpSummaryLine(std::ostream& os, ObjPtr<mirror::Object> obj, size_t element_count,
                            int identical, int equiv)
    REQUIRES_SHARED(Locks::mutator_lock_){
    if (obj == nullptr) {
    os << "    null reference (count=" << equiv << ")\n";
    return;
    }
    if (Runtime::Current()->IsClearedJniWeakGlobal(obj)) {
    os << "    cleared jweak (count=" << equiv << ")\n";
    return;
    }
    
    std::string className(obj->PrettyTypeOf());
    if (obj->IsClass()) {
    // We're summarizing multiple instances, so using the exemplar
    // Class' type parameter here would be misleading.
    className = "java.lang.Class";
    }
    if (element_count != 0) {
    StringAppendF(&className, " (%zd elements)", element_count);
    }
    
    size_t total = identical + equiv + 1;
    std::string msg(StringPrintf("%5zd of %s", total, className.c_str()));
    if (identical + equiv != 0) {
    StringAppendF(&msg, " (%d unique instances)", equiv + 1);
    }
    os << "    " << msg << "\n";
    }
    
} // namespace art
