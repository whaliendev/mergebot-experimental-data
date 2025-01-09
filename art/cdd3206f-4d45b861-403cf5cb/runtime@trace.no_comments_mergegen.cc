#include "trace.h"
#include <sys/uio.h>
#include <unistd.h>
#include "android-base/macros.h"
#include "android-base/stringprintf.h"
#include "art_method-inl.h"
#include "base/casts.h"
#include "base/leb128.h"
#include "base/os.h"
#include "base/pointer_size.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "base/time_utils.h"
#include "base/unix_file/fd_file.h"
#include "base/utils.h"
#include "class_linker.h"
#include "common_throws.h"
#include "debugger.h"
#include "dex/descriptors_names.h"
#include "dex/dex_file-inl.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "gc/scoped_gc_critical_section.h"
#include "instrumentation.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "nativehelper/scoped_local_ref.h"
#include "scoped_thread_state_change-inl.h"
#include "stack.h"
#include "thread.h"
#include "thread_list.h"
namespace art HIDDEN {
struct MethodTraceRecord {
  ArtMethod* method;
  TraceAction action;
  uint32_t wall_clock_time;
  uint32_t thread_cpu_time;
};
using android::base::StringPrintf;
static constexpr size_t TraceActionBits = MinimumBitsToStore(
    static_cast<size_t>(kTraceMethodActionMask));
static constexpr uint8_t kOpNewMethod = 1U;
static constexpr uint8_t kOpNewThread = 2U;
static constexpr uint8_t kOpTraceSummary = 3U;
static const char kTraceTokenChar = '*';
static const uint16_t kTraceHeaderLength = 32;
static const uint32_t kTraceMagicValue = 0x574f4c53;
static const uint16_t kTraceVersionSingleClock = 2;
static const uint16_t kTraceVersionDualClock = 3;
static const uint16_t kTraceRecordSizeSingleClock = 10;
static const uint16_t kTraceRecordSizeDualClock = 14;
static const size_t kNumTracePoolBuffers = 32;
static const int kThreadInfoHeaderV2 = 0;
static const int kMethodInfoHeaderV2 = 1;
static const int kEntryHeaderV2 = 2;
static const int kSummaryHeaderV2 = 3;
static const uint16_t kTraceHeaderLengthV2 = 32;
static const uint16_t kTraceRecordSizeSingleClockV2 = 6;
static const uint16_t kTraceRecordSizeDualClockV2 = kTraceRecordSizeSingleClockV2 + 2;
static const uint16_t kEntryHeaderSizeSingleClockV2 = 17;
static const uint16_t kEntryHeaderSizeDualClockV2 = kEntryHeaderSizeSingleClockV2 + 4;
static const uint16_t kTraceVersionSingleClockV2 = 4;
static const uint16_t kTraceVersionDualClockV2 = 5;
TraceClockSource Trace::default_clock_source_ = kDefaultTraceClockSource;
Trace* volatile Trace::the_trace_ = nullptr;
pthread_t Trace::sampling_pthread_ = 0U;
std::unique_ptr<std::vector<ArtMethod*>> Trace::temp_stack_trace_;
static constexpr const char* kTracerInstrumentationKey = "Tracer";
static TraceAction DecodeTraceAction(uint32_t tmid) {
  return static_cast<TraceAction>(tmid & kTraceMethodActionMask);
}
namespace {
double tsc_to_microsec_scaling_factor = -1.0;
uint64_t GetTimestamp() {
  uint64_t t = 0;
#if defined(__arm__)
  t = MicroTime();
#elif defined(__aarch64__)
  asm volatile("mrs %0, cntvct_el0" : "=r"(t));
#elif defined(__i386__) || defined(__x86_64__)
  unsigned int lo, hi;
  asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
  t = (static_cast<uint64_t>(hi) << 32) | lo;
#elif defined(__riscv)
  asm volatile("rdtime %0" : "=r"(t));
#else
  t = MicroTime();
#endif
  return t;
}
#if defined(__i386__) || defined(__x86_64__) || defined(__aarch64__)
double computeScalingFactor() {
  uint64_t start = MicroTime();
  uint64_t start_tsc = GetTimestamp();
  usleep(1000);
  uint64_t diff_tsc = GetTimestamp() - start_tsc;
  uint64_t diff_time = MicroTime() - start;
  double scaling_factor = static_cast<double>(diff_time) / diff_tsc;
  DCHECK(scaling_factor > 0.0) << scaling_factor;
  return scaling_factor;
}
#endif
#if defined(__i386__) || defined(__x86_64__)
double GetScalingFactorForX86() {
  uint32_t eax, ebx, ecx;
  asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx) : "a"(0x0), "c"(0));
  if (eax < 0x15) {
    return computeScalingFactor();
  }
  asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx) : "a"(0x15), "c"(0));
  if (ebx == 0 || ecx == 0) {
    return computeScalingFactor();
  }
  double coreCrystalFreq = ecx;
  double seconds_to_microseconds = 1000 * 1000;
  double scaling_factor = (seconds_to_microseconds * eax) / (coreCrystalFreq * ebx);
  return scaling_factor;
}
#endif
void InitializeTimestampCounters() {
  if (tsc_to_microsec_scaling_factor > 0.0) {
    return;
  }
#if defined(__arm__)
  tsc_to_microsec_scaling_factor = 1.0;
#elif defined(__aarch64__)
  double seconds_to_microseconds = 1000 * 1000;
  uint64_t freq = 0;
  asm volatile("mrs %0,  cntfrq_el0" : "=r"(freq));
  if (freq == 0) {
    tsc_to_microsec_scaling_factor = computeScalingFactor();
  } else {
    tsc_to_microsec_scaling_factor = seconds_to_microseconds / static_cast<double>(freq);
  }
#elif defined(__i386__) || defined(__x86_64__)
  tsc_to_microsec_scaling_factor = GetScalingFactorForX86();
#else
  tsc_to_microsec_scaling_factor = 1.0;
#endif
}
ALWAYS_INLINE uint64_t GetMicroTime(uint64_t counter) {
  DCHECK(tsc_to_microsec_scaling_factor > 0.0) << tsc_to_microsec_scaling_factor;
  return tsc_to_microsec_scaling_factor * counter;
}
}
bool TraceWriter::HasMethodEncoding(ArtMethod* method) {
  return art_method_id_map_.find(method) != art_method_id_map_.end();
}
std::pair<uint32_t, bool> TraceWriter::GetMethodEncoding(ArtMethod* method) {
  auto it = art_method_id_map_.find(method);
  if (it != art_method_id_map_.end()) {
    return std::pair<uint32_t, bool>(it->second, false);
  } else {
    uint32_t idx = current_method_index_;
    art_method_id_map_.emplace(method, idx);
    current_method_index_++;
    return std::pair<uint32_t, bool>(idx, true);
  }
}
uint16_t TraceWriter::GetThreadEncoding(pid_t thread_id) {
  auto it = thread_id_map_.find(thread_id);
  if (it != thread_id_map_.end()) {
    return it->second;
  }
  uint16_t idx = current_thread_index_;
  thread_id_map_.emplace(thread_id, current_thread_index_);
  DCHECK_LT(current_thread_index_, (1 << 16) - 2);
  current_thread_index_++;
  return idx;
}
class TraceWriterTask final : public SelfDeletingTask {
 public:
  TraceWriterTask(
      TraceWriter* trace_writer, int index, uintptr_t* buffer, size_t cur_offset, size_t thread_id)
      : trace_writer_(trace_writer),
        index_(index),
        buffer_(buffer),
        cur_offset_(cur_offset),
        thread_id_(thread_id),
        reserve_buf_for_tid_(0) {}
  void Run(Thread* self ATTRIBUTE_UNUSED) override {
    std::unordered_map<ArtMethod*, std::string> method_infos;
    {
      ScopedObjectAccess soa(Thread::Current());
      trace_writer_->PreProcessTraceForMethodInfos(buffer_, cur_offset_, method_infos);
    }
    trace_writer_->FlushBuffer(buffer_, cur_offset_, thread_id_, method_infos);
    if (index_ == -1) {
      if (reserve_buf_for_tid_ == 0) {
        delete[] buffer_;
      }
    } else {
      trace_writer_->FetchTraceBufferForThread(index_, reserve_buf_for_tid_);
    }
  }
  uintptr_t* ReserveBufferForTid(size_t tid) {
    reserve_buf_for_tid_ = tid;
    return buffer_;
  }
 private:
  TraceWriter* trace_writer_;
  int index_;
  uintptr_t* buffer_;
  size_t cur_offset_;
  size_t thread_id_;
  size_t reserve_buf_for_tid_;
};
std::vector<ArtMethod*>* Trace::AllocStackTrace() {
  return (temp_stack_trace_.get() != nullptr) ? temp_stack_trace_.release() :
      new std::vector<ArtMethod*>();
}
void Trace::FreeStackTrace(std::vector<ArtMethod*>* stack_trace) {
  stack_trace->clear();
  temp_stack_trace_.reset(stack_trace);
}
void Trace::SetDefaultClockSource(TraceClockSource clock_source) {
#if defined(__linux__)
  default_clock_source_ = clock_source;
#else
  if (clock_source != TraceClockSource::kWall) {
    LOG(WARNING) << "Ignoring tracing request to use CPU time.";
  }
#endif
}
static uint16_t GetTraceVersion(TraceClockSource clock_source, int version) {
  if (version == Trace::kFormatV1) {
    return (clock_source == TraceClockSource::kDual) ? kTraceVersionDualClock :
                                                       kTraceVersionSingleClock;
  } else {
    return (clock_source == TraceClockSource::kDual) ? kTraceVersionDualClockV2 :
                                                       kTraceVersionSingleClockV2;
  }
}
static uint16_t GetRecordSize(TraceClockSource clock_source, int version) {
  if (version == Trace::kFormatV1) {
    return (clock_source == TraceClockSource::kDual) ? kTraceRecordSizeDualClock :
                                                       kTraceRecordSizeSingleClock;
  } else {
    return (clock_source == TraceClockSource::kDual) ? kTraceRecordSizeDualClockV2 :
                                                       kTraceRecordSizeSingleClockV2;
  }
}
static uint16_t GetNumEntries(TraceClockSource clock_source) {
  return (clock_source == TraceClockSource::kDual) ? kNumEntriesForDualClock
                                                   : kNumEntriesForWallClock;
}
bool UseThreadCpuClock(TraceClockSource clock_source) {
  return (clock_source == TraceClockSource::kThreadCpu) ||
         (clock_source == TraceClockSource::kDual);
}
bool UseWallClock(TraceClockSource clock_source) {
  return (clock_source == TraceClockSource::kWall) || (clock_source == TraceClockSource::kDual);
}
void Trace::MeasureClockOverhead() {
  if (UseThreadCpuClock(clock_source_)) {
    Thread::Current()->GetCpuMicroTime();
  }
  if (UseWallClock(clock_source_)) {
    GetTimestamp();
  }
}
uint32_t Trace::GetClockOverheadNanoSeconds() {
  Thread* self = Thread::Current();
  uint64_t start = self->GetCpuMicroTime();
  for (int i = 4000; i > 0; i--) {
    MeasureClockOverhead();
    MeasureClockOverhead();
    MeasureClockOverhead();
    MeasureClockOverhead();
    MeasureClockOverhead();
    MeasureClockOverhead();
    MeasureClockOverhead();
    MeasureClockOverhead();
  }
  uint64_t elapsed_us = self->GetCpuMicroTime() - start;
  return static_cast<uint32_t>(elapsed_us / 32);
}
static void Append2LE(uint8_t* buf, uint16_t val) {
  *buf++ = static_cast<uint8_t>(val);
  *buf++ = static_cast<uint8_t>(val >> 8);
}
static void Append4LE(uint8_t* buf, uint32_t val) {
  *buf++ = static_cast<uint8_t>(val);
  *buf++ = static_cast<uint8_t>(val >> 8);
  *buf++ = static_cast<uint8_t>(val >> 16);
  *buf++ = static_cast<uint8_t>(val >> 24);
}
static void Append8LE(uint8_t* buf, uint64_t val) {
  *buf++ = static_cast<uint8_t>(val);
  *buf++ = static_cast<uint8_t>(val >> 8);
  *buf++ = static_cast<uint8_t>(val >> 16);
  *buf++ = static_cast<uint8_t>(val >> 24);
  *buf++ = static_cast<uint8_t>(val >> 32);
  *buf++ = static_cast<uint8_t>(val >> 40);
  *buf++ = static_cast<uint8_t>(val >> 48);
  *buf++ = static_cast<uint8_t>(val >> 56);
}
static void GetSample(Thread* thread, void* arg) REQUIRES_SHARED(Locks::mutator_lock_) {
  std::vector<ArtMethod*>* const stack_trace = Trace::AllocStackTrace();
  StackVisitor::WalkStack(
      [&](const art::StackVisitor* stack_visitor) REQUIRES_SHARED(Locks::mutator_lock_) {
        ArtMethod* m = stack_visitor->GetMethod();
        if (!m->IsRuntimeMethod()) {
          stack_trace->push_back(m);
        }
        return true;
      },
      thread,
                     nullptr,
      art::StackVisitor::StackWalkKind::kIncludeInlinedFrames);
  Trace* the_trace = reinterpret_cast<Trace*>(arg);
  the_trace->CompareAndUpdateStackTrace(thread, stack_trace);
}
static void ClearThreadStackTraceAndClockBase(Thread* thread, [[maybe_unused]] void* arg) {
  thread->SetTraceClockBase(0);
  std::vector<ArtMethod*>* stack_trace = thread->GetStackTraceSample();
  thread->SetStackTraceSample(nullptr);
  delete stack_trace;
}
void Trace::CompareAndUpdateStackTrace(Thread* thread,
                                       std::vector<ArtMethod*>* stack_trace) {
  CHECK_EQ(pthread_self(), sampling_pthread_);
  std::vector<ArtMethod*>* old_stack_trace = thread->GetStackTraceSample();
  thread->SetStackTraceSample(stack_trace);
  uint32_t thread_clock_diff = 0;
  uint64_t timestamp_counter = 0;
  ReadClocks(thread, &thread_clock_diff, &timestamp_counter);
  if (old_stack_trace == nullptr) {
    for (auto rit = stack_trace->rbegin(); rit != stack_trace->rend(); ++rit) {
      LogMethodTraceEvent(thread, *rit, kTraceMethodEnter, thread_clock_diff, timestamp_counter);
    }
  } else {
    auto old_rit = old_stack_trace->rbegin();
    auto rit = stack_trace->rbegin();
    while (old_rit != old_stack_trace->rend() && rit != stack_trace->rend() && *old_rit == *rit) {
      old_rit++;
      rit++;
    }
    for (auto old_it = old_stack_trace->begin(); old_it != old_rit.base(); ++old_it) {
      LogMethodTraceEvent(thread, *old_it, kTraceMethodExit, thread_clock_diff, timestamp_counter);
    }
    for (; rit != stack_trace->rend(); ++rit) {
      LogMethodTraceEvent(thread, *rit, kTraceMethodEnter, thread_clock_diff, timestamp_counter);
    }
    FreeStackTrace(old_stack_trace);
  }
}
void* Trace::RunSamplingThread(void* arg) {
  Runtime* runtime = Runtime::Current();
  intptr_t interval_us = reinterpret_cast<intptr_t>(arg);
  CHECK_GE(interval_us, 0);
  CHECK(runtime->AttachCurrentThread("Sampling Profiler", true, runtime->GetSystemThreadGroup(),
                                     !runtime->IsAotCompiler()));
  while (true) {
    usleep(interval_us);
    ScopedTrace trace("Profile sampling");
    Thread* self = Thread::Current();
    Trace* the_trace;
    {
      MutexLock mu(self, *Locks::trace_lock_);
      the_trace = the_trace_;
      if (the_trace_->stop_tracing_) {
        break;
      }
    }
    {
      gc::ScopedGCCriticalSection gcs(self,
                                      art::gc::kGcCauseInstrumentation,
                                      art::gc::kCollectorTypeInstrumentation);
      ScopedSuspendAll ssa(__FUNCTION__);
      MutexLock mu(self, *Locks::thread_list_lock_);
      runtime->GetThreadList()->ForEach(GetSample, the_trace);
    }
  }
  runtime->DetachCurrentThread();
  return nullptr;
}
void Trace::Start(const char* trace_filename,
                  size_t buffer_size,
                  int flags,
                  TraceOutputMode output_mode,
                  TraceMode trace_mode,
                  int interval_us) {
  std::unique_ptr<File> file(OS::CreateEmptyFileWriteOnly(trace_filename));
  if (file == nullptr) {
    std::string msg = android::base::StringPrintf("Unable to open trace file '%s'", trace_filename);
    PLOG(ERROR) << msg;
    ScopedObjectAccess soa(Thread::Current());
    Thread::Current()->ThrowNewException("Ljava/lang/RuntimeException;", msg.c_str());
    return;
  }
  Start(std::move(file), buffer_size, flags, output_mode, trace_mode, interval_us);
}
void Trace::Start(int trace_fd,
                  size_t buffer_size,
                  int flags,
                  TraceOutputMode output_mode,
                  TraceMode trace_mode,
                  int interval_us) {
  if (trace_fd < 0) {
    std::string msg = android::base::StringPrintf("Unable to start tracing with invalid fd %d",
                                                  trace_fd);
    LOG(ERROR) << msg;
    ScopedObjectAccess soa(Thread::Current());
    Thread::Current()->ThrowNewException("Ljava/lang/RuntimeException;", msg.c_str());
    return;
  }
  std::unique_ptr<File> file(new File(trace_fd, "tracefile", true));
  Start(std::move(file), buffer_size, flags, output_mode, trace_mode, interval_us);
}
void Trace::StartDDMS(size_t buffer_size,
                      int flags,
                      TraceMode trace_mode,
                      int interval_us) {
  Start(std::unique_ptr<File>(),
        buffer_size,
        flags,
        TraceOutputMode::kDDMS,
        trace_mode,
        interval_us);
}
void Trace::Start(std::unique_ptr<File>&& trace_file_in,
                  size_t buffer_size,
                  int flags,
                  TraceOutputMode output_mode,
                  TraceMode trace_mode,
                  int interval_us) {
  auto deleter = [](File* file) {
    if (file != nullptr) {
      file->MarkUnchecked();
      [[maybe_unused]] int result = file->Close();
      delete file;
    }
  };
  std::unique_ptr<File, decltype(deleter)> trace_file(trace_file_in.release(), deleter);
  Thread* self = Thread::Current();
  {
    MutexLock mu(self, *Locks::trace_lock_);
    if (the_trace_ != nullptr) {
      LOG(ERROR) << "Trace already in progress, ignoring this request";
      return;
    }
  }
  if (trace_mode == TraceMode::kSampling && interval_us <= 0) {
    LOG(ERROR) << "Invalid sampling interval: " << interval_us;
    ScopedObjectAccess soa(self);
    ThrowRuntimeException("Invalid sampling interval: %d", interval_us);
    return;
  }
  InitializeTimestampCounters();
  Runtime* runtime = Runtime::Current();
  bool enable_stats = false;
  {
    jit::ScopedJitSuspend suspend_jit;
    gc::ScopedGCCriticalSection gcs(self,
                                    gc::kGcCauseInstrumentation,
                                    gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa(__FUNCTION__);
    MutexLock mu(self, *Locks::trace_lock_);
    if (the_trace_ != nullptr) {
      LOG(ERROR) << "Trace already in progress, ignoring this request";
    } else {
      enable_stats = (flags & kTraceCountAllocs) != 0;
      the_trace_ = new Trace(trace_file.release(), buffer_size, flags, output_mode, trace_mode);
      if (trace_mode == TraceMode::kSampling) {
        CHECK_PTHREAD_CALL(pthread_create, (&sampling_pthread_, nullptr, &RunSamplingThread,
                                            reinterpret_cast<void*>(interval_us)),
                                            "Sampling profiler thread");
        the_trace_->interval_us_ = interval_us;
      } else {
        if (!runtime->IsJavaDebuggable()) {
          art::jit::Jit* jit = runtime->GetJit();
          if (jit != nullptr) {
            jit->GetCodeCache()->InvalidateAllCompiledCode();
            jit->GetCodeCache()->TransitionToDebuggable();
            jit->GetJitCompiler()->SetDebuggableCompilerOption(true);
          }
          runtime->SetRuntimeDebugState(art::Runtime::RuntimeDebugState::kJavaDebuggable);
          runtime->GetInstrumentation()->UpdateEntrypointsForDebuggable();
          runtime->DeoptimizeBootImage();
        }
        bool is_fast_trace = !UseThreadCpuClock(the_trace_->GetClockSource());
#if defined(__arm__)
        is_fast_trace = false;
#endif
        runtime->GetInstrumentation()->AddListener(
            the_trace_,
            instrumentation::Instrumentation::kMethodEntered |
                instrumentation::Instrumentation::kMethodExited |
                instrumentation::Instrumentation::kMethodUnwind,
            is_fast_trace);
        runtime->GetInstrumentation()->EnableMethodTracing(kTracerInstrumentationKey,
                                                           the_trace_,
                                                                                 false);
      }
    }
  }
  if (enable_stats) {
    runtime->SetStatsEnabled(true);
  }
}
void Trace::StopTracing(bool flush_entries) {
  Runtime* const runtime = Runtime::Current();
  Thread* const self = Thread::Current();
  pthread_t sampling_pthread = 0U;
  {
    MutexLock mu(self, *Locks::trace_lock_);
    if (the_trace_ == nullptr) {
      LOG(ERROR) << "Trace stop requested, but no trace currently running";
      return;
    }
    the_trace_->stop_tracing_ = true;
    sampling_pthread = sampling_pthread_;
  }
  if (sampling_pthread != 0U) {
    CHECK_PTHREAD_CALL(pthread_join, (sampling_pthread, nullptr), "sampling thread shutdown");
  }
  Trace* the_trace = the_trace_;
  bool stop_alloc_counting = (the_trace->flags_ & Trace::kTraceCountAllocs) != 0;
  {
    gc::ScopedGCCriticalSection gcs(
        self, gc::kGcCauseInstrumentation, gc::kCollectorTypeInstrumentation);
    jit::ScopedJitSuspend suspend_jit;
    ScopedSuspendAll ssa(__FUNCTION__);
    if (the_trace->trace_mode_ == TraceMode::kSampling) {
      MutexLock mu(self, *Locks::thread_list_lock_);
      runtime->GetThreadList()->ForEach(ClearThreadStackTraceAndClockBase, nullptr);
    } else {
      bool is_fast_trace = !UseThreadCpuClock(the_trace_->GetClockSource());
#if defined(__arm__)
        is_fast_trace = false;
#endif
      runtime->GetInstrumentation()->RemoveListener(
          the_trace,
          instrumentation::Instrumentation::kMethodEntered |
              instrumentation::Instrumentation::kMethodExited |
              instrumentation::Instrumentation::kMethodUnwind,
          is_fast_trace);
      runtime->GetInstrumentation()->DisableMethodTracing(kTracerInstrumentationKey);
    }
    {
      MutexLock mu(self, *Locks::trace_lock_);
      MutexLock tl_lock(Thread::Current(), *Locks::thread_list_lock_);
      for (Thread* thread : Runtime::Current()->GetThreadList()->GetList()) {
        if (thread->GetMethodTraceBuffer() != nullptr) {
          the_trace->trace_writer_->FlushBuffer(
              thread, false, true);
        }
      }
      the_trace_ = nullptr;
      sampling_pthread_ = 0U;
    }
  }
  the_trace->trace_writer_->FinishTracing(the_trace->flags_, flush_entries);
  delete the_trace;
  if (stop_alloc_counting) {
    runtime->SetStatsEnabled(false);
  }
}
void Trace::FlushThreadBuffer(Thread* self) {
  MutexLock mu(self, *Locks::trace_lock_);
  if (the_trace_ == nullptr) {
    DCHECK_EQ(self->GetMethodTraceBuffer(), nullptr);
    return;
  }
  the_trace_->trace_writer_->FlushBuffer(self, false, true);
}
void Trace::Abort() {
  StopTracing( false);
}
void Trace::Stop() {
  StopTracing( true);
}
void Trace::Shutdown() {
  if (GetMethodTracingMode() != kTracingInactive) {
    Stop();
  }
}
TracingMode Trace::GetMethodTracingMode() {
  MutexLock mu(Thread::Current(), *Locks::trace_lock_);
  if (the_trace_ == nullptr) {
    return kTracingInactive;
  } else {
    switch (the_trace_->trace_mode_) {
      case TraceMode::kSampling:
        return kSampleProfilingActive;
      case TraceMode::kMethodTracing:
        return kMethodTracingActive;
    }
    LOG(FATAL) << "Unreachable";
    UNREACHABLE();
  }
}
static constexpr size_t kMinBufSize = 18U;
static constexpr size_t kPerThreadBufSize = 512 * 1024;
static_assert(kPerThreadBufSize > kMinBufSize);
static constexpr size_t kScalingFactorEncodedEntries = 6;
namespace {
TraceClockSource GetClockSourceFromFlags(int flags) {
  bool need_wall = flags & Trace::TraceFlag::kTraceClockSourceWallClock;
  bool need_thread_cpu = flags & Trace::TraceFlag::kTraceClockSourceThreadCpu;
  if (need_wall && need_thread_cpu) {
    return TraceClockSource::kDual;
  } else if (need_wall) {
    return TraceClockSource::kWall;
  } else if (need_thread_cpu) {
    return TraceClockSource::kThreadCpu;
  } else {
    return kDefaultTraceClockSource;
  }
}
int GetTraceFormatVersionFromFlags(int flags) {
  int version = (flags & Trace::kTraceFormatVersionFlagMask) >> Trace::kTraceFormatVersionShift;
  return version;
}
}
TraceWriter::TraceWriter(File* trace_file,
                         TraceOutputMode output_mode,
                         TraceClockSource clock_source,
                         size_t buffer_size,
                         int num_trace_buffers,
                         int trace_format_version,
                         uint32_t clock_overhead_ns)
    : trace_file_(trace_file),
      trace_output_mode_(output_mode),
      clock_source_(clock_source),
      buf_(new uint8_t[std::max(kMinBufSize, buffer_size)]()),
      buffer_size_(std::max(kMinBufSize, buffer_size)),
      trace_format_version_(trace_format_version),
      start_time_(GetMicroTime(GetTimestamp())),
      overflow_(false),
      num_records_(0),
      clock_overhead_ns_(clock_overhead_ns),
      owner_tids_(num_trace_buffers),
      tracing_lock_("tracing lock", LockLevel::kTracingStreamingLock) {
  if (output_mode == TraceOutputMode::kStreaming) {
    trace_version |= 0xF0U;
  }
  if (trace_format_version_ == Trace::kFormatV1) {
    memset(buf_.get(), 0, kTraceHeaderLength);
    Append4LE(buf_.get(), kTraceMagicValue);
    Append2LE(buf_.get() + 4, trace_version);
    Append2LE(buf_.get() + 6, kTraceHeaderLength);
    Append8LE(buf_.get() + 8, start_time_monotonic);
    if (trace_version >= kTraceVersionDualClock) {
      uint16_t record_size = GetRecordSize(clock_source_, trace_format_version_);
      Append2LE(buf_.get() + 16, record_size);
    }
    static_assert(18 <= kMinBufSize, "Minimum buffer size not large enough for trace header");
    cur_offset_ = kTraceHeaderLength;
  } else {
    memset(buf_.get(), 0, kTraceHeaderLengthV2);
    Append4LE(buf_.get(), kTraceMagicValue);
    Append2LE(buf_.get() + 4, trace_version);
    Append8LE(buf_.get() + 6, start_time_monotonic);
    cur_offset_ = kTraceHeaderLengthV2;
  }
  if (output_mode == TraceOutputMode::kStreaming) {
    if (!trace_file_->WriteFully(buf_.get(), kTraceHeaderLength)) {
      PLOG(WARNING) << "Failed streaming a tracing event.";
    }
    cur_offset_ = 0;
  }
  current_thread_index_ = 1;
  if (!Runtime::Current()->IsZygote()) {
    thread_pool_.reset(TraceWriterThreadPool::Create("Trace writer pool"));
    thread_pool_->StartWorkers(Thread::Current());
  }
  InitializeTraceBuffers();
}
Trace::Trace(File* trace_file,
             size_t buffer_size,
             int flags,
             TraceOutputMode output_mode,
             TraceMode trace_mode)
    : flags_(flags),
      trace_mode_(trace_mode),
      clock_source_(GetClockSourceFromFlags(flags)),
      interval_us_(0),
      stop_tracing_(false) {
  CHECK_IMPLIES(trace_file == nullptr, output_mode == TraceOutputMode::kDDMS);
  int trace_format_version = GetTraceFormatVersionFromFlags(flags_);
  size_t buf_size = (output_mode == TraceOutputMode::kStreaming) ?
                        kPerThreadBufSize * kScalingFactorEncodedEntries :
                        buffer_size;
  trace_writer_.reset(new TraceWriter(trace_file,
                                      output_mode,
                                      clock_source_,
                                      buf_size,
                                      kNumTracePoolBuffers,
                                      trace_format_version,
                                      GetClockOverheadNanoSeconds()));
}
void TraceWriter::FinishTracing(int flags, bool flush_entries) {
  Thread* self = Thread::Current();
  if (flush_entries) {
    if (thread_pool_ != nullptr) {
      thread_pool_->WaitForWorkersToBeCreated();
      thread_pool_->Wait(self, true, true);
      DCHECK_EQ(thread_pool_->GetTaskCount(self), 0u);
      thread_pool_->StopWorkers(self);
    }
    size_t final_offset = 0;
    if (trace_output_mode_ != TraceOutputMode::kStreaming) {
      MutexLock mu(Thread::Current(), tracing_lock_);
      final_offset = cur_offset_;
    }
    uint64_t elapsed = GetMicroTime(GetTimestamp()) - start_time_;
    std::ostringstream os;
    os << StringPrintf("%cversion\n", kTraceTokenChar);
    os << StringPrintf("%d\n", GetTraceVersion(clock_source_, trace_format_version_));
    os << StringPrintf("data-file-overflow=%s\n", overflow_ ? "true" : "false");
    if (UseThreadCpuClock(clock_source_)) {
      if (UseWallClock(clock_source_)) {
        os << StringPrintf("clock=dual\n");
      } else {
        os << StringPrintf("clock=thread-cpu\n");
      }
    } else {
      os << StringPrintf("clock=wall\n");
    }
    os << StringPrintf("elapsed-time-usec=%" PRIu64 "\n", elapsed);
    if (trace_output_mode_ != TraceOutputMode::kStreaming) {
      os << StringPrintf("num-method-calls=%zd\n", num_records_);
    }
    os << StringPrintf("clock-call-overhead-nsec=%d\n", clock_overhead_ns_);
    os << StringPrintf("vm=art\n");
    os << StringPrintf("pid=%d\n", getpid());
    if ((flags & Trace::kTraceCountAllocs) != 0) {
      os << "alloc-count=" << Runtime::Current()->GetStat(KIND_ALLOCATED_OBJECTS) << "\n";
      os << "alloc-size=" << Runtime::Current()->GetStat(KIND_ALLOCATED_BYTES) << "\n";
      os << "gc-count=" << Runtime::Current()->GetStat(KIND_GC_INVOCATIONS) << "\n";
    }
    if (trace_format_version_ == Trace::kFormatV1) {
      os << StringPrintf("%cthreads\n", kTraceTokenChar);
      DumpThreadList(os);
      os << StringPrintf("%cmethods\n", kTraceTokenChar);
      DumpMethodList(os);
    }
    os << StringPrintf("%cend\n", kTraceTokenChar);
    std::string header(os.str());
    if (trace_output_mode_ == TraceOutputMode::kStreaming) {
      DCHECK_NE(trace_file_.get(), nullptr);
      if (trace_format_version_ == Trace::kFormatV1) {
        uint8_t buf[7];
        Append2LE(buf, 0);
        buf[2] = kOpTraceSummary;
        Append4LE(buf + 3, static_cast<uint32_t>(header.length()));
        if (!trace_file_->WriteFully(buf, sizeof(buf)) ||
            !trace_file_->WriteFully(header.c_str(), header.length())) {
          PLOG(WARNING) << "Failed streaming a tracing event.";
        }
      } else {
        uint8_t buf[3];
        buf[0] = kSummaryHeaderV2;
        Append2LE(buf + 1, static_cast<uint32_t>(header.length()));
        if (!trace_file_->WriteFully(buf, sizeof(buf)) ||
            !trace_file_->WriteFully(header.c_str(), header.length())) {
          PLOG(WARNING) << "Failed streaming a tracing event.";
        }
      }
    } else {
      if (trace_file_.get() == nullptr) {
        std::vector<uint8_t> data;
        data.resize(header.length() + final_offset);
        memcpy(data.data(), header.c_str(), header.length());
        memcpy(data.data() + header.length(), buf_.get(), final_offset);
        Runtime::Current()->GetRuntimeCallbacks()->DdmPublishChunk(CHUNK_TYPE("MPSE"),
                                                                   ArrayRef<const uint8_t>(data));
      } else {
        if (!trace_file_->WriteFully(header.c_str(), header.length()) ||
            !trace_file_->WriteFully(buf_.get(), final_offset)) {
          std::string detail(StringPrintf("Trace data write failed: %s", strerror(errno)));
          PLOG(ERROR) << detail;
          ThrowRuntimeException("%s", detail.c_str());
        }
      }
    }
  } else {
    DCHECK_EQ(thread_pool_, nullptr);
  }
  if (trace_file_.get() != nullptr) {
    if (flush_entries) {
      if (trace_file_->Flush() != 0) {
        PLOG(WARNING) << "Could not flush trace file.";
      }
    } else {
      trace_file_->MarkUnchecked();
    }
    if (trace_file_->Close() != 0) {
      PLOG(ERROR) << "Could not close trace file.";
    }
  }
}
void Trace::DexPcMoved([[maybe_unused]] Thread* thread,
                       [[maybe_unused]] Handle<mirror::Object> this_object,
                       ArtMethod* method,
                       uint32_t new_dex_pc) {
  LOG(ERROR) << "Unexpected dex PC event in tracing " << ArtMethod::PrettyMethod(method)
             << " " << new_dex_pc;
}
void Trace::FieldRead([[maybe_unused]] Thread* thread,
                      [[maybe_unused]] Handle<mirror::Object> this_object,
                      ArtMethod* method,
                      uint32_t dex_pc,
                      [[maybe_unused]] ArtField* field) REQUIRES_SHARED(Locks::mutator_lock_) {
  LOG(ERROR) << "Unexpected field read event in tracing " << ArtMethod::PrettyMethod(method)
             << " " << dex_pc;
}
void Trace::FieldWritten([[maybe_unused]] Thread* thread,
                         [[maybe_unused]] Handle<mirror::Object> this_object,
                         ArtMethod* method,
                         uint32_t dex_pc,
                         [[maybe_unused]] ArtField* field,
                         [[maybe_unused]] const JValue& field_value)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  LOG(ERROR) << "Unexpected field write event in tracing " << ArtMethod::PrettyMethod(method)
             << " " << dex_pc;
}
void Trace::MethodEntered(Thread* thread, ArtMethod* method) {
  uint32_t thread_clock_diff = 0;
  uint64_t timestamp_counter = 0;
  ReadClocks(thread, &thread_clock_diff, &timestamp_counter);
  LogMethodTraceEvent(thread, method, kTraceMethodEnter, thread_clock_diff, timestamp_counter);
}
void Trace::MethodExited(Thread* thread,
                         ArtMethod* method,
                         [[maybe_unused]] instrumentation::OptionalFrame frame,
                         [[maybe_unused]] JValue& return_value) {
  uint32_t thread_clock_diff = 0;
  uint64_t timestamp_counter = 0;
  ReadClocks(thread, &thread_clock_diff, &timestamp_counter);
  LogMethodTraceEvent(thread, method, kTraceMethodExit, thread_clock_diff, timestamp_counter);
}
void Trace::MethodUnwind(Thread* thread, ArtMethod* method, [[maybe_unused]] uint32_t dex_pc) {
  uint32_t thread_clock_diff = 0;
  uint64_t timestamp_counter = 0;
  ReadClocks(thread, &thread_clock_diff, &timestamp_counter);
  LogMethodTraceEvent(thread, method, kTraceUnroll, thread_clock_diff, timestamp_counter);
}
void Trace::ExceptionThrown([[maybe_unused]] Thread* thread,
                            [[maybe_unused]] Handle<mirror::Throwable> exception_object)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  LOG(ERROR) << "Unexpected exception thrown event in tracing";
}
void Trace::ExceptionHandled([[maybe_unused]] Thread* thread,
                             [[maybe_unused]] Handle<mirror::Throwable> exception_object)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  LOG(ERROR) << "Unexpected exception thrown event in tracing";
}
void Trace::Branch(Thread* , ArtMethod* method,
                   uint32_t , int32_t )
      REQUIRES_SHARED(Locks::mutator_lock_) {
  LOG(ERROR) << "Unexpected branch event in tracing" << ArtMethod::PrettyMethod(method);
}
void Trace::WatchedFramePop([[maybe_unused]] Thread* self,
                            [[maybe_unused]] const ShadowFrame& frame) {
  LOG(ERROR) << "Unexpected WatchedFramePop event in tracing";
}
void Trace::ReadClocks(Thread* thread, uint32_t* thread_clock_diff, uint64_t* timestamp_counter) {
  if (UseThreadCpuClock(clock_source_)) {
    uint64_t clock_base = thread->GetTraceClockBase();
    if (UNLIKELY(clock_base == 0)) {
      uint64_t time = thread->GetCpuMicroTime();
      thread->SetTraceClockBase(time);
    } else {
      *thread_clock_diff = thread->GetCpuMicroTime() - clock_base;
    }
  }
  if (UseWallClock(clock_source_)) {
    *timestamp_counter = GetTimestamp();
  }
}
uintptr_t* TraceWriterThreadPool::FinishTaskAndClaimBuffer(size_t tid) {
  Thread* self = Thread::Current();
  TraceWriterTask* task = static_cast<TraceWriterTask*>(TryGetTask(self));
  if (task == nullptr) {
    LOG(WARNING)
        << "Fewer buffers in the pool than the number of threads. Might cause some slowdown";
    return nullptr;
  }
  uintptr_t* buffer = task->ReserveBufferForTid(tid);
  task->Run(self);
  task->Finalize();
  return buffer;
}
std::string TraceWriter::GetMethodLine(const std::string& method_line, uint32_t method_index) {
  return StringPrintf("%#x\t%s", (method_index << TraceActionBits), method_line.c_str());
}
std::string TraceWriter::GetMethodInfoLine(ArtMethod* method) {
  method = method->GetInterfaceMethodIfProxy(kRuntimePointerSize);
  return StringPrintf("%s\t%s\t%s\t%s\n",
                      PrettyDescriptor(method->GetDeclaringClassDescriptor()).c_str(),
                      method->GetName(),
                      method->GetSignature().ToString().c_str(),
                      method->GetDeclaringClassSourceFile());
}
void TraceWriter::RecordThreadInfo(Thread* thread) {
  std::string thread_name;
  thread->GetThreadName(thread_name);
  if (thread_name.compare("Shutdown thread") == 0) {
    return;
  }
  MutexLock mu(Thread::Current(), tracing_lock_);
  if (trace_output_mode_ != TraceOutputMode::kStreaming) {
    threads_list_.Overwrite(GetThreadEncoding(thread->GetTid()), thread_name);
    return;
  }
  static constexpr size_t kThreadNameHeaderSize = 7;
  uint8_t header[kThreadNameHeaderSize];
  if (trace_format_version_ == Trace::kFormatV1) {
    Append2LE(header, 0);
    header[2] = kOpNewThread;
    Append2LE(header + 3, GetThreadEncoding(thread->GetTid()));
  } else {
    header[0] = kThreadInfoHeaderV2;
    Append4LE(header + 1, thread->GetTid());
  }
  DCHECK(thread_name.length() < (1 << 16));
  Append2LE(header + 5, static_cast<uint16_t>(thread_name.length()));
  if (!trace_file_->WriteFully(header, kThreadNameHeaderSize) ||
      !trace_file_->WriteFully(reinterpret_cast<const uint8_t*>(thread_name.c_str()),
                               thread_name.length())) {
    PLOG(WARNING) << "Failed streaming a tracing event.";
  }
}
void TraceWriter::PreProcessTraceForMethodInfos(
    uintptr_t* method_trace_entries,
    size_t current_offset,
    std::unordered_map<ArtMethod*, std::string>& method_infos) {
  MutexLock mu(Thread::Current(), tracing_lock_);
  size_t num_entries = GetNumEntries(clock_source_);
  DCHECK_EQ((kPerThreadBufSize - current_offset) % num_entries, 0u);
  for (size_t entry_index = kPerThreadBufSize; entry_index != current_offset;) {
    entry_index -= num_entries;
    uintptr_t method_and_action = method_trace_entries[entry_index];
    ArtMethod* method = reinterpret_cast<ArtMethod*>(method_and_action & kMaskTraceAction);
    if (!HasMethodEncoding(method) && method_infos.find(method) == method_infos.end()) {
      method_infos.emplace(method, std::move(GetMethodInfoLine(method)));
    }
  }
}
void TraceWriter::RecordMethodInfo(const std::string& method_info_line, uint32_t method_id) {
  std::string method_line;
  size_t header_size;
  static constexpr size_t kMaxMethodNameHeaderSize = 7;
  uint8_t method_header[kMaxMethodNameHeaderSize];
  uint16_t method_line_length = static_cast<uint16_t>(method_line.length());
  DCHECK(method_line.length() < (1 << 16));
  if (trace_format_version_ == Trace::kFormatV1) {
    static constexpr size_t kMethodNameHeaderSize = 5;
    DCHECK_LT(kMethodNameHeaderSize, kPerThreadBufSize);
    Append2LE(method_header, 0);
    method_header[2] = kOpNewMethod;
    method_line = GetMethodLine(method_info_line, method_id);
    method_line_length = static_cast<uint16_t>(method_line.length());
    Append2LE(method_header + 3, method_line_length);
    header_size = kMethodNameHeaderSize;
  } else {
    method_line = method_info_line;
    method_line_length = static_cast<uint16_t>(method_line.length());
    method_header[0] = kMethodInfoHeaderV2;
    Append4LE(method_header + 1, method_id);
    Append2LE(method_header + 5, method_line_length);
    header_size = 7;
  }
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(method_line.c_str());
  if (!trace_file_->WriteFully(method_header, header_size) ||
      !trace_file_->WriteFully(ptr, method_line_length)) {
    PLOG(WARNING) << "Failed streaming a tracing event.";
  }
}
void TraceWriter::FlushAllThreadBuffers() {
  ScopedThreadStateChange stsc(Thread::Current(), ThreadState::kSuspended);
  ScopedSuspendAll ssa(__FUNCTION__);
  MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
  for (Thread* thread : Runtime::Current()->GetThreadList()->GetList()) {
    if (thread->GetMethodTraceBuffer() != nullptr) {
      FlushBuffer(thread, true, false);
      if (overflow_) {
        return;
      }
    }
  }
  return;
}
uintptr_t* TraceWriter::PrepareBufferForNewEntries(Thread* thread) {
  if (trace_output_mode_ == TraceOutputMode::kStreaming) {
    FlushBuffer(thread, false, false);
    DCHECK_EQ(overflow_, false);
  } else {
    FlushAllThreadBuffers();
  }
  if (overflow_) {
    return nullptr;
  }
  return thread->GetMethodTraceBuffer();
}
void TraceWriter::InitializeTraceBuffers() {
  for (size_t i = 0; i < owner_tids_.size(); i++) {
    owner_tids_[i].store(0);
  }
  trace_buffer_.reset(new uintptr_t[kPerThreadBufSize * owner_tids_.size()]);
  CHECK(trace_buffer_.get() != nullptr);
}
uintptr_t* TraceWriter::AcquireTraceBuffer(size_t tid) {
  for (size_t index = 0; index < owner_tids_.size(); index++) {
    size_t owner = 0;
    if (owner_tids_[index].compare_exchange_strong(owner, tid)) {
      return trace_buffer_.get() + index * kPerThreadBufSize;
    }
  }
  uintptr_t* buffer = thread_pool_->FinishTaskAndClaimBuffer(tid);
  if (buffer == nullptr) {
    buffer = new uintptr_t[kPerThreadBufSize];
    CHECK(buffer != nullptr);
  }
  return buffer;
}
void TraceWriter::FetchTraceBufferForThread(int index, size_t tid) {
  owner_tids_[index].store(tid);
}
int TraceWriter::GetMethodTraceIndex(uintptr_t* current_buffer) {
  if (current_buffer < trace_buffer_.get() ||
      current_buffer > trace_buffer_.get() + (owner_tids_.size() - 1) * kPerThreadBufSize) {
    return -1;
  }
  return (current_buffer - trace_buffer_.get()) / kPerThreadBufSize;
}
void TraceWriter::FlushBuffer(Thread* thread, bool is_sync, bool release) {
  uintptr_t* method_trace_entries = thread->GetMethodTraceBuffer();
  size_t* current_offset = thread->GetMethodTraceIndexPtr();
  size_t tid = thread->GetTid();
  DCHECK(method_trace_entries != nullptr);
  if (is_sync || thread_pool_ == nullptr) {
    std::unordered_map<ArtMethod*, std::string> method_infos;
    PreProcessTraceForMethodInfos(method_trace_entries, *current_offset, method_infos);
    FlushBuffer(method_trace_entries, *current_offset, tid, method_infos);
    if (release) {
      thread->SetMethodTraceBuffer(nullptr);
      *current_offset = 0;
    } else {
      *current_offset = kPerThreadBufSize;
    }
  } else {
    int old_index = GetMethodTraceIndex(method_trace_entries);
    thread_pool_->AddTask(
        Thread::Current(),
        new TraceWriterTask(this, old_index, method_trace_entries, *current_offset, tid));
    if (release) {
      thread->SetMethodTraceBuffer(nullptr);
      *current_offset = 0;
    } else {
      thread->SetMethodTraceBuffer(AcquireTraceBuffer(tid));
      *current_offset = kPerThreadBufSize;
    }
  }
  return;
}
void TraceWriter::ReadValuesFromRecord(uintptr_t* method_trace_entries,
                                       size_t record_index,
                                       MethodTraceRecord& record,
                                       bool has_thread_cpu_clock,
                                       bool has_wall_clock) {
  uintptr_t method_and_action = method_trace_entries[record_index++];
  record.method = reinterpret_cast<ArtMethod*>(method_and_action & kMaskTraceAction);
  CHECK(record.method != nullptr);
  record.action = DecodeTraceAction(method_and_action);
  record.thread_cpu_time = 0;
  record.wall_clock_time = 0;
  if (has_thread_cpu_clock) {
    record.thread_cpu_time = method_trace_entries[record_index++];
  }
  if (has_wall_clock) {
    uint64_t timestamp = method_trace_entries[record_index++];
    if (art::kRuntimePointerSize == PointerSize::k32) {
      uint64_t high_timestamp = method_trace_entries[record_index++];
      timestamp = (high_timestamp << 32 | timestamp);
    }
    record.wall_clock_time = GetMicroTime(timestamp) - start_time_;
  }
}
void TraceWriter::FlushEntriesFormatV1(
    uintptr_t* method_trace_entries,
    size_t tid,
    const std::unordered_map<ArtMethod*, std::string>& method_infos,
    size_t end_offset,
    size_t* current_index,
    uint8_t* buffer_ptr) {
  uint16_t thread_id = GetThreadEncoding(tid);
  bool has_thread_cpu_clock = UseThreadCpuClock(clock_source_);
  bool has_wall_clock = UseWallClock(clock_source_);
  size_t buffer_index = *current_index;
  size_t num_entries = GetNumEntries(clock_source_);
  const size_t record_size = GetRecordSize(clock_source_, trace_format_version_);
  for (size_t entry_index = kPerThreadBufSize; entry_index != end_offset;) {
    entry_index -= num_entries;
    MethodTraceRecord record;
    ReadValuesFromRecord(
        method_trace_entries, entry_index, record, has_thread_cpu_clock, has_wall_clock);
    auto [method_id, is_new_method] = GetMethodEncoding(record.method);
    if (is_new_method && trace_output_mode_ == TraceOutputMode::kStreaming) {
      RecordMethodInfo(method_infos.find(record.method)->second, method_id);
    }
    DCHECK_LT(buffer_index + record_size, buffer_size_);
    EncodeEventEntry(buffer_ptr + buffer_index,
                     thread_id,
                     method_id,
                     record.action,
                     record.thread_cpu_time,
                     record.wall_clock_time);
    buffer_index += record_size;
  }
  *current_index = buffer_index;
}
void TraceWriter::FlushEntriesFormatV2(
    uintptr_t* method_trace_entries,
    size_t tid,
    const std::unordered_map<ArtMethod*, std::string>& method_infos,
    size_t num_records,
    size_t* current_index,
    uint8_t* init_buffer_ptr) {
  bool has_thread_cpu_clock = UseThreadCpuClock(clock_source_);
  bool has_wall_clock = UseWallClock(clock_source_);
  size_t num_entries = GetNumEntries(clock_source_);
  uint32_t prev_wall_timestamp = 0;
  uint32_t prev_thread_timestamp = 0;
  int32_t init_method_action_encoding = 0;
  bool is_first_entry = true;
  uint8_t* current_buffer_ptr = init_buffer_ptr;
  uint32_t header_size = (clock_source_ == TraceClockSource::kDual) ? kEntryHeaderSizeDualClockV2 :
                                                                      kEntryHeaderSizeSingleClockV2;
  size_t entry_index = kPerThreadBufSize;
  for (size_t i = 0; i < num_records; i++) {
    entry_index -= num_entries;
    MethodTraceRecord record;
    ReadValuesFromRecord(
        method_trace_entries, entry_index, record, has_thread_cpu_clock, has_wall_clock);
    auto [method_id, is_new_method] = GetMethodEncoding(record.method);
    if (is_new_method && trace_output_mode_ == TraceOutputMode::kStreaming) {
      RecordMethodInfo(method_infos.find(record.method)->second, method_id);
    }
    DCHECK(method_id < (1 << (31 - TraceActionBits)));
    uint32_t method_action_encoding = (method_id << TraceActionBits) | record.action;
    if (is_first_entry) {
      prev_wall_timestamp = record.wall_clock_time;
      prev_thread_timestamp = record.thread_cpu_time;
      init_method_action_encoding = method_action_encoding;
      is_first_entry = false;
      EncodeEventBlockHeader(init_buffer_ptr,
                             tid,
                             method_action_encoding,
                             prev_thread_timestamp,
                             prev_wall_timestamp,
                             num_records);
      current_buffer_ptr += header_size;
    } else {
      current_buffer_ptr = EncodeSignedLeb128(current_buffer_ptr,
                                           (method_action_encoding - init_method_action_encoding));
      if (has_wall_clock) {
        current_buffer_ptr =
            EncodeUnsignedLeb128(current_buffer_ptr, (record.wall_clock_time - prev_wall_timestamp));
        prev_wall_timestamp = record.wall_clock_time;
      }
      if (has_thread_cpu_clock) {
        current_buffer_ptr =
            EncodeUnsignedLeb128(current_buffer_ptr, (record.thread_cpu_time - prev_thread_timestamp));
        prev_thread_timestamp = record.thread_cpu_time;
      }
    }
  }
  uint8_t* total_size_loc = init_buffer_ptr + header_size - 2;
  Append2LE(total_size_loc, current_buffer_ptr - (init_buffer_ptr + header_size));
  *current_index += current_buffer_ptr - init_buffer_ptr;
}
void TraceWriter::FlushBuffer(uintptr_t* method_trace_entries,
                              size_t current_offset,
                              size_t tid,
                              const std::unordered_map<ArtMethod*, std::string>& method_infos) {
  MutexLock mu(Thread::Current(), tracing_lock_);
  size_t current_index = 0;
  uint8_t* buffer_ptr = buf_.get();
  size_t buffer_size = buffer_size_;
  size_t num_entries = GetNumEntries(clock_source_);
  size_t num_records = (kPerThreadBufSize - current_offset) / num_entries;
  DCHECK_EQ((kPerThreadBufSize - current_offset) % num_entries, 0u);
  const size_t record_size = GetRecordSize(clock_source_, trace_format_version_);
  DCHECK_LT(record_size, kPerThreadBufSize);
  if (trace_output_mode_ != TraceOutputMode::kStreaming) {
    current_index = cur_offset_;
    if (cur_offset_ + record_size * num_records >= buffer_size) {
      overflow_ = true;
      return;
    }
  }
  num_records_ += num_records;
  DCHECK_GT(buffer_size_, record_size * num_entries);
  if (trace_format_version_ == Trace::kFormatV1) {
    FlushEntriesFormatV1(
        method_trace_entries, tid, method_infos, current_offset, &current_index, buffer_ptr);
  } else {
    FlushEntriesFormatV2(
        method_trace_entries, tid, method_infos, num_records, &current_index, buffer_ptr);
  }
  if (trace_output_mode_ == TraceOutputMode::kStreaming) {
    if (!trace_file_->WriteFully(buffer_ptr, current_index)) {
      PLOG(WARNING) << "Failed streaming a tracing event.";
    }
  } else {
    cur_offset_ = current_index;
  }
  return;
}
void Trace::LogMethodTraceEvent(Thread* thread,
                                ArtMethod* method,
                                TraceAction action,
                                uint32_t thread_clock_diff,
                                uint64_t timestamp_counter) {
  if (trace_writer_->HasOverflow()) {
    return;
  }
  uintptr_t* method_trace_buffer = thread->GetMethodTraceBuffer();
  size_t* current_index = thread->GetMethodTraceIndexPtr();
  if (method_trace_buffer == nullptr) {
    method_trace_buffer = trace_writer_->AcquireTraceBuffer(thread->GetTid());
    DCHECK(method_trace_buffer != nullptr);
    thread->SetMethodTraceBuffer(method_trace_buffer);
    *current_index = kPerThreadBufSize;
    trace_writer_->RecordThreadInfo(thread);
  }
  size_t required_entries = GetNumEntries(clock_source_);
  if (*current_index < required_entries) {
    method_trace_buffer = trace_writer_->PrepareBufferForNewEntries(thread);
    if (method_trace_buffer == nullptr) {
      return;
    }
  }
  int new_entry_index = *current_index - required_entries;
  *current_index = new_entry_index;
  method = method->GetNonObsoleteMethod();
  method_trace_buffer[new_entry_index++] = reinterpret_cast<uintptr_t>(method) | action;
  if (UseThreadCpuClock(clock_source_)) {
    method_trace_buffer[new_entry_index++] = thread_clock_diff;
  }
  if (UseWallClock(clock_source_)) {
    if (art::kRuntimePointerSize == PointerSize::k32) {
      method_trace_buffer[new_entry_index++] = static_cast<uint32_t>(timestamp_counter);
      method_trace_buffer[new_entry_index++] = timestamp_counter >> 32;
    } else {
      method_trace_buffer[new_entry_index++] = timestamp_counter;
    }
  }
}
void TraceWriter::EncodeEventEntry(uint8_t* ptr,
                                   uint16_t thread_id,
                                   uint32_t method_index,
                                   TraceAction action,
                                   uint32_t thread_clock_diff,
                                   uint32_t wall_clock_diff) {
  static constexpr size_t kPacketSize = 14U;
  DCHECK(method_index < (1 << (32 - TraceActionBits)));
  uint32_t method_value = (method_index << TraceActionBits) | action;
  Append2LE(ptr, thread_id);
  Append4LE(ptr + 2, method_value);
  ptr += 6;
  if (UseThreadCpuClock(clock_source_)) {
    Append4LE(ptr, thread_clock_diff);
    ptr += 4;
  }
  if (UseWallClock(clock_source_)) {
    Append4LE(ptr, wall_clock_diff);
  }
  static_assert(kPacketSize == 2 + 4 + 4 + 4, "Packet size incorrect.");
}
void TraceWriter::EncodeEventBlockHeader(uint8_t* ptr,
                                         uint32_t thread_id,
                                         uint32_t init_method_index,
                                         uint32_t init_thread_clock,
                                         uint32_t init_wall_clock,
                                         uint16_t num_records) {
  ptr[0] = kEntryHeaderV2;
  Append4LE(ptr + 1, thread_id);
  Append4LE(ptr + 5, init_method_index);
  ptr += 9;
  if (UseThreadCpuClock(clock_source_)) {
    Append4LE(ptr, init_thread_clock);
    ptr += 4;
  }
  if (UseWallClock(clock_source_)) {
    Append4LE(ptr, init_wall_clock);
    ptr += 4;
  }
  Append2LE(ptr, num_records - 1);
}
void TraceWriter::EnsureSpace(uint8_t* buffer,
                              size_t* current_index,
                              size_t buffer_size,
                              size_t required_size) {
  if (*current_index + required_size < buffer_size) {
    return;
  }
  if (!trace_file_->WriteFully(buffer, *current_index)) {
    PLOG(WARNING) << "Failed streaming a tracing event.";
  }
  *current_index = 0;
}
void TraceWriter::DumpMethodList(std::ostream& os) {
  MutexLock mu(Thread::Current(), tracing_lock_);
  for (auto const& entry : art_method_id_map_) {
    os << GetMethodLine(GetMethodInfoLine(entry.first), entry.second);
  }
}
void TraceWriter::DumpThreadList(std::ostream& os) {
  MutexLock mu(Thread::Current(), tracing_lock_);
  for (const auto& it : threads_list_) {
    os << it.first << "\t" << it.second << "\n";
  }
}
TraceOutputMode Trace::GetOutputMode() {
  MutexLock mu(Thread::Current(), *Locks::trace_lock_);
  CHECK(the_trace_ != nullptr) << "Trace output mode requested, but no trace currently running";
  return the_trace_->trace_writer_->GetOutputMode();
}
Trace::TraceMode Trace::GetMode() {
  MutexLock mu(Thread::Current(), *Locks::trace_lock_);
  CHECK(the_trace_ != nullptr) << "Trace mode requested, but no trace currently running";
  return the_trace_->trace_mode_;
}
int Trace::GetFlags() {
  MutexLock mu(Thread::Current(), *Locks::trace_lock_);
  CHECK(the_trace_ != nullptr) << "Trace flags requested, but no trace currently running";
  return the_trace_->flags_;
}
int Trace::GetIntervalInMillis() {
  MutexLock mu(Thread::Current(), *Locks::trace_lock_);
  CHECK(the_trace_ != nullptr) << "Trace interval requested, but no trace currently running";
  return the_trace_->interval_us_;
}
size_t Trace::GetBufferSize() {
  MutexLock mu(Thread::Current(), *Locks::trace_lock_);
  CHECK(the_trace_ != nullptr) << "Trace buffer size requested, but no trace currently running";
  return the_trace_->trace_writer_->GetBufferSize();
}
bool Trace::IsTracingEnabled() {
  MutexLock mu(Thread::Current(), *Locks::trace_lock_);
  return the_trace_ != nullptr;
}
}
