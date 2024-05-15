#ifdef DEBUG
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/list.hpp"
#include "duckdb/common/types/chunk_collection.hpp"
#include "duckdb/common/types/column/column_data_allocator.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/row/tuple_data_allocator.hpp"
#include "duckdb/common/types/row/tuple_data_collection.hpp"
#include "duckdb/execution/aggregate_hashtable.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/execution/join_hashtable.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/main/relation.hpp"
#include "duckdb/main/stream_query_result.hpp"
#include "duckdb/optimizer/join_order/join_order_optimizer.hpp"
#include "duckdb/optimizer/rule.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parser/constraint.hpp"
#include "duckdb/parser/constraints/list.hpp"
#include "duckdb/parser/expression/list.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/statement/list.hpp"
#include "duckdb/parser/tableref/list.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/list.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/planner/query_node/bound_set_operation_node.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/write_ahead_log.hpp"
#include "duckdb/transaction/transaction.hpp"
using namespace duckdb;
template class std::unique_ptr<SQLStatement>;
template class std::unique_ptr<AlterStatement>;
template class std::unique_ptr<CopyStatement>;
template class std::unique_ptr<CreateStatement>;
template class std::unique_ptr<DeleteStatement>;
template class std::unique_ptr<DropStatement>;
template class std::unique_ptr<InsertStatement>;
template class std::unique_ptr<SelectStatement>;
template class std::unique_ptr<TransactionStatement>;
template class std::unique_ptr<UpdateStatement>;
template class std::unique_ptr<PrepareStatement>;
template class std::unique_ptr<ExecuteStatement>;
template class std::unique_ptr<VacuumStatement>;
template class std::unique_ptr<QueryNode>;
template class std::unique_ptr<SelectNode>;
template class std::unique_ptr<SetOperationNode>;
template class std::unique_ptr<ParsedExpression>;
template class std::unique_ptr<CaseExpression>;
template class std::unique_ptr<CastExpression>;
template class std::unique_ptr<ColumnRefExpression>;
template class std::unique_ptr<ComparisonExpression>;
template class std::unique_ptr<ConjunctionExpression>;
template class std::unique_ptr<ConstantExpression>;
template class std::unique_ptr<DefaultExpression>;
template class std::unique_ptr<FunctionExpression>;
template class std::unique_ptr<OperatorExpression>;
template class std::unique_ptr<ParameterExpression>;
template class std::unique_ptr<StarExpression>;
template class std::unique_ptr<SubqueryExpression>;
template class std::unique_ptr<WindowExpression>;
template class std::unique_ptr<Constraint>;
template class std::unique_ptr<NotNullConstraint>;
template class std::unique_ptr<CheckConstraint>;
template class std::unique_ptr<UniqueConstraint>;
template class std::unique_ptr<ForeignKeyConstraint>;
template class std::unique_ptr<BaseTableRef>;
template class std::unique_ptr<JoinRef>;
template class std::unique_ptr<SubqueryRef>;
template class std::unique_ptr<TableFunctionRef>;
template class std::shared_ptr<Event>;
template class std::unique_ptr<Pipeline>;
template class std::shared_ptr<Pipeline>;
template class std::weak_ptr<Pipeline>;
template class std::shared_ptr<MetaPipeline>;
template class std::unique_ptr<RowGroup>;
template class std::shared_ptr<RowGroupCollection>;
template class std::unique_ptr<RowDataBlock>;
template class std::unique_ptr<RowDataCollection>;
template class std::unique_ptr<ColumnDataCollection>;
template class std::shared_ptr<ColumnDataAllocator>;
template class std::unique_ptr<PartitionedColumnData>;
template class std::unique_ptr<TupleDataCollection>;
template class std::shared_ptr<TupleDataAllocator>;
template class std::unique_ptr<PartitionedTupleData>;
template class std::shared_ptr<PreparedStatementData>;
template class std::unique_ptr<VacuumInfo>;
template class std::unique_ptr<Expression>;
template class std::unique_ptr<BoundQueryNode>;
template class std::unique_ptr<BoundSelectNode>;
template class std::unique_ptr<BoundSetOperationNode>;
template class std::unique_ptr<BoundAggregateExpression>;
template class std::unique_ptr<BoundCaseExpression>;
template class std::unique_ptr<BoundCastExpression>;
template class std::unique_ptr<BoundColumnRefExpression>;
template class std::unique_ptr<BoundComparisonExpression>;
template class std::unique_ptr<BoundConjunctionExpression>;
template class std::unique_ptr<BoundConstantExpression>;
template class std::unique_ptr<BoundDefaultExpression>;
template class std::unique_ptr<BoundFunctionExpression>;
template class std::unique_ptr<BoundOperatorExpression>;
template class std::unique_ptr<BoundParameterExpression>;
template class std::unique_ptr<BoundReferenceExpression>;
template class std::unique_ptr<BoundSubqueryExpression>;
template class std::unique_ptr<BoundWindowExpression>;
template class std::unique_ptr<BoundBaseTableRef>;
template class std::unique_ptr<CatalogEntry>;
template class std::unique_ptr<BindContext>;
template class std::unique_ptr<char[]>;
template class std::unique_ptr<QueryResult>;
template class std::unique_ptr<MaterializedQueryResult>;
template class std::unique_ptr<StreamQueryResult>;
template class std::unique_ptr<LogicalOperator>;
template class std::unique_ptr<PhysicalOperator>;
template class std::unique_ptr<OperatorState>;
template class std::unique_ptr<sel_t[]>;
template class std::unique_ptr<StringHeap>;
template class std::unique_ptr<GroupedAggregateHashTable>;
template class std::unique_ptr<TableRef>;
template class std::unique_ptr<Transaction>;
template class std::unique_ptr<uint64_t[]>;
template class std::unique_ptr<data_t[]>;
template class std::unique_ptr<Vector[]>;
template class std::unique_ptr<DataChunk>;
template class std::unique_ptr<JoinHashTable>;
template class std::unique_ptr<JoinHashTable::ScanStructure>;
template class std::unique_ptr<JoinHashTable::ProbeSpill>;
template class std::unique_ptr<data_ptr_t[]>;
template class std::unique_ptr<Rule>;
template class std::unique_ptr<LogicalFilter>;
template class std::unique_ptr<LogicalJoin>;
template class std::unique_ptr<LogicalComparisonJoin>;
template class std::unique_ptr<FilterInfo>;
template class std::unique_ptr<JoinNode>;
template class std::unique_ptr<SingleJoinRelation>;
template class std::shared_ptr<Relation>;
template class std::unique_ptr<CatalogSet>;
template class std::unique_ptr<Binder>;
template class std::unique_ptr<PrivateAllocatorData>;
#define INSTANTIATE_VECTOR(VECTOR_DEFINITION) \
 template VECTOR_DEFINITION::size_type VECTOR_DEFINITION::size() const; \
 template VECTOR_DEFINITION::const_reference VECTOR_DEFINITION::operator[](VECTOR_DEFINITION::size_type n) const; \
 template VECTOR_DEFINITION::reference VECTOR_DEFINITION::operator[](VECTOR_DEFINITION::size_type n); \
 template VECTOR_DEFINITION::const_reference VECTOR_DEFINITION::back() const; \
 template VECTOR_DEFINITION::reference VECTOR_DEFINITION::back(); \
 template VECTOR_DEFINITION::const_reference VECTOR_DEFINITION::front() const; \
 template VECTOR_DEFINITION::reference VECTOR_DEFINITION::front();
INSTANTIATE_VECTOR(std::vector<ColumnDefinition>)
template class std::vector<ExpressionType>;
INSTANTIATE_VECTOR(std::vector<JoinCondition>)
INSTANTIATE_VECTOR(std::vector<OrderByNode>)
template class std::vector<uint64_t>;
template class std::vector<string>;
INSTANTIATE_VECTOR(std::vector<Expression *>)
INSTANTIATE_VECTOR(std::vector<BoundParameterExpression *>)
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<Expression>>)
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<DataChunk>>)
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<SQLStatement>>)
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<PhysicalOperator>>)
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<LogicalOperator>>)
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<Transaction>>)
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<JoinNode>>)
template class std::vector<PhysicalType>;
template class std::vector<Value>;
template class std::vector<int>;
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<Rule>>)
INSTANTIATE_VECTOR(std::vector<std::shared_ptr<Event>>)
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<Pipeline>>)
INSTANTIATE_VECTOR(std::vector<std::shared_ptr<Pipeline>>)
INSTANTIATE_VECTOR(std::vector<std::weak_ptr<Pipeline>>)
INSTANTIATE_VECTOR(std::vector<std::shared_ptr<MetaPipeline>>)
template class std::vector<std::vector<Expression *>>;
template class std::vector<LogicalType>;
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<JoinHashTable>>)
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<ColumnDataCollection>>)
INSTANTIATE_VECTOR(std::vector<std::shared_ptr<ColumnDataAllocator>>)
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<RowDataBlock>>)
#if !defined(__clang__)
template struct std::atomic<uint64_t>;
#endif
template class std::bitset<STANDARD_VECTOR_SIZE>;
template class std::unordered_map<PhysicalOperator *, QueryProfiler::TreeNode *>;
template class std::stack<PhysicalOperator *>;
template class std::unordered_map<string, uint64_t>;
template class std::unordered_map<string, std::vector<string>>;
template class std::unordered_map<string, std::pair<uint64_t, Expression *>>;
template class std::unordered_map<string, SelectStatement *>;
template class std::unordered_map<uint64_t, uint64_t>;
#endif
#ifdef DEBUG
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/list.hpp"
#include "duckdb/common/types/chunk_collection.hpp"
#include "duckdb/execution/aggregate_hashtable.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/execution/join_hashtable.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/main/relation.hpp"
#include "duckdb/main/stream_query_result.hpp"
#include "duckdb/optimizer/join_order/join_order_optimizer.hpp"
#include "duckdb/optimizer/rule.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parser/constraint.hpp"
#include "duckdb/parser/constraints/list.hpp"
#include "duckdb/parser/expression/list.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/statement/list.hpp"
#include "duckdb/parser/tableref/list.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/list.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/planner/query_node/bound_set_operation_node.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/write_ahead_log.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/common/types/column_data_collection.hpp"
#include "duckdb/common/types/column_data_allocator.hpp"
using namespace duckdb;
template class std::unique_ptr<SQLStatement>;
template class std::unique_ptr<AlterStatement>;
template class std::unique_ptr<CopyStatement>;
template class std::unique_ptr<CreateStatement>;
template class std::unique_ptr<DeleteStatement>;
template class std::unique_ptr<DropStatement>;
template class std::unique_ptr<InsertStatement>;
template class std::unique_ptr<SelectStatement>;
template class std::unique_ptr<TransactionStatement>;
template class std::unique_ptr<UpdateStatement>;
template class std::unique_ptr<PrepareStatement>;
template class std::unique_ptr<ExecuteStatement>;
template class std::unique_ptr<VacuumStatement>;
template class std::unique_ptr<QueryNode>;
template class std::unique_ptr<SelectNode>;
template class std::unique_ptr<SetOperationNode>;
template class std::unique_ptr<ParsedExpression>;
template class std::unique_ptr<CaseExpression>;
template class std::unique_ptr<CastExpression>;
template class std::unique_ptr<ColumnRefExpression>;
template class std::unique_ptr<ComparisonExpression>;
template class std::unique_ptr<ConjunctionExpression>;
template class std::unique_ptr<ConstantExpression>;
template class std::unique_ptr<DefaultExpression>;
template class std::unique_ptr<FunctionExpression>;
template class std::unique_ptr<OperatorExpression>;
template class std::unique_ptr<ParameterExpression>;
template class std::unique_ptr<StarExpression>;
template class std::unique_ptr<SubqueryExpression>;
template class std::unique_ptr<WindowExpression>;
template class std::unique_ptr<Constraint>;
template class std::unique_ptr<NotNullConstraint>;
template class std::unique_ptr<CheckConstraint>;
template class std::unique_ptr<UniqueConstraint>;
template class std::unique_ptr<ForeignKeyConstraint>;
template class std::unique_ptr<BaseTableRef>;
template class std::unique_ptr<JoinRef>;
template class std::unique_ptr<SubqueryRef>;
template class std::unique_ptr<TableFunctionRef>;
template class std::shared_ptr<Event>;
template class std::unique_ptr<Pipeline>;
template class std::shared_ptr<Pipeline>;
template class std::weak_ptr<Pipeline>;
template class std::shared_ptr<MetaPipeline>;
template class std::unique_ptr<RowGroup>;
template class std::shared_ptr<RowGroupCollection>;
template class std::unique_ptr<RowDataBlock>;
template class std::unique_ptr<RowDataCollection>;
template class std::unique_ptr<ColumnDataCollection>;
template class std::shared_ptr<ColumnDataAllocator>;
template class std::unique_ptr<PartitionedColumnData>;
template class std::shared_ptr<PreparedStatementData>;
template class std::unique_ptr<VacuumInfo>;
template class std::unique_ptr<Expression>;
template class std::unique_ptr<BoundQueryNode>;
template class std::unique_ptr<BoundSelectNode>;
template class std::unique_ptr<BoundSetOperationNode>;
template class std::unique_ptr<BoundAggregateExpression>;
template class std::unique_ptr<BoundCaseExpression>;
template class std::unique_ptr<BoundCastExpression>;
template class std::unique_ptr<BoundColumnRefExpression>;
template class std::unique_ptr<BoundComparisonExpression>;
template class std::unique_ptr<BoundConjunctionExpression>;
template class std::unique_ptr<BoundConstantExpression>;
template class std::unique_ptr<BoundDefaultExpression>;
template class std::unique_ptr<BoundFunctionExpression>;
template class std::unique_ptr<BoundOperatorExpression>;
template class std::unique_ptr<BoundParameterExpression>;
template class std::unique_ptr<BoundReferenceExpression>;
template class std::unique_ptr<BoundSubqueryExpression>;
template class std::unique_ptr<BoundWindowExpression>;
template class std::unique_ptr<BoundBaseTableRef>;
template class std::unique_ptr<CatalogEntry>;
template class std::unique_ptr<BindContext>;
template class std::unique_ptr<char[]>;
template class std::unique_ptr<QueryResult>;
template class std::unique_ptr<MaterializedQueryResult>;
template class std::unique_ptr<StreamQueryResult>;
template class std::unique_ptr<LogicalOperator>;
template class std::unique_ptr<PhysicalOperator>;
template class std::unique_ptr<OperatorState>;
template class std::unique_ptr<sel_t[]>;
template class std::unique_ptr<StringHeap>;
template class std::unique_ptr<GroupedAggregateHashTable>;
template class std::unique_ptr<TableRef>;
template class std::unique_ptr<Transaction>;
template class std::unique_ptr<uint64_t[]>;
template class std::unique_ptr<data_t[]>;
template class std::unique_ptr<Vector[]>;
template class std::unique_ptr<DataChunk>;
template class std::unique_ptr<JoinHashTable>;
template class std::unique_ptr<JoinHashTable::ScanStructure>;
template class std::unique_ptr<JoinHashTable::ProbeSpill>;
template class std::unique_ptr<data_ptr_t[]>;
template class std::unique_ptr<Rule>;
template class std::unique_ptr<LogicalFilter>;
template class std::unique_ptr<LogicalJoin>;
template class std::unique_ptr<LogicalComparisonJoin>;
template class std::unique_ptr<FilterInfo>;
template class std::unique_ptr<JoinNode>;
template class std::unique_ptr<SingleJoinRelation>;
template class std::shared_ptr<Relation>;
template class std::unique_ptr<CatalogSet>;
template class std::unique_ptr<Binder>;
template class std::unique_ptr<PrivateAllocatorData>;
#define INSTANTIATE_VECTOR(VECTOR_DEFINITION) \
 template VECTOR_DEFINITION::size_type VECTOR_DEFINITION::size() const; \
 template VECTOR_DEFINITION::const_reference VECTOR_DEFINITION::operator[](VECTOR_DEFINITION::size_type n) const; \
 template VECTOR_DEFINITION::reference VECTOR_DEFINITION::operator[](VECTOR_DEFINITION::size_type n); \
 template VECTOR_DEFINITION::const_reference VECTOR_DEFINITION::back() const; \
 template VECTOR_DEFINITION::reference VECTOR_DEFINITION::back(); \
 template VECTOR_DEFINITION::const_reference VECTOR_DEFINITION::front() const; \
 template VECTOR_DEFINITION::reference VECTOR_DEFINITION::front();
INSTANTIATE_VECTOR(std::vector<ColumnDefinition>)
template class std::vector<ExpressionType>;
INSTANTIATE_VECTOR(std::vector<JoinCondition>)
INSTANTIATE_VECTOR(std::vector<OrderByNode>)
template class std::vector<uint64_t>;
template class std::vector<string>;
INSTANTIATE_VECTOR(std::vector<Expression *>)
INSTANTIATE_VECTOR(std::vector<BoundParameterExpression *>)
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<Expression>>)
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<DataChunk>>)
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<SQLStatement>>)
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<PhysicalOperator>>)
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<LogicalOperator>>)
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<Transaction>>)
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<JoinNode>>)
template class std::vector<PhysicalType>;
template class std::vector<Value>;
template class std::vector<int>;
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<Rule>>)
INSTANTIATE_VECTOR(std::vector<std::shared_ptr<Event>>)
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<Pipeline>>)
INSTANTIATE_VECTOR(std::vector<std::shared_ptr<Pipeline>>)
INSTANTIATE_VECTOR(std::vector<std::weak_ptr<Pipeline>>)
INSTANTIATE_VECTOR(std::vector<std::shared_ptr<MetaPipeline>>)
template class std::vector<std::vector<Expression *>>;
template class std::vector<LogicalType>;
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<JoinHashTable>>)
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<ColumnDataCollection>>)
INSTANTIATE_VECTOR(std::vector<std::shared_ptr<ColumnDataAllocator>>)
INSTANTIATE_VECTOR(std::vector<std::unique_ptr<RowDataBlock>>)
#if !defined(__clang__)
template struct std::atomic<uint64_t>;
#endif
template class std::bitset<STANDARD_VECTOR_SIZE>;
template class std::unordered_map<PhysicalOperator *, QueryProfiler::TreeNode *>;
template class std::stack<PhysicalOperator *>;
template class std::unordered_map<string, uint64_t>;
template class std::unordered_map<string, std::vector<string>>;
template class std::unordered_map<string, std::pair<uint64_t, Expression *>>;
template class std::unordered_map<string, SelectStatement *>;
template class std::unordered_map<uint64_t, uint64_t>;
#endif
#ifdef DEBUG
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/list.hpp"
#include "duckdb/common/types/chunk_collection.hpp"
#include "duckdb/execution/aggregate_hashtable.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/execution/join_hashtable.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/main/relation.hpp"
#include "duckdb/main/stream_query_result.hpp"
#include "duckdb/optimizer/join_order/join_order_optimizer.hpp"
#include "duckdb/optimizer/rule.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parser/constraint.hpp"
#include "duckdb/parser/constraints/list.hpp"
#include "duckdb/parser/expression/list.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/statement/list.hpp"
#include "duckdb/parser/tableref/list.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/list.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/planner/query_node/bound_set_operation_node.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/write_ahead_log.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/common/types/column_data_collection.hpp"
#include "duckdb/common/types/column_data_allocator.hpp"
using namespace duckdb;
namespace duckdb {
template class unique_ptr<SQLStatement>;
template class unique_ptr<AlterStatement>;
template class unique_ptr<CopyStatement>;
template class unique_ptr<CreateStatement>;
template class unique_ptr<DeleteStatement>;
template class unique_ptr<DropStatement>;
template class unique_ptr<InsertStatement>;
template class unique_ptr<SelectStatement>;
template class unique_ptr<TransactionStatement>;
template class unique_ptr<UpdateStatement>;
template class unique_ptr<PrepareStatement>;
template class unique_ptr<ExecuteStatement>;
template class unique_ptr<VacuumStatement>;
template class unique_ptr<QueryNode>;
template class unique_ptr<SelectNode>;
template class unique_ptr<SetOperationNode>;
template class unique_ptr<ParsedExpression>;
template class unique_ptr<CaseExpression>;
template class unique_ptr<CastExpression>;
template class unique_ptr<ColumnRefExpression>;
template class unique_ptr<ComparisonExpression>;
template class unique_ptr<ConjunctionExpression>;
template class unique_ptr<ConstantExpression>;
template class unique_ptr<DefaultExpression>;
template class unique_ptr<FunctionExpression>;
template class unique_ptr<OperatorExpression>;
template class unique_ptr<ParameterExpression>;
template class unique_ptr<StarExpression>;
template class unique_ptr<SubqueryExpression>;
template class unique_ptr<WindowExpression>;
template class unique_ptr<Constraint>;
template class unique_ptr<NotNullConstraint>;
template class unique_ptr<CheckConstraint>;
template class unique_ptr<UniqueConstraint>;
template class unique_ptr<ForeignKeyConstraint>;
template class unique_ptr<BaseTableRef>;
template class unique_ptr<JoinRef>;
template class unique_ptr<SubqueryRef>;
template class unique_ptr<TableFunctionRef>;
template class unique_ptr<Pipeline>;
template class unique_ptr<RowGroup>;
template class unique_ptr<RowDataBlock>;
template class unique_ptr<RowDataCollection>;
template class unique_ptr<ColumnDataCollection>;
template class unique_ptr<PartitionedColumnData>;
template class unique_ptr<VacuumInfo>;
template class unique_ptr<Expression>;
template class unique_ptr<BoundQueryNode>;
template class unique_ptr<BoundSelectNode>;
template class unique_ptr<BoundSetOperationNode>;
template class unique_ptr<BoundAggregateExpression>;
template class unique_ptr<BoundCaseExpression>;
template class unique_ptr<BoundCastExpression>;
template class unique_ptr<BoundColumnRefExpression>;
template class unique_ptr<BoundComparisonExpression>;
template class unique_ptr<BoundConjunctionExpression>;
template class unique_ptr<BoundConstantExpression>;
template class unique_ptr<BoundDefaultExpression>;
template class unique_ptr<BoundFunctionExpression>;
template class unique_ptr<BoundOperatorExpression>;
template class unique_ptr<BoundParameterExpression>;
template class unique_ptr<BoundReferenceExpression>;
template class unique_ptr<BoundSubqueryExpression>;
template class unique_ptr<BoundWindowExpression>;
template class unique_ptr<BoundBaseTableRef>;
template class unique_ptr<CatalogEntry>;
template class unique_ptr<BindContext>;
template class unique_ptr<char[]>;
template class unique_ptr<QueryResult>;
template class unique_ptr<MaterializedQueryResult>;
template class unique_ptr<StreamQueryResult>;
template class unique_ptr<LogicalOperator>;
template class unique_ptr<PhysicalOperator>;
template class unique_ptr<OperatorState>;
template class unique_ptr<sel_t[]>;
template class unique_ptr<StringHeap>;
template class unique_ptr<GroupedAggregateHashTable>;
template class unique_ptr<TableRef>;
template class unique_ptr<Transaction>;
template class unique_ptr<uint64_t[]>;
template class unique_ptr<data_t[]>;
template class unique_ptr<Vector[]>;
template class unique_ptr<DataChunk>;
template class unique_ptr<JoinHashTable>;
template class unique_ptr<JoinHashTable::ScanStructure>;
template class unique_ptr<JoinHashTable::ProbeSpill>;
template class unique_ptr<data_ptr_t[]>;
template class unique_ptr<Rule>;
template class unique_ptr<LogicalFilter>;
template class unique_ptr<LogicalJoin>;
template class unique_ptr<LogicalComparisonJoin>;
template class unique_ptr<FilterInfo>;
template class unique_ptr<JoinNode>;
template class unique_ptr<SingleJoinRelation>;
template class unique_ptr<CatalogSet>;
template class unique_ptr<Binder>;
template class unique_ptr<PrivateAllocatorData>;
}
template class std::shared_ptr<Relation>;
template class std::shared_ptr<Event>;
template class std::shared_ptr<Pipeline>;
template class std::shared_ptr<MetaPipeline>;
template class std::shared_ptr<RowGroupCollection>;
template class std::shared_ptr<ColumnDataAllocator>;
template class std::shared_ptr<PreparedStatementData>;
template class std::weak_ptr<Pipeline>;
#define INSTANTIATE_VECTOR(VECTOR_DEFINITION) \
 template VECTOR_DEFINITION::size_type VECTOR_DEFINITION::size() const; \
 template VECTOR_DEFINITION::const_reference VECTOR_DEFINITION::operator[](VECTOR_DEFINITION::size_type n) const; \
 template VECTOR_DEFINITION::reference VECTOR_DEFINITION::operator[](VECTOR_DEFINITION::size_type n); \
 template VECTOR_DEFINITION::const_reference VECTOR_DEFINITION::back() const; \
 template VECTOR_DEFINITION::reference VECTOR_DEFINITION::back(); \
 template VECTOR_DEFINITION::const_reference VECTOR_DEFINITION::front() const; \
 template VECTOR_DEFINITION::reference VECTOR_DEFINITION::front();
INSTANTIATE_VECTOR(std::vector<ColumnDefinition>)
template class std::vector<ExpressionType>;
INSTANTIATE_VECTOR(std::vector<JoinCondition>)
INSTANTIATE_VECTOR(std::vector<OrderByNode>)
template class std::vector<uint64_t>;
template class std::vector<string>;
INSTANTIATE_VECTOR(std::vector<Expression *>)
INSTANTIATE_VECTOR(std::vector<BoundParameterExpression *>)
INSTANTIATE_VECTOR(std::vector<unique_ptr<Expression>>)
INSTANTIATE_VECTOR(std::vector<unique_ptr<DataChunk>>)
INSTANTIATE_VECTOR(std::vector<unique_ptr<SQLStatement>>)
INSTANTIATE_VECTOR(std::vector<unique_ptr<PhysicalOperator>>)
INSTANTIATE_VECTOR(std::vector<unique_ptr<LogicalOperator>>)
INSTANTIATE_VECTOR(std::vector<unique_ptr<Transaction>>)
INSTANTIATE_VECTOR(std::vector<unique_ptr<JoinNode>>)
template class std::vector<PhysicalType>;
template class std::vector<Value>;
template class std::vector<int>;
INSTANTIATE_VECTOR(std::vector<unique_ptr<Rule>>)
INSTANTIATE_VECTOR(std::vector<std::shared_ptr<Event>>)
INSTANTIATE_VECTOR(std::vector<unique_ptr<Pipeline>>)
INSTANTIATE_VECTOR(std::vector<std::shared_ptr<Pipeline>>)
INSTANTIATE_VECTOR(std::vector<std::weak_ptr<Pipeline>>)
INSTANTIATE_VECTOR(std::vector<std::shared_ptr<MetaPipeline>>)
template class std::vector<std::vector<Expression *>>;
template class std::vector<LogicalType>;
INSTANTIATE_VECTOR(std::vector<unique_ptr<JoinHashTable>>)
INSTANTIATE_VECTOR(std::vector<unique_ptr<ColumnDataCollection>>)
INSTANTIATE_VECTOR(std::vector<std::shared_ptr<ColumnDataAllocator>>)
INSTANTIATE_VECTOR(std::vector<unique_ptr<RowDataBlock>>)
#if !defined(__clang__)
template struct std::atomic<uint64_t>;
#endif
template class std::bitset<STANDARD_VECTOR_SIZE>;
template class std::unordered_map<PhysicalOperator *, QueryProfiler::TreeNode *>;
template class std::stack<PhysicalOperator *>;
template class std::unordered_map<string, uint64_t>;
template class std::unordered_map<string, std::vector<string>>;
template class std::unordered_map<string, std::pair<uint64_t, Expression *>>;
template class std::unordered_map<string, SelectStatement *>;
template class std::unordered_map<uint64_t, uint64_t>;
#endif
