#include "duckdb/parallel/pipeline_executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/limits.hpp"
#ifdef DUCKDB_DEBUG_ASYNC_SINK_SOURCE
#include <thread>
#include <chrono>
#endif
namespace duckdb {
PipelineExecutor::PipelineExecutor(ClientContext &context_p, Pipeline &pipeline_p)
    : pipeline(pipeline_p), thread(context_p), context(context_p, thread, &pipeline_p) {
 D_ASSERT(pipeline.source_state);
 if (pipeline.sink) {
  local_sink_state = pipeline.sink->GetLocalSinkState(context);
  requires_batch_index = pipeline.sink->RequiresBatchIndex() && pipeline.source->SupportsBatchIndex();
  if (requires_batch_index) {
   auto &partition_info = local_sink_state->partition_info;
   if (!partition_info.batch_index.IsValid()) {
    partition_info.batch_index = pipeline.RegisterNewBatchIndex();
    partition_info.min_batch_index = partition_info.batch_index;
   }
  }
 }
 local_source_state = pipeline.source->GetLocalSourceState(context, *pipeline.source_state);
 intermediate_chunks.reserve(pipeline.operators.size());
 intermediate_states.reserve(pipeline.operators.size());
 for (idx_t i = 0; i < pipeline.operators.size(); i++) {
  auto &prev_operator = i == 0 ? *pipeline.source : pipeline.operators[i - 1].get();
  auto &current_operator = pipeline.operators[i].get();
  auto chunk = make_uniq<DataChunk>();
  chunk->Initialize(Allocator::Get(context.client), prev_operator.GetTypes());
  intermediate_chunks.push_back(std::move(chunk));
  auto op_state = current_operator.GetOperatorState(context);
  intermediate_states.push_back(std::move(op_state));
  if (current_operator.IsSink() && current_operator.sink_state->state == SinkFinalizeType::NO_OUTPUT_POSSIBLE) {
   FinishProcessing();
  }
 }
 InitializeChunk(final_chunk);
}
bool PipelineExecutor::TryFlushCachingOperators() {
 if (!started_flushing) {
  D_ASSERT(in_process_operators.empty());
  started_flushing = true;
  flushing_idx = IsFinished() ? idx_t(finished_processing_idx) : 0;
 }
 while (flushing_idx < pipeline.operators.size()) {
  if (!pipeline.operators[flushing_idx].get().RequiresFinalExecute()) {
   flushing_idx++;
   continue;
  }
  if (!should_flush_current_idx && in_process_operators.empty()) {
   should_flush_current_idx = true;
   flushing_idx++;
   continue;
  }
  auto &curr_chunk =
      flushing_idx + 1 >= intermediate_chunks.size() ? final_chunk : *intermediate_chunks[flushing_idx + 1];
  auto &current_operator = pipeline.operators[flushing_idx].get();
  OperatorFinalizeResultType finalize_result;
  OperatorResultType push_result;
  if (in_process_operators.empty()) {
   StartOperator(current_operator);
   finalize_result = current_operator.FinalExecute(context, curr_chunk, *current_operator.op_state,
                                                   *intermediate_states[flushing_idx]);
   EndOperator(current_operator, &curr_chunk);
  } else {
   finalize_result = OperatorFinalizeResultType::HAVE_MORE_OUTPUT;
  }
  push_result = ExecutePushInternal(curr_chunk, flushing_idx + 1);
  if (finalize_result == OperatorFinalizeResultType::HAVE_MORE_OUTPUT) {
   should_flush_current_idx = true;
  } else {
   should_flush_current_idx = false;
  }
  if (push_result == OperatorResultType::BLOCKED) {
   remaining_sink_chunk = true;
   return false;
  } else if (push_result == OperatorResultType::FINISHED) {
   break;
  }
 }
 return true;
}
PipelineExecuteResult PipelineExecutor::Execute(idx_t max_chunks) {
 D_ASSERT(pipeline.sink);
 auto &source_chunk = pipeline.operators.empty() ? final_chunk : *intermediate_chunks[0];
 for (idx_t i = 0; i < max_chunks; i++) {
  if (context.client.interrupted) {
   throw InterruptException();
  }
  OperatorResultType result;
  if (exhausted_source && done_flushing && !remaining_sink_chunk && in_process_operators.empty()) {
   break;
  } else if (remaining_sink_chunk) {
   result = ExecutePushInternal(final_chunk);
   remaining_sink_chunk = false;
  } else if (!in_process_operators.empty() && !started_flushing) {
   D_ASSERT(source_chunk.size() > 0);
   result = ExecutePushInternal(source_chunk);
  } else if (exhausted_source && !done_flushing) {
   auto flush_completed = TryFlushCachingOperators();
   if (flush_completed) {
    done_flushing = true;
    break;
   } else {
    return PipelineExecuteResult::INTERRUPTED;
   }
  } else if (!exhausted_source) {
   source_chunk.Reset();
   SourceResultType source_result = FetchFromSource(source_chunk);
   if (source_result == SourceResultType::BLOCKED) {
    return PipelineExecuteResult::INTERRUPTED;
   }
   if (source_result == SourceResultType::FINISHED) {
    exhausted_source = true;
    if (source_chunk.size() == 0) {
     continue;
    }
   }
   result = ExecutePushInternal(source_chunk);
  } else {
   throw InternalException("Unexpected state reached in pipeline executor");
  }
  if (result == OperatorResultType::BLOCKED) {
   remaining_sink_chunk = true;
   return PipelineExecuteResult::INTERRUPTED;
  }
  if (result == OperatorResultType::FINISHED) {
   break;
  }
 }
 if ((!exhausted_source || !done_flushing) && !IsFinished()) {
  return PipelineExecuteResult::NOT_FINISHED;
 }
 PushFinalize();
 return PipelineExecuteResult::FINISHED;
}
PipelineExecuteResult PipelineExecutor::Execute() {
 return Execute(NumericLimits<idx_t>::Maximum());
}
OperatorResultType PipelineExecutor::ExecutePush(DataChunk &input) {
 return ExecutePushInternal(input);
}
void PipelineExecutor::FinishProcessing(int32_t operator_idx) {
 finished_processing_idx = operator_idx < 0 ? NumericLimits<int32_t>::Maximum() : operator_idx;
 in_process_operators = stack<idx_t>();
}
bool PipelineExecutor::IsFinished() {
 return finished_processing_idx >= 0;
}
OperatorResultType PipelineExecutor::ExecutePushInternal(DataChunk &input, idx_t initial_idx) {
 D_ASSERT(pipeline.sink);
 if (input.size() == 0) {
  return OperatorResultType::NEED_MORE_INPUT;
 }
 while (true) {
  OperatorResultType result;
  if (&input != &final_chunk) {
   final_chunk.Reset();
   result = Execute(input, final_chunk, initial_idx);
   if (result == OperatorResultType::FINISHED) {
    return OperatorResultType::FINISHED;
   }
  } else {
   result = OperatorResultType::NEED_MORE_INPUT;
  }
  auto &sink_chunk = final_chunk;
  if (sink_chunk.size() > 0) {
   StartOperator(*pipeline.sink);
   D_ASSERT(pipeline.sink);
   D_ASSERT(pipeline.sink->sink_state);
   OperatorSinkInput sink_input {*pipeline.sink->sink_state, *local_sink_state, interrupt_state};
   auto sink_result = Sink(sink_chunk, sink_input);
   EndOperator(*pipeline.sink, nullptr);
   if (sink_result == SinkResultType::BLOCKED) {
    return OperatorResultType::BLOCKED;
   } else if (sink_result == SinkResultType::FINISHED) {
    FinishProcessing();
    return OperatorResultType::FINISHED;
   }
  }
  if (result == OperatorResultType::NEED_MORE_INPUT) {
   return OperatorResultType::NEED_MORE_INPUT;
  }
 }
}
void PipelineExecutor::PushFinalize() {
 if (finalized) {
  throw InternalException("Calling PushFinalize on a pipeline that has been finalized already");
 }
 D_ASSERT(local_sink_state);
 finalized = true;
 pipeline.sink->Combine(context, *pipeline.sink->sink_state, *local_sink_state);
 for (idx_t i = 0; i < intermediate_states.size(); i++) {
  intermediate_states[i]->Finalize(pipeline.operators[i].get(), context);
 }
 pipeline.executor.Flush(thread);
 local_sink_state.reset();
}
void PipelineExecutor::ExecutePull(DataChunk &result) {
 if (IsFinished()) {
  return;
 }
 auto &executor = pipeline.executor;
 try {
  D_ASSERT(!pipeline.sink);
  auto &source_chunk = pipeline.operators.empty() ? result : *intermediate_chunks[0];
  while (result.size() == 0 && !exhausted_source) {
   if (in_process_operators.empty()) {
    source_chunk.Reset();
    auto done_signal = make_shared<InterruptDoneSignalState>();
    interrupt_state = InterruptState(done_signal);
    SourceResultType source_result;
    while (true) {
     source_result = FetchFromSource(source_chunk);
     if (source_result != SourceResultType::BLOCKED) {
      break;
     }
     done_signal->Await();
    }
    if (source_result == SourceResultType::FINISHED) {
     exhausted_source = true;
     if (source_chunk.size() == 0) {
      break;
     }
    }
   }
   if (!pipeline.operators.empty()) {
    auto state = Execute(source_chunk, result);
    if (state == OperatorResultType::FINISHED) {
     break;
    }
   }
  }
 } catch (const Exception &ex) {
  if (executor.HasError()) {
   executor.ThrowException();
  }
  throw;
 } catch (std::exception &ex) {
  if (executor.HasError()) {
   executor.ThrowException();
  }
  throw;
 } catch (...) {
  if (executor.HasError()) {
   executor.ThrowException();
  }
  throw;
 }
}
void PipelineExecutor::PullFinalize() {
 if (finalized) {
  throw InternalException("Calling PullFinalize on a pipeline that has been finalized already");
 }
 finalized = true;
 pipeline.executor.Flush(thread);
}
void PipelineExecutor::GoToSource(idx_t &current_idx, idx_t initial_idx) {
 current_idx = initial_idx;
 if (!in_process_operators.empty()) {
  current_idx = in_process_operators.top();
  in_process_operators.pop();
 }
 D_ASSERT(current_idx >= initial_idx);
}
OperatorResultType PipelineExecutor::Execute(DataChunk &input, DataChunk &result, idx_t initial_idx) {
 if (input.size() == 0) {
  return OperatorResultType::NEED_MORE_INPUT;
 }
 D_ASSERT(!pipeline.operators.empty());
 idx_t current_idx;
 GoToSource(current_idx, initial_idx);
 if (current_idx == initial_idx) {
  current_idx++;
 }
 if (current_idx > pipeline.operators.size()) {
  result.Reference(input);
  return OperatorResultType::NEED_MORE_INPUT;
 }
 while (true) {
  if (context.client.interrupted) {
   throw InterruptException();
  }
  auto current_intermediate = current_idx;
  auto &current_chunk =
      current_intermediate >= intermediate_chunks.size() ? result : *intermediate_chunks[current_intermediate];
  current_chunk.Reset();
  if (current_idx == initial_idx) {
   return OperatorResultType::NEED_MORE_INPUT;
  } else {
   auto &prev_chunk =
       current_intermediate == initial_idx + 1 ? input : *intermediate_chunks[current_intermediate - 1];
   auto operator_idx = current_idx - 1;
   auto &current_operator = pipeline.operators[operator_idx].get();
   StartOperator(current_operator);
   auto result = current_operator.Execute(context, prev_chunk, current_chunk, *current_operator.op_state,
                                          *intermediate_states[current_intermediate - 1]);
   EndOperator(current_operator, &current_chunk);
   if (result == OperatorResultType::HAVE_MORE_OUTPUT) {
    in_process_operators.push(current_idx);
   } else if (result == OperatorResultType::FINISHED) {
    D_ASSERT(current_chunk.size() == 0);
    FinishProcessing(current_idx);
    return OperatorResultType::FINISHED;
   }
   current_chunk.Verify();
  }
  if (current_chunk.size() == 0) {
   if (current_idx == initial_idx) {
    break;
   } else {
    GoToSource(current_idx, initial_idx);
    continue;
   }
  } else {
   current_idx++;
   if (current_idx > pipeline.operators.size()) {
    break;
   }
  }
 }
 return in_process_operators.empty() ? OperatorResultType::NEED_MORE_INPUT : OperatorResultType::HAVE_MORE_OUTPUT;
}
void PipelineExecutor::SetTaskForInterrupts(weak_ptr<Task> current_task) {
 interrupt_state = InterruptState(std::move(current_task));
}
SourceResultType PipelineExecutor::GetData(DataChunk &chunk, OperatorSourceInput &input) {
#ifdef DUCKDB_DEBUG_ASYNC_SINK_SOURCE
 if (debug_blocked_source_count < debug_blocked_target_count) {
  debug_blocked_source_count++;
  auto &callback_state = input.interrupt_state;
  std::thread rewake_thread([callback_state] {
   std::this_thread::sleep_for(std::chrono::milliseconds(1));
   callback_state.Callback();
  });
  rewake_thread.detach();
  return SourceResultType::BLOCKED;
 }
#endif
 return pipeline.source->GetData(context, chunk, input);
}
SinkResultType PipelineExecutor::Sink(DataChunk &chunk, OperatorSinkInput &input) {
#ifdef DUCKDB_DEBUG_ASYNC_SINK_SOURCE
 if (debug_blocked_sink_count < debug_blocked_target_count) {
  debug_blocked_sink_count++;
  auto &callback_state = input.interrupt_state;
  std::thread rewake_thread([callback_state] {
   std::this_thread::sleep_for(std::chrono::milliseconds(1));
   callback_state.Callback();
  });
  rewake_thread.detach();
  return SinkResultType::BLOCKED;
 }
#endif
 return pipeline.sink->Sink(context, chunk, input);
}
SourceResultType PipelineExecutor::FetchFromSource(DataChunk &result) {
 StartOperator(*pipeline.source);
 OperatorSourceInput source_input = {*pipeline.source_state, *local_source_state, interrupt_state};
 auto res = GetData(result, source_input);
 D_ASSERT(res != SourceResultType::BLOCKED || result.size() == 0);
 if (requires_batch_index) {
  idx_t next_batch_index;
  if (result.size() == 0) {
   next_batch_index = NumericLimits<int64_t>::Maximum();
  } else {
   next_batch_index =
       pipeline.source->GetBatchIndex(context, result, *pipeline.source_state, *local_source_state);
   next_batch_index += pipeline.base_batch_index;
  }
  auto &partition_info = local_sink_state->partition_info;
  if (next_batch_index != partition_info.batch_index.GetIndex()) {
   if (partition_info.batch_index.GetIndex() > next_batch_index) {
    throw InternalException(
        "Pipeline batch index - gotten lower batch index %llu (down from previous batch index of %llu)",
        next_batch_index, partition_info.batch_index.GetIndex());
   }
   auto current_batch = partition_info.batch_index.GetIndex();
   partition_info.batch_index = next_batch_index;
   pipeline.sink->NextBatch(context, *pipeline.sink->sink_state, *local_sink_state);
   partition_info.min_batch_index = pipeline.UpdateBatchIndex(current_batch, next_batch_index);
  }
 }
 EndOperator(*pipeline.source, &result);
 return res;
}
void PipelineExecutor::InitializeChunk(DataChunk &chunk) {
 auto &last_op = pipeline.operators.empty() ? *pipeline.source : pipeline.operators.back().get();
 chunk.Initialize(Allocator::DefaultAllocator(), last_op.GetTypes());
}
void PipelineExecutor::StartOperator(PhysicalOperator &op) {
 if (context.client.interrupted) {
  throw InterruptException();
 }
 context.thread.profiler.StartOperator(&op);
}
void PipelineExecutor::EndOperator(PhysicalOperator &op, optional_ptr<DataChunk> chunk) {
 context.thread.profiler.EndOperator(chunk);
 if (chunk) {
  chunk->Verify();
 }
}
}
