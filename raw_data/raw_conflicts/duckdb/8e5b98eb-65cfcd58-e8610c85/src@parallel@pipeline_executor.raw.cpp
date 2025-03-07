#include "duckdb/parallel/pipeline_executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/limits.hpp"

#include <thread>
#include <chrono>

namespace duckdb {

PipelineExecutor::PipelineExecutor(ClientContext &context_p, Pipeline &pipeline_p)
    : pipeline(pipeline_p), thread(context_p), context(context_p, thread, &pipeline_p) {
	D_ASSERT(pipeline.source_state);
	local_source_state = pipeline.source->GetLocalSourceState(context, *pipeline.source_state);
	if (pipeline.sink) {
		local_sink_state = pipeline.sink->GetLocalSinkState(context);
		requires_batch_index = pipeline.sink->RequiresBatchIndex() && pipeline.source->SupportsBatchIndex();
	}

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
			// one of the operators has already figured out no output is possible
			// we can skip executing the pipeline
			FinishProcessing();
		}
	}
	InitializeChunk(final_chunk);
}

// TODO: Refactor this steaming pile of spaghet
OperatorResultType PipelineExecutor::FlushCachingOperatorsPush() {
	if (!started_flushing) {
		started_flushing = true;
		flushing_idx = IsFinished() ? idx_t(finished_processing_idx) : 0;
	}

	// Main flushing loop -> keep flushing each operator that needs flushing until it's done or an interrupt happens
	while (flushing_idx < pipeline.operators.size()) {
		if (!pipeline.operators[flushing_idx]->RequiresFinalExecute()) {
			flushing_idx++;
			continue;
		}

		auto &curr_chunk =
			flushing_idx + 1 >= intermediate_chunks.size() ? final_chunk : *intermediate_chunks[flushing_idx + 1];
		auto current_operator = pipeline.operators[flushing_idx];


		OperatorFinalizeResultType finalize_result;
		OperatorResultType push_result;
		if (!blocked_on_have_more_output) {
			StartOperator(current_operator);
			finalize_result = current_operator->FinalExecute(context, curr_chunk, *current_operator->op_state,
																						*intermediate_states[flushing_idx]);
			EndOperator(current_operator, &curr_chunk);

			push_result = ExecutePushInternal(curr_chunk, flushing_idx + 1);
		} else {
			// Reset flag and reflush the last chunk we were flushing.
			blocked_on_have_more_output = false;
			finalize_result = OperatorFinalizeResultType::HAVE_MORE_OUTPUT;

			if (finalize_result_on_block == OperatorFinalizeResultType::FINISHED) {
				// Here we
				auto &curr_chunk =
				    flushing_idx >= intermediate_chunks.size() ? final_chunk : *intermediate_chunks[flushing_idx];
				push_result = ExecutePushInternal(curr_chunk, flushing_idx);
			} else {
				push_result = ExecutePushInternal(curr_chunk, flushing_idx + 1);
			}
		}

		if (finalize_result == OperatorFinalizeResultType::FINISHED) {
			flushing_idx++;
		}

		// Sink interrupted -> when continuing the pipeline we need to re-sink the final_chunk
		if (push_result == OperatorResultType::BLOCKED) {
			finalize_result_on_block = finalize_result;
			return OperatorResultType::BLOCKED;
		}

		// Sink is done, no more flushing required!
		if (push_result == OperatorResultType::FINISHED) {
			break;
		}
	}

	done_flushing = true;
	return OperatorResultType::FINISHED;
}

PipelineExecuteResult PipelineExecutor::Execute(idx_t max_chunks) {
	D_ASSERT(pipeline.sink);
	auto &source_chunk = pipeline.operators.empty() ? final_chunk : *intermediate_chunks[0];
	for (idx_t i = 0; i < max_chunks; i++) {
		// TODO: make single switch on state?
		if (exhausted_source && done_flushing && !remaining_sink_chunk && in_process_operators.empty()) {
			break;
		}

		// The reasoning we now also need this is because before in this loop we would always go through FetchFromSource
		// which now can be interrupted (ambiguity of this sentence is also a TODO...)
		// TODO: move up? remove call in FetchFromSource?
		if (context.client.interrupted) {
			throw InterruptException();
		}

		// There's 6 ways we can process a chunk in the pipeline here:
		// 1a. Regular fetch from source into pipeline:         Fetch from source, push through pipeline
		// 1b. A sink interrupt happened with have more output: Fetch from source, push through pipeline
		// 2.  Resuming after Sink interrupt:                   Retry pushing the final chunk into the sink
		// 3.  Flushing operators:                              Flushing a cached chunk through the pipeline into sink
		// 4a. Fetch had in process ops after Sink interrupt:   Retry pushing the last source chunk through the pipeline
		// 4b. Flush had in process ops after Sink interrupt:   Retry flushing the pipeline
		OperatorResultType result;
		if (remaining_sink_chunk) {
			// 2. Resuming after Sink interrupt
			result = ExecutePushInternal(final_chunk);
			remaining_sink_chunk = false;
		} else if (!in_process_operators.empty()) {
			if (started_flushing) {
				// 4b. Resume after sink interrupt happened with in process operators
				result = FlushCachingOperatorsPush();
			} else {
				// 4a. Resume after sink interrupt while fetching from source with in process operators
				D_ASSERT(source_chunk.size() > 0);
				result = ExecutePushInternal(source_chunk);
				// We need to reset this flag
				blocked_on_have_more_output = false;
			}
		} else if (exhausted_source && !done_flushing) {
			// 3. Flushing operators
			result = FlushCachingOperatorsPush();
		} else if (!exhausted_source) {
			SourceResultType source_result;
			if (!blocked_on_have_more_output) {
				// 1a. Regular fetch from source into pipeline
				source_chunk.Reset();
				source_result = FetchFromSource(source_chunk);
			} else {
				// 1b. We blocked on a have_more_output, need to re-execute the pipeline with same input
				blocked_on_have_more_output = false;
				source_result = SourceResultType::HAVE_MORE_OUTPUT;
			}

			// SOURCE INTERRUPT
			if(source_result == SourceResultType::BLOCKED) {
				return PipelineExecuteResult::INTERRUPTED;
			}

			// TODO check source result type instead of source chunk size
			if (source_chunk.size() == 0) {
				exhausted_source = true;
				continue;
			}
			result = ExecutePushInternal(source_chunk);
		} else {
			throw InternalException("Unexpected state reached in pipeline executor");
		}

		// SINK INTERRUPT
		if(result == OperatorResultType::BLOCKED) {
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

OperatorResultType PipelineExecutor::ExecutePush(DataChunk &input) { // LCOV_EXCL_START
	return ExecutePushInternal(input);
} // LCOV_EXCL_STOP

void PipelineExecutor::FinishProcessing(int32_t operator_idx) {
	finished_processing_idx = operator_idx < 0 ? NumericLimits<int32_t>::Maximum() : operator_idx;
	in_process_operators = stack<idx_t>();
}

bool PipelineExecutor::IsFinished() {
	return finished_processing_idx >= 0;
}

OperatorResultType PipelineExecutor::ExecutePushInternal(DataChunk &input, idx_t initial_idx) {
	D_ASSERT(pipeline.sink);
	if (input.size() == 0) { // LCOV_EXCL_START
		return OperatorResultType::NEED_MORE_INPUT;
	} // LCOV_EXCL_STOP

	// this loop will continuously push the input chunk through the pipeline as long as:
	// - the OperatorResultType for the Execute is HAVE_MORE_OUTPUT
	// - the Sink doesn't block
	while (true) {
		OperatorResultType result;
		// Note: if input is the final_chunk, we don't do any executing, the chunk just needs to be sinked
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
<<<<<<< HEAD
			StartOperator(pipeline.sink);

||||||| e8610c85fb
			StartOperator(pipeline.sink);
=======
			StartOperator(*pipeline.sink);
>>>>>>> 65cfcd58
			D_ASSERT(pipeline.sink);
			D_ASSERT(pipeline.sink->sink_state);
<<<<<<< HEAD
			OperatorSinkInput sink_input { *pipeline.sink->sink_state, *local_sink_state, interrupt_state };

			auto sink_result = Sink(sink_chunk, sink_input);

			EndOperator(pipeline.sink, nullptr);

			if (sink_result == SinkResultType::BLOCKED) {
				if (result == OperatorResultType::HAVE_MORE_OUTPUT) {
					blocked_on_have_more_output = true;
				}
				return OperatorResultType::BLOCKED;
			} else if (sink_result == SinkResultType::FINISHED) {
||||||| e8610c85fb
			auto sink_result = pipeline.sink->Sink(context, *pipeline.sink->sink_state, *local_sink_state, sink_chunk);
			EndOperator(pipeline.sink, nullptr);
			if (sink_result == SinkResultType::FINISHED) {
=======
			auto sink_result = pipeline.sink->Sink(context, *pipeline.sink->sink_state, *local_sink_state, sink_chunk);
			EndOperator(*pipeline.sink, nullptr);
			if (sink_result == SinkResultType::FINISHED) {
>>>>>>> 65cfcd58
				FinishProcessing();
				return OperatorResultType::FINISHED;
			}
		}
		if (result == OperatorResultType::NEED_MORE_INPUT) {
			return OperatorResultType::NEED_MORE_INPUT;
		}
	}
}

<<<<<<< HEAD
||||||| e8610c85fb
// Push all remaining cached operator output through the pipeline
void PipelineExecutor::FlushCachingOperatorsPush() {
	idx_t start_idx = IsFinished() ? idx_t(finished_processing_idx) : 0;
	for (idx_t op_idx = start_idx; op_idx < pipeline.operators.size(); op_idx++) {
		if (!pipeline.operators[op_idx]->RequiresFinalExecute()) {
			continue;
		}

		OperatorFinalizeResultType finalize_result;
		OperatorResultType push_result;

		do {
			auto &curr_chunk =
			    op_idx + 1 >= intermediate_chunks.size() ? final_chunk : *intermediate_chunks[op_idx + 1];
			auto current_operator = pipeline.operators[op_idx];
			StartOperator(current_operator);
			finalize_result = current_operator->FinalExecute(context, curr_chunk, *current_operator->op_state,
			                                                 *intermediate_states[op_idx]);
			EndOperator(current_operator, &curr_chunk);
			push_result = ExecutePushInternal(curr_chunk, op_idx + 1);
		} while (finalize_result != OperatorFinalizeResultType::FINISHED &&
		         push_result != OperatorResultType::FINISHED);

		if (push_result == OperatorResultType::FINISHED) {
			break;
		}
	}
}

=======
// Push all remaining cached operator output through the pipeline
void PipelineExecutor::FlushCachingOperatorsPush() {
	idx_t start_idx = IsFinished() ? idx_t(finished_processing_idx) : 0;
	for (idx_t op_idx = start_idx; op_idx < pipeline.operators.size(); op_idx++) {
		if (!pipeline.operators[op_idx].get().RequiresFinalExecute()) {
			continue;
		}

		OperatorFinalizeResultType finalize_result;
		OperatorResultType push_result;

		do {
			auto &curr_chunk =
			    op_idx + 1 >= intermediate_chunks.size() ? final_chunk : *intermediate_chunks[op_idx + 1];
			auto &current_operator = pipeline.operators[op_idx].get();
			StartOperator(current_operator);
			finalize_result = current_operator.FinalExecute(context, curr_chunk, *current_operator.op_state,
			                                                *intermediate_states[op_idx]);
			EndOperator(current_operator, &curr_chunk);
			push_result = ExecutePushInternal(curr_chunk, op_idx + 1);
		} while (finalize_result != OperatorFinalizeResultType::FINISHED &&
		         push_result != OperatorResultType::FINISHED);

		if (push_result == OperatorResultType::FINISHED) {
			break;
		}
	}
}

>>>>>>> 65cfcd58
void PipelineExecutor::PushFinalize() {
	if (finalized) {
		throw InternalException("Calling PushFinalize on a pipeline that has been finalized already");
	}

	D_ASSERT(local_sink_state);

	finalized = true;

	// run the combine for the sink
	pipeline.sink->Combine(context, *pipeline.sink->sink_state, *local_sink_state);

	// flush all query profiler info
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
		while (result.size() == 0) {
			if (in_process_operators.empty()) {
				source_chunk.Reset();

				// NOTE: we need to do blocking GetData, so we switch to Blocking I/O mode here.
				// Set interrupt mode to blocking, passing the done marker
				auto done_marker = make_shared<atomic<bool>>(false);
				interrupt_state = InterruptState(done_marker);
				SourceResultType source_result;

				// Repeatedly try to fetch from the source until it doesn't block. Note that it may block multiple times!
				while(true) {
					source_result = FetchFromSource(source_chunk);

					// No interrupt happened, all good.
					if (source_result != SourceResultType::BLOCKED){
						break;
					}

					// Busy wait for async callback from source operator TODO: backoff?
					while(!*done_marker) {};

					// Source made callback, reset marker and try again
					*done_marker = false;
				}

				if (source_chunk.size() == 0) {
//					D_ASSERT(res == SourceResultType::FINISHED);
					break;
				}
			}
			if (!pipeline.operators.empty()) {
				auto state = Execute(source_chunk, result);
				if (state == OperatorResultType::FINISHED) {
					break;
				}
			}
		}
	} catch (const Exception &ex) { // LCOV_EXCL_START
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
	} // LCOV_EXCL_STOP
}

void PipelineExecutor::PullFinalize() {
	if (finalized) {
		throw InternalException("Calling PullFinalize on a pipeline that has been finalized already");
	}
	finalized = true;
	pipeline.executor.Flush(thread);
}

void PipelineExecutor::GoToSource(idx_t &current_idx, idx_t initial_idx) {
	// we go back to the first operator (the source)
	current_idx = initial_idx;
	if (!in_process_operators.empty()) {
		// ... UNLESS there is an in process operator
		// if there is an in-process operator, we start executing at the latest one
		// for example, if we have a join operator that has tuples left, we first need to emit those tuples
		current_idx = in_process_operators.top();
		in_process_operators.pop();
	}
	D_ASSERT(current_idx >= initial_idx);
}

OperatorResultType PipelineExecutor::Execute(DataChunk &input, DataChunk &result, idx_t initial_idx) {
	if (input.size() == 0) { // LCOV_EXCL_START
		return OperatorResultType::NEED_MORE_INPUT;
	} // LCOV_EXCL_STOP
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
		// now figure out where to put the chunk
		// if current_idx is the last possible index (>= operators.size()) we write to the result
		// otherwise we write to an intermediate chunk
		auto current_intermediate = current_idx;
		auto &current_chunk =
		    current_intermediate >= intermediate_chunks.size() ? result : *intermediate_chunks[current_intermediate];
		current_chunk.Reset();
		if (current_idx == initial_idx) {
			// we went back to the source: we need more input
			return OperatorResultType::NEED_MORE_INPUT;
		} else {
			auto &prev_chunk =
			    current_intermediate == initial_idx + 1 ? input : *intermediate_chunks[current_intermediate - 1];
			auto operator_idx = current_idx - 1;
			auto &current_operator = pipeline.operators[operator_idx].get();

			// if current_idx > source_idx, we pass the previous operators' output through the Execute of the current
			// operator
			StartOperator(current_operator);
			auto result = current_operator.Execute(context, prev_chunk, current_chunk, *current_operator.op_state,
			                                       *intermediate_states[current_intermediate - 1]);
			EndOperator(current_operator, &current_chunk);
			if (result == OperatorResultType::HAVE_MORE_OUTPUT) {
				// more data remains in this operator
				// push in-process marker
				in_process_operators.push(current_idx);
			} else if (result == OperatorResultType::FINISHED) {
				D_ASSERT(current_chunk.size() == 0);
				FinishProcessing(current_idx);
				return OperatorResultType::FINISHED;
			}
			current_chunk.Verify();
		}

		if (current_chunk.size() == 0) {
			// no output from this operator!
			if (current_idx == initial_idx) {
				// if we got no output from the scan, we are done
				break;
			} else {
				// if we got no output from an intermediate op
				// we go back and try to pull data from the source again
				GoToSource(current_idx, initial_idx);
				continue;
			}
		} else {
			// we got output! continue to the next operator
			current_idx++;
			if (current_idx > pipeline.operators.size()) {
				// if we got output and are at the last operator, we are finished executing for this output chunk
				// return the data and push it into the chunk
				break;
			}
		}
	}
	return in_process_operators.empty() ? OperatorResultType::NEED_MORE_INPUT : OperatorResultType::HAVE_MORE_OUTPUT;
}

<<<<<<< HEAD
void PipelineExecutor::SetTaskForInterrupts(weak_ptr<Task> current_task) {
	interrupt_state = InterruptState(std::move(current_task));
}

SourceResultType PipelineExecutor::GetData(DataChunk &chunk, OperatorSourceInput &input) {
	//! Testing feature to enable async source on every operator
	if (context.client.config.force_async_pipelines && !debug_blocked_source) {
		debug_blocked_source = true;

		auto callback_state = input.interrupt_state;
		std::thread rewake_thread([callback_state] {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			callback_state.Callback();
		});
		rewake_thread.detach();

		return SourceResultType::BLOCKED;
	}

	return pipeline.source->GetData(context, chunk, input);
}

SinkResultType PipelineExecutor::Sink(DataChunk &chunk, OperatorSinkInput &input) {
	//! Testing feature to enable async sink on every operator
	if (context.client.config.force_async_pipelines && !debug_blocked_sink) {
		debug_blocked_sink = true;

		auto callback_state = input.interrupt_state;
		std::thread rewake_thread([callback_state] {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			callback_state.Callback();
		});
		rewake_thread.detach();

		return SinkResultType::BLOCKED;
	}

	return pipeline.sink->Sink(context, chunk, input);
}

SourceResultType PipelineExecutor::FetchFromSource(DataChunk &result) {
	StartOperator(pipeline.source);

	OperatorSourceInput source_input = { *pipeline.source_state, *local_source_state, interrupt_state };
	auto res = GetData(result, source_input);
	D_ASSERT(res != SourceResultType::BLOCKED || result.size() == 0);
	D_ASSERT(res != SourceResultType::FINISHED || result.size() == 0);

||||||| e8610c85fb
void PipelineExecutor::FetchFromSource(DataChunk &result) {
	StartOperator(pipeline.source);
	pipeline.source->GetData(context, result, *pipeline.source_state, *local_source_state);
=======
void PipelineExecutor::FetchFromSource(DataChunk &result) {
	StartOperator(*pipeline.source);
	pipeline.source->GetData(context, result, *pipeline.source_state, *local_source_state);
>>>>>>> 65cfcd58
	if (result.size() != 0 && requires_batch_index) {
		auto next_batch_index =
		    pipeline.source->GetBatchIndex(context, result, *pipeline.source_state, *local_source_state);
		next_batch_index += pipeline.base_batch_index;
		D_ASSERT(local_sink_state->batch_index <= next_batch_index ||
		         local_sink_state->batch_index == DConstants::INVALID_INDEX);
		local_sink_state->batch_index = next_batch_index;
	}
<<<<<<< HEAD

	EndOperator(pipeline.source, &result);

	return res;
||||||| e8610c85fb
	EndOperator(pipeline.source, &result);
=======
	EndOperator(*pipeline.source, &result);
>>>>>>> 65cfcd58
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

} // namespace duckdb
