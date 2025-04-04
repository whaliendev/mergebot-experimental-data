       
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/planner/bound_tokens.hpp"
#include "duckdb/planner/expression.hpp"
namespace duckdb {
class ExpressionExecutor {
public:
 ExpressionExecutor();
 ExpressionExecutor(Expression *expression);
 ExpressionExecutor(Expression &expression);
 ExpressionExecutor(vector<unique_ptr<Expression>> &expressions);
 void AddExpression(Expression &expr);
 void Execute(DataChunk *input, DataChunk &result);
 void Execute(DataChunk &input, DataChunk &result) {
  Execute(&input, result);
 }
 void Execute(DataChunk &result) {
  Execute(nullptr, result);
 }
 void ExecuteExpression(DataChunk &input, Vector &result);
 void ExecuteExpression(Vector &result);
 void ExecuteExpression(index_t expr_idx, Vector &result);
 static Value EvaluateScalar(Expression &expr);
 static unique_ptr<ExpressionState> InitializeState(Expression &expr, ExpressionExecutorState &state);
 void SetChunk(DataChunk *chunk) {
  this->chunk = chunk;
 }
 void SetChunk(DataChunk &chunk) {
  SetChunk(&chunk);
 }
 vector<Expression *> expressions;
 DataChunk *chunk = nullptr;
protected:
 void Initialize(Expression &expr, ExpressionExecutorState &state);
 static unique_ptr<ExpressionState> InitializeState(BoundReferenceExpression &expr, ExpressionExecutorState &state);
 static unique_ptr<ExpressionState> InitializeState(BoundCaseExpression &expr, ExpressionExecutorState &state);
 static unique_ptr<ExpressionState> InitializeState(BoundCastExpression &expr, ExpressionExecutorState &state);
 static unique_ptr<ExpressionState> InitializeState(CommonSubExpression &expr, ExpressionExecutorState &state);
 static unique_ptr<ExpressionState> InitializeState(BoundComparisonExpression &expr, ExpressionExecutorState &state);
 static unique_ptr<ExpressionState> InitializeState(BoundConjunctionExpression &expr, ExpressionExecutorState &state);
 static unique_ptr<ExpressionState> InitializeState(BoundConstantExpression &expr, ExpressionExecutorState &state);
 static unique_ptr<ExpressionState> InitializeState(BoundFunctionExpression &expr, ExpressionExecutorState &state);
 static unique_ptr<ExpressionState> InitializeState(BoundOperatorExpression &expr, ExpressionExecutorState &state);
 static unique_ptr<ExpressionState> InitializeState(BoundParameterExpression &expr, ExpressionExecutorState &state);
 void Execute(Expression &expr, ExpressionState *state, Vector &result);
 void Execute(BoundReferenceExpression &expr, ExpressionState *state, Vector &result);
 void Execute(BoundCaseExpression &expr, ExpressionState *state, Vector &result);
 void Execute(BoundCastExpression &expr, ExpressionState *state, Vector &result);
 void Execute(CommonSubExpression &expr, ExpressionState *state, Vector &result);
 void Execute(BoundComparisonExpression &expr, ExpressionState *state, Vector &result);
 void Execute(BoundConjunctionExpression &expr, ExpressionState *state, Vector &result);
 void Execute(BoundConstantExpression &expr, ExpressionState *state, Vector &result);
 void Execute(BoundFunctionExpression &expr, ExpressionState *state, Vector &result);
 void Execute(BoundOperatorExpression &expr, ExpressionState *state, Vector &result);
 void Execute(BoundParameterExpression &expr, ExpressionState *state, Vector &result);
 void Verify(Expression &expr, Vector &result);
private:
 vector<unique_ptr<ExpressionExecutorState>> states;
 unordered_map<Expression *, unique_ptr<Vector>> cached_cse;
};
}
