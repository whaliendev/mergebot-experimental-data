#ifndef ART_RUNTIME_RUNTIME_H_
#define ART_RUNTIME_RUNTIME_H_ 
#include <jni.h>
#include <stdio.h>
#include <forward_list>
#include <iosfwd>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "app_info.h"
#include "base/locks.h"
#include "base/macros.h"
#include "base/mem_map.h"
#include "base/metrics/metrics.h"
#include "base/string_view_cpp20.h"
#include "compat_framework.h"
#include "deoptimization_kind.h"
#include "dex/dex_file_types.h"
#include "experimental_flags.h"
#include "gc_root.h"
#include "instrumentation.h"
#include "jdwp_provider.h"
#include "jni/jni_id_manager.h"
#include "jni_id_type.h"
#include "metrics/reporter.h"
#include "obj_ptr.h"
#include "offsets.h"
#include "process_state.h"
#include "quick/quick_method_frame_info.h"
#include "reflective_value_visitor.h"
#include "runtime_stats.h"
namespace art {
namespace gc {
class AbstractSystemWeakHolder;
class Heap;
}
namespace hiddenapi {
enum class EnforcementPolicy;
}
namespace jit {
class Jit;
class JitCodeCache;
class JitOptions;
}
namespace jni {
class SmallLrtAllocator;
}
namespace mirror {
class Array;
class ClassLoader;
class DexCache;
template<class T> class ObjectArray;
template<class T> class PrimitiveArray;
using ByteArray = PrimitiveArray<int8_t>;
class String;
class Throwable;
}
namespace ti {
class Agent;
class AgentSpec;
}
namespace verifier {
class MethodVerifier;
enum class VerifyMode : int8_t;
}
class ArenaPool;
class ArtMethod;
enum class CalleeSaveType: uint32_t;
class ClassLinker;
class CompilerCallbacks;
class Dex2oatImageTest;
class DexFile;
enum class InstructionSet;
class InternTable;
class IsMarkedVisitor;
class JavaVMExt;
class LinearAlloc;
class MonitorList;
class MonitorPool;
class NullPointerHandler;
class OatFileAssistantTest;
class OatFileManager;
class Plugin;
struct RuntimeArgumentMap;
class RuntimeCallbacks;
class SignalCatcher;
class StackOverflowHandler;
class SuspensionHandler;
class ThreadList;
class ThreadPool;
class Trace;
struct TraceConfig;
class Transaction;
using RuntimeOptions = std::vector<std::pair<std::string, const void*>>;
class Runtime {
 public:
  static bool ParseOptions(const RuntimeOptions& raw_options,
                           bool ignore_unrecognized,
                           RuntimeArgumentMap* runtime_options);
  static bool Create(RuntimeArgumentMap&& runtime_options)
      SHARED_TRYLOCK_FUNCTION(true, Locks::mutator_lock_);
  static bool Create(const RuntimeOptions& raw_options, bool ignore_unrecognized)
      SHARED_TRYLOCK_FUNCTION(true, Locks::mutator_lock_);
  enum class RuntimeDebugState {
    kNonJavaDebuggable,
    kJavaDebuggable,
    kJavaDebuggableAtInit
  };
  bool EnsurePluginLoaded(const char* plugin_name, std::string* error_msg);
  bool EnsurePerfettoPlugin(std::string* error_msg);
  bool IsAotCompiler() const {
    return !UseJitCompilation() && IsCompiler();
  }
  bool IsCompiler() const {
    return compiler_callbacks_ != nullptr;
  }
  bool IsCompilingBootImage() const;
  bool CanRelocate() const;
  bool ShouldRelocate() const {
    return must_relocate_ && CanRelocate();
  }
  bool MustRelocateIfPossible() const {
    return must_relocate_;
  }
  bool IsImageDex2OatEnabled() const {
    return image_dex2oat_enabled_;
  }
  CompilerCallbacks* GetCompilerCallbacks() {
    return compiler_callbacks_;
  }
  void SetCompilerCallbacks(CompilerCallbacks* callbacks) {
    CHECK(callbacks != nullptr);
    compiler_callbacks_ = callbacks;
  }
  bool IsZygote() const {
    return is_zygote_;
  }
  bool IsPrimaryZygote() const {
    return is_primary_zygote_;
  }
  bool IsSystemServer() const {
    return is_system_server_;
  }
  void SetAsSystemServer() {
    is_system_server_ = true;
    is_zygote_ = false;
    is_primary_zygote_ = false;
  }
  void SetAsZygoteChild(bool is_system_server, bool is_zygote) {
    CHECK_EQ(is_system_server_, is_system_server);
    is_zygote_ = is_zygote;
    is_primary_zygote_ = false;
  }
  bool IsExplicitGcDisabled() const {
    return is_explicit_gc_disabled_;
  }
  std::string GetCompilerExecutable() const;
  const std::vector<std::string>& GetCompilerOptions() const {
    return compiler_options_;
  }
  void AddCompilerOption(const std::string& option) {
    compiler_options_.push_back(option);
  }
  const std::vector<std::string>& GetImageCompilerOptions() const {
    return image_compiler_options_;
  }
  const std::vector<std::string>& GetImageLocations() const {
    return image_locations_;
  }
  bool Start() UNLOCK_FUNCTION(Locks::mutator_lock_);
  bool IsShuttingDown(Thread* self);
  bool IsShuttingDownLocked() const REQUIRES(Locks::runtime_shutdown_lock_) {
    return shutting_down_.load(std::memory_order_relaxed);
  }
  bool IsShuttingDownUnsafe() const {
    return shutting_down_.load(std::memory_order_relaxed);
  }
  void SetShuttingDown() REQUIRES(Locks::runtime_shutdown_lock_) {
    shutting_down_.store(true, std::memory_order_relaxed);
  }
  size_t NumberOfThreadsBeingBorn() const REQUIRES(Locks::runtime_shutdown_lock_) {
    return threads_being_born_;
  }
  void StartThreadBirth() REQUIRES(Locks::runtime_shutdown_lock_) {
    threads_being_born_++;
  }
  void EndThreadBirth() REQUIRES(Locks::runtime_shutdown_lock_);
  bool IsStarted() const {
    return started_;
  }
  bool IsFinishedStarting() const {
    return finished_starting_;
  }
  void RunRootClinits(Thread* self) REQUIRES_SHARED(Locks::mutator_lock_);
  static Runtime* Current() {
    return instance_;
  }
  static void TestOnlySetCurrent(Runtime* instance) { instance_ = instance; }
  NO_RETURN static void Abort(const char* msg) REQUIRES(!Locks::abort_lock_);
  jobject GetMainThreadGroup() const;
  jobject GetSystemThreadGroup() const;
  jobject GetSystemClassLoader() const;
  bool AttachCurrentThread(const char* thread_name,
                           bool as_daemon,
                           jobject thread_group,
                           bool create_peer,
                           bool should_run_callbacks = true);
  void CallExitHook(jint status);
  void DetachCurrentThread(bool should_run_callbacks = true) REQUIRES(!Locks::mutator_lock_);
  void DumpDeoptimizations(std::ostream& os);
  void DumpForSigQuit(std::ostream& os);
  void DumpLockHolders(std::ostream& os);
  ~Runtime();
  const std::vector<std::string>& GetBootClassPath() const {
    return boot_class_path_;
  }
  const std::vector<std::string>& GetBootClassPathLocations() const {
    DCHECK(boot_class_path_locations_.empty() ||
           boot_class_path_locations_.size() == boot_class_path_.size());
    return boot_class_path_locations_.empty() ? boot_class_path_ : boot_class_path_locations_;
  }
  void AppendToBootClassPath(const std::string& filename,
                             const std::string& location,
                             const std::vector<std::unique_ptr<const art::DexFile>>& dex_files);
  void AppendToBootClassPath(const std::string& filename,
                             const std::string& location,
                             const std::vector<const art::DexFile*>& dex_files);
  void AppendToBootClassPath(
      const std::string& filename,
      const std::string& location,
      const std::vector<std::pair<const art::DexFile*, ObjPtr<mirror::DexCache>>>&
          dex_files_and_cache);
  void AddExtraBootDexFiles(const std::string& filename,
                            const std::string& location,
                            std::vector<std::unique_ptr<const art::DexFile>>&& dex_files);
  const std::vector<int>& GetBootClassPathFds() const {
    return boot_class_path_fds_;
  }
  const std::vector<int>& GetBootClassPathImageFds() const {
    return boot_class_path_image_fds_;
  }
  const std::vector<int>& GetBootClassPathVdexFds() const {
    return boot_class_path_vdex_fds_;
  }
  const std::vector<int>& GetBootClassPathOatFds() const {
    return boot_class_path_oat_fds_;
  }
  const std::string& GetBootClassPathChecksums() const {
    return boot_class_path_checksums_;
  }
  const std::string& GetClassPathString() const {
    return class_path_string_;
  }
  ClassLinker* GetClassLinker() const {
    return class_linker_;
  }
  jni::SmallLrtAllocator* GetSmallLrtAllocator() const {
    return small_lrt_allocator_;
  }
  jni::JniIdManager* GetJniIdManager() const {
    return jni_id_manager_.get();
  }
  size_t GetDefaultStackSize() const {
    return default_stack_size_;
  }
  unsigned int GetFinalizerTimeoutMs() const {
    return finalizer_timeout_ms_;
  }
  gc::Heap* GetHeap() const {
    return heap_;
  }
  InternTable* GetInternTable() const {
    DCHECK(intern_table_ != nullptr);
    return intern_table_;
  }
  JavaVMExt* GetJavaVM() const {
    return java_vm_.get();
  }
  size_t GetMaxSpinsBeforeThinLockInflation() const {
    return max_spins_before_thin_lock_inflation_;
  }
  MonitorList* GetMonitorList() const {
    return monitor_list_;
  }
  MonitorPool* GetMonitorPool() const {
    return monitor_pool_;
  }
  bool IsClearedJniWeakGlobal(ObjPtr<mirror::Object> obj) REQUIRES_SHARED(Locks::mutator_lock_);
  mirror::Object* GetClearedJniWeakGlobal() REQUIRES_SHARED(Locks::mutator_lock_);
  mirror::Throwable* GetPreAllocatedOutOfMemoryErrorWhenThrowingException()
      REQUIRES_SHARED(Locks::mutator_lock_);
  mirror::Throwable* GetPreAllocatedOutOfMemoryErrorWhenThrowingOOME()
      REQUIRES_SHARED(Locks::mutator_lock_);
  mirror::Throwable* GetPreAllocatedOutOfMemoryErrorWhenHandlingStackOverflow()
      REQUIRES_SHARED(Locks::mutator_lock_);
  mirror::Throwable* GetPreAllocatedNoClassDefFoundError()
      REQUIRES_SHARED(Locks::mutator_lock_);
  const std::vector<std::string>& GetProperties() const {
    return properties_;
  }
  ThreadList* GetThreadList() const {
    return thread_list_;
  }
  static const char* GetVersion() {
    return "2.1.0";
  }
  bool IsMethodHandlesEnabled() const {
    return true;
  }
  void DisallowNewSystemWeaks() REQUIRES_SHARED(Locks::mutator_lock_);
  void AllowNewSystemWeaks() REQUIRES_SHARED(Locks::mutator_lock_);
  void BroadcastForNewSystemWeaks(bool broadcast_for_checkpoint = false);
  void VisitRoots(RootVisitor* visitor, VisitRootFlags flags = kVisitRootFlagAllRoots)
      REQUIRES(!Locks::classlinker_classes_lock_, !Locks::trace_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void VisitImageRoots(RootVisitor* visitor) REQUIRES_SHARED(Locks::mutator_lock_);
  void VisitConcurrentRoots(RootVisitor* visitor,
                            VisitRootFlags flags = kVisitRootFlagAllRoots)
      REQUIRES(!Locks::classlinker_classes_lock_, !Locks::trace_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void VisitNonThreadRoots(RootVisitor* visitor)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void VisitTransactionRoots(RootVisitor* visitor)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void SweepSystemWeaks(IsMarkedVisitor* visitor) REQUIRES_SHARED(Locks::mutator_lock_);
  void VisitReflectiveTargets(ReflectiveValueVisitor* visitor) REQUIRES(Locks::mutator_lock_);
  template <typename FieldVis, typename MethodVis>
  void VisitReflectiveTargets(FieldVis&& fv, MethodVis&& mv) REQUIRES(Locks::mutator_lock_) {
    FunctionReflectiveValueVisitor frvv(fv, mv);
    VisitReflectiveTargets(&frvv);
  }
  ArtMethod* GetResolutionMethod();
  bool HasResolutionMethod() const {
    return resolution_method_ != nullptr;
  }
  void SetResolutionMethod(ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_);
  void ClearResolutionMethod() {
    resolution_method_ = nullptr;
  }
  ArtMethod* CreateResolutionMethod() REQUIRES_SHARED(Locks::mutator_lock_);
  ArtMethod* GetImtConflictMethod();
  ArtMethod* GetImtUnimplementedMethod();
  bool HasImtConflictMethod() const {
    return imt_conflict_method_ != nullptr;
  }
  void ClearImtConflictMethod() {
    imt_conflict_method_ = nullptr;
  }
  void FixupConflictTables() REQUIRES_SHARED(Locks::mutator_lock_);
  void SetImtConflictMethod(ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_);
  void SetImtUnimplementedMethod(ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_);
  ArtMethod* CreateImtConflictMethod(LinearAlloc* linear_alloc)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void ClearImtUnimplementedMethod() {
    imt_unimplemented_method_ = nullptr;
  }
  bool HasCalleeSaveMethod(CalleeSaveType type) const {
    return callee_save_methods_[static_cast<size_t>(type)] != 0u;
  }
  ArtMethod* GetCalleeSaveMethod(CalleeSaveType type)
      REQUIRES_SHARED(Locks::mutator_lock_);
  ArtMethod* GetCalleeSaveMethodUnchecked(CalleeSaveType type)
      REQUIRES_SHARED(Locks::mutator_lock_);
  QuickMethodFrameInfo GetRuntimeMethodFrameInfo(ArtMethod* method)
      REQUIRES_SHARED(Locks::mutator_lock_);
  static constexpr size_t GetCalleeSaveMethodOffset(CalleeSaveType type) {
    return OFFSETOF_MEMBER(Runtime, callee_save_methods_[static_cast<size_t>(type)]);
  }
  static constexpr MemberOffset GetInstrumentationOffset() {
    return MemberOffset(OFFSETOF_MEMBER(Runtime, instrumentation_));
  }
  InstructionSet GetInstructionSet() const {
    return instruction_set_;
  }
  void SetInstructionSet(InstructionSet instruction_set);
  void ClearInstructionSet();
  void SetCalleeSaveMethod(ArtMethod* method, CalleeSaveType type);
  void ClearCalleeSaveMethods();
  ArtMethod* CreateCalleeSaveMethod() REQUIRES_SHARED(Locks::mutator_lock_);
  uint64_t GetStat(int kind);
  RuntimeStats* GetStats() {
    return &stats_;
  }
  bool HasStatsEnabled() const {
    return stats_enabled_;
  }
  void ResetStats(int kinds);
  void SetStatsEnabled(bool new_state)
      REQUIRES(!Locks::instrument_entrypoints_lock_, !Locks::mutator_lock_);
  enum class NativeBridgeAction {
    kUnload,
    kInitialize
  };
  jit::Jit* GetJit() const {
    return jit_.get();
  }
  jit::JitCodeCache* GetJitCodeCache() const {
    return jit_code_cache_.get();
  }
  bool UseJitCompilation() const;
  void PreZygoteFork();
  void PostZygoteFork();
  void InitNonZygoteOrPostFork(
      JNIEnv* env,
      bool is_system_server,
      bool is_child_zygote,
      NativeBridgeAction action,
      const char* isa,
      bool profile_system_server = false);
  const instrumentation::Instrumentation* GetInstrumentation() const {
    return &instrumentation_;
  }
  instrumentation::Instrumentation* GetInstrumentation() {
    return &instrumentation_;
  }
  void RegisterAppInfo(const std::string& package_name,
                       const std::vector<std::string>& code_paths,
                       const std::string& profile_output_filename,
                       const std::string& ref_profile_filename,
                       int32_t code_type);
  bool IsActiveTransaction() const;
  void EnterTransactionMode(bool strict, mirror::Class* root) REQUIRES_SHARED(Locks::mutator_lock_);
  void ExitTransactionMode();
  void RollbackAllTransactions() REQUIRES_SHARED(Locks::mutator_lock_);
  void RollbackAndExitTransactionMode() REQUIRES_SHARED(Locks::mutator_lock_);
  bool IsTransactionAborted() const;
  const Transaction* GetTransaction() const;
  Transaction* GetTransaction();
  bool IsActiveStrictTransactionMode() const;
  void AbortTransactionAndThrowAbortError(Thread* self, const std::string& abort_message)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void ThrowTransactionAbortError(Thread* self)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void RecordWriteFieldBoolean(mirror::Object* obj,
                               MemberOffset field_offset,
                               uint8_t value,
                               bool is_volatile);
  void RecordWriteFieldByte(mirror::Object* obj,
                            MemberOffset field_offset,
                            int8_t value,
                            bool is_volatile);
  void RecordWriteFieldChar(mirror::Object* obj,
                            MemberOffset field_offset,
                            uint16_t value,
                            bool is_volatile);
  void RecordWriteFieldShort(mirror::Object* obj,
                             MemberOffset field_offset,
                             int16_t value,
                             bool is_volatile);
  void RecordWriteField32(mirror::Object* obj,
                          MemberOffset field_offset,
                          uint32_t value,
                          bool is_volatile);
  void RecordWriteField64(mirror::Object* obj,
                          MemberOffset field_offset,
                          uint64_t value,
                          bool is_volatile);
  void RecordWriteFieldReference(mirror::Object* obj,
                                 MemberOffset field_offset,
                                 ObjPtr<mirror::Object> value,
                                 bool is_volatile)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void RecordWriteArray(mirror::Array* array, size_t index, uint64_t value)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void RecordStrongStringInsertion(ObjPtr<mirror::String> s)
      REQUIRES(Locks::intern_table_lock_);
  void RecordWeakStringInsertion(ObjPtr<mirror::String> s)
      REQUIRES(Locks::intern_table_lock_);
  void RecordStrongStringRemoval(ObjPtr<mirror::String> s)
      REQUIRES(Locks::intern_table_lock_);
  void RecordWeakStringRemoval(ObjPtr<mirror::String> s)
      REQUIRES(Locks::intern_table_lock_);
  void RecordResolveString(ObjPtr<mirror::DexCache> dex_cache, dex::StringIndex string_idx)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void RecordResolveMethodType(ObjPtr<mirror::DexCache> dex_cache, dex::ProtoIndex proto_idx)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void SetFaultMessage(const std::string& message);
  void AddCurrentRuntimeFeaturesAsDex2OatArguments(std::vector<std::string>* arg_vector) const;
  bool GetImplicitStackOverflowChecks() const {
    return implicit_so_checks_;
  }
  bool GetImplicitSuspendChecks() const {
    return implicit_suspend_checks_;
  }
  bool GetImplicitNullChecks() const {
    return implicit_null_checks_;
  }
  void DisableVerifier();
  bool IsVerificationEnabled() const;
  bool IsVerificationSoftFail() const;
  void SetHiddenApiEnforcementPolicy(hiddenapi::EnforcementPolicy policy) {
    hidden_api_policy_ = policy;
  }
  hiddenapi::EnforcementPolicy GetHiddenApiEnforcementPolicy() const {
    return hidden_api_policy_;
  }
  void SetCorePlatformApiEnforcementPolicy(hiddenapi::EnforcementPolicy policy) {
    core_platform_api_policy_ = policy;
  }
  hiddenapi::EnforcementPolicy GetCorePlatformApiEnforcementPolicy() const {
    return core_platform_api_policy_;
  }
  void SetTestApiEnforcementPolicy(hiddenapi::EnforcementPolicy policy) {
    test_api_policy_ = policy;
  }
  hiddenapi::EnforcementPolicy GetTestApiEnforcementPolicy() const {
    return test_api_policy_;
  }
  void SetHiddenApiExemptions(const std::vector<std::string>& exemptions) {
    hidden_api_exemptions_ = exemptions;
  }
  const std::vector<std::string>& GetHiddenApiExemptions() {
    return hidden_api_exemptions_;
  }
  void SetDedupeHiddenApiWarnings(bool value) {
    dedupe_hidden_api_warnings_ = value;
  }
  bool ShouldDedupeHiddenApiWarnings() {
    return dedupe_hidden_api_warnings_;
  }
  void SetHiddenApiEventLogSampleRate(uint32_t rate) {
    hidden_api_access_event_log_rate_ = rate;
  }
  uint32_t GetHiddenApiEventLogSampleRate() const {
    return hidden_api_access_event_log_rate_;
  }
  const std::string& GetProcessPackageName() const {
    return process_package_name_;
  }
  void SetProcessPackageName(const char* package_name) {
    if (package_name == nullptr) {
      process_package_name_.clear();
    } else {
      process_package_name_ = package_name;
    }
  }
  const std::string& GetProcessDataDirectory() const {
    return process_data_directory_;
  }
  void SetProcessDataDirectory(const char* data_dir) {
    if (data_dir == nullptr) {
      process_data_directory_.clear();
    } else {
      process_data_directory_ = data_dir;
    }
  }
  const std::vector<std::string>& GetCpuAbilist() const {
    return cpu_abilist_;
  }
  bool IsRunningOnMemoryTool() const {
    return is_running_on_memory_tool_;
  }
  void SetTargetSdkVersion(uint32_t version) {
    target_sdk_version_ = version;
  }
  uint32_t GetTargetSdkVersion() const {
    return target_sdk_version_;
  }
  CompatFramework& GetCompatFramework() {
    return compat_framework_;
  }
  uint32_t GetZygoteMaxFailedBoots() const {
    return zygote_max_failed_boots_;
  }
  bool AreExperimentalFlagsEnabled(ExperimentalFlags flags) {
    return (experimental_flags_ & flags) != ExperimentalFlags::kNone;
  }
  void CreateJitCodeCache(bool rwx_memory_allowed);
  void CreateJit();
  ArenaPool* GetLinearAllocArenaPool() {
    return linear_alloc_arena_pool_.get();
  }
  ArenaPool* GetArenaPool() {
    return arena_pool_.get();
  }
  const ArenaPool* GetArenaPool() const {
    return arena_pool_.get();
  }
  ArenaPool* GetJitArenaPool() {
    return jit_arena_pool_.get();
  }
  void ReclaimArenaPoolMemory();
  LinearAlloc* GetLinearAlloc() {
    return linear_alloc_.get();
  }
  LinearAlloc* GetStartupLinearAlloc() {
    return startup_linear_alloc_.get();
  }
  jit::JitOptions* GetJITOptions() {
    return jit_options_.get();
  }
  bool IsJavaDebuggable() const {
    return runtime_debug_state_ == RuntimeDebugState::kJavaDebuggable ||
           runtime_debug_state_ == RuntimeDebugState::kJavaDebuggableAtInit;
  }
  bool IsJavaDebuggableAtInit() const {
    return runtime_debug_state_ == RuntimeDebugState::kJavaDebuggableAtInit;
  }
  void SetProfileableFromShell(bool value) {
    is_profileable_from_shell_ = value;
  }
  bool IsProfileableFromShell() const {
    return is_profileable_from_shell_;
  }
  void SetProfileable(bool value) {
    is_profileable_ = value;
  }
  bool IsProfileable() const {
    return is_profileable_;
  }
  void SetRuntimeDebugState(RuntimeDebugState state);
  void DeoptimizeBootImage() REQUIRES(Locks::mutator_lock_);
  bool IsNativeDebuggable() const {
    return is_native_debuggable_;
  }
  void SetNativeDebuggable(bool value) {
    is_native_debuggable_ = value;
  }
  void SetSignalHookDebuggable(bool value);
  bool AreNonStandardExitsEnabled() const {
    return non_standard_exits_enabled_;
  }
  void SetNonStandardExitsEnabled() {
    non_standard_exits_enabled_ = true;
  }
  bool AreAsyncExceptionsThrown() const {
    return async_exceptions_thrown_;
  }
  void SetAsyncExceptionsThrown() {
    async_exceptions_thrown_ = true;
  }
  std::string GetFingerprint() {
    return fingerprint_;
  }
  void SetSentinel(ObjPtr<mirror::Object> sentinel) REQUIRES_SHARED(Locks::mutator_lock_);
  GcRoot<mirror::Object> GetSentinel() REQUIRES_SHARED(Locks::mutator_lock_);
  static mirror::Class* GetWeakClassSentinel() {
    return reinterpret_cast<mirror::Class*>(0xebadbeef);
  }
  static void ProcessWeakClass(GcRoot<mirror::Class>* root_ptr,
                               IsMarkedVisitor* visitor,
                               mirror::Class* update)
      REQUIRES_SHARED(Locks::mutator_lock_);
  LinearAlloc* CreateLinearAlloc();
  void SetupLinearAllocForPostZygoteFork(Thread* self)
      REQUIRES(!Locks::mutator_lock_, !Locks::classlinker_classes_lock_);
  OatFileManager& GetOatFileManager() const {
    DCHECK(oat_file_manager_ != nullptr);
    return *oat_file_manager_;
  }
  double GetHashTableMinLoadFactor() const;
  double GetHashTableMaxLoadFactor() const;
  bool IsSafeMode() const {
    return safe_mode_;
  }
  void SetSafeMode(bool mode) {
    safe_mode_ = mode;
  }
  bool GetDumpNativeStackOnSigQuit() const {
    return dump_native_stack_on_sig_quit_;
  }
  void UpdateProcessState(ProcessState process_state);
  bool InJankPerceptibleProcessState() const {
    return process_state_ == kProcessStateJankPerceptible;
  }
  void RegisterSensitiveThread() const;
  void SetZygoteNoThreadSection(bool val) {
    zygote_no_threads_ = val;
  }
  bool IsZygoteNoThreadSection() const {
    return zygote_no_threads_;
  }
  bool IsAsyncDeoptimizeable(ArtMethod* method, uintptr_t code) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  char** GetEnvSnapshot() const {
    return env_snapshot_.GetSnapshot();
  }
  void AddSystemWeakHolder(gc::AbstractSystemWeakHolder* holder);
  void RemoveSystemWeakHolder(gc::AbstractSystemWeakHolder* holder);
  void AttachAgent(JNIEnv* env, const std::string& agent_arg, jobject class_loader);
  const std::list<std::unique_ptr<ti::Agent>>& GetAgents() const {
    return agents_;
  }
  RuntimeCallbacks* GetRuntimeCallbacks();
  bool HasLoadedPlugins() const {
    return !plugins_.empty();
  }
  void InitThreadGroups(Thread* self);
  void SetDumpGCPerformanceOnShutdown(bool value) {
    dump_gc_performance_on_shutdown_ = value;
  }
  bool GetDumpGCPerformanceOnShutdown() const {
    return dump_gc_performance_on_shutdown_;
  }
  void IncrementDeoptimizationCount(DeoptimizationKind kind) {
    DCHECK_LE(kind, DeoptimizationKind::kLast);
    deoptimization_counts_[static_cast<size_t>(kind)]++;
  }
  uint32_t GetNumberOfDeoptimizations() const {
    uint32_t result = 0;
    for (size_t i = 0; i <= static_cast<size_t>(DeoptimizationKind::kLast); ++i) {
      result += deoptimization_counts_[i];
    }
    return result;
  }
  bool DenyArtApexDataFiles() const {
    return deny_art_apex_data_files_;
  }
  bool MAdviseRandomAccess() const {
    return madvise_random_access_;
  }
  size_t GetMadviseWillNeedSizeVdex() const {
    return madvise_willneed_vdex_filesize_;
  }
  size_t GetMadviseWillNeedSizeOdex() const {
    return madvise_willneed_odex_filesize_;
  }
  size_t GetMadviseWillNeedSizeArt() const {
    return madvise_willneed_art_filesize_;
  }
  const std::string& GetJdwpOptions() {
    return jdwp_options_;
  }
  JdwpProvider GetJdwpProvider() const {
    return jdwp_provider_;
  }
  JniIdType GetJniIdType() const {
    return jni_ids_indirection_;
  }
  bool CanSetJniIdType() const {
    return GetJniIdType() == JniIdType::kSwapablePointer;
  }
  void SetJniIdType(JniIdType t);
  uint32_t GetVerifierLoggingThresholdMs() const {
    return verifier_logging_threshold_ms_;
  }
  bool DeleteThreadPool() REQUIRES(!Locks::runtime_thread_pool_lock_);
  void WaitForThreadPoolWorkersToStart() REQUIRES(!Locks::runtime_thread_pool_lock_);
  class ScopedThreadPoolUsage {
   public:
    ScopedThreadPoolUsage();
    ~ScopedThreadPoolUsage();
    ThreadPool* GetThreadPool() const {
      return thread_pool_;
    }
   private:
    ThreadPool* const thread_pool_;
  };
  LinearAlloc* ReleaseStartupLinearAlloc() {
    return startup_linear_alloc_.release();
  }
  bool LoadAppImageStartupCache() const {
    return load_app_image_startup_cache_;
  }
  void SetLoadAppImageStartupCacheEnabled(bool enabled) {
    load_app_image_startup_cache_ = enabled;
  }
  void ResetStartupCompleted();
  bool NotifyStartupCompleted();
  void NotifyDexFileLoaded();
  bool GetStartupCompleted() const;
  bool IsVerifierMissingKThrowFatal() const {
    return verifier_missing_kthrow_fatal_;
  }
  bool IsJavaZygoteForkLoopRequired() const {
    return force_java_zygote_fork_loop_;
  }
  bool IsPerfettoHprofEnabled() const {
    return perfetto_hprof_enabled_;
  }
  bool IsPerfettoJavaHeapStackProfEnabled() const {
    return perfetto_javaheapprof_enabled_;
  }
  bool IsMonitorTimeoutEnabled() const {
    return monitor_timeout_enable_;
  }
  uint64_t GetMonitorTimeoutNs() const {
    return monitor_timeout_ns_;
  }
  bool IsSystemServerProfiled() const;
  bool GetOatFilesExecutable() const;
  metrics::ArtMetrics* GetMetrics() { return &metrics_; }
  AppInfo* GetAppInfo() { return &app_info_; }
  void RequestMetricsReport(bool synchronous = true);
  static void MadviseFileForRange(size_t madvise_size_limit_bytes,
                                  size_t map_size_bytes,
                                  const uint8_t* map_begin,
                                  const uint8_t* map_end,
                                  const std::string& file_name);
  const std::string& GetApexVersions() const {
    return apex_versions_;
  }
  bool HasImageWithProfile() const;
  bool GetNoSigChain() const {
    return no_sig_chain_;
  }
  void AddGeneratedCodeRange(const void* start, size_t size);
  void RemoveGeneratedCodeRange(const void* start, size_t size)
      REQUIRES_SHARED(Locks::mutator_lock_);
  static void ReloadAllFlags(const std::string& caller);
  static std::string GetApexVersions(ArrayRef<const std::string> boot_class_path_locations);
  bool AllowInMemoryCompilation() const { return allow_in_memory_compilation_; }
  void SetOutOfMemoryErrorHook(void (*hook)()) {
    out_of_memory_error_hook_ = hook;
  }
  void OutOfMemoryErrorHook() {
    if (out_of_memory_error_hook_ != nullptr) {
      out_of_memory_error_hook_();
    }
  }
 private:
  static void InitPlatformSignalHandlers();
  Runtime();
  bool HandlesSignalsInCompiledCode() const {
    return !no_sig_chain_ &&
           (implicit_null_checks_ || implicit_so_checks_ || implicit_suspend_checks_);
  }
  void BlockSignals();
  bool Init(RuntimeArgumentMap&& runtime_options)
      SHARED_TRYLOCK_FUNCTION(true, Locks::mutator_lock_);
  void InitNativeMethods() REQUIRES(!Locks::mutator_lock_);
  void RegisterRuntimeNativeMethods(JNIEnv* env);
  void InitMetrics();
  void StartDaemonThreads() REQUIRES_SHARED(Locks::mutator_lock_);
  void StartSignalCatcher();
  void MaybeSaveJitProfilingInfo();
  void VisitThreadRoots(RootVisitor* visitor, VisitRootFlags flags)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void VisitNonConcurrentRoots(RootVisitor* visitor, VisitRootFlags flags)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void VisitConstantRoots(RootVisitor* visitor)
      REQUIRES_SHARED(Locks::mutator_lock_);
  std::string GetFaultMessage();
  ThreadPool* AcquireThreadPool() REQUIRES(!Locks::runtime_thread_pool_lock_);
  void ReleaseThreadPool() REQUIRES(!Locks::runtime_thread_pool_lock_);
  void InitializeApexVersions();
  void AppendToBootClassPath(const std::string& filename, const std::string& location);
  static Runtime* instance_;
  static constexpr int kProfileForground = 0;
  static constexpr int kProfileBackground = 1;
  static constexpr uint32_t kCalleeSaveSize = 6u;
  uint64_t callee_save_methods_[kCalleeSaveSize];
  GcRoot<mirror::Throwable> pre_allocated_OutOfMemoryError_when_throwing_exception_;
  GcRoot<mirror::Throwable> pre_allocated_OutOfMemoryError_when_throwing_oome_;
  GcRoot<mirror::Throwable> pre_allocated_OutOfMemoryError_when_handling_stack_overflow_;
  GcRoot<mirror::Throwable> pre_allocated_NoClassDefFoundError_;
  ArtMethod* resolution_method_;
  ArtMethod* imt_conflict_method_;
  ArtMethod* imt_unimplemented_method_;
  GcRoot<mirror::Object> sentinel_;
  InstructionSet instruction_set_;
  CompilerCallbacks* compiler_callbacks_;
  bool is_zygote_;
  bool is_primary_zygote_;
  bool is_system_server_;
  bool must_relocate_;
  bool is_concurrent_gc_enabled_;
  bool is_explicit_gc_disabled_;
  bool image_dex2oat_enabled_;
  std::string compiler_executable_;
  std::vector<std::string> compiler_options_;
  std::vector<std::string> image_compiler_options_;
  std::vector<std::string> image_locations_;
  std::vector<std::string> boot_class_path_;
  std::vector<std::string> boot_class_path_locations_;
  std::string boot_class_path_checksums_;
  std::vector<int> boot_class_path_fds_;
  std::vector<int> boot_class_path_image_fds_;
  std::vector<int> boot_class_path_vdex_fds_;
  std::vector<int> boot_class_path_oat_fds_;
  std::string class_path_string_;
  std::vector<std::string> properties_;
  std::list<ti::AgentSpec> agent_specs_;
  std::list<std::unique_ptr<ti::Agent>> agents_;
  std::vector<Plugin> plugins_;
  size_t default_stack_size_;
  unsigned int finalizer_timeout_ms_;
  gc::Heap* heap_;
  std::unique_ptr<ArenaPool> jit_arena_pool_;
  std::unique_ptr<ArenaPool> arena_pool_;
  std::unique_ptr<ArenaPool> linear_alloc_arena_pool_;
  std::unique_ptr<LinearAlloc> linear_alloc_;
  std::unique_ptr<LinearAlloc> startup_linear_alloc_;
  size_t max_spins_before_thin_lock_inflation_;
  MonitorList* monitor_list_;
  MonitorPool* monitor_pool_;
  ThreadList* thread_list_;
  InternTable* intern_table_;
  ClassLinker* class_linker_;
  SignalCatcher* signal_catcher_;
  jni::SmallLrtAllocator* small_lrt_allocator_;
  std::unique_ptr<jni::JniIdManager> jni_id_manager_;
  std::unique_ptr<JavaVMExt> java_vm_;
  std::unique_ptr<jit::Jit> jit_;
  std::unique_ptr<jit::JitCodeCache> jit_code_cache_;
  std::unique_ptr<jit::JitOptions> jit_options_;
  std::unique_ptr<ThreadPool> thread_pool_ GUARDED_BY(Locks::runtime_thread_pool_lock_);
  size_t thread_pool_ref_count_ GUARDED_BY(Locks::runtime_thread_pool_lock_);
  std::atomic<std::string*> fault_message_;
  size_t threads_being_born_ GUARDED_BY(Locks::runtime_shutdown_lock_);
  std::unique_ptr<ConditionVariable> shutdown_cond_ GUARDED_BY(Locks::runtime_shutdown_lock_);
  std::atomic<bool> shutting_down_;
  bool shutting_down_started_ GUARDED_BY(Locks::runtime_shutdown_lock_);
  bool started_;
  bool finished_starting_;
  jint (*vfprintf_)(FILE* stream, const char* format, va_list ap);
  void (*exit_)(jint status);
  void (*abort_)();
  bool stats_enabled_;
  RuntimeStats stats_;
  const bool is_running_on_memory_tool_;
  std::unique_ptr<TraceConfig> trace_config_;
  instrumentation::Instrumentation instrumentation_;
  jobject main_thread_group_;
  jobject system_thread_group_;
  jobject system_class_loader_;
  bool dump_gc_performance_on_shutdown_;
  std::forward_list<Transaction> preinitialization_transactions_;
  verifier::VerifyMode verify_;
  std::vector<std::string> cpu_abilist_;
  uint32_t target_sdk_version_;
  CompatFramework compat_framework_;
  bool implicit_null_checks_;
  bool implicit_so_checks_;
  bool implicit_suspend_checks_;
  bool no_sig_chain_;
  bool force_native_bridge_;
  bool is_native_bridge_loaded_;
  bool is_native_debuggable_;
  bool async_exceptions_thrown_;
  bool non_standard_exits_enabled_;
  RuntimeDebugState runtime_debug_state_;
  bool monitor_timeout_enable_;
  uint64_t monitor_timeout_ns_;
  bool is_profileable_from_shell_ = false;
  bool is_profileable_ = false;
  uint32_t zygote_max_failed_boots_;
  ExperimentalFlags experimental_flags_;
  std::string fingerprint_;
  OatFileManager* oat_file_manager_;
  bool is_low_memory_mode_;
  bool madvise_random_access_;
  size_t madvise_willneed_vdex_filesize_;
  size_t madvise_willneed_odex_filesize_;
  size_t madvise_willneed_art_filesize_;
  bool safe_mode_;
  hiddenapi::EnforcementPolicy hidden_api_policy_;
  hiddenapi::EnforcementPolicy core_platform_api_policy_;
  hiddenapi::EnforcementPolicy test_api_policy_;
  std::vector<std::string> hidden_api_exemptions_;
  bool dedupe_hidden_api_warnings_;
  uint32_t hidden_api_access_event_log_rate_;
  std::string process_package_name_;
  std::string process_data_directory_;
  bool dump_native_stack_on_sig_quit_;
  ProcessState process_state_;
  bool zygote_no_threads_;
  std::string jdwp_options_;
  JdwpProvider jdwp_provider_;
  JniIdType jni_ids_indirection_;
  bool automatically_set_jni_ids_indirection_;
  bool deny_art_apex_data_files_;
  bool allow_in_memory_compilation_ = false;
  class EnvSnapshot {
   public:
    EnvSnapshot() = default;
    void TakeSnapshot();
    char** GetSnapshot() const;
   private:
    std::unique_ptr<char*[]> c_env_vector_;
    std::vector<std::unique_ptr<std::string>> name_value_pairs_;
    DISALLOW_COPY_AND_ASSIGN(EnvSnapshot);
  } env_snapshot_;
  std::vector<gc::AbstractSystemWeakHolder*> system_weak_holders_;
  std::unique_ptr<RuntimeCallbacks> callbacks_;
  std::atomic<uint32_t> deoptimization_counts_[
      static_cast<uint32_t>(DeoptimizationKind::kLast) + 1];
  MemMap protected_fault_page_;
  uint32_t verifier_logging_threshold_ms_;
  bool load_app_image_startup_cache_ = false;
  std::atomic<bool> startup_completed_ = false;
  bool verifier_missing_kthrow_fatal_;
  bool force_java_zygote_fork_loop_;
  bool perfetto_hprof_enabled_;
  bool perfetto_javaheapprof_enabled_;
  void (*out_of_memory_error_hook_)();
  metrics::ArtMetrics metrics_;
  std::unique_ptr<metrics::MetricsReporter> metrics_reporter_;
  std::string apex_versions_;
  AppInfo app_info_;
  friend std::string GetFaultMessageForAbortLogging();
  friend class Dex2oatImageTest;
  friend class ScopedThreadPoolUsage;
  friend class OatFileAssistantTest;
  class SetupLinearAllocForZygoteFork;
  DISALLOW_COPY_AND_ASSIGN(Runtime);
};
inline metrics::ArtMetrics* GetMetrics() { return Runtime::Current()->GetMetrics(); }
}
#endif
