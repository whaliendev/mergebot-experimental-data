       
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/types.hpp"
namespace duckdb {
class Allocator;
struct AggregateObject;
struct AggregateFilterData;
class DataChunk;
class RowLayout;
class TupleDataLayout;
class RowDataCollection;
struct SelectionVector;
class StringHeap;
class Vector;
struct UnifiedVectorFormat;
struct RowOperationsState {
 RowOperationsState(Allocator &allocator) : allocator(allocator) {
 }
 Allocator &allocator;
};
struct RowOperations {
 static void InitializeStates(TupleDataLayout &layout, Vector &addresses, const SelectionVector &sel, idx_t count);
 static void DestroyStates(RowOperationsState &state, TupleDataLayout &layout, Vector &addresses, idx_t count);
 static void UpdateStates(RowOperationsState &state, AggregateObject &aggr, Vector &addresses, DataChunk &payload,
                          idx_t arg_idx, idx_t count);
 static void UpdateFilteredStates(RowOperationsState &state, AggregateFilterData &filter_data, AggregateObject &aggr,
                                  Vector &addresses, DataChunk &payload, idx_t arg_idx);
 static void CombineStates(RowOperationsState &state, TupleDataLayout &layout, Vector &sources, Vector &targets,
                           idx_t count);
 static void FinalizeStates(RowOperationsState &state, TupleDataLayout &layout, Vector &addresses, DataChunk &result,
                            idx_t aggr_idx);
 static void Scatter(DataChunk &columns, UnifiedVectorFormat col_data[], const RowLayout &layout, Vector &rows,
                     RowDataCollection &string_heap, const SelectionVector &sel, idx_t count);
 static void Gather(Vector &rows, const SelectionVector &row_sel, Vector &col, const SelectionVector &col_sel,
                    const idx_t count, const RowLayout &layout, const idx_t col_no, const idx_t build_size = 0,
                    data_ptr_t heap_ptr = nullptr);
 static void FullScanColumn(const TupleDataLayout &layout, Vector &rows, Vector &col, idx_t count, idx_t col_idx);
 using Predicates = vector<ExpressionType>;
 static idx_t Match(DataChunk &columns, UnifiedVectorFormat col_data[], const TupleDataLayout &layout, Vector &rows,
                    const Predicates &predicates, SelectionVector &sel, idx_t count, SelectionVector *no_match,
                    idx_t &no_match_count);
 static void ComputeEntrySizes(Vector &v, idx_t entry_sizes[], idx_t vcount, idx_t ser_count,
                               const SelectionVector &sel, idx_t offset = 0);
 static void ComputeEntrySizes(Vector &v, UnifiedVectorFormat &vdata, idx_t entry_sizes[], idx_t vcount,
                               idx_t ser_count, const SelectionVector &sel, idx_t offset = 0);
 static void HeapScatter(Vector &v, idx_t vcount, const SelectionVector &sel, idx_t ser_count, idx_t col_idx,
                         data_ptr_t *key_locations, data_ptr_t *validitymask_locations, idx_t offset = 0);
 static void HeapScatterVData(UnifiedVectorFormat &vdata, PhysicalType type, const SelectionVector &sel,
                              idx_t ser_count, idx_t col_idx, data_ptr_t *key_locations,
                              data_ptr_t *validitymask_locations, idx_t offset = 0);
 static void HeapGather(Vector &v, const idx_t &vcount, const SelectionVector &sel, const idx_t &col_idx,
                        data_ptr_t key_locations[], data_ptr_t validitymask_locations[]);
 static void RadixScatter(Vector &v, idx_t vcount, const SelectionVector &sel, idx_t ser_count,
                          data_ptr_t key_locations[], bool desc, bool has_null, bool nulls_first, idx_t prefix_len,
                          idx_t width, idx_t offset = 0);
 static void SwizzleColumns(const RowLayout &layout, const data_ptr_t base_row_ptr, const idx_t count);
 static void SwizzleHeapPointer(const RowLayout &layout, data_ptr_t row_ptr, const data_ptr_t heap_base_ptr,
                                const idx_t count, const idx_t base_offset = 0);
 static void CopyHeapAndSwizzle(const RowLayout &layout, data_ptr_t row_ptr, const data_ptr_t heap_base_ptr,
                                data_ptr_t heap_ptr, const idx_t count);
 static void UnswizzleHeapPointer(const RowLayout &layout, const data_ptr_t base_row_ptr,
                                  const data_ptr_t base_heap_ptr, const idx_t count);
 static void UnswizzlePointers(const RowLayout &layout, const data_ptr_t base_row_ptr,
                               const data_ptr_t base_heap_ptr, const idx_t count);
};
}
