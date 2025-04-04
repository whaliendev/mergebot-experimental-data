       
#include "common/types/vector.hpp"
#include <functional>
namespace duckdb {
struct VectorOperations {
 static void Add(Vector &A, Vector &B, Vector &result);
 static void Subtract(Vector &A, Vector &B, Vector &result);
 static void Multiply(Vector &A, Vector &B, Vector &result);
 static void Divide(Vector &A, Vector &B, Vector &result);
 static void Modulo(Vector &A, Vector &B, Vector &result);
 static void AddInPlace(Vector &A, Vector &B);
 static void AddInPlace(Vector &A, int64_t B);
 static void ModuloInPlace(Vector &A, Vector &B);
 static void ModuloInPlace(Vector &A, int64_t B);
 static void Abs(Vector &A, Vector &result);
 static void Round(Vector &A, Vector &B, Vector &result);
 static void BitwiseXOR(Vector &A, Vector &B, Vector &result);
 static void BitwiseAND(Vector &A, Vector &B, Vector &result);
 static void BitwiseOR(Vector &A, Vector &B, Vector &result);
 static void BitwiseShiftLeft(Vector &A, Vector &B, Vector &result);
 static void BitwiseShiftRight(Vector &A, Vector &B, Vector &result);
 static void BitwiseXORInPlace(Vector &A, Vector &B);
 static void IsNotNull(Vector &A, Vector &result);
 static void IsNull(Vector &A, Vector &result);
 static void And(Vector &A, Vector &B, Vector &result);
 static void Or(Vector &A, Vector &B, Vector &result);
 static void Not(Vector &A, Vector &result);
 static void Equals(Vector &A, Vector &B, Vector &result);
 static void NotEquals(Vector &A, Vector &B, Vector &result);
 static void GreaterThan(Vector &A, Vector &B, Vector &result);
 static void GreaterThanEquals(Vector &A, Vector &B, Vector &result);
 static void LessThan(Vector &A, Vector &B, Vector &result);
 static void LessThanEquals(Vector &A, Vector &B, Vector &result);
 static void Like(Vector &A, Vector &B, Vector &result);
 static void NotLike(Vector &A, Vector &B, Vector &result);
 static Value Sum(Vector &A);
 static Value Count(Vector &A);
 static Value Max(Vector &A);
 static Value Min(Vector &A);
 static bool HasNull(Vector &A);
 static Value MaximumStringLength(Vector &A);
 static bool AnyTrue(Vector &A);
 static bool AllTrue(Vector &A);
 static void Case(Vector &check, Vector &A, Vector &B, Vector &result);
 static bool Contains(Vector &vector, Value &value);
 struct Scatter {
  static void Set(Vector &source, Vector &dest);
  static void Add(Vector &source, Vector &dest);
  static void Max(Vector &source, Vector &dest);
  static void Min(Vector &source, Vector &dest);
  static void AddOne(Vector &source, Vector &dest);
  static void SetFirst(Vector &source, Vector &dest);
  static void Add(int64_t source, void **dest, count_t length);
 };
 struct Gather {
  static void Set(Vector &source, Vector &dest, bool set_null = true);
 };
 static void Sort(Vector &vector, sel_t result[]);
 static void Sort(Vector &vector, sel_t *result_vector, count_t count, sel_t result[]);
 static void Hash(Vector &A, Vector &result);
 static void CombineHash(Vector &hashes, Vector &B);
 static void GenerateSequence(Vector &result, int64_t start = 0, int64_t increment = 1);
 static void Cast(Vector &source, Vector &result, SQLType source_type, SQLType target_type);
 static void Cast(Vector &source, Vector &result);
 static void Copy(Vector &source, void *target, index_t offset = 0, count_t element_count = 0);
 static void Copy(Vector &source, Vector &target, index_t offset = 0);
 static void CopyToStorage(Vector &source, void *target, index_t offset = 0, count_t element_count = 0);
 static void AppendFromStorage(Vector &source, Vector &target);
 static void Set(Vector &result, Value value);
 template <class T> static void Exec(sel_t *sel_vector, count_t count, T &&fun, index_t offset = 0) {
  index_t i = offset;
  if (sel_vector) {
   for (; i < count; i++) {
    fun(sel_vector[i], i);
   }
  } else {
   for (; i < count; i++) {
    fun(i, i);
   }
  }
 }
 template <class T> static void Exec(const Vector &vector, T &&fun, index_t offset = 0, count_t count = 0) {
  if (count == 0) {
   count = vector.count;
  } else {
   count += offset;
  }
  Exec(vector.sel_vector, count, fun, offset);
 }
 template <typename T, class FUNC>
 static void ExecType(Vector &vector, FUNC &&fun, index_t offset = 0, count_t limit = 0) {
  auto data = (T *)vector.data;
  VectorOperations::Exec(
      vector, [&](index_t i, index_t k) { fun(data[i], i, k); }, offset, limit);
 }
 template <class FUNC> static void BinaryExec(Vector &a, Vector &b, Vector &result, FUNC &&fun) {
  if (!a.IsConstant()) {
   result.sel_vector = a.sel_vector;
   result.count = a.count;
  } else if (!b.IsConstant()) {
   result.sel_vector = b.sel_vector;
   result.count = b.count;
  } else {
   result.sel_vector = nullptr;
   result.count = 1;
  }
  index_t a_mul = a.IsConstant() ? 0 : 1;
  index_t b_mul = b.IsConstant() ? 0 : 1;
  assert(a.IsConstant() || a.count == result.count);
  assert(b.IsConstant() || b.count == result.count);
  VectorOperations::Exec(result, [&](index_t i, index_t k) { fun(a_mul * i, b_mul * i, i); });
 }
 template <class FUNC> static void TernaryExec(Vector &a, Vector &b, Vector &c, Vector &result, FUNC &&fun) {
  if (!a.IsConstant()) {
   result.sel_vector = a.sel_vector;
   result.count = a.count;
  } else if (!b.IsConstant()) {
   result.sel_vector = b.sel_vector;
   result.count = b.count;
  } else if (!c.IsConstant()) {
   result.sel_vector = c.sel_vector;
   result.count = c.count;
  } else {
   result.sel_vector = nullptr;
   result.count = 1;
  }
  index_t a_mul = a.IsConstant() ? 0 : 1;
  index_t b_mul = b.IsConstant() ? 0 : 1;
  index_t c_mul = c.IsConstant() ? 0 : 1;
  assert(a.IsConstant() || a.count == result.count);
  assert(b.IsConstant() || b.count == result.count);
  assert(c.IsConstant() || c.count == result.count);
  VectorOperations::Exec(result, [&](index_t i, index_t k) { fun(a_mul * i, b_mul * i, c_mul * i, i); });
 }
};
}
