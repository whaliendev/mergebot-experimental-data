#include "duckdb/execution/expression_executor.hpp"

#include "duckdb/common/types/static_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

using namespace duckdb;
using namespace std;

ExpressionExecutor::ExpressionExecutor() {
}

ExpressionExecutor::ExpressionExecutor(Expression *expression) {
	assert(expression);
	AddExpression(*expression);
}

ExpressionExecutor::ExpressionExecutor(Expression &expression) {
	AddExpression(expression);
}


ExpressionExecutor::ExpressionExecutor(vector<unique_ptr<Expression>> &exprs) {
	assert(exprs.size() > 0);
	for(auto &expr : exprs) {
		AddExpression(*expr);
	}
}

void ExpressionExecutor::AddExpression(Expression &expr) {
	expressions.push_back(&expr);
	auto state = make_unique<ExpressionExecutorState>();
	Initialize(expr, *state);
	states.push_back(move(state));
}

void ExpressionExecutor::Initialize(Expression &expression, ExpressionExecutorState &state) {
	state.root_state = InitializeState(expression, state);
	state.executor = this;
}

void ExpressionExecutor::Execute(DataChunk *input, DataChunk &result) {
	SetChunk(input);

	assert(expressions.size() == result.column_count);
	assert(expressions.size() > 0);
	result.Reset();
	for (index_t i = 0; i < expressions.size(); i++) {
		ExecuteExpression(i, result.data[i]);
		result.heap.MergeHeap(result.data[i].string_heap);
	}
	result.sel_vector = result.data[0].sel_vector;
	result.Verify();
}

void ExpressionExecutor::ExecuteExpression(DataChunk &input, Vector &result) {
	SetChunk(&input);
	ExecuteExpression(result);
}

<<<<<<< HEAD
void ExpressionExecutor::ExecuteExpression(Vector &result) {
	assert(expressions.size() == 1);
	ExecuteExpression(0, result);
||||||| ea042a3262
void ExpressionExecutor::Merge(std::vector<std::unique_ptr<Expression>> &expressions, Vector &result) {
	assert(expressions.size() > 0);

	ExecuteExpression(*expressions[0], result);
	for (index_t i = 1; i < expressions.size(); i++) {
		MergeExpression(*expressions[i], result);
	}
=======
void ExpressionExecutor::Merge(std::vector<std::unique_ptr<Expression>> &expressions) {
	assert(expressions.size() > 0);
	//evaluate all expressions
	for (index_t i = 0; i < expressions.size(); i++) {
		//return if no more true rows
		if (chunk->size() != 0) {
			//evaluate current expression
			Vector intermediate;
			Execute(*expressions[i], intermediate);
			assert(intermediate.type == TypeId::BOOLEAN);
			//if constant and false/null, set count == 0 to fetch the next chunk
			if (intermediate.IsConstant()) {
				if (!intermediate.data[0] || intermediate.nullmask[0]) {
					chunk->data[0].count = 0;
					break;
				}
			} else {
				chunk->SetSelectionVector(intermediate);
			}
		} else {
			break;
		}
	}
>>>>>>> 8cf352bc
}

void ExpressionExecutor::ExecuteExpression(index_t expr_idx, Vector &result) {
	assert(expr_idx < expressions.size());
	assert(result.type == expressions[expr_idx]->return_type);

	auto owned_data = move(result.owned_data);
	auto initial_data = result.data;
	Execute(*expressions[expr_idx], states[expr_idx]->root_state.get(), result);
	if (chunk) {
		// we have an input chunk: result of this vector should have the same length as input chunk
		// check if the result is a single constant value
		if (result.IsConstant()) {
			// have to duplicate the constant value to match the rows in the
			// other columns
			auto constant_value = result.GetValue(0);
			result.data = initial_data;
			result.count = chunk->size();
			result.sel_vector = chunk->sel_vector;
			VectorOperations::Set(result, constant_value);
		} else if (result.count != chunk->size()) {
			throw Exception("Computed vector length does not match expected length!");
		}
		assert(result.sel_vector == chunk->sel_vector);
	}
	if (result.data == initial_data) {
		result.owned_data = move(owned_data);
	}
<<<<<<< HEAD
}

||||||| ea042a3262
	assert(result.type == vector.type);
	vector.Move(result);
}

void ExpressionExecutor::MergeExpression(Expression &expr, Vector &result) {
	Vector intermediate;
	Execute(expr, intermediate);

	assert(result.type == TypeId::BOOLEAN);
	assert(intermediate.type == TypeId::BOOLEAN);

	StaticVector<bool> and_result;
	VectorOperations::And(result, intermediate, and_result);
	and_result.Move(result);
}

=======
	assert(result.type == vector.type);
	vector.Move(result);
}

>>>>>>> 8cf352bc
Value ExpressionExecutor::EvaluateScalar(Expression &expr) {
	assert(expr.IsFoldable());
	// use an ExpressionExecutor to execute the expression
	ExpressionExecutor executor(expr);

	Vector result(expr.return_type, true, false);
	executor.ExecuteExpression(result);

	assert(result.count == 1);
	return result.GetValue(0);
}

void ExpressionExecutor::Verify(Expression &expr, Vector &vector) {
	assert(expr.return_type == vector.type);
	vector.Verify();
}

unique_ptr<ExpressionState> ExpressionExecutor::InitializeState(Expression &expr, ExpressionExecutorState &state) {
	switch (expr.expression_class) {
	case ExpressionClass::BOUND_REF:
		return InitializeState((BoundReferenceExpression &)expr, state);
	case ExpressionClass::BOUND_CASE:
		return InitializeState((BoundCaseExpression &)expr, state);
	case ExpressionClass::BOUND_CAST:
		return InitializeState((BoundCastExpression &)expr, state);
	case ExpressionClass::COMMON_SUBEXPRESSION:
		return InitializeState((CommonSubExpression &)expr, state);
	case ExpressionClass::BOUND_COMPARISON:
		return InitializeState((BoundComparisonExpression &)expr, state);
	case ExpressionClass::BOUND_CONJUNCTION:
		return InitializeState((BoundConjunctionExpression &)expr, state);
	case ExpressionClass::BOUND_CONSTANT:
		return InitializeState((BoundConstantExpression &)expr, state);
	case ExpressionClass::BOUND_FUNCTION:
		return InitializeState((BoundFunctionExpression &)expr, state);
	case ExpressionClass::BOUND_OPERATOR:
		return InitializeState((BoundOperatorExpression &)expr, state);
	case ExpressionClass::BOUND_PARAMETER:
		return InitializeState((BoundParameterExpression &)expr, state);
	default:
		throw NotImplementedException("Attempting to initialize state of expression of unknown type!");
	}
}

void ExpressionExecutor::Execute(Expression &expr, ExpressionState *state, Vector &result) {
	if (state) {
		state->Reset();
	}
	switch (expr.expression_class) {
	case ExpressionClass::BOUND_REF:
		Execute((BoundReferenceExpression &)expr, state, result);
		break;
	case ExpressionClass::BOUND_CASE:
		Execute((BoundCaseExpression &)expr, state, result);
		break;
	case ExpressionClass::BOUND_CAST:
		Execute((BoundCastExpression &)expr, state, result);
		break;
	case ExpressionClass::COMMON_SUBEXPRESSION:
		Execute((CommonSubExpression &)expr, state, result);
		break;
	case ExpressionClass::BOUND_COMPARISON:
		Execute((BoundComparisonExpression &)expr, state, result);
		break;
	case ExpressionClass::BOUND_CONJUNCTION:
		Execute((BoundConjunctionExpression &)expr, state, result);
		break;
	case ExpressionClass::BOUND_CONSTANT:
		Execute((BoundConstantExpression &)expr, state, result);
		break;
	case ExpressionClass::BOUND_FUNCTION:
		Execute((BoundFunctionExpression &)expr, state, result);
		break;
	case ExpressionClass::BOUND_OPERATOR:
		Execute((BoundOperatorExpression &)expr, state, result);
		break;
	case ExpressionClass::BOUND_PARAMETER:
		Execute((BoundParameterExpression &)expr, state, result);
		break;
	default:
		throw NotImplementedException("Attempting to execute expression of unknown type!");
	}
	Verify(expr, result);
}
