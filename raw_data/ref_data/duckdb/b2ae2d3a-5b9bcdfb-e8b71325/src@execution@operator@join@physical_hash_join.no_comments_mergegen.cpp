#include "duckdb/execution/operator/join/physical_hash_join.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/parallel/base_pipeline_event.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/storage_manager.hpp"
namespace duckdb {
PhysicalHashJoin::PhysicalHashJoin(LogicalOperator &op, unique_ptr<PhysicalOperator> left,
                                   unique_ptr<PhysicalOperator> right, vector<JoinCondition> cond, JoinType join_type,
                                   const vector<idx_t> &left_projection_map,
                                   const vector<idx_t> &right_projection_map_p, vector<LogicalType> delim_types,
                                   idx_t estimated_cardinality, PerfectHashJoinStats perfect_join_stats)
    : PhysicalComparisonJoin(op, PhysicalOperatorType::HASH_JOIN, std::move(cond), join_type, estimated_cardinality),
      right_projection_map(right_projection_map_p), delim_types(std::move(delim_types)),
      perfect_join_statistics(std::move(perfect_join_stats)) {
 children.push_back(std::move(left));
 children.push_back(std::move(right));
 D_ASSERT(left_projection_map.empty());
 for (auto &condition : conditions) {
  condition_types.push_back(condition.left->return_type);
 }
 if (join_type != JoinType::ANTI && join_type != JoinType::SEMI && join_type != JoinType::MARK) {
  build_types = LogicalOperator::MapTypes(children[1]->GetTypes(), right_projection_map);
 }
}
PhysicalHashJoin::PhysicalHashJoin(LogicalOperator &op, unique_ptr<PhysicalOperator> left,
                                   unique_ptr<PhysicalOperator> right, vector<JoinCondition> cond, JoinType join_type,
                                   idx_t estimated_cardinality, PerfectHashJoinStats perfect_join_state)
    : PhysicalHashJoin(op, std::move(left), std::move(right), std::move(cond), join_type, {}, {}, {},
                       estimated_cardinality, std::move(perfect_join_state)) {
}
class HashJoinGlobalSinkState : public GlobalSinkState {
public:
 HashJoinGlobalSinkState(const PhysicalHashJoin &op, ClientContext &context_p)
     : context(context_p), finalized(false), scanned_data(false) {
  hash_table = op.InitializeHashTable(context);
  perfect_join_executor = make_uniq<PerfectHashJoinExecutor>(op, *hash_table, op.perfect_join_statistics);
  external = ClientConfig::GetConfig(context).force_external;
  const auto &payload_types = op.children[0]->types;
  probe_types.insert(probe_types.end(), op.condition_types.begin(), op.condition_types.end());
  probe_types.insert(probe_types.end(), payload_types.begin(), payload_types.end());
  probe_types.emplace_back(LogicalType::HASH);
 }
 void ScheduleFinalize(Pipeline &pipeline, Event &event);
 void InitializeProbeSpill();
public:
 ClientContext &context;
 unique_ptr<JoinHashTable> hash_table;
 unique_ptr<PerfectHashJoinExecutor> perfect_join_executor;
 bool finalized = false;
 bool external;
 mutex lock;
 vector<unique_ptr<JoinHashTable>> local_hash_tables;
 vector<LogicalType> probe_types;
 unique_ptr<JoinHashTable::ProbeSpill> probe_spill;
 atomic<bool> scanned_data;
};
class HashJoinLocalSinkState : public LocalSinkState {
public:
 HashJoinLocalSinkState(const PhysicalHashJoin &op, ClientContext &context) : build_executor(context) {
  auto &allocator = Allocator::Get(context);
  if (!op.right_projection_map.empty()) {
   build_chunk.Initialize(allocator, op.build_types);
  }
  for (auto &cond : op.conditions) {
   build_executor.AddExpression(*cond.right);
  }
  join_keys.Initialize(allocator, op.condition_types);
  hash_table = op.InitializeHashTable(context);
  hash_table->GetSinkCollection().InitializeAppendState(append_state);
 }
public:
 PartitionedTupleDataAppendState append_state;
 DataChunk build_chunk;
 DataChunk join_keys;
 ExpressionExecutor build_executor;
 unique_ptr<JoinHashTable> hash_table;
};
unique_ptr<JoinHashTable> PhysicalHashJoin::InitializeHashTable(ClientContext &context) const {
 auto result =
auto result = make_uniq<JoinHashTable>(BufferManager::GetBufferManager(context), conditions, build_types, join_type); result->max_ht_size = double(BufferManager::GetBufferManager(context).GetMaxMemory()) * 0.6; if (!delim_types.empty() && join_type == JoinType::MARK) {
 if (!delim_types.empty() && join_type == JoinType::MARK) {
  if (delim_types.size() + 1 == conditions.size()) {
   auto &info = result->correlated_mark_join_info;
   vector<LogicalType> payload_types;
   vector<BoundAggregateExpression *> correlated_aggregates;
   unique_ptr<BoundAggregateExpression> aggr;
   FunctionBinder function_binder(context);
   aggr = function_binder.BindAggregateFunction(CountStarFun::GetFunction(), {}, nullptr,
                                                AggregateType::NON_DISTINCT);
   correlated_aggregates.push_back(&*aggr);
   payload_types.push_back(aggr->return_type);
   info.correlated_aggregates.push_back(std::move(aggr));
   auto count_fun = CountFun::GetFunction();
   vector<unique_ptr<Expression>> children;
   children.push_back(make_uniq_base<Expression, BoundReferenceExpression>(count_fun.return_type, 0));
   aggr = function_binder.BindAggregateFunction(count_fun, std::move(children), nullptr,
                                                AggregateType::NON_DISTINCT);
   correlated_aggregates.push_back(&*aggr);
   payload_types.push_back(aggr->return_type);
   info.correlated_aggregates.push_back(std::move(aggr));
   auto &allocator = Allocator::Get(context);
   info.correlated_counts = make_uniq<GroupedAggregateHashTable>(context, allocator, delim_types,
                                                                 payload_types, correlated_aggregates);
   info.correlated_types = delim_types;
   info.group_chunk.Initialize(allocator, delim_types);
   info.result_chunk.Initialize(allocator, payload_types);
  }
 }
 return result;
}
unique_ptr<GlobalSinkState> PhysicalHashJoin::GetGlobalSinkState(ClientContext &context) const {
 return make_uniq<HashJoinGlobalSinkState>(*this, context);
}
unique_ptr<LocalSinkState> PhysicalHashJoin::GetLocalSinkState(ExecutionContext &context) const {
 return make_uniq<HashJoinLocalSinkState>(*this, context.client);
}
SinkResultType PhysicalHashJoin::Sink(ExecutionContext &context, GlobalSinkState &gstate_p, LocalSinkState &lstate_p,
                                      DataChunk &input) const {
 auto &lstate = (HashJoinLocalSinkState &)lstate_p;
 lstate.join_keys.Reset();
 lstate.build_executor.Execute(input, lstate.join_keys);
 auto &ht = *lstate.hash_table;
 if (!right_projection_map.empty()) {
  lstate.build_chunk.Reset();
  lstate.build_chunk.SetCardinality(input);
  for (idx_t i = 0; i < right_projection_map.size(); i++) {
   lstate.build_chunk.data[i].Reference(input.data[right_projection_map[i]]);
  }
  ht.Build(lstate.append_state, lstate.join_keys, lstate.build_chunk);
 } else if (!build_types.empty()) {
  ht.Build(lstate.append_state, lstate.join_keys, input);
 } else {
  lstate.build_chunk.SetCardinality(input.size());
  ht.Build(lstate.append_state, lstate.join_keys, lstate.build_chunk);
 }
 return SinkResultType::NEED_MORE_INPUT;
}
void PhysicalHashJoin::Combine(ExecutionContext &context, GlobalSinkState &gstate_p, LocalSinkState &lstate_p) const {
 auto &gstate = (HashJoinGlobalSinkState &)gstate_p;
 auto &lstate = (HashJoinLocalSinkState &)lstate_p;
 if (lstate.hash_table) {
  lstate.hash_table->GetSinkCollection().FlushAppendState(lstate.append_state);
  lock_guard<mutex> local_ht_lock(gstate.lock);
  gstate.local_hash_tables.push_back(std::move(lstate.hash_table));
 }
 auto &client_profiler = QueryProfiler::Get(context.client);
 context.thread.profiler.Flush(this, &lstate.build_executor, "build_executor", 1);
 client_profiler.Flush(context.thread.profiler);
}
class HashJoinFinalizeTask : public ExecutorTask {
public:
 HashJoinFinalizeTask(shared_ptr<Event> event_p, ClientContext &context, HashJoinGlobalSinkState &sink_p,
                      idx_t chunk_idx_from_p, idx_t chunk_idx_to_p, bool parallel_p)
     : ExecutorTask(context), event(std::move(event_p)), sink(sink_p), chunk_idx_from(chunk_idx_from_p),
       chunk_idx_to(chunk_idx_to_p), parallel(parallel_p) {
 }
 TaskExecutionResult ExecuteTask(TaskExecutionMode mode) override {
  sink.hash_table->Finalize(chunk_idx_from, chunk_idx_to, parallel);
  event->FinishTask();
  return TaskExecutionResult::TASK_FINISHED;
 }
private:
 shared_ptr<Event> event;
 HashJoinGlobalSinkState &sink;
 idx_t chunk_idx_from;
 idx_t chunk_idx_to;
 bool parallel;
};
class HashJoinFinalizeEvent : public BasePipelineEvent {
public:
 HashJoinFinalizeEvent(Pipeline &pipeline_p, HashJoinGlobalSinkState &sink)
     : BasePipelineEvent(pipeline_p), sink(sink) {
 }
 HashJoinGlobalSinkState &sink;
public:
 void Schedule() override {
  auto &context = pipeline->GetClientContext();
  vector<unique_ptr<Task>> finalize_tasks;
  auto &ht = *sink.hash_table;
  const auto chunk_count = ht.GetDataCollection().ChunkCount();
  const idx_t num_threads = TaskScheduler::GetScheduler(context).NumberOfThreads();
  if (num_threads == 1 || (ht.Count() < PARALLEL_CONSTRUCT_THRESHOLD && !context.config.verify_parallelism)) {
   finalize_tasks.push_back(
  } else {
   auto chunks_per_thread = MaxValue<idx_t>((chunk_count + num_threads - 1) / num_threads, 1);
   idx_t chunk_idx = 0;
   for (idx_t thread_idx = 0; thread_idx < num_threads; thread_idx++) {
auto chunk_idx_from = chunk_idx; auto chunk_idx_to = MinValue<idx_t>(chunk_idx_from + chunks_per_thread, chunk_count); finalize_tasks.push_back(make_uniq<HashJoinFinalizeTask>(shared_from_this(), context, sink, chunk_idx_from, chunk_idx_to, true));
     break;
    }
   }
  }
  SetTasks(std::move(finalize_tasks));
 }
 void FinishEvent() override {
  sink.hash_table->GetDataCollection().VerifyEverythingPinned();
  sink.hash_table->finalized = true;
 }
 static constexpr const idx_t PARALLEL_CONSTRUCT_THRESHOLD = 1048576;
};
void HashJoinGlobalSinkState::ScheduleFinalize(Pipeline &pipeline, Event &event) {
 if (hash_table->Count() == 0) {
  hash_table->finalized = true;
  return;
 }
 hash_table->InitializePointerTable();
 auto new_event = make_shared<HashJoinFinalizeEvent>(pipeline, *this);
 event.InsertEvent(std::move(new_event));
}
void HashJoinGlobalSinkState::InitializeProbeSpill() {
 lock_guard<mutex> guard(lock);
 if (!probe_spill) {
  probe_spill = make_uniq<JoinHashTable::ProbeSpill>(*hash_table, context, probe_types);
 }
}
class HashJoinPartitionTask : public ExecutorTask {
public:
 HashJoinPartitionTask(shared_ptr<Event> event_p, ClientContext &context, JoinHashTable &global_ht,
                       JoinHashTable &local_ht)
     : ExecutorTask(context), event(std::move(event_p)), global_ht(global_ht), local_ht(local_ht) {
 }
 TaskExecutionResult ExecuteTask(TaskExecutionMode mode) override {
  local_ht.Partition(global_ht);
  event->FinishTask();
  return TaskExecutionResult::TASK_FINISHED;
 }
private:
 shared_ptr<Event> event;
 JoinHashTable &global_ht;
 JoinHashTable &local_ht;
};
class HashJoinPartitionEvent : public BasePipelineEvent {
public:
 HashJoinPartitionEvent(Pipeline &pipeline_p, HashJoinGlobalSinkState &sink,
                        vector<unique_ptr<JoinHashTable>> &local_hts)
     : BasePipelineEvent(pipeline_p), sink(sink), local_hts(local_hts) {
 }
 HashJoinGlobalSinkState &sink;
 vector<unique_ptr<JoinHashTable>> &local_hts;
public:
 void Schedule() override {
  auto &context = pipeline->GetClientContext();
  vector<unique_ptr<Task>> partition_tasks;
  partition_tasks.reserve(local_hts.size());
  for (auto &local_ht : local_hts) {
   partition_tasks.push_back(
       make_uniq<HashJoinPartitionTask>(shared_from_this(), context, *sink.hash_table, *local_ht));
  }
  SetTasks(std::move(partition_tasks));
 }
 void FinishEvent() override {
  local_hts.clear();
  sink.hash_table->PrepareExternalFinalize();
  sink.ScheduleFinalize(*pipeline, *this);
 }
};
SinkFinalizeType PhysicalHashJoin::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                            GlobalSinkState &gstate) const {
 auto &sink = (HashJoinGlobalSinkState &)gstate;
 auto &ht = *sink.hash_table;
 sink.external = ht.RequiresExternalJoin(context.config, sink.local_hash_tables);
 if (sink.external) {
  sink.perfect_join_executor.reset();
  if (ht.RequiresPartitioning(context.config, sink.local_hash_tables)) {
   auto new_event = make_shared<HashJoinPartitionEvent>(pipeline, sink, sink.local_hash_tables);
   event.InsertEvent(std::move(new_event));
  } else {
   for (auto &local_ht : sink.local_hash_tables) {
    ht.Merge(*local_ht);
   }
   sink.local_hash_tables.clear();
   sink.hash_table->PrepareExternalFinalize();
   sink.ScheduleFinalize(pipeline, event);
  }
  sink.finalized = true;
  return SinkFinalizeType::READY;
 } else {
  for (auto &local_ht : sink.local_hash_tables) {
   ht.Merge(*local_ht);
  }
  sink.local_hash_tables.clear();
  ht.Unpartition();
 }
 auto use_perfect_hash = sink.perfect_join_executor->CanDoPerfectHashJoin();
 if (use_perfect_hash) {
  D_ASSERT(ht.equality_types.size() == 1);
  auto key_type = ht.equality_types[0];
  use_perfect_hash = sink.perfect_join_executor->BuildPerfectHashTable(key_type);
 }
 if (!use_perfect_hash) {
  sink.perfect_join_executor.reset();
  sink.ScheduleFinalize(pipeline, event);
 }
 sink.finalized = true;
 if (ht.Count() == 0 && EmptyResultIfRHSIsEmpty()) {
  return SinkFinalizeType::NO_OUTPUT_POSSIBLE;
 }
 return SinkFinalizeType::READY;
}
class HashJoinOperatorState : public CachingOperatorState {
public:
 explicit HashJoinOperatorState(ClientContext &context) : probe_executor(context), initialized(false) {
 }
 DataChunk join_keys;
 ExpressionExecutor probe_executor;
 unique_ptr<JoinHashTable::ScanStructure> scan_structure;
 unique_ptr<OperatorState> perfect_hash_join_state;
 bool initialized;
 JoinHashTable::ProbeSpillLocalAppendState spill_state;
 DataChunk spill_chunk;
public:
 void Finalize(PhysicalOperator *op, ExecutionContext &context) override {
  context.thread.profiler.Flush(op, &probe_executor, "probe_executor", 0);
 }
};
unique_ptr<OperatorState> PhysicalHashJoin::GetOperatorState(ExecutionContext &context) const {
 auto &allocator = Allocator::Get(context.client);
 auto &sink = (HashJoinGlobalSinkState &)*sink_state;
 auto state = make_uniq<HashJoinOperatorState>(context.client);
 if (sink.perfect_join_executor) {
  state->perfect_hash_join_state = sink.perfect_join_executor->GetOperatorState(context);
 } else {
  state->join_keys.Initialize(allocator, condition_types);
  for (auto &cond : conditions) {
   state->probe_executor.AddExpression(*cond.left);
  }
 }
 if (sink.external) {
  state->spill_chunk.Initialize(allocator, sink.probe_types);
  sink.InitializeProbeSpill();
 }
 return std::move(state);
}
OperatorResultType PhysicalHashJoin::ExecuteInternal(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                     GlobalOperatorState &gstate, OperatorState &state_p) const {
 auto &state = (HashJoinOperatorState &)state_p;
 auto &sink = (HashJoinGlobalSinkState &)*sink_state;
 D_ASSERT(sink.finalized);
 D_ASSERT(!sink.scanned_data);
 if (sink.external && !state.initialized) {
  if (!sink.probe_spill) {
   sink.InitializeProbeSpill();
  }
  state.spill_state = sink.probe_spill->RegisterThread();
  state.initialized = true;
 }
 if (sink.hash_table->Count() == 0 && EmptyResultIfRHSIsEmpty()) {
  return OperatorResultType::FINISHED;
 }
 if (sink.perfect_join_executor) {
  D_ASSERT(!sink.external);
  return sink.perfect_join_executor->ProbePerfectHashTable(context, input, chunk, *state.perfect_hash_join_state);
 }
 if (state.scan_structure) {
  state.scan_structure->Next(state.join_keys, input, chunk);
  if (chunk.size() > 0) {
   return OperatorResultType::HAVE_MORE_OUTPUT;
  }
  state.scan_structure = nullptr;
  return OperatorResultType::NEED_MORE_INPUT;
 }
 if (sink.hash_table->Count() == 0) {
  ConstructEmptyJoinResult(sink.hash_table->join_type, sink.hash_table->has_null, input, chunk);
  return OperatorResultType::NEED_MORE_INPUT;
 }
 state.join_keys.Reset();
 state.probe_executor.Execute(input, state.join_keys);
 if (sink.external) {
  state.scan_structure = sink.hash_table->ProbeAndSpill(state.join_keys, input, *sink.probe_spill,
                                                        state.spill_state, state.spill_chunk);
 } else {
  state.scan_structure = sink.hash_table->Probe(state.join_keys);
 }
 state.scan_structure->Next(state.join_keys, input, chunk);
 return OperatorResultType::HAVE_MORE_OUTPUT;
}
enum class HashJoinSourceStage : uint8_t { INIT, BUILD, PROBE, SCAN_HT, DONE };
class HashJoinLocalSourceState;
class HashJoinGlobalSourceState : public GlobalSourceState {
public:
 HashJoinGlobalSourceState(const PhysicalHashJoin &op, ClientContext &context);
 void Initialize(HashJoinGlobalSinkState &sink);
 void TryPrepareNextStage(HashJoinGlobalSinkState &sink);
 void PrepareBuild(HashJoinGlobalSinkState &sink);
 void PrepareProbe(HashJoinGlobalSinkState &sink);
 void PrepareScanHT(HashJoinGlobalSinkState &sink);
 bool AssignTask(HashJoinGlobalSinkState &sink, HashJoinLocalSourceState &lstate);
 idx_t MaxThreads() override {
  return probe_count / ((idx_t)STANDARD_VECTOR_SIZE * parallel_scan_chunk_count);
 }
public:
 const PhysicalHashJoin &op;
 atomic<HashJoinSourceStage> global_stage;
 mutex lock;
 idx_t build_chunk_idx;
 idx_t build_chunk_count;
 idx_t build_chunk_done;
 idx_t build_chunks_per_thread;
 idx_t probe_chunk_count;
 idx_t probe_chunk_done;
 idx_t probe_count;
 idx_t parallel_scan_chunk_count;
 idx_t full_outer_chunk_idx;
 idx_t full_outer_chunk_count;
 idx_t full_outer_chunk_done;
 idx_t full_outer_chunks_per_thread;
};
class HashJoinLocalSourceState : public LocalSourceState {
public:
 HashJoinLocalSourceState(const PhysicalHashJoin &op, Allocator &allocator);
 void ExecuteTask(HashJoinGlobalSinkState &sink, HashJoinGlobalSourceState &gstate, DataChunk &chunk);
 bool TaskFinished();
 void ExternalBuild(HashJoinGlobalSinkState &sink, HashJoinGlobalSourceState &gstate);
 void ExternalProbe(HashJoinGlobalSinkState &sink, HashJoinGlobalSourceState &gstate, DataChunk &chunk);
 void ExternalScanHT(HashJoinGlobalSinkState &sink, HashJoinGlobalSourceState &gstate, DataChunk &chunk);
public:
 HashJoinSourceStage local_stage;
 Vector addresses;
 idx_t build_chunk_idx_from;
 idx_t build_chunk_idx_to;
 ColumnDataConsumerScanState probe_local_scan;
 DataChunk probe_chunk;
 DataChunk join_keys;
 DataChunk payload;
 vector<idx_t> join_key_indices;
 vector<idx_t> payload_indices;
 unique_ptr<JoinHashTable::ScanStructure> scan_structure;
 bool empty_ht_probe_in_progress;
 idx_t full_outer_chunk_idx_from;
 idx_t full_outer_chunk_idx_to;
 unique_ptr<JoinHTScanState> full_outer_scan_state;
};
unique_ptr<GlobalSourceState> PhysicalHashJoin::GetGlobalSourceState(ClientContext &context) const {
 return make_uniq<HashJoinGlobalSourceState>(*this, context);
}
unique_ptr<LocalSourceState> PhysicalHashJoin::GetLocalSourceState(ExecutionContext &context,
                                                                   GlobalSourceState &gstate) const {
 return make_uniq<HashJoinLocalSourceState>(*this, Allocator::Get(context.client));
}
HashJoinGlobalSourceState::HashJoinGlobalSourceState(const PhysicalHashJoin &op, ClientContext &context)
    : op(op), global_stage(HashJoinSourceStage::INIT), build_chunk_count(0), build_chunk_done(0), probe_chunk_count(0),
      probe_chunk_done(0), probe_count(op.children[0]->estimated_cardinality),
      parallel_scan_chunk_count(context.config.verify_parallelism ? 1 : 120) {
}
void HashJoinGlobalSourceState::Initialize(HashJoinGlobalSinkState &sink) {
 lock_guard<mutex> init_lock(lock);
 if (global_stage != HashJoinSourceStage::INIT) {
  return;
 }
 if (sink.probe_spill) {
  sink.probe_spill->Finalize();
 }
 global_stage = HashJoinSourceStage::PROBE;
 TryPrepareNextStage(sink);
}
void HashJoinGlobalSourceState::TryPrepareNextStage(HashJoinGlobalSinkState &sink) {
 switch (global_stage.load()) {
 case HashJoinSourceStage::BUILD:
  if (build_chunk_done == build_chunk_count) {
   sink.hash_table->GetDataCollection().VerifyEverythingPinned();
   sink.hash_table->finalized = true;
   PrepareProbe(sink);
  }
  break;
 case HashJoinSourceStage::PROBE:
  if (probe_chunk_done == probe_chunk_count) {
   if (IsRightOuterJoin(op.join_type)) {
    PrepareScanHT(sink);
   } else {
    PrepareBuild(sink);
   }
  }
  break;
 case HashJoinSourceStage::SCAN_HT:
  if (full_outer_chunk_done == full_outer_chunk_count) {
   PrepareBuild(sink);
  }
  break;
 default:
  break;
 }
}
void HashJoinGlobalSourceState::PrepareBuild(HashJoinGlobalSinkState &sink) {
 D_ASSERT(global_stage != HashJoinSourceStage::BUILD);
 auto &ht = *sink.hash_table;
 if (!sink.external || !ht.PrepareExternalFinalize()) {
  global_stage = HashJoinSourceStage::DONE;
  return;
 }
 auto &data_collection = ht.GetDataCollection();
 if (data_collection.Count() == 0 && op.EmptyResultIfRHSIsEmpty()) {
  PrepareBuild(sink);
  return;
 }
 build_chunk_idx = 0;
 build_chunk_count = data_collection.ChunkCount();
 build_chunk_done = 0;
 auto num_threads = TaskScheduler::GetScheduler(sink.context).NumberOfThreads();
 build_chunks_per_thread = MaxValue<idx_t>((build_chunk_count + num_threads - 1) / num_threads, 1);
 ht.InitializePointerTable();
 global_stage = HashJoinSourceStage::BUILD;
}
void HashJoinGlobalSourceState::PrepareProbe(HashJoinGlobalSinkState &sink) {
 sink.probe_spill->PrepareNextProbe();
 const auto &consumer = *sink.probe_spill->consumer;
 probe_chunk_count = consumer.Count() == 0 ? 0 : consumer.ChunkCount();
 probe_chunk_done = 0;
 global_stage = HashJoinSourceStage::PROBE;
 if (probe_chunk_count == 0) {
  TryPrepareNextStage(sink);
  return;
 }
}
void HashJoinGlobalSourceState::PrepareScanHT(HashJoinGlobalSinkState &sink) {
 D_ASSERT(global_stage != HashJoinSourceStage::SCAN_HT);
 auto &ht = *sink.hash_table;
 auto &data_collection = ht.GetDataCollection();
 full_outer_chunk_idx = 0;
 full_outer_chunk_count = data_collection.ChunkCount();
 full_outer_chunk_done = 0;
 auto num_threads = TaskScheduler::GetScheduler(sink.context).NumberOfThreads();
 full_outer_chunks_per_thread = MaxValue<idx_t>((full_outer_chunk_count + num_threads - 1) / num_threads, 1);
 global_stage = HashJoinSourceStage::SCAN_HT;
}
bool HashJoinGlobalSourceState::AssignTask(HashJoinGlobalSinkState &sink, HashJoinLocalSourceState &lstate) {
 D_ASSERT(lstate.TaskFinished());
 lock_guard<mutex> guard(lock);
 switch (global_stage.load()) {
 case HashJoinSourceStage::BUILD:
  if (build_chunk_idx != build_chunk_count) {
   lstate.local_stage = global_stage;
   lstate.build_chunk_idx_from = build_chunk_idx;
   build_chunk_idx = MinValue<idx_t>(build_chunk_count, build_chunk_idx + build_chunks_per_thread);
   lstate.build_chunk_idx_to = build_chunk_idx;
   return true;
  }
  break;
 case HashJoinSourceStage::PROBE:
  if (sink.probe_spill->consumer && sink.probe_spill->consumer->AssignChunk(lstate.probe_local_scan)) {
   lstate.local_stage = global_stage;
   lstate.empty_ht_probe_in_progress = false;
   return true;
  }
  break;
 case HashJoinSourceStage::SCAN_HT:
  if (full_outer_chunk_idx != full_outer_chunk_count) {
   lstate.local_stage = global_stage;
   lstate.full_outer_chunk_idx_from = full_outer_chunk_idx;
   full_outer_chunk_idx =
       MinValue<idx_t>(full_outer_chunk_count, full_outer_chunk_idx + full_outer_chunks_per_thread);
   lstate.full_outer_chunk_idx_to = full_outer_chunk_idx;
   return true;
  }
  break;
 case HashJoinSourceStage::DONE:
  break;
 default:
  throw InternalException("Unexpected HashJoinSourceStage in AssignTask!");
 }
 return false;
}
HashJoinLocalSourceState::HashJoinLocalSourceState(const PhysicalHashJoin &op, Allocator &allocator)
    : local_stage(HashJoinSourceStage::INIT), addresses(LogicalType::POINTER) {
 auto &chunk_state = probe_local_scan.current_chunk_state;
 chunk_state.properties = ColumnDataScanProperties::ALLOW_ZERO_COPY;
 auto &sink = (HashJoinGlobalSinkState &)*op.sink_state;
 probe_chunk.Initialize(allocator, sink.probe_types);
 join_keys.Initialize(allocator, op.condition_types);
 payload.Initialize(allocator, op.children[0]->types);
 idx_t col_idx = 0;
 for (; col_idx < op.condition_types.size(); col_idx++) {
  join_key_indices.push_back(col_idx);
 }
 for (; col_idx < sink.probe_types.size() - 1; col_idx++) {
  payload_indices.push_back(col_idx);
 }
}
void HashJoinLocalSourceState::ExecuteTask(HashJoinGlobalSinkState &sink, HashJoinGlobalSourceState &gstate,
                                           DataChunk &chunk) {
 switch (local_stage) {
 case HashJoinSourceStage::BUILD:
  ExternalBuild(sink, gstate);
  break;
 case HashJoinSourceStage::PROBE:
  ExternalProbe(sink, gstate, chunk);
  break;
 case HashJoinSourceStage::SCAN_HT:
  ExternalScanHT(sink, gstate, chunk);
  break;
 default:
  throw InternalException("Unexpected HashJoinSourceStage in ExecuteTask!");
 }
}
bool HashJoinLocalSourceState::TaskFinished() {
 switch (local_stage) {
 case HashJoinSourceStage::INIT:
 case HashJoinSourceStage::BUILD:
  return true;
 case HashJoinSourceStage::PROBE:
  return scan_structure == nullptr && !empty_ht_probe_in_progress;
 case HashJoinSourceStage::SCAN_HT:
  return full_outer_scan_state == nullptr;
 default:
  throw InternalException("Unexpected HashJoinSourceStage in TaskFinished!");
 }
}
void HashJoinLocalSourceState::ExternalBuild(HashJoinGlobalSinkState &sink, HashJoinGlobalSourceState &gstate) {
 D_ASSERT(local_stage == HashJoinSourceStage::BUILD);
 auto &ht = *sink.hash_table;
 ht.Finalize(build_chunk_idx_from, build_chunk_idx_to, true);
 lock_guard<mutex> guard(gstate.lock);
 gstate.build_chunk_done += build_chunk_idx_to - build_chunk_idx_from;
}
void HashJoinLocalSourceState::ExternalProbe(HashJoinGlobalSinkState &sink, HashJoinGlobalSourceState &gstate,
                                             DataChunk &chunk) {
 D_ASSERT(local_stage == HashJoinSourceStage::PROBE && sink.hash_table->finalized);
 if (scan_structure) {
  scan_structure->Next(join_keys, payload, chunk);
  if (chunk.size() != 0) {
   return;
  }
 }
 if (scan_structure || empty_ht_probe_in_progress) {
  scan_structure = nullptr;
  empty_ht_probe_in_progress = false;
  sink.probe_spill->consumer->FinishChunk(probe_local_scan);
  lock_guard<mutex> lock(gstate.lock);
  gstate.probe_chunk_done++;
  return;
 }
 sink.probe_spill->consumer->ScanChunk(probe_local_scan, probe_chunk);
 join_keys.ReferenceColumns(probe_chunk, join_key_indices);
 payload.ReferenceColumns(probe_chunk, payload_indices);
 auto precomputed_hashes = &probe_chunk.data.back();
 if (sink.hash_table->Count() == 0 && !gstate.op.EmptyResultIfRHSIsEmpty()) {
  gstate.op.ConstructEmptyJoinResult(sink.hash_table->join_type, sink.hash_table->has_null, payload, chunk);
  empty_ht_probe_in_progress = true;
  return;
 }
 scan_structure = sink.hash_table->Probe(join_keys, precomputed_hashes);
 scan_structure->Next(join_keys, payload, chunk);
}
void HashJoinLocalSourceState::ExternalScanHT(HashJoinGlobalSinkState &sink, HashJoinGlobalSourceState &gstate,
                                              DataChunk &chunk) {
 D_ASSERT(local_stage == HashJoinSourceStage::SCAN_HT);
 if (!full_outer_scan_state) {
  full_outer_scan_state = make_unique<JoinHTScanState>(sink.hash_table->GetDataCollection(),
                                                       full_outer_chunk_idx_from, full_outer_chunk_idx_to);
 }
 sink.hash_table->ScanFullOuter(*full_outer_scan_state, addresses, chunk);
 if (chunk.size() == 0) {
  full_outer_scan_state = nullptr;
  lock_guard<mutex> guard(gstate.lock);
  gstate.full_outer_chunk_done += full_outer_chunk_idx_to - full_outer_chunk_idx_from;
 }
}
void PhysicalHashJoin::GetData(ExecutionContext &context, DataChunk &chunk, GlobalSourceState &gstate_p,
                               LocalSourceState &lstate_p) const {
 auto &sink = (HashJoinGlobalSinkState &)*sink_state;
 auto &gstate = (HashJoinGlobalSourceState &)gstate_p;
 auto &lstate = (HashJoinLocalSourceState &)lstate_p;
 sink.scanned_data = true;
 if (!sink.external && !IsRightOuterJoin(join_type)) {
  return;
 }
 if (gstate.global_stage == HashJoinSourceStage::INIT) {
  gstate.Initialize(sink);
 }
 while (gstate.global_stage != HashJoinSourceStage::DONE && chunk.size() == 0) {
  if (!lstate.TaskFinished() || gstate.AssignTask(sink, lstate)) {
   lstate.ExecuteTask(sink, gstate, chunk);
  } else {
   lock_guard<mutex> guard(gstate.lock);
   gstate.TryPrepareNextStage(sink);
  }
 }
}
}
