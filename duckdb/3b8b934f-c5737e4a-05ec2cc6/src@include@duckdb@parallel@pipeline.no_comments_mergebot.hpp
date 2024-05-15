       
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/common/reference_map.hpp"
namespace duckdb {
class Executor;
class Event;
class MetaPipeline;
class PipelineBuildState {
public:
 constexpr static idx_t BATCH_INCREMENT = 10000000000000;
 reference_map_t<const PhysicalOperator, reference<Pipeline>> delim_join_dependencies;
 void SetPipelineSource(Pipeline &pipeline, PhysicalOperator &op);
 void SetPipelineSink(Pipeline &pipeline, optional_ptr<PhysicalOperator> op, idx_t sink_pipeline_count);
 void SetPipelineOperators(Pipeline &pipeline, vector<reference<PhysicalOperator>> operators);
 void AddPipelineOperator(Pipeline &pipeline, PhysicalOperator &op);
 shared_ptr<Pipeline> CreateChildPipeline(Executor &executor, Pipeline &pipeline, PhysicalOperator &op);
 optional_ptr<PhysicalOperator> GetPipelineSource(Pipeline &pipeline);
 optional_ptr<PhysicalOperator> GetPipelineSink(Pipeline &pipeline);
 vector<reference<PhysicalOperator>> GetPipelineOperators(Pipeline &pipeline);
};
class Pipeline : public std::enable_shared_from_this<Pipeline> {
 friend class Executor;
 friend class PipelineExecutor;
 friend class PipelineEvent;
 friend class PipelineFinishEvent;
 friend class PipelineBuildState;
 friend class MetaPipeline;
public:
 explicit Pipeline(Executor &execution_context);
 Executor &executor;
 ClientContext &GetClientContext();
 void AddDependency(shared_ptr<Pipeline> &pipeline);
 void Ready();
 void Reset();
 void ResetSink();
 void ResetSource(bool force);
 void ClearSource();
 void Schedule(shared_ptr<Event> &event);
 void Finalize(Event &event);
 string ToString() const;
 void Print() const;
 void PrintDependencies() const;
 bool GetProgress(double &current_percentage, idx_t &estimated_cardinality);
 vector<reference<PhysicalOperator>> GetOperators();
 vector<const_reference<PhysicalOperator>> GetOperators() const;
 optional_ptr<PhysicalOperator> GetSink() {
  return sink;
 }
 optional_ptr<PhysicalOperator> GetSource() {
  return source;
 }
 bool IsOrderDependent() const;
 idx_t RegisterNewBatchIndex();
 idx_t UpdateBatchIndex(idx_t old_index, idx_t new_index);
private:
 bool ready;
 atomic<bool> initialized;
 optional_ptr<PhysicalOperator> source;
 vector<reference<PhysicalOperator>> operators;
 optional_ptr<PhysicalOperator> sink;
 unique_ptr<GlobalSourceState> source_state;
 vector<weak_ptr<Pipeline>> parents;
 vector<weak_ptr<Pipeline>> dependencies;
 idx_t base_batch_index = 0;
 mutex batch_lock;
 multiset<idx_t> batch_indexes;
 void ScheduleSequentialTask(shared_ptr<Event> &event);
 bool LaunchScanTasks(shared_ptr<Event> &event, idx_t max_threads);
 bool ScheduleParallel(shared_ptr<Event> &event);
};
}
