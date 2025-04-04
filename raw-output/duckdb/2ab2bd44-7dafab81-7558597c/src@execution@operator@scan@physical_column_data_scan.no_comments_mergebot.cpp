#include "duckdb/execution/operator/scan/physical_column_data_scan.hpp"
#include "duckdb/execution/operator/join/physical_delim_join.hpp"
#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"
#include "duckdb/execution/operator/set/physical_cte.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"
namespace duckdb {
PhysicalColumnDataScan::PhysicalColumnDataScan(vector<LogicalType> types, PhysicalOperatorType op_type,
                                               idx_t estimated_cardinality,
                                               unique_ptr<ColumnDataCollection> owned_collection_p)
    : PhysicalOperator(op_type, std::move(types), estimated_cardinality), collection(owned_collection_p.get()),
      owned_collection(std::move(owned_collection_p)) {
}
class PhysicalColumnDataScanState : public GlobalSourceState {
public:
 explicit PhysicalColumnDataScanState() : initialized(false) {
 }
 ColumnDataScanState scan_state;
 bool initialized;
};
unique_ptr<GlobalSourceState> PhysicalColumnDataScan::GetGlobalSourceState(ClientContext &context) const {
 return make_uniq<PhysicalColumnDataScanState>();
}
SourceResultType PhysicalColumnDataScan::GetData(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &input) const {
 auto &state = input.global_state.Cast<PhysicalColumnDataScanState>();
 if (collection->Count() == 0) {
  return SourceResultType::FINISHED;
 }
 if (!state.initialized) {
  collection->InitializeScan(state.scan_state);
  state.initialized = true;
 }
 collection->Scan(state.scan_state, chunk);
 return chunk.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}
void PhysicalColumnDataScan::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
 auto &state = meta_pipeline.GetState();
 switch (type) {
 case PhysicalOperatorType::DELIM_SCAN: {
  auto entry = state.delim_join_dependencies.find(*this);
  D_ASSERT(entry != state.delim_join_dependencies.end());
  auto delim_dependency = entry->second.get().shared_from_this();
  auto delim_sink = state.GetPipelineSink(*delim_dependency);
  D_ASSERT(delim_sink);
  D_ASSERT(delim_sink->type == PhysicalOperatorType::DELIM_JOIN);
  auto &delim_join = delim_sink->Cast<PhysicalDelimJoin>();
  current.AddDependency(delim_dependency);
  state.SetPipelineSource(current, delim_join.distinct->Cast<PhysicalOperator>());
  return;
 }
 case PhysicalOperatorType::CTE_SCAN: {
  break;
 }
 case PhysicalOperatorType::RECURSIVE_CTE_SCAN:
  if (!meta_pipeline.HasRecursiveCTE()) {
   throw InternalException("Recursive CTE scan found without recursive CTE node");
  }
  break;
 default:
  break;
 }
 D_ASSERT(children.empty());
 state.SetPipelineSource(current, *this);
}
string PhysicalColumnDataScan::ParamsToString() const {
 string result = "";
 switch (type) {
 case PhysicalOperatorType::CTE_SCAN:
 case PhysicalOperatorType::RECURSIVE_CTE_SCAN: {
  result += "\n[INFOSEPARATOR]\n";
  result += StringUtil::Format("idx: %llu", cte_index);
  break;
 }
 default:
  break;
 }
 return result;
}
}
