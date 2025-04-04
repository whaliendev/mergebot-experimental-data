//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/case_insensitive_map.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/map.hpp"

namespace duckdb {

struct CaseInsensitiveStringHashFunction {
	uint64_t operator()(const string &str) const {
		return StringUtil::CIHash(str);
	}
};

struct CaseInsensitiveStringEquality {
	bool operator()(const string &a, const string &b) const {
		return StringUtil::CIEquals(a, b);
	}
};

template <typename T>
using case_insensitive_map_t =
    unordered_map<string, T, CaseInsensitiveStringHashFunction, CaseInsensitiveStringEquality>;

using case_insensitive_set_t = unordered_set<string, CaseInsensitiveStringHashFunction, CaseInsensitiveStringEquality>;

<<<<<<< HEAD
struct CaseInsensitiveStringCompare {
	bool operator()(const string &a, const string &b) const {
		return StringUtil::CILessThan(a, b);
	}
};

template <typename T>
using case_insensitive_ordered_map_t = map<string, T, CaseInsensitiveStringCompare>;

||||||| d02b472cbf
=======
struct CaseInsensitiveStringCompare {
	bool operator()(const string &s1, const string &s2) const {
		return StringUtil::CILessThan(s1, s2);
	}
};

template <typename T>
using case_insensitive_tree_t = map<string, T, CaseInsensitiveStringCompare>;

>>>>>>> e4dd1e9d
} // namespace duckdb
