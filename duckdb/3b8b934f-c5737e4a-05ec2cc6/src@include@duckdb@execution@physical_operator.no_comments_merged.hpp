       
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/operator_result_type.hpp"
#include "duckdb/common/enums/physical_operator_type.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/optimizer/join_order/join_node.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/execution/physical_operator_states.hpp"
#include "duckdb/common/enums/order_preservation_type.hpp"
namespace duckdb {
class Event;
class Executor;
class PhysicalOperator;
class Pipeline;
class PipelineBuildState;
class MetaPipeline;
class PhysicalOperator {
public:
 PhysicalOperator(PhysicalOperatorType type, vector<LogicalType> types, idx_t estimated_cardinality)
     : type(type), types(std::move(types)), estimated_cardinality(estimated_cardinality) {
  estimated_props = make_uniq<EstimatedProperties>(estimated_cardinality, 0);
 }
 virtual ~PhysicalOperator() {
 }
 PhysicalOperatorType type;
 vector<unique_ptr<PhysicalOperator>> children;
 vector<LogicalType> types;
 idx_t estimated_cardinality;
 unique_ptr<EstimatedProperties> estimated_props;
 unique_ptr<GlobalSinkState> sink_state;
 unique_ptr<GlobalOperatorState> op_state;
 mutex lock;
public:
 virtual string GetName() const;
 virtual string ParamsToString() const {
  return "";
 }
 virtual string ToString() const;
 void Print() const;
 virtual vector<const_reference<PhysicalOperator>> GetChildren() const;
 const vector<LogicalType> &GetTypes() const {
  return types;
 }
 virtual bool Equals(const PhysicalOperator &other) const {
  return false;
 }
 virtual void Verify();
public:
 virtual unique_ptr<OperatorState> GetOperatorState(ExecutionContext &context) const;
 virtual unique_ptr<GlobalOperatorState> GetGlobalOperatorState(ClientContext &context) const;
 virtual OperatorResultType Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                    GlobalOperatorState &gstate, OperatorState &state) const;
 virtual OperatorFinalizeResultType FinalExecute(ExecutionContext &context, DataChunk &chunk,
                                                 GlobalOperatorState &gstate, OperatorState &state) const;
 virtual bool ParallelOperator() const {
  return false;
 }
 virtual bool RequiresFinalExecute() const {
  return false;
 }
 virtual OrderPreservationType OperatorOrder() const {
  return OrderPreservationType::INSERTION_ORDER;
 }
public:
 virtual unique_ptr<LocalSourceState> GetLocalSourceState(ExecutionContext &context,
                                                          GlobalSourceState &gstate) const;
 virtual unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const;
 virtual void GetData(ExecutionContext &context, DataChunk &chunk, GlobalSourceState &gstate,
                      LocalSourceState &lstate) const;
 virtual idx_t GetBatchIndex(ExecutionContext &context, DataChunk &chunk, GlobalSourceState &gstate,
                             LocalSourceState &lstate) const;
 virtual bool IsSource() const {
  return false;
 }
 virtual bool ParallelSource() const {
  return false;
 }
 virtual bool SupportsBatchIndex() const {
  return false;
 }
 virtual OrderPreservationType SourceOrder() const {
  return OrderPreservationType::INSERTION_ORDER;
 }
 virtual double GetProgress(ClientContext &context, GlobalSourceState &gstate) const;
public:
 virtual SinkResultType Sink(ExecutionContext &context, GlobalSinkState &gstate, LocalSinkState &lstate,
                             DataChunk &input) const;
 virtual void Combine(ExecutionContext &context, GlobalSinkState &gstate, LocalSinkState &lstate) const;
 virtual SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                   GlobalSinkState &gstate) const;
 virtual unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const;
 virtual unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const;
 static idx_t GetMaxThreadMemory(ClientContext &context);
 virtual bool IsSink() const {
  return false;
 }
 virtual bool ParallelSink() const {
  return false;
 }
 virtual bool RequiresBatchIndex() const {
  return false;
 }
 virtual bool SinkOrderDependent() const {
  return false;
 }
public:
 virtual vector<const_reference<PhysicalOperator>> GetSources() const;
 bool AllSourcesSupportBatchIndex() const;
 virtual void BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline);
public:
 template <class TARGET>
 TARGET &Cast() {
  if (TARGET::TYPE != PhysicalOperatorType::INVALID && type != TARGET::TYPE) {
   throw InternalException("Failed to cast physical operator to type - physical operator type mismatch");
  }
  return (TARGET &)*this;
 }
 template <class TARGET>
 const TARGET &Cast() const {
  if (TARGET::TYPE != PhysicalOperatorType::INVALID && type != TARGET::TYPE) {
   throw InternalException("Failed to cast physical operator to type - physical operator type mismatch");
  }
  return (const TARGET &)*this;
 }
};
class CachingOperatorState : public OperatorState {
public:
 ~CachingOperatorState() override {
 }
 void Finalize(const PhysicalOperator &op, ExecutionContext &context) override {
 }
 unique_ptr<DataChunk> cached_chunk;
 bool initialized = false;
 bool can_cache_chunk = false;
};
class CachingPhysicalOperator : public PhysicalOperator {
public:
 static constexpr const idx_t CACHE_THRESHOLD = 64;
 CachingPhysicalOperator(PhysicalOperatorType type, vector<LogicalType> types, idx_t estimated_cardinality);
 bool caching_supported;
public:
 OperatorResultType Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                            GlobalOperatorState &gstate, OperatorState &state) const final;
 OperatorFinalizeResultType FinalExecute(ExecutionContext &context, DataChunk &chunk, GlobalOperatorState &gstate,
                                         OperatorState &state) const final;
 bool RequiresFinalExecute() const final {
  return caching_supported;
 }
protected:
 virtual OperatorResultType ExecuteInternal(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                            GlobalOperatorState &gstate, OperatorState &state) const = 0;
private:
 bool CanCacheType(const LogicalType &type);
};
}
