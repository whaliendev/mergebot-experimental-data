#include "duckdb/execution/operator/helper/physical_batch_collector.hpp"
#include "duckdb/common/types/batched_data_collection.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/main/client_context.hpp"
namespace duckdb {
PhysicalBatchCollector::PhysicalBatchCollector(PreparedStatementData &data) : PhysicalResultCollector(data) {
}
class BatchCollectorGlobalState : public GlobalSinkState {
public:
 BatchCollectorGlobalState(ClientContext &context, const PhysicalBatchCollector &op) : data(op.types) {
 }
 mutex glock;
 BatchedDataCollection data;
 unique_ptr<MaterializedQueryResult> result;
};
class BatchCollectorLocalState : public LocalSinkState {
public:
 BatchCollectorLocalState(ClientContext &context, const PhysicalBatchCollector &op) : data(op.types) {
 }
 BatchedDataCollection data;
};
 SinkResultType PhysicalBatchCollector::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const { auto &state = input.local_state.Cast<BatchCollectorLocalState>(); state.data.Append(chunk, state.partition_info.batch_index.GetIndex());
 return SinkResultType::NEED_MORE_INPUT;
}
void PhysicalBatchCollector::Combine(ExecutionContext &context, GlobalSinkState &gstate_p,
                                     LocalSinkState &lstate_p) const {
 auto &gstate = gstate_p.Cast<BatchCollectorGlobalState>();
 auto &state = lstate_p.Cast<BatchCollectorLocalState>();
 lock_guard<mutex> lock(gstate.glock);
 gstate.data.Merge(state.data);
}
SinkFinalizeType PhysicalBatchCollector::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                                  GlobalSinkState &gstate_p) const {
 auto &gstate = gstate_p.Cast<BatchCollectorGlobalState>();
 auto collection = gstate.data.FetchCollection();
 D_ASSERT(collection);
 auto result = make_uniq<MaterializedQueryResult>(statement_type, properties, names, std::move(collection),
                                                  context.GetClientProperties());
 gstate.result = std::move(result);
 return SinkFinalizeType::READY;
}
unique_ptr<LocalSinkState> PhysicalBatchCollector::GetLocalSinkState(ExecutionContext &context) const {
 return make_uniq<BatchCollectorLocalState>(context.client, *this);
}
unique_ptr<GlobalSinkState> PhysicalBatchCollector::GetGlobalSinkState(ClientContext &context) const {
 return make_uniq<BatchCollectorGlobalState>(context, *this);
}
unique_ptr<QueryResult> PhysicalBatchCollector::GetResult(GlobalSinkState &state) {
 auto &gstate = state.Cast<BatchCollectorGlobalState>();
 D_ASSERT(gstate.result);
 return std::move(gstate.result);
}
}
