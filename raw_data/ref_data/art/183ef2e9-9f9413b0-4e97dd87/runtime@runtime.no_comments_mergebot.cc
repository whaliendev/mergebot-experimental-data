#include "runtime.h"
#include <utility>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <fcntl.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string.h>
#include <thread>
#include <unordered_set>
#include <vector>
#include "android-base/strings.h"
#include "aot_class_linker.h"
#include "arch/arm/registers_arm.h"
#include "arch/arm64/registers_arm64.h"
#include "arch/context.h"
#include "arch/instruction_set_features.h"
#include "arch/x86/registers_x86.h"
#include "arch/x86_64/registers_x86_64.h"
#include "art_field-inl.h"
#include "art_method-inl.h"
#include "asm_support.h"
#include "base/aborting.h"
#include "base/arena_allocator.h"
#include "base/atomic.h"
#include "base/dumpable.h"
#include "base/enums.h"
#include "base/file_utils.h"
#include "base/flags.h"
#include "base/malloc_arena_pool.h"
#include "base/mem_map_arena_pool.h"
#include "base/memory_tool.h"
#include "base/mutex.h"
#include "base/os.h"
#include "base/quasi_atomic.h"
#include "base/sdk_version.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "base/unix_file/fd_file.h"
#include "base/utils.h"
#include "class_linker-inl.h"
#include "class_root-inl.h"
#include "compiler_callbacks.h"
#include "debugger.h"
#include "dex/art_dex_file_loader.h"
#include "dex/dex_file_loader.h"
#include "elf_file.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "entrypoints/entrypoint_utils-inl.h"
#include "experimental_flags.h"
#include "fault_handler.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/heap.h"
#include "gc/scoped_gc_critical_section.h"
#include "gc/space/image_space.h"
#include "gc/space/space-inl.h"
#include "gc/system_weak.h"
#include "gc/task_processor.h"
#include "handle_scope-inl.h"
#include "hidden_api.h"
#include "image-inl.h"
#include "indirect_reference_table.h"
#include "instrumentation.h"
#include "intern_table-inl.h"
#include "interpreter/interpreter.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "jit/profile_saver.h"
#include "jni/java_vm_ext.h"
#include "jni/jni_id_manager.h"
#include "jni_id_type.h"
#include "linear_alloc.h"
#include "memory_representation.h"
#include "metrics/statsd.h"
#include "mirror/array.h"
#include "mirror/class-alloc-inl.h"
#include "mirror/class-inl.h"
#include "mirror/class_ext.h"
#include "mirror/class_loader-inl.h"
#include "mirror/emulated_stack_frame.h"
#include "mirror/field.h"
#include "mirror/method.h"
#include "mirror/method_handle_impl.h"
#include "mirror/method_handles_lookup.h"
#include "mirror/method_type.h"
#include "mirror/stack_trace_element.h"
#include "mirror/throwable.h"
#include "mirror/var_handle.h"
#include "monitor.h"
#include "native/dalvik_system_DexFile.h"
#include "native/dalvik_system_BaseDexClassLoader.h"
#include "native/dalvik_system_VMDebug.h"
#include "native/dalvik_system_VMRuntime.h"
#include "native/dalvik_system_VMStack.h"
#include "native/dalvik_system_ZygoteHooks.h"
#include "native/java_lang_Class.h"
#include "native/java_lang_Object.h"
#include "native/java_lang_StackStreamFactory.h"
#include "native/java_lang_String.h"
#include "native/java_lang_StringFactory.h"
#include "native/java_lang_System.h"
#include "native/java_lang_Thread.h"
#include "native/java_lang_Throwable.h"
#include "native/java_lang_VMClassLoader.h"
#include "native/java_lang_invoke_MethodHandle.h"
#include "native/java_lang_invoke_MethodHandleImpl.h"
#include "native/java_lang_ref_FinalizerReference.h"
#include "native/java_lang_ref_Reference.h"
#include "native/java_lang_reflect_Array.h"
#include "native/java_lang_reflect_Constructor.h"
#include "native/java_lang_reflect_Executable.h"
#include "native/java_lang_reflect_Field.h"
#include "native/java_lang_reflect_Method.h"
#include "native/java_lang_reflect_Parameter.h"
#include "native/java_lang_reflect_Proxy.h"
#include "native/java_util_concurrent_atomic_AtomicLong.h"
#include "native/libcore_io_Memory.h"
#include "native/libcore_util_CharsetUtils.h"
#include "native/org_apache_harmony_dalvik_ddmc_DdmServer.h"
#include "native/org_apache_harmony_dalvik_ddmc_DdmVmInternal.h"
#include "native/sun_misc_Unsafe.h"
#include "native/jdk_internal_misc_Unsafe.h"
#include "native_bridge_art_interface.h"
#include "native_stack_dump.h"
#include "nativehelper/scoped_local_ref.h"
#include "nterp_helpers.h"
#include "oat.h"
#include "oat_file_manager.h"
#include "oat_quick_method_header.h"
#include "object_callbacks.h"
#include "odr_statslog/odr_statslog.h"
#include "parsed_options.h"
#include "quick/quick_method_frame_info.h"
#include "reflection.h"
#include "runtime_callbacks.h"
#include "runtime_common.h"
#include "runtime_image.h"
#include "runtime_intrinsics.h"
#include "runtime_options.h"
#include "scoped_thread_state_change-inl.h"
#include "sigchain.h"
#include "signal_catcher.h"
#include "signal_set.h"
#include "thread.h"
#include "thread_list.h"
#include "ti/agent.h"
#include "trace.h"
#include "transaction.h"
#include "vdex_file.h"
#include "verifier/class_verifier.h"
#include "well_known_classes.h"
#include "well_known_classes-inl.h"
#ifdef ART_TARGET_ANDROID
#include <android/api-level.h>
#include <android/set_abort_message.h>
#include "com_android_apex.h"
namespace apex = com::android::apex;
#endif
#define ASM_DEFINE(NAME,EXPR) static_assert((NAME) == (EXPR), "Unexpected value of " #NAME);
#include "asm_defines.def"
#undef ASM_DEFINE
namespace art {
static constexpr bool kEnableJavaStackTraceHandler = false;
static constexpr double kLowMemoryMinLoadFactor = 0.5;
static constexpr double kLowMemoryMaxLoadFactor = 0.8;
static constexpr double kNormalMinLoadFactor = 0.4;
static constexpr double kNormalMaxLoadFactor = 0.7;
void Runtime::EndThreadBirth() struct TraceConfig {
  Trace::TraceMode trace_mode;
  Trace::TraceOutputMode trace_output_mode;
  std::string trace_file;
  size_t trace_file_size;
  TraceClockSource clock_source;
};
namespace {
#ifdef __APPLE__
inline char** GetEnviron() {
  return *_NSGetEnviron();
}
#else
extern "C" char** environ;
inline char** GetEnviron() { return environ; }
#endif
void CheckConstants() {
  CHECK_EQ(mirror::Array::kFirstElementOffset, mirror::Array::FirstElementOffset());
}
}
Runtime::Runtime()
    : resolution_method_(nullptr),
      imt_conflict_method_(nullptr),
      imt_unimplemented_method_(nullptr),
      instruction_set_(InstructionSet::kNone),
      compiler_callbacks_(nullptr),
      is_zygote_(false),
      is_primary_zygote_(false),
      is_system_server_(false),
      must_relocate_(false),
      is_concurrent_gc_enabled_(true),
      is_explicit_gc_disabled_(false),
      image_dex2oat_enabled_(true),
      default_stack_size_(0),
      heap_(nullptr),
      max_spins_before_thin_lock_inflation_(Monitor::kDefaultMaxSpinsBeforeThinLockInflation),
      monitor_list_(nullptr),
      monitor_pool_(nullptr),
      thread_list_(nullptr),
      intern_table_(nullptr),
      class_linker_(nullptr),
      signal_catcher_(nullptr),
      java_vm_(nullptr),
      thread_pool_ref_count_(0u),
      fault_message_(nullptr),
      threads_being_born_(0),
      shutdown_cond_(new ConditionVariable("Runtime shutdown", *Locks::runtime_shutdown_lock_)),
      shutting_down_(false),
      shutting_down_started_(false),
      started_(false),
      finished_starting_(false),
      vfprintf_(nullptr),
      exit_(nullptr),
      abort_(nullptr),
      stats_enabled_(false),
      is_running_on_memory_tool_(kRunningOnMemoryTool),
      instrumentation_(),
      main_thread_group_(nullptr),
      system_thread_group_(nullptr),
      system_class_loader_(nullptr),
      dump_gc_performance_on_shutdown_(false),
      preinitialization_transactions_(),
      verify_(verifier::VerifyMode::kNone),
      target_sdk_version_(static_cast<uint32_t>(SdkVersion::kUnset)),
      compat_framework_(),
      implicit_null_checks_(false),
      implicit_so_checks_(false),
      implicit_suspend_checks_(false),
      no_sig_chain_(false),
      force_native_bridge_(false),
      is_native_bridge_loaded_(false),
      is_native_debuggable_(false),
      async_exceptions_thrown_(false),
      non_standard_exits_enabled_(false),
      runtime_debug_state_(RuntimeDebugState::kNonJavaDebuggable),
      monitor_timeout_enable_(false),
      monitor_timeout_ns_(0),
      zygote_max_failed_boots_(0),
      experimental_flags_(ExperimentalFlags::kNone),
      oat_file_manager_(nullptr),
      is_low_memory_mode_(false),
      madvise_willneed_vdex_filesize_(0),
      madvise_willneed_odex_filesize_(0),
      madvise_willneed_art_filesize_(0),
      safe_mode_(false),
      hidden_api_policy_(hiddenapi::EnforcementPolicy::kDisabled),
      core_platform_api_policy_(hiddenapi::EnforcementPolicy::kDisabled),
      test_api_policy_(hiddenapi::EnforcementPolicy::kDisabled),
      dedupe_hidden_api_warnings_(true),
      hidden_api_access_event_log_rate_(0),
      dump_native_stack_on_sig_quit_(true),
      process_state_(kProcessStateJankPerceptible),
      zygote_no_threads_(false),
      verifier_logging_threshold_ms_(100),
      verifier_missing_kthrow_fatal_(false),
      perfetto_hprof_enabled_(false),
      perfetto_javaheapprof_enabled_(false),
      out_of_memory_error_hook_(nullptr) {
  static_assert(
      Runtime::kCalleeSaveSize == static_cast<uint32_t>(CalleeSaveType::kLastCalleeSaveType),
      "Unexpected size");
  CheckConstants();
  std::fill(callee_save_methods_, callee_save_methods_ + arraysize(callee_save_methods_), 0u);
  interpreter::CheckInterpreterAsmConstants();
  callbacks_.reset(new RuntimeCallbacks());
  for (size_t i = 0; i <= static_cast<size_t>(DeoptimizationKind::kLast); ++i) {
    deoptimization_counts_[i] = 0u;
  }
}
Runtime::~Runtime() {
  ScopedTrace trace("Runtime shutdown");
  if (is_native_bridge_loaded_) {
    UnloadNativeBridge();
  }
  Thread* self = Thread::Current();
  const bool attach_shutdown_thread = self == nullptr;
  if (attach_shutdown_thread) {
    bool thread_attached = AttachCurrentThread("Shutdown thread",
                                                                false,
                                               GetSystemThreadGroup(),
                                                                  IsStarted(),
                                                                           false);
    if (UNLIKELY(!thread_attached)) {
      LOG(WARNING) << "Failed to attach shutdown thread. Trying again without a peer.";
      CHECK(AttachCurrentThread("Shutdown thread (no java peer)",
                                                 false,
                                                  nullptr,
                                                   false));
    }
    self = Thread::Current();
  } else {
    LOG(WARNING) << "Current thread not detached in Runtime shutdown";
  }
  if (dump_gc_performance_on_shutdown_) {
    heap_->CalculatePreGcWeightedAllocatedBytes();
    uint64_t process_cpu_end_time = ProcessCpuNanoTime();
    ScopedLogSeverity sls(LogSeverity::INFO);
    heap_->DumpGcPerformanceInfo(LOG_STREAM(INFO));
    uint64_t process_cpu_time = process_cpu_end_time - heap_->GetProcessCpuStartTime();
    uint64_t gc_cpu_time = heap_->GetTotalGcCpuTime();
    float ratio = static_cast<float>(gc_cpu_time) / process_cpu_time;
    LOG_STREAM(INFO) << "GC CPU time " << PrettyDuration(gc_cpu_time) << " out of process CPU time "
                     << PrettyDuration(process_cpu_time) << " (" << ratio << ")"
                     << "\n";
    double pre_gc_weighted_allocated_bytes =
        heap_->GetPreGcWeightedAllocatedBytes() / process_cpu_time;
    double post_gc_weighted_allocated_bytes =
        heap_->GetPostGcWeightedAllocatedBytes() /
        (heap_->GetPostGCLastProcessCpuTime() - heap_->GetProcessCpuStartTime());
    LOG_STREAM(INFO) << "Average bytes allocated at GC start, weighted by CPU time between GCs: "
                     << static_cast<uint64_t>(pre_gc_weighted_allocated_bytes) << " ("
                     << PrettySize(pre_gc_weighted_allocated_bytes) << ")";
    LOG_STREAM(INFO) << "Average bytes allocated at GC end, weighted by CPU time between GCs: "
                     << static_cast<uint64_t>(post_gc_weighted_allocated_bytes) << " ("
                     << PrettySize(post_gc_weighted_allocated_bytes) << ")"
                     << "\n";
  }
  WaitForThreadPoolWorkersToStart();
  if (jit_ != nullptr) {
    jit_->WaitForWorkersToBeCreated();
    jit_->StopProfileSaver();
    jit_->DeleteThreadPool();
  }
  if (oat_file_manager_ != nullptr) {
    oat_file_manager_->WaitForWorkersToBeCreated();
  }
  heap_->DisableGCForShutdown();
  heap_->WaitForWorkersToBeCreated();
  heap_->WaitForGcToComplete(gc::kGcCauseBackground, self);
  {
    ScopedTrace trace2("Wait for shutdown cond");
    MutexLock mu(self, *Locks::runtime_shutdown_lock_);
    shutting_down_started_ = true;
    while (threads_being_born_ > 0) {
      shutdown_cond_->Wait(self);
    }
    SetShuttingDown();
  }
  CHECK(self != nullptr);
  if (IsFinishedStarting()) {
    ScopedTrace trace2("Waiting for Daemons");
    self->ClearException();
    ScopedObjectAccess soa(self);
    WellKnownClasses::java_lang_Daemons_stop->InvokeStatic<'V'>(self);
  }
  Trace::Shutdown();
  {
    ScopedObjectAccess soa(self);
    callbacks_->NextRuntimePhase(RuntimePhaseCallback::RuntimePhase::kDeath);
  }
  heap_->DeleteThreadPool();
  if (oat_file_manager_ != nullptr) {
    oat_file_manager_->DeleteThreadPool();
  }
  DeleteThreadPool();
  CHECK(thread_pool_ == nullptr);
  if (attach_shutdown_thread) {
    DetachCurrentThread( false);
    self = nullptr;
  }
  GetRuntimeCallbacks()->StopDebugger();
  delete signal_catcher_;
  signal_catcher_ = nullptr;
  metrics_reporter_.reset();
  {
    ScopedTrace trace2("Delete thread list");
    thread_list_->ShutDown();
  }
  for (auto& agent : agents_) {
    agent->Unload();
  }
  for (auto& plugin : plugins_) {
    plugin.Unload();
  }
  delete thread_list_;
  thread_list_ = nullptr;
  if (jit_ != nullptr) {
    VLOG(jit) << "Deleting jit";
    jit_.reset(nullptr);
    jit_code_cache_.reset(nullptr);
  }
  fault_manager.Shutdown();
  ScopedTrace trace2("Delete state");
  delete monitor_list_;
  monitor_list_ = nullptr;
  delete monitor_pool_;
  monitor_pool_ = nullptr;
  delete class_linker_;
  class_linker_ = nullptr;
  delete small_lrt_allocator_;
  small_lrt_allocator_ = nullptr;
  delete heap_;
  heap_ = nullptr;
  delete intern_table_;
  intern_table_ = nullptr;
  delete oat_file_manager_;
  oat_file_manager_ = nullptr;
  Thread::Shutdown();
  QuasiAtomic::Shutdown();
  verifier::ClassVerifier::Shutdown();
  java_vm_.reset();
  linear_alloc_.reset();
  startup_linear_alloc_.reset();
  linear_alloc_arena_pool_.reset();
  arena_pool_.reset();
  jit_arena_pool_.reset();
  protected_fault_page_.Reset();
  MemMap::Shutdown();
  CHECK(instance_ == nullptr || instance_ == this);
  instance_ = nullptr;
  WellKnownClasses::Clear();
}
struct AbortState {
  void Dump(std::ostream& os) const {
    if (gAborting > 1) {
      os << "Runtime aborting --- recursively, so no thread-specific detail!\n";
      DumpRecursiveAbort(os);
      return;
    }
    gAborting++;
    os << "Runtime aborting...\n";
    if (Runtime::Current() == nullptr) {
      os << "(Runtime does not yet exist!)\n";
      DumpNativeStack(os, GetTid(), "  native: ", nullptr);
      return;
    }
    Thread* self = Thread::Current();
    DumpAllThreads(os, self);
    if (self == nullptr) {
      os << "(Aborting thread was not attached to runtime!)\n";
      DumpNativeStack(os, GetTid(), "  native: ", nullptr);
    } else {
      os << "Aborting thread:\n";
      if (Locks::mutator_lock_->IsExclusiveHeld(self) || Locks::mutator_lock_->IsSharedHeld(self)) {
        DumpThread(os, self);
      } else {
        if (Locks::mutator_lock_->SharedTryLock(self)) {
          DumpThread(os, self);
          Locks::mutator_lock_->SharedUnlock(self);
        }
      }
    }
  }
  void DumpThread(std::ostream& os, Thread* self) const NO_THREAD_SAFETY_ANALYSIS {
    DCHECK(Locks::mutator_lock_->IsExclusiveHeld(self) || Locks::mutator_lock_->IsSharedHeld(self));
    self->Dump(os);
    if (self->IsExceptionPending()) {
      mirror::Throwable* exception = self->GetException();
      os << "Pending exception " << exception->Dump();
    }
  }
  void DumpAllThreads(std::ostream& os, Thread* self) const {
    Runtime* runtime = Runtime::Current();
    if (runtime != nullptr) {
      ThreadList* thread_list = runtime->GetThreadList();
      if (thread_list != nullptr) {
        bool tll_already_held = Locks::thread_list_lock_->IsExclusiveHeld(self);
        bool tscl_already_held = Locks::thread_suspend_count_lock_->IsExclusiveHeld(self);
        if (tll_already_held || tscl_already_held) {
          os << "Skipping all-threads dump as locks are held:"
             << (tll_already_held ? "" : " thread_list_lock")
             << (tscl_already_held ? "" : " thread_suspend_count_lock") << "\n";
          return;
        }
        bool ml_already_exlusively_held = Locks::mutator_lock_->IsExclusiveHeld(self);
        if (ml_already_exlusively_held) {
          os << "Skipping all-threads dump as mutator lock is exclusively held.";
          return;
        }
        bool ml_already_held = Locks::mutator_lock_->IsSharedHeld(self);
        if (!ml_already_held) {
          os << "Dumping all threads without mutator lock held\n";
        }
        os << "All threads:\n";
        thread_list->Dump(os);
      }
    }
  }
  void DumpRecursiveAbort(std::ostream& os) const NO_THREAD_SAFETY_ANALYSIS {
    static constexpr size_t kOnlyPrintWhenRecursionLessThan = 100u;
    if (gAborting < kOnlyPrintWhenRecursionLessThan) {
      gAborting++;
      DumpNativeStack(os, GetTid());
    }
  }
};
void Runtime::Abort(const char* msg) {
  auto old_value = gAborting.fetch_add(1);
  if (old_value == 0) {
#ifdef ART_TARGET_ANDROID
    android_set_abort_message(msg);
#else
    Runtime* current = Runtime::Current();
    if (current != nullptr) {
      current->SetFaultMessage(msg);
    }
#endif
  }
  if (Thread::Current() == nullptr) {
    Runtime* current = Runtime::Current();
    if (current != nullptr && current->IsStarted() && !current->IsShuttingDownUnsafe()) {
      abort();
      UNREACHABLE();
    }
  }
  {
    ScopedThreadStateChange tsc(Thread::Current(), ThreadState::kNativeForAbort);
    Locks::abort_lock_->ExclusiveLock(Thread::Current());
  }
  fflush(nullptr);
  AbortState state;
  if (kIsTargetBuild) {
    LOG(FATAL_WITHOUT_ABORT) << Dumpable<AbortState>(state);
  } else {
    std::cerr << Dumpable<AbortState>(state);
  }
  if (msg != nullptr && strchr(msg, '\n') != nullptr) {
    LOG(FATAL_WITHOUT_ABORT) << msg;
  }
  FlagRuntimeAbort();
  if (Runtime::Current() != nullptr && Runtime::Current()->abort_ != nullptr) {
    LOG(FATAL_WITHOUT_ABORT) << "Calling abort hook...";
    Runtime::Current()->abort_();
    LOG(FATAL_WITHOUT_ABORT) << "Unexpectedly returned from abort hook!";
  }
  abort();
}
class UpdateMethodsPreFirstForkVisitor : public ClassVisitor {
 public:
  UpdateMethodsPreFirstForkVisitor(Thread* self, ClassLinker* class_linker)
      : vm_(down_cast<JNIEnvExt*>(self->GetJniEnv())->GetVm()),
        self_(self),
        class_linker_(class_linker),
        can_use_nterp_(interpreter::CanRuntimeUseNterp()) {}
  bool operator()(ObjPtr<mirror::Class> klass) override REQUIRES_SHARED(Locks::mutator_lock_) {
    bool is_initialized = klass->IsVisiblyInitialized();
    for (ArtMethod& method : klass->GetDeclaredMethods(kRuntimePointerSize)) {
      if (is_initialized || !method.NeedsClinitCheckBeforeCall()) {
        if (method.IsNative()) {
          const void* existing = method.GetEntryPointFromJni();
          if (method.IsCriticalNative() ? class_linker_->IsJniDlsymLookupCriticalStub(existing) :
                                          class_linker_->IsJniDlsymLookupStub(existing)) {
            const void* native_code =
                vm_->FindCodeForNativeMethod(&method, nullptr, false);
            if (native_code != nullptr) {
              class_linker_->RegisterNative(self_, &method, native_code);
            }
          }
        }
      } else if (can_use_nterp_) {
        const void* existing = method.GetEntryPointFromQuickCompiledCode();
        if (class_linker_->IsQuickResolutionStub(existing) && CanMethodUseNterp(&method)) {
          method.SetEntryPointFromQuickCompiledCode(interpreter::GetNterpWithClinitEntryPoint());
        }
      }
    }
    return true;
  }
 private:
  JavaVMExt* const vm_;
  Thread* const self_;
  ClassLinker* const class_linker_;
  const bool can_use_nterp_;
  DISALLOW_COPY_AND_ASSIGN(UpdateMethodsPreFirstForkVisitor);
};
void Runtime::PreZygoteFork() {
  if (GetJit() != nullptr) {
    GetJit()->PreZygoteFork();
  }
  if (!heap_->HasZygoteSpace()) {
    Thread* self = Thread::Current();
    class_linker_->MakeInitializedClassesVisiblyInitialized(self, true);
    ScopedObjectAccess soa(self);
    UpdateMethodsPreFirstForkVisitor visitor(self, class_linker_);
    class_linker_->VisitClasses(&visitor);
  }
  heap_->PreZygoteFork();
  PreZygoteForkNativeBridge();
}
void Runtime::PostZygoteFork() {
  jit::Jit* jit = GetJit();
  if (jit != nullptr) {
    jit->PostZygoteFork();
    if (kIsDebugBuild && jit->GetThreadPool() != nullptr) {
      jit->GetThreadPool()->CheckPthreadPriority(IsZygote() ?
                                                     jit->GetZygoteThreadPoolPthreadPriority() :
                                                     jit->GetThreadPoolPthreadPriority());
    }
  }
  ResetStats(0xFFFFFFFF);
}
void Runtime::CallExitHook(jint status) {
  if (exit_ != nullptr) {
    ScopedThreadStateChange tsc(Thread::Current(), ThreadState::kNative);
    exit_(status);
    LOG(WARNING) << "Exit hook returned instead of exiting!";
  }
}
void Runtime::SweepSystemWeaks(IsMarkedVisitor* visitor) {
  GetInternTable()->SweepInternTableWeaks(visitor);
  GetMonitorList()->SweepMonitorList(visitor);
  GetJavaVM()->SweepJniWeakGlobals(visitor);
  GetHeap()->SweepAllocationRecords(visitor);
  if (GetJit() != nullptr && GetHeap()->IsMovingGc()) {
    GetJit()->GetCodeCache()->SweepRootTables(visitor);
  }
  for (gc::AbstractSystemWeakHolder* holder : system_weak_holders_) {
    holder->Sweep(visitor);
  }
}
bool Runtime::ParseOptions(const RuntimeOptions& raw_options,
                           bool ignore_unrecognized,
                           RuntimeArgumentMap* runtime_options) {
  Locks::Init();
  InitLogging( nullptr, Abort);
  bool parsed = ParsedOptions::Parse(raw_options, ignore_unrecognized, runtime_options);
  if (!parsed) {
    LOG(ERROR) << "Failed to parse options";
    return false;
  }
  return true;
}
{
  Runtime* runtime = Runtime::Current();
  return runtime != nullptr && runtime->IsStarted() && !runtime->IsShuttingDownLocked();
}
void Runtime::AddGeneratedCodeRange(const void* start, size_t size) {
  if (HandlesSignalsInCompiledCode()) {
    fault_manager.AddGeneratedCodeRange(start, size);
  }
}
void Runtime::RemoveGeneratedCodeRange(const void* start, size_t size) {
  if (HandlesSignalsInCompiledCode()) {
    fault_manager.RemoveGeneratedCodeRange(start, size);
  }
}
bool Runtime::Create(RuntimeArgumentMap&& runtime_options) {
  if (Runtime::instance_ != nullptr) {
    return false;
  }
  instance_ = new Runtime;
  Locks::SetClientCallback(IsSafeToCallAbort);
  if (!instance_->Init(std::move(runtime_options))) {
    instance_ = nullptr;
    return false;
  }
  return true;
}
bool Runtime::Create(const RuntimeOptions& raw_options, bool ignore_unrecognized) {
  RuntimeArgumentMap runtime_options;
  return ParseOptions(raw_options, ignore_unrecognized, &runtime_options) &&
         Create(std::move(runtime_options));
}
static jobject CreateSystemClassLoader(Runtime* runtime) {
  if (runtime->IsAotCompiler() && !runtime->GetCompilerCallbacks()->IsBootImage()) {
    return nullptr;
  }
  ScopedObjectAccess soa(Thread::Current());
  ClassLinker* cl = runtime->GetClassLinker();
  auto pointer_size = cl->GetImagePointerSize();
  ObjPtr<mirror::Class> class_loader_class = GetClassRoot<mirror::ClassLoader>(cl);
  DCHECK(class_loader_class->IsInitialized());
  ArtMethod* getSystemClassLoader = class_loader_class->FindClassMethod(
      "getSystemClassLoader", "()Ljava/lang/ClassLoader;", pointer_size);
  CHECK(getSystemClassLoader != nullptr);
  CHECK(getSystemClassLoader->IsStatic());
  ObjPtr<mirror::Object> system_class_loader = getSystemClassLoader->InvokeStatic<'L'>(soa.Self());
  CHECK(system_class_loader != nullptr);
  ScopedAssertNoThreadSuspension sants(__FUNCTION__);
  jobject g_system_class_loader =
      runtime->GetJavaVM()->AddGlobalRef(soa.Self(), system_class_loader);
  soa.Self()->SetClassLoaderOverride(g_system_class_loader);
  ObjPtr<mirror::Class> thread_class = WellKnownClasses::java_lang_Thread.Get();
  ArtField* contextClassLoader =
      thread_class->FindDeclaredInstanceField("contextClassLoader", "Ljava/lang/ClassLoader;");
  CHECK(contextClassLoader != nullptr);
  contextClassLoader->SetObject<false>(soa.Self()->GetPeer(), system_class_loader);
  return g_system_class_loader;
}
std::string Runtime::GetCompilerExecutable() const {
  if (!compiler_executable_.empty()) {
    return compiler_executable_;
  }
  std::string compiler_executable = GetArtBinDir() + "/dex2oat";
  if (kIsDebugBuild) {
    compiler_executable += 'd';
  }
  if (kIsTargetBuild) {
    compiler_executable += Is64BitInstructionSet(kRuntimeISA) ? "64" : "32";
  }
  return compiler_executable;
}
void Runtime::RunRootClinits(Thread* self) {
  class_linker_->RunRootClinits(self);
  GcRoot<mirror::Throwable>* exceptions[] = {
      &pre_allocated_OutOfMemoryError_when_throwing_exception_,
      &pre_allocated_NoClassDefFoundError_,
  };
  for (GcRoot<mirror::Throwable>* exception : exceptions) {
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> klass = hs.NewHandle<mirror::Class>(exception->Read()->GetClass());
    class_linker_->EnsureInitialized(self, klass, true, true);
    self->AssertNoPendingException();
  }
}
bool Runtime::Start() {
  VLOG(startup) << "Runtime::Start entering";
  CHECK(!no_sig_chain_) << "A started runtime should have sig chain enabled";
#if defined(__linux__) && !defined(ART_TARGET_ANDROID) && defined(__x86_64__)
  if (kIsDebugBuild) {
    if (prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY) != 0) {
      PLOG(WARNING) << "Failed setting PR_SET_PTRACER to PR_SET_PTRACER_ANY";
    }
  }
#endif
  Thread* self = Thread::Current();
  started_ = true;
  RegisterRuntimeNativeMethods(self->GetJniEnv());
  class_linker_->RunEarlyRootClinits(self);
  InitializeIntrinsics();
  self->TransitionFromRunnableToSuspended(ThreadState::kNative);
  {
    ScopedTrace trace2("InitNativeMethods");
    InitNativeMethods();
  }
  art::hiddenapi::InitializeCorePlatformApiPrivateFields();
  InitThreadGroups(self);
  Thread::FinishStartup();
  if (jit_options_->UseJitCompilation() || jit_options_->GetSaveProfilingInfo()) {
    std::string error_msg;
    if (!jit::Jit::LoadCompilerLibrary(&error_msg)) {
      LOG(WARNING) << "Failed to load JIT compiler with error " << error_msg;
    }
    CreateJit();
#ifdef ADDRESS_SANITIZER
    if (jit_ != nullptr) {
      jit_->GetThreadPool()->WaitForWorkersToBeCreated();
    }
#endif
  }
  {
    ScopedObjectAccess soa(self);
    callbacks_->NextRuntimePhase(RuntimePhaseCallback::RuntimePhase::kStart);
  }
  system_class_loader_ = CreateSystemClassLoader(this);
  if (!is_zygote_) {
    if (is_native_bridge_loaded_) {
      PreInitializeNativeBridge(".");
    }
    NativeBridgeAction action =
        force_native_bridge_ ? NativeBridgeAction::kInitialize : NativeBridgeAction::kUnload;
    InitNonZygoteOrPostFork(self->GetJniEnv(),
                                                    false,
                                                   false,
                            action,
                            GetInstructionSetString(kRuntimeISA));
  }
  {
    ScopedObjectAccess soa(self);
    StartDaemonThreads();
    self->GetJniEnv()->AssertLocalsEmpty();
    callbacks_->NextRuntimePhase(RuntimePhaseCallback::RuntimePhase::kInit);
    self->GetJniEnv()->AssertLocalsEmpty();
  }
  VLOG(startup) << "Runtime::Start exiting";
  finished_starting_ = true;
  if (trace_config_.get() != nullptr && trace_config_->trace_file != "") {
    ScopedThreadStateChange tsc(self, ThreadState::kWaitingForMethodTracingStart);
    int flags = 0;
    if (trace_config_->clock_source == TraceClockSource::kDual) {
      flags = Trace::TraceFlag::kTraceClockSourceWallClock |
              Trace::TraceFlag::kTraceClockSourceThreadCpu;
    } else if (trace_config_->clock_source == TraceClockSource::kWall) {
      flags = Trace::TraceFlag::kTraceClockSourceWallClock;
    } else if (TraceClockSource::kThreadCpu == trace_config_->clock_source) {
      flags = Trace::TraceFlag::kTraceClockSourceThreadCpu;
    } else {
      LOG(ERROR) << "Unexpected clock source";
    }
    Trace::Start(trace_config_->trace_file.c_str(),
                 static_cast<int>(trace_config_->trace_file_size),
                 flags,
                 trace_config_->trace_output_mode,
                 trace_config_->trace_mode,
                 0);
  }
  if (jit_.get() != nullptr && jit_options_->GetSaveProfilingInfo() &&
      !jit_options_->GetProfileSaverOptions().GetProfilePath().empty()) {
    std::vector<std::string> dex_filenames;
    Split(class_path_string_, ':', &dex_filenames);
    RegisterAppInfo(
                         "",
        dex_filenames,
        jit_options_->GetProfileSaverOptions().GetProfilePath(),
                                 "",
        kVMRuntimePrimaryApk);
  }
  return true;
}
void Runtime::EndThreadBirth() REQUIRES(Locks::runtime_shutdown_lock_) {
  DCHECK_GT(threads_being_born_, 0U);
  threads_being_born_--;
  if (shutting_down_started_ && threads_being_born_ == 0) {
    shutdown_cond_->Broadcast(Thread::Current());
  }
}
void Runtime::InitNonZygoteOrPostFork(JNIEnv* env,
                                      bool is_system_server,
                                      bool is_child_zygote,
                                      NativeBridgeAction action,
                                      const char* isa,
                                      bool profile_system_server) {
  if (is_native_bridge_loaded_) {
    switch (action) {
      case NativeBridgeAction::kUnload:
        UnloadNativeBridge();
        is_native_bridge_loaded_ = false;
        break;
      case NativeBridgeAction::kInitialize:
        InitializeNativeBridge(env, isa);
        break;
    }
  }
  if (is_child_zygote) {
    return;
  }
  DCHECK(!IsZygote());
  if (is_system_server) {
    const char* system_server_classpath = getenv("SYSTEMSERVERCLASSPATH");
    if (system_server_classpath == nullptr || (strlen(system_server_classpath) == 0)) {
      LOG(WARNING) << "System server class path not set";
    } else {
      std::vector<std::string> jars = android::base::Split(system_server_classpath, ":");
      app_info_.RegisterAppInfo("android",
                                jars,
                                                            "",
                                                         "",
                                AppInfo::CodeType::kPrimaryApk);
    }
    SetProcessPackageName("android");
    if (profile_system_server) {
      jit_options_->SetWaitForJitNotificationsToSaveProfile(false);
      VLOG(profiler) << "Enabling system server profiles";
    }
  }
  if (!is_system_server) {
    ScopedTrace timing("CreateThreadPool");
    constexpr size_t kStackSize = 64 * KB;
    constexpr size_t kMaxRuntimeWorkers = 4u;
    const size_t num_workers =
        std::min(static_cast<size_t>(std::thread::hardware_concurrency()), kMaxRuntimeWorkers);
    MutexLock mu(Thread::Current(), *Locks::runtime_thread_pool_lock_);
    CHECK(thread_pool_ == nullptr);
    thread_pool_.reset(new ThreadPool("Runtime", num_workers, false, kStackSize));
    thread_pool_->StartWorkers(Thread::Current());
  }
  heap_->ResetGcPerformanceInfo();
  GetMetrics()->Reset();
  if (metrics_reporter_ != nullptr) {
    metrics::ReportingConfig metrics_config = metrics::ReportingConfig::FromFlags(is_system_server);
    metrics_reporter_->ReloadConfig(metrics_config);
    metrics::SessionData session_data{metrics::SessionData::CreateDefault()};
    session_data.session_id = GetRandomNumber<int64_t>(1, std::numeric_limits<int64_t>::max());
    metrics_reporter_->MaybeStartBackgroundThread(session_data);
    metrics_reporter_->NotifyAppInfoUpdated(&app_info_);
  }
  StartSignalCatcher();
  ScopedObjectAccess soa(Thread::Current());
  if (IsPerfettoHprofEnabled() &&
      (Dbg::IsJdwpAllowed() || IsProfileable() || IsProfileableFromShell() || IsJavaDebuggable() ||
       Runtime::Current()->IsSystemServer())) {
    std::string err;
    ScopedTrace tr("perfetto_hprof init.");
    ScopedThreadSuspension sts(Thread::Current(), ThreadState::kNative);
    if (!EnsurePerfettoPlugin(&err)) {
      LOG(WARNING) << "Failed to load perfetto_hprof: " << err;
    }
  }
  if (IsPerfettoJavaHeapStackProfEnabled() &&
      (Dbg::IsJdwpAllowed() || IsProfileable() || IsProfileableFromShell() || IsJavaDebuggable() ||
       Runtime::Current()->IsSystemServer())) {
    ScopedTrace tr("perfetto_javaheapprof init.");
  }
  if (Runtime::Current()->IsSystemServer()) {
    std::string err;
    ScopedTrace tr("odrefresh and device stats logging");
    ScopedThreadSuspension sts(Thread::Current(), ThreadState::kNative);
    if (!odrefresh::UploadStatsIfAvailable(&err)) {
      LOG(WARNING) << "Failed to upload odrefresh metrics: " << err;
    }
    metrics::ReportDeviceMetrics();
  }
  if (LIKELY(automatically_set_jni_ids_indirection_) && CanSetJniIdType()) {
    if (IsJavaDebuggable()) {
      SetJniIdType(JniIdType::kIndices);
    } else {
      SetJniIdType(JniIdType::kPointer);
    }
  }
  ATraceIntegerValue(
      "profilebootclasspath",
      static_cast<int>(jit_options_->GetProfileSaverOptions().GetProfileBootClassPath()));
  GetRuntimeCallbacks()->StartDebugger();
}
void Runtime::StartSignalCatcher() {
  if (!is_zygote_) {
    signal_catcher_ = new SignalCatcher();
  }
}
bool Runtime::IsShuttingDown(Thread* self) {
  MutexLock mu(self, *Locks::runtime_shutdown_lock_);
  return IsShuttingDownLocked();
}
void Runtime::StartDaemonThreads() {
  ScopedTrace trace(__FUNCTION__);
  VLOG(startup) << "Runtime::StartDaemonThreads entering";
  Thread* self = Thread::Current();
  DCHECK_EQ(self->GetState(), ThreadState::kRunnable);
  WellKnownClasses::java_lang_Daemons_start->InvokeStatic<'V'>(self);
  if (UNLIKELY(self->IsExceptionPending())) {
    LOG(FATAL) << "Error starting java.lang.Daemons: " << self->GetException()->Dump();
  }
  VLOG(startup) << "Runtime::StartDaemonThreads exiting";
}
static size_t OpenBootDexFiles(ArrayRef<const std::string> dex_filenames,
                               ArrayRef<const std::string> dex_locations,
                               ArrayRef<const int> dex_fds,
                               std::vector<std::unique_ptr<const DexFile>>* dex_files) {
  DCHECK(dex_files != nullptr) << "OpenDexFiles: out-param is nullptr";
  size_t failure_count = 0;
  for (size_t i = 0; i < dex_filenames.size(); i++) {
    const char* dex_filename = dex_filenames[i].c_str();
    const char* dex_location = dex_locations[i].c_str();
    const int dex_fd = i < dex_fds.size() ? dex_fds[i] : -1;
    static constexpr bool kVerifyChecksum = true;
    std::string error_msg;
    if (!OS::FileExists(dex_filename) && dex_fd < 0) {
      LOG(WARNING) << "Skipping non-existent dex file '" << dex_filename << "'";
      continue;
    }
    bool verify = Runtime::Current()->IsVerificationEnabled();
    ArtDexFileLoader dex_file_loader(dex_filename, dex_fd, dex_location);
    if (!dex_file_loader.Open(verify, kVerifyChecksum, &error_msg, dex_files)) {
      LOG(WARNING) << "Failed to open .dex from file '" << dex_filename << "' / fd " << dex_fd
                   << ": " << error_msg;
      ++failure_count;
    }
  }
  return failure_count;
}
void Runtime::SetSentinel(ObjPtr<mirror::Object> sentinel) {
  CHECK(sentinel_.Read() == nullptr);
  CHECK(sentinel != nullptr);
  CHECK(!heap_->IsMovableObject(sentinel));
  sentinel_ = GcRoot<mirror::Object>(sentinel);
}
GcRoot<mirror::Object> Runtime::GetSentinel() { return sentinel_; }
static inline void CreatePreAllocatedException(Thread* self,
                                               Runtime* runtime,
                                               GcRoot<mirror::Throwable>* exception,
                                               const char* exception_class_descriptor,
                                               const char* msg)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK_EQ(self, Thread::Current());
  ClassLinker* class_linker = runtime->GetClassLinker();
  ObjPtr<mirror::Class> klass = class_linker->FindSystemClass(self, exception_class_descriptor);
  CHECK(klass != nullptr);
  gc::AllocatorType allocator_type = runtime->GetHeap()->GetCurrentAllocator();
  ObjPtr<mirror::Throwable> exception_object =
      ObjPtr<mirror::Throwable>::DownCast(klass->Alloc(self, allocator_type));
  CHECK(exception_object != nullptr);
  *exception = GcRoot<mirror::Throwable>(exception_object);
  ObjPtr<mirror::String> message = mirror::String::AllocFromModifiedUtf8(self, msg);
  CHECK(message != nullptr);
  ObjPtr<mirror::Class> throwable = GetClassRoot<mirror::Throwable>(class_linker);
  ArtField* detailMessageField =
      throwable->FindDeclaredInstanceField("detailMessage", "Ljava/lang/String;");
  CHECK(detailMessageField != nullptr);
  detailMessageField->SetObject< false>(exception->Read(), message);
}
std::string Runtime::GetApexVersions(ArrayRef<const std::string> boot_class_path_locations) {
  std::vector<std::string_view> bcp_apexes;
  for (std::string_view jar : boot_class_path_locations) {
    std::string_view apex = ApexNameFromLocation(jar);
    if (!apex.empty()) {
      bcp_apexes.push_back(apex);
    }
  }
  static const char* kApexFileName = "/apex/apex-info-list.xml";
  std::string empty_apex_versions(bcp_apexes.size(), '/');
  if (!kIsTargetBuild || !OS::FileExists(kApexFileName)) {
    return empty_apex_versions;
  }
#ifdef ART_TARGET_ANDROID
  if (access(kApexFileName, R_OK) != 0) {
    PLOG(WARNING) << "Failed to read " << kApexFileName;
    return empty_apex_versions;
  }
  auto info_list = apex::readApexInfoList(kApexFileName);
  if (!info_list.has_value()) {
    LOG(WARNING) << "Failed to parse " << kApexFileName;
    return empty_apex_versions;
  }
  std::string result;
  std::map<std::string_view, const apex::ApexInfo*> apex_infos;
  for (const apex::ApexInfo& info : info_list->getApexInfo()) {
    if (info.getIsActive()) {
      apex_infos.emplace(info.getModuleName(), &info);
    }
  }
  for (const std::string_view& str : bcp_apexes) {
    auto info = apex_infos.find(str);
    if (info == apex_infos.end() || info->second->getIsFactory()) {
      result += '/';
    } else {
      uint64_t version = info->second->hasLastUpdateMillis() ? info->second->getLastUpdateMillis() :
                                                               info->second->getVersionCode();
      android::base::StringAppendF(&result, "/%" PRIu64, version);
    }
  }
  return result;
#else
  return empty_apex_versions;
#endif
}
void Runtime::InitializeApexVersions() {
  apex_versions_ =
      GetApexVersions(ArrayRef<const std::string>(Runtime::Current()->GetBootClassPathLocations()));
}
void Runtime::ReloadAllFlags(const std::string& caller) { FlagBase::ReloadAllFlags(caller); }
bool Runtime::Init(RuntimeArgumentMap&& runtime_options_in) {
  env_snapshot_.TakeSnapshot();
  using Opt = RuntimeArgumentMap;
  Opt runtime_options(std::move(runtime_options_in));
  ScopedTrace trace(__FUNCTION__);
  CHECK_EQ(static_cast<size_t>(sysconf(_SC_PAGE_SIZE)), kPageSize);
  ReloadAllFlags(__FUNCTION__);
  deny_art_apex_data_files_ = runtime_options.Exists(Opt::DenyArtApexDataFiles);
  if (deny_art_apex_data_files_) {
    LOG(WARNING) << "ART APEX data files are untrusted.";
  }
  if (runtime_options.Exists(Opt::UseStderrLogger)) {
    android::base::SetLogger(android::base::StderrLogger);
  }
  MemMap::Init();
  verifier_missing_kthrow_fatal_ = runtime_options.GetOrDefault(Opt::VerifierMissingKThrowFatal);
  force_java_zygote_fork_loop_ = runtime_options.GetOrDefault(Opt::ForceJavaZygoteForkLoop);
  perfetto_hprof_enabled_ = runtime_options.GetOrDefault(Opt::PerfettoHprof);
  perfetto_javaheapprof_enabled_ = runtime_options.GetOrDefault(Opt::PerfettoJavaHeapStackProf);
  {
    constexpr uintptr_t kSentinelAddr =
        RoundDown(static_cast<uintptr_t>(Context::kBadGprBase), kPageSize);
    protected_fault_page_ = MemMap::MapAnonymous("Sentinel fault page",
                                                 reinterpret_cast<uint8_t*>(kSentinelAddr),
                                                 kPageSize,
                                                 PROT_NONE,
                                                             true,
                                                           false,
                                                                 nullptr,
                                                               nullptr);
    if (!protected_fault_page_.IsValid()) {
      LOG(WARNING) << "Could not reserve sentinel fault page";
    } else if (reinterpret_cast<uintptr_t>(protected_fault_page_.Begin()) != kSentinelAddr) {
      LOG(WARNING) << "Could not reserve sentinel fault page at the right address.";
      protected_fault_page_.Reset();
    }
  }
  VLOG(startup) << "Runtime::Init -verbose:startup enabled";
  QuasiAtomic::Startup();
  oat_file_manager_ = new OatFileManager();
  jni_id_manager_.reset(new jni::JniIdManager());
  Thread::SetSensitiveThreadHook(runtime_options.GetOrDefault(Opt::HookIsSensitiveThread));
  Monitor::Init(runtime_options.GetOrDefault(Opt::LockProfThreshold),
                runtime_options.GetOrDefault(Opt::StackDumpLockProfThreshold));
  image_locations_ = runtime_options.ReleaseOrDefault(Opt::Image);
  SetInstructionSet(runtime_options.GetOrDefault(Opt::ImageInstructionSet));
  boot_class_path_ = runtime_options.ReleaseOrDefault(Opt::BootClassPath);
  boot_class_path_locations_ = runtime_options.ReleaseOrDefault(Opt::BootClassPathLocations);
  DCHECK(boot_class_path_locations_.empty() ||
         boot_class_path_locations_.size() == boot_class_path_.size());
  if (boot_class_path_.empty()) {
    LOG(ERROR) << "Boot classpath is empty";
    return false;
  }
  boot_class_path_fds_ = runtime_options.ReleaseOrDefault(Opt::BootClassPathFds);
  if (!boot_class_path_fds_.empty() && boot_class_path_fds_.size() != boot_class_path_.size()) {
    LOG(ERROR) << "Number of FDs specified in -Xbootclasspathfds must match the number of JARs in "
               << "-Xbootclasspath.";
    return false;
  }
  boot_class_path_image_fds_ = runtime_options.ReleaseOrDefault(Opt::BootClassPathImageFds);
  boot_class_path_vdex_fds_ = runtime_options.ReleaseOrDefault(Opt::BootClassPathVdexFds);
  boot_class_path_oat_fds_ = runtime_options.ReleaseOrDefault(Opt::BootClassPathOatFds);
  CHECK(boot_class_path_image_fds_.empty() ||
        boot_class_path_image_fds_.size() == boot_class_path_.size());
  CHECK(boot_class_path_vdex_fds_.empty() ||
        boot_class_path_vdex_fds_.size() == boot_class_path_.size());
  CHECK(boot_class_path_oat_fds_.empty() ||
        boot_class_path_oat_fds_.size() == boot_class_path_.size());
  class_path_string_ = runtime_options.ReleaseOrDefault(Opt::ClassPath);
  properties_ = runtime_options.ReleaseOrDefault(Opt::PropertiesList);
  compiler_callbacks_ = runtime_options.GetOrDefault(Opt::CompilerCallbacksPtr);
  must_relocate_ = runtime_options.GetOrDefault(Opt::Relocate);
  is_zygote_ = runtime_options.Exists(Opt::Zygote);
  is_primary_zygote_ = runtime_options.Exists(Opt::PrimaryZygote);
  is_explicit_gc_disabled_ = runtime_options.Exists(Opt::DisableExplicitGC);
  image_dex2oat_enabled_ = runtime_options.GetOrDefault(Opt::ImageDex2Oat);
  dump_native_stack_on_sig_quit_ = runtime_options.GetOrDefault(Opt::DumpNativeStackOnSigQuit);
  allow_in_memory_compilation_ = runtime_options.Exists(Opt::AllowInMemoryCompilation);
  if (is_zygote_ || runtime_options.Exists(Opt::OnlyUseTrustedOatFiles)) {
    oat_file_manager_->SetOnlyUseTrustedOatFiles();
  }
  vfprintf_ = runtime_options.GetOrDefault(Opt::HookVfprintf);
  exit_ = runtime_options.GetOrDefault(Opt::HookExit);
  abort_ = runtime_options.GetOrDefault(Opt::HookAbort);
  default_stack_size_ = runtime_options.GetOrDefault(Opt::StackSize);
  compiler_executable_ = runtime_options.ReleaseOrDefault(Opt::Compiler);
  compiler_options_ = runtime_options.ReleaseOrDefault(Opt::CompilerOptions);
  for (const std::string& option : Runtime::Current()->GetCompilerOptions()) {
    if (option == "--debuggable") {
      SetRuntimeDebugState(RuntimeDebugState::kJavaDebuggableAtInit);
      break;
    }
  }
  image_compiler_options_ = runtime_options.ReleaseOrDefault(Opt::ImageCompilerOptions);
  finalizer_timeout_ms_ = runtime_options.GetOrDefault(Opt::FinalizerTimeoutMs);
  max_spins_before_thin_lock_inflation_ =
      runtime_options.GetOrDefault(Opt::MaxSpinsBeforeThinLockInflation);
  monitor_list_ = new MonitorList;
  monitor_pool_ = MonitorPool::Create();
  thread_list_ = new ThreadList(runtime_options.GetOrDefault(Opt::ThreadSuspendTimeout));
  intern_table_ = new InternTable;
  monitor_timeout_enable_ = runtime_options.GetOrDefault(Opt::MonitorTimeoutEnable);
  int monitor_timeout_ms = runtime_options.GetOrDefault(Opt::MonitorTimeout);
  if (monitor_timeout_ms < Monitor::kMonitorTimeoutMinMs) {
    LOG(WARNING) << "Monitor timeout too short: Increasing";
    monitor_timeout_ms = Monitor::kMonitorTimeoutMinMs;
  }
  if (monitor_timeout_ms >= Monitor::kMonitorTimeoutMaxMs) {
    LOG(WARNING) << "Monitor timeout too long: Decreasing";
    monitor_timeout_ms = Monitor::kMonitorTimeoutMaxMs - 1;
  }
  monitor_timeout_ns_ = MsToNs(monitor_timeout_ms);
  verify_ = runtime_options.GetOrDefault(Opt::Verify);
  target_sdk_version_ = runtime_options.GetOrDefault(Opt::TargetSdkVersion);
  hidden_api_policy_ = runtime_options.GetOrDefault(Opt::HiddenApiPolicy);
  DCHECK_IMPLIES(is_zygote_, hidden_api_policy_ == hiddenapi::EnforcementPolicy::kDisabled);
  core_platform_api_policy_ = runtime_options.GetOrDefault(Opt::CorePlatformApiPolicy);
  if (core_platform_api_policy_ != hiddenapi::EnforcementPolicy::kDisabled) {
    LOG(INFO) << "Core platform API reporting enabled, enforcing="
              << (core_platform_api_policy_ == hiddenapi::EnforcementPolicy::kEnabled ? "true" :
                                                                                        "false");
  }
  no_sig_chain_ = runtime_options.Exists(Opt::NoSigChain);
  force_native_bridge_ = runtime_options.Exists(Opt::ForceNativeBridge);
  Split(runtime_options.GetOrDefault(Opt::CpuAbiList), ',', &cpu_abilist_);
  fingerprint_ = runtime_options.ReleaseOrDefault(Opt::Fingerprint);
  if (runtime_options.GetOrDefault(Opt::Interpret)) {
    GetInstrumentation()->ForceInterpretOnly();
  }
  zygote_max_failed_boots_ = runtime_options.GetOrDefault(Opt::ZygoteMaxFailedBoots);
  experimental_flags_ = runtime_options.GetOrDefault(Opt::Experimental);
  is_low_memory_mode_ = runtime_options.Exists(Opt::LowMemoryMode);
  madvise_random_access_ = runtime_options.GetOrDefault(Opt::MadviseRandomAccess);
  madvise_willneed_vdex_filesize_ = runtime_options.GetOrDefault(Opt::MadviseWillNeedVdexFileSize);
  madvise_willneed_odex_filesize_ = runtime_options.GetOrDefault(Opt::MadviseWillNeedOdexFileSize);
  madvise_willneed_art_filesize_ = runtime_options.GetOrDefault(Opt::MadviseWillNeedArtFileSize);
  jni_ids_indirection_ = runtime_options.GetOrDefault(Opt::OpaqueJniIds);
  automatically_set_jni_ids_indirection_ =
      runtime_options.GetOrDefault(Opt::AutoPromoteOpaqueJniIds);
  plugins_ = runtime_options.ReleaseOrDefault(Opt::Plugins);
  agent_specs_ = runtime_options.ReleaseOrDefault(Opt::AgentPath);
  float foreground_heap_growth_multiplier;
  if (is_low_memory_mode_ && !runtime_options.Exists(Opt::ForegroundHeapGrowthMultiplier)) {
    foreground_heap_growth_multiplier = 1.0f;
  } else {
    foreground_heap_growth_multiplier =
        runtime_options.GetOrDefault(Opt::ForegroundHeapGrowthMultiplier) + 1.0f;
  }
  XGcOption xgc_option = runtime_options.GetOrDefault(Opt::GcOption);
  bool use_generational_cc = kUseBakerReadBarrier && xgc_option.generational_cc;
  InitializeApexVersions();
  BackgroundGcOption background_gc =
      gUseReadBarrier ? BackgroundGcOption(gc::kCollectorTypeCCBackground) :
                        (gUseUserfaultfd ? BackgroundGcOption(xgc_option.collector_type_) :
                                           runtime_options.GetOrDefault(Opt::BackgroundGc));
  heap_ = new gc::Heap(runtime_options.GetOrDefault(Opt::MemoryInitialSize),
                       runtime_options.GetOrDefault(Opt::HeapGrowthLimit),
                       runtime_options.GetOrDefault(Opt::HeapMinFree),
                       runtime_options.GetOrDefault(Opt::HeapMaxFree),
                       runtime_options.GetOrDefault(Opt::HeapTargetUtilization),
                       foreground_heap_growth_multiplier,
                       runtime_options.GetOrDefault(Opt::StopForNativeAllocs),
                       runtime_options.GetOrDefault(Opt::MemoryMaximumSize),
                       runtime_options.GetOrDefault(Opt::NonMovingSpaceCapacity),
                       GetBootClassPath(),
                       GetBootClassPathLocations(),
                       GetBootClassPathFds(),
                       GetBootClassPathImageFds(),
                       GetBootClassPathVdexFds(),
                       GetBootClassPathOatFds(),
                       image_locations_,
                       instruction_set_,
                       gUseReadBarrier ? gc::kCollectorTypeCC : xgc_option.collector_type_,
                       background_gc,
                       runtime_options.GetOrDefault(Opt::LargeObjectSpace),
                       runtime_options.GetOrDefault(Opt::LargeObjectThreshold),
                       runtime_options.GetOrDefault(Opt::ParallelGCThreads),
                       runtime_options.GetOrDefault(Opt::ConcGCThreads),
                       runtime_options.Exists(Opt::LowMemoryMode),
                       runtime_options.GetOrDefault(Opt::LongPauseLogThreshold),
                       runtime_options.GetOrDefault(Opt::LongGCLogThreshold),
                       runtime_options.Exists(Opt::IgnoreMaxFootprint),
                       runtime_options.GetOrDefault(Opt::AlwaysLogExplicitGcs),
                       runtime_options.GetOrDefault(Opt::UseTLAB),
                       xgc_option.verify_pre_gc_heap_,
                       xgc_option.verify_pre_sweeping_heap_,
                       xgc_option.verify_post_gc_heap_,
                       xgc_option.verify_pre_gc_rosalloc_,
                       xgc_option.verify_pre_sweeping_rosalloc_,
                       xgc_option.verify_post_gc_rosalloc_,
                       xgc_option.gcstress_,
                       xgc_option.measure_,
                       runtime_options.GetOrDefault(Opt::EnableHSpaceCompactForOOM),
                       use_generational_cc,
                       runtime_options.GetOrDefault(Opt::HSpaceCompactForOOMMinIntervalsMs),
                       runtime_options.Exists(Opt::DumpRegionInfoBeforeGC),
                       runtime_options.Exists(Opt::DumpRegionInfoAfterGC));
  dump_gc_performance_on_shutdown_ = runtime_options.Exists(Opt::DumpGCPerformanceOnShutdown);
  bool has_explicit_jdwp_options = runtime_options.Get(Opt::JdwpOptions) != nullptr;
  jdwp_options_ = runtime_options.GetOrDefault(Opt::JdwpOptions);
  jdwp_provider_ =
      CanonicalizeJdwpProvider(runtime_options.GetOrDefault(Opt::JdwpProvider), IsJavaDebuggable());
  switch (jdwp_provider_) {
    case JdwpProvider::kNone: {
      VLOG(jdwp) << "Disabling all JDWP support.";
      if (!jdwp_options_.empty()) {
        bool has_transport = jdwp_options_.find("transport") != std::string::npos;
        std::string adb_connection_args =
            std::string("  -XjdwpProvider:adbconnection -XjdwpOptions:") + jdwp_options_;
        if (has_explicit_jdwp_options) {
          LOG(WARNING) << "Jdwp options given when jdwp is disabled! You probably want to enable "
                       << "jdwp with one of:" << std::endl
                       << "  -Xplugin:libopenjdkjvmti" << (kIsDebugBuild ? "d" : "") << ".so "
                       << "-agentpath:libjdwp.so=" << jdwp_options_ << std::endl
                       << (has_transport ? "" : adb_connection_args);
        }
      }
      break;
    }
    case JdwpProvider::kAdbConnection: {
      constexpr const char* plugin_name =
          kIsDebugBuild ? "libadbconnectiond.so" : "libadbconnection.so";
      plugins_.push_back(Plugin::Create(plugin_name));
      break;
    }
    case JdwpProvider::kUnset: {
      LOG(FATAL) << "Illegal jdwp provider " << jdwp_provider_ << " was not filtered out!";
    }
  }
  callbacks_->AddThreadLifecycleCallback(Dbg::GetThreadLifecycleCallback());
  jit_options_.reset(jit::JitOptions::CreateFromRuntimeArguments(runtime_options));
  if (IsAotCompiler()) {
    jit_options_->SetUseJitCompilation(false);
    jit_options_->SetSaveProfilingInfo(false);
  }
  const bool use_malloc = IsAotCompiler();
  if (use_malloc) {
    arena_pool_.reset(new MallocArenaPool());
    jit_arena_pool_.reset(new MallocArenaPool());
  } else {
    arena_pool_.reset(new MemMapArenaPool( false));
    jit_arena_pool_.reset(new MemMapArenaPool( false, "CompilerMetadata"));
  }
  const bool low_4gb = IsAotCompiler() && Is64BitInstructionSet(kRuntimeISA);
  if (gUseUserfaultfd) {
    linear_alloc_arena_pool_.reset(new GcVisitedArenaPool(low_4gb, IsZygote()));
  } else if (low_4gb) {
    linear_alloc_arena_pool_.reset(new MemMapArenaPool(low_4gb));
  }
  linear_alloc_.reset(CreateLinearAlloc());
  startup_linear_alloc_.reset(CreateLinearAlloc());
  small_lrt_allocator_ = new jni::SmallLrtAllocator();
  BlockSignals();
  InitPlatformSignalHandlers();
  switch (kRuntimeISA) {
    case InstructionSet::kArm64:
      implicit_suspend_checks_ = true;
      FALLTHROUGH_INTENDED;
    case InstructionSet::kArm:
    case InstructionSet::kThumb2:
    case InstructionSet::kX86:
    case InstructionSet::kX86_64:
      implicit_null_checks_ = true;
      implicit_so_checks_ = true;
      break;
    default:
      break;
  }
  fault_manager.Init(!no_sig_chain_);
  if (!no_sig_chain_) {
    if (HandlesSignalsInCompiledCode()) {
      if (implicit_suspend_checks_) {
        new SuspensionHandler(&fault_manager);
      }
      if (implicit_so_checks_) {
        new StackOverflowHandler(&fault_manager);
      }
      if (implicit_null_checks_) {
        new NullPointerHandler(&fault_manager);
      }
      if (kEnableJavaStackTraceHandler) {
        new JavaStackTraceHandler(&fault_manager);
      }
      if (interpreter::CanRuntimeUseNterp()) {
        OatQuickMethodHeader* nterp_header = OatQuickMethodHeader::NterpMethodHeader;
        fault_manager.AddGeneratedCodeRange(nterp_header->GetCode(), nterp_header->GetCodeSize());
      }
    }
  }
  verifier_logging_threshold_ms_ = runtime_options.GetOrDefault(Opt::VerifierLoggingThreshold);
  std::string error_msg;
  java_vm_ = JavaVMExt::Create(this, runtime_options, &error_msg);
  if (java_vm_.get() == nullptr) {
    LOG(ERROR) << "Could not initialize JavaVMExt: " << error_msg;
    return false;
  }
  java_vm_->AddEnvironmentHook(JNIEnvExt::GetEnvHandler);
  Thread::Startup();
  Thread* self = Thread::Attach("main", false, nullptr, false, true);
  CHECK_EQ(self->GetThreadId(), ThreadList::kMainThreadId);
  CHECK(self != nullptr);
  self->SetIsRuntimeThread(IsAotCompiler());
  self->TransitionFromSuspendedToRunnable();
  GetHeap()->EnableObjectValidation();
  CHECK_GE(GetHeap()->GetContinuousSpaces().size(), 1U);
  if (UNLIKELY(IsAotCompiler())) {
    class_linker_ = new AotClassLinker(intern_table_);
  } else {
    class_linker_ = new ClassLinker(intern_table_,
                                    runtime_options.GetOrDefault(Opt::FastClassNotFoundException));
  }
  if (GetHeap()->HasBootImageSpace()) {
    bool result = class_linker_->InitFromBootImage(&error_msg);
    if (!result) {
      LOG(ERROR) << "Could not initialize from image: " << error_msg;
      return false;
    }
    if (kIsDebugBuild) {
      for (auto image_space : GetHeap()->GetBootImageSpaces()) {
        image_space->VerifyImageAllocations();
      }
    }
    {
      ScopedTrace trace2("AddImageStringsToTable");
      for (gc::space::ImageSpace* image_space : heap_->GetBootImageSpaces()) {
        GetInternTable()->AddImageStringsToTable(image_space, VoidFunctor());
      }
    }
    const size_t total_components = gc::space::ImageSpace::GetNumberOfComponents(
        ArrayRef<gc::space::ImageSpace* const>(heap_->GetBootImageSpaces()));
    if (total_components != GetBootClassPath().size()) {
      CHECK_LT(total_components, GetBootClassPath().size());
      size_t start = total_components;
      DCHECK_LT(start, GetBootClassPath().size());
      std::vector<std::unique_ptr<const DexFile>> extra_boot_class_path;
      if (runtime_options.Exists(Opt::BootClassPathDexList)) {
        extra_boot_class_path.swap(*runtime_options.GetOrDefault(Opt::BootClassPathDexList));
      } else {
        ArrayRef<const int> bcp_fds =
            start < GetBootClassPathFds().size() ?
                ArrayRef<const int>(GetBootClassPathFds()).SubArray(start) :
                ArrayRef<const int>();
        OpenBootDexFiles(ArrayRef<const std::string>(GetBootClassPath()).SubArray(start),
                         ArrayRef<const std::string>(GetBootClassPathLocations()).SubArray(start),
                         bcp_fds,
                         &extra_boot_class_path);
      }
      class_linker_->AddExtraBootDexFiles(self, std::move(extra_boot_class_path));
    }
    if (IsJavaDebuggable() || jit_options_->GetProfileSaverOptions().GetProfileBootClassPath()) {
      ScopedThreadSuspension sts(self, ThreadState::kNative);
      ScopedSuspendAll ssa(__FUNCTION__);
      DeoptimizeBootImage();
    }
  } else {
    std::vector<std::unique_ptr<const DexFile>> boot_class_path;
    if (runtime_options.Exists(Opt::BootClassPathDexList)) {
      boot_class_path.swap(*runtime_options.GetOrDefault(Opt::BootClassPathDexList));
    } else {
      OpenBootDexFiles(ArrayRef<const std::string>(GetBootClassPath()),
                       ArrayRef<const std::string>(GetBootClassPathLocations()),
                       ArrayRef<const int>(GetBootClassPathFds()),
                       &boot_class_path);
    }
    if (!class_linker_->InitWithoutImage(std::move(boot_class_path), &error_msg)) {
      LOG(ERROR) << "Could not initialize without image: " << error_msg;
      return false;
    }
    SetInstructionSet(instruction_set_);
    for (uint32_t i = 0; i < kCalleeSaveSize; i++) {
      CalleeSaveType type = CalleeSaveType(i);
      if (!HasCalleeSaveMethod(type)) {
        SetCalleeSaveMethod(CreateCalleeSaveMethod(), type);
      }
    }
  }
  ArrayRef<gc::space::ImageSpace* const> image_spaces(GetHeap()->GetBootImageSpaces());
  ArrayRef<const DexFile* const> bcp_dex_files(GetClassLinker()->GetBootClassPath());
  boot_class_path_checksums_ =
      gc::space::ImageSpace::GetBootClassPathChecksums(image_spaces, bcp_dex_files);
  CHECK(class_linker_ != nullptr);
  verifier::ClassVerifier::Init(class_linker_);
  if (runtime_options.Exists(Opt::MethodTrace)) {
    trace_config_.reset(new TraceConfig());
    trace_config_->trace_file = runtime_options.ReleaseOrDefault(Opt::MethodTraceFile);
    trace_config_->trace_file_size = runtime_options.ReleaseOrDefault(Opt::MethodTraceFileSize);
    trace_config_->trace_mode = Trace::TraceMode::kMethodTracing;
    trace_config_->trace_output_mode = runtime_options.Exists(Opt::MethodTraceStreaming) ?
                                           Trace::TraceOutputMode::kStreaming :
                                           Trace::TraceOutputMode::kFile;
    trace_config_->clock_source = runtime_options.GetOrDefault(Opt::MethodTraceClock);
  }
  Trace::SetDefaultClockSource(runtime_options.GetOrDefault(Opt::ProfileClock));
  if (GetHeap()->HasBootImageSpace()) {
    const ImageHeader& image_header = GetHeap()->GetBootImageSpaces()[0]->GetImageHeader();
    ObjPtr<mirror::ObjectArray<mirror::Object>> boot_image_live_objects =
        ObjPtr<mirror::ObjectArray<mirror::Object>>::DownCast(
            image_header.GetImageRoot(ImageHeader::kBootImageLiveObjects));
    pre_allocated_OutOfMemoryError_when_throwing_exception_ = GcRoot<mirror::Throwable>(
        boot_image_live_objects->Get(ImageHeader::kOomeWhenThrowingException)->AsThrowable());
    DCHECK(pre_allocated_OutOfMemoryError_when_throwing_exception_.Read()
               ->GetClass()
               ->DescriptorEquals("Ljava/lang/OutOfMemoryError;"));
    pre_allocated_OutOfMemoryError_when_throwing_oome_ = GcRoot<mirror::Throwable>(
        boot_image_live_objects->Get(ImageHeader::kOomeWhenThrowingOome)->AsThrowable());
    DCHECK(pre_allocated_OutOfMemoryError_when_throwing_oome_.Read()->GetClass()->DescriptorEquals(
        "Ljava/lang/OutOfMemoryError;"));
    pre_allocated_OutOfMemoryError_when_handling_stack_overflow_ = GcRoot<mirror::Throwable>(
        boot_image_live_objects->Get(ImageHeader::kOomeWhenHandlingStackOverflow)->AsThrowable());
    DCHECK(pre_allocated_OutOfMemoryError_when_handling_stack_overflow_.Read()
               ->GetClass()
               ->DescriptorEquals("Ljava/lang/OutOfMemoryError;"));
    pre_allocated_NoClassDefFoundError_ = GcRoot<mirror::Throwable>(
        boot_image_live_objects->Get(ImageHeader::kNoClassDefFoundError)->AsThrowable());
    DCHECK(pre_allocated_NoClassDefFoundError_.Read()->GetClass()->DescriptorEquals(
        "Ljava/lang/NoClassDefFoundError;"));
  } else {
    CreatePreAllocatedException(self,
                                this,
                                &pre_allocated_OutOfMemoryError_when_throwing_exception_,
                                "Ljava/lang/OutOfMemoryError;",
                                "OutOfMemoryError thrown while trying to throw an exception; "
                                "no stack trace available");
    CreatePreAllocatedException(self,
                                this,
                                &pre_allocated_OutOfMemoryError_when_throwing_oome_,
                                "Ljava/lang/OutOfMemoryError;",
                                "OutOfMemoryError thrown while trying to throw OutOfMemoryError; "
                                "no stack trace available");
    CreatePreAllocatedException(self,
                                this,
                                &pre_allocated_OutOfMemoryError_when_handling_stack_overflow_,
                                "Ljava/lang/OutOfMemoryError;",
                                "OutOfMemoryError thrown while trying to handle a stack overflow; "
                                "no stack trace available");
    CreatePreAllocatedException(self,
                                this,
                                &pre_allocated_NoClassDefFoundError_,
                                "Ljava/lang/NoClassDefFoundError;",
                                "Class not found using the boot class loader; "
                                "no stack trace available");
  }
  GetJniIdManager()->Init(self);
  InitMetrics();
  {
    ScopedThreadSuspension sts(self, ThreadState::kNative);
    for (auto& plugin : plugins_) {
      std::string err;
      if (!plugin.Load(&err)) {
        LOG(FATAL) << plugin << " failed to load: " << err;
      }
    }
  }
  {
    std::string native_bridge_file_name = runtime_options.ReleaseOrDefault(Opt::NativeBridge);
    is_native_bridge_loaded_ = LoadNativeBridge(native_bridge_file_name);
  }
  for (auto& agent_spec : agent_specs_) {
    int res = 0;
    std::string err = "";
    ti::LoadError error;
    std::unique_ptr<ti::Agent> agent = agent_spec.Load(&res, &error, &err);
    if (agent != nullptr) {
      agents_.push_back(std::move(agent));
      continue;
    }
    switch (error) {
      case ti::LoadError::kInitializationError:
        LOG(FATAL) << "Unable to initialize agent!";
        UNREACHABLE();
      case ti::LoadError::kLoadingError:
        LOG(ERROR) << "Unable to load an agent: " << err;
        continue;
      case ti::LoadError::kNoError:
        break;
    }
    LOG(FATAL) << "Unreachable";
    UNREACHABLE();
  }
  {
    ScopedObjectAccess soa(self);
    callbacks_->NextRuntimePhase(RuntimePhaseCallback::RuntimePhase::kInitialAgents);
  }
  if (IsZygote() && IsPerfettoHprofEnabled()) {
    constexpr const char* plugin_name =
        kIsDebugBuild ? "libperfetto_hprofd.so" : "libperfetto_hprof.so";
    dlopen(plugin_name, RTLD_NOW | RTLD_LOCAL);
  }
  VLOG(startup) << "Runtime::Init exiting";
  return true;
}
void Runtime::InitMetrics() {
  metrics::ReportingConfig metrics_config = metrics::ReportingConfig::FromFlags();
  metrics_reporter_ = metrics::MetricsReporter::Create(metrics_config, this);
}
void Runtime::RequestMetricsReport(bool synchronous) {
  if (metrics_reporter_) {
    metrics_reporter_->RequestMetricsReport(synchronous);
  }
}
bool Runtime::EnsurePluginLoaded(const char* plugin_name, std::string* error_msg) {
  for (const Plugin& p : plugins_) {
    if (p.GetLibrary() == plugin_name) {
      return true;
    }
  }
  Plugin new_plugin = Plugin::Create(plugin_name);
  if (!new_plugin.Load(error_msg)) {
    return false;
  }
  plugins_.push_back(std::move(new_plugin));
  return true;
}
bool Runtime::EnsurePerfettoPlugin(std::string* error_msg) {
  constexpr const char* plugin_name =
      kIsDebugBuild ? "libperfetto_hprofd.so" : "libperfetto_hprof.so";
  return EnsurePluginLoaded(plugin_name, error_msg);
}
static bool EnsureJvmtiPlugin(Runtime* runtime, std::string* error_msg) {
  DCHECK(Dbg::IsJdwpAllowed() || !runtime->IsJavaDebuggable())
      << "Being debuggable requires that jdwp (i.e. debugging) is allowed.";
  if (!Dbg::IsJdwpAllowed()) {
    *error_msg = "Process is not allowed to load openjdkjvmti plugin. Process must be debuggable";
    return false;
  }
  constexpr const char* plugin_name = kIsDebugBuild ? "libopenjdkjvmtid.so" : "libopenjdkjvmti.so";
  return runtime->EnsurePluginLoaded(plugin_name, error_msg);
}
void Runtime::AttachAgent(JNIEnv* env, const std::string& agent_arg, jobject class_loader) {
  std::string error_msg;
  if (!EnsureJvmtiPlugin(this, &error_msg)) {
    LOG(WARNING) << "Could not load plugin: " << error_msg;
    ScopedObjectAccess soa(Thread::Current());
    ThrowIOException("%s", error_msg.c_str());
    return;
  }
  ti::AgentSpec agent_spec(agent_arg);
  int res = 0;
  ti::LoadError error;
  std::unique_ptr<ti::Agent> agent = agent_spec.Attach(env, class_loader, &res, &error, &error_msg);
  if (agent != nullptr) {
    agents_.push_back(std::move(agent));
  } else {
    LOG(WARNING) << "Agent attach failed (result=" << error << ") : " << error_msg;
    ScopedObjectAccess soa(Thread::Current());
    ThrowIOException("%s", error_msg.c_str());
  }
}
void Runtime::InitNativeMethods() {
  VLOG(startup) << "Runtime::InitNativeMethods entering";
  Thread* self = Thread::Current();
  JNIEnv* env = self->GetJniEnv();
  CHECK_EQ(self->GetState(), ThreadState::kNative);
  jclass java_lang_Object;
  {
    ScopedObjectAccess soa(self);
    java_lang_Object = reinterpret_cast<jclass>(
        GetJavaVM()->AddGlobalRef(self, GetClassRoot<mirror::Object>(GetClassLinker())));
  }
  {
    std::string error_msg;
    if (!java_vm_->LoadNativeLibrary(env, "libicu_jni.so", nullptr, java_lang_Object, &error_msg)) {
      LOG(FATAL) << "LoadNativeLibrary failed for \"libicu_jni.so\": " << error_msg;
    }
  }
  {
    std::string error_msg;
    if (!java_vm_->LoadNativeLibrary(
            env, "libjavacore.so", nullptr, java_lang_Object, &error_msg)) {
      LOG(FATAL) << "LoadNativeLibrary failed for \"libjavacore.so\": " << error_msg;
    }
  }
  {
    constexpr const char* kOpenJdkLibrary = kIsDebugBuild ? "libopenjdkd.so" : "libopenjdk.so";
    std::string error_msg;
    if (!java_vm_->LoadNativeLibrary(env, kOpenJdkLibrary, nullptr, java_lang_Object, &error_msg)) {
      LOG(FATAL) << "LoadNativeLibrary failed for \"" << kOpenJdkLibrary << "\": " << error_msg;
    }
  }
  env->DeleteGlobalRef(java_lang_Object);
  WellKnownClasses::LateInit(env);
  VLOG(startup) << "Runtime::InitNativeMethods exiting";
}
void Runtime::ReclaimArenaPoolMemory() { arena_pool_->LockReclaimMemory(); }
void Runtime::InitThreadGroups(Thread* self) {
  ScopedObjectAccess soa(self);
  ArtField* main_thread_group_field = WellKnownClasses::java_lang_ThreadGroup_mainThreadGroup;
  ArtField* system_thread_group_field = WellKnownClasses::java_lang_ThreadGroup_systemThreadGroup;
  StackHandleScope<2u> hs(self);
  Handle<mirror::Class> thread_group_class =
      hs.NewHandle(main_thread_group_field->GetDeclaringClass());
  bool initialized = GetClassLinker()->EnsureInitialized(
      self, thread_group_class, true, true);
  CHECK(initialized);
  Handle<mirror::Class> thread_class = hs.NewHandle(WellKnownClasses::java_lang_Thread.Get());
  initialized = GetClassLinker()->EnsureInitialized(
      self, thread_class, true, true);
  CHECK(initialized);
  main_thread_group_ =
      soa.Vm()->AddGlobalRef(self, main_thread_group_field->GetObject(thread_group_class.Get()));
  CHECK_IMPLIES(main_thread_group_ == nullptr, IsAotCompiler());
  system_thread_group_ =
      soa.Vm()->AddGlobalRef(self, system_thread_group_field->GetObject(thread_group_class.Get()));
  CHECK_IMPLIES(system_thread_group_ == nullptr, IsAotCompiler());
}
jobject Runtime::GetMainThreadGroup() const {
  CHECK_IMPLIES(main_thread_group_ == nullptr, IsAotCompiler());
  return main_thread_group_;
}
jobject Runtime::GetSystemThreadGroup() const {
  CHECK_IMPLIES(system_thread_group_ == nullptr, IsAotCompiler());
  return system_thread_group_;
}
jobject Runtime::GetSystemClassLoader() const {
  CHECK_IMPLIES(system_class_loader_ == nullptr, IsAotCompiler());
  return system_class_loader_;
}
void Runtime::RegisterRuntimeNativeMethods(JNIEnv* env) {
  register_dalvik_system_DexFile(env);
  register_dalvik_system_BaseDexClassLoader(env);
  register_dalvik_system_VMDebug(env);
  register_dalvik_system_VMRuntime(env);
  register_dalvik_system_VMStack(env);
  register_dalvik_system_ZygoteHooks(env);
  register_java_lang_Class(env);
  register_java_lang_Object(env);
  register_java_lang_invoke_MethodHandle(env);
  register_java_lang_invoke_MethodHandleImpl(env);
  register_java_lang_ref_FinalizerReference(env);
  register_java_lang_reflect_Array(env);
  register_java_lang_reflect_Constructor(env);
  register_java_lang_reflect_Executable(env);
  register_java_lang_reflect_Field(env);
  register_java_lang_reflect_Method(env);
  register_java_lang_reflect_Parameter(env);
  register_java_lang_reflect_Proxy(env);
  register_java_lang_ref_Reference(env);
  register_java_lang_StackStreamFactory(env);
  register_java_lang_String(env);
  register_java_lang_StringFactory(env);
  register_java_lang_System(env);
  register_java_lang_Thread(env);
  register_java_lang_Throwable(env);
  register_java_lang_VMClassLoader(env);
  register_java_util_concurrent_atomic_AtomicLong(env);
  register_jdk_internal_misc_Unsafe(env);
  register_libcore_io_Memory(env);
  register_libcore_util_CharsetUtils(env);
  register_org_apache_harmony_dalvik_ddmc_DdmServer(env);
  register_org_apache_harmony_dalvik_ddmc_DdmVmInternal(env);
  register_sun_misc_Unsafe(env);
}
std::ostream& operator<<(std::ostream& os, const DeoptimizationKind& kind) {
  os << GetDeoptimizationKindName(kind);
  return os;
}
void Runtime::DumpDeoptimizations(std::ostream& os) {
  for (size_t i = 0; i <= static_cast<size_t>(DeoptimizationKind::kLast); ++i) {
    if (deoptimization_counts_[i] != 0) {
      os << "Number of " << GetDeoptimizationKindName(static_cast<DeoptimizationKind>(i))
         << " deoptimizations: " << deoptimization_counts_[i] << "\n";
    }
  }
}
void Runtime::DumpForSigQuit(std::ostream& os) {
  thread_list_->DumpForSigQuit(os);
  GetClassLinker()->DumpForSigQuit(os);
  GetInternTable()->DumpForSigQuit(os);
  GetJavaVM()->DumpForSigQuit(os);
  GetHeap()->DumpForSigQuit(os);
  oat_file_manager_->DumpForSigQuit(os);
  if (GetJit() != nullptr) {
    GetJit()->DumpForSigQuit(os);
  } else {
    os << "Running non JIT\n";
  }
  DumpDeoptimizations(os);
  TrackedAllocators::Dump(os);
  GetMetrics()->DumpForSigQuit(os);
  os << "\n";
  BaseMutex::DumpAll(os);
  {
    ScopedObjectAccess soa(Thread::Current());
    callbacks_->SigQuit();
  }
}
void Runtime::DumpLockHolders(std::ostream& os) {
  pid_t mutator_lock_owner = Locks::mutator_lock_->GetExclusiveOwnerTid();
  pid_t thread_list_lock_owner = GetThreadList()->GetLockOwner();
  pid_t classes_lock_owner = GetClassLinker()->GetClassesLockOwner();
  pid_t dex_lock_owner = GetClassLinker()->GetDexLockOwner();
  if ((mutator_lock_owner | thread_list_lock_owner | classes_lock_owner | dex_lock_owner) != 0) {
    os << "Mutator lock exclusive owner tid: " << mutator_lock_owner << "\n"
       << "ThreadList lock owner tid: " << thread_list_lock_owner << "\n"
       << "ClassLinker classes lock owner tid: " << classes_lock_owner << "\n"
       << "ClassLinker dex lock owner tid: " << dex_lock_owner << "\n";
  }
}
void Runtime::SetStatsEnabled(bool new_state) {
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::instrument_entrypoints_lock_);
  if (new_state == true) {
    GetStats()->Clear(~0);
    self->GetStats()->Clear(~0);
    if (stats_enabled_ != new_state) {
      GetInstrumentation()->InstrumentQuickAllocEntryPointsLocked();
    }
  } else if (stats_enabled_ != new_state) {
    GetInstrumentation()->UninstrumentQuickAllocEntryPointsLocked();
  }
  stats_enabled_ = new_state;
}
void Runtime::ResetStats(int kinds) {
  GetStats()->Clear(kinds & 0xffff);
  Thread::Current()->GetStats()->Clear(kinds >> 16);
}
uint64_t Runtime::GetStat(int kind) {
  RuntimeStats* stats;
  if (kind < (1 << 16)) {
    stats = GetStats();
  } else {
    stats = Thread::Current()->GetStats();
    kind >>= 16;
  }
  switch (kind) {
    case KIND_ALLOCATED_OBJECTS:
      return stats->allocated_objects;
    case KIND_ALLOCATED_BYTES:
      return stats->allocated_bytes;
    case KIND_FREED_OBJECTS:
      return stats->freed_objects;
    case KIND_FREED_BYTES:
      return stats->freed_bytes;
    case KIND_GC_INVOCATIONS:
      return stats->gc_for_alloc_count;
    case KIND_CLASS_INIT_COUNT:
      return stats->class_init_count;
    case KIND_CLASS_INIT_TIME:
      return stats->class_init_time_ns;
    case KIND_EXT_ALLOCATED_OBJECTS:
    case KIND_EXT_ALLOCATED_BYTES:
    case KIND_EXT_FREED_OBJECTS:
    case KIND_EXT_FREED_BYTES:
      return 0;
    default:
      LOG(FATAL) << "Unknown statistic " << kind;
      UNREACHABLE();
  }
}
void Runtime::BlockSignals() {
  SignalSet signals;
  signals.Add(SIGPIPE);
  signals.Add(SIGQUIT);
  signals.Add(SIGUSR1);
  signals.Block();
}
bool Runtime::AttachCurrentThread(const char* thread_name,
                                  bool as_daemon,
                                  jobject thread_group,
                                  bool create_peer,
                                  bool should_run_callbacks) {
  ScopedTrace trace(__FUNCTION__);
  Thread* self =
      Thread::Attach(thread_name, as_daemon, thread_group, create_peer, should_run_callbacks);
  if (self != nullptr && create_peer && !IsAotCompiler()) {
    ScopedObjectAccess soa(self);
    self->NotifyThreadGroup(soa, thread_group);
  }
  return self != nullptr;
}
void Runtime::DetachCurrentThread(bool should_run_callbacks) {
  ScopedTrace trace(__FUNCTION__);
  Thread* self = Thread::Current();
  if (self == nullptr) {
    LOG(FATAL) << "attempting to detach thread that is not attached";
  }
  if (self->HasManagedStack()) {
    LOG(FATAL) << *Thread::Current() << " attempting to detach while still running code";
  }
  thread_list_->Unregister(self, should_run_callbacks);
}
mirror::Throwable* Runtime::GetPreAllocatedOutOfMemoryErrorWhenThrowingException() {
  mirror::Throwable* oome = pre_allocated_OutOfMemoryError_when_throwing_exception_.Read();
  if (oome == nullptr) {
    LOG(ERROR) << "Failed to return pre-allocated OOME-when-throwing-exception";
  }
  return oome;
}
mirror::Throwable* Runtime::GetPreAllocatedOutOfMemoryErrorWhenThrowingOOME() {
  mirror::Throwable* oome = pre_allocated_OutOfMemoryError_when_throwing_oome_.Read();
  if (oome == nullptr) {
    LOG(ERROR) << "Failed to return pre-allocated OOME-when-throwing-OOME";
  }
  return oome;
}
mirror::Throwable* Runtime::GetPreAllocatedOutOfMemoryErrorWhenHandlingStackOverflow() {
  mirror::Throwable* oome = pre_allocated_OutOfMemoryError_when_handling_stack_overflow_.Read();
  if (oome == nullptr) {
    LOG(ERROR) << "Failed to return pre-allocated OOME-when-handling-stack-overflow";
  }
  return oome;
}
mirror::Throwable* Runtime::GetPreAllocatedNoClassDefFoundError() {
  mirror::Throwable* ncdfe = pre_allocated_NoClassDefFoundError_.Read();
  if (ncdfe == nullptr) {
    LOG(ERROR) << "Failed to return pre-allocated NoClassDefFoundError";
  }
  return ncdfe;
}
void Runtime::VisitConstantRoots(RootVisitor* visitor) {
  BufferedRootVisitor<16> buffered_visitor(visitor, RootInfo(kRootVMInternal));
  const PointerSize pointer_size = GetClassLinker()->GetImagePointerSize();
  if (HasResolutionMethod()) {
    resolution_method_->VisitRoots(buffered_visitor, pointer_size);
  }
  if (HasImtConflictMethod()) {
    imt_conflict_method_->VisitRoots(buffered_visitor, pointer_size);
  }
  if (imt_unimplemented_method_ != nullptr) {
    imt_unimplemented_method_->VisitRoots(buffered_visitor, pointer_size);
  }
  for (uint32_t i = 0; i < kCalleeSaveSize; ++i) {
    auto* m = reinterpret_cast<ArtMethod*>(callee_save_methods_[i]);
    if (m != nullptr) {
      m->VisitRoots(buffered_visitor, pointer_size);
    }
  }
}
void Runtime::VisitConcurrentRoots(RootVisitor* visitor, VisitRootFlags flags) {
  intern_table_->VisitRoots(visitor, flags);
  class_linker_->VisitRoots(visitor, flags);
  jni_id_manager_->VisitRoots(visitor);
  heap_->VisitAllocationRecords(visitor);
  if (jit_ != nullptr) {
    jit_->GetCodeCache()->VisitRoots(visitor);
  }
  if ((flags & kVisitRootFlagNewRoots) == 0) {
    VisitConstantRoots(visitor);
  }
}
void Runtime::VisitTransactionRoots(RootVisitor* visitor) {
  for (Transaction& transaction : preinitialization_transactions_) {
    transaction.VisitRoots(visitor);
  }
}
void Runtime::VisitNonThreadRoots(RootVisitor* visitor) {
  java_vm_->VisitRoots(visitor);
  sentinel_.VisitRootIfNonNull(visitor, RootInfo(kRootVMInternal));
  pre_allocated_OutOfMemoryError_when_throwing_exception_.VisitRootIfNonNull(
      visitor, RootInfo(kRootVMInternal));
  pre_allocated_OutOfMemoryError_when_throwing_oome_.VisitRootIfNonNull(visitor,
                                                                        RootInfo(kRootVMInternal));
  pre_allocated_OutOfMemoryError_when_handling_stack_overflow_.VisitRootIfNonNull(
      visitor, RootInfo(kRootVMInternal));
  pre_allocated_NoClassDefFoundError_.VisitRootIfNonNull(visitor, RootInfo(kRootVMInternal));
  VisitImageRoots(visitor);
  verifier::ClassVerifier::VisitStaticRoots(visitor);
  VisitTransactionRoots(visitor);
}
void Runtime::VisitNonConcurrentRoots(RootVisitor* visitor, VisitRootFlags flags) {
  VisitThreadRoots(visitor, flags);
  VisitNonThreadRoots(visitor);
}
void Runtime::VisitThreadRoots(RootVisitor* visitor, VisitRootFlags flags) {
  thread_list_->VisitRoots(visitor, flags);
}
void Runtime::VisitRoots(RootVisitor* visitor, VisitRootFlags flags) {
  VisitNonConcurrentRoots(visitor, flags);
  VisitConcurrentRoots(visitor, flags);
}
void Runtime::VisitReflectiveTargets(ReflectiveValueVisitor* visitor) {
  thread_list_->VisitReflectiveTargets(visitor);
  heap_->VisitReflectiveTargets(visitor);
  jni_id_manager_->VisitReflectiveTargets(visitor);
  callbacks_->VisitReflectiveTargets(visitor);
}
void Runtime::VisitImageRoots(RootVisitor* visitor) {
  if (kIsDebugBuild) {
    for (auto* space : GetHeap()->GetContinuousSpaces()) {
      if (space->IsImageSpace()) {
        auto* image_space = space->AsImageSpace();
        const auto& image_header = image_space->GetImageHeader();
        for (int32_t i = 0, size = image_header.GetImageRoots()->GetLength(); i != size; ++i) {
          mirror::Object* obj =
              image_header.GetImageRoot(static_cast<ImageHeader::ImageRoot>(i)).Ptr();
          if (obj != nullptr) {
            mirror::Object* after_obj = obj;
            visitor->VisitRoot(&after_obj, RootInfo(kRootStickyClass));
            CHECK_EQ(after_obj, obj);
          }
        }
      }
    }
  }
}
static ArtMethod* CreateRuntimeMethod(ClassLinker* class_linker, LinearAlloc* linear_alloc)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const PointerSize image_pointer_size = class_linker->GetImagePointerSize();
  const size_t method_alignment = ArtMethod::Alignment(image_pointer_size);
  const size_t method_size = ArtMethod::Size(image_pointer_size);
  LengthPrefixedArray<ArtMethod>* method_array =
      class_linker->AllocArtMethodArray(Thread::Current(), linear_alloc, 1);
  ArtMethod* method = &method_array->At(0, method_size, method_alignment);
  CHECK(method != nullptr);
  method->SetDexMethodIndex(dex::kDexNoIndex);
  CHECK(method->IsRuntimeMethod());
  return method;
}
ArtMethod* Runtime::CreateImtConflictMethod(LinearAlloc* linear_alloc) {
  ClassLinker* const class_linker = GetClassLinker();
  ArtMethod* method = CreateRuntimeMethod(class_linker, linear_alloc);
  const PointerSize pointer_size = GetInstructionSetPointerSize(instruction_set_);
  if (IsAotCompiler()) {
    method->SetEntryPointFromQuickCompiledCodePtrSize(nullptr, pointer_size);
  } else {
    method->SetEntryPointFromQuickCompiledCode(GetQuickImtConflictStub());
  }
  method->SetImtConflictTable(class_linker->CreateImtConflictTable( 0u, linear_alloc),
                              pointer_size);
  return method;
}
void Runtime::SetImtConflictMethod(ArtMethod* method) {
  CHECK(method != nullptr);
  CHECK(method->IsRuntimeMethod());
  imt_conflict_method_ = method;
}
ArtMethod* Runtime::CreateResolutionMethod() {
  auto* method = CreateRuntimeMethod(GetClassLinker(), GetLinearAlloc());
  if (IsAotCompiler()) {
    PointerSize pointer_size = GetInstructionSetPointerSize(instruction_set_);
    method->SetEntryPointFromQuickCompiledCodePtrSize(nullptr, pointer_size);
    method->SetEntryPointFromJniPtrSize(nullptr, pointer_size);
  } else {
    method->SetEntryPointFromQuickCompiledCode(GetQuickResolutionStub());
    method->SetEntryPointFromJni(GetJniDlsymLookupCriticalStub());
  }
  return method;
}
ArtMethod* Runtime::CreateCalleeSaveMethod() {
  auto* method = CreateRuntimeMethod(GetClassLinker(), GetLinearAlloc());
  PointerSize pointer_size = GetInstructionSetPointerSize(instruction_set_);
  method->SetEntryPointFromQuickCompiledCodePtrSize(nullptr, pointer_size);
  DCHECK_NE(instruction_set_, InstructionSet::kNone);
  DCHECK(method->IsRuntimeMethod());
  return method;
}
void Runtime::DisallowNewSystemWeaks() {
  CHECK(!gUseReadBarrier);
  monitor_list_->DisallowNewMonitors();
  intern_table_->ChangeWeakRootState(gc::kWeakRootStateNoReadsOrWrites);
  java_vm_->DisallowNewWeakGlobals();
  heap_->DisallowNewAllocationRecords();
  if (GetJit() != nullptr) {
    GetJit()->GetCodeCache()->DisallowInlineCacheAccess();
  }
  for (gc::AbstractSystemWeakHolder* holder : system_weak_holders_) {
    holder->Disallow();
  }
}
void Runtime::AllowNewSystemWeaks() {
  CHECK(!gUseReadBarrier);
  monitor_list_->AllowNewMonitors();
  intern_table_->ChangeWeakRootState(gc::kWeakRootStateNormal);
  java_vm_->AllowNewWeakGlobals();
  heap_->AllowNewAllocationRecords();
  if (GetJit() != nullptr) {
    GetJit()->GetCodeCache()->AllowInlineCacheAccess();
  }
  for (gc::AbstractSystemWeakHolder* holder : system_weak_holders_) {
    holder->Allow();
  }
}
void Runtime::BroadcastForNewSystemWeaks(bool broadcast_for_checkpoint) {
  monitor_list_->BroadcastForNewMonitors();
  intern_table_->BroadcastForNewInterns();
  java_vm_->BroadcastForNewWeakGlobals();
  heap_->BroadcastForNewAllocationRecords();
  if (GetJit() != nullptr) {
    GetJit()->GetCodeCache()->BroadcastForInlineCacheAccess();
  }
  for (gc::AbstractSystemWeakHolder* holder : system_weak_holders_) {
    holder->Broadcast(broadcast_for_checkpoint);
  }
}
void Runtime::SetInstructionSet(InstructionSet instruction_set) {
  instruction_set_ = instruction_set;
  switch (instruction_set) {
    case InstructionSet::kThumb2:
      instruction_set_ = InstructionSet::kArm;
      break;
    case InstructionSet::kArm:
    case InstructionSet::kArm64:
    case InstructionSet::kRiscv64:
    case InstructionSet::kX86:
    case InstructionSet::kX86_64:
      break;
    default:
      UNIMPLEMENTED(FATAL) << instruction_set_;
      UNREACHABLE();
  }
}
void Runtime::ClearInstructionSet() { instruction_set_ = InstructionSet::kNone; }
void Runtime::SetCalleeSaveMethod(ArtMethod* method, CalleeSaveType type) {
  DCHECK_LT(static_cast<uint32_t>(type), kCalleeSaveSize);
  CHECK(method != nullptr);
  callee_save_methods_[static_cast<size_t>(type)] = reinterpret_cast<uintptr_t>(method);
}
void Runtime::ClearCalleeSaveMethods() {
  for (size_t i = 0; i < kCalleeSaveSize; ++i) {
    callee_save_methods_[i] = reinterpret_cast<uintptr_t>(nullptr);
  }
}
void Runtime::RegisterAppInfo(const std::string& package_name,
                              const std::vector<std::string>& code_paths,
                              const std::string& profile_output_filename,
                              const std::string& ref_profile_filename,
                              int32_t code_type) {
  app_info_.RegisterAppInfo(package_name,
                            code_paths,
                            profile_output_filename,
                            ref_profile_filename,
                            AppInfo::FromVMRuntimeConstants(code_type));
  if (metrics_reporter_ != nullptr) {
    metrics_reporter_->NotifyAppInfoUpdated(&app_info_);
  }
  if (jit_.get() == nullptr) {
    return;
  }
  VLOG(profiler) << "Register app with " << profile_output_filename << " "
                 << android::base::Join(code_paths, ':');
  VLOG(profiler) << "Reference profile is: " << ref_profile_filename;
  if (profile_output_filename.empty()) {
    LOG(WARNING) << "JIT profile information will not be recorded: profile filename is empty.";
    return;
  }
  if (code_paths.empty()) {
    LOG(WARNING) << "JIT profile information will not be recorded: code paths is empty.";
    return;
  }
  jit_->StartProfileSaver(profile_output_filename, code_paths, ref_profile_filename);
}
bool Runtime::IsActiveTransaction() const {
  return !preinitialization_transactions_.empty() && !GetTransaction()->IsRollingBack();
}
void Runtime::EnterTransactionMode(bool strict, mirror::Class* root) {
  DCHECK(IsAotCompiler());
  ArenaPool* arena_pool = nullptr;
  ArenaStack* arena_stack = nullptr;
  if (preinitialization_transactions_.empty()) {
    {
      Thread* self = Thread::Current();
      StackHandleScope<1> hs(self);
      HandleWrapper<mirror::Class> h(hs.NewHandleWrapper(&root));
      ScopedThreadSuspension sts(self, ThreadState::kNative);
      GetClassLinker()->MakeInitializedClassesVisiblyInitialized(Thread::Current(), true);
    }
    arena_pool = GetArenaPool();
  } else {
    arena_stack = preinitialization_transactions_.front().GetArenaStack();
  }
  preinitialization_transactions_.emplace_front(strict, root, arena_stack, arena_pool);
}
void Runtime::ExitTransactionMode() {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transactions_.pop_front();
}
void Runtime::RollbackAndExitTransactionMode() {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transactions_.front().Rollback();
  preinitialization_transactions_.pop_front();
}
bool Runtime::IsTransactionAborted() const {
  if (!IsActiveTransaction()) {
    return false;
  } else {
    DCHECK(IsAotCompiler());
    return GetTransaction()->IsAborted();
  }
}
void Runtime::RollbackAllTransactions() {
  while (IsActiveTransaction()) {
    RollbackAndExitTransactionMode();
  }
}
bool Runtime::IsActiveStrictTransactionMode() const {
  return IsActiveTransaction() && GetTransaction()->IsStrict();
}
const Transaction* Runtime::GetTransaction() const {
  DCHECK(!preinitialization_transactions_.empty());
  return &preinitialization_transactions_.front();
}
Transaction* Runtime::GetTransaction() {
  DCHECK(!preinitialization_transactions_.empty());
  return &preinitialization_transactions_.front();
}
void Runtime::AbortTransactionAndThrowAbortError(Thread* self, const std::string& abort_message) {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  GetTransaction()->Abort(abort_message);
  GetTransaction()->ThrowAbortError(self, &abort_message);
}
void Runtime::ThrowTransactionAbortError(Thread* self) {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  GetTransaction()->ThrowAbortError(self, nullptr);
}
void Runtime::RecordWriteFieldBoolean(mirror::Object* obj,
                                      MemberOffset field_offset,
                                      uint8_t value,
                                      bool is_volatile) {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  GetTransaction()->RecordWriteFieldBoolean(obj, field_offset, value, is_volatile);
}
void Runtime::RecordWriteFieldByte(mirror::Object* obj,
                                   MemberOffset field_offset,
                                   int8_t value,
                                   bool is_volatile) {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  GetTransaction()->RecordWriteFieldByte(obj, field_offset, value, is_volatile);
}
void Runtime::RecordWriteFieldChar(mirror::Object* obj,
                                   MemberOffset field_offset,
                                   uint16_t value,
                                   bool is_volatile) {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  GetTransaction()->RecordWriteFieldChar(obj, field_offset, value, is_volatile);
}
void Runtime::RecordWriteFieldShort(mirror::Object* obj,
                                    MemberOffset field_offset,
                                    int16_t value,
                                    bool is_volatile) {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  GetTransaction()->RecordWriteFieldShort(obj, field_offset, value, is_volatile);
}
void Runtime::RecordWriteField32(mirror::Object* obj,
                                 MemberOffset field_offset,
                                 uint32_t value,
                                 bool is_volatile) {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  GetTransaction()->RecordWriteField32(obj, field_offset, value, is_volatile);
}
void Runtime::RecordWriteField64(mirror::Object* obj,
                                 MemberOffset field_offset,
                                 uint64_t value,
                                 bool is_volatile) {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  GetTransaction()->RecordWriteField64(obj, field_offset, value, is_volatile);
}
void Runtime::RecordWriteFieldReference(mirror::Object* obj,
                                        MemberOffset field_offset,
                                        ObjPtr<mirror::Object> value,
                                        bool is_volatile) {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  GetTransaction()->RecordWriteFieldReference(obj, field_offset, value.Ptr(), is_volatile);
}
void Runtime::RecordWriteArray(mirror::Array* array, size_t index, uint64_t value) {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  GetTransaction()->RecordWriteArray(array, index, value);
}
void Runtime::RecordStrongStringInsertion(ObjPtr<mirror::String> s) {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  GetTransaction()->RecordStrongStringInsertion(s);
}
void Runtime::RecordWeakStringInsertion(ObjPtr<mirror::String> s) {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  GetTransaction()->RecordWeakStringInsertion(s);
}
void Runtime::RecordStrongStringRemoval(ObjPtr<mirror::String> s) {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  GetTransaction()->RecordStrongStringRemoval(s);
}
void Runtime::RecordWeakStringRemoval(ObjPtr<mirror::String> s) {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  GetTransaction()->RecordWeakStringRemoval(s);
}
void Runtime::RecordResolveString(ObjPtr<mirror::DexCache> dex_cache, dex::StringIndex string_idx) {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  GetTransaction()->RecordResolveString(dex_cache, string_idx);
}
void Runtime::RecordResolveMethodType(ObjPtr<mirror::DexCache> dex_cache,
                                      dex::ProtoIndex proto_idx) {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  GetTransaction()->RecordResolveMethodType(dex_cache, proto_idx);
}
void Runtime::SetFaultMessage(const std::string& message) {
  std::string* new_msg = new std::string(message);
  std::string* cur_msg = fault_message_.exchange(new_msg);
  delete cur_msg;
}
std::string Runtime::GetFaultMessage() {
  std::string* cur_msg = fault_message_.exchange(nullptr);
  std::string ret = cur_msg == nullptr ? "" : *cur_msg;
  std::string* null_str = nullptr;
  if (!fault_message_.compare_exchange_strong(null_str, cur_msg)) {
    delete cur_msg;
  }
  return ret;
}
void Runtime::AddCurrentRuntimeFeaturesAsDex2OatArguments(std::vector<std::string>* argv) const {
  if (GetInstrumentation()->InterpretOnly()) {
    argv->push_back("--compiler-filter=quicken");
  }
  std::string instruction_set("--instruction-set=");
  instruction_set += GetInstructionSetString(kRuntimeISA);
  argv->push_back(instruction_set);
  if (InstructionSetFeatures::IsRuntimeDetectionSupported()) {
    argv->push_back("--instruction-set-features=runtime");
  } else {
    std::unique_ptr<const InstructionSetFeatures> features(
        InstructionSetFeatures::FromCppDefines());
    std::string feature_string("--instruction-set-features=");
    feature_string += features->GetFeatureString();
    argv->push_back(feature_string);
  }
}
void Runtime::CreateJit() {
  DCHECK(jit_code_cache_ == nullptr);
  DCHECK(jit_ == nullptr);
  if (kIsDebugBuild && GetInstrumentation()->IsForcedInterpretOnly()) {
    DCHECK(!jit_options_->UseJitCompilation());
  }
  if (!jit_options_->UseJitCompilation() && !jit_options_->GetSaveProfilingInfo()) {
    return;
  }
  if (IsSafeMode()) {
    LOG(INFO) << "Not creating JIT because of SafeMode.";
    return;
  }
  std::string error_msg;
  bool profiling_only = !jit_options_->UseJitCompilation();
  jit_code_cache_.reset(jit::JitCodeCache::Create(profiling_only,
                                                                         true,
                                                  IsZygote(),
                                                  &error_msg));
  if (jit_code_cache_.get() == nullptr) {
    LOG(WARNING) << "Failed to create JIT Code Cache: " << error_msg;
    return;
  }
  jit::Jit* jit = jit::Jit::Create(jit_code_cache_.get(), jit_options_.get());
  jit_.reset(jit);
  if (jit == nullptr) {
    LOG(WARNING) << "Failed to allocate JIT";
    jit_code_cache_.reset();
  } else {
    jit->CreateThreadPool();
  }
}
bool Runtime::CanRelocate() const { return !IsAotCompiler(); }
bool Runtime::IsCompilingBootImage() const {
  return IsCompiler() && compiler_callbacks_->IsBootImage();
}
void Runtime::SetResolutionMethod(ArtMethod* method) {
  CHECK(method != nullptr);
  CHECK(method->IsRuntimeMethod()) << method;
  resolution_method_ = method;
}
void Runtime::SetImtUnimplementedMethod(ArtMethod* method) {
  CHECK(method != nullptr);
  CHECK(method->IsRuntimeMethod());
  imt_unimplemented_method_ = method;
}
void Runtime::FixupConflictTables() {
  const PointerSize pointer_size = GetClassLinker()->GetImagePointerSize();
  if (imt_unimplemented_method_->GetImtConflictTable(pointer_size) == nullptr) {
    imt_unimplemented_method_->SetImtConflictTable(
        ClassLinker::CreateImtConflictTable( 0u, GetLinearAlloc(), pointer_size),
        pointer_size);
  }
  if (imt_conflict_method_->GetImtConflictTable(pointer_size) == nullptr) {
    imt_conflict_method_->SetImtConflictTable(
        ClassLinker::CreateImtConflictTable( 0u, GetLinearAlloc(), pointer_size),
        pointer_size);
  }
}
void Runtime::DisableVerifier() { verify_ = verifier::VerifyMode::kNone; }
bool Runtime::IsVerificationEnabled() const {
  return verify_ == verifier::VerifyMode::kEnable || verify_ == verifier::VerifyMode::kSoftFail;
}
bool Runtime::IsVerificationSoftFail() const { return verify_ == verifier::VerifyMode::kSoftFail; }
bool Runtime::IsAsyncDeoptimizeable(ArtMethod* method, uintptr_t code) const {
  if (OatQuickMethodHeader::NterpMethodHeader != nullptr) {
    if (OatQuickMethodHeader::NterpMethodHeader->Contains(code)) {
      return true;
    }
  }
  if (GetJit() != nullptr &&
      GetJit()->GetCodeCache()->PrivateRegionContainsPc(reinterpret_cast<const void*>(code))) {
    const OatQuickMethodHeader* header = method->GetOatQuickMethodHeader(code);
    return CodeInfo::IsDebuggable(header->GetOptimizedCodeInfoPtr());
  }
  return false;
}
LinearAlloc* Runtime::CreateLinearAlloc() {
  ArenaPool* pool = linear_alloc_arena_pool_.get();
  return pool != nullptr ? new LinearAlloc(pool, gUseUserfaultfd) :
                           new LinearAlloc(arena_pool_.get(), false);
}
class Runtime::SetupLinearAllocForZygoteFork : public AllocatorVisitor {
 public:
  explicit SetupLinearAllocForZygoteFork(Thread* self) : self_(self) {}
  bool Visit(LinearAlloc* alloc) override {
    alloc->SetupForPostZygoteFork(self_);
    return true;
  }
 private:
  Thread* self_;
};
void Runtime::SetupLinearAllocForPostZygoteFork(Thread* self) {
  if (gUseUserfaultfd) {
    if (GetLinearAlloc() != nullptr) {
      GetLinearAlloc()->SetupForPostZygoteFork(self);
    }
    if (GetStartupLinearAlloc() != nullptr) {
      GetStartupLinearAlloc()->SetupForPostZygoteFork(self);
    }
    {
      Locks::mutator_lock_->AssertNotHeld(self);
      ReaderMutexLock mu2(self, *Locks::mutator_lock_);
      ReaderMutexLock mu3(self, *Locks::classlinker_classes_lock_);
      SetupLinearAllocForZygoteFork visitor(self);
      GetClassLinker()->VisitAllocators(&visitor);
    }
    static_cast<GcVisitedArenaPool*>(GetLinearAllocArenaPool())->SetupPostZygoteMode();
  }
}
double Runtime::GetHashTableMinLoadFactor() const {
  return is_low_memory_mode_ ? kLowMemoryMinLoadFactor : kNormalMinLoadFactor;
}
double Runtime::GetHashTableMaxLoadFactor() const {
  return is_low_memory_mode_ ? kLowMemoryMaxLoadFactor : kNormalMaxLoadFactor;
}
void Runtime::UpdateProcessState(ProcessState process_state) {
  ProcessState old_process_state = process_state_;
  process_state_ = process_state;
  GetHeap()->UpdateProcessState(old_process_state, process_state);
}
void Runtime::RegisterSensitiveThread() const { Thread::SetJitSensitiveThread(); }
bool Runtime::UseJitCompilation() const { return (jit_ != nullptr) && jit_->UseJitCompilation(); }
void Runtime::EnvSnapshot::TakeSnapshot() {
  char** env = GetEnviron();
  for (size_t i = 0; env[i] != nullptr; ++i) {
    name_value_pairs_.emplace_back(new std::string(env[i]));
  }
  c_env_vector_.reset(new char*[name_value_pairs_.size() + 1]);
  for (size_t i = 0; env[i] != nullptr; ++i) {
    c_env_vector_[i] = const_cast<char*>(name_value_pairs_[i]->c_str());
  }
  c_env_vector_[name_value_pairs_.size()] = nullptr;
}
char** Runtime::EnvSnapshot::GetSnapshot() const { return c_env_vector_.get(); }
void Runtime::AddSystemWeakHolder(gc::AbstractSystemWeakHolder* holder) {
  gc::ScopedGCCriticalSection gcs(Thread::Current(),
                                  gc::kGcCauseAddRemoveSystemWeakHolder,
                                  gc::kCollectorTypeAddRemoveSystemWeakHolder);
  system_weak_holders_.push_back(holder);
}
void Runtime::RemoveSystemWeakHolder(gc::AbstractSystemWeakHolder* holder) {
  gc::ScopedGCCriticalSection gcs(Thread::Current(),
                                  gc::kGcCauseAddRemoveSystemWeakHolder,
                                  gc::kCollectorTypeAddRemoveSystemWeakHolder);
  auto it = std::find(system_weak_holders_.begin(), system_weak_holders_.end(), holder);
  if (it != system_weak_holders_.end()) {
    system_weak_holders_.erase(it);
  }
}
RuntimeCallbacks* Runtime::GetRuntimeCallbacks() { return callbacks_.get(); }
class UpdateEntryPointsClassVisitor : public ClassVisitor {
 public:
  explicit UpdateEntryPointsClassVisitor(instrumentation::Instrumentation* instrumentation)
      : instrumentation_(instrumentation) {}
  bool operator()(ObjPtr<mirror::Class> klass) override REQUIRES(Locks::mutator_lock_) {
    DCHECK(Locks::mutator_lock_->IsExclusiveHeld(Thread::Current()));
    auto pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
    for (auto& m : klass->GetMethods(pointer_size)) {
      const void* code = m.GetEntryPointFromQuickCompiledCode();
      if (!m.IsInvokable()) {
        continue;
      }
      bool deoptimize_native_methods = Runtime::Current()->IsJavaDebuggable();
      if (Runtime::Current()->GetHeap()->IsInBootImageOatFile(code) &&
          (!m.IsNative() || deoptimize_native_methods) && !m.IsProxyMethod()) {
        instrumentation_->InitializeMethodsCode(&m, nullptr);
      }
      if (Runtime::Current()->GetJit() != nullptr &&
          Runtime::Current()->GetJit()->GetCodeCache()->IsInZygoteExecSpace(code) &&
          (!m.IsNative() || deoptimize_native_methods)) {
        DCHECK(!m.IsProxyMethod());
        instrumentation_->InitializeMethodsCode(&m, nullptr);
      }
      if (m.IsPreCompiled()) {
        m.ClearPreCompiled();
        instrumentation_->InitializeMethodsCode(&m, nullptr);
      }
    }
    return true;
  }
 private:
  instrumentation::Instrumentation* const instrumentation_;
};
void Runtime::SetRuntimeDebugState(RuntimeDebugState state) {
  if (state != RuntimeDebugState::kJavaDebuggableAtInit) {
    DCHECK(runtime_debug_state_ != RuntimeDebugState::kJavaDebuggableAtInit);
  }
  runtime_debug_state_ = state;
}
void Runtime::DeoptimizeBootImage() {
  UpdateEntryPointsClassVisitor visitor(GetInstrumentation());
  GetClassLinker()->VisitClasses(&visitor);
  jit::Jit* jit = GetJit();
  if (jit != nullptr) {
    jit->GetCodeCache()->TransitionToDebuggable();
  }
}
Runtime::ScopedThreadPoolUsage::ScopedThreadPoolUsage()
    : thread_pool_(Runtime::Current()->AcquireThreadPool()) {}
Runtime::ScopedThreadPoolUsage::~ScopedThreadPoolUsage() {
  Runtime::Current()->ReleaseThreadPool();
}
bool Runtime::DeleteThreadPool() {
  WaitForThreadPoolWorkersToStart();
  std::unique_ptr<ThreadPool> thread_pool;
  {
    MutexLock mu(Thread::Current(), *Locks::runtime_thread_pool_lock_);
    if (thread_pool_ref_count_ == 0) {
      thread_pool = std::move(thread_pool_);
    }
  }
  return thread_pool != nullptr;
}
ThreadPool* Runtime::AcquireThreadPool() {
  MutexLock mu(Thread::Current(), *Locks::runtime_thread_pool_lock_);
  ++thread_pool_ref_count_;
  return thread_pool_.get();
}
void Runtime::ReleaseThreadPool() {
  MutexLock mu(Thread::Current(), *Locks::runtime_thread_pool_lock_);
  CHECK_GT(thread_pool_ref_count_, 0u);
  --thread_pool_ref_count_;
}
void Runtime::WaitForThreadPoolWorkersToStart() {
  ScopedThreadPoolUsage stpu;
  if (stpu.GetThreadPool() != nullptr) {
    stpu.GetThreadPool()->WaitForWorkersToBeCreated();
  }
}
void Runtime::ResetStartupCompleted() {
  startup_completed_.store(false, std::memory_order_seq_cst);
}
bool Runtime::NotifyStartupCompleted() {
  bool expected = false;
  if (!startup_completed_.compare_exchange_strong(expected, true, std::memory_order_seq_cst)) {
    return false;
  }
  VLOG(startup) << app_info_;
  ProfileSaver::NotifyStartupCompleted();
  if (metrics_reporter_ != nullptr) {
    metrics_reporter_->NotifyStartupCompleted();
  }
  return true;
}
void Runtime::NotifyDexFileLoaded() {
  if (metrics_reporter_ != nullptr) {
    metrics_reporter_->NotifyAppInfoUpdated(&app_info_);
  }
}
bool Runtime::GetStartupCompleted() const {
  return startup_completed_.load(std::memory_order_seq_cst);
}
void Runtime::SetSignalHookDebuggable(bool value) { SkipAddSignalHandler(value); }
void Runtime::SetJniIdType(JniIdType t) {
  CHECK(CanSetJniIdType()) << "Not allowed to change id type!";
  if (t == GetJniIdType()) {
    return;
  }
  jni_ids_indirection_ = t;
  JNIEnvExt::ResetFunctionTable();
  WellKnownClasses::HandleJniIdTypeChange(Thread::Current()->GetJniEnv());
}
bool Runtime::IsSystemServerProfiled() const {
  return IsSystemServer() && jit_options_->GetSaveProfilingInfo();
}
bool Runtime::GetOatFilesExecutable() const {
  return !IsAotCompiler() && !IsSystemServerProfiled();
}
void Runtime::MadviseFileForRange(size_t madvise_size_limit_bytes,
                                  size_t map_size_bytes,
                                  const uint8_t* map_begin,
                                  const uint8_t* map_end,
                                  const std::string& file_name) {
#ifdef ART_TARGET_ANDROID
  static const int kApiLevel = android_get_device_api_level();
  const bool accurate_process_state_at_startup = kApiLevel >= __ANDROID_API_T__;
  if (accurate_process_state_at_startup) {
    const Runtime* runtime = Runtime::Current();
    if (runtime != nullptr && !runtime->InJankPerceptibleProcessState()) {
      return;
    }
  }
#endif
  static constexpr size_t kIdealIoTransferSizeBytes = 128 * 1024;
  size_t target_size_bytes = std::min<size_t>(map_size_bytes, madvise_size_limit_bytes);
  if (target_size_bytes > 0) {
    ScopedTrace madvising_trace("madvising " + file_name +
                                " size=" + std::to_string(target_size_bytes));
    const uint8_t* target_pos = map_begin + target_size_bytes;
    if (target_pos > map_end) {
      target_pos = map_end;
    }
    for (const uint8_t* madvise_start = map_begin; madvise_start < target_pos;
         madvise_start += kIdealIoTransferSizeBytes) {
      void* madvise_addr = const_cast<void*>(reinterpret_cast<const void*>(madvise_start));
      size_t madvise_length =
          std::min(kIdealIoTransferSizeBytes, static_cast<size_t>(target_pos - madvise_start));
      int status = madvise(madvise_addr, madvise_length, MADV_WILLNEED);
      if (status < 0) {
        LOG(ERROR) << "Failed to madvise file:" << file_name << " for size:" << map_size_bytes;
        break;
      }
    }
  }
}
bool Runtime::HasImageWithProfile() const {
  for (gc::space::ImageSpace* space : GetHeap()->GetBootImageSpaces()) {
    if (!space->GetProfileFiles().empty()) {
      return true;
    }
  }
  return false;
}
void Runtime::AppendToBootClassPath(const std::string& filename, const std::string& location) {
  DCHECK(!DexFileLoader::IsMultiDexLocation(filename.c_str()));
  boot_class_path_.push_back(filename);
  if (!boot_class_path_locations_.empty()) {
    DCHECK(!DexFileLoader::IsMultiDexLocation(location.c_str()));
    boot_class_path_locations_.push_back(location);
  }
}
void Runtime::AppendToBootClassPath(
    const std::string& filename,
    const std::string& location,
    const std::vector<std::pair<const art::DexFile*, ObjPtr<mirror::DexCache>>>&
        dex_files_and_cache) {
  AppendToBootClassPath(filename, location);
  ScopedObjectAccess soa(Thread::Current());
  for (const auto& [dex_file, dex_cache] : dex_files_and_cache) {
    DCHECK_NE(DexFileLoader::IsMultiDexLocation(dex_file->GetLocation().c_str()),
              dex_file == dex_files_and_cache.begin()->first);
    GetClassLinker()->AppendToBootClassPath(dex_file, dex_cache);
  }
}
void Runtime::AddExtraBootDexFiles(const std::string& filename,
                                   const std::string& location,
                                   std::vector<std::unique_ptr<const art::DexFile>>&& dex_files) {
  AppendToBootClassPath(filename, location);
  ScopedObjectAccess soa(Thread::Current());
  if (kIsDebugBuild) {
    for (const std::unique_ptr<const art::DexFile>& dex_file : dex_files) {
      DCHECK_NE(DexFileLoader::IsMultiDexLocation(dex_file->GetLocation().c_str()),
                dex_file.get() == dex_files.begin()->get());
    }
  }
  GetClassLinker()->AddExtraBootDexFiles(Thread::Current(), std::move(dex_files));
}
}
