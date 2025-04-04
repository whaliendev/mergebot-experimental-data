#include "duckdb/execution/operator/filter/physical_filter.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
using namespace duckdb;
using namespace std;
void PhysicalFilter::GetChunkInternal(ClientContext &context, DataChunk &chunk, PhysicalOperatorState *state_) {
 auto state = reinterpret_cast<PhysicalFilterState *>(state_);
 do {
  children[0]->GetChunk(context, state->child_chunk, state->child_state.get());
  if (state->child_chunk.size() == 0) {
   return;
  }
  state->intermediate.Reset();
<<<<<<< HEAD
  state->executor.ExecuteExpression(state->child_chunk, state->intermediate.data[0]);
||||||| ea042a3262
  assert(expressions.size() > 0);
  Vector result(TypeId::BOOLEAN, true, false);
  ExpressionExecutor executor(state->child_chunk);
  executor.Merge(expressions, result);
=======
  assert(expressions.size() > 0);
  ExpressionExecutor executor(state->child_chunk);
  executor.Merge(expressions);
>>>>>>> 8cf352bc
  if (state->child_chunk.size() != 0) {
  chunk.sel_vector = state->child_chunk.sel_vector;
  for (index_t i = 0; i < chunk.column_count; i++) {
   chunk.data[i].Reference(state->child_chunk.data[i]);
  }
<<<<<<< HEAD
  chunk.SetSelectionVector(state->intermediate.data[0]);
||||||| ea042a3262
  chunk.SetSelectionVector(result);
=======
   for (index_t i = 0; i < chunk.column_count; i++) {
    chunk.data[i].count = state->child_chunk.data[i].count;
    chunk.data[i].sel_vector = state->child_chunk.sel_vector;
   }
  }
>>>>>>> 8cf352bc
 } while (chunk.size() == 0);
}
unique_ptr<PhysicalOperatorState> PhysicalFilter::GetOperatorState() {
 return make_unique<PhysicalFilterState>(children[0].get(), *expression);
}
string PhysicalFilter::ExtraRenderInformation() const {
 return expression->GetName();
}class PhysicalFilterState : public PhysicalOperatorState {
public:
 PhysicalFilterState(PhysicalOperator *child, Expression &expr): PhysicalOperatorState(child), executor(expr) {
  vector<TypeId> result_type = { TypeId::BOOLEAN };
  intermediate.Initialize(result_type);
 }
 DataChunk intermediate;
 ExpressionExecutor executor;
};
PhysicalFilter::PhysicalFilter(vector<TypeId> types, vector<unique_ptr<Expression>> select_list): PhysicalOperator(PhysicalOperatorType::FILTER, types) {
 assert(select_list.size() > 0);
 if (select_list.size() > 1) {
  auto conjunction = make_unique<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
  for(auto &expr : select_list) {
   conjunction->children.push_back(move(expr));
  }
  expression = move(conjunction);
 } else {
  expression = move(select_list[0]);
 }
}
