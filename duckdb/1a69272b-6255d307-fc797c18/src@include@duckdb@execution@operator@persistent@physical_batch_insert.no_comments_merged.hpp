       
#include "duckdb/execution/operator/persistent/physical_insert.hpp"
namespace duckdb {
class PhysicalBatchInsert : public PhysicalOperator {
public:
 static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::BATCH_INSERT;
public:
 PhysicalBatchInsert(vector<LogicalType> types, TableCatalogEntry &table,
                     physical_index_vector_t<idx_t> column_index_map, vector<unique_ptr<Expression>> bound_defaults,
                     idx_t estimated_cardinality);
 PhysicalBatchInsert(LogicalOperator &op, SchemaCatalogEntry &schema, unique_ptr<BoundCreateTableInfo> info,
                     idx_t estimated_cardinality);
 physical_index_vector_t<idx_t> column_index_map;
 optional_ptr<TableCatalogEntry> insert_table;
 vector<LogicalType> insert_types;
 vector<unique_ptr<Expression>> bound_defaults;
 optional_ptr<SchemaCatalogEntry> schema;
 unique_ptr<BoundCreateTableInfo> info;
 OnConflictAction action_type;
public:
 SourceResultType GetData(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const override;
 bool IsSource() const override {
  return true;
 }
public:
 unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
 unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
 void NextBatch(ExecutionContext &context, GlobalSinkState &state, LocalSinkState &lstate_p) const override;
 SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
 void Combine(ExecutionContext &context, GlobalSinkState &gstate, LocalSinkState &lstate) const override;
 SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                           GlobalSinkState &gstate) const override;
 bool RequiresBatchIndex() const override {
  return true;
 }
 bool IsSink() const override {
  return true;
 }
 bool ParallelSink() const override {
  return true;
 }
};
}
