#include "execution/operator/physical_nested_loop_join.hpp"
#include "common/types/vector_operations.hpp"
#include "execution/expression_executor.hpp"
using namespace duckdb;
using namespace std;
PhysicalNestedLoopJoin::PhysicalNestedLoopJoin(
    std::unique_ptr<PhysicalOperator> left,
    std::unique_ptr<PhysicalOperator> right, std::vector<JoinCondition> cond,
    JoinType join_type)
    : PhysicalOperator(PhysicalOperatorType::NESTED_LOOP_JOIN),
      conditions(move(cond)), type(join_type) {
 children.push_back(move(left));
 children.push_back(move(right));
}
vector<TypeId> PhysicalNestedLoopJoin::GetTypes() {
 auto types = children[0]->GetTypes();
 auto right_types = children[1]->GetTypes();
 types.insert(types.end(), right_types.begin(), right_types.end());
 return types;
}
void PhysicalNestedLoopJoin::GetChunk(DataChunk &chunk,
                                      PhysicalOperatorState *state_) {
 auto state =
     reinterpret_cast<PhysicalNestedLoopJoinOperatorState *>(state_);
 chunk.Reset();
 if (type != JoinType::INNER) {
  throw Exception("Only inner joins supported for now!");
 }
 if (state->right_chunks.column_count() == 0) {
  auto right_state = children[1]->GetOperatorState(state->parent);
  auto types = children[1]->GetTypes();
  DataChunk new_chunk;
  new_chunk.Initialize(types);
  do {
   children[1]->GetChunk(new_chunk, right_state.get());
   state->right_chunks.Append(new_chunk);
  } while (new_chunk.count > 0);
  if (state->right_chunks.count == 0) {
   return;
  }
  vector<TypeId> left_types, right_types;
  for (auto &cond : conditions) {
   left_types.push_back(cond.left->return_type);
   right_types.push_back(cond.right->return_type);
  }
  state->left_join_condition.Initialize(left_types);
  state->right_join_condition.Initialize(right_types);
 }
 do {
  if (state->left_position >= state->child_chunk.count) {
   children[0]->GetChunk(state->child_chunk, state->child_state.get());
   if (state->child_chunk.count == 0) {
    return;
   }
   state->left_position = 0;
   state->right_chunk = 0;
   state->left_join_condition.Reset();
   ExpressionExecutor executor(state->child_chunk);
   for (size_t i = 0; i < conditions.size(); i++) {
    executor.Execute(conditions[i].left.get(),
                     state->left_join_condition.data[i]);
   }
   state->left_join_condition.count =
       state->left_join_condition.data[0].count;
  }
  auto &left_chunk = state->child_chunk;
<<<<<<< HEAD
  auto &right_chunk = *state->right_chunks.chunks[state->right_chunk];
  assert(right_chunk.count <= STANDARD_VECTOR_SIZE);
||||||| 9a48b79af8
  auto &right_chunk = *state->right_chunks[state->right_chunk].get();
  assert(right_chunk.count <= chunk.maximum_size);
=======
  auto &right_chunk = *state->right_chunks[state->right_chunk].get();
  assert(right_chunk.count <= chunk.maximum_size);
>>>>>>> 79bf6dbb
  state->right_join_condition.Reset();
  ExpressionExecutor executor(right_chunk);
  Vector final_result;
  for (size_t i = 0; i < conditions.size(); i++) {
   Vector &right_match = state->right_join_condition.data[i];
   executor.Execute(conditions[i].right.get(), right_match);
   Vector left_match(state->left_join_condition.data[i].GetValue(
       state->left_position));
   Vector intermediate(TypeId::BOOLEAN, STANDARD_VECTOR_SIZE);
   switch (conditions[i].comparison) {
   case ExpressionType::COMPARE_EQUAL:
    VectorOperations::Equals(left_match, right_match, intermediate);
    break;
   case ExpressionType::COMPARE_NOTEQUAL:
    VectorOperations::NotEquals(left_match, right_match,
                                intermediate);
    break;
   case ExpressionType::COMPARE_LESSTHAN:
    VectorOperations::LessThan(left_match, right_match,
                               intermediate);
    break;
   case ExpressionType::COMPARE_GREATERTHAN:
    VectorOperations::GreaterThan(left_match, right_match,
                                  intermediate);
    break;
   case ExpressionType::COMPARE_LESSTHANOREQUALTO:
    VectorOperations::LessThanEquals(left_match, right_match,
                                     intermediate);
    break;
   case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
    VectorOperations::GreaterThanEquals(left_match, right_match,
                                        intermediate);
    break;
   default:
    throw Exception(
        "Unsupported join comparison expression %s",
        ExpressionTypeToString(conditions[i].comparison).c_str());
   }
   if (i == 0) {
    intermediate.Move(final_result);
   } else {
    VectorOperations::And(intermediate, final_result, final_result);
   }
  }
  assert(final_result.type == TypeId::BOOLEAN);
<<<<<<< HEAD
  chunk.SetSelectionVector(final_result);
  if (chunk.count > 0) {
||||||| 9a48b79af8
  auto sel_vector = std::unique_ptr<sel_t[]>(new sel_t[final_result.count]);
  size_t current_index = 0;
  bool *join_condition = (bool *)final_result.data;
  for(size_t i = 0; i < final_result.count; i++) {
   if (join_condition[i]) {
    sel_vector[current_index++] = i;
   }
  }
  chunk.count = current_index;
  if (current_index > 0) {
   chunk.sel_vector = move(sel_vector);
=======
  auto sel_vector =
      std::unique_ptr<sel_t[]>(new sel_t[final_result.count]);
  size_t current_index = 0;
  bool *join_condition = (bool *)final_result.data;
  for (size_t i = 0; i < final_result.count; i++) {
   if (join_condition[i]) {
    sel_vector[current_index++] = i;
   }
  }
  chunk.count = current_index;
  if (current_index > 0) {
   chunk.sel_vector = move(sel_vector);
>>>>>>> 79bf6dbb
   for (size_t i = 0; i < left_chunk.column_count; i++) {
<<<<<<< HEAD
||||||| 9a48b79af8
=======
>>>>>>> 79bf6dbb
    chunk.data[i].count = chunk.count;
    VectorOperations::Set(
        chunk.data[i],
        left_chunk.data[i].GetValue(state->left_position));
   }
   for (size_t i = 0; i < right_chunk.column_count; i++) {
    size_t chunk_entry = left_chunk.column_count + i;
    chunk.data[chunk_entry].Reference(right_chunk.data[i]);
    chunk.data[chunk_entry].count = chunk.count;
    chunk.data[chunk_entry].sel_vector = chunk.sel_vector;
   }
  }
  state->right_chunk++;
<<<<<<< HEAD
  if (state->right_chunk >= state->right_chunks.chunks.size()) {
||||||| 9a48b79af8
  if (state->right_chunk >= state->right_chunks.size()) {
=======
  if (state->right_chunk >= state->right_chunks.size()) {
>>>>>>> 79bf6dbb
   state->left_position++;
   state->right_chunk = 0;
  }
<<<<<<< HEAD
 } while (chunk.count == 0);
 chunk.Verify();
||||||| 9a48b79af8
 } while(chunk.count == 0);
=======
 } while (chunk.count == 0);
>>>>>>> 79bf6dbb
}
std::unique_ptr<PhysicalOperatorState>
PhysicalNestedLoopJoin::GetOperatorState(ExpressionExecutor *parent_executor) {
 return make_unique<PhysicalNestedLoopJoinOperatorState>(
     children[0].get(), children[1].get(), parent_executor);
}
