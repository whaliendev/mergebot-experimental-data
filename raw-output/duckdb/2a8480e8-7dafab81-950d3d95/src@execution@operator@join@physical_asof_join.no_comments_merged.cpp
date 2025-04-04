#include "duckdb/execution/operator/join/physical_asof_join.hpp"
#include "duckdb/common/fast_mem.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/row_operations/row_operations.hpp"
#include "duckdb/common/sort/comparators.hpp"
#include "duckdb/common/sort/partition_state.hpp"
#include "duckdb/common/sort/sort.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/operator/join/outer_join_marker.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/event.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include <thread>
namespace duckdb {
PhysicalAsOfJoin::PhysicalAsOfJoin(LogicalComparisonJoin &op, unique_ptr<PhysicalOperator> left,
                                   unique_ptr<PhysicalOperator> right)
    : PhysicalComparisonJoin(op, PhysicalOperatorType::ASOF_JOIN, std::move(op.conditions), op.join_type,
                             op.estimated_cardinality) {
 for (auto &cond : conditions) {
  D_ASSERT(cond.left->return_type == cond.right->return_type);
  join_key_types.push_back(cond.left->return_type);
  auto left = cond.left->Copy();
  auto right = cond.right->Copy();
  switch (cond.comparison) {
  case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
   null_sensitive.emplace_back(lhs_orders.size());
   lhs_orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_LAST, std::move(left));
   rhs_orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_LAST, std::move(right));
   break;
  case ExpressionType::COMPARE_EQUAL:
   null_sensitive.emplace_back(lhs_orders.size());
  case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
   lhs_partitions.emplace_back(std::move(left));
   rhs_partitions.emplace_back(std::move(right));
   break;
  default:
   throw NotImplementedException("Unsupported join condition for ASOF join");
  }
 }
 D_ASSERT(!lhs_orders.empty());
 D_ASSERT(!rhs_orders.empty());
 children.push_back(std::move(left));
 children.push_back(std::move(right));
 right_projection_map = op.right_projection_map;
 if (right_projection_map.empty()) {
  const auto right_count = children[1]->types.size();
  right_projection_map.reserve(right_count);
  for (column_t i = 0; i < right_count; ++i) {
   right_projection_map.emplace_back(i);
  }
 }
}
class AsOfGlobalSinkState : public GlobalSinkState {
public:
 AsOfGlobalSinkState(ClientContext &context, const PhysicalAsOfJoin &op)
     : rhs_sink(context, op.rhs_partitions, op.rhs_orders, op.children[1]->types, {}, op.estimated_cardinality),
       is_outer(IsRightOuterJoin(op.join_type)), has_null(false) {
 }
 idx_t Count() const {
  return rhs_sink.count;
 }
 PartitionLocalSinkState *RegisterBuffer(ClientContext &context) {
  lock_guard<mutex> guard(lock);
  lhs_buffers.emplace_back(make_uniq<PartitionLocalSinkState>(context, *lhs_sink));
  return lhs_buffers.back().get();
 }
 PartitionGlobalSinkState rhs_sink;
 const bool is_outer;
 vector<OuterJoinMarker> right_outers;
 bool has_null;
 unique_ptr<PartitionGlobalSinkState> lhs_sink;
 mutex lock;
 vector<unique_ptr<PartitionLocalSinkState>> lhs_buffers;
};
class AsOfLocalSinkState : public LocalSinkState {
public:
 explicit AsOfLocalSinkState(ClientContext &context, PartitionGlobalSinkState &gstate_p)
     : local_partition(context, gstate_p) {
 }
 void Sink(DataChunk &input_chunk) {
  local_partition.Sink(input_chunk);
 }
 void Combine() {
  local_partition.Combine();
 }
 PartitionLocalSinkState local_partition;
};
unique_ptr<GlobalSinkState> PhysicalAsOfJoin::GetGlobalSinkState(ClientContext &context) const {
 return make_uniq<AsOfGlobalSinkState>(context, *this);
}
unique_ptr<LocalSinkState> PhysicalAsOfJoin::GetLocalSinkState(ExecutionContext &context) const {
 auto &gsink = sink_state->Cast<AsOfGlobalSinkState>();
 return make_uniq<AsOfLocalSinkState>(context.client, gsink.rhs_sink);
}
SinkResultType PhysicalAsOfJoin::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
 auto &lstate = input.local_state.Cast<AsOfLocalSinkState>();
 lstate.Sink(chunk);
 return SinkResultType::NEED_MORE_INPUT;
}
void PhysicalAsOfJoin::Combine(ExecutionContext &context, GlobalSinkState &gstate_p, LocalSinkState &lstate_p) const {
 auto &lstate = lstate_p.Cast<AsOfLocalSinkState>();
 lstate.Combine();
}
SinkFinalizeType PhysicalAsOfJoin::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                            GlobalSinkState &gstate_p) const {
 auto &gstate = gstate_p.Cast<AsOfGlobalSinkState>();
 const vector<unique_ptr<BaseStatistics>> partitions_stats;
 gstate.lhs_sink = make_uniq<PartitionGlobalSinkState>(context, lhs_partitions, lhs_orders, children[0]->types,
                                                       partitions_stats, 0);
 gstate.lhs_sink->SyncPartitioning(gstate.rhs_sink);
 auto &groups = gstate.rhs_sink.grouping_data->GetPartitions();
 if (groups.empty() && EmptyResultIfRHSIsEmpty()) {
  return SinkFinalizeType::NO_OUTPUT_POSSIBLE;
 }
 auto new_event = make_shared<PartitionMergeEvent>(gstate.rhs_sink, pipeline);
 event.InsertEvent(std::move(new_event));
 return SinkFinalizeType::READY;
}
class AsOfGlobalState : public GlobalOperatorState {
public:
 explicit AsOfGlobalState(AsOfGlobalSinkState &gsink) {
  auto &rhs_partition = gsink.rhs_sink;
  auto &right_outers = gsink.right_outers;
  right_outers.reserve(rhs_partition.hash_groups.size());
  for (const auto &hash_group : rhs_partition.hash_groups) {
   right_outers.emplace_back(OuterJoinMarker(gsink.is_outer));
   right_outers.back().Initialize(hash_group->count);
  }
 }
};
unique_ptr<GlobalOperatorState> PhysicalAsOfJoin::GetGlobalOperatorState(ClientContext &context) const {
 auto &gsink = sink_state->Cast<AsOfGlobalSinkState>();
 return make_uniq<AsOfGlobalState>(gsink);
}
class AsOfLocalState : public CachingOperatorState {
public:
 AsOfLocalState(ClientContext &context, const PhysicalAsOfJoin &op)
     : context(context), allocator(Allocator::Get(context)), op(op), lhs_executor(context),
       left_outer(IsLeftOuterJoin(op.join_type)), fetch_next_left(true) {
  lhs_keys.Initialize(allocator, op.join_key_types);
  for (const auto &cond : op.conditions) {
   lhs_executor.AddExpression(*cond.left);
  }
  lhs_payload.Initialize(allocator, op.children[0]->types);
  lhs_sel.Initialize();
  left_outer.Initialize(STANDARD_VECTOR_SIZE);
  auto &gsink = op.sink_state->Cast<AsOfGlobalSinkState>();
  lhs_partition_sink = gsink.RegisterBuffer(context);
 }
 bool Sink(DataChunk &input);
 OperatorResultType ExecuteInternal(ExecutionContext &context, DataChunk &input, DataChunk &chunk);
 ClientContext &context;
 Allocator &allocator;
 const PhysicalAsOfJoin &op;
 ExpressionExecutor lhs_executor;
 DataChunk lhs_keys;
 ValidityMask lhs_valid_mask;
 SelectionVector lhs_sel;
 DataChunk lhs_payload;
 OuterJoinMarker left_outer;
 bool fetch_next_left;
 optional_ptr<PartitionLocalSinkState> lhs_partition_sink;
};
bool AsOfLocalState::Sink(DataChunk &input) {
 lhs_keys.Reset();
 lhs_executor.Execute(input, lhs_keys);
 const auto count = input.size();
 lhs_valid_mask.Reset();
 for (auto col_idx : op.null_sensitive) {
  auto &col = lhs_keys.data[col_idx];
  UnifiedVectorFormat unified;
  col.ToUnifiedFormat(count, unified);
  lhs_valid_mask.Combine(unified.validity, count);
 }
 idx_t lhs_valid = 0;
 const auto entry_count = lhs_valid_mask.EntryCount(count);
 idx_t base_idx = 0;
 left_outer.Reset();
 for (idx_t entry_idx = 0; entry_idx < entry_count;) {
  const auto validity_entry = lhs_valid_mask.GetValidityEntry(entry_idx++);
  const auto next = MinValue<idx_t>(base_idx + ValidityMask::BITS_PER_VALUE, count);
  if (ValidityMask::AllValid(validity_entry)) {
   for (; base_idx < next; ++base_idx) {
    lhs_sel.set_index(lhs_valid++, base_idx);
    left_outer.SetMatch(base_idx);
   }
  } else if (ValidityMask::NoneValid(validity_entry)) {
   base_idx = next;
  } else {
   const auto start = base_idx;
   for (; base_idx < next; ++base_idx) {
    if (ValidityMask::RowIsValid(validity_entry, base_idx - start)) {
     lhs_sel.set_index(lhs_valid++, base_idx);
     left_outer.SetMatch(base_idx);
    }
   }
  }
 }
 lhs_payload.Reset();
 if (lhs_valid == count) {
  lhs_payload.Reference(input);
  lhs_payload.SetCardinality(input);
 } else {
  lhs_payload.Slice(input, lhs_sel, lhs_valid);
  lhs_payload.SetCardinality(lhs_valid);
  fetch_next_left = false;
 }
 lhs_partition_sink->Sink(lhs_payload);
<<<<<<< HEAD
 return false;
||||||| merged common ancestors
 DataChunk payload_chunk;
 payload_chunk.InitializeEmpty({LogicalType::UINTEGER});
 FlatVector::SetData(payload_chunk.data[0], (data_ptr_t)lhs_sel.data());
 payload_chunk.SetCardinality(lhs_valid);
 local_sort.SinkChunk(lhs_keys, payload_chunk);
 global_state.external = force_external;
 global_state.AddLocalState(local_sort);
 global_state.PrepareMergePhase();
 while (global_state.sorted_blocks.size() > 1) {
  MergeSorter merge_sorter(*lhs_global_state, buffer_manager);
  merge_sorter.PerformInMergeRound();
  global_state.CompleteMergeRound();
 }
 D_ASSERT(global_state.sorted_blocks.size() == 1);
 auto scanner = make_uniq<PayloadScanner>(*global_state.sorted_blocks[0]->payload_data, global_state, false);
 lhs_sorted.Reset();
 scanner->Scan(lhs_sorted);
=======
 DataChunk payload_chunk;
 payload_chunk.InitializeEmpty({LogicalType::UINTEGER});
 FlatVector::SetData(payload_chunk.data[0], data_ptr_cast(lhs_sel.data()));
 payload_chunk.SetCardinality(lhs_valid);
 local_sort.SinkChunk(lhs_keys, payload_chunk);
 global_state.external = force_external;
 global_state.AddLocalState(local_sort);
 global_state.PrepareMergePhase();
 while (global_state.sorted_blocks.size() > 1) {
  MergeSorter merge_sorter(*lhs_global_state, buffer_manager);
  merge_sorter.PerformInMergeRound();
  global_state.CompleteMergeRound();
 }
 D_ASSERT(global_state.sorted_blocks.size() == 1);
 auto scanner = make_uniq<PayloadScanner>(*global_state.sorted_blocks[0]->payload_data, global_state, false);
 lhs_sorted.Reset();
 scanner->Scan(lhs_sorted);
>>>>>>> 7dafab81
}
OperatorResultType AsOfLocalState::ExecuteInternal(ExecutionContext &context, DataChunk &input, DataChunk &chunk) {
 input.Verify();
 Sink(input);
<<<<<<< HEAD
 if (!fetch_next_left) {
  fetch_next_left = true;
  left_outer.ConstructLeftJoinResult(input, chunk);
  left_outer.Reset();
||||||| merged common ancestors
 auto &gsink = op.sink_state->Cast<AsOfGlobalSinkState>();
 auto &global_partition = gsink.global_partition;
 UnifiedVectorFormat bin_unified;
 bin_vector.ToUnifiedFormat(lhs_valid, bin_unified);
 const auto bins = (hash_t *)bin_unified.data;
 hash_t prev_bin = global_partition.bin_groups.size();
 optional_ptr<PartitionGlobalHashGroup> hash_group;
 optional_ptr<OuterJoinMarker> right_outer;
 SBIterator left(*lhs_global_state, ExpressionType::COMPARE_LESSTHANOREQUALTO);
 unique_ptr<SBIterator> right;
 lhs_match_count = 0;
 const auto sorted_sel = FlatVector::GetData<sel_t>(lhs_sorted.data[0]);
 for (idx_t i = 0; i < lhs_valid; ++i) {
  const auto idx = sorted_sel[i];
  const auto curr_bin = bins[bin_unified.sel->get_index(idx)];
  if (!hash_group || curr_bin != prev_bin) {
   prev_bin = curr_bin;
   const auto group_idx = global_partition.bin_groups[curr_bin];
   if (group_idx >= global_partition.hash_groups.size()) {
    hash_group = nullptr;
    right_outer = nullptr;
    right.reset();
    continue;
   }
   hash_group = global_partition.hash_groups[group_idx].get();
   right_outer = gsink.right_outers.data() + group_idx;
   right = make_uniq<SBIterator>(*(hash_group->global_sort), ExpressionType::COMPARE_LESSTHANOREQUALTO);
  }
  left.SetIndex(i);
  if (!right->Compare(left)) {
   continue;
  }
  idx_t bound = 1;
  idx_t begin = right->GetIndex();
  right->SetIndex(begin + bound);
  while (right->GetIndex() < hash_group->count) {
   if (right->Compare(left)) {
    bound *= 2;
    right->SetIndex(begin + bound);
   } else {
    break;
   }
  }
  auto first = begin + bound / 2;
  auto last = MinValue<idx_t>(begin + bound, hash_group->count);
  while (first < last) {
   const auto mid = first + (last - first) / 2;
   right->SetIndex(mid);
   if (right->Compare(left)) {
    first = mid + 1;
   } else {
    last = mid;
   }
  }
  right->SetIndex(--first);
  if (!op.lhs_partitions.empty() && hash_group->ComparePartitions(left, *right)) {
   continue;
  }
  right_outer->SetMatch(first);
  left_outer.SetMatch(idx);
  if (found_match) {
   found_match[idx] = true;
  }
  if (matches) {
   matches[idx] = Match(curr_bin, first);
  }
  lhs_matched.set_index(lhs_match_count++, idx);
 }
}
unique_ptr<OperatorState> PhysicalAsOfJoin::GetOperatorState(ExecutionContext &context) const {
 auto &config = ClientConfig::GetConfig(context.client);
 return make_uniq<AsOfLocalState>(context.client, *this, config.force_external);
}
void PhysicalAsOfJoin::ResolveSimpleJoin(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                         OperatorState &lstate_p) const {
 auto &lstate = lstate_p.Cast<AsOfLocalState>();
 auto &gsink = sink_state->Cast<AsOfGlobalSinkState>();
 bool found_match[STANDARD_VECTOR_SIZE] = {false};
 lstate.ResolveJoin(input, found_match);
 switch (join_type) {
 case JoinType::MARK: {
  PhysicalJoin::ConstructMarkJoinResult(lstate.lhs_keys, input, chunk, found_match, gsink.has_null);
  break;
 }
 case JoinType::SEMI:
  PhysicalJoin::ConstructSemiJoinResult(input, chunk, found_match);
  break;
 case JoinType::ANTI:
  PhysicalJoin::ConstructAntiJoinResult(input, chunk, found_match);
  break;
 default:
  throw NotImplementedException("Unimplemented join type for AsOf join");
 }
}
OperatorResultType PhysicalAsOfJoin::ResolveComplexJoin(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                        OperatorState &lstate_p) const {
 auto &lstate = lstate_p.Cast<AsOfLocalState>();
 auto &gsink = sink_state->Cast<AsOfGlobalSinkState>();
 if (!lstate.fetch_next_left) {
  lstate.fetch_next_left = true;
  if (lstate.left_outer.Enabled()) {
   lstate.left_outer.ConstructLeftJoinResult(input, chunk);
   lstate.left_outer.Reset();
  }
  return OperatorResultType::NEED_MORE_INPUT;
 }
 AsOfLocalState::Match matches[STANDARD_VECTOR_SIZE];
 lstate.ResolveJoin(input, nullptr, matches);
 lstate.group_payload.Reset();
 lstate.rhs_payload.Reset();
 auto &global_partition = gsink.global_partition;
 hash_t scan_bin = global_partition.bin_groups.size();
 optional_ptr<PartitionGlobalHashGroup> hash_group;
 unique_ptr<PayloadScanner> scanner;
 for (idx_t i = 0; i < lstate.lhs_match_count; ++i) {
  const auto idx = lstate.lhs_matched[i];
  const auto match_bin = matches[idx].first;
  const auto match_pos = matches[idx].second;
  if (match_bin != scan_bin) {
   const auto group_idx = global_partition.bin_groups[match_bin];
   hash_group = global_partition.hash_groups[group_idx].get();
   scan_bin = match_bin;
   scanner = make_uniq<PayloadScanner>(*hash_group->global_sort, false);
   lstate.group_payload.Reset();
  }
  while (match_pos >= scanner->Scanned()) {
   lstate.group_payload.Reset();
   scanner->Scan(lstate.group_payload);
  }
  const auto source_offset = match_pos - (scanner->Scanned() - lstate.group_payload.size());
  for (idx_t col_idx = 0; col_idx < right_projection_map.size(); ++col_idx) {
   const auto rhs_idx = right_projection_map[col_idx];
   auto &source = lstate.group_payload.data[rhs_idx];
   auto &target = chunk.data[input.ColumnCount() + col_idx];
   VectorOperations::Copy(source, target, source_offset + 1, source_offset, i);
  }
 }
 chunk.Slice(input, lstate.lhs_matched, lstate.lhs_match_count);
 if (lstate.left_outer.Enabled()) {
  lstate.fetch_next_left = false;
  return OperatorResultType::HAVE_MORE_OUTPUT;
=======
 auto &gsink = op.sink_state->Cast<AsOfGlobalSinkState>();
 auto &global_partition = gsink.global_partition;
 UnifiedVectorFormat bin_unified;
 bin_vector.ToUnifiedFormat(lhs_valid, bin_unified);
 const auto bins = UnifiedVectorFormat::GetData<hash_t>(bin_unified);
 hash_t prev_bin = global_partition.bin_groups.size();
 optional_ptr<PartitionGlobalHashGroup> hash_group;
 optional_ptr<OuterJoinMarker> right_outer;
 SBIterator left(*lhs_global_state, ExpressionType::COMPARE_LESSTHANOREQUALTO);
 unique_ptr<SBIterator> right;
 lhs_match_count = 0;
 const auto sorted_sel = FlatVector::GetData<sel_t>(lhs_sorted.data[0]);
 for (idx_t i = 0; i < lhs_valid; ++i) {
  const auto idx = sorted_sel[i];
  const auto curr_bin = bins[bin_unified.sel->get_index(idx)];
  if (!hash_group || curr_bin != prev_bin) {
   prev_bin = curr_bin;
   const auto group_idx = global_partition.bin_groups[curr_bin];
   if (group_idx >= global_partition.hash_groups.size()) {
    hash_group = nullptr;
    right_outer = nullptr;
    right.reset();
    continue;
   }
   hash_group = global_partition.hash_groups[group_idx].get();
   right_outer = gsink.right_outers.data() + group_idx;
   right = make_uniq<SBIterator>(*(hash_group->global_sort), ExpressionType::COMPARE_LESSTHANOREQUALTO);
  }
  left.SetIndex(i);
  if (!right->Compare(left)) {
   continue;
  }
  idx_t bound = 1;
  idx_t begin = right->GetIndex();
  right->SetIndex(begin + bound);
  while (right->GetIndex() < hash_group->count) {
   if (right->Compare(left)) {
    bound *= 2;
    right->SetIndex(begin + bound);
   } else {
    break;
   }
  }
  auto first = begin + bound / 2;
  auto last = MinValue<idx_t>(begin + bound, hash_group->count);
  while (first < last) {
   const auto mid = first + (last - first) / 2;
   right->SetIndex(mid);
   if (right->Compare(left)) {
    first = mid + 1;
   } else {
    last = mid;
   }
  }
  right->SetIndex(--first);
  if (!op.lhs_partitions.empty() && hash_group->ComparePartitions(left, *right)) {
   continue;
  }
  right_outer->SetMatch(first);
  left_outer.SetMatch(idx);
  if (found_match) {
   found_match[idx] = true;
  }
  if (matches) {
   matches[idx] = Match(curr_bin, first);
  }
  lhs_matched.set_index(lhs_match_count++, idx);
 }
}
unique_ptr<OperatorState> PhysicalAsOfJoin::GetOperatorState(ExecutionContext &context) const {
 auto &config = ClientConfig::GetConfig(context.client);
 return make_uniq<AsOfLocalState>(context.client, *this, config.force_external);
}
void PhysicalAsOfJoin::ResolveSimpleJoin(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                         OperatorState &lstate_p) const {
 auto &lstate = lstate_p.Cast<AsOfLocalState>();
 auto &gsink = sink_state->Cast<AsOfGlobalSinkState>();
 bool found_match[STANDARD_VECTOR_SIZE] = {false};
 lstate.ResolveJoin(input, found_match);
 switch (join_type) {
 case JoinType::MARK: {
  PhysicalJoin::ConstructMarkJoinResult(lstate.lhs_keys, input, chunk, found_match, gsink.has_null);
  break;
 }
 case JoinType::SEMI:
  PhysicalJoin::ConstructSemiJoinResult(input, chunk, found_match);
  break;
 case JoinType::ANTI:
  PhysicalJoin::ConstructAntiJoinResult(input, chunk, found_match);
  break;
 default:
  throw NotImplementedException("Unimplemented join type for AsOf join");
 }
}
OperatorResultType PhysicalAsOfJoin::ResolveComplexJoin(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                        OperatorState &lstate_p) const {
 auto &lstate = lstate_p.Cast<AsOfLocalState>();
 auto &gsink = sink_state->Cast<AsOfGlobalSinkState>();
 if (!lstate.fetch_next_left) {
  lstate.fetch_next_left = true;
  if (lstate.left_outer.Enabled()) {
   lstate.left_outer.ConstructLeftJoinResult(input, chunk);
   lstate.left_outer.Reset();
  }
  return OperatorResultType::NEED_MORE_INPUT;
 }
 AsOfLocalState::Match matches[STANDARD_VECTOR_SIZE];
 lstate.ResolveJoin(input, nullptr, matches);
 lstate.group_payload.Reset();
 lstate.rhs_payload.Reset();
 auto &global_partition = gsink.global_partition;
 hash_t scan_bin = global_partition.bin_groups.size();
 optional_ptr<PartitionGlobalHashGroup> hash_group;
 unique_ptr<PayloadScanner> scanner;
 for (idx_t i = 0; i < lstate.lhs_match_count; ++i) {
  const auto idx = lstate.lhs_matched[i];
  const auto match_bin = matches[idx].first;
  const auto match_pos = matches[idx].second;
  if (match_bin != scan_bin) {
   const auto group_idx = global_partition.bin_groups[match_bin];
   hash_group = global_partition.hash_groups[group_idx].get();
   scan_bin = match_bin;
   scanner = make_uniq<PayloadScanner>(*hash_group->global_sort, false);
   lstate.group_payload.Reset();
  }
  while (match_pos >= scanner->Scanned()) {
   lstate.group_payload.Reset();
   scanner->Scan(lstate.group_payload);
  }
  const auto source_offset = match_pos - (scanner->Scanned() - lstate.group_payload.size());
  for (idx_t col_idx = 0; col_idx < right_projection_map.size(); ++col_idx) {
   const auto rhs_idx = right_projection_map[col_idx];
   auto &source = lstate.group_payload.data[rhs_idx];
   auto &target = chunk.data[input.ColumnCount() + col_idx];
   VectorOperations::Copy(source, target, source_offset + 1, source_offset, i);
  }
 }
 chunk.Slice(input, lstate.lhs_matched, lstate.lhs_match_count);
 if (lstate.left_outer.Enabled()) {
  lstate.fetch_next_left = false;
  return OperatorResultType::HAVE_MORE_OUTPUT;
>>>>>>> 7dafab81
 }
 return OperatorResultType::NEED_MORE_INPUT;
}
OperatorResultType PhysicalAsOfJoin::ExecuteInternal(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                     GlobalOperatorState &gstate, OperatorState &lstate_p) const {
 auto &gsink = sink_state->Cast<AsOfGlobalSinkState>();
 auto &lstate = lstate_p.Cast<AsOfLocalState>();
 if (gsink.rhs_sink.count == 0) {
  if (!EmptyResultIfRHSIsEmpty()) {
   ConstructEmptyJoinResult(join_type, gsink.has_null, input, chunk);
   return OperatorResultType::NEED_MORE_INPUT;
  } else {
   return OperatorResultType::FINISHED;
  }
 }
 return lstate.ExecuteInternal(context, input, chunk);
}
class AsOfProbeBuffer {
public:
 using Orders = vector<BoundOrderByNode>;
 static bool IsExternal(ClientContext &context) {
  return ClientConfig::GetConfig(context).force_external;
 }
 AsOfProbeBuffer(ClientContext &context, const PhysicalAsOfJoin &op);
public:
 void ResolveJoin(bool *found_matches, idx_t *matches = nullptr);
 bool Scanning() const {
  return lhs_scanner.get();
 }
 void BeginLeftScan(hash_t scan_bin);
 bool NextLeft();
 void EndScan();
 void ResolveSimpleJoin(ExecutionContext &context, DataChunk &chunk);
 void ResolveComplexJoin(ExecutionContext &context, DataChunk &chunk);
 void GetData(ExecutionContext &context, DataChunk &chunk);
 bool HasMoreData() const {
  return !fetch_next_left || (lhs_scanner && lhs_scanner->Remaining());
 }
 ClientContext &context;
 Allocator &allocator;
 const PhysicalAsOfJoin &op;
 BufferManager &buffer_manager;
 const bool force_external;
 const idx_t memory_per_thread;
 Orders lhs_orders;
 SelectionVector lhs_sel;
 optional_ptr<PartitionGlobalHashGroup> left_hash;
 OuterJoinMarker left_outer;
 unique_ptr<SBIterator> left_itr;
 unique_ptr<PayloadScanner> lhs_scanner;
 DataChunk lhs_payload;
 optional_ptr<PartitionGlobalHashGroup> right_hash;
 optional_ptr<OuterJoinMarker> right_outer;
 unique_ptr<SBIterator> right_itr;
 unique_ptr<PayloadScanner> rhs_scanner;
 DataChunk rhs_payload;
 idx_t lhs_match_count;
 bool fetch_next_left;
};
AsOfProbeBuffer::AsOfProbeBuffer(ClientContext &context, const PhysicalAsOfJoin &op)
    : context(context), allocator(Allocator::Get(context)), op(op),
      buffer_manager(BufferManager::GetBufferManager(context)), force_external(IsExternal(context)),
      memory_per_thread(op.GetMaxThreadMemory(context)), left_outer(IsLeftOuterJoin(op.join_type)),
      fetch_next_left(true) {
 vector<unique_ptr<BaseStatistics>> partition_stats;
 Orders partitions;
 PartitionGlobalSinkState::GenerateOrderings(partitions, lhs_orders, op.lhs_partitions, op.lhs_orders,
                                             partition_stats);
 lhs_payload.Initialize(allocator, op.children[0]->types);
 rhs_payload.Initialize(allocator, op.children[1]->types);
 lhs_sel.Initialize();
 left_outer.Initialize(STANDARD_VECTOR_SIZE);
}
void AsOfProbeBuffer::BeginLeftScan(hash_t scan_bin) {
 auto &gsink = op.sink_state->Cast<AsOfGlobalSinkState>();
 auto &lhs_sink = *gsink.lhs_sink;
 const auto left_group = lhs_sink.bin_groups[scan_bin];
 if (left_group >= lhs_sink.bin_groups.size()) {
  return;
 }
 left_hash = lhs_sink.hash_groups[left_group].get();
 auto &left_sort = *(left_hash->global_sort);
 lhs_scanner = make_uniq<PayloadScanner>(left_sort, false);
 left_itr = make_uniq<SBIterator>(left_sort, ExpressionType::COMPARE_LESSTHANOREQUALTO);
 auto &rhs_sink = gsink.rhs_sink;
 const auto right_group = rhs_sink.bin_groups[scan_bin];
 if (right_group < rhs_sink.bin_groups.size()) {
  right_hash = rhs_sink.hash_groups[right_group].get();
  right_outer = gsink.right_outers.data() + right_group;
  auto &right_sort = *(right_hash->global_sort);
  right_itr = make_uniq<SBIterator>(right_sort, ExpressionType::COMPARE_LESSTHANOREQUALTO);
  rhs_scanner = make_uniq<PayloadScanner>(right_sort, false);
 }
}
bool AsOfProbeBuffer::NextLeft() {
 if (!HasMoreData()) {
  return false;
 }
 lhs_payload.Reset();
 left_itr->SetIndex(lhs_scanner->Scanned());
 lhs_scanner->Scan(lhs_payload);
 return true;
}
void AsOfProbeBuffer::EndScan() {
 right_hash = nullptr;
 right_itr.reset();
 rhs_scanner.reset();
 right_outer = nullptr;
 left_hash = nullptr;
 left_itr.reset();
 lhs_scanner.reset();
}
void AsOfProbeBuffer::ResolveJoin(bool *found_match, idx_t *matches) {
 lhs_match_count = 0;
 left_outer.Reset();
 if (!right_itr) {
  return;
 }
 const auto count = lhs_payload.size();
 const auto left_base = left_itr->GetIndex();
 for (idx_t i = 0; i < count; ++i) {
  left_itr->SetIndex(left_base + i);
  if (!right_itr->Compare(*left_itr)) {
   continue;
  }
  idx_t bound = 1;
  idx_t begin = right_itr->GetIndex();
  right_itr->SetIndex(begin + bound);
  while (right_itr->GetIndex() < right_hash->count) {
   if (right_itr->Compare(*left_itr)) {
    bound *= 2;
    right_itr->SetIndex(begin + bound);
   } else {
    break;
   }
  }
  auto first = begin + bound / 2;
  auto last = MinValue<idx_t>(begin + bound, right_hash->count);
  while (first < last) {
   const auto mid = first + (last - first) / 2;
   right_itr->SetIndex(mid);
   if (right_itr->Compare(*left_itr)) {
    first = mid + 1;
   } else {
    last = mid;
   }
  }
  right_itr->SetIndex(--first);
  if (right_hash->ComparePartitions(*left_itr, *right_itr)) {
   continue;
  }
  right_outer->SetMatch(first);
  left_outer.SetMatch(i);
  if (found_match) {
   found_match[i] = true;
  }
  if (matches) {
   matches[i] = first;
  }
  lhs_sel.set_index(lhs_match_count++, i);
 }
}
unique_ptr<OperatorState> PhysicalAsOfJoin::GetOperatorState(ExecutionContext &context) const {
 return make_uniq<AsOfLocalState>(context.client, *this);
}
void AsOfProbeBuffer::ResolveSimpleJoin(ExecutionContext &context, DataChunk &chunk) {
 bool found_match[STANDARD_VECTOR_SIZE] = {false};
 ResolveJoin(found_match);
 switch (op.join_type) {
 case JoinType::SEMI:
  PhysicalJoin::ConstructSemiJoinResult(lhs_payload, chunk, found_match);
  break;
 case JoinType::ANTI:
  PhysicalJoin::ConstructAntiJoinResult(lhs_payload, chunk, found_match);
  break;
 default:
  throw NotImplementedException("Unimplemented join type for AsOf join");
 }
}
void AsOfProbeBuffer::ResolveComplexJoin(ExecutionContext &context, DataChunk &chunk) {
 idx_t matches[STANDARD_VECTOR_SIZE];
 ResolveJoin(nullptr, matches);
 for (idx_t i = 0; i < lhs_match_count; ++i) {
  const auto idx = lhs_sel[i];
  const auto match_pos = matches[idx];
  while (match_pos >= rhs_scanner->Scanned()) {
   rhs_payload.Reset();
   rhs_scanner->Scan(rhs_payload);
  }
  const auto source_offset = match_pos - (rhs_scanner->Scanned() - rhs_payload.size());
  for (column_t col_idx = 0; col_idx < op.right_projection_map.size(); ++col_idx) {
   const auto rhs_idx = op.right_projection_map[col_idx];
   auto &source = rhs_payload.data[rhs_idx];
   auto &target = chunk.data[lhs_payload.ColumnCount() + col_idx];
   VectorOperations::Copy(source, target, source_offset + 1, source_offset, i);
  }
 }
 for (column_t i = 0; i < lhs_payload.ColumnCount(); ++i) {
  chunk.data[i].Slice(lhs_payload.data[i], lhs_sel, lhs_match_count);
 }
 chunk.SetCardinality(lhs_match_count);
 fetch_next_left = !left_outer.Enabled();
}
void AsOfProbeBuffer::GetData(ExecutionContext &context, DataChunk &chunk) {
 if (!fetch_next_left) {
  fetch_next_left = true;
  if (left_outer.Enabled()) {
   left_outer.ConstructLeftJoinResult(lhs_payload, chunk);
   left_outer.Reset();
  }
  return;
 }
 if (!NextLeft()) {
  return;
 }
 switch (op.join_type) {
 case JoinType::SEMI:
 case JoinType::ANTI:
 case JoinType::MARK:
  ResolveSimpleJoin(context, chunk);
  break;
 case JoinType::LEFT:
 case JoinType::INNER:
 case JoinType::RIGHT:
 case JoinType::OUTER:
  ResolveComplexJoin(context, chunk);
  break;
 default:
  throw NotImplementedException("Unimplemented type for as-of join!");
 }
}
class AsOfGlobalSourceState : public GlobalSourceState {
public:
 explicit AsOfGlobalSourceState(AsOfGlobalSinkState &gsink_p)
     : gsink(gsink_p), next_combine(0), combined(0), merged(0), mergers(0), next_left(0), flushed(0), next_right(0) {
 }
 PartitionGlobalMergeStates &GetMergeStates() {
  lock_guard<mutex> guard(lock);
  if (!merge_states) {
   merge_states = make_uniq<PartitionGlobalMergeStates>(*gsink.lhs_sink);
  }
  return *merge_states;
 }
 AsOfGlobalSinkState &gsink;
 atomic<size_t> next_combine;
 atomic<size_t> combined;
 atomic<size_t> merged;
 atomic<size_t> mergers;
 atomic<size_t> next_left;
 atomic<size_t> flushed;
 atomic<idx_t> next_right;
 mutex lock;
 unique_ptr<PartitionGlobalMergeStates> merge_states;
public:
 idx_t MaxThreads() override {
  return gsink.lhs_buffers.size();
 }
};
unique_ptr<GlobalSourceState> PhysicalAsOfJoin::GetGlobalSourceState(ClientContext &context) const {
 auto &gsink = sink_state->Cast<AsOfGlobalSinkState>();
 return make_uniq<AsOfGlobalSourceState>(gsink);
}
class AsOfLocalSourceState : public LocalSourceState {
public:
 using HashGroupPtr = unique_ptr<PartitionGlobalHashGroup>;
 AsOfLocalSourceState(AsOfGlobalSourceState &gsource, const PhysicalAsOfJoin &op);
 void CombineLeftPartitions();
 void MergeLeftPartitions();
 idx_t BeginRightScan(const idx_t hash_bin);
 AsOfGlobalSourceState &gsource;
 AsOfProbeBuffer probe_buffer;
 idx_t hash_bin;
 HashGroupPtr hash_group;
 unique_ptr<PayloadScanner> scanner;
 const bool *found_match;
};
AsOfLocalSourceState::AsOfLocalSourceState(AsOfGlobalSourceState &gsource, const PhysicalAsOfJoin &op)
    : gsource(gsource), probe_buffer(gsource.gsink.lhs_sink->context, op) {
 gsource.mergers++;
}
void AsOfLocalSourceState::CombineLeftPartitions() {
 const auto buffer_count = gsource.gsink.lhs_buffers.size();
 while (gsource.combined < buffer_count) {
  const auto next_combine = gsource.next_combine++;
  if (next_combine < buffer_count) {
   gsource.gsink.lhs_buffers[next_combine]->Combine();
   ++gsource.combined;
  } else {
   std::this_thread::yield();
  }
 }
}
void AsOfLocalSourceState::MergeLeftPartitions() {
 PartitionGlobalMergeStates::Callback local_callback;
 PartitionLocalMergeState local_merge;
 gsource.GetMergeStates().ExecuteTask(local_merge, local_callback);
 gsource.merged++;
 while (gsource.merged < gsource.mergers) {
  std::this_thread::yield();
 }
}
idx_t AsOfLocalSourceState::BeginRightScan(const idx_t hash_bin_p) {
 hash_bin = hash_bin_p;
 hash_group = std::move(gsource.gsink.rhs_sink.hash_groups[hash_bin]);
 scanner = make_uniq<PayloadScanner>(*hash_group->global_sort);
 found_match = gsource.gsink.right_outers[hash_bin].GetMatches();
 return scanner->Remaining();
}
unique_ptr<LocalSourceState> PhysicalAsOfJoin::GetLocalSourceState(ExecutionContext &context,
                                                                   GlobalSourceState &gstate) const {
 auto &gsource = gstate.Cast<AsOfGlobalSourceState>();
 return make_uniq<AsOfLocalSourceState>(gsource, *this);
}
SourceResultType PhysicalAsOfJoin::GetData(ExecutionContext &context, DataChunk &chunk,
                                           OperatorSourceInput &input) const {
 auto &gsource = input.global_state.Cast<AsOfGlobalSourceState>();
 auto &lsource = input.local_state.Cast<AsOfLocalSourceState>();
 auto &rhs_sink = gsource.gsink.rhs_sink;
 lsource.CombineLeftPartitions();
 lsource.MergeLeftPartitions();
 auto &lhs_sink = *gsource.gsink.lhs_sink;
 auto &partitions = lhs_sink.grouping_data->GetPartitions();
 const auto left_bins = partitions.size();
 while (gsource.flushed < left_bins) {
  if (!lsource.probe_buffer.Scanning()) {
   const auto left_bin = gsource.next_left++;
   if (left_bin < left_bins) {
    lsource.probe_buffer.BeginLeftScan(left_bin);
   } else if (!IsRightOuterJoin(join_type)) {
    return SourceResultType::FINISHED;
   } else {
    std::this_thread::yield();
    continue;
   }
  }
  lsource.probe_buffer.GetData(context, chunk);
  if (chunk.size()) {
   return SourceResultType::HAVE_MORE_OUTPUT;
  } else if (lsource.probe_buffer.HasMoreData()) {
   continue;
  } else {
   lsource.probe_buffer.EndScan();
   gsource.flushed++;
  }
 }
 if (!IsRightOuterJoin(join_type)) {
  return SourceResultType::FINISHED;
 }
 auto &hash_groups = rhs_sink.hash_groups;
 const auto right_groups = hash_groups.size();
 DataChunk rhs_chunk;
 rhs_chunk.Initialize(Allocator::Get(context.client), rhs_sink.payload_types);
 SelectionVector rsel(STANDARD_VECTOR_SIZE);
 while (chunk.size() == 0) {
  while (!lsource.scanner || !lsource.scanner->Remaining()) {
   lsource.scanner.reset();
   lsource.hash_group.reset();
   auto hash_bin = gsource.next_right++;
   if (hash_bin >= right_groups) {
    return SourceResultType::FINISHED;
   }
   for (; hash_bin < hash_groups.size(); hash_bin = gsource.next_right++) {
    if (hash_groups[hash_bin]) {
     break;
    }
   }
   lsource.BeginRightScan(hash_bin);
  }
  const auto rhs_position = lsource.scanner->Scanned();
  lsource.scanner->Scan(rhs_chunk);
  const auto count = rhs_chunk.size();
  if (count == 0) {
   return SourceResultType::FINISHED;
  }
  auto found_match = lsource.found_match;
  idx_t result_count = 0;
  for (idx_t i = 0; i < count; i++) {
   if (!found_match[rhs_position + i]) {
    rsel.set_index(result_count++, i);
   }
  }
  if (result_count > 0) {
   const idx_t left_column_count = children[0]->types.size();
   for (idx_t col_idx = 0; col_idx < left_column_count; ++col_idx) {
    chunk.data[col_idx].SetVectorType(VectorType::CONSTANT_VECTOR);
    ConstantVector::SetNull(chunk.data[col_idx], true);
   }
   for (idx_t col_idx = 0; col_idx < right_projection_map.size(); ++col_idx) {
    const auto rhs_idx = right_projection_map[col_idx];
    chunk.data[left_column_count + col_idx].Slice(rhs_chunk.data[rhs_idx], rsel, result_count);
   }
   chunk.SetCardinality(result_count);
   break;
  }
 }
 return chunk.size() > 0 ? SourceResultType::HAVE_MORE_OUTPUT : SourceResultType::FINISHED;
}
}
