#include "thread.h"
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/time.h>
#if __has_feature(hwaddress_sanitizer)
#include <sanitizer/hwasan_interface.h>
#else
#define __hwasan_tag_pointer(p,t) (p)
#endif
#include <algorithm>
#include <atomic>
#include <bitset>
#include <cerrno>
#include <iostream>
#include <list>
#include <sstream>
#include "android-base/file.h"
#include "android-base/stringprintf.h"
#include "android-base/strings.h"
#include "unwindstack/AndroidUnwinder.h"
#include "arch/context-inl.h"
#include "arch/context.h"
#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/atomic.h"
#include "base/bit_utils.h"
#include "base/casts.h"
#include "base/file_utils.h"
#include "base/memory_tool.h"
#include "base/mutex.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "base/time_utils.h"
#include "base/timing_logger.h"
#include "base/to_str.h"
#include "base/utils.h"
#include "class_linker-inl.h"
#include "class_root-inl.h"
#include "debugger.h"
#include "dex/descriptors_names.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file_annotations.h"
#include "dex/dex_file_types.h"
#include "entrypoints/entrypoint_utils.h"
#include "entrypoints/quick/quick_alloc_entrypoints.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/accounting/heap_bitmap-inl.h"
#include "gc/allocator/rosalloc.h"
#include "gc/heap.h"
#include "gc/space/space-inl.h"
#include "gc_root.h"
#include "handle_scope-inl.h"
#include "indirect_reference_table-inl.h"
#include "instrumentation.h"
#include "interpreter/interpreter.h"
#include "interpreter/shadow_frame-inl.h"
#include "java_frame_root_info.h"
#include "jni/java_vm_ext.h"
#include "jni/jni_internal.h"
#include "mirror/class-alloc-inl.h"
#include "mirror/class_loader.h"
#include "mirror/object_array-alloc-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/stack_frame_info.h"
#include "mirror/stack_trace_element.h"
#include "monitor.h"
#include "monitor_objects_stack_visitor.h"
#include "native_stack_dump.h"
#include "nativehelper/scoped_local_ref.h"
#include "nativehelper/scoped_utf_chars.h"
#include "nterp_helpers.h"
#include "nth_caller_visitor.h"
#include "oat_quick_method_header.h"
#include "obj_ptr-inl.h"
#include "object_lock.h"
#include "palette/palette.h"
#include "quick/quick_method_frame_info.h"
#include "quick_exception_handler.h"
#include "read_barrier-inl.h"
#include "reflection.h"
#include "reflective_handle_scope-inl.h"
#include "runtime-inl.h"
#include "runtime.h"
#include "runtime_callbacks.h"
#include "scoped_thread_state_change-inl.h"
#include "scoped_disable_public_sdk_checker.h"
#include "stack.h"
#include "stack_map.h"
#include "thread-inl.h"
#include "thread_list.h"
#include "trace.h"
#include "verifier/method_verifier.h"
#include "verify_object.h"
#include "well_known_classes.h"
#include "well_known_classes-inl.h"
#if ART_USE_FUTEXES
#include "linux/futex.h"
#include "sys/syscall.h"
#ifndef SYS_futex
#define SYS_futex __NR_futex
#endif
#endif
#pragma clang diagnostic push
#pragma clang diagnostic error "-Wconversion"
namespace art {
using android::base::StringAppendV;
using android::base::StringPrintf;
extern "C" NO_RETURN void artDeoptimize(Thread* self, bool skip_method_exit_callbacks);
Thread* Thread::jit_sensitive_thread_ = nullptr;
Thread* Thread::jit_sensitive_thread_ = nullptr;
Thread* Thread::jit_sensitive_thread_ = nullptr;
Thread* Thread::jit_sensitive_thread_ = nullptr;
Thread* Thread::jit_sensitive_thread_ = nullptr;
#ifndef __BIONIC__
thread_local Thread* Thread::self_tls_ = nullptr;
#endif
static constexpr bool kVerifyImageObjectsMarked = kIsDebugBuild;
constexpr size_t kStackOverflowProtectedSize = 4 * kMemoryToolStackGuardSizeScale * KB;
static const char* kThreadNameDuringStartup = "<native thread without managed peer>";
void Thread::InitCardTable() {
  tlsPtr_.card_table = Runtime::Current()->GetHeap()->GetCardTable()->GetBiasedBegin();
}
static void UnimplementedEntryPoint() {
  UNIMPLEMENTED(FATAL);
}
void InitEntryPoints(JniEntryPoints* jpoints,
                     QuickEntryPoints* qpoints,
                     bool monitor_jni_entry_exit);
void UpdateReadBarrierEntrypoints(QuickEntryPoints* qpoints, bool is_active);
void Thread::SetIsGcMarkingAndUpdateEntrypoints(bool is_marking) {
  CHECK(gUseReadBarrier);
  tls32_.is_gc_marking = is_marking;
  UpdateReadBarrierEntrypoints(&tlsPtr_.quick_entrypoints, is_marking);
}
void Thread::InitTlsEntryPoints() {
  ScopedTrace trace("InitTlsEntryPoints");
  uintptr_t* begin = reinterpret_cast<uintptr_t*>(&tlsPtr_.jni_entrypoints);
  uintptr_t* end = reinterpret_cast<uintptr_t*>(
      reinterpret_cast<uint8_t*>(&tlsPtr_.quick_entrypoints) + sizeof(tlsPtr_.quick_entrypoints));
  for (uintptr_t* it = begin; it != end; ++it) {
    *it = reinterpret_cast<uintptr_t>(UnimplementedEntryPoint);
  }
  bool monitor_jni_entry_exit = false;
  PaletteShouldReportJniInvocations(&monitor_jni_entry_exit);
  if (monitor_jni_entry_exit) {
    AtomicSetFlag(ThreadFlag::kMonitorJniEntryExit);
  }
  InitEntryPoints(&tlsPtr_.jni_entrypoints, &tlsPtr_.quick_entrypoints, monitor_jni_entry_exit);
}
void Thread::ResetQuickAllocEntryPointsForThread() {
  ResetQuickAllocEntryPoints(&tlsPtr_.quick_entrypoints);
}
class DeoptimizationContextRecord {
public:
  DeoptimizationContextRecord(const JValue& ret_val,
                              bool is_reference,
                              bool from_code,
                              ObjPtr<mirror::Throwable> pending_exception,
                              DeoptimizationMethodType method_type,
                              DeoptimizationContextRecord* link)
      : ret_val_(ret_val),
        is_reference_(is_reference),
        from_code_(from_code),
        pending_exception_(pending_exception.Ptr()),
        deopt_method_type_(method_type),
        link_(link) {}
  JValue GetReturnValue() const { return ret_val_; }
  bool IsReference() const { return is_reference_; }
  bool GetFromCode() const { return from_code_; }
ObjPtr<mirror::Throwable> GetPendingException() const REQUIRES_SHARED(Locks::mutator_lock_){
                                                        return pending_exception_;
                                                        }
  DeoptimizationContextRecord* GetLink() const { return link_; }
  mirror::Object** GetReturnValueAsGCRoot() {
    DCHECK(is_reference_);
    return ret_val_.GetGCRoot();
  }
  mirror::Object** GetPendingExceptionAsGCRoot() {
    return reinterpret_cast<mirror::Object**>(&pending_exception_);
  }
  DeoptimizationMethodType GetDeoptimizationMethodType() const {
    return deopt_method_type_;
  }
private:
  JValue ret_val_;
  const bool is_reference_;
  const bool from_code_;
  mirror::Throwable* pending_exception_;
  const DeoptimizationMethodType deopt_method_type_;
  DeoptimizationContextRecord* const link_;
  DISALLOW_COPY_AND_ASSIGN(DeoptimizationContextRecord);
};
class StackedShadowFrameRecord {
public:
  StackedShadowFrameRecord(ShadowFrame* shadow_frame,
                           StackedShadowFrameType type,
                           StackedShadowFrameRecord* link)
      : shadow_frame_(shadow_frame),
        type_(type),
        link_(link) {}
  ShadowFrame* GetShadowFrame() const { return shadow_frame_; }
  StackedShadowFrameType GetType() const { return type_; }
  StackedShadowFrameRecord* GetLink() const { return link_; }
private:
  ShadowFrame* const shadow_frame_;
  const StackedShadowFrameType type_;
  StackedShadowFrameRecord* const link_;
  DISALLOW_COPY_AND_ASSIGN(StackedShadowFrameRecord);
};
void Thread::PushDeoptimizationContext(const JValue& return_value,
                                       bool is_reference,
                                       ObjPtr<mirror::Throwable> exception,
                                       bool from_code,
                                       DeoptimizationMethodType method_type) {
  DCHECK(exception != Thread::GetDeoptimizationException());
  DeoptimizationContextRecord* record = new DeoptimizationContextRecord(
      return_value,
      is_reference,
      from_code,
      exception,
      method_type,
      tlsPtr_.deoptimization_context_stack);
  tlsPtr_.deoptimization_context_stack = record;
}
void Thread::PopDeoptimizationContext(JValue* result,
                                      ObjPtr<mirror::Throwable>* exception,
                                      bool* from_code,
                                      DeoptimizationMethodType* method_type) {
  AssertHasDeoptimizationContext();
  DeoptimizationContextRecord* record = tlsPtr_.deoptimization_context_stack;
  tlsPtr_.deoptimization_context_stack = record->GetLink();
  result->SetJ(record->GetReturnValue().GetJ());
  *exception = record->GetPendingException();
  *from_code = record->GetFromCode();
  *method_type = record->GetDeoptimizationMethodType();
  delete record;
}
void Thread::AssertHasDeoptimizationContext() {
  CHECK(tlsPtr_.deoptimization_context_stack != nullptr)
      << "No deoptimization context for thread " << *this;
}
enum {
kPermitAvailable = 0,
kNoPermit = 1,
  kNoPermitWaiterWaiting = 2
};
void Thread::Park(bool is_absolute, int64_t time) {
  DCHECK(this == Thread::Current());
#if ART_USE_FUTEXES
  int old_state = tls32_.park_state_.fetch_add(1, std::memory_order_relaxed);
  if (old_state == kNoPermit) {
    Runtime::Current()->GetRuntimeCallbacks()->ThreadParkStart(is_absolute, time);
    bool timed_out = false;
    if (!is_absolute && time == 0) {
      ScopedThreadSuspension sts(this, ThreadState::kWaiting);
      DCHECK_EQ(NumberOfHeldMutexes(), 0u);
      int result = futex(tls32_.park_state_.Address(),
                     FUTEX_WAIT_PRIVATE,
                                          kNoPermitWaiterWaiting,
                                   nullptr,
                     nullptr,
                     0);
      if (result == -1) {
        switch (errno) {
          case EAGAIN:
            FALLTHROUGH_INTENDED;
          case EINTR: break;
          default: PLOG(FATAL) << "Failed to park";
        }
      }
    } else if (time > 0) {
      ScopedThreadSuspension sts(this, ThreadState::kTimedWaiting);
      DCHECK_EQ(NumberOfHeldMutexes(), 0u);
      timespec timespec;
      int result = 0;
      if (is_absolute) {
        timespec.tv_nsec = (time % 1000) * 1000000;
        timespec.tv_sec = SaturatedTimeT(time / 1000);
        result = futex(tls32_.park_state_.Address(),
                       FUTEX_WAIT_BITSET_PRIVATE | FUTEX_CLOCK_REALTIME,
                                            kNoPermitWaiterWaiting,
                       &timespec,
                       nullptr,
                       static_cast<int>(FUTEX_BITSET_MATCH_ANY));
      } else {
        timespec.tv_sec = SaturatedTimeT(time / 1000000000);
        timespec.tv_nsec = time % 1000000000;
        result = futex(tls32_.park_state_.Address(),
                       FUTEX_WAIT_PRIVATE,
                                            kNoPermitWaiterWaiting,
                       &timespec,
                       nullptr,
                       0);
      }
      if (result == -1) {
        switch (errno) {
          case ETIMEDOUT:
            timed_out = true;
            FALLTHROUGH_INTENDED;
          case EAGAIN:
          case EINTR: break;
          default: PLOG(FATAL) << "Failed to park";
        }
      }
    }
    tls32_.park_state_.store(kNoPermit, std::memory_order_relaxed);
    Runtime::Current()->GetRuntimeCallbacks()->ThreadParkFinished(timed_out);
  } else {
    DCHECK_EQ(old_state, kPermitAvailable);
  }
#else
  #pragma clang diagnostic push
  #pragma clang diagnostic warning "-W#warnings"
  #warning "LockSupport.park/unpark implemented as noops without FUTEX support."
  #pragma clang diagnostic pop
  UNUSED(is_absolute, time);
  UNIMPLEMENTED(WARNING);
  sched_yield();
#endif
}
void Thread::Unpark() {
#if ART_USE_FUTEXES
  if (tls32_.park_state_.exchange(kPermitAvailable, std::memory_order_relaxed)
      == kNoPermitWaiterWaiting) {
    int result = futex(tls32_.park_state_.Address(),
                       FUTEX_WAKE_PRIVATE,
                                                 1,
                       nullptr,
                       nullptr,
                       0);
    if (result == -1) {
      PLOG(FATAL) << "Failed to unpark";
    }
  }
#else
  UNIMPLEMENTED(WARNING);
#endif
}
void Thread::PushStackedShadowFrame(ShadowFrame* sf, StackedShadowFrameType type) {
  StackedShadowFrameRecord* record = new StackedShadowFrameRecord(
      sf, type, tlsPtr_.stacked_shadow_frame_record);
  tlsPtr_.stacked_shadow_frame_record = record;
}
ShadowFrame* Thread::MaybePopDeoptimizedStackedShadowFrame() {
  StackedShadowFrameRecord* record = tlsPtr_.stacked_shadow_frame_record;
  if (record == nullptr ||
      record->GetType() != StackedShadowFrameType::kDeoptimizationShadowFrame) {
    return nullptr;
  }
  return PopStackedShadowFrame();
}
ShadowFrame* Thread::PopStackedShadowFrame() {
  StackedShadowFrameRecord* record = tlsPtr_.stacked_shadow_frame_record;
  DCHECK_NE(record, nullptr);
  tlsPtr_.stacked_shadow_frame_record = record->GetLink();
  ShadowFrame* shadow_frame = record->GetShadowFrame();
  delete record;
  return shadow_frame;
}
class FrameIdToShadowFrame {
public:
  static FrameIdToShadowFrame* Create(size_t frame_id,
                                      ShadowFrame* shadow_frame,
                                      FrameIdToShadowFrame* next,
                                      size_t num_vregs) {
    uint8_t* memory = new uint8_t[sizeof(FrameIdToShadowFrame) + sizeof(bool) * num_vregs];
    return new (memory) FrameIdToShadowFrame(frame_id, shadow_frame, next);
  }
  static void Delete(FrameIdToShadowFrame* f) {
    uint8_t* memory = reinterpret_cast<uint8_t*>(f);
    delete[] memory;
  }
  size_t GetFrameId() const { return frame_id_; }
  ShadowFrame* GetShadowFrame() const { return shadow_frame_; }
  FrameIdToShadowFrame* GetNext() const { return next_; }
  void SetNext(FrameIdToShadowFrame* next) { next_ = next; }
  bool* GetUpdatedVRegFlags() {
    return updated_vreg_flags_;
  }
private:
  FrameIdToShadowFrame(size_t frame_id,
                       ShadowFrame* shadow_frame,
                       FrameIdToShadowFrame* next)
      : frame_id_(frame_id),
        shadow_frame_(shadow_frame),
        next_(next) {}
  const size_t frame_id_;
  ShadowFrame* const shadow_frame_;
  FrameIdToShadowFrame* next_;
  bool updated_vreg_flags_[0];
  DISALLOW_COPY_AND_ASSIGN(FrameIdToShadowFrame);
};
static FrameIdToShadowFrame* FindFrameIdToShadowFrame(FrameIdToShadowFrame* head,
                                                      size_t frame_id) {
  FrameIdToShadowFrame* found = nullptr;
  for (FrameIdToShadowFrame* record = head; record != nullptr; record = record->GetNext()) {
    if (record->GetFrameId() == frame_id) {
      if (kIsDebugBuild) {
        CHECK(found == nullptr) << "Multiple records for the frame " << frame_id;
        found = record;
      } else {
        return record;
      }
    }
  }
  return found;
}
ShadowFrame* Thread::FindDebuggerShadowFrame(size_t frame_id) {
  FrameIdToShadowFrame* record = FindFrameIdToShadowFrame(
      tlsPtr_.frame_id_to_shadow_frame, frame_id);
  if (record != nullptr) {
    return record->GetShadowFrame();
  }
  return nullptr;
}
bool* Thread::GetUpdatedVRegFlags(size_t frame_id) {
  FrameIdToShadowFrame* record = FindFrameIdToShadowFrame(
      tlsPtr_.frame_id_to_shadow_frame, frame_id);
  CHECK(record != nullptr);
  return record->GetUpdatedVRegFlags();
}
ShadowFrame* Thread::FindOrCreateDebuggerShadowFrame(size_t frame_id,
                                                     uint32_t num_vregs,
                                                     ArtMethod* method,
                                                     uint32_t dex_pc) {
  ShadowFrame* shadow_frame = FindDebuggerShadowFrame(frame_id);
  if (shadow_frame != nullptr) {
    return shadow_frame;
  }
  VLOG(deopt) << "Create pre-deopted ShadowFrame for " << ArtMethod::PrettyMethod(method);
  shadow_frame = ShadowFrame::CreateDeoptimizedFrame(num_vregs, method, dex_pc);
  FrameIdToShadowFrame* record = FrameIdToShadowFrame::Create(frame_id,
                                                              shadow_frame,
                                                              tlsPtr_.frame_id_to_shadow_frame,
                                                              num_vregs);
  for (uint32_t i = 0; i < num_vregs; i++) {
    shadow_frame->SetVRegReference(i, nullptr);
    record->GetUpdatedVRegFlags()[i] = false;
  }
  tlsPtr_.frame_id_to_shadow_frame = record;
  return shadow_frame;
}
TLSData* Thread::GetCustomTLS(const char* key) {
  MutexLock mu(Thread::Current(), *Locks::custom_tls_lock_);
  auto it = custom_tls_.find(key);
  return (it != custom_tls_.end()) ? it->second.get() : nullptr;
}
void Thread::SetCustomTLS(const char* key, TLSData* data) {
  std::unique_ptr<TLSData> old_data(data);
  {
    MutexLock mu(Thread::Current(), *Locks::custom_tls_lock_);
    custom_tls_.GetOrCreate(key, []() { return std::unique_ptr<TLSData>(); }).swap(old_data);
  }
}
void Thread::RemoveDebuggerShadowFrameMapping(size_t frame_id) {
  FrameIdToShadowFrame* head = tlsPtr_.frame_id_to_shadow_frame;
  if (head->GetFrameId() == frame_id) {
    tlsPtr_.frame_id_to_shadow_frame = head->GetNext();
    FrameIdToShadowFrame::Delete(head);
    return;
  }
  FrameIdToShadowFrame* prev = head;
  for (FrameIdToShadowFrame* record = head->GetNext();
       record != nullptr;
       prev = record, record = record->GetNext()) {
    if (record->GetFrameId() == frame_id) {
      prev->SetNext(record->GetNext());
      FrameIdToShadowFrame::Delete(record);
      return;
    }
  }
  LOG(FATAL) << "No shadow frame for frame " << frame_id;
  UNREACHABLE();
}
void Thread::InitTid() {
  tls32_.tid = ::art::GetTid();
}
void Thread::InitAfterFork() {
  InitTid();
}
void Thread::DeleteJPeer(JNIEnv* env) {
  jobject old_jpeer = tlsPtr_.jpeer;
  CHECK(old_jpeer != nullptr);
  tlsPtr_.jpeer = nullptr;
  env->DeleteGlobalRef(old_jpeer);
}
void* Thread::CreateCallbackWithUffdGc(void* arg) {
  return Thread::CreateCallback(arg);
}
void* Thread::CreateCallback(void* arg) {
  Thread* self = reinterpret_cast<Thread*>(arg);
  Runtime* runtime = Runtime::Current();
  if (runtime == nullptr) {
    LOG(ERROR) << "Thread attaching to non-existent runtime: " << *self;
    return nullptr;
  }
  {
    MutexLock mu(nullptr, *Locks::runtime_shutdown_lock_);
    CHECK(!runtime->IsShuttingDownLocked());
    CHECK(self->Init(runtime->GetThreadList(), runtime->GetJavaVM(), self->tlsPtr_.tmp_jni_env));
    self->tlsPtr_.tmp_jni_env = nullptr;
    Runtime::Current()->EndThreadBirth();
  }
  {
    ScopedObjectAccess soa(self);
    self->InitStringEntryPoints();
    CHECK(self->tlsPtr_.jpeer != nullptr);
    self->tlsPtr_.opeer = soa.Decode<mirror::Object>(self->tlsPtr_.jpeer).Ptr();
    self->DeleteJPeer(self->GetJniEnv());
    self->SetThreadName(self->GetThreadName()->ToModifiedUtf8().c_str());
    ArtField* priorityField = WellKnownClasses::java_lang_Thread_priority;
    self->SetNativePriority(priorityField->GetInt(self->tlsPtr_.opeer));
    runtime->GetRuntimeCallbacks()->ThreadStart(self);
    ArtField* unparkedField = WellKnownClasses::java_lang_Thread_unparkedBeforeStart;
    bool should_unpark = false;
    {
      art::MutexLock mu(soa.Self(), *art::Locks::thread_list_lock_);
      should_unpark = unparkedField->GetBoolean(self->tlsPtr_.opeer) == JNI_TRUE;
    }
    if (should_unpark) {
      self->Unpark();
    }
    ObjPtr<mirror::Object> receiver = self->tlsPtr_.opeer;
    WellKnownClasses::java_lang_Thread_run->InvokeVirtual<'V'>(self, receiver);
  }
  Runtime::Current()->GetThreadList()->Unregister(self, true);
  return nullptr;
}
Thread* Thread::FromManagedThread(const ScopedObjectAccessAlreadyRunnable& soa,
                                  ObjPtr<mirror::Object> thread_peer) {
  ArtField* f = WellKnownClasses::java_lang_Thread_nativePeer;
  Thread* result = reinterpret_cast64<Thread*>(f->GetLong(thread_peer));
  if (kIsDebugBuild) {
    MutexLock mu(soa.Self(), *Locks::thread_suspend_count_lock_);
    if (result != nullptr && !result->IsSuspended()) {
      Locks::thread_list_lock_->AssertHeld(soa.Self());
    }
  }
  return result;
}
Thread* Thread::FromManagedThread(const ScopedObjectAccessAlreadyRunnable& soa,
                                  jobject java_thread) {
  return FromManagedThread(soa, soa.Decode<mirror::Object>(java_thread));
}
static size_t FixStackSize(size_t stack_size) {
  if (stack_size == 0) {
    stack_size = Runtime::Current()->GetDefaultStackSize();
  }
  stack_size += 1 * MB;
  if (kMemoryToolIsAvailable) {
    stack_size = std::max(2 * MB, stack_size);
  }
  if (stack_size < PTHREAD_STACK_MIN) {
    stack_size = PTHREAD_STACK_MIN;
  }
  if (Runtime::Current()->GetImplicitStackOverflowChecks()) {
    stack_size += Thread::kStackOverflowImplicitCheckSize +
        GetStackOverflowReservedBytes(kRuntimeISA);
  } else {
    stack_size += GetStackOverflowReservedBytes(kRuntimeISA);
  }
  stack_size = RoundUp(stack_size, kPageSize);
  return stack_size;
}
static uint8_t* FindStackTop() {
  return reinterpret_cast<uint8_t*>(
      AlignDown(__builtin_frame_address(0), kPageSize));
}
void Thread::InstallImplicitProtection() {
  uint8_t* pregion = tlsPtr_.stack_begin - kStackOverflowProtectedSize;
  uint8_t* stack_top = FindStackTop();
  VLOG(threads) << "installing stack protected region at " << std::hex <<
        static_cast<void*>(pregion) << " to " <<
        static_cast<void*>(pregion + kStackOverflowProtectedSize - 1);
  if (ProtectStack( false)) {
    size_t unwanted_size =
        reinterpret_cast<uintptr_t>(stack_top) - reinterpret_cast<uintptr_t>(pregion) - kPageSize;
    madvise(pregion, unwanted_size, MADV_DONTNEED);
    return;
  }
  UnprotectStack();
  VLOG(threads) << "Need to map in stack for thread at " << std::hex <<
      static_cast<void*>(pregion);
  struct RecurseDownStack {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wframe-larger-than="
    NO_INLINE
    __attribute__((no_sanitize("memtag"))) static void Touch(uintptr_t target) {
      volatile size_t zero = 0;
      constexpr size_t kAsanMultiplier =
#ifdef ADDRESS_SANITIZER
          2u;
#else
          1u;
#endif
      volatile char space[kPageSize - (kAsanMultiplier * 256)] __attribute__((uninitialized));
      char sink ATTRIBUTE_UNUSED = space[zero];
      uintptr_t addr = reinterpret_cast<uintptr_t>(__hwasan_tag_pointer(space, 0));
      if (addr >= target + kPageSize) {
        Touch(target);
      }
      zero *= 2;
    }
#pragma GCC diagnostic pop
  };
  RecurseDownStack::Touch(reinterpret_cast<uintptr_t>(pregion));
  VLOG(threads) << "(again) installing stack protected region at " << std::hex <<
      static_cast<void*>(pregion) << " to " <<
      static_cast<void*>(pregion + kStackOverflowProtectedSize - 1);
  ProtectStack( true);
  size_t unwanted_size =
      reinterpret_cast<uintptr_t>(stack_top) - reinterpret_cast<uintptr_t>(pregion) - kPageSize;
  madvise(pregion, unwanted_size, MADV_DONTNEED);
}
void Thread::CreateNativeThread(JNIEnv* env, jobject java_peer, size_t stack_size, bool is_daemon) {
  CHECK(java_peer != nullptr);
  Thread* self = static_cast<JNIEnvExt*>(env)->GetSelf();
  if (VLOG_IS_ON(threads)) {
    ScopedObjectAccess soa(env);
    ArtField* f = WellKnownClasses::java_lang_Thread_name;
    ObjPtr<mirror::String> java_name =
        f->GetObject(soa.Decode<mirror::Object>(java_peer))->AsString();
    std::string thread_name;
    if (java_name != nullptr) {
      thread_name = java_name->ToModifiedUtf8();
    } else {
      thread_name = "(Unnamed)";
    }
    VLOG(threads) << "Creating native thread for " << thread_name;
    self->Dump(LOG_STREAM(INFO));
  }
  Runtime* runtime = Runtime::Current();
  bool thread_start_during_shutdown = false;
  {
    MutexLock mu(self, *Locks::runtime_shutdown_lock_);
    if (runtime->IsShuttingDownLocked()) {
      thread_start_during_shutdown = true;
    } else {
      runtime->StartThreadBirth();
    }
  }
  if (thread_start_during_shutdown) {
    ScopedLocalRef<jclass> error_class(env, env->FindClass("java/lang/InternalError"));
    env->ThrowNew(error_class.get(), "Thread starting during runtime shutdown");
    return;
  }
  Thread* child_thread = new Thread(is_daemon);
  child_thread->tlsPtr_.jpeer = env->NewGlobalRef(java_peer);
  stack_size = FixStackSize(stack_size);
  SetNativePeer(env, java_peer, child_thread);
  std::string error_msg;
  std::unique_ptr<JNIEnvExt> child_jni_env_ext(
      JNIEnvExt::Create(child_thread, Runtime::Current()->GetJavaVM(), &error_msg));
  int pthread_create_result = 0;
  if (child_jni_env_ext.get() != nullptr) {
    pthread_t new_pthread;
    pthread_attr_t attr;
    child_thread->tlsPtr_.tmp_jni_env = child_jni_env_ext.get();
    CHECK_PTHREAD_CALL(pthread_attr_init, (&attr), "new thread");
    CHECK_PTHREAD_CALL(pthread_attr_setdetachstate, (&attr, PTHREAD_CREATE_DETACHED),
                       "PTHREAD_CREATE_DETACHED");
    CHECK_PTHREAD_CALL(pthread_attr_setstacksize, (&attr, stack_size), stack_size);
    pthread_create_result = pthread_create(&new_pthread,
                                           &attr,
                                           gUseUserfaultfd ? Thread::CreateCallbackWithUffdGc
                                                           : Thread::CreateCallback,
                                           child_thread);
    CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), "new thread");
    if (pthread_create_result == 0) {
      child_jni_env_ext.release();
      return;
    }
  }
  {
    MutexLock mu(self, *Locks::runtime_shutdown_lock_);
    runtime->EndThreadBirth();
  }
  child_thread->DeleteJPeer(env);
  delete child_thread;
  child_thread = nullptr;
  SetNativePeer(env, java_peer, nullptr);
  {
    std::string msg(child_jni_env_ext.get() == nullptr ?
        StringPrintf("Could not allocate JNI Env: %s", error_msg.c_str()) :
        StringPrintf("pthread_create (%s stack) failed: %s",
                                 PrettySize(stack_size).c_str(), strerror(pthread_create_result)));
    ScopedObjectAccess soa(env);
    soa.Self()->ThrowOutOfMemoryError(msg.c_str());
  }
}
bool Thread::Init(ThreadList* thread_list, JavaVMExt* java_vm, JNIEnvExt* jni_env_ext) {
  CHECK(Thread::Current() == nullptr);
  tlsPtr_.pthread_self = pthread_self();
  CHECK(is_started_);
  ScopedTrace trace("Thread::Init");
  SetUpAlternateSignalStack();
  if (!InitStackHwm()) {
    return false;
  }
  InitCpu();
  InitTlsEntryPoints();
  RemoveSuspendTrigger();
  InitCardTable();
  InitTid();
#ifdef __BIONIC__
  __get_tls()[TLS_SLOT_ART_THREAD_SELF] = this;
#else
  CHECK_PTHREAD_CALL(pthread_setspecific, (Thread::pthread_key_self_, this), "attach self");
  Thread::self_tls_ = this;
#endif
  DCHECK_EQ(Thread::Current(), this);
  tls32_.thin_lock_thread_id = thread_list->AllocThreadId(this);
  if (jni_env_ext != nullptr) {
    DCHECK_EQ(jni_env_ext->GetVm(), java_vm);
    DCHECK_EQ(jni_env_ext->GetSelf(), this);
    tlsPtr_.jni_env = jni_env_ext;
  } else {
    std::string error_msg;
    tlsPtr_.jni_env = JNIEnvExt::Create(this, java_vm, &error_msg);
    if (tlsPtr_.jni_env == nullptr) {
      LOG(ERROR) << "Failed to create JNIEnvExt: " << error_msg;
      return false;
    }
  }
  ScopedTrace trace3("ThreadList::Register");
  thread_list->Register(this);
  return true;
}
Thread* Thread::Attach(const char* thread_name, bool as_daemon, jobject thread_peer) {
  auto set_peer_action = [&](Thread* self) {
    DCHECK(self == Thread::Current());
    ScopedObjectAccess soa(self);
    ObjPtr<mirror::Object> peer = soa.Decode<mirror::Object>(thread_peer);
    self->tlsPtr_.opeer = peer.Ptr();
    SetNativePeer< false>(peer, self);
    return true;
  };
  return Attach(thread_name, as_daemon, set_peer_action, true);
}
Thread* Thread::Attach(const char* thread_name, bool as_daemon, jobject thread_peer) {
  auto set_peer_action = [&](Thread* self) {
    DCHECK(self == Thread::Current());
    ScopedObjectAccess soa(self);
    ObjPtr<mirror::Object> peer = soa.Decode<mirror::Object>(thread_peer);
    self->tlsPtr_.opeer = peer.Ptr();
    SetNativePeer< false>(peer, self);
    return true;
  };
  return Attach(thread_name, as_daemon, set_peer_action, true);
}
Thread* Thread::Attach(const char* thread_name, bool as_daemon, jobject thread_peer) {
  auto set_peer_action = [&](Thread* self) {
    DCHECK(self == Thread::Current());
    ScopedObjectAccess soa(self);
    ObjPtr<mirror::Object> peer = soa.Decode<mirror::Object>(thread_peer);
    self->tlsPtr_.opeer = peer.Ptr();
    SetNativePeer< false>(peer, self);
    return true;
  };
  return Attach(thread_name, as_daemon, set_peer_action, true);
}
void Thread::CreatePeer(const char* name, bool as_daemon, jobject thread_group) {
  Runtime* runtime = Runtime::Current();
  CHECK(runtime->IsStarted());
  Thread* self = this;
  DCHECK_EQ(self, Thread::Current());
  ScopedObjectAccess soa(self);
  StackHandleScope<4u> hs(self);
  DCHECK(WellKnownClasses::java_lang_ThreadGroup->IsInitialized());
  Handle<mirror::Object> thr_group = hs.NewHandle(soa.Decode<mirror::Object>(
      thread_group != nullptr ? thread_group : runtime->GetMainThreadGroup()));
  Handle<mirror::String> thread_name = hs.NewHandle(
      name != nullptr ? mirror::String::AllocFromModifiedUtf8(self, name) : nullptr);
  if (name != nullptr && UNLIKELY(thread_name == nullptr)) {
    CHECK(self->IsExceptionPending());
    return;
  }
  jint thread_priority = GetNativePriority();
  DCHECK(WellKnownClasses::java_lang_Thread->IsInitialized());
  Handle<mirror::Object> peer =
      hs.NewHandle(WellKnownClasses::java_lang_Thread->AllocObject(self));
  if (UNLIKELY(peer == nullptr)) {
    CHECK(IsExceptionPending());
    return;
  }
  tlsPtr_.opeer = peer.Get();
  WellKnownClasses::java_lang_Thread_init->InvokeInstance<'V', 'L', 'L', 'I', 'Z'>(
      self, peer.Get(), thr_group.Get(), thread_name.Get(), thread_priority, as_daemon);
  if (self->IsExceptionPending()) {
    return;
  }
  SetNativePeer< false>(peer.Get(), self);
  MutableHandle<mirror::String> peer_thread_name(hs.NewHandle(GetThreadName()));
  if (peer_thread_name == nullptr) {
    if (runtime->IsActiveTransaction()) {
      InitPeer<true>(tlsPtr_.opeer,
                     as_daemon,
                     thr_group.Get(),
                     thread_name.Get(),
                     thread_priority);
    } else {
      InitPeer<false>(tlsPtr_.opeer,
                      as_daemon,
                      thr_group.Get(),
                      thread_name.Get(),
                      thread_priority);
    }
    peer_thread_name.Assign(GetThreadName());
  }
  if (peer_thread_name != nullptr) {
    SetThreadName(peer_thread_name->ToModifiedUtf8().c_str());
  }
}
ObjPtr<mirror::Object> Thread::CreateCompileTimePeer(const char* name,
                                                     bool as_daemon,
                                                     jobject thread_group) {
  Runtime* runtime = Runtime::Current();
  CHECK(!runtime->IsStarted());
  Thread* self = this;
  DCHECK_EQ(self, Thread::Current());
  ScopedObjectAccessUnchecked soa(self);
  StackHandleScope<3u> hs(self);
  DCHECK(WellKnownClasses::java_lang_ThreadGroup->IsInitialized());
  Handle<mirror::Object> thr_group = hs.NewHandle(soa.Decode<mirror::Object>(
      thread_group != nullptr ? thread_group : runtime->GetMainThreadGroup()));
  Handle<mirror::String> thread_name = hs.NewHandle(
      name != nullptr ? mirror::String::AllocFromModifiedUtf8(self, name) : nullptr);
  if (name != nullptr && UNLIKELY(thread_name == nullptr)) {
    CHECK(self->IsExceptionPending());
    return nullptr;
  }
  jint thread_priority = kNormThreadPriority;
  DCHECK(WellKnownClasses::java_lang_Thread->IsInitialized());
  Handle<mirror::Object> peer = hs.NewHandle(
      WellKnownClasses::java_lang_Thread->AllocObject(self));
  if (peer == nullptr) {
    CHECK(Thread::Current()->IsExceptionPending());
    return nullptr;
  }
  if (runtime->IsActiveTransaction()) {
    InitPeer<true>(peer.Get(),
                   as_daemon,
                   thr_group.Get(),
                   thread_name.Get(),
                   thread_priority);
  } else {
    InitPeer<false>(peer.Get(),
                    as_daemon,
                    thr_group.Get(),
                    thread_name.Get(),
                    thread_priority);
  }
  return peer.Get();
}
template<bool kTransactionActive>
void Thread::InitPeer(ObjPtr<mirror::Object> peer,
                      bool as_daemon,
                      ObjPtr<mirror::Object> thread_group,
                      ObjPtr<mirror::String> thread_name,
                      jint thread_priority) {
  WellKnownClasses::java_lang_Thread_daemon->SetBoolean<kTransactionActive>(peer,
      static_cast<uint8_t>(as_daemon ? 1u : 0u));
  WellKnownClasses::java_lang_Thread_group->SetObject<kTransactionActive>(peer, thread_group);
  WellKnownClasses::java_lang_Thread_name->SetObject<kTransactionActive>(peer, thread_name);
  WellKnownClasses::java_lang_Thread_priority->SetInt<kTransactionActive>(peer, thread_priority);
}
void Thread::SetCachedThreadName(const char* name) {
  DCHECK(name != kThreadNameDuringStartup);
  const char* old_name = tlsPtr_.name.exchange(name == nullptr ? nullptr : strdup(name));
  if (old_name != nullptr && old_name != kThreadNameDuringStartup) {
    for (uint32_t i = 0; UNLIKELY(tls32_.num_name_readers.load(std::memory_order_seq_cst) != 0);
         ++i) {
      static constexpr uint32_t kNumSpins = 1000;
      if (i > kNumSpins) {
        usleep(500);
      }
    }
    free(const_cast<char *>(old_name));
  }
}
void Thread::SetThreadName(const char* name) {
  SetCachedThreadName(name);
  ::art::SetThreadName(name);
  Dbg::DdmSendThreadNotification(this, CHUNK_TYPE("THNM"));
}
static void GetThreadStack(pthread_t thread,
                           void** stack_base,
                           size_t* stack_size,
                           size_t* guard_size) {
#if defined(__APPLE__)
  *stack_size = pthread_get_stacksize_np(thread);
  void* stack_addr = pthread_get_stackaddr_np(thread);
  int stack_variable;
  if (stack_addr > &stack_variable) {
    *stack_base = reinterpret_cast<uint8_t*>(stack_addr) - *stack_size;
  } else {
    *stack_base = stack_addr;
  }
  pthread_attr_t attributes;
  CHECK_PTHREAD_CALL(pthread_attr_init, (&attributes), __FUNCTION__);
  CHECK_PTHREAD_CALL(pthread_attr_getguardsize, (&attributes, guard_size), __FUNCTION__);
  CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attributes), __FUNCTION__);
#else
  pthread_attr_t attributes;
  CHECK_PTHREAD_CALL(pthread_getattr_np, (thread, &attributes), __FUNCTION__);
  CHECK_PTHREAD_CALL(pthread_attr_getstack, (&attributes, stack_base, stack_size), __FUNCTION__);
  CHECK_PTHREAD_CALL(pthread_attr_getguardsize, (&attributes, guard_size), __FUNCTION__);
  CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attributes), __FUNCTION__);
#if defined(__GLIBC__)
  bool is_main_thread = (::art::GetTid() == static_cast<uint32_t>(getpid()));
  if (is_main_thread) {
    rlimit stack_limit;
    if (getrlimit(RLIMIT_STACK, &stack_limit) == -1) {
      PLOG(FATAL) << "getrlimit(RLIMIT_STACK) failed";
    }
    if (stack_limit.rlim_cur == RLIM_INFINITY) {
      size_t old_stack_size = *stack_size;
      *stack_size = 8 * MB;
      *stack_base = reinterpret_cast<uint8_t*>(*stack_base) + (old_stack_size - *stack_size);
      VLOG(threads) << "Limiting unlimited stack (reported as " << PrettySize(old_stack_size) << ")"
                    << " to " << PrettySize(*stack_size)
                    << " with base " << *stack_base;
    }
  }
#endif
#endif
}
bool Thread::InitStackHwm() {
  ScopedTrace trace("InitStackHwm");
  void* read_stack_base;
  size_t read_stack_size;
  size_t read_guard_size;
  GetThreadStack(tlsPtr_.pthread_self, &read_stack_base, &read_stack_size, &read_guard_size);
  tlsPtr_.stack_begin = reinterpret_cast<uint8_t*>(read_stack_base);
  tlsPtr_.stack_size = read_stack_size;
  uint32_t min_stack = GetStackOverflowReservedBytes(kRuntimeISA) + kStackOverflowProtectedSize
    + 4 * KB;
  if (read_stack_size <= min_stack) {
    LogHelper::LogLineLowStack(__PRETTY_FUNCTION__,
                               __LINE__,
                               ::android::base::ERROR,
                               "Attempt to attach a thread with a too-small stack");
    return false;
  }
  VLOG(threads) << StringPrintf("Native stack is at %p (%s with %s guard)",
                                read_stack_base,
                                PrettySize(read_stack_size).c_str(),
                                PrettySize(read_guard_size).c_str());
  Runtime* runtime = Runtime::Current();
  bool implicit_stack_check =
      runtime->GetImplicitStackOverflowChecks() && !runtime->IsAotCompiler();
  ResetDefaultStackEnd();
  if (implicit_stack_check) {
    tlsPtr_.stack_begin += read_guard_size + kStackOverflowProtectedSize;
    tlsPtr_.stack_end += read_guard_size + kStackOverflowProtectedSize;
    tlsPtr_.stack_size -= read_guard_size + kStackOverflowProtectedSize;
    InstallImplicitProtection();
  }
  CHECK_GT(FindStackTop(), reinterpret_cast<void*>(tlsPtr_.stack_end));
  return true;
}
void Thread::ShortDump(std::ostream& os) const {
  os << "Thread[";
  if (GetThreadId() != 0) {
    os << GetThreadId()
       << ",tid=" << GetTid() << ',';
  }
  tls32_.num_name_readers.fetch_add(1, std::memory_order_seq_cst);
  const char* name = tlsPtr_.name.load();
  os << GetState()
     << ",Thread*=" << this
     << ",peer=" << tlsPtr_.opeer
     << ",\"" << (name == nullptr ? "null" : name) << "\""
     << "]";
  tls32_.num_name_readers.fetch_sub(1 );
}
Thread::DumpOrder Thread::Dump(std::ostream& os,
                               unwindstack::AndroidLocalUnwinder& unwinder,
                               bool dump_native_stack,
                               bool force_dump_stack) const {
  DumpState(os);
  return DumpStack(os, unwinder, dump_native_stack, force_dump_stack);
}
void Thread::GetThreadName(std::string& name) const {
  tls32_.num_name_readers.fetch_add(1, std::memory_order_seq_cst);
  name.assign(tlsPtr_.name.load(std::memory_order_seq_cst));
  tls32_.num_name_readers.fetch_sub(1 );
}
void Thread::GetThreadName(std::string& name) const {
  tls32_.num_name_readers.fetch_add(1, std::memory_order_seq_cst);
  name.assign(tlsPtr_.name.load(std::memory_order_seq_cst));
  tls32_.num_name_readers.fetch_sub(1 );
}
uint64_t Thread::GetCpuMicroTime() const {
#if defined(__linux__)
  clockid_t cpu_clock_id;
  pthread_getcpuclockid(tlsPtr_.pthread_self, &cpu_clock_id);
  timespec now;
  clock_gettime(cpu_clock_id, &now);
  return static_cast<uint64_t>(now.tv_sec) * UINT64_C(1000000) +
         static_cast<uint64_t>(now.tv_nsec) / UINT64_C(1000);
#else
  UNIMPLEMENTED(WARNING);
  return -1;
#endif
}
static void UnsafeLogFatalForSuspendCount(Thread* self, Thread* thread) NO_THREAD_SAFETY_ANALYSIS {
  LOG(ERROR) << *thread << " suspend count already zero.";
  Locks::thread_suspend_count_lock_->Unlock(self);
  if (!Locks::mutator_lock_->IsSharedHeld(self)) {
    Locks::mutator_lock_->SharedTryLock(self);
    if (!Locks::mutator_lock_->IsSharedHeld(self)) {
      LOG(WARNING) << "Dumping thread list without holding mutator_lock_";
    }
  }
  if (!Locks::thread_list_lock_->IsExclusiveHeld(self)) {
    Locks::thread_list_lock_->TryLock(self);
    if (!Locks::thread_list_lock_->IsExclusiveHeld(self)) {
      LOG(WARNING) << "Dumping thread list without holding thread_list_lock_";
    }
  }
  std::ostringstream ss;
  Runtime::Current()->GetThreadList()->Dump(ss);
  LOG(FATAL) << ss.str();
}
bool Thread::ModifySuspendCountInternal(Thread* self,
                                        int delta,
                                        AtomicInteger* suspend_barrier,
                                        SuspendReason reason) {
  if (kIsDebugBuild) {
    DCHECK(delta == -1 || delta == +1)
          << reason << " " << delta << " " << this;
    Locks::thread_suspend_count_lock_->AssertHeld(self);
    if (this != self && !IsSuspended()) {
      Locks::thread_list_lock_->AssertHeld(self);
    }
  }
  if (UNLIKELY(reason == SuspendReason::kForUserCode)) {
    Locks::user_code_suspension_lock_->AssertHeld(self);
    if (UNLIKELY(delta + tls32_.user_code_suspend_count < 0)) {
      LOG(ERROR) << "attempting to modify suspend count in an illegal way.";
      return false;
    }
  }
  if (UNLIKELY(delta < 0 && tls32_.suspend_count <= 0)) {
    UnsafeLogFatalForSuspendCount(self, this);
    return false;
  }
  if (delta > 0 && this != self && tlsPtr_.flip_function != nullptr) {
    return false;
  }
  uint32_t flags = enum_cast<uint32_t>(ThreadFlag::kSuspendRequest);
  if (delta > 0 && suspend_barrier != nullptr) {
    uint32_t available_barrier = kMaxSuspendBarriers;
    for (uint32_t i = 0; i < kMaxSuspendBarriers; ++i) {
      if (tlsPtr_.active_suspend_barriers[i] == nullptr) {
        available_barrier = i;
        break;
      }
    }
    if (available_barrier == kMaxSuspendBarriers) {
      return false;
    }
    tlsPtr_.active_suspend_barriers[available_barrier] = suspend_barrier;
    flags |= enum_cast<uint32_t>(ThreadFlag::kActiveSuspendBarrier);
  }
  tls32_.suspend_count += delta;
  switch (reason) {
    case SuspendReason::kForUserCode:
      tls32_.user_code_suspend_count += delta;
      break;
    case SuspendReason::kInternal:
      break;
  }
  if (tls32_.suspend_count == 0) {
    AtomicClearFlag(ThreadFlag::kSuspendRequest);
  } else {
    tls32_.state_and_flags.fetch_or(flags, std::memory_order_seq_cst);
    TriggerSuspend();
  }
  return true;
}
bool Thread::PassActiveSuspendBarriers(Thread* self) {
  AtomicInteger* pass_barriers[kMaxSuspendBarriers];
  {
    MutexLock mu(self, *Locks::thread_suspend_count_lock_);
    if (!ReadFlag(ThreadFlag::kActiveSuspendBarrier)) {
      return false;
    }
    for (uint32_t i = 0; i < kMaxSuspendBarriers; ++i) {
      pass_barriers[i] = tlsPtr_.active_suspend_barriers[i];
      tlsPtr_.active_suspend_barriers[i] = nullptr;
    }
    AtomicClearFlag(ThreadFlag::kActiveSuspendBarrier);
  }
  uint32_t barrier_count = 0;
  for (uint32_t i = 0; i < kMaxSuspendBarriers; i++) {
    AtomicInteger* pending_threads = pass_barriers[i];
    if (pending_threads != nullptr) {
      bool done = false;
      do {
        int32_t cur_val = pending_threads->load(std::memory_order_relaxed);
        CHECK_GT(cur_val, 0) << "Unexpected value for PassActiveSuspendBarriers(): " << cur_val;
        done = pending_threads->CompareAndSetWeakRelaxed(cur_val, cur_val - 1);
#if ART_USE_FUTEXES
        if (done && (cur_val - 1) == 0) {
          futex(pending_threads->Address(), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
        }
#endif
      } while (!done);
      ++barrier_count;
    }
  }
  CHECK_GT(barrier_count, 0U);
  return true;
}
void Thread::ClearSuspendBarrier(AtomicInteger* target) {
  CHECK(ReadFlag(ThreadFlag::kActiveSuspendBarrier));
  bool clear_flag = true;
  for (uint32_t i = 0; i < kMaxSuspendBarriers; ++i) {
    AtomicInteger* ptr = tlsPtr_.active_suspend_barriers[i];
    if (ptr == target) {
      tlsPtr_.active_suspend_barriers[i] = nullptr;
    } else if (ptr != nullptr) {
      clear_flag = false;
    }
  }
  if (LIKELY(clear_flag)) {
    AtomicClearFlag(ThreadFlag::kActiveSuspendBarrier);
  }
}
void Thread::RunCheckpointFunction() {
  StateAndFlags state_and_flags = GetStateAndFlags(std::memory_order_relaxed);
  if (UNLIKELY(state_and_flags.IsAnyOfFlagsSet(FlipFunctionFlags()))) {
    DCHECK(IsSuspended());
    Thread* self = Thread::Current();
    DCHECK(self != this);
    if (state_and_flags.IsFlagSet(ThreadFlag::kPendingFlipFunction)) {
      EnsureFlipFunctionStarted(self);
      state_and_flags = GetStateAndFlags(std::memory_order_relaxed);
      DCHECK(!state_and_flags.IsFlagSet(ThreadFlag::kPendingFlipFunction));
    }
    if (state_and_flags.IsFlagSet(ThreadFlag::kRunningFlipFunction)) {
      WaitForFlipFunction(self);
    }
  }
  Closure* checkpoint;
  {
    MutexLock mu(this, *Locks::thread_suspend_count_lock_);
    checkpoint = tlsPtr_.checkpoint_function;
    if (!checkpoint_overflow_.empty()) {
      tlsPtr_.checkpoint_function = checkpoint_overflow_.front();
      checkpoint_overflow_.pop_front();
    } else {
      tlsPtr_.checkpoint_function = nullptr;
      AtomicClearFlag(ThreadFlag::kCheckpointRequest);
    }
  }
  ScopedTrace trace("Run checkpoint function");
  CHECK(checkpoint != nullptr) << "Checkpoint flag set without pending checkpoint";
  checkpoint->Run(this);
}
void Thread::RunEmptyCheckpoint() {
  DCHECK_EQ(Thread::Current(), this);
  AtomicClearFlag(ThreadFlag::kEmptyCheckpointRequest);
  Runtime::Current()->GetThreadList()->EmptyCheckpointBarrier()->Pass(this);
}
bool Thread::RequestCheckpoint(Closure* function) {
  StateAndFlags old_state_and_flags = GetStateAndFlags(std::memory_order_relaxed);
  if (old_state_and_flags.GetState() != ThreadState::kRunnable) {
    return false;
  }
  DCHECK_EQ(old_state_and_flags.GetState(), ThreadState::kRunnable);
  StateAndFlags new_state_and_flags = old_state_and_flags;
  new_state_and_flags.SetFlag(ThreadFlag::kCheckpointRequest);
  bool success = tls32_.state_and_flags.CompareAndSetStrongSequentiallyConsistent(
      old_state_and_flags.GetValue(), new_state_and_flags.GetValue());
  if (success) {
    if (tlsPtr_.checkpoint_function == nullptr) {
      tlsPtr_.checkpoint_function = function;
    } else {
      checkpoint_overflow_.push_back(function);
    }
    CHECK(ReadFlag(ThreadFlag::kCheckpointRequest));
    TriggerSuspend();
  }
  return success;
}
bool Thread::RequestEmptyCheckpoint() {
  StateAndFlags old_state_and_flags = GetStateAndFlags(std::memory_order_relaxed);
  if (old_state_and_flags.GetState() != ThreadState::kRunnable) {
    return false;
  }
  DCHECK_EQ(old_state_and_flags.GetState(), ThreadState::kRunnable);
  StateAndFlags new_state_and_flags = old_state_and_flags;
  new_state_and_flags.SetFlag(ThreadFlag::kEmptyCheckpointRequest);
  bool success = tls32_.state_and_flags.CompareAndSetStrongSequentiallyConsistent(
      old_state_and_flags.GetValue(), new_state_and_flags.GetValue());
  if (success) {
    TriggerSuspend();
  }
  return success;
}
class BarrierClosure : public Closure {
public:
  explicit BarrierClosure(Closure* wrapped) : wrapped_(wrapped), barrier_(0) {}
  void Run(Thread* self) override {
    wrapped_->Run(self);
    barrier_.Pass(self);
  }
  void Wait(Thread* self, ThreadState suspend_state) {
    if (suspend_state != ThreadState::kRunnable) {
      barrier_.Increment<Barrier::kDisallowHoldingLocks>(self, 1);
    } else {
      barrier_.Increment<Barrier::kAllowHoldingLocks>(self, 1);
    }
  }
private:
  Closure* wrapped_;
  Barrier barrier_;
};
bool Thread::RequestSynchronousCheckpoint(Closure* function, ThreadState suspend_state) {
  Thread* self = Thread::Current();
  if (this == Thread::Current()) {
    Locks::thread_list_lock_->AssertExclusiveHeld(self);
    Locks::thread_list_lock_->ExclusiveUnlock(self);
    function->Run(this);
    return true;
  }
  if (GetState() == ThreadState::kTerminated) {
    Locks::thread_list_lock_->ExclusiveUnlock(self);
    return false;
  }
  struct ScopedThreadListLockUnlock {
    explicit ScopedThreadListLockUnlock(Thread* self_in) RELEASE(*Locks::thread_list_lock_)
        : self_thread(self_in) {
      Locks::thread_list_lock_->AssertHeld(self_thread);
      Locks::thread_list_lock_->Unlock(self_thread);
    }
    ~ScopedThreadListLockUnlock() ACQUIRE(*Locks::thread_list_lock_) {
      Locks::thread_list_lock_->AssertNotHeld(self_thread);
      Locks::thread_list_lock_->Lock(self_thread);
    }
    Thread* self_thread;
  };
  for (;;) {
    Locks::thread_list_lock_->AssertExclusiveHeld(self);
    if (GetState() == ThreadState::kRunnable) {
      BarrierClosure barrier_closure(function);
      bool installed = false;
      {
        MutexLock mu(self, *Locks::thread_suspend_count_lock_);
        installed = RequestCheckpoint(&barrier_closure);
      }
      if (installed) {
        Locks::thread_list_lock_->ExclusiveUnlock(self);
        ScopedThreadStateChange sts(self, suspend_state);
        barrier_closure.Wait(self, suspend_state);
        return true;
      }
    }
    {
      MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
      if (!ModifySuspendCount(self, +1, nullptr, SuspendReason::kInternal)) {
        sched_yield();
        continue;
      }
    }
    {
      ScopedThreadListLockUnlock stllu(self);
      {
        ScopedThreadStateChange sts(self, suspend_state);
        while (GetState() == ThreadState::kRunnable) {
          sched_yield();
        }
      }
      function->Run(this);
    }
    {
      MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
      DCHECK_NE(GetState(), ThreadState::kRunnable);
      bool updated = ModifySuspendCount(self, -1, nullptr, SuspendReason::kInternal);
      DCHECK(updated);
    }
    {
      MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
      Thread::resume_cond_->Broadcast(self);
    }
    Locks::thread_list_lock_->ExclusiveUnlock(self);
    return true;
  }
}
void Thread::SetFlipFunction(Closure* function) {
  DCHECK(IsSuspended() || Thread::Current() == this);
  DCHECK(function != nullptr);
  DCHECK(tlsPtr_.flip_function == nullptr);
  tlsPtr_.flip_function = function;
  DCHECK(!GetStateAndFlags(std::memory_order_relaxed).IsAnyOfFlagsSet(FlipFunctionFlags()));
  AtomicSetFlag(ThreadFlag::kPendingFlipFunction, std::memory_order_release);
}
void Thread::EnsureFlipFunctionStarted(Thread* self) {
  while (true) {
    StateAndFlags old_state_and_flags = GetStateAndFlags(std::memory_order_relaxed);
    if (!old_state_and_flags.IsFlagSet(ThreadFlag::kPendingFlipFunction)) {
      return;
    }
    DCHECK(!old_state_and_flags.IsFlagSet(ThreadFlag::kRunningFlipFunction));
    StateAndFlags new_state_and_flags =
        old_state_and_flags.WithFlag(ThreadFlag::kRunningFlipFunction)
                           .WithoutFlag(ThreadFlag::kPendingFlipFunction);
    if (tls32_.state_and_flags.CompareAndSetWeakAcquire(old_state_and_flags.GetValue(),
                                                        new_state_and_flags.GetValue())) {
      RunFlipFunction(self, true);
      DCHECK(!GetStateAndFlags(std::memory_order_relaxed).IsAnyOfFlagsSet(FlipFunctionFlags()));
      return;
    }
  }
}
void Thread::RunFlipFunction(Thread* self, bool notify) {
  DCHECK_EQ(notify, ReadFlag(ThreadFlag::kRunningFlipFunction));
  Closure* flip_function = tlsPtr_.flip_function;
  tlsPtr_.flip_function = nullptr;
  DCHECK(flip_function != nullptr);
  flip_function->Run(this);
  if (notify) {
    constexpr uint32_t kFlagsToClear = enum_cast<uint32_t>(ThreadFlag::kRunningFlipFunction) |
                                       enum_cast<uint32_t>(ThreadFlag::kWaitingForFlipFunction);
    StateAndFlags old_state_and_flags(
        tls32_.state_and_flags.fetch_and(~kFlagsToClear, std::memory_order_release));
    if (old_state_and_flags.IsFlagSet(ThreadFlag::kWaitingForFlipFunction)) {
      MutexLock mu(self, *Locks::thread_suspend_count_lock_);
      resume_cond_->Broadcast(self);
    }
  }
}
void Thread::WaitForFlipFunction(Thread* self) {
  MutexLock mu(self, *Locks::thread_suspend_count_lock_);
  while (true) {
    StateAndFlags old_state_and_flags = GetStateAndFlags(std::memory_order_acquire);
    DCHECK(!old_state_and_flags.IsFlagSet(ThreadFlag::kPendingFlipFunction));
    if (!old_state_and_flags.IsFlagSet(ThreadFlag::kRunningFlipFunction)) {
      DCHECK(!old_state_and_flags.IsAnyOfFlagsSet(FlipFunctionFlags()));
      break;
    }
    if (!old_state_and_flags.IsFlagSet(ThreadFlag::kWaitingForFlipFunction)) {
      StateAndFlags new_state_and_flags =
          old_state_and_flags.WithFlag(ThreadFlag::kWaitingForFlipFunction);
      if (!tls32_.state_and_flags.CompareAndSetWeakRelaxed(old_state_and_flags.GetValue(),
                                                           new_state_and_flags.GetValue())) {
        continue;
      }
    }
    resume_cond_->Wait(self);
  }
}
void Thread::FullSuspendCheck(bool implicit) {
  ScopedTrace trace(__FUNCTION__);
  VLOG(threads) << this << " self-suspending";
  ScopedThreadSuspension(this, ThreadState::kSuspended);
  if (implicit) {
    MadviseAwayAlternateSignalStack();
  }
  VLOG(threads) << this << " self-reviving";
}
static std::string GetSchedulerGroupName(pid_t tid) {
  std::string cgroup_file;
  if (!android::base::ReadFileToString(StringPrintf("/proc/self/task/%d/cgroup", tid),
                                       &cgroup_file)) {
    return "";
  }
  std::vector<std::string> cgroup_lines;
  Split(cgroup_file, '\n', &cgroup_lines);
  for (size_t i = 0; i < cgroup_lines.size(); ++i) {
    std::vector<std::string> cgroup_fields;
    Split(cgroup_lines[i], ':', &cgroup_fields);
    std::vector<std::string> cgroups;
    Split(cgroup_fields[1], ',', &cgroups);
    for (size_t j = 0; j < cgroups.size(); ++j) {
      if (cgroups[j] == "cpu") {
        return cgroup_fields[2].substr(1);
      }
    }
  }
  return "";
}
void Thread::DumpState(std::ostream& os, const Thread* thread, pid_t tid) {
  std::string group_name;
  int priority;
  bool is_daemon = false;
  Thread* self = Thread::Current();
  if (gAborting == 0 && self != nullptr && thread != nullptr && thread->tlsPtr_.opeer != nullptr) {
    ScopedObjectAccessUnchecked soa(self);
    priority = WellKnownClasses::java_lang_Thread_priority->GetInt(thread->tlsPtr_.opeer);
    is_daemon = WellKnownClasses::java_lang_Thread_daemon->GetBoolean(thread->tlsPtr_.opeer);
    ObjPtr<mirror::Object> thread_group =
        WellKnownClasses::java_lang_Thread_group->GetObject(thread->tlsPtr_.opeer);
    if (thread_group != nullptr) {
      ObjPtr<mirror::Object> group_name_object =
          WellKnownClasses::java_lang_ThreadGroup_name->GetObject(thread_group);
      group_name = (group_name_object != nullptr)
          ? group_name_object->AsString()->ToModifiedUtf8()
          : "<null>";
    }
  } else if (thread != nullptr) {
    priority = thread->GetNativePriority();
  } else {
    palette_status_t status = PaletteSchedGetPriority(tid, &priority);
    CHECK(status == PALETTE_STATUS_OK || status == PALETTE_STATUS_CHECK_ERRNO);
  }
  std::string scheduler_group_name(GetSchedulerGroupName(tid));
  if (scheduler_group_name.empty()) {
    scheduler_group_name = "default";
  }
  if (thread != nullptr) {
    thread->tls32_.num_name_readers.fetch_add(1, std::memory_order_seq_cst);
    os << '"' << thread->tlsPtr_.name.load() << '"';
    thread->tls32_.num_name_readers.fetch_sub(1 );
    if (is_daemon) {
      os << " daemon";
    }
    os << " prio=" << priority
       << " tid=" << thread->GetThreadId()
       << " " << thread->GetState();
    if (thread->IsStillStarting()) {
      os << " (still starting up)";
    }
    if (thread->tls32_.disable_thread_flip_count != 0) {
      os << " DisableFlipCount = " << thread->tls32_.disable_thread_flip_count;
    }
    os << "\n";
  } else {
    os << '"' << ::art::GetThreadName(tid) << '"'
       << " prio=" << priority
       << " (not attached)\n";
  }
  if (thread != nullptr) {
    auto suspend_log_fn = [&]() REQUIRES(Locks::thread_suspend_count_lock_) {
      StateAndFlags state_and_flags = thread->GetStateAndFlags(std::memory_order_relaxed);
      static_assert(
          static_cast<std::underlying_type_t<ThreadState>>(ThreadState::kRunnable) == 0u);
      state_and_flags.SetState(ThreadState::kRunnable);
      os << "  | group=\"" << group_name << "\""
         << " sCount=" << thread->tls32_.suspend_count
         << " ucsCount=" << thread->tls32_.user_code_suspend_count
         << " flags=" << state_and_flags.GetValue()
         << " obj=" << reinterpret_cast<void*>(thread->tlsPtr_.opeer)
         << " self=" << reinterpret_cast<const void*>(thread) << "\n";
    };
    if (Locks::thread_suspend_count_lock_->IsExclusiveHeld(self)) {
      Locks::thread_suspend_count_lock_->AssertExclusiveHeld(self);
      suspend_log_fn();
    } else {
      MutexLock mu(self, *Locks::thread_suspend_count_lock_);
      suspend_log_fn();
    }
  }
  os << "  | sysTid=" << tid
     << " nice=" << getpriority(PRIO_PROCESS, static_cast<id_t>(tid))
     << " cgrp=" << scheduler_group_name;
  if (thread != nullptr) {
    int policy;
    sched_param sp;
#if !defined(__APPLE__)
    policy = sched_getscheduler(tid);
    if (policy == -1) {
      PLOG(WARNING) << "sched_getscheduler(" << tid << ")";
    }
    int sched_getparam_result = sched_getparam(tid, &sp);
    if (sched_getparam_result == -1) {
      PLOG(WARNING) << "sched_getparam(" << tid << ", &sp)";
      sp.sched_priority = -1;
    }
#else
    CHECK_PTHREAD_CALL(pthread_getschedparam, (thread->tlsPtr_.pthread_self, &policy, &sp),
                       __FUNCTION__);
#endif
    os << " sched=" << policy << "/" << sp.sched_priority
       << " handle=" << reinterpret_cast<void*>(thread->tlsPtr_.pthread_self);
  }
  os << "\n";
  std::string scheduler_stats;
  if (android::base::ReadFileToString(StringPrintf("/proc/self/task/%d/schedstat", tid),
                                      &scheduler_stats)
      && !scheduler_stats.empty()) {
    scheduler_stats = android::base::Trim(scheduler_stats);
  } else {
    scheduler_stats = "0 0 0";
  }
  char native_thread_state = '?';
  int utime = 0;
  int stime = 0;
  int task_cpu = 0;
  GetTaskStats(tid, &native_thread_state, &utime, &stime, &task_cpu);
  os << "  | state=" << native_thread_state
     << " schedstat=( " << scheduler_stats << " )"
     << " utm=" << utime
     << " stm=" << stime
     << " core=" << task_cpu
     << " HZ=" << sysconf(_SC_CLK_TCK) << "\n";
  if (thread != nullptr) {
    os << "  | stack=" << reinterpret_cast<void*>(thread->tlsPtr_.stack_begin) << "-"
        << reinterpret_cast<void*>(thread->tlsPtr_.stack_end) << " stackSize="
        << PrettySize(thread->tlsPtr_.stack_size) << "\n";
    os << "  | held mutexes=";
    for (size_t i = 0; i < kLockLevelCount; ++i) {
      if (i != kMonitorLock) {
        BaseMutex* mutex = thread->GetHeldMutex(static_cast<LockLevel>(i));
        if (mutex != nullptr) {
          os << " \"" << mutex->GetName() << "\"";
          if (mutex->IsReaderWriterMutex()) {
            ReaderWriterMutex* rw_mutex = down_cast<ReaderWriterMutex*>(mutex);
            if (rw_mutex->GetExclusiveOwnerTid() == tid) {
              os << "(exclusive held)";
            } else {
              os << "(shared held)";
            }
          }
        }
      }
    }
    os << "\n";
  }
}
void Thread::DumpState(std::ostream& os) const {
  Thread::DumpState(os, this, GetTid()){
  Thread::DumpState(os, this, GetTid());
}
struct StackDumpVisitor : public MonitorObjectsStackVisitor {
  StackDumpVisitor(std::ostream& os_in,
                   Thread* thread_in,
                   Context* context,
                   bool can_allocate,
                   bool check_suspended = true,
                   bool dump_locks = true)
      REQUIRES_SHARED(Locks::mutator_lock_)
      : MonitorObjectsStackVisitor(thread_in,
                                   context,
                                   check_suspended,
                                   can_allocate && dump_locks),
        os(os_in),
        last_method(nullptr),
        last_line_number(0),
        repetition_count(0) {}
  virtual ~StackDumpVisitor() {
    if (frame_count == 0) {
      os << "  (no managed stack frames)\n";
    }
  }
  static constexpr size_t kMaxRepetition = 3u;
  VisitMethodResult StartMethod(ArtMethod* m, size_t frame_nr ATTRIBUTE_UNUSED)
      override
      REQUIRES_SHARED(Locks::mutator_lock_) {
      m = m->GetInterfaceMethodIfProxy(kRuntimePointerSize);
      ObjPtr<mirror::DexCache> dex_cache = m->GetDexCache();
      int line_number = -1;
      uint32_t dex_pc = GetDexPc(false);
      if (dex_cache != nullptr) {
      const DexFile* dex_file = dex_cache->GetDexFile();
      line_number = annotations::GetLineNumFromPC(dex_file, m, dex_pc);
      }
      if (line_number == last_line_number && last_method == m) {
      ++repetition_count;
      } else {
      if (repetition_count >= kMaxRepetition) {
        os << "  ... repeated " << (repetition_count - kMaxRepetition) << " times\n";
      }
      repetition_count = 0;
      last_line_number = line_number;
      last_method = m;
      }
      if (repetition_count >= kMaxRepetition) {
      return VisitMethodResult::kSkipMethod;
      }
      os << "  at " << m->PrettyMethod(false);
      if (m->IsNative()) {
      os << "(Native method)";
      } else {
      const char* source_file(m->GetDeclaringClassSourceFile());
      if (line_number == -1) {
        source_file = nullptr;
        line_number = static_cast<int32_t>(dex_pc);
      }
      os << "(" << (source_file != nullptr ? source_file : "unavailable")
                       << ":" << line_number << ")";
      }
      os << "\n";
      return VisitMethodResult::kContinueMethod;
      }
  VisitMethodResult EndMethod(ArtMethod* m ATTRIBUTE_UNUSED) override {
    return VisitMethodResult::kContinueMethod;
  }
  void VisitWaitingObject(ObjPtr<mirror::Object> obj, ThreadState state ATTRIBUTE_UNUSED)
      override
      REQUIRES_SHARED(Locks::mutator_lock_) {
      PrintObject(obj, "  - waiting on ", ThreadList::kInvalidThreadId);
      }
  void VisitSleepingObject(ObjPtr<mirror::Object> obj)
      override
      REQUIRES_SHARED(Locks::mutator_lock_) {
      PrintObject(obj, "  - sleeping on ", ThreadList::kInvalidThreadId);
      }
  void VisitBlockedOnObject(ObjPtr<mirror::Object> obj,
                            ThreadState state,
                            uint32_t owner_tid)
      override
      REQUIRES_SHARED(Locks::mutator_lock_) {
      const char* msg;
      switch (state) {
      case ThreadState::kBlocked:
        msg {
      const char* msg;
      switch (state) {
      case ThreadState::kBlocked:
        msg = "  - waiting to lock ";
        break;
      case ThreadState::kWaitingForLockInflation:
        msg = "  - waiting for lock inflation of ";
        break;
      default:
        LOG(FATAL) << "Unreachable";
        UNREACHABLE();
      }
      PrintObject(obj, msg, owner_tid);
      num_blocked++;
      }
  void VisitLockedObject(ObjPtr<mirror::Object> obj)
      override
      REQUIRES_SHARED(Locks::mutator_lock_) {
      PrintObject(obj, "  - locked ", ThreadList::kInvalidThreadId);
      num_locked++;
      }
void PrintObject(ObjPtr<mirror::Object> obj,
                   const char* msg,
                   uint32_t owner_tid) REQUIRES_SHARED(Locks::mutator_lock_) {
                                       if (obj == nullptr) {
                                       os << msg << "an unknown object";
                                       } else {
                                       if ((obj->GetLockWord(true).GetState() == LockWord::kThinLocked) &&
                                       Locks::mutator_lock_->IsExclusiveHeld(Thread::Current())) {
                                       os << msg << StringPrintf("<@addr=0x%" PRIxPTR "> (a %s)",
                                       reinterpret_cast<intptr_t>(obj.Ptr()),
                                       obj->PrettyTypeOf().c_str());
                                       } else {
                                       const std::string pretty_type(obj->PrettyTypeOf());
                                       os << msg << StringPrintf("<0x%08x> (a %s)", obj->IdentityHashCode(), pretty_type.c_str());
                                       }
                                       }
                                       if (owner_tid != ThreadList::kInvalidThreadId) {
                                       os << " held by thread " << owner_tid;
                                       }
                                       os << "\n";
                                       }
  std::ostream& os;
  ArtMethod* last_method;
  int last_line_number;
  size_t repetition_count;
  size_t num_blocked = 0;
  size_t num_locked = 0;
};
static bool ShouldShowNativeStack(const Thread* thread)
    REQUIRES_SHARED(Locks::mutator_lock_) {
    ThreadState state = thread->GetState();
    if (state > ThreadState::kWaiting && state < ThreadState::kStarting) {
    return true;
    }
    if (state == ThreadState::kTimedWaiting ||
      state == ThreadState::kSleeping ||
      state == ThreadState::kWaiting) {
    return false;
    }
    if (!thread->HasManagedStack()) {
    return true;
    }
    ArtMethod* current_method = thread->GetCurrentMethod(nullptr);
    return current_method != nullptr && current_method->IsNative();
    }
Thread::DumpOrder Thread::DumpJavaStack(std::ostream& os,
                                        bool check_suspended,
                                        bool dump_locks) const {
  ScopedExceptionStorage ses(Thread::Current());
  std::unique_ptr<Context> context(Context::Create());
  StackDumpVisitor dumper(os, const_cast<Thread*>(this), context.get(),
                          !tls32_.throwing_OutOfMemoryError, check_suspended, dump_locks);
  dumper.WalkStack();
  if (IsJitSensitiveThread()) {
    return DumpOrder::kMain;
  } else if (dumper.num_blocked > 0) {
    return DumpOrder::kBlocked;
  } else if (dumper.num_locked > 0) {
    return DumpOrder::kLocked;
  } else {
    return DumpOrder::kDefault;
  }
}
Thread::DumpOrder Thread::DumpStack(std::ostream& os, bool dump_native_stack, bool force_dump_stack) const {
  unwindstack::AndroidLocalUnwinder unwinder;
  return DumpStack(os, unwinder, dump_native_stack, force_dump_stack);
}
Thread::DumpOrder Thread::DumpStack(std::ostream& os, unwindstack::AndroidLocalUnwinder& unwinder, bool dump_native_stack, bool force_dump_stack) const {
  bool dump_for_abort = (gAborting > 0);
  bool safe_to_dump = (this == Thread::Current() || IsSuspended());
  if (!kIsDebugBuild) {
    safe_to_dump = (safe_to_dump || dump_for_abort);
  }
  DumpOrder dump_order = DumpOrder::kDefault;
  if (safe_to_dump || force_dump_stack) {
    if (dump_native_stack && (dump_for_abort || force_dump_stack || ShouldShowNativeStack(this))) {
      ArtMethod* method =
          GetCurrentMethod(nullptr,
                                                !force_dump_stack,
                                               !(dump_for_abort || force_dump_stack));
      DumpNativeStack(os, unwinder, GetTid(), "  native: ", method);
    }
    dump_order = DumpJavaStack(os,
                                                    !force_dump_stack,
                                               !force_dump_stack);
  } else {
    os << "Not able to dump stack of thread that isn't suspended";
  }
  return dump_order;
}
void Thread::ThreadExitCallback(void* arg) {
  Thread* self = reinterpret_cast<Thread*>(arg);
  if (self->tls32_.thread_exit_check_count == 0) {
    LOG(WARNING) << "Native thread exiting without having called DetachCurrentThread (maybe it's "
        "going to use a pthread_key_create destructor?): " << *self;
    CHECK(is_started_);
#ifdef __BIONIC__
    __get_tls()[TLS_SLOT_ART_THREAD_SELF] = self;
#else
    CHECK_PTHREAD_CALL(pthread_setspecific, (Thread::pthread_key_self_, self), "reattach self");
    Thread::self_tls_ = self;
#endif
    self->tls32_.thread_exit_check_count = 1;
  } else {
    LOG(FATAL) << "Native thread exited without calling DetachCurrentThread: " << *self;
  }
}
void Thread::Startup() {
  CHECK(!is_started_);
  is_started_ = true;
  {
    MutexLock mu(nullptr, *Locks::thread_suspend_count_lock_);
    resume_cond_ = new ConditionVariable("Thread resumption condition variable",
                                         *Locks::thread_suspend_count_lock_);
  }
  CHECK_PTHREAD_CALL(pthread_key_create, (&Thread::pthread_key_self_, Thread::ThreadExitCallback),
                     "self key");
  if (pthread_getspecific(pthread_key_self_) != nullptr) {
    LOG(FATAL) << "Newly-created pthread TLS slot is not nullptr";
  }
#ifndef __BIONIC__
  CHECK(Thread::self_tls_ == nullptr);
#endif
}
void Thread::FinishStartup() {
  Runtime* runtime = Runtime::Current();
  CHECK(runtime->IsStarted());
  ScopedObjectAccess soa(Thread::Current());
  soa.Self()->CreatePeer("main", false, runtime->GetMainThreadGroup());
  soa.Self()->AssertNoPendingException();
  runtime->RunRootClinits(soa.Self());
  soa.Self()->NotifyThreadGroup(soa, runtime->GetMainThreadGroup());
  soa.Self()->AssertNoPendingException();
}
void Thread::Shutdown() {
  CHECK(is_started_);
  is_started_ = false;
  CHECK_PTHREAD_CALL(pthread_key_delete, (Thread::pthread_key_self_), "self key");
  MutexLock mu(Thread::Current(), *Locks::thread_suspend_count_lock_);
  if (resume_cond_ != nullptr) {
    delete resume_cond_;
    resume_cond_ = nullptr;
  }
}
void Thread::NotifyThreadGroup(ScopedObjectAccessAlreadyRunnable& soa, jobject thread_group) {
  ObjPtr<mirror::Object> thread_object = soa.Self()->GetPeer();
  ObjPtr<mirror::Object> thread_group_object = soa.Decode<mirror::Object>(thread_group);
  if (thread_group == nullptr || kIsDebugBuild) {
    thread_group_object = WellKnownClasses::java_lang_Thread_group->GetObject(thread_object);
    if (kIsDebugBuild && thread_group != nullptr) {
      CHECK(thread_group_object == soa.Decode<mirror::Object>(thread_group));
    }
  }
  WellKnownClasses::java_lang_ThreadGroup_add->InvokeVirtual<'V', 'L'>(
      soa.Self(), thread_group_object, thread_object);
}
Thread::Thread(bool daemon)
    : tls32_(daemon),
      wait_monitor_(nullptr),
      is_runtime_thread_(false) {
  wait_mutex_ = new Mutex("a thread wait mutex", LockLevel::kThreadWaitLock);
  wait_cond_ = new ConditionVariable("a thread wait condition variable", *wait_mutex_);
  tlsPtr_.mutator_lock = Locks::mutator_lock_;
  DCHECK(tlsPtr_.mutator_lock != nullptr);
  tlsPtr_.name.store(kThreadNameDuringStartup, std::memory_order_relaxed);
  static_assert((sizeof(Thread) % 4) == 0U,
                "art::Thread has a size which is not a multiple of 4.");
  DCHECK_EQ(GetStateAndFlags(std::memory_order_relaxed).GetValue(), 0u);
  StateAndFlags state_and_flags = StateAndFlags(0u).WithState(ThreadState::kNative);
  tls32_.state_and_flags.store(state_and_flags.GetValue(), std::memory_order_relaxed);
  tls32_.interrupted.store(false, std::memory_order_relaxed);
  tls32_.park_state_.store(kNoPermit, std::memory_order_relaxed);
  memset(&tlsPtr_.held_mutexes[0], 0, sizeof(tlsPtr_.held_mutexes));
  std::fill(tlsPtr_.rosalloc_runs,
            tlsPtr_.rosalloc_runs + kNumRosAllocThreadLocalSizeBracketsInThread,
            gc::allocator::RosAlloc::GetDedicatedFullRun());
  tlsPtr_.checkpoint_function = nullptr;
  for (uint32_t i = 0; i < kMaxSuspendBarriers; ++i) {
    tlsPtr_.active_suspend_barriers[i] = nullptr;
  }
  tlsPtr_.flip_function = nullptr;
  tlsPtr_.thread_local_mark_stack = nullptr;
  tls32_.is_transitioning_to_runnable = false;
  ResetTlab();
}
bool Thread::CanLoadClasses() const {
  return !IsRuntimeThread() || !Runtime::Current()->IsJavaDebuggable();
}
bool Thread::IsStillStarting() const {
  return (tlsPtr_.jpeer == nullptr && tlsPtr_.opeer == nullptr) ||
      (tlsPtr_.name.load() == kThreadNameDuringStartup);
}
void Thread::AssertPendingException() const {
  CHECK(IsExceptionPending()) << "Pending exception expected.";
}
void Thread::AssertPendingOOMException() const {
  AssertPendingException();
  auto* e = GetException();
  CHECK_EQ(e->GetClass(), WellKnownClasses::java_lang_OutOfMemoryError.Get()) << e->Dump();
}
void Thread::AssertNoPendingException() const {
  if (UNLIKELY(IsExceptionPending())) {
    ScopedObjectAccess soa(Thread::Current());
    LOG(FATAL) << "No pending exception expected: " << GetException()->Dump();
  }
}
void Thread::AssertNoPendingExceptionForNewException(const char* msg) const {
  if (UNLIKELY(IsExceptionPending())) {
    ScopedObjectAccess soa(Thread::Current());
    LOG(FATAL) << "Throwing new exception '" << msg << "' with unexpected pending exception: "
        << GetException()->Dump();
  }
}
class MonitorExitVisitor : public SingleRootVisitor {
public:
  explicit MonitorExitVisitor(Thread* self) : self_(self) { }
  void VisitRoot(mirror::Object* entered_monitor, const RootInfo& info ATTRIBUTE_UNUSED)
      override NO_THREAD_SAFETY_ANALYSIS {
    if (self_->HoldsLock(entered_monitor)) {
      LOG(WARNING) << "Calling MonitorExit on object "
                   << entered_monitor << " (" << entered_monitor->PrettyTypeOf() << ")"
                   << " left locked by native thread "
                   << *Thread::Current() << " which is detaching";
      entered_monitor->MonitorExit(self_);
    }
  }
private:
  Thread* const self_;
};
void Thread::Destroy(bool should_run_callbacks) {
  Thread* self = this;
  DCHECK_EQ(self, Thread::Current());
  if (tlsPtr_.jni_env != nullptr) {
    {
      ScopedObjectAccess soa(self);
      MonitorExitVisitor visitor(self);
      tlsPtr_.jni_env->monitors_.VisitRoots(&visitor, RootInfo(kRootVMInternal));
    }
    if (tlsPtr_.jpeer != nullptr) {
      tlsPtr_.jni_env->DeleteGlobalRef(tlsPtr_.jpeer);
      tlsPtr_.jpeer = nullptr;
    }
    if (tlsPtr_.class_loader_override != nullptr) {
      tlsPtr_.jni_env->DeleteGlobalRef(tlsPtr_.class_loader_override);
      tlsPtr_.class_loader_override = nullptr;
    }
  }
  if (tlsPtr_.opeer != nullptr) {
    ScopedObjectAccess soa(self);
    if (UNLIKELY(self->GetMethodTraceBuffer() != nullptr)) {
      Trace::FlushThreadBuffer(self);
      self->ResetMethodTraceBuffer();
    }
    HandleUncaughtExceptions();
    RemoveFromThreadGroup();
    Runtime* runtime = Runtime::Current();
    if (runtime != nullptr && should_run_callbacks) {
      runtime->GetRuntimeCallbacks()->ThreadDeath(self);
    }
    SetNativePeer< true>(tlsPtr_.opeer, nullptr);
    ObjPtr<mirror::Object> lock =
        WellKnownClasses::java_lang_Thread_lock->GetObject(tlsPtr_.opeer);
    if (lock != nullptr) {
      StackHandleScope<1> hs(self);
      Handle<mirror::Object> h_obj(hs.NewHandle(lock));
      ObjectLock<mirror::Object> locker(self, h_obj);
      locker.NotifyAll();
    }
    tlsPtr_.opeer = nullptr;
  }
  {
    ScopedObjectAccess soa(self);
    Runtime::Current()->GetHeap()->RevokeThreadLocalBuffers(this);
  }
  if (gUseReadBarrier) {
    Runtime::Current()->GetHeap()->ConcurrentCopyingCollector()->RevokeThreadLocalMarkStack(this);
  }
}
Thread::~Thread() {
  CHECK(tlsPtr_.class_loader_override {
  CHECK(tlsPtr_.class_loader_override == nullptr);
  CHECK(tlsPtr_.jpeer == nullptr);
  CHECK(tlsPtr_.opeer == nullptr);
  bool initialized = (tlsPtr_.jni_env != nullptr);
  if (initialized) {
    delete tlsPtr_.jni_env;
    tlsPtr_.jni_env = nullptr;
  }
  CHECK_NE(GetState(), ThreadState::kRunnable);
  CHECK(!ReadFlag(ThreadFlag::kCheckpointRequest));
  CHECK(!ReadFlag(ThreadFlag::kEmptyCheckpointRequest));
  CHECK(tlsPtr_.checkpoint_function == nullptr);
  CHECK_EQ(checkpoint_overflow_.size(), 0u);
  CHECK(tlsPtr_.flip_function == nullptr);
  CHECK_EQ(tls32_.is_transitioning_to_runnable, false);
  CHECK(tlsPtr_.deoptimization_context_stack == nullptr) << "Missed deoptimization";
  CHECK(tlsPtr_.frame_id_to_shadow_frame == nullptr) <<
      "Not all deoptimized frames have been consumed by the debugger.";
  SetStateUnsafe(ThreadState::kTerminated);
  delete wait_cond_;
  delete wait_mutex_;
  if (tlsPtr_.long_jump_context != nullptr) {
    delete tlsPtr_.long_jump_context;
  }
  if (initialized) {
    CleanupCpu();
  }
  SetCachedThreadName(nullptr);
  delete tlsPtr_.deps_or_stack_trace_sample.stack_trace_sample;
  if (tlsPtr_.method_trace_buffer != nullptr) {
    delete[] tlsPtr_.method_trace_buffer;
  }
  Runtime::Current()->GetHeap()->AssertThreadLocalBuffersAreRevoked(this);
  TearDownAlternateSignalStack();
}
void Thread::HandleUncaughtExceptions() {
  Thread* self = this;
  DCHECK_EQ(self, Thread::Current());
  if (!self->IsExceptionPending()) {
    return;
  }
  ObjPtr<mirror::Object> exception = self->GetException();
  self->ClearException();
  WellKnownClasses::java_lang_Thread_dispatchUncaughtException->InvokeFinal<'V', 'L'>(
      self, tlsPtr_.opeer, exception);
  self->ClearException();
}
void Thread::RemoveFromThreadGroup() {
  Thread* self = this;
  DCHECK_EQ(self, Thread::Current());
  ObjPtr<mirror::Object> group =
      WellKnownClasses::java_lang_Thread_group->GetObject(tlsPtr_.opeer);
  if (group != nullptr) {
    WellKnownClasses::java_lang_ThreadGroup_threadTerminated->InvokeVirtual<'V', 'L'>(
        self, group, tlsPtr_.opeer);
  }
}
template <bool kPointsToStack>
class JniTransitionReferenceVisitor : public StackVisitor {
public:
JniTransitionReferenceVisitor(Thread* thread, void* obj) REQUIRES_SHARED(Locks::mutator_lock_)
                                                           : StackVisitor(thread, nullptr, StackVisitor::StackWalkKind::kSkipInlinedFrames),
                                                           obj_(obj),
                                                           found_(false) {}
bool VisitFrame() override REQUIRES_SHARED(Locks::mutator_lock_) {
                             ArtMethod* m = GetMethod();
                             if (!m->IsNative() || m->IsCriticalNative()) {
                             return true;
                             }
                             if (kPointsToStack) {
                             uint8_t* sp = reinterpret_cast<uint8_t*>(GetCurrentQuickFrame());
                             size_t frame_size = GetCurrentQuickFrameInfo().FrameSizeInBytes();
                             uint32_t* current_vreg = reinterpret_cast<uint32_t*>(sp + frame_size + sizeof(ArtMethod*));
                             if (!m->IsStatic()) {
                             if (current_vreg == obj_) {
                             found_ = true;
                             return false;
                             }
                             current_vreg += 1u;
                             }
                             uint32_t shorty_length;
                             const char* shorty = m->GetShorty(&shorty_length);
                             for (size_t i = 1; i != shorty_length; ++i) {
                             switch (shorty[i]) {
                             case 'D':
                             case 'J':
                             current_vreg += 2u;
                             break;
                             case 'L':
                             if (current_vreg == obj_) {
                             found_ = true;
                             return false;
                             }
                             FALLTHROUGH_INTENDED;
                             default:
                             current_vreg += 1u;
                             break;
                             }
                             }
                             return obj_ >= current_vreg;
                             } else {
                             if (m->IsStatic() && obj_ == m->GetDeclaringClassAddressWithoutBarrier()) {
                             found_ = true;
                             return false;
                             }
                             return true;
                             }
                             }
  bool Found() const {
    return found_;
  }
private:
  void* obj_;
  bool found_;
};
bool Thread::IsJniTransitionReference(jobject obj) const {
  DCHECK(obj != nullptr);
  Thread* thread = const_cast<Thread*>(this);
  uint8_t* raw_obj = reinterpret_cast<uint8_t*>(obj);
  if (static_cast<size_t>(raw_obj - tlsPtr_.stack_begin) < tlsPtr_.stack_size) {
    JniTransitionReferenceVisitor< true> visitor(thread, raw_obj);
    visitor.WalkStack();
    return visitor.Found();
  } else {
    JniTransitionReferenceVisitor< false> visitor(thread, raw_obj);
    visitor.WalkStack();
    return visitor.Found();
  }
}
void Thread::HandleScopeVisitRoots(RootVisitor* visitor, uint32_t thread_id) {
  BufferedRootVisitor<kDefaultBufferedRootCount> buffered_visitor(
      visitor, RootInfo(kRootNativeStack, thread_id));
  for (BaseHandleScope* cur = tlsPtr_.top_handle_scope; cur; cur = cur->GetLink()) {
    cur->VisitRoots(buffered_visitor);
  }
}
ObjPtr<mirror::Object> Thread::DecodeJObject(jobject obj) const {
  if (obj == nullptr) {
    return nullptr;
  }
  IndirectRef ref = reinterpret_cast<IndirectRef>(obj);
  IndirectRefKind kind = IndirectReferenceTable::GetIndirectRefKind(ref);
  ObjPtr<mirror::Object> result;
  bool expect_null = false;
  if (kind == kLocal) {
    jni::LocalReferenceTable& locals = tlsPtr_.jni_env->locals_;
    result = locals.Get(ref);
  } else if (kind == kJniTransition) {
    DCHECK(IsJniTransitionReference(obj));
    result = reinterpret_cast<mirror::CompressedReference<mirror::Object>*>(obj)->AsMirrorPtr();
    VerifyObject(result);
  } else if (kind == kGlobal) {
    result = tlsPtr_.jni_env->vm_->DecodeGlobal(ref);
  } else {
    DCHECK_EQ(kind, kWeakGlobal);
    result = tlsPtr_.jni_env->vm_->DecodeWeakGlobal(const_cast<Thread*>(this), ref);
    if (Runtime::Current()->IsClearedJniWeakGlobal(result)) {
      expect_null = true;
      result = nullptr;
    }
  }
  DCHECK(expect_null || result != nullptr)
      << "use of deleted " << ToStr<IndirectRefKind>(kind).c_str()
      << " " << static_cast<const void*>(obj);
  return result;
}
bool Thread::IsJWeakCleared(jweak obj) const {
  CHECK(obj != nullptr);
  IndirectRef ref = reinterpret_cast<IndirectRef>(obj);
  IndirectRefKind kind = IndirectReferenceTable::GetIndirectRefKind(ref);
  CHECK_EQ(kind, kWeakGlobal);
  return tlsPtr_.jni_env->vm_->IsWeakGlobalCleared(const_cast<Thread*>(this), ref);
}
bool Thread::Interrupted() {
  DCHECK_EQ(Thread::Current(), this);
  bool interrupted = tls32_.interrupted.load(std::memory_order_seq_cst);
  if (interrupted) {
    tls32_.interrupted.store(false, std::memory_order_seq_cst);
  }
  return interrupted;
}
bool Thread::IsInterrupted() {
  return tls32_.interrupted.load(std::memory_order_seq_cst);
}
void Thread::Interrupt(Thread* self) {
  {
    MutexLock mu(self, *wait_mutex_);
    if (tls32_.interrupted.load(std::memory_order_seq_cst)) {
      return;
    }
    tls32_.interrupted.store(true, std::memory_order_seq_cst);
    NotifyLocked(self);
  }
  Unpark();
}
void Thread::Notify() {
  Thread* self = Thread::Current();
  MutexLock mu(self, *wait_mutex_);
  NotifyLocked(self);
}
void Thread::NotifyLocked(Thread* self) {
  if (wait_monitor_ != nullptr) {
    wait_cond_->Signal(self);
  }
}
void Thread::SetClassLoaderOverride(jobject class_loader_override) {
  if (tlsPtr_.class_loader_override != nullptr) {
    GetJniEnv()->DeleteGlobalRef(tlsPtr_.class_loader_override);
  }
  tlsPtr_.class_loader_override = GetJniEnv()->NewGlobalRef(class_loader_override);
}
using ArtMethodDexPcPair = std::pair<ArtMethod*, uint32_t>;
class FetchStackTraceVisitor : public StackVisitor {
public:
  explicit FetchStackTraceVisitor(Thread* thread,
                                  ArtMethodDexPcPair* saved_frames = nullptr,
                                  size_t max_saved_frames = 0)
      REQUIRES_SHARED(Locks::mutator_lock_)
      : StackVisitor(thread, nullptr, StackVisitor::StackWalkKind::kIncludeInlinedFrames),
        saved_frames_(saved_frames),
        max_saved_frames_(max_saved_frames) {}
bool VisitFrame() override REQUIRES_SHARED(Locks::mutator_lock_) {
                             ArtMethod* m = GetMethod();
                             if (skipping_ && !m->IsRuntimeMethod() &&
                             !GetClassRoot<mirror::Throwable>()->IsAssignableFrom(m->GetDeclaringClass())) {
                             skipping_ = false;
                             }
                             if (!skipping_) {
                             if (!m->IsRuntimeMethod()) {
                             if (depth_ < max_saved_frames_) {
                             saved_frames_[depth_].first = m;
                             saved_frames_[depth_].second = m->IsProxyMethod() ? dex::kDexNoIndex : GetDexPc();
                             }
                             ++depth_;
                             }
                             } else {
                             ++skip_depth_;
                             }
                             return true;
                             }
  uint32_t GetDepth() const {
    return depth_;
  }
  uint32_t GetSkipDepth() const {
    return skip_depth_;
  }
private:
  uint32_t depth_ = 0;
  uint32_t skip_depth_ = 0;
  bool skipping_ = true;
  ArtMethodDexPcPair* saved_frames_;
  const size_t max_saved_frames_;
  DISALLOW_COPY_AND_ASSIGN(FetchStackTraceVisitor);
};
class BuildInternalStackTraceVisitor : public StackVisitor {
public:
  BuildInternalStackTraceVisitor(Thread* self, Thread* thread, uint32_t skip_depth)
      : StackVisitor(thread, nullptr, StackVisitor::StackWalkKind::kIncludeInlinedFrames),
        self_(self),
        skip_depth_(skip_depth),
        pointer_size_(Runtime::Current()->GetClassLinker()->GetImagePointerSize()) {}
REQUIRES_SHARED(Locks::mutator_lock_)REQUIRES_SHARED(Locks::mutator_lock_) ACQUIRE(Roles::uninterruptible_) {
                                                                  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
                                                                  StackHandleScope<1> hs(self_);
                                                                  ObjPtr<mirror::Class> array_class =
                                                                  GetClassRoot<mirror::ObjectArray<mirror::Object>>(class_linker);
                                                                  Handle<mirror::ObjectArray<mirror::Object>> trace(
                                                                  hs.NewHandle(mirror::ObjectArray<mirror::Object>::Alloc(
                                                                  hs.Self(), array_class, static_cast<int32_t>(depth) + 1)));
                                                                  if (trace == nullptr) {
                                                                  self_->StartAssertNoThreadSuspension("Building internal stack trace");
                                                                  self_->AssertPendingOOMException();
                                                                  return false;
                                                                  }
                                                                  ObjPtr<mirror::PointerArray> methods_and_pcs =
                                                                  class_linker->AllocPointerArray(self_, depth * 2);
                                                                  const char* last_no_suspend_cause =
                                                                  self_->StartAssertNoThreadSuspension("Building internal stack trace");
                                                                  if (methods_and_pcs == nullptr) {
                                                                  self_->AssertPendingOOMException();
                                                                  return false;
                                                                  }
                                                                  trace->Set< false, false>(0, methods_and_pcs);
                                                                  trace_ = trace.Get();
                                                                  CHECK(last_no_suspend_cause == nullptr) << last_no_suspend_cause;
                                                                  return true;
                                                                  }
virtual ~BuildInternalStackTraceVisitor() RELEASE(Roles::uninterruptible_) {
                                            self_->EndAssertNoThreadSuspension(nullptr);
                                            }
bool VisitFrame() override REQUIRES_SHARED(Locks::mutator_lock_) {
                             if (trace_ == nullptr) {
                             return true;
                             }
                             if (skip_depth_ > 0) {
                             skip_depth_--;
                             return true;
                             }
                             ArtMethod* m = GetMethod();
                             if (m->IsRuntimeMethod()) {
                             return true;
                             }
                             AddFrame(m, m->IsProxyMethod() ? dex::kDexNoIndex : GetDexPc());
                             return true;
                             }
void AddFrame(ArtMethod* method, uint32_t dex_pc) REQUIRES_SHARED(Locks::mutator_lock_) {
                                                    ObjPtr<mirror::PointerArray> methods_and_pcs = GetTraceMethodsAndPCs();
                                                    methods_and_pcs->SetElementPtrSize< false, false>(
                                                    count_, method, pointer_size_);
                                                    methods_and_pcs->SetElementPtrSize< false, false>(
                                                    static_cast<uint32_t>(methods_and_pcs->GetLength()) / 2 + count_, dex_pc, pointer_size_);
                                                    ObjPtr<mirror::Object> keep_alive;
                                                    if (UNLIKELY(method->IsCopied())) {
                                                    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
                                                    keep_alive = class_linker->GetHoldingClassLoaderOfCopiedMethod(self_, method);
                                                    } else {
                                                    keep_alive = method->GetDeclaringClass();
                                                    }
                                                    trace_->Set< false, false>(
                                                    static_cast<int32_t>(count_) + 1, keep_alive);
                                                    ++count_;
                                                    }
ObjPtr<mirror::PointerArray> GetTraceMethodsAndPCs() const REQUIRES_SHARED(Locks::mutator_lock_) {
                                                             return ObjPtr<mirror::PointerArray>::DownCast(trace_->Get(0));
                                                             }
  mirror::ObjectArray<mirror::Object>* GetInternalStackTrace() const {
    return trace_;
  }
private:
  Thread* const self_;
  uint32_t skip_depth_;
  uint32_t count_ = 0;
  mirror::ObjectArray<mirror::Object>* trace_ = nullptr;
  const PointerSize pointer_size_;
  DISALLOW_COPY_AND_ASSIGN(BuildInternalStackTraceVisitor);
};
jobject Thread::CreateInternalStackTrace(const ScopedObjectAccessAlreadyRunnable& soa) const {
  constexpr size_t kMaxSavedFrames = 256;
  std::unique_ptr<ArtMethodDexPcPair[]> saved_frames(new ArtMethodDexPcPair[kMaxSavedFrames]);
  FetchStackTraceVisitor count_visitor(const_cast<Thread*>(this),
                                       &saved_frames[0],
                                       kMaxSavedFrames);
  count_visitor.WalkStack();
  const uint32_t depth = count_visitor.GetDepth();
  const uint32_t skip_depth = count_visitor.GetSkipDepth();
  BuildInternalStackTraceVisitor build_trace_visitor(
      soa.Self(), const_cast<Thread*>(this), skip_depth);
  if (!build_trace_visitor.Init(depth)) {
    return nullptr;
  }
  if (depth < kMaxSavedFrames) {
    for (size_t i = 0; i < depth; ++i) {
      build_trace_visitor.AddFrame(saved_frames[i].first, saved_frames[i].second);
    }
  } else {
    build_trace_visitor.WalkStack();
  }
  mirror::ObjectArray<mirror::Object>* trace = build_trace_visitor.GetInternalStackTrace();
  if (kIsDebugBuild) {
    ObjPtr<mirror::PointerArray> trace_methods = build_trace_visitor.GetTraceMethodsAndPCs();
    for (uint32_t i = 0; i < static_cast<uint32_t>(trace_methods->GetLength() / 2); ++i) {
      auto* method = trace_methods->GetElementPtrSize<ArtMethod*>(
          i, Runtime::Current()->GetClassLinker()->GetImagePointerSize());
      CHECK(method != nullptr);
    }
  }
  return soa.AddLocalReference<jobject>(trace);
}
bool Thread::IsExceptionThrownByCurrentMethod(ObjPtr<mirror::Throwable> exception) const {
  FetchStackTraceVisitor count_visitor(const_cast<Thread*>(this));
  count_visitor.WalkStack();
  return count_visitor.GetDepth() == static_cast<uint32_t>(exception->GetStackDepth());
}
static ObjPtr<mirror::StackTraceElement> CreateStackTraceElement(
    const ScopedObjectAccessAlreadyRunnable& soa,
    ArtMethod* method,
    uint32_t dex_pc)public:
               REQUIRES_SHARED(Locks::mutator_lock_) {
               VerifyObject(root);
               }
constexpr jlong FILL_CLASS_REFS_ONLY = 0x2;jint Thread::InternalStackTraceToStackFrameInfoArray(const ScopedObjectAccessAlreadyRunnable& soa, jlong mode,
    jobject internal, jint startLevel, jint batchSize, jint startBufferIndex, jobjectArray output_array) {
  int32_t depth = soa.Decode<mirror::Array>(internal)->GetLength() - 1;
  DCHECK_GE(depth, 0);
  StackHandleScope<6> hs(soa.Self());
  Handle<mirror::ObjectArray<mirror::Object>> framesOrClasses =
      hs.NewHandle(soa.Decode<mirror::ObjectArray<mirror::Object>>(output_array));
  jint endBufferIndex = startBufferIndex;
  if (startLevel < 0 || startLevel >= depth) {
    return endBufferIndex;
  }
  int32_t bufferSize = framesOrClasses->GetLength();
  if (startBufferIndex < 0 || startBufferIndex >= bufferSize) {
    return endBufferIndex;
  }
  bool isClassArray = (mode & FILL_CLASS_REFS_ONLY) != 0;
  Handle<mirror::ObjectArray<mirror::Object>> decoded_traces =
      hs.NewHandle(soa.Decode<mirror::Object>(internal)->AsObjectArray<mirror::Object>());
  DCHECK(decoded_traces->Get(0)->IsIntArray() || decoded_traces->Get(0)->IsLongArray());
  Handle<mirror::PointerArray> method_trace =
      hs.NewHandle(ObjPtr<mirror::PointerArray>::DownCast(decoded_traces->Get(0)));
  ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();
  Handle<mirror::Class> sfi_class =
      hs.NewHandle(class_linker->FindSystemClass(soa.Self(), "Ljava/lang/StackFrameInfo;"));
  DCHECK(sfi_class != nullptr);
  MutableHandle<mirror::StackFrameInfo> frame = hs.NewHandle<mirror::StackFrameInfo>(nullptr);
  MutableHandle<mirror::Class> clazz = hs.NewHandle<mirror::Class>(nullptr);
  for (uint32_t i = static_cast<uint32_t>(startLevel); i < static_cast<uint32_t>(depth); ++i) {
    if (endBufferIndex >= startBufferIndex + batchSize || endBufferIndex >= bufferSize) {
      break;
    }
    ArtMethod* method = method_trace->GetElementPtrSize<ArtMethod*>(i, kRuntimePointerSize);
    if (isClassArray) {
      clazz.Assign(method->GetDeclaringClass());
      framesOrClasses->Set(endBufferIndex, clazz.Get());
    } else {
      uint32_t dex_pc = method_trace->GetElementPtrSize<uint32_t>(
          i + static_cast<uint32_t>(method_trace->GetLength()) / 2, kRuntimePointerSize);
      ObjPtr<mirror::Object> frameObject = framesOrClasses->Get(endBufferIndex);
      if (frameObject == nullptr || !frameObject->InstanceOf(sfi_class.Get())) {
        break;
      }
      frame.Assign(ObjPtr<mirror::StackFrameInfo>::DownCast(frameObject));
      frame.Assign(InitStackFrameInfo(soa, class_linker, frame, method, dex_pc));
      if (frame == nullptr) {
        break;
      }
    }
    ++endBufferIndex;
  }
  return endBufferIndex;
}
static void SetNativePeer(JNIEnv* env, jobject java_peer, Thread* thread) {
  ScopedObjectAccess soa(env);
  SetNativePeer< false>(soa.Decode<mirror::Object>(java_peer), thread);
}
jobjectArray Thread::InternalStackTraceToStackTraceElementArray(
    const ScopedObjectAccessAlreadyRunnable& soa,
    jobject internal,
    jobjectArray output_array,
    int* stack_depth) {
  int32_t depth = soa.Decode<mirror::Array>(internal)->GetLength() - 1;
  DCHECK_GE(depth, 0);
  ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();
  jobjectArray result;
  if (output_array != nullptr) {
    result = output_array;
    const int32_t traces_length =
        soa.Decode<mirror::ObjectArray<mirror::StackTraceElement>>(result)->GetLength();
    depth = std::min(depth, traces_length);
  } else {
    ObjPtr<mirror::ObjectArray<mirror::StackTraceElement>> java_traces =
        class_linker->AllocStackTraceElementArray(soa.Self(), static_cast<size_t>(depth));
    if (java_traces == nullptr) {
      return nullptr;
    }
    result = soa.AddLocalReference<jobjectArray>(java_traces);
  }
  if (stack_depth != nullptr) {
    *stack_depth = depth;
  }
  for (uint32_t i = 0; i < static_cast<uint32_t>(depth); ++i) {
    ObjPtr<mirror::ObjectArray<mirror::Object>> decoded_traces =
        soa.Decode<mirror::Object>(internal)->AsObjectArray<mirror::Object>();
    DCHECK(decoded_traces->Get(0)->IsIntArray() || decoded_traces->Get(0)->IsLongArray());
    const ObjPtr<mirror::PointerArray> method_trace =
        ObjPtr<mirror::PointerArray>::DownCast(decoded_traces->Get(0));
    ArtMethod* method = method_trace->GetElementPtrSize<ArtMethod*>(i, kRuntimePointerSize);
    uint32_t dex_pc = method_trace->GetElementPtrSize<uint32_t>(
        i + static_cast<uint32_t>(method_trace->GetLength()) / 2, kRuntimePointerSize);
    const ObjPtr<mirror::StackTraceElement> obj = CreateStackTraceElement(soa, method, dex_pc);
    if (obj == nullptr) {
      return nullptr;
    }
    soa.Decode<mirror::ObjectArray<mirror::StackTraceElement>>(result)->Set<false>(
        static_cast<int32_t>(i), obj);
  }
  return result;
}
[[nodiscard]] static ObjPtr<mirror::StackFrameInfo> InitStackFrameInfo(
    const ScopedObjectAccessAlreadyRunnable& soa,
    ClassLinker* class_linker,
    Handle<mirror::StackFrameInfo> stackFrameInfo,
    ArtMethod* method,
    uint32_t dex_pc)jobjectArray Thread::CreateAnnotatedStackTrace(const ScopedObjectAccessAlreadyRunnable& soa) const {
  if (IsExceptionPending()) {
    return nullptr;
  }
  class CollectFramesAndLocksStackVisitor : public MonitorObjectsStackVisitor {
   public:
    CollectFramesAndLocksStackVisitor(const ScopedObjectAccessAlreadyRunnable& soaa_in,
                                      Thread* self,
                                      Context* context)
        : MonitorObjectsStackVisitor(self, context),
          wait_jobject_(soaa_in.Env(), nullptr),
          block_jobject_(soaa_in.Env(), nullptr),
          soaa_(soaa_in) {}
   protected:
    VisitMethodResult StartMethod(ArtMethod* m, size_t frame_nr ATTRIBUTE_UNUSED)
        override
        REQUIRES_SHARED(Locks::mutator_lock_) {
      ObjPtr<mirror::StackTraceElement> obj = CreateStackTraceElement(
          soaa_, m, GetDexPc( false));
      if (obj == nullptr) {
        return VisitMethodResult::kEndStackWalk;
      }
      stack_trace_elements_.emplace_back(soaa_.Env(), soaa_.AddLocalReference<jobject>(obj.Ptr()));
      return VisitMethodResult::kContinueMethod;
    }
    VisitMethodResult EndMethod(ArtMethod* m ATTRIBUTE_UNUSED) override {
      lock_objects_.push_back({});
      lock_objects_[lock_objects_.size() - 1].swap(frame_lock_objects_);
      DCHECK_EQ(lock_objects_.size(), stack_trace_elements_.size());
      return VisitMethodResult::kContinueMethod;
    }
    void VisitWaitingObject(ObjPtr<mirror::Object> obj, ThreadState state ATTRIBUTE_UNUSED)
        override
        REQUIRES_SHARED(Locks::mutator_lock_) {
      wait_jobject_.reset(soaa_.AddLocalReference<jobject>(obj));
    }
    void VisitSleepingObject(ObjPtr<mirror::Object> obj)
        override
        REQUIRES_SHARED(Locks::mutator_lock_) {
      wait_jobject_.reset(soaa_.AddLocalReference<jobject>(obj));
    }
    void VisitBlockedOnObject(ObjPtr<mirror::Object> obj,
                              ThreadState state ATTRIBUTE_UNUSED,
                              uint32_t owner_tid ATTRIBUTE_UNUSED)
        override
        REQUIRES_SHARED(Locks::mutator_lock_) {
      block_jobject_.reset(soaa_.AddLocalReference<jobject>(obj));
    }
    void VisitLockedObject(ObjPtr<mirror::Object> obj)
        override
        REQUIRES_SHARED(Locks::mutator_lock_) {
      frame_lock_objects_.emplace_back(soaa_.Env(), soaa_.AddLocalReference<jobject>(obj));
    }
   public:
    std::vector<ScopedLocalRef<jobject>> stack_trace_elements_;
    ScopedLocalRef<jobject> wait_jobject_;
    ScopedLocalRef<jobject> block_jobject_;
    std::vector<std::vector<ScopedLocalRef<jobject>>> lock_objects_;
   private:
    const ScopedObjectAccessAlreadyRunnable& soaa_;
    std::vector<ScopedLocalRef<jobject>> frame_lock_objects_;
  };
  std::unique_ptr<Context> context(Context::Create());
  CollectFramesAndLocksStackVisitor dumper(soa, const_cast<Thread*>(this), context.get());
  dumper.WalkStack();
  if (IsExceptionPending()) {
    return nullptr;
  }
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  StackHandleScope<6> hs(soa.Self());
  Handle<mirror::Class> h_aste_array_class = hs.NewHandle(class_linker->FindSystemClass(
      soa.Self(),
      "[Ldalvik/system/AnnotatedStackTraceElement;"));
  if (h_aste_array_class == nullptr) {
    return nullptr;
  }
  Handle<mirror::Class> h_aste_class = hs.NewHandle(h_aste_array_class->GetComponentType());
  Handle<mirror::Class> h_o_array_class =
      hs.NewHandle(GetClassRoot<mirror::ObjectArray<mirror::Object>>(class_linker));
  DCHECK(h_o_array_class != nullptr);
  class_linker->EnsureInitialized(soa.Self(),
                                  h_aste_class,
                                                         true,
                                                          true);
  if (soa.Self()->IsExceptionPending()) {
    return nullptr;
  }
  ArtField* stack_trace_element_field =
      h_aste_class->FindDeclaredInstanceField("stackTraceElement", "Ljava/lang/StackTraceElement;");
  DCHECK(stack_trace_element_field != nullptr);
  ArtField* held_locks_field =
      h_aste_class->FindDeclaredInstanceField("heldLocks", "[Ljava/lang/Object;");
  DCHECK(held_locks_field != nullptr);
  ArtField* blocked_on_field =
      h_aste_class->FindDeclaredInstanceField("blockedOn", "Ljava/lang/Object;");
  DCHECK(blocked_on_field != nullptr);
  int32_t length = static_cast<int32_t>(dumper.stack_trace_elements_.size());
  ObjPtr<mirror::ObjectArray<mirror::Object>> array =
      mirror::ObjectArray<mirror::Object>::Alloc(soa.Self(), h_aste_array_class.Get(), length);
  if (array == nullptr) {
    soa.Self()->AssertPendingOOMException();
    return nullptr;
  }
  ScopedLocalRef<jobjectArray> result(soa.Env(), soa.Env()->AddLocalReference<jobjectArray>(array));
  MutableHandle<mirror::Object> handle(hs.NewHandle<mirror::Object>(nullptr));
  MutableHandle<mirror::ObjectArray<mirror::Object>> handle2(
      hs.NewHandle<mirror::ObjectArray<mirror::Object>>(nullptr));
  for (size_t i = 0; i != static_cast<size_t>(length); ++i) {
    handle.Assign(h_aste_class->AllocObject(soa.Self()));
    if (handle == nullptr) {
      soa.Self()->AssertPendingOOMException();
      return nullptr;
    }
    stack_trace_element_field->SetObject<false>(
        handle.Get(), soa.Decode<mirror::Object>(dumper.stack_trace_elements_[i].get()));
    if (!dumper.lock_objects_[i].empty()) {
      handle2.Assign(mirror::ObjectArray<mirror::Object>::Alloc(
          soa.Self(), h_o_array_class.Get(), static_cast<int32_t>(dumper.lock_objects_[i].size())));
      if (handle2 == nullptr) {
        soa.Self()->AssertPendingOOMException();
        return nullptr;
      }
      int32_t j = 0;
      for (auto& scoped_local : dumper.lock_objects_[i]) {
        if (scoped_local == nullptr) {
          continue;
        }
        handle2->Set(j, soa.Decode<mirror::Object>(scoped_local.get()));
        DCHECK(!soa.Self()->IsExceptionPending());
        j++;
      }
      held_locks_field->SetObject<false>(handle.Get(), handle2.Get());
    }
    if (i == 0) {
      if (dumper.block_jobject_ != nullptr) {
        blocked_on_field->SetObject<false>(
            handle.Get(), soa.Decode<mirror::Object>(dumper.block_jobject_.get()));
      }
    }
    ScopedLocalRef<jobject> elem(soa.Env(), soa.AddLocalReference<jobject>(handle.Get()));
    soa.Env()->SetObjectArrayElement(result.get(), static_cast<jsize>(i), elem.get());
    DCHECK(!soa.Self()->IsExceptionPending());
  }
  return result.release();
}
void Thread::ThrowNewExceptionF(const char* exception_class_descriptor, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ThrowNewExceptionV(exception_class_descriptor, fmt, args);
  va_end(args);
}
void Thread::ThrowNewExceptionV(const char* exception_class_descriptor,
                                const char* fmt, va_list ap) {
  std::string msg;
  StringAppendV(&msg, fmt, ap);
  ThrowNewException(exception_class_descriptor, msg.c_str());
}
void Thread::ThrowNewException(const char* exception_class_descriptor,
                               const char* msg) {
  AssertNoPendingExceptionForNewException(msg);
  ThrowNewWrappedException(exception_class_descriptor, msg);
}
static ObjPtr<mirror::ClassLoader> GetCurrentClassLoader(Thread* self)
               REQUIRES_SHARED(Locks::mutator_lock_) {
               VerifyObject(root);
               }
void Thread::ThrowNewWrappedException(const char* exception_class_descriptor,
                                      const char* msg) {
  DCHECK_EQ(this, Thread::Current());
  ScopedObjectAccessUnchecked soa(this);
  StackHandleScope<3> hs(soa.Self());
  ScopedDisablePublicSdkChecker sdpsc;
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(GetCurrentClassLoader(soa.Self())));
  ScopedLocalRef<jobject> cause(GetJniEnv(), soa.AddLocalReference<jobject>(GetException()));
  ClearException();
  Runtime* runtime = Runtime::Current();
  auto* cl = runtime->GetClassLinker();
  Handle<mirror::Class> exception_class(
      hs.NewHandle(cl->FindClass(this, exception_class_descriptor, class_loader)));
  if (UNLIKELY(exception_class == nullptr)) {
    CHECK(IsExceptionPending());
    LOG(ERROR) << "No exception class " << PrettyDescriptor(exception_class_descriptor);
    return;
  }
  if (UNLIKELY(!runtime->GetClassLinker()->EnsureInitialized(soa.Self(), exception_class, true,
                                                             true))) {
    DCHECK(IsExceptionPending());
    return;
  }
  DCHECK_IMPLIES(runtime->IsStarted(), exception_class->IsThrowableClass());
  Handle<mirror::Throwable> exception(
      hs.NewHandle(ObjPtr<mirror::Throwable>::DownCast(exception_class->AllocObject(this))));
  if (exception == nullptr) {
    Dump(LOG_STREAM(WARNING));
    SetException(Runtime::Current()->GetPreAllocatedOutOfMemoryErrorWhenThrowingException());
    return;
  }
  const char* signature;
  ScopedLocalRef<jstring> msg_string(GetJniEnv(), nullptr);
  if (msg != nullptr) {
    msg_string.reset(
        soa.AddLocalReference<jstring>(mirror::String::AllocFromModifiedUtf8(this, msg)));
    if (UNLIKELY(msg_string.get() == nullptr)) {
      CHECK(IsExceptionPending());
      return;
    }
    if (cause.get() == nullptr) {
      signature = "(Ljava/lang/String;)V";
    } else {
      signature = "(Ljava/lang/String;Ljava/lang/Throwable;)V";
    }
  } else {
    if (cause.get() == nullptr) {
      signature = "()V";
    } else {
      signature = "(Ljava/lang/Throwable;)V";
    }
  }
  ArtMethod* exception_init_method =
      exception_class->FindConstructor(signature, cl->GetImagePointerSize());
  CHECK(exception_init_method != nullptr) << "No <init>" << signature << " in "
      << PrettyDescriptor(exception_class_descriptor);
  if (UNLIKELY(!runtime->IsStarted())) {
    if (msg != nullptr) {
      exception->SetDetailMessage(DecodeJObject(msg_string.get())->AsString());
    }
    if (cause.get() != nullptr) {
      exception->SetCause(DecodeJObject(cause.get())->AsThrowable());
    }
    ScopedLocalRef<jobject> trace(GetJniEnv(), CreateInternalStackTrace(soa));
    if (trace.get() != nullptr) {
      exception->SetStackState(DecodeJObject(trace.get()).Ptr());
    }
    SetException(exception.Get());
  } else {
    jvalue jv_args[2];
    size_t i = 0;
    if (msg != nullptr) {
      jv_args[i].l = msg_string.get();
      ++i;
    }
    if (cause.get() != nullptr) {
      jv_args[i].l = cause.get();
      ++i;
    }
    ScopedLocalRef<jobject> ref(soa.Env(), soa.AddLocalReference<jobject>(exception.Get()));
    InvokeWithJValues(soa, ref.get(), exception_init_method, jv_args);
    if (LIKELY(!IsExceptionPending())) {
      SetException(exception.Get());
    }
  }
}
void Thread::ThrowOutOfMemoryError(const char* msg) {
  LOG(WARNING) << "Throwing OutOfMemoryError "
               << '"' << msg << '"'
               << " (VmSize " << GetProcessStatus("VmSize")
               << (tls32_.throwing_OutOfMemoryError ? ", recursive case)" : ")");
  ScopedTrace trace("OutOfMemoryError");
  if (!tls32_.throwing_OutOfMemoryError) {
    tls32_.throwing_OutOfMemoryError = true;
    ThrowNewException("Ljava/lang/OutOfMemoryError;", msg);
    tls32_.throwing_OutOfMemoryError = false;
  } else {
    Dump(LOG_STREAM(WARNING));
    SetException(Runtime::Current()->GetPreAllocatedOutOfMemoryErrorWhenThrowingOOME());
  }
}
Thread* Thread::CurrentFromGdb() {
  return Thread::Current();
}
void Thread::DumpFromGdb() const {
  std::ostringstream ss;
  Dump(ss);
  std::string str(ss.str());
  std::cerr << str;
#ifdef ART_TARGET_ANDROID
  LOG(INFO) << str;
#endif
}
template<PointerSize ptr_size>
void Thread::DumpThreadOffset(std::ostream& os, uint32_t offset) {
#define DO_THREAD_OFFSET(x,y) \
    if (offset == (x).Uint32Value()) { \
      os << (y); \
      return; \
    }
  DO_THREAD_OFFSET(ThreadFlagsOffset<ptr_size>(), "state_and_flags")
  DO_THREAD_OFFSET(CardTableOffset<ptr_size>(), "card_table")
  DO_THREAD_OFFSET(ExceptionOffset<ptr_size>(), "exception")
  DO_THREAD_OFFSET(PeerOffset<ptr_size>(), "peer");
  DO_THREAD_OFFSET(JniEnvOffset<ptr_size>(), "jni_env")
  DO_THREAD_OFFSET(SelfOffset<ptr_size>(), "self")
  DO_THREAD_OFFSET(StackEndOffset<ptr_size>(), "stack_end")
  DO_THREAD_OFFSET(ThinLockIdOffset<ptr_size>(), "thin_lock_thread_id")
  DO_THREAD_OFFSET(IsGcMarkingOffset<ptr_size>(), "is_gc_marking")
  DO_THREAD_OFFSET(TopOfManagedStackOffset<ptr_size>(), "top_quick_frame_method")
  DO_THREAD_OFFSET(TopShadowFrameOffset<ptr_size>(), "top_shadow_frame")
  DO_THREAD_OFFSET(TopHandleScopeOffset<ptr_size>(), "top_handle_scope")
  DO_THREAD_OFFSET(ThreadSuspendTriggerOffset<ptr_size>(), "suspend_trigger")
#undef DO_THREAD_OFFSET
#define JNI_ENTRY_POINT_INFO(x) \
    if (JNI_ENTRYPOINT_OFFSET(ptr_size, x).Uint32Value() == offset) { \
      os << #x; \
      return; \
    }
  JNI_ENTRY_POINT_INFO(pDlsymLookup)
  JNI_ENTRY_POINT_INFO(pDlsymLookupCritical)
#undef JNI_ENTRY_POINT_INFO
#define QUICK_ENTRY_POINT_INFO(x) \
    if (QUICK_ENTRYPOINT_OFFSET(ptr_size, x).Uint32Value() == offset) { \
      os << #x; \
      return; \
    }
  QUICK_ENTRY_POINT_INFO(pAllocArrayResolved)
  QUICK_ENTRY_POINT_INFO(pAllocArrayResolved8)
  QUICK_ENTRY_POINT_INFO(pAllocArrayResolved16)
  QUICK_ENTRY_POINT_INFO(pAllocArrayResolved32)
  QUICK_ENTRY_POINT_INFO(pAllocArrayResolved64)
  QUICK_ENTRY_POINT_INFO(pAllocObjectResolved)
  QUICK_ENTRY_POINT_INFO(pAllocObjectInitialized)
  QUICK_ENTRY_POINT_INFO(pAllocObjectWithChecks)
  QUICK_ENTRY_POINT_INFO(pAllocStringObject)
  QUICK_ENTRY_POINT_INFO(pAllocStringFromBytes)
  QUICK_ENTRY_POINT_INFO(pAllocStringFromChars)
  QUICK_ENTRY_POINT_INFO(pAllocStringFromString)
  QUICK_ENTRY_POINT_INFO(pInstanceofNonTrivial)
  QUICK_ENTRY_POINT_INFO(pCheckInstanceOf)
  QUICK_ENTRY_POINT_INFO(pInitializeStaticStorage)
  QUICK_ENTRY_POINT_INFO(pResolveTypeAndVerifyAccess)
  QUICK_ENTRY_POINT_INFO(pResolveType)
  QUICK_ENTRY_POINT_INFO(pResolveString)
  QUICK_ENTRY_POINT_INFO(pSet8Instance)
  QUICK_ENTRY_POINT_INFO(pSet8Static)
  QUICK_ENTRY_POINT_INFO(pSet16Instance)
  QUICK_ENTRY_POINT_INFO(pSet16Static)
  QUICK_ENTRY_POINT_INFO(pSet32Instance)
  QUICK_ENTRY_POINT_INFO(pSet32Static)
  QUICK_ENTRY_POINT_INFO(pSet64Instance)
  QUICK_ENTRY_POINT_INFO(pSet64Static)
  QUICK_ENTRY_POINT_INFO(pSetObjInstance)
  QUICK_ENTRY_POINT_INFO(pSetObjStatic)
  QUICK_ENTRY_POINT_INFO(pGetByteInstance)
  QUICK_ENTRY_POINT_INFO(pGetBooleanInstance)
  QUICK_ENTRY_POINT_INFO(pGetByteStatic)
  QUICK_ENTRY_POINT_INFO(pGetBooleanStatic)
  QUICK_ENTRY_POINT_INFO(pGetShortInstance)
  QUICK_ENTRY_POINT_INFO(pGetCharInstance)
  QUICK_ENTRY_POINT_INFO(pGetShortStatic)
  QUICK_ENTRY_POINT_INFO(pGetCharStatic)
  QUICK_ENTRY_POINT_INFO(pGet32Instance)
  QUICK_ENTRY_POINT_INFO(pGet32Static)
  QUICK_ENTRY_POINT_INFO(pGet64Instance)
  QUICK_ENTRY_POINT_INFO(pGet64Static)
  QUICK_ENTRY_POINT_INFO(pGetObjInstance)
  QUICK_ENTRY_POINT_INFO(pGetObjStatic)
  QUICK_ENTRY_POINT_INFO(pAputObject)
  QUICK_ENTRY_POINT_INFO(pJniMethodStart)
  QUICK_ENTRY_POINT_INFO(pJniMethodEnd)
  QUICK_ENTRY_POINT_INFO(pJniMethodEntryHook)
  QUICK_ENTRY_POINT_INFO(pJniDecodeReferenceResult)
  QUICK_ENTRY_POINT_INFO(pJniLockObject)
  QUICK_ENTRY_POINT_INFO(pJniUnlockObject)
  QUICK_ENTRY_POINT_INFO(pQuickGenericJniTrampoline)
  QUICK_ENTRY_POINT_INFO(pLockObject)
  QUICK_ENTRY_POINT_INFO(pUnlockObject)
  QUICK_ENTRY_POINT_INFO(pCmpgDouble)
  QUICK_ENTRY_POINT_INFO(pCmpgFloat)
  QUICK_ENTRY_POINT_INFO(pCmplDouble)
  QUICK_ENTRY_POINT_INFO(pCmplFloat)
  QUICK_ENTRY_POINT_INFO(pCos)
  QUICK_ENTRY_POINT_INFO(pSin)
  QUICK_ENTRY_POINT_INFO(pAcos)
  QUICK_ENTRY_POINT_INFO(pAsin)
  QUICK_ENTRY_POINT_INFO(pAtan)
  QUICK_ENTRY_POINT_INFO(pAtan2)
  QUICK_ENTRY_POINT_INFO(pCbrt)
  QUICK_ENTRY_POINT_INFO(pCosh)
  QUICK_ENTRY_POINT_INFO(pExp)
  QUICK_ENTRY_POINT_INFO(pExpm1)
  QUICK_ENTRY_POINT_INFO(pHypot)
  QUICK_ENTRY_POINT_INFO(pLog)
  QUICK_ENTRY_POINT_INFO(pLog10)
  QUICK_ENTRY_POINT_INFO(pNextAfter)
  QUICK_ENTRY_POINT_INFO(pSinh)
  QUICK_ENTRY_POINT_INFO(pTan)
  QUICK_ENTRY_POINT_INFO(pTanh)
  QUICK_ENTRY_POINT_INFO(pFmod)
  QUICK_ENTRY_POINT_INFO(pL2d)
  QUICK_ENTRY_POINT_INFO(pFmodf)
  QUICK_ENTRY_POINT_INFO(pL2f)
  QUICK_ENTRY_POINT_INFO(pD2iz)
  QUICK_ENTRY_POINT_INFO(pF2iz)
  QUICK_ENTRY_POINT_INFO(pIdivmod)
  QUICK_ENTRY_POINT_INFO(pD2l)
  QUICK_ENTRY_POINT_INFO(pF2l)
  QUICK_ENTRY_POINT_INFO(pLdiv)
  QUICK_ENTRY_POINT_INFO(pLmod)
  QUICK_ENTRY_POINT_INFO(pLmul)
  QUICK_ENTRY_POINT_INFO(pShlLong)
  QUICK_ENTRY_POINT_INFO(pShrLong)
  QUICK_ENTRY_POINT_INFO(pUshrLong)
  QUICK_ENTRY_POINT_INFO(pIndexOf)
  QUICK_ENTRY_POINT_INFO(pStringCompareTo)
  QUICK_ENTRY_POINT_INFO(pMemcpy)
  QUICK_ENTRY_POINT_INFO(pQuickImtConflictTrampoline)
  QUICK_ENTRY_POINT_INFO(pQuickResolutionTrampoline)
  QUICK_ENTRY_POINT_INFO(pQuickToInterpreterBridge)
  QUICK_ENTRY_POINT_INFO(pInvokeDirectTrampolineWithAccessCheck)
  QUICK_ENTRY_POINT_INFO(pInvokeInterfaceTrampolineWithAccessCheck)
  QUICK_ENTRY_POINT_INFO(pInvokeStaticTrampolineWithAccessCheck)
  QUICK_ENTRY_POINT_INFO(pInvokeSuperTrampolineWithAccessCheck)
  QUICK_ENTRY_POINT_INFO(pInvokeVirtualTrampolineWithAccessCheck)
  QUICK_ENTRY_POINT_INFO(pInvokePolymorphic)
  QUICK_ENTRY_POINT_INFO(pTestSuspend)
  QUICK_ENTRY_POINT_INFO(pDeliverException)
  QUICK_ENTRY_POINT_INFO(pThrowArrayBounds)
  QUICK_ENTRY_POINT_INFO(pThrowDivZero)
  QUICK_ENTRY_POINT_INFO(pThrowNullPointer)
  QUICK_ENTRY_POINT_INFO(pThrowStackOverflow)
  QUICK_ENTRY_POINT_INFO(pDeoptimize)
  QUICK_ENTRY_POINT_INFO(pA64Load)
  QUICK_ENTRY_POINT_INFO(pA64Store)
  QUICK_ENTRY_POINT_INFO(pNewEmptyString)
  QUICK_ENTRY_POINT_INFO(pNewStringFromBytes_B)
  QUICK_ENTRY_POINT_INFO(pNewStringFromBytes_BB)
  QUICK_ENTRY_POINT_INFO(pNewStringFromBytes_BI)
  QUICK_ENTRY_POINT_INFO(pNewStringFromBytes_BII)
  QUICK_ENTRY_POINT_INFO(pNewStringFromBytes_BIII)
  QUICK_ENTRY_POINT_INFO(pNewStringFromBytes_BIIString)
  QUICK_ENTRY_POINT_INFO(pNewStringFromBytes_BString)
  QUICK_ENTRY_POINT_INFO(pNewStringFromBytes_BIICharset)
  QUICK_ENTRY_POINT_INFO(pNewStringFromBytes_BCharset)
  QUICK_ENTRY_POINT_INFO(pNewStringFromChars_C)
  QUICK_ENTRY_POINT_INFO(pNewStringFromChars_CII)
  QUICK_ENTRY_POINT_INFO(pNewStringFromChars_IIC)
  QUICK_ENTRY_POINT_INFO(pNewStringFromCodePoints)
  QUICK_ENTRY_POINT_INFO(pNewStringFromString)
  QUICK_ENTRY_POINT_INFO(pNewStringFromStringBuffer)
  QUICK_ENTRY_POINT_INFO(pNewStringFromStringBuilder)
  QUICK_ENTRY_POINT_INFO(pNewStringFromUtf16Bytes_BII)
  QUICK_ENTRY_POINT_INFO(pJniReadBarrier)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg00)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg01)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg02)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg03)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg04)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg05)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg06)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg07)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg08)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg09)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg10)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg11)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg12)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg13)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg14)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg15)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg16)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg17)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg18)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg19)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg20)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg21)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg22)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg23)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg24)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg25)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg26)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg27)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg28)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg29)
  QUICK_ENTRY_POINT_INFO(pReadBarrierSlow)
  QUICK_ENTRY_POINT_INFO(pReadBarrierForRootSlow)
#undef QUICK_ENTRY_POINT_INFO
  os << offset;
}
void Thread::QuickDeliverException(bool skip_method_exit_callbacks) {
  ObjPtr<mirror::Throwable> exception = GetException();
  CHECK(exception != nullptr);
  if (exception == GetDeoptimizationException()) {
    ClearException();
    artDeoptimize(this, skip_method_exit_callbacks);
    UNREACHABLE();
  }
  ReadBarrier::MaybeAssertToSpaceInvariant(exception.Ptr());
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  if (instrumentation->HasExceptionThrownListeners() &&
      IsExceptionThrownByCurrentMethod(exception)) {
    StackHandleScope<1> hs(this);
    HandleWrapperObjPtr<mirror::Throwable> h_exception(hs.NewHandleWrapper(&exception));
    instrumentation->ExceptionThrownEvent(this, exception);
  }
  bool needs_deopt =
      instrumentation->HasMethodExitListeners() && Runtime::Current()->AreNonStandardExitsEnabled();
  if (Dbg::IsForcedInterpreterNeededForException(this) || IsForceInterpreter() || needs_deopt) {
    NthCallerVisitor visitor(this, 0, false);
    visitor.WalkStack();
    if (visitor.GetCurrentQuickFrame() != nullptr) {
      if (Runtime::Current()->IsAsyncDeoptimizeable(visitor.GetOuterMethod(), visitor.caller_pc)) {
        const DeoptimizationMethodType method_type = DeoptimizationMethodType::kDefault;
        PushDeoptimizationContext(
            JValue(),
                                false,
            exception,
                             false,
            method_type);
        artDeoptimize(this, skip_method_exit_callbacks);
        UNREACHABLE();
      } else {
        LOG(WARNING) << "Got a deoptimization request on un-deoptimizable method "
                     << visitor.caller->PrettyMethod();
      }
    } else {
      DCHECK(visitor.caller == nullptr || visitor.IsShadowFrame());
    }
  }
  ClearException();
  QuickExceptionHandler exception_handler(this, false);
  exception_handler.FindCatch(exception, skip_method_exit_callbacks);
  if (exception_handler.GetClearException()) {
    DCHECK(!IsExceptionPending());
  } else {
    DCHECK(IsExceptionPending());
    ReadBarrier::MaybeAssertToSpaceInvariant(GetException());
  }
  exception_handler.DoLongJump();
}
Context* Thread::GetLongJumpContext() {
  Context* result = tlsPtr_.long_jump_context;
  if (result == nullptr) {
    result = Context::Create();
  } else {
    tlsPtr_.long_jump_context = nullptr;
    result->Reset();
  }
  return result;
}
ArtMethod* Thread::GetCurrentMethod(uint32_t* dex_pc_out,
                                    bool check_suspended,
                                    bool abort_on_error) const {
  ArtMethod* method = nullptr;
  uint32_t dex_pc = dex::kDexNoIndex;
  StackVisitor::WalkStack(
      [&](const StackVisitor* visitor) REQUIRES_SHARED(Locks::mutator_lock_) {
        ArtMethod* m = visitor->GetMethod();
        if (m->IsRuntimeMethod()) {
          return true;
        }
        method = m;
        dex_pc = visitor->GetDexPc(abort_on_error);
        return false;
      },
      const_cast<Thread*>(this),
                     nullptr,
      StackVisitor::StackWalkKind::kIncludeInlinedFrames,
      check_suspended);
  if (dex_pc_out != nullptr) {
    *dex_pc_out = dex_pc;
  }
  return method;
}
bool Thread::HoldsLock(ObjPtr<mirror::Object> object) const {
  return object != nullptr && object->GetLockOwnerThreadId() == GetThreadId();
}
extern std::vector<StackReference<mirror::Object>*> GetProxyReferenceArguments(ArtMethod** sp)
ReferenceMapVisitor(Thread* thread, Context* context, RootVisitor& visitor)REQUIRES_SHARED(Locks::mutator_lock_)
      : StackVisitor(thread, context, StackVisitor::StackWalkKind::kSkipInlinedFrames),
        visitor_(visitor),
        visit_declaring_class_(!Runtime::Current()->GetHeap()->IsPerformingUffdCompaction()) {
    if (kPrecise) {
      VisitQuickFramePrecise();
    } else {
      VisitQuickFrameNonPrecise();
    }
  }
void VisitQuickFrameNonPrecise() REQUIRES_SHARED(Locks::mutator_lock_) {
               VerifyObject(root);
               }
void VisitQuickFramePrecise() REQUIRES_SHARED(Locks::mutator_lock_) {
               VerifyObject(root);
               }
  RootVisitor& visitor_;
  bool visit_declaring_class_;
}
class RootCallbackVisitor {
private:
  RootVisitor* const visitor_;
  const uint32_t tid_;
public:
  RootCallbackVisitor(RootVisitor* visitor, uint32_t tid): visitor_(visitor), tid_(tid) {}
  RootCallbackVisitor(RootVisitor* visitor, uint32_t tid): visitor_(visitor), tid_(tid) {}
  void operator()(mirror::Object** obj, size_t vreg, const StackVisitor* stack_visitor) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
      visitor_->VisitRoot(obj, JavaFrameRootInfo(tid_, stack_visitor, vreg));
      }
private:
  const uint32_t tid_;
};
void Thread::VisitReflectiveTargets(ReflectiveValueVisitor* visitor) {
  for (BaseReflectiveHandleScope* brhs = GetTopReflectiveHandleScope();
       brhs != nullptr;
       brhs = brhs->GetLink()) {
    brhs->VisitTargets(visitor);
  }
}
void Thread::VisitRoots(RootVisitor* visitor, VisitRootFlags flags) {
  if ((flags & VisitRootFlags::kVisitRootFlagPrecise) != 0) {
    VisitRoots< true>(visitor);
  } else {
    VisitRoots< false>(visitor);
  }
}
static void SweepCacheEntry(IsMarkedVisitor* visitor,
                            const Instruction* inst,
                            size_t* value,
                            bool only_update_class) REQUIRES_SHARED(Locks::mutator_lock_) {
               VerifyObject(root);
               }
void Thread::SweepInterpreterCache(IsMarkedVisitor* visitor) {
  bool only_update_class = Runtime::Current()->GetHeap()->IsPerformingUffdCompaction();
  for (InterpreterCache::Entry& entry : GetInterpreterCache()->GetArray()) {
    SweepCacheEntry(visitor,
                    reinterpret_cast<const Instruction*>(entry.first),
                    &entry.second,
                    only_update_class);
  }
}
void Thread::VisitRoots(RootVisitor* visitor, VisitRootFlags flags) {
  if ((flags & VisitRootFlags::kVisitRootFlagPrecise) != 0) {
    VisitRoots< true>(visitor);
  } else {
    VisitRoots< false>(visitor);
  }
}
class VerifyRootVisitor : public SingleRootVisitor {
public:
void VisitRoot(mirror::Object* root, const RootInfo& info ATTRIBUTE_UNUSED)
      override REQUIRES_SHARED(Locks::mutator_lock_) {
               VerifyObject(root);
               }
};
void Thread::VerifyStackImpl() {
  if (Runtime::Current()->GetHeap()->IsObjectValidationEnabled()) {
    VerifyRootVisitor visitor;
    std::unique_ptr<Context> context(Context::Create());
    RootCallbackVisitor visitor_to_callback(&visitor, GetThreadId());
    ReferenceMapVisitor<RootCallbackVisitor> mapper(this, context.get(), visitor_to_callback);
    mapper.WalkStack();
  }
}
void Thread::SetStackEndForStackOverflow() {
  if (tlsPtr_.stack_end == tlsPtr_.stack_begin) {
    LOG(ERROR) << "Need to increase kStackOverflowReservedBytes (currently "
               << GetStackOverflowReservedBytes(kRuntimeISA) << ")?";
    DumpStack(LOG_STREAM(ERROR));
    LOG(FATAL) << "Recursive stack overflow.";
  }
  tlsPtr_.stack_end = tlsPtr_.stack_begin;
  bool implicit_stack_check = Runtime::Current()->GetImplicitStackOverflowChecks();
  if (implicit_stack_check) {
    if (!UnprotectStack()) {
      LOG(ERROR) << "Unable to remove stack protection for stack overflow";
    }
  }
}
void Thread::SetTlab(uint8_t* start, uint8_t* end, uint8_t* limit) {
  DCHECK_LE(start, end);
  DCHECK_LE(end, limit);
  tlsPtr_.thread_local_start = start;
  tlsPtr_.thread_local_pos = tlsPtr_.thread_local_start;
  tlsPtr_.thread_local_end = end;
  tlsPtr_.thread_local_limit = limit;
  tlsPtr_.thread_local_objects = 0;
}
void Thread::ResetTlab() {
  gc::Heap* const heap = Runtime::Current()->GetHeap();
  if (heap->GetHeapSampler().IsEnabled()) {
    heap->AdjustSampleOffset(GetTlabPosOffset());
    VLOG(heap) << "JHP: ResetTlab, Tid: " << GetTid()
               << " adjustment = "
               << (tlsPtr_.thread_local_pos - tlsPtr_.thread_local_start);
  }
  SetTlab(nullptr, nullptr, nullptr);
}
bool Thread::HasTlab() const {
  const bool has_tlab = tlsPtr_.thread_local_pos != nullptr;
  if (has_tlab) {
    DCHECK(tlsPtr_.thread_local_start != nullptr && tlsPtr_.thread_local_end != nullptr);
  } else {
    DCHECK(tlsPtr_.thread_local_start == nullptr && tlsPtr_.thread_local_end == nullptr);
  }
  return has_tlab;
}
void Thread::AdjustTlab(size_t slide_bytes) {
  if (HasTlab()) {
    tlsPtr_.thread_local_start -= slide_bytes;
    tlsPtr_.thread_local_pos -= slide_bytes;
    tlsPtr_.thread_local_end -= slide_bytes;
    tlsPtr_.thread_local_limit -= slide_bytes;
  }
}
std::ostream& operator<<(std::ostream& os, const Thread& thread) {
  thread.ShortDump(os);
  return os;
}
bool Thread::ProtectStack(bool fatal_on_error) {
  void* pregion = tlsPtr_.stack_begin - kStackOverflowProtectedSize;
  VLOG(threads) << "Protecting stack at " << pregion;
  if (mprotect(pregion, kStackOverflowProtectedSize, PROT_NONE) == -1) {
    if (fatal_on_error) {
      LOG(ERROR) << "Unable to create protected region in stack for implicit overflow check. "
          "Reason: "
          << strerror(errno) << " size:  " << kStackOverflowProtectedSize;
      exit(1);
    }
    return false;
  }
  return true;
}
bool Thread::UnprotectStack() {
  void* pregion = tlsPtr_.stack_begin - kStackOverflowProtectedSize;
  VLOG(threads) << "Unprotecting stack at " << pregion;
  return mprotect(pregion, kStackOverflowProtectedSize, PROT_READ|PROT_WRITE) == 0;
}
void Thread::PushVerifier(verifier::MethodVerifier* verifier) {
  verifier->link_ = tlsPtr_.method_verifier;
  tlsPtr_.method_verifier = verifier;
}
void Thread::PopVerifier(verifier::MethodVerifier* verifier) {
  CHECK_EQ(tlsPtr_.method_verifier, verifier);
  tlsPtr_.method_verifier = verifier->link_;
}
size_t Thread::NumberOfHeldMutexes() const {
  size_t count = 0;
  for (BaseMutex* mu : tlsPtr_.held_mutexes) {
    count += mu != nullptr ? 1 : 0;
  }
  return count;
}
void Thread::DeoptimizeWithDeoptimizationException(JValue* result) {
  DCHECK_EQ(GetException(), Thread::GetDeoptimizationException());
  ClearException();
  ObjPtr<mirror::Throwable> pending_exception;
  bool from_code = false;
  DeoptimizationMethodType method_type;
  PopDeoptimizationContext(result, &pending_exception, &from_code, &method_type);
  SetTopOfStack(nullptr);
  if (pending_exception != nullptr) {
    SetException(pending_exception);
  }
  ShadowFrame* shadow_frame = MaybePopDeoptimizedStackedShadowFrame();
  if (shadow_frame != nullptr) {
    SetTopOfShadowStack(shadow_frame);
    interpreter::EnterInterpreterFromDeoptimize(this,
                                                shadow_frame,
                                                result,
                                                from_code,
                                                method_type);
  }
}
void Thread::SetAsyncException(ObjPtr<mirror::Throwable> new_exception) {
  CHECK(new_exception != nullptr);
  Runtime::Current()->SetAsyncExceptionsThrown();
  if (kIsDebugBuild) {
    MutexLock mu(Thread::Current(), *Locks::thread_suspend_count_lock_);
    CHECK(this == Thread::Current() || GetSuspendCount() >= 1)
        << "It doesn't look like this was called in a checkpoint! this: "
        << this << " count: " << GetSuspendCount();
  }
  tlsPtr_.async_exception = new_exception.Ptr();
}
bool Thread::ObserveAsyncException() {
  DCHECK(this == Thread::Current());
  if (tlsPtr_.async_exception != nullptr) {
    if (tlsPtr_.exception != nullptr) {
      LOG(WARNING) << "Overwriting pending exception with async exception. Pending exception is: "
                   << tlsPtr_.exception->Dump();
      LOG(WARNING) << "Async exception is " << tlsPtr_.async_exception->Dump();
    }
    tlsPtr_.exception = tlsPtr_.async_exception;
    tlsPtr_.async_exception = nullptr;
    return true;
  } else {
    return IsExceptionPending();
  }
}
void Thread::SetException(ObjPtr<mirror::Throwable> new_exception) {
  CHECK(new_exception != nullptr);
  tlsPtr_.exception = new_exception.Ptr();
}
bool Thread::IsAotCompiler() {
  return Runtime::Current()->IsAotCompiler();
}
mirror::Object* Thread::GetPeerFromOtherThread() const {
  DCHECK(tlsPtr_.jpeer == nullptr);
  mirror::Object* peer = tlsPtr_.opeer;
  if (gUseReadBarrier && Current()->GetIsGcMarking()) {
    peer = art::ReadBarrier::Mark(peer);
  }
  return peer;
}
void Thread::SetReadBarrierEntrypoints() {
  UpdateReadBarrierEntrypoints(&tlsPtr_.quick_entrypoints, true);
}
void Thread::ClearAllInterpreterCaches() {
  static struct ClearInterpreterCacheClosure : Closure {
    void Run(Thread* thread) override {
      thread->GetInterpreterCache()->Clear(thread);
    }
  } closure;
  Runtime::Current()->GetThreadList()->RunCheckpoint(&closure);
}
void Thread::ReleaseLongJumpContextInternal() {
  delete tlsPtr_.long_jump_context;
}
void Thread::SetNativePriority(int new_priority) {
  palette_status_t status = PaletteSchedSetPriority(GetTid(), new_priority);
  CHECK(status == PALETTE_STATUS_OK || status == PALETTE_STATUS_CHECK_ERRNO);
}
int Thread::GetNativePriority() const {
  int priority = 0;
  palette_status_t status = PaletteSchedGetPriority(GetTid(), &priority);
  CHECK(status == PALETTE_STATUS_OK || status == PALETTE_STATUS_CHECK_ERRNO);
  return priority;
}
bool Thread::IsSystemDaemon() const {
  if (GetPeer() == nullptr) {
    return false;
  }
  return WellKnownClasses::java_lang_Thread_systemDaemon->GetBoolean(GetPeer());
}
std::string Thread::StateAndFlagsAsHexString() const {
  std::stringstream result_stream;
  result_stream << std::hex << GetStateAndFlags(std::memory_order_relaxed).GetValue();
  return result_stream.str();
}
ScopedExceptionStorage::ScopedExceptionStorage(art::Thread* self)
    : self_(self), hs_(self_), excp_(hs_.NewHandle<art::mirror::Throwable>(self_->GetException())) {
  self_->ClearException();
}
void ScopedExceptionStorage::SuppressOldException(const char* message) {
  CHECK(self_->IsExceptionPending()) << *self_;
  ObjPtr<mirror::Throwable> old_suppressed(excp_.Get());
  excp_.Assign(self_->GetException());
  if (old_suppressed != nullptr) {
    LOG(WARNING) << message << "Suppressing old exception: " << old_suppressed->Dump();
  }
  self_->ClearException();
}
ScopedExceptionStorage::~ScopedExceptionStorage() {
  CHECK(!self_->IsExceptionPending()) << *self_;
  if (!excp_.IsNull()) {
    self_->SetException(excp_.Get());
  }
}
