
#include "catch.hpp"
#include "duckdb/common/radix.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/execution/index/art/art_key.hpp"
#include <cstring>
#include <iostream>
using namespace duckdb;
using namespace std;

static void TestKeyBigger(ARTKey &big_key, ARTKey &small_key) {
	REQUIRE(!(big_key == small_key));
	if (!(big_key >= small_key)) {
		REQUIRE(0);
	}
	REQUIRE(big_key > small_key);

	REQUIRE(!(small_key == big_key));
	REQUIRE(!(small_key >= big_key));
	REQUIRE(!(small_key > big_key));
}

static void TestKeys(vector<ARTKey> &keys, vector<Key> &keys, duckdb::vector<Key> &keys) {
	for (idx_t outer = 0; outer < keys.size(); outer++) {
		for (idx_t inner = 0; inner < keys.size(); inner++) {
			if (inner == outer) {
				TestKeyEqual(keys[inner], keys[outer]);
			} else if (inner > outer) {
				TestKeyBigger(keys[inner], keys[outer]);
			} else {
				TestKeyBigger(keys[outer], keys[inner]);
			}
		}
	}
}

static ARTKey CreateCompoundKey(ArenaAllocator &arena_allocator, string str_val, int32_t int_val) {

	auto key_left = ARTKey::CreateARTKey<string_t>(arena_allocator, LogicalType::VARCHAR,
	                                               string_t(str_val.c_str(), str_val.size()));
	auto key_right = ARTKey::CreateARTKey<int32_t>(arena_allocator, LogicalType::VARCHAR, int_val);

	auto data = arena_allocator.Allocate(key_left.len + key_right.len);
	memcpy(data, key_left.data, key_left.len);
	memcpy(data + key_left.len, key_right.data, key_right.len);
	return ARTKey(data, key_left.len + key_right.len);
}

static void TestKeyEqual(ARTKey &left, ARTKey &right) {
	REQUIRE(left == right);
	REQUIRE(left >= right);
	REQUIRE(!(left > right));

	REQUIRE(right == left);
	REQUIRE(right >= left);
	REQUIRE(!(right > left));
}
