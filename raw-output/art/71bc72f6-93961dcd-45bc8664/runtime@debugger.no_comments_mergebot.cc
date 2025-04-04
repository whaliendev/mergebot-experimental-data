#include "debugger.h"
#include <sys/uio.h>
#include <set>
#include "arch/context.h"
#include "class_linker.h"
#include "class_linker-inl.h"
#include "dex_file-inl.h"
#include "dex_instruction.h"
#include "field_helper.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/space/large_object_space.h"
#include "gc/space/space-inl.h"
#include "handle_scope.h"
#include "jdwp/object_registry.h"
#include "method_helper.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/class.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string-inl.h"
#include "mirror/throwable.h"
#include "quick/inline_method_analyser.h"
#include "reflection.h"
#include "safe_map.h"
#include "scoped_thread_state_change.h"
#include "ScopedLocalRef.h"
#include "ScopedPrimitiveArray.h"
#include "handle_scope-inl.h"
#include "thread_list.h"
#include "throw_location.h"
#include "utf.h"
#include "verifier/method_verifier-inl.h"
#include "well_known_classes.h"
#ifdef HAVE_ANDROID_OS
#include "cutils/properties.h"
#endif
namespace art {
static const size_t kMaxAllocRecordStackDepth = 16;
static const size_t kDefaultNumAllocRecords = 64*1024;
static uint16_t CappedAllocRecordCount(size_t alloc_record_count) {
  if (alloc_record_count > 0xffff) {
    return 0xffff;
  }
  return alloc_record_count;
}
class AllocRecordStackTraceElement {
public:
  AllocRecordStackTraceElement() : method_(nullptr), dex_pc_(0) {
  }
int32_t LineNumber() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
                       mirror::ArtMethod* method = Method();
                       DCHECK(method != nullptr);
                       return method->GetLineNumFromDexPC(DexPc());
                       }
  mirror::ArtMethod* Method() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ScopedObjectAccessUnchecked soa(Thread::Current());
    return soa.DecodeMethod(method_);
  }
void SetMethod(mirror::ArtMethod* m) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
                                       ScopedObjectAccessUnchecked soa(Thread::Current());
                                       method_ = soa.EncodeMethod(m);
                                       }
  uint32_t DexPc() const {
    return dex_pc_;
  }
  void SetDexPc(uint32_t pc) {
    dex_pc_ = pc;
  }
private:
  jmethodID method_;
  uint32_t dex_pc_;
};
jobject Dbg::TypeCache::Add(mirror::Class* t) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  int32_t hash_code = t->IdentityHashCode();
  auto range = objects_.equal_range(hash_code);
  for (auto it = range.first; it != range.second; ++it) {
    if (soa.Decode<mirror::Class*>(it->second) == t) {
      return it->second;
    }
  }
  JNIEnv* env = soa.Env();
  const jobject local_ref = soa.AddLocalReference<jobject>(t);
  const jobject weak_global = env->NewWeakGlobalRef(local_ref);
  env->DeleteLocalRef(local_ref);
  objects_.insert(std::make_pair(hash_code, weak_global));
  return weak_global;
}
void Dbg::TypeCache::Clear() {
  JavaVMExt* vm = Runtime::Current()->GetJavaVM();
  Thread* self = Thread::Current();
  for (const auto& p : objects_) {
    vm->DeleteWeakGlobalRef(self, p.second);
  }
  objects_.clear();
}
class AllocRecord {
public:
  AllocRecord() : type_(nullptr), byte_count_(0), thin_lock_id_(0) {}
  mirror::Class* Type() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return down_cast<mirror::Class*>(Thread::Current()->DecodeJObject(type_));
  }
void SetType(mirror::Class* t) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_,
                                                       Locks::alloc_tracker_lock_) {
                                 type_ = Dbg::type_cache_.Add(t);
                                 }
  size_t GetDepth() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    size_t depth = 0;
    while (depth < kMaxAllocRecordStackDepth && stack_[depth].Method() != nullptr) {
      ++depth;
    }
    return depth;
  }
  size_t ByteCount() const {
    return byte_count_;
  }
  void SetByteCount(size_t count) {
    byte_count_ = count;
  }
  uint16_t ThinLockId() const {
    return thin_lock_id_;
  }
  void SetThinLockId(uint16_t id) {
    thin_lock_id_ = id;
  }
  AllocRecordStackTraceElement* StackElement(size_t index) {
    DCHECK_LT(index, kMaxAllocRecordStackDepth);
    return &stack_[index];
  }
private:
jobject type_;
  size_t byte_count_;
  uint16_t thin_lock_id_;
AllocRecordStackTraceElement stack_[kMaxAllocRecordStackDepth];
};
class Breakpoint {
public:
  Breakpoint(mirror::ArtMethod* method, uint32_t dex_pc, bool need_full_deoptimization)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
    : method_(nullptr), dex_pc_(dex_pc), need_full_deoptimization_(need_full_deoptimization) {
    ScopedObjectAccessUnchecked soa(Thread::Current());
    method_ = soa.EncodeMethod(method);
    }
Breakpoint(const Breakpoint& other) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
                                      : method_(nullptr), dex_pc_(other.dex_pc_),
                                      need_full_deoptimization_(other.need_full_deoptimization_) {
                                      ScopedObjectAccessUnchecked soa(Thread::Current());
                                      method_ = soa.EncodeMethod(other.Method());
                                      }
mirror::ArtMethod* Method() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
                                    ScopedObjectAccessUnchecked soa(Thread::Current());
                                    return soa.DecodeMethod(method_);
                                    }
  uint32_t DexPc() const {
    return dex_pc_;
  }
  bool NeedFullDeoptimization() const {
    return need_full_deoptimization_;
  }
private:
  jmethodID method_;
  uint32_t dex_pc_;
  bool need_full_deoptimization_;
};
static std::ostream& operator<<(std::ostream& os, const Breakpoint& rhs)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    os << StringPrintf("Breakpoint[%s @%#x]", PrettyMethod(rhs.Method()).c_str(), rhs.DexPc());
    return os;
    }
{
 public:
  DebugInstrumentationListener() {}
  virtual ~DebugInstrumentationListener() {}
  void MethodEntered(Thread* thread, mirror::Object* this_object, mirror::ArtMethod* method,
                     uint32_t dex_pc)
      OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (method->IsNative()) {
      return;
    }
    Dbg::UpdateDebugger(thread, this_object, method, 0, Dbg::kMethodEntry, nullptr);
  }
  void MethodExited(Thread* thread, mirror::Object* this_object, mirror::ArtMethod* method,
                    uint32_t dex_pc, const JValue& return_value)
      OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (method->IsNative()) {
      return;
    }
    Dbg::UpdateDebugger(thread, this_object, method, dex_pc, Dbg::kMethodExit, &return_value);
  }
  void MethodUnwind(Thread* thread, mirror::Object* this_object, mirror::ArtMethod* method,
                    uint32_t dex_pc)
      OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    LOG(ERROR) << "Unexpected method unwind event in debugger " << PrettyMethod(method)
               << " " << dex_pc;
  }
  void DexPcMoved(Thread* thread, mirror::Object* this_object, mirror::ArtMethod* method,
                  uint32_t new_dex_pc)
      OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Dbg::UpdateDebugger(thread, this_object, method, new_dex_pc, 0, nullptr);
  }
  void FieldRead(Thread* thread, mirror::Object* this_object, mirror::ArtMethod* method,
                 uint32_t dex_pc, mirror::ArtField* field)
      OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Dbg::PostFieldAccessEvent(method, dex_pc, this_object, field);
  }
  void FieldWritten(Thread* thread, mirror::Object* this_object, mirror::ArtMethod* method,
                    uint32_t dex_pc, mirror::ArtField* field, const JValue& field_value)
      OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Dbg::PostFieldModificationEvent(method, dex_pc, this_object, field, &field_value);
  }
  void ExceptionCaught(Thread* thread, const ThrowLocation& throw_location,
                       mirror::ArtMethod* catch_method, uint32_t catch_dex_pc,
                       mirror::Throwable* exception_object)
      OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Dbg::PostException(throw_location, catch_method, catch_dex_pc, exception_object);
  }
 private:
  DISALLOW_COPY_AND_ASSIGN(DebugInstrumentationListener);
}
static bool gJdwpAllowed = true;
static bool gJdwpConfigured = false;
static JDWP::JdwpOptions gJdwpOptions;
static JDWP::JdwpState* gJdwpState = nullptr;
static bool gDebuggerConnected;
static bool gDebuggerActive;
static bool gDisposed;
static bool gDdmThreadNotification = false;
static Dbg::HpifWhen gDdmHpifWhen = Dbg::HPIF_WHEN_NEVER;
static Dbg::HpsgWhen gDdmHpsgWhen = Dbg::HPSG_WHEN_NEVER;
static Dbg::HpsgWhat gDdmHpsgWhat;
static Dbg::HpsgWhen gDdmNhsgWhen = Dbg::HPSG_WHEN_NEVER;
static Dbg::HpsgWhat gDdmNhsgWhat;
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
static std::vector<Breakpoint> gBreakpoints GUARDED_BY(Locks::breakpoint_lock_);
void DebugInvokeReq::VisitRoots(RootCallback* callback, void* arg, uint32_t tid,
                                RootType root_type) {
  if (receiver != nullptr) {
    callback(&receiver, arg, tid, root_type);
  }
  if (thread != nullptr) {
    callback(&thread, arg, tid, root_type);
  }
  if (klass != nullptr) {
    callback(reinterpret_cast<mirror::Object**>(&klass), arg, tid, root_type);
  }
  if (method != nullptr) {
    callback(reinterpret_cast<mirror::Object**>(&method), arg, tid, root_type);
  }
}
void DebugInvokeReq::Clear() {
  invoke_needed = false;
  receiver = nullptr;
  thread = nullptr;
  klass = nullptr;
  method = nullptr;
}
void SingleStepControl::VisitRoots(RootCallback* callback, void* arg, uint32_t tid,
                                   RootType root_type) {
  if (method != nullptr) {
    callback(reinterpret_cast<mirror::Object**>(&method), arg, tid, root_type);
  }
}
bool SingleStepControl::ContainsDexPc(uint32_t dex_pc) const {
  return dex_pcs.find(dex_pc) == dex_pcs.end();
}
void SingleStepControl::Clear() {
  is_active = false;
  method = nullptr;
  dex_pcs.clear();
}
static bool IsBreakpoint(const mirror::ArtMethod* m, uint32_t dex_pc)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ReaderMutexLock mu(Thread::Current(), *Locks::breakpoint_lock_);
    for (size_t i = 0, e = gBreakpoints.size(); i < e; ++i) {
    if (gBreakpoints[i].DexPc() == dex_pc && gBreakpoints[i].Method() == m) {
      VLOG(jdwp) << "Hit breakpoint #" << i << ": " << gBreakpoints[i];
      return true;
    }
    }
    return false;
    }
static bool IsSuspendedForDebugger(ScopedObjectAccessUnchecked& soa, Thread* thread)
    LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_) {
    MutexLock mu(soa.Self(), *Locks::thread_suspend_count_lock_);
    return thread->IsSuspended() && thread->GetDebugSuspendCount() > 0;
    }
static mirror::Array* DecodeNonNullArray(JDWP::RefTypeId id, JDWP::JdwpError* error)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::Object* o = Dbg::GetObjectRegistry()->Get<mirror::Object*>(id, error);
    if (o == nullptr) {
    *error = JDWP::ERR_INVALID_OBJECT;
    return nullptr;
    }
    if (!o->IsArrayInstance()) {
    *error = JDWP::ERR_INVALID_ARRAY;
    return nullptr;
    }
    *error = JDWP::ERR_NONE;
    return o->AsArray();
    }
static Thread* DecodeThread(ScopedObjectAccessUnchecked& soa, JDWP::ObjectId thread_id,
                            JDWP::JdwpError* error)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::Object* thread_peer = Dbg::GetObjectRegistry()->Get<mirror::Object*>(thread_id, error);
    if (thread_peer == nullptr) {
    *error = JDWP::ERR_INVALID_OBJECT;
    return nullptr;
    }
    mirror::Class* java_lang_Thread = soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_Thread);
    if (!java_lang_Thread->IsAssignableFrom(thread_peer->GetClass())) {
    *error = JDWP::ERR_INVALID_THREAD;
    return nullptr;
    }
    Thread* thread = Thread::FromManagedThread(soa, thread_peer);
    *error = (thread == nullptr) ? JDWP::ERR_THREAD_NOT_ALIVE : JDWP::ERR_NONE;
    return thread;
    }
static JDWP::JdwpTag BasicTagFromDescriptor(const char* descriptor) {
  return static_cast<JDWP::JdwpTag>(descriptor[0]);
}
static JDWP::JdwpTag BasicTagFromClass(mirror::Class* klass)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    std::string temp;
    const char* descriptor = klass->GetDescriptor(&temp);
    return BasicTagFromDescriptor(descriptor);
    }
static JDWP::JdwpTag TagFromClass(const ScopedObjectAccessUnchecked& soa, mirror::Class* c)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    CHECK(c != nullptr);
    if (c->IsArrayClass()) {
    return JDWP::JT_ARRAY;
    }
    if (c->IsStringClass()) {
    return JDWP::JT_STRING;
    }
    if (c->IsClassClass()) {
    return JDWP::JT_CLASS_OBJECT;
    }
    {
    mirror::Class* thread_class = soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_Thread);
    if (thread_class->IsAssignableFrom(c)) {
      return JDWP::JT_THREAD;
    }
    }
    {
    mirror::Class* thread_group_class =
        soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_ThreadGroup);
    if (thread_group_class->IsAssignableFrom(c)) {
      return JDWP::JT_THREAD_GROUP;
    }
    }
    {
    mirror::Class* class_loader_class =
        soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_ClassLoader);
    if (class_loader_class->IsAssignableFrom(c)) {
      return JDWP::JT_CLASS_LOADER;
    }
    }
    return JDWP::JT_OBJECT;
    }
JDWP::JdwpTag Dbg::TagFromObject(const ScopedObjectAccessUnchecked& soa, mirror::Object* o) {
  return (o == nullptr) ? JDWP::JT_OBJECT : TagFromClass(soa, o->GetClass());
}
static bool IsPrimitiveTag(JDWP::JdwpTag tag) {
  switch (tag) {
  case JDWP::JT_BOOLEAN:
  case JDWP::JT_BYTE:
  case JDWP::JT_CHAR:
  case JDWP::JT_FLOAT:
  case JDWP::JT_DOUBLE:
  case JDWP::JT_INT:
  case JDWP::JT_LONG:
  case JDWP::JT_SHORT:
  case JDWP::JT_VOID:
    return true;
  default:
    return false;
  }
}
static bool ParseJdwpOption(const std::string& name, const std::string& value) {
  if (name == "transport") {
    if (value == "dt_socket") {
      gJdwpOptions.transport = JDWP::kJdwpTransportSocket;
    } else if (value == "dt_android_adb") {
      gJdwpOptions.transport = JDWP::kJdwpTransportAndroidAdb;
    } else {
      LOG(ERROR) << "JDWP transport not supported: " << value;
      return false;
    }
  } else if (name == "server") {
    if (value == "n") {
      gJdwpOptions.server = false;
    } else if (value == "y") {
      gJdwpOptions.server = true;
    } else {
      LOG(ERROR) << "JDWP option 'server' must be 'y' or 'n'";
      return false;
    }
  } else if (name == "suspend") {
    if (value == "n") {
      gJdwpOptions.suspend = false;
    } else if (value == "y") {
      gJdwpOptions.suspend = true;
    } else {
      LOG(ERROR) << "JDWP option 'suspend' must be 'y' or 'n'";
      return false;
    }
  } else if (name == "address") {
    std::string port_string;
    gJdwpOptions.host.clear();
    std::string::size_type colon = value.find(':');
    if (colon != std::string::npos) {
      gJdwpOptions.host = value.substr(0, colon);
      port_string = value.substr(colon + 1);
    } else {
      port_string = value;
    }
    if (port_string.empty()) {
      LOG(ERROR) << "JDWP address missing port: " << value;
      return false;
    }
    char* end;
    uint64_t port = strtoul(port_string.c_str(), &end, 10);
    if (*end != '\0' || port > 0xffff) {
      LOG(ERROR) << "JDWP address has junk in port field: " << value;
      return false;
    }
    gJdwpOptions.port = port;
  } else if (name == "launch" || name == "onthrow" || name == "oncaught" || name == "timeout") {
    LOG(INFO) << "Ignoring JDWP option '" << name << "'='" << value << "'";
  } else {
    LOG(INFO) << "Ignoring unrecognized JDWP option '" << name << "'='" << value << "'";
  }
  return true;
}
bool Dbg::ParseJdwpOptions(const std::string& options) {
  VLOG(jdwp) << "ParseJdwpOptions: " << options;
  std::vector<std::string> pairs;
  Split(options, ',', pairs);
  for (size_t i = 0; i < pairs.size(); ++i) {
    std::string::size_type equals = pairs[i].find('=');
    if (equals == std::string::npos) {
      LOG(ERROR) << "Can't parse JDWP option '" << pairs[i] << "' in '" << options << "'";
      return false;
    }
    ParseJdwpOption(pairs[i].substr(0, equals), pairs[i].substr(equals + 1));
  }
  if (gJdwpOptions.transport == JDWP::kJdwpTransportUnknown) {
    LOG(ERROR) << "Must specify JDWP transport: " << options;
  }
  if (!gJdwpOptions.server && (gJdwpOptions.host.empty() || gJdwpOptions.port == 0)) {
    LOG(ERROR) << "Must specify JDWP host and port when server=n: " << options;
    return false;
  }
  gJdwpConfigured = true;
  return true;
}
void Dbg::StartJdwp() {
  if (!gJdwpAllowed || !IsJdwpConfigured()) {
    return;
  }
  CHECK(gRegistry == nullptr);
  gRegistry = new ObjectRegistry;
  gJdwpState = JDWP::JdwpState::Create(&gJdwpOptions);
  if (gJdwpState == nullptr) {
    LOG(FATAL) << "Debugger thread failed to initialize";
  }
  if (gJdwpState->IsActive()) {
    ScopedObjectAccess soa(Thread::Current());
    if (!gJdwpState->PostVMStart()) {
      LOG(WARNING) << "Failed to post 'start' message to debugger";
    }
  }
}
void Dbg::StopJdwp() {
  if (gJdwpState != nullptr && gJdwpState->IsActive()) {
    gJdwpState->PostVMDeath();
  }
  Disposed();
  delete gJdwpState;
  gJdwpState = nullptr;
  delete gRegistry;
  gRegistry = nullptr;
}
void Dbg::GcDidFinish() {
  if (gDdmHpifWhen != HPIF_WHEN_NEVER) {
    ScopedObjectAccess soa(Thread::Current());
    VLOG(jdwp) << "Sending heap info to DDM";
    DdmSendHeapInfo(gDdmHpifWhen);
  }
  if (gDdmHpsgWhen != HPSG_WHEN_NEVER) {
    ScopedObjectAccess soa(Thread::Current());
    VLOG(jdwp) << "Dumping heap to DDM";
    DdmSendHeapSegments(false);
  }
  if (gDdmNhsgWhen != HPSG_WHEN_NEVER) {
    ScopedObjectAccess soa(Thread::Current());
    VLOG(jdwp) << "Dumping native heap to DDM";
    DdmSendHeapSegments(true);
  }
}
void Dbg::SetJdwpAllowed(bool allowed) {
  gJdwpAllowed = allowed;
}
DebugInvokeReq* Dbg::GetInvokeReq() {
  return Thread::Current()->GetInvokeReq();
}
Thread* Dbg::GetDebugThread() {
  return (gJdwpState != nullptr) ? gJdwpState->GetDebugThread() : nullptr;
}
void Dbg::ClearWaitForEventThread() {
  gJdwpState->ClearWaitForEventThread();
}
void Dbg::Connected() {
  CHECK(!gDebuggerConnected);
  VLOG(jdwp) << "JDWP has attached";
  gDebuggerConnected = true;
  gDisposed = false;
}
void Dbg::Disposed() {
  gDisposed = true;
}
bool Dbg::IsDisposed() {
  return gDisposed;
}
void Dbg::GoActive() {
  if (gDebuggerActive) {
    return;
  }
  {
    ReaderMutexLock mu(Thread::Current(), *Locks::breakpoint_lock_);
    CHECK_EQ(gBreakpoints.size(), 0U);
  }
  {
    MutexLock mu(Thread::Current(), *Locks::deoptimization_lock_);
    CHECK_EQ(deoptimization_requests_.size(), 0U);
    CHECK_EQ(full_deoptimization_event_count_, 0U);
    CHECK_EQ(delayed_full_undeoptimization_count_, 0U);
    CHECK_EQ(dex_pc_change_event_ref_count_, 0U);
    CHECK_EQ(method_enter_event_ref_count_, 0U);
    CHECK_EQ(method_exit_event_ref_count_, 0U);
    CHECK_EQ(field_read_event_ref_count_, 0U);
    CHECK_EQ(field_write_event_ref_count_, 0U);
    CHECK_EQ(exception_catch_event_ref_count_, 0U);
  }
  Runtime* runtime = Runtime::Current();
  runtime->GetThreadList()->SuspendAll();
  Thread* self = Thread::Current();
  ThreadState old_state = self->SetStateUnsafe(kRunnable);
  CHECK_NE(old_state, kRunnable);
  runtime->GetInstrumentation()->EnableDeoptimization();
  instrumentation_events_ = 0;
  gDebuggerActive = true;
  CHECK_EQ(self->SetStateUnsafe(old_state), kRunnable);
  runtime->GetThreadList()->ResumeAll();
  LOG(INFO) << "Debugger is active";
}
void Dbg::Disconnected() {
  CHECK(gDebuggerConnected);
  LOG(INFO) << "Debugger is no longer active";
  Runtime* runtime = Runtime::Current();
  runtime->GetThreadList()->SuspendAll();
  Thread* self = Thread::Current();
  ThreadState old_state = self->SetStateUnsafe(kRunnable);
  if (gDebuggerActive) {
    {
      MutexLock mu(Thread::Current(), *Locks::deoptimization_lock_);
      deoptimization_requests_.clear();
      full_deoptimization_event_count_ = 0U;
      delayed_full_undeoptimization_count_ = 0U;
    }
    if (instrumentation_events_ != 0) {
      runtime->GetInstrumentation()->RemoveListener(&gDebugInstrumentationListener,
                                                    instrumentation_events_);
      instrumentation_events_ = 0;
    }
    runtime->GetInstrumentation()->DisableDeoptimization();
    gDebuggerActive = false;
  }
  gRegistry->Clear();
  gDebuggerConnected = false;
  CHECK_EQ(self->SetStateUnsafe(old_state), kRunnable);
  runtime->GetThreadList()->ResumeAll();
}
bool Dbg::IsDebuggerActive() {
  return gDebuggerActive;
}
bool Dbg::IsJdwpConfigured() {
  return gJdwpConfigured;
}
int64_t Dbg::LastDebuggerActivity() {
  return gJdwpState->LastDebuggerActivity();
}
void Dbg::UndoDebuggerSuspensions() {
  Runtime::Current()->GetThreadList()->UndoDebuggerSuspensions();
}
std::string Dbg::GetClassName(JDWP::RefTypeId class_id) {
  JDWP::JdwpError error;
  mirror::Object* o = gRegistry->Get<mirror::Object*>(class_id, &error);
  if (o == nullptr) {
    if (error == JDWP::ERR_NONE) {
      return "NULL";
    } else {
      return StringPrintf("invalid object %p", reinterpret_cast<void*>(class_id));
    }
  }
  if (!o->IsClass()) {
    return StringPrintf("non-class %p", o);
  }
  return GetClassName(o->AsClass());
}
std::string Dbg::GetClassName(mirror::Class* klass) {
  if (klass == nullptr) {
    return "NULL";
  }
  std::string temp;
  return DescriptorToName(klass->GetDescriptor(&temp));
}
JDWP::JdwpError Dbg::GetClassObject(JDWP::RefTypeId id, JDWP::ObjectId* class_object_id) {
  JDWP::JdwpError status;
  mirror::Class* c = DecodeClass(id, &status);
  if (c == nullptr) {
    *class_object_id = 0;
    return status;
  }
  *class_object_id = gRegistry->Add(c);
  return JDWP::ERR_NONE;
}
JDWP::JdwpError Dbg::GetSuperclass(JDWP::RefTypeId id, JDWP::RefTypeId* superclass_id) {
  JDWP::JdwpError status;
  mirror::Class* c = DecodeClass(id, &status);
  if (c == nullptr) {
    *superclass_id = 0;
    return status;
  }
  if (c->IsInterface()) {
    *superclass_id = 0;
  } else {
    *superclass_id = gRegistry->Add(c->GetSuperClass());
  }
  return JDWP::ERR_NONE;
}
JDWP::JdwpError Dbg::GetClassLoader(JDWP::RefTypeId id, JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError error;
  mirror::Object* o = gRegistry->Get<mirror::Object*>(id, &error);
  if (o == nullptr) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  expandBufAddObjectId(pReply, gRegistry->Add(o->GetClass()->GetClassLoader()));
  return JDWP::ERR_NONE;
}
JDWP::JdwpError Dbg::GetModifiers(JDWP::RefTypeId id, JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError error;
  mirror::Class* c = DecodeClass(id, &error);
  if (c == nullptr) {
    return error;
  }
  uint32_t access_flags = c->GetAccessFlags() & kAccJavaFlagsMask;
  if ((access_flags & kAccInterface) == 0) {
    access_flags |= kAccSuper;
  }
  expandBufAdd4BE(pReply, access_flags);
  return JDWP::ERR_NONE;
}
JDWP::JdwpError Dbg::GetMonitorInfo(JDWP::ObjectId object_id, JDWP::ExpandBuf* reply) {
  JDWP::JdwpError error;
  mirror::Object* o = gRegistry->Get<mirror::Object*>(object_id, &error);
  if (o == nullptr) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  Thread* self = Thread::Current();
  CHECK_EQ(self->GetState(), kRunnable);
  self->TransitionFromRunnableToSuspended(kSuspended);
  Runtime::Current()->GetThreadList()->SuspendAll();
  MonitorInfo monitor_info(o);
  Runtime::Current()->GetThreadList()->ResumeAll();
  self->TransitionFromSuspendedToRunnable();
  if (monitor_info.owner_ != nullptr) {
    expandBufAddObjectId(reply, gRegistry->Add(monitor_info.owner_->GetPeer()));
  } else {
    expandBufAddObjectId(reply, gRegistry->Add(nullptr));
  }
  expandBufAdd4BE(reply, monitor_info.entry_count_);
  expandBufAdd4BE(reply, monitor_info.waiters_.size());
  for (size_t i = 0; i < monitor_info.waiters_.size(); ++i) {
    expandBufAddObjectId(reply, gRegistry->Add(monitor_info.waiters_[i]->GetPeer()));
  }
  return JDWP::ERR_NONE;
}
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
JDWP::JdwpError Dbg::GetOwnedMonitors(JDWP::ObjectId thread_id,
                                      std::vector<JDWP::ObjectId>* monitors,
                                      std::vector<uint32_t>* stack_depths) {
  struct OwnedMonitorVisitor : public StackVisitor {
    OwnedMonitorVisitor(Thread* thread, Context* context,
                        std::vector<JDWP::ObjectId>* monitor_vector,
                        std::vector<uint32_t>* stack_depth_vector)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), current_stack_depth(0),
        monitors(monitor_vector), stack_depths(stack_depth_vector) {}
    bool VisitFrame() NO_THREAD_SAFETY_ANALYSIS {
      if (!GetMethod()->IsRuntimeMethod()) {
        Monitor::VisitLocks(this, AppendOwnedMonitors, this);
        ++current_stack_depth;
      }
      return true;
    }
    static void AppendOwnedMonitors(mirror::Object* owned_monitor, void* arg)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      OwnedMonitorVisitor* visitor = reinterpret_cast<OwnedMonitorVisitor*>(arg);
      visitor->monitors->push_back(gRegistry->Add(owned_monitor));
      visitor->stack_depths->push_back(visitor->current_stack_depth);
    }
    size_t current_stack_depth;
    std::vector<JDWP::ObjectId>* const monitors;
    std::vector<uint32_t>* const stack_depths;
  };
  ScopedObjectAccessUnchecked soa(Thread::Current());
  Thread* thread;
  {
    MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
    JDWP::JdwpError error;
    thread = DecodeThread(soa, thread_id, &error);
    if (thread == nullptr) {
      return error;
    }
    if (!IsSuspendedForDebugger(soa, thread)) {
      return JDWP::ERR_THREAD_NOT_SUSPENDED;
    }
  }
  std::unique_ptr<Context> context(Context::Create());
  OwnedMonitorVisitor visitor(thread, context.get(), monitors, stack_depths);
  visitor.WalkStack();
  return JDWP::ERR_NONE;
}
JDWP::JdwpError Dbg::GetContendedMonitor(JDWP::ObjectId thread_id,
                                         JDWP::ObjectId* contended_monitor) {
  mirror::Object* contended_monitor_obj;
  ScopedObjectAccessUnchecked soa(Thread::Current());
  *contended_monitor = 0;
  {
    MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
    JDWP::JdwpError error;
    Thread* thread = DecodeThread(soa, thread_id, &error);
    if (thread == nullptr) {
      return error;
    }
    if (!IsSuspendedForDebugger(soa, thread)) {
      return JDWP::ERR_THREAD_NOT_SUSPENDED;
    }
    contended_monitor_obj = Monitor::GetContendedMonitor(thread);
  }
  *contended_monitor = gRegistry->Add(contended_monitor_obj);
  return JDWP::ERR_NONE;
}
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count)
JDWP::JdwpTypeTag Dbg::GetTypeTag(mirror::Class* klass) {
  DCHECK(klass != nullptr);
  if (klass->IsArrayClass()) {
    return JDWP::TT_ARRAY;
  } else if (klass->IsInterface()) {
    return JDWP::TT_INTERFACE;
  } else {
    return JDWP::TT_CLASS;
  }
}
JDWP::JdwpError Dbg::GetReflectedType(JDWP::RefTypeId class_id, JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError error;
  mirror::Class* c = DecodeClass(class_id, &error);
  if (c == nullptr) {
    return error;
  }
  JDWP::JdwpTypeTag type_tag = GetTypeTag(c);
  expandBufAdd1(pReply, type_tag);
  expandBufAddRefTypeId(pReply, class_id);
  return JDWP::ERR_NONE;
}
void Dbg::GetClassList(std::vector<JDWP::RefTypeId>* classes) {
  struct ClassListCreator {
    explicit ClassListCreator(std::vector<JDWP::RefTypeId>* classes) : classes(classes) {
    }
    static bool Visit(mirror::Class* c, void* arg) {
      return reinterpret_cast<ClassListCreator*>(arg)->Visit(c);
    }
    bool Visit(mirror::Class* c) NO_THREAD_SAFETY_ANALYSIS {
      if (!c->IsPrimitive()) {
        classes->push_back(gRegistry->AddRefType(c));
      }
      return true;
    }
    std::vector<JDWP::RefTypeId>* const classes;
  };
  ClassListCreator clc(classes);
  Runtime::Current()->GetClassLinker()->VisitClassesWithoutClassesLock(ClassListCreator::Visit,
                                                                       &clc);
}
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  JDWP::FrameId frame_id;
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), this_object(nullptr), frame_id(frame_id) {}
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  JDWP::FrameId frame_id;
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), this_object(nullptr), frame_id(frame_id) {}
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  JDWP::FrameId frame_id;
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), this_object(nullptr), frame_id(frame_id) {}
  JDWP::FrameId frame_id;
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), this_object(nullptr), frame_id(frame_id) {}
  JDWP::FrameId frame_id;
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), this_object(nullptr), frame_id(frame_id) {}
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  JDWP::FrameId frame_id;
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), this_object(nullptr), frame_id(frame_id) {}
  JDWP::FrameId frame_id;
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), this_object(nullptr), frame_id(frame_id) {}
  JDWP::FrameId frame_id;
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), this_object(nullptr), frame_id(frame_id) {}
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  JDWP::FrameId frame_id;
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), this_object(nullptr), frame_id(frame_id) {}
  JDWP::FrameId frame_id;
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), this_object(nullptr), frame_id(frame_id) {}
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  JDWP::FrameId frame_id;
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), this_object(nullptr), frame_id(frame_id) {}
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  JDWP::FrameId frame_id;
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), this_object(nullptr), frame_id(frame_id) {}
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  JDWP::FrameId frame_id;
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), this_object(nullptr), frame_id(frame_id) {}
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  JDWP::FrameId frame_id;
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), this_object(nullptr), frame_id(frame_id) {}
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  JDWP::FrameId frame_id;
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), this_object(nullptr), frame_id(frame_id) {}
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  JDWP::FrameId frame_id;
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), this_object(nullptr), frame_id(frame_id) {}
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
struct GetThisVisitor : public StackVisitor {
  JDWP::FrameId frame_id;
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), this_object(nullptr), frame_id(frame_id) {}
  {
    if (frame_id != GetFrameId()) {
      return true;
    } else {
      this_object = GetThisObject();
      return false;
    }
  }
  JDWP::FrameId frame_id;
  JDWP::FrameId frame_id;
};
JDWP::JdwpError Dbg::GetThisObject(JDWP::ObjectId thread_id, JDWP::FrameId frame_id,
                                   JDWP::ObjectId* result) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  Thread* thread;
  {
    MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
    JDWP::JdwpError error;
    thread = DecodeThread(soa, thread_id, &error);
    if (error != JDWP::ERR_NONE) {
      return error;
    }
    if (!IsSuspendedForDebugger(soa, thread)) {
      return JDWP::ERR_THREAD_NOT_SUSPENDED;
    }
  }
  std::unique_ptr<Context> context(Context::Create());
  GetThisVisitor visitor(thread, context.get(), frame_id);
  visitor.WalkStack();
  *result = gRegistry->Add(visitor.this_object);
  return JDWP::ERR_NONE;
}
: StackVisitor(thread, context), frame_id_(frame_id), error_(JDWP::ERR_INVALID_FRAMEID) {
 public:
  FindFrameVisitor(Thread* thread, Context* context, JDWP::FrameId frame_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), frame_id_(frame_id), error_(JDWP::ERR_INVALID_FRAMEID) {}
  bool VisitFrame() NO_THREAD_SAFETY_ANALYSIS {
    if (GetFrameId() != frame_id_) {
      return true;
    }
    mirror::ArtMethod* m = GetMethod();
    if (m->IsNative()) {
      error_ = JDWP::ERR_OPAQUE_FRAME;
    } else {
      error_ = JDWP::ERR_NONE;
    }
    return false;
  }
  JDWP::JdwpError GetError() const {
    return error_;
  }
 private:
  const JDWP::FrameId frame_id_;
  JDWP::JdwpError error_;
}JDWP::JdwpError Dbg::GetLocalValues(JDWP::Request* request, JDWP::ExpandBuf* pReply) {
  JDWP::ObjectId thread_id = request->ReadThreadId();
  JDWP::FrameId frame_id = request->ReadFrameId();
  ScopedObjectAccessUnchecked soa(Thread::Current());
  Thread* thread;
  {
    MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
    JDWP::JdwpError error = DecodeThread(soa, thread_id, thread);
    if (error != JDWP::ERR_NONE) {
      return error;
    }
  }
  std::unique_ptr<Context> context(Context::Create());
  FindFrameVisitor visitor(thread, context.get(), frame_id);
  visitor.WalkStack();
  if (visitor.GetError() != JDWP::ERR_NONE) {
    return visitor.GetError();
  }
  int32_t slot_count = request->ReadSigned32("slot count");
  expandBufAdd4BE(pReply, slot_count);
  for (int32_t i = 0; i < slot_count; ++i) {
    uint32_t slot = request->ReadUnsigned32("slot");
    JDWP::JdwpTag reqSigByte = request->ReadTag();
    VLOG(jdwp) << "    --> slot " << slot << " " << reqSigByte;
    size_t width = Dbg::GetTagWidth(reqSigByte);
    uint8_t* ptr = expandBufAddSpace(pReply, width+1);
    JDWP::JdwpError error = Dbg::GetLocalValue(visitor, soa, slot, reqSigByte, ptr, width);
    if (error != JDWP::ERR_NONE) {
      return error;
    }
  }
  return JDWP::ERR_NONE;
}
JDWP::JdwpError Dbg::GetLocalValue(const StackVisitor& visitor, ScopedObjectAccessUnchecked& soa, int slot, JDWP::JdwpTag tag, uint8_t* buf, size_t width) {
  mirror::ArtMethod* m = visitor.GetMethod();
  uint16_t reg = DemangleSlot(slot, m);
  constexpr JDWP::JdwpError kFailureErrorCode = JDWP::ERR_ABSENT_INFORMATION;
  switch (tag) {
    case JDWP::JT_BOOLEAN: {
      CHECK_EQ(width, 1U);
      uint32_t intVal;
      if (visitor.GetVReg(m, reg, kIntVReg, &intVal)) {
        VLOG(jdwp) << "get boolean local " << reg << " = " << intVal;
        JDWP::Set1(buf + 1, intVal != 0);
      } else {
        VLOG(jdwp) << "failed to get boolean local " << reg;
        return kFailureErrorCode;
      }
      break;
    }
    case JDWP::JT_BYTE: {
      CHECK_EQ(width, 1U);
      uint32_t intVal;
      if (visitor.GetVReg(m, reg, kIntVReg, &intVal)) {
        VLOG(jdwp) << "get byte local " << reg << " = " << intVal;
        JDWP::Set1(buf + 1, intVal);
      } else {
        VLOG(jdwp) << "failed to get byte local " << reg;
        return kFailureErrorCode;
      }
      break;
    }
    case JDWP::JT_SHORT:
    case JDWP::JT_CHAR: {
      CHECK_EQ(width, 2U);
      uint32_t intVal;
      if (visitor.GetVReg(m, reg, kIntVReg, &intVal)) {
        VLOG(jdwp) << "get short/char local " << reg << " = " << intVal;
        JDWP::Set2BE(buf + 1, intVal);
      } else {
        VLOG(jdwp) << "failed to get short/char local " << reg;
        return kFailureErrorCode;
      }
      break;
    }
    case JDWP::JT_INT: {
      CHECK_EQ(width, 4U);
      uint32_t intVal;
      if (visitor.GetVReg(m, reg, kIntVReg, &intVal)) {
        VLOG(jdwp) << "get int local " << reg << " = " << intVal;
        JDWP::Set4BE(buf + 1, intVal);
      } else {
        VLOG(jdwp) << "failed to get int local " << reg;
        return kFailureErrorCode;
      }
      break;
    }
    case JDWP::JT_FLOAT: {
      CHECK_EQ(width, 4U);
      uint32_t intVal;
      if (visitor.GetVReg(m, reg, kFloatVReg, &intVal)) {
        VLOG(jdwp) << "get float local " << reg << " = " << intVal;
        JDWP::Set4BE(buf + 1, intVal);
      } else {
        VLOG(jdwp) << "failed to get float local " << reg;
        return kFailureErrorCode;
      }
      break;
    }
    case JDWP::JT_ARRAY:
    case JDWP::JT_CLASS_LOADER:
    case JDWP::JT_CLASS_OBJECT:
    case JDWP::JT_OBJECT:
    case JDWP::JT_STRING:
    case JDWP::JT_THREAD:
    case JDWP::JT_THREAD_GROUP: {
      CHECK_EQ(width, sizeof(JDWP::ObjectId));
      uint32_t intVal;
      if (visitor.GetVReg(m, reg, kReferenceVReg, &intVal)) {
        mirror::Object* o = reinterpret_cast<mirror::Object*>(intVal);
        VLOG(jdwp) << "get " << tag << " object local " << reg << " = " << o;
        if (!Runtime::Current()->GetHeap()->IsValidObjectAddress(o)) {
          LOG(FATAL) << "Register " << reg << " expected to hold " << tag << " object: " << o;
        }
        tag = TagFromObject(soa, o);
        JDWP::SetObjectId(buf + 1, gRegistry->Add(o));
      } else {
        VLOG(jdwp) << "failed to get " << tag << " object local " << reg;
        return kFailureErrorCode;
      }
      break;
    }
    case JDWP::JT_DOUBLE: {
      CHECK_EQ(width, 8U);
      uint64_t longVal;
      if (visitor.GetVRegPair(m, reg, kDoubleLoVReg, kDoubleHiVReg, &longVal)) {
        VLOG(jdwp) << "get double local " << reg << " = " << longVal;
        JDWP::Set8BE(buf + 1, longVal);
      } else {
        VLOG(jdwp) << "failed to get double local " << reg;
        return kFailureErrorCode;
      }
      break;
    }
    case JDWP::JT_LONG: {
      CHECK_EQ(width, 8U);
      uint64_t longVal;
      if (visitor.GetVRegPair(m, reg, kLongLoVReg, kLongHiVReg, &longVal)) {
        VLOG(jdwp) << "get long local " << reg << " = " << longVal;
        JDWP::Set8BE(buf + 1, longVal);
      } else {
        VLOG(jdwp) << "failed to get long local " << reg;
        return kFailureErrorCode;
      }
      break;
    }
    default:
      LOG(FATAL) << "Unknown tag " << tag;
      break;
  }
  JDWP::Set1(buf, tag);
  return JDWP::ERR_NONE;
}
JDWP::JdwpError Dbg::SetLocalValues(JDWP::Request* request) {
  JDWP::ObjectId thread_id = request->ReadThreadId();
  JDWP::FrameId frame_id = request->ReadFrameId();
  ScopedObjectAccessUnchecked soa(Thread::Current());
  Thread* thread;
  {
    MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
    JDWP::JdwpError error = DecodeThread(soa, thread_id, thread);
    if (error != JDWP::ERR_NONE) {
      return error;
    }
  }
  std::unique_ptr<Context> context(Context::Create());
  FindFrameVisitor visitor(thread, context.get(), frame_id);
  visitor.WalkStack();
  if (visitor.GetError() != JDWP::ERR_NONE) {
    return visitor.GetError();
  }
  int32_t slot_count = request->ReadSigned32("slot count");
  for (int32_t i = 0; i < slot_count; ++i) {
    uint32_t slot = request->ReadUnsigned32("slot");
    JDWP::JdwpTag sigByte = request->ReadTag();
    size_t width = Dbg::GetTagWidth(sigByte);
    uint64_t value = request->ReadValue(width);
    VLOG(jdwp) << "    --> slot " << slot << " " << sigByte << " " << value;
    JDWP::JdwpError error = Dbg::SetLocalValue(visitor, slot, sigByte, value, width);
    if (error != JDWP::ERR_NONE) {
      return error;
    }
  }
  return JDWP::ERR_NONE;
}
JDWP::JdwpError Dbg::SetLocalValue(StackVisitor& visitor, int slot, JDWP::JdwpTag tag, uint64_t value, size_t width) {
  mirror::ArtMethod* m = visitor.GetMethod();
  uint16_t reg = DemangleSlot(slot, m);
  constexpr JDWP::JdwpError kFailureErrorCode = JDWP::ERR_ABSENT_INFORMATION;
  switch (tag) {
    case JDWP::JT_BOOLEAN:
    case JDWP::JT_BYTE:
      CHECK_EQ(width, 1U);
      if (!visitor.SetVReg(m, reg, static_cast<uint32_t>(value), kIntVReg)) {
        VLOG(jdwp) << "failed to set boolean/byte local " << reg << " = "
                   << static_cast<uint32_t>(value);
        return kFailureErrorCode;
      }
      break;
    case JDWP::JT_SHORT:
    case JDWP::JT_CHAR:
      CHECK_EQ(width, 2U);
      if (!visitor.SetVReg(m, reg, static_cast<uint32_t>(value), kIntVReg)) {
        VLOG(jdwp) << "failed to set short/char local " << reg << " = "
                   << static_cast<uint32_t>(value);
        return kFailureErrorCode;
      }
      break;
    case JDWP::JT_INT:
      CHECK_EQ(width, 4U);
      if (!visitor.SetVReg(m, reg, static_cast<uint32_t>(value), kIntVReg)) {
        VLOG(jdwp) << "failed to set int local " << reg << " = "
                   << static_cast<uint32_t>(value);
        return kFailureErrorCode;
      }
      break;
    case JDWP::JT_FLOAT:
      CHECK_EQ(width, 4U);
      if (!visitor.SetVReg(m, reg, static_cast<uint32_t>(value), kFloatVReg)) {
        VLOG(jdwp) << "failed to set float local " << reg << " = "
                   << static_cast<uint32_t>(value);
        return kFailureErrorCode;
      }
      break;
    case JDWP::JT_ARRAY:
    case JDWP::JT_CLASS_LOADER:
    case JDWP::JT_CLASS_OBJECT:
    case JDWP::JT_OBJECT:
    case JDWP::JT_STRING:
    case JDWP::JT_THREAD:
    case JDWP::JT_THREAD_GROUP: {
      CHECK_EQ(width, sizeof(JDWP::ObjectId));
      mirror::Object* o = gRegistry->Get<mirror::Object*>(static_cast<JDWP::ObjectId>(value));
      if (o == ObjectRegistry::kInvalidObject) {
        VLOG(jdwp) << tag << " object " << o << " is an invalid object";
        return JDWP::ERR_INVALID_OBJECT;
      } else if (!visitor.SetVReg(m, reg, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(o)),
                          kReferenceVReg)) {
        VLOG(jdwp) << "failed to set " << tag << " object local " << reg << " = " << o;
        return kFailureErrorCode;
      }
      break;
    }
    case JDWP::JT_DOUBLE: {
      CHECK_EQ(width, 8U);
      if (!visitor.SetVRegPair(m, reg, value, kDoubleLoVReg, kDoubleHiVReg)) {
        VLOG(jdwp) << "failed to set double local " << reg << " = " << value;
        return kFailureErrorCode;
      }
      break;
    }
    case JDWP::JT_LONG: {
      CHECK_EQ(width, 8U);
      if (!visitor.SetVRegPair(m, reg, value, kLongLoVReg, kLongHiVReg)) {
        VLOG(jdwp) << "failed to set double local " << reg << " = " << value;
        return kFailureErrorCode;
      }
      break;
    }
    default:
      LOG(FATAL) << "Unknown tag " << tag;
      break;
  }
  return JDWP::ERR_NONE;
}
static void SetEventLocation(JDWP::EventLocation* location, mirror::ArtMethod* m, uint32_t dex_pc)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(location != nullptr);
    if (m == nullptr) {
    memset(location, 0, sizeof(*location));
    } else {
    location->method = m;
    location->dex_pc = (m->IsNative() || m->IsProxyMethod()) ? static_cast<uint32_t>(-1) : dex_pc;
    }
    }
void Dbg::PostLocationEvent(mirror::ArtMethod* m, int dex_pc, mirror::Object* this_object,
                            int event_flags, const JValue* return_value) {
  if (!IsDebuggerActive()) {
    return;
  }
  DCHECK(m != nullptr);
  DCHECK_EQ(m->IsStatic(), this_object == nullptr);
  JDWP::EventLocation location;
  SetEventLocation(&location, m, dex_pc);
  gJdwpState->PostLocationEvent(&location, this_object, event_flags, return_value);
}
void Dbg::PostFieldAccessEvent(mirror::ArtMethod* m, int dex_pc,
                               mirror::Object* this_object, mirror::ArtField* f) {
  if (!IsDebuggerActive()) {
    return;
  }
  DCHECK(m != nullptr);
  DCHECK(f != nullptr);
  JDWP::EventLocation location;
  SetEventLocation(&location, m, dex_pc);
  gJdwpState->PostFieldEvent(&location, f, this_object, nullptr, false);
}
void Dbg::PostFieldModificationEvent(mirror::ArtMethod* m, int dex_pc,
                                     mirror::Object* this_object, mirror::ArtField* f,
                                     const JValue* field_value) {
  if (!IsDebuggerActive()) {
    return;
  }
  DCHECK(m != nullptr);
  DCHECK(f != nullptr);
  DCHECK(field_value != nullptr);
  JDWP::EventLocation location;
  SetEventLocation(&location, m, dex_pc);
  gJdwpState->PostFieldEvent(&location, f, this_object, field_value, true);
}
void Dbg::PostException(const ThrowLocation& throw_location,
                        mirror::ArtMethod* catch_method,
                        uint32_t catch_dex_pc, mirror::Throwable* exception_object) {
  if (!IsDebuggerActive()) {
    return;
  }
  JDWP::EventLocation exception_throw_location;
  SetEventLocation(&exception_throw_location, throw_location.GetMethod(), throw_location.GetDexPc());
  JDWP::EventLocation exception_catch_location;
  SetEventLocation(&exception_catch_location, catch_method, catch_dex_pc);
  gJdwpState->PostException(&exception_throw_location, exception_object, &exception_catch_location,
                            throw_location.GetThis());
}
void Dbg::PostClassPrepare(mirror::Class* c) {
  if (!IsDebuggerActive()) {
    return;
  }
  gJdwpState->PostClassPrepare(c);
}
void Dbg::UpdateDebugger(Thread* thread, mirror::Object* this_object,
                         mirror::ArtMethod* m, uint32_t dex_pc,
                         int event_flags, const JValue* return_value) {
  if (!IsDebuggerActive() || dex_pc == static_cast<uint32_t>(-2) ) {
    return;
  }
  if (IsBreakpoint(m, dex_pc)) {
    event_flags |= kBreakpoint;
  }
  const SingleStepControl* single_step_control = thread->GetSingleStepControl();
  DCHECK(single_step_control != nullptr);
  if (single_step_control->is_active) {
    CHECK(!m->IsNative());
    if (single_step_control->step_depth == JDWP::SD_INTO) {
      if (single_step_control->method != m) {
        event_flags |= kSingleStep;
        VLOG(jdwp) << "SS new method";
      } else if (single_step_control->step_size == JDWP::SS_MIN) {
        event_flags |= kSingleStep;
        VLOG(jdwp) << "SS new instruction";
      } else if (single_step_control->ContainsDexPc(dex_pc)) {
        event_flags |= kSingleStep;
        VLOG(jdwp) << "SS new line";
      }
    } else if (single_step_control->step_depth == JDWP::SD_OVER) {
      int stack_depth = GetStackDepth(thread);
      if (stack_depth < single_step_control->stack_depth) {
        event_flags |= kSingleStep;
        VLOG(jdwp) << "SS method pop";
      } else if (stack_depth == single_step_control->stack_depth) {
        if (single_step_control->step_size == JDWP::SS_MIN) {
          event_flags |= kSingleStep;
          VLOG(jdwp) << "SS new instruction";
        } else if (single_step_control->ContainsDexPc(dex_pc)) {
          event_flags |= kSingleStep;
          VLOG(jdwp) << "SS new line";
        }
      }
    } else {
      CHECK_EQ(single_step_control->step_depth, JDWP::SD_OUT);
      int stack_depth = GetStackDepth(thread);
      if (stack_depth < single_step_control->stack_depth) {
        event_flags |= kSingleStep;
        VLOG(jdwp) << "SS method pop";
      }
    }
  }
  if (event_flags != 0) {
    Dbg::PostLocationEvent(m, dex_pc, this_object, event_flags, return_value);
  }
}
size_t* Dbg::GetReferenceCounterForEvent(uint32_t instrumentation_event) {
  switch (instrumentation_event) {
    case instrumentation::Instrumentation::kMethodEntered:
      return &method_enter_event_ref_count_;
    case instrumentation::Instrumentation::kMethodExited:
      return &method_exit_event_ref_count_;
    case instrumentation::Instrumentation::kDexPcMoved:
      return &dex_pc_change_event_ref_count_;
    case instrumentation::Instrumentation::kFieldRead:
      return &field_read_event_ref_count_;
    case instrumentation::Instrumentation::kFieldWritten:
      return &field_write_event_ref_count_;
    case instrumentation::Instrumentation::kExceptionCaught:
      return &exception_catch_event_ref_count_;
    default:
      return nullptr;
  }
}
void Dbg::ProcessDeoptimizationRequest(const DeoptimizationRequest& request) {
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  switch (request.GetKind()) {
    case DeoptimizationRequest::kNothing:
      LOG(WARNING) << "Ignoring empty deoptimization request.";
      break;
    case DeoptimizationRequest::kRegisterForEvent:
      VLOG(jdwp) << StringPrintf("Add debugger as listener for instrumentation event 0x%x",
                                 request.InstrumentationEvent());
      instrumentation->AddListener(&gDebugInstrumentationListener, request.InstrumentationEvent());
      instrumentation_events_ |= request.InstrumentationEvent();
      break;
    case DeoptimizationRequest::kUnregisterForEvent:
      VLOG(jdwp) << StringPrintf("Remove debugger as listener for instrumentation event 0x%x",
                                 request.InstrumentationEvent());
      instrumentation->RemoveListener(&gDebugInstrumentationListener,
                                      request.InstrumentationEvent());
      instrumentation_events_ &= ~request.InstrumentationEvent();
      break;
    case DeoptimizationRequest::kFullDeoptimization:
      VLOG(jdwp) << "Deoptimize the world ...";
      instrumentation->DeoptimizeEverything();
      VLOG(jdwp) << "Deoptimize the world DONE";
      break;
    case DeoptimizationRequest::kFullUndeoptimization:
      VLOG(jdwp) << "Undeoptimize the world ...";
      instrumentation->UndeoptimizeEverything();
      VLOG(jdwp) << "Undeoptimize the world DONE";
      break;
    case DeoptimizationRequest::kSelectiveDeoptimization:
      VLOG(jdwp) << "Deoptimize method " << PrettyMethod(request.Method()) << " ...";
      instrumentation->Deoptimize(request.Method());
      VLOG(jdwp) << "Deoptimize method " << PrettyMethod(request.Method()) << " DONE";
      break;
    case DeoptimizationRequest::kSelectiveUndeoptimization:
      VLOG(jdwp) << "Undeoptimize method " << PrettyMethod(request.Method()) << " ...";
      instrumentation->Undeoptimize(request.Method());
      VLOG(jdwp) << "Undeoptimize method " << PrettyMethod(request.Method()) << " DONE";
      break;
    default:
      LOG(FATAL) << "Unsupported deoptimization request kind " << request.GetKind();
      break;
  }
}
void Dbg::DelayFullUndeoptimization() {
  MutexLock mu(Thread::Current(), *Locks::deoptimization_lock_);
  ++delayed_full_undeoptimization_count_;
  DCHECK_LE(delayed_full_undeoptimization_count_, full_deoptimization_event_count_);
}
void Dbg::ProcessDelayedFullUndeoptimizations() {
  {
    MutexLock mu(Thread::Current(), *Locks::deoptimization_lock_);
    while (delayed_full_undeoptimization_count_ > 0) {
      DeoptimizationRequest req;
      req.SetKind(DeoptimizationRequest::kFullUndeoptimization);
      req.SetMethod(nullptr);
      RequestDeoptimizationLocked(req);
      --delayed_full_undeoptimization_count_;
    }
  }
  ManageDeoptimization();
}
void Dbg::RequestDeoptimization(const DeoptimizationRequest& req) {
  if (req.GetKind() == DeoptimizationRequest::kNothing) {
    return;
  }
  MutexLock mu(Thread::Current(), *Locks::deoptimization_lock_);
  RequestDeoptimizationLocked(req);
}
void Dbg::RequestDeoptimizationLocked(const DeoptimizationRequest& req) {
  switch (req.GetKind()) {
    case DeoptimizationRequest::kRegisterForEvent: {
      DCHECK_NE(req.InstrumentationEvent(), 0u);
      size_t* counter = GetReferenceCounterForEvent(req.InstrumentationEvent());
      CHECK(counter != nullptr) << StringPrintf("No counter for instrumentation event 0x%x",
                                                req.InstrumentationEvent());
      if (*counter == 0) {
        VLOG(jdwp) << StringPrintf("Queue request #%zd to start listening to instrumentation event 0x%x",
                                   deoptimization_requests_.size(), req.InstrumentationEvent());
        deoptimization_requests_.push_back(req);
      }
      *counter = *counter + 1;
      break;
    }
    case DeoptimizationRequest::kUnregisterForEvent: {
      DCHECK_NE(req.InstrumentationEvent(), 0u);
      size_t* counter = GetReferenceCounterForEvent(req.InstrumentationEvent());
      CHECK(counter != nullptr) << StringPrintf("No counter for instrumentation event 0x%x",
                                                req.InstrumentationEvent());
      *counter = *counter - 1;
      if (*counter == 0) {
        VLOG(jdwp) << StringPrintf("Queue request #%zd to stop listening to instrumentation event 0x%x",
                                   deoptimization_requests_.size(), req.InstrumentationEvent());
        deoptimization_requests_.push_back(req);
      }
      break;
    }
    case DeoptimizationRequest::kFullDeoptimization: {
      DCHECK(req.Method() == nullptr);
      if (full_deoptimization_event_count_ == 0) {
        VLOG(jdwp) << "Queue request #" << deoptimization_requests_.size()
                   << " for full deoptimization";
        deoptimization_requests_.push_back(req);
      }
      ++full_deoptimization_event_count_;
      break;
    }
    case DeoptimizationRequest::kFullUndeoptimization: {
      DCHECK(req.Method() == nullptr);
      DCHECK_GT(full_deoptimization_event_count_, 0U);
      --full_deoptimization_event_count_;
      if (full_deoptimization_event_count_ == 0) {
        VLOG(jdwp) << "Queue request #" << deoptimization_requests_.size()
                   << " for full undeoptimization";
        deoptimization_requests_.push_back(req);
      }
      break;
    }
    case DeoptimizationRequest::kSelectiveDeoptimization: {
      DCHECK(req.Method() != nullptr);
      VLOG(jdwp) << "Queue request #" << deoptimization_requests_.size()
                 << " for deoptimization of " << PrettyMethod(req.Method());
      deoptimization_requests_.push_back(req);
      break;
    }
    case DeoptimizationRequest::kSelectiveUndeoptimization: {
      DCHECK(req.Method() != nullptr);
      VLOG(jdwp) << "Queue request #" << deoptimization_requests_.size()
                 << " for undeoptimization of " << PrettyMethod(req.Method());
      deoptimization_requests_.push_back(req);
      break;
    }
    default: {
      LOG(FATAL) << "Unknown deoptimization request kind " << req.GetKind();
      break;
    }
  }
}
void Dbg::ManageDeoptimization() {
  Thread* const self = Thread::Current();
  {
    MutexLock mu(self, *Locks::deoptimization_lock_);
    if (deoptimization_requests_.empty()) {
      return;
    }
  }
  CHECK_EQ(self->GetState(), kRunnable);
  self->TransitionFromRunnableToSuspended(kWaitingForDeoptimization);
  Runtime* const runtime = Runtime::Current();
  runtime->GetThreadList()->SuspendAll();
  const ThreadState old_state = self->SetStateUnsafe(kRunnable);
  {
    MutexLock mu(self, *Locks::deoptimization_lock_);
    size_t req_index = 0;
    for (DeoptimizationRequest& request : deoptimization_requests_) {
      VLOG(jdwp) << "Process deoptimization request #" << req_index++;
      ProcessDeoptimizationRequest(request);
    }
    deoptimization_requests_.clear();
  }
  CHECK_EQ(self->SetStateUnsafe(old_state), kRunnable);
  runtime->GetThreadList()->ResumeAll();
  self->TransitionFromSuspendedToRunnable();
}
static bool IsMethodPossiblyInlined(Thread* self, mirror::ArtMethod* m)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const DexFile::CodeItem* code_item = m->GetCodeItem();
    if (code_item == nullptr) {
    return false;
    }
    self->AssertThreadSuspensionIsAllowable();
    StackHandleScope<3> hs(self);
    mirror::Class* declaring_class = m->GetDeclaringClass();
    Handle<mirror::DexCache> dex_cache(hs.NewHandle(declaring_class->GetDexCache()));
    Handle<mirror::ClassLoader> class_loader(hs.NewHandle(declaring_class->GetClassLoader()));
    Handle<mirror::ArtMethod> method(hs.NewHandle(m));
    verifier::MethodVerifier verifier(self, dex_cache->GetDexFile(), dex_cache, class_loader,
                                    &m->GetClassDef(), code_item, m->GetDexMethodIndex(), method,
                                    m->GetAccessFlags(), false, true, false);
    return InlineMethodAnalyser::AnalyseMethodCode(&verifier, nullptr);
    }
static const Breakpoint* FindFirstBreakpointForMethod(mirror::ArtMethod* m)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::breakpoint_lock_) {
    for (Breakpoint& breakpoint : gBreakpoints) {
    if (breakpoint.Method() == m) {
      return &breakpoint;
    }
    }
    return nullptr;
    }
static void SanityCheckExistingBreakpoints(mirror::ArtMethod* m, bool need_full_deoptimization)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::breakpoint_lock_) {
    for (const Breakpoint& breakpoint : gBreakpoints) {
    CHECK_EQ(need_full_deoptimization, breakpoint.NeedFullDeoptimization());
    }
    if (need_full_deoptimization) {
    CHECK(Runtime::Current()->GetInstrumentation()->AreAllMethodsDeoptimized());
    CHECK(!Runtime::Current()->GetInstrumentation()->IsDeoptimized(m));
    } else {
    CHECK(Runtime::Current()->GetInstrumentation()->IsDeoptimized(m));
    }
    }
void Dbg::WatchLocation(const JDWP::JdwpLocation* location, DeoptimizationRequest* req) {
  Thread* const self = Thread::Current();
  mirror::ArtMethod* m = FromMethodId(location->method_id);
  DCHECK(m != nullptr) << "No method for method id " << location->method_id;
  const Breakpoint* existing_breakpoint;
  {
    ReaderMutexLock mu(self, *Locks::breakpoint_lock_);
    existing_breakpoint = FindFirstBreakpointForMethod(m);
  }
  bool need_full_deoptimization;
  if (existing_breakpoint == nullptr) {
    need_full_deoptimization = IsMethodPossiblyInlined(self, m);
    if (need_full_deoptimization) {
      req->SetKind(DeoptimizationRequest::kFullDeoptimization);
      req->SetMethod(nullptr);
    } else {
      req->SetKind(DeoptimizationRequest::kSelectiveDeoptimization);
      req->SetMethod(m);
    }
  } else {
    req->SetKind(DeoptimizationRequest::kNothing);
    req->SetMethod(nullptr);
    need_full_deoptimization = existing_breakpoint->NeedFullDeoptimization();
    if (kIsDebugBuild) {
      ReaderMutexLock mu(self, *Locks::breakpoint_lock_);
      SanityCheckExistingBreakpoints(m, need_full_deoptimization);
    }
  }
  {
    WriterMutexLock mu(self, *Locks::breakpoint_lock_);
    gBreakpoints.push_back(Breakpoint(m, location->dex_pc, need_full_deoptimization));
    VLOG(jdwp) << "Set breakpoint #" << (gBreakpoints.size() - 1) << ": "
               << gBreakpoints[gBreakpoints.size() - 1];
  }
}
void Dbg::UnwatchLocation(const JDWP::JdwpLocation* location, DeoptimizationRequest* req) {
  WriterMutexLock mu(Thread::Current(), *Locks::breakpoint_lock_);
  mirror::ArtMethod* m = FromMethodId(location->method_id);
  DCHECK(m != nullptr) << "No method for method id " << location->method_id;
  bool need_full_deoptimization = false;
  for (size_t i = 0, e = gBreakpoints.size(); i < e; ++i) {
    if (gBreakpoints[i].DexPc() == location->dex_pc && gBreakpoints[i].Method() == m) {
      VLOG(jdwp) << "Removed breakpoint #" << i << ": " << gBreakpoints[i];
      need_full_deoptimization = gBreakpoints[i].NeedFullDeoptimization();
      DCHECK_NE(need_full_deoptimization, Runtime::Current()->GetInstrumentation()->IsDeoptimized(m));
      gBreakpoints.erase(gBreakpoints.begin() + i);
      break;
    }
  }
  const Breakpoint* const existing_breakpoint = FindFirstBreakpointForMethod(m);
  if (existing_breakpoint == nullptr) {
    if (need_full_deoptimization) {
      req->SetKind(DeoptimizationRequest::kFullUndeoptimization);
      req->SetMethod(nullptr);
    } else {
      req->SetKind(DeoptimizationRequest::kSelectiveUndeoptimization);
      req->SetMethod(m);
    }
  } else {
    req->SetKind(DeoptimizationRequest::kNothing);
    req->SetMethod(nullptr);
    if (kIsDebugBuild) {
      SanityCheckExistingBreakpoints(m, need_full_deoptimization);
    }
  }
}
class ScopedThreadSuspension {
public:
      LOCKS_EXCLUDED(Locks::thread_list_lock_)
      LOCKS_EXCLUDED(Locks::thread_list_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) :
      thread_(nullptr),
      error_(JDWP::ERR_NONE),
      self_suspend_(false),
      other_suspend_(false) {
      ScopedObjectAccessUnchecked soa(self);
      {
      MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
      thread_ = DecodeThread(soa, thread_id, &error_);
      }
      if (error_ == JDWP::ERR_NONE) {
      if (thread_ == soa.Self()) {
        self_suspend_ = true;
      } else {
        soa.Self()->TransitionFromRunnableToSuspended(kWaitingForDebuggerSuspension);
        jobject thread_peer = Dbg::GetObjectRegistry()->GetJObject(thread_id);
        bool timed_out;
        Thread* suspended_thread;
        {
          MutexLock mu(soa.Self(), *Locks::thread_list_suspend_thread_lock_);
          ThreadList* thread_list = Runtime::Current()->GetThreadList();
          suspended_thread = thread_list->SuspendThreadByPeer(thread_peer, true, true, &timed_out);
        }
        CHECK_EQ(soa.Self()->TransitionFromSuspendedToRunnable(), kWaitingForDebuggerSuspension);
        if (suspended_thread == nullptr) {
          error_ = JDWP::ERR_INVALID_THREAD;
        } else {
          CHECK_EQ(suspended_thread, thread_);
          other_suspend_ = true;
        }
      }
      }
      }
  Thread* GetThread() const {
    return thread_;
  }
  JDWP::JdwpError GetError() const {
    return error_;
  }
  ~ScopedThreadSuspension() {
    if (other_suspend_) {
      Runtime::Current()->GetThreadList()->Resume(thread_, true);
    }
  }
private:
  Thread* thread_;
  JDWP::JdwpError error_;
  bool self_suspend_;
  bool other_suspend_;
};
JDWP::JdwpError Dbg::ConfigureStep(JDWP::ObjectId thread_id, JDWP::JdwpStepSize step_size,
                                   JDWP::JdwpStepDepth step_depth) {
  Thread* self = Thread::Current();
  ScopedThreadSuspension sts(self, thread_id);
  if (sts.GetError() != JDWP::ERR_NONE) {
    return sts.GetError();
  }
  struct SingleStepStackVisitor : public StackVisitor {
    explicit SingleStepStackVisitor(Thread* thread, SingleStepControl* single_step_control,
                                    int32_t* line_number)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
        : StackVisitor(thread, nullptr), single_step_control_(single_step_control),
          line_number_(line_number) {
      DCHECK_EQ(single_step_control_, thread->GetSingleStepControl());
      single_step_control_->method = nullptr;
      single_step_control_->stack_depth = 0;
    }
    bool VisitFrame() NO_THREAD_SAFETY_ANALYSIS {
      mirror::ArtMethod* m = GetMethod();
      if (!m->IsRuntimeMethod()) {
        ++single_step_control_->stack_depth;
        if (single_step_control_->method == nullptr) {
          mirror::DexCache* dex_cache = m->GetDeclaringClass()->GetDexCache();
          single_step_control_->method = m;
          *line_number_ = -1;
          if (dex_cache != nullptr) {
            const DexFile& dex_file = *dex_cache->GetDexFile();
            *line_number_ = dex_file.GetLineNumFromPC(m, GetDexPc());
          }
        }
      }
      return true;
    }
    SingleStepControl* const single_step_control_;
    int32_t* const line_number_;
  };
  Thread* const thread = sts.GetThread();
  SingleStepControl* const single_step_control = thread->GetSingleStepControl();
  DCHECK(single_step_control != nullptr);
  int32_t line_number = -1;
  SingleStepStackVisitor visitor(thread, single_step_control, &line_number);
  visitor.WalkStack();
  struct DebugCallbackContext {
    explicit DebugCallbackContext(SingleStepControl* single_step_control, int32_t line_number,
                                  const DexFile::CodeItem* code_item)
      : single_step_control_(single_step_control), line_number_(line_number), code_item_(code_item),
        last_pc_valid(false), last_pc(0) {
    }
    static bool Callback(void* raw_context, uint32_t address, uint32_t line_number) {
      DebugCallbackContext* context = reinterpret_cast<DebugCallbackContext*>(raw_context);
      if (static_cast<int32_t>(line_number) == context->line_number_) {
        if (!context->last_pc_valid) {
          context->last_pc = address;
          context->last_pc_valid = true;
        }
      } else if (context->last_pc_valid) {
        for (uint32_t dex_pc = context->last_pc; dex_pc < address; ++dex_pc) {
          context->single_step_control_->dex_pcs.insert(dex_pc);
        }
        context->last_pc_valid = false;
      }
      return false;
    }
    ~DebugCallbackContext() {
      if (last_pc_valid) {
        size_t end = code_item_->insns_size_in_code_units_;
        for (uint32_t dex_pc = last_pc; dex_pc < end; ++dex_pc) {
          single_step_control_->dex_pcs.insert(dex_pc);
        }
      }
    }
    SingleStepControl* const single_step_control_;
    const int32_t line_number_;
    const DexFile::CodeItem* const code_item_;
    bool last_pc_valid;
    uint32_t last_pc;
  };
  single_step_control->dex_pcs.clear();
  mirror::ArtMethod* m = single_step_control->method;
  if (!m->IsNative()) {
    const DexFile::CodeItem* const code_item = m->GetCodeItem();
    DebugCallbackContext context(single_step_control, line_number, code_item);
    m->GetDexFile()->DecodeDebugInfo(code_item, m->IsStatic(), m->GetDexMethodIndex(),
                                     DebugCallbackContext::Callback, nullptr, &context);
  }
  single_step_control->step_size = step_size;
  single_step_control->step_depth = step_depth;
  single_step_control->is_active = true;
  if (VLOG_IS_ON(jdwp)) {
    VLOG(jdwp) << "Single-step thread: " << *thread;
    VLOG(jdwp) << "Single-step step size: " << single_step_control->step_size;
    VLOG(jdwp) << "Single-step step depth: " << single_step_control->step_depth;
    VLOG(jdwp) << "Single-step current method: " << PrettyMethod(single_step_control->method);
    VLOG(jdwp) << "Single-step current line: " << line_number;
    VLOG(jdwp) << "Single-step current stack depth: " << single_step_control->stack_depth;
    VLOG(jdwp) << "Single-step dex_pc values:";
    for (uint32_t dex_pc : single_step_control->dex_pcs) {
      VLOG(jdwp) << StringPrintf(" %#x", dex_pc);
    }
  }
  return JDWP::ERR_NONE;
}
void Dbg::UnconfigureStep(JDWP::ObjectId thread_id) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
  JDWP::JdwpError error;
  Thread* thread = DecodeThread(soa, thread_id, &error);
  if (error == JDWP::ERR_NONE) {
    SingleStepControl* single_step_control = thread->GetSingleStepControl();
    DCHECK(single_step_control != nullptr);
    single_step_control->Clear();
  }
}
static char JdwpTagToShortyChar(JDWP::JdwpTag tag) {
  switch (tag) {
    default:
      LOG(FATAL) << "unknown JDWP tag: " << PrintableChar(tag);
      UNREACHABLE();
    case JDWP::JT_BYTE: return 'B';
    case JDWP::JT_CHAR: return 'C';
    case JDWP::JT_FLOAT: return 'F';
    case JDWP::JT_DOUBLE: return 'D';
    case JDWP::JT_INT: return 'I';
    case JDWP::JT_LONG: return 'J';
    case JDWP::JT_SHORT: return 'S';
    case JDWP::JT_VOID: return 'V';
    case JDWP::JT_BOOLEAN: return 'Z';
    case JDWP::JT_ARRAY:
    case JDWP::JT_OBJECT:
    case JDWP::JT_STRING:
    case JDWP::JT_THREAD:
    case JDWP::JT_THREAD_GROUP:
    case JDWP::JT_CLASS_LOADER:
    case JDWP::JT_CLASS_OBJECT:
      return 'L';
  }
}
JDWP::JdwpError Dbg::InvokeMethod(JDWP::ObjectId thread_id, JDWP::ObjectId object_id,
                                  JDWP::RefTypeId class_id, JDWP::MethodId method_id,
                                  uint32_t arg_count, uint64_t* arg_values,
                                  JDWP::JdwpTag* arg_types, uint32_t options,
                                  JDWP::JdwpTag* pResultTag, uint64_t* pResultValue,
                                  JDWP::ObjectId* pExceptionId) {
  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  Thread* targetThread = nullptr;
  DebugInvokeReq* req = nullptr;
  Thread* self = Thread::Current();
  {
    ScopedObjectAccessUnchecked soa(self);
    MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
    JDWP::JdwpError error;
    targetThread = DecodeThread(soa, thread_id, &error);
    if (error != JDWP::ERR_NONE) {
      LOG(ERROR) << "InvokeMethod request for invalid thread id " << thread_id;
      return error;
    }
    req = targetThread->GetInvokeReq();
    if (!req->ready) {
      LOG(ERROR) << "InvokeMethod request for thread not stopped by event: " << *targetThread;
      return JDWP::ERR_INVALID_THREAD;
    }
    int suspend_count;
    {
      MutexLock mu2(soa.Self(), *Locks::thread_suspend_count_lock_);
      suspend_count = targetThread->GetSuspendCount();
    }
    if (suspend_count > 1) {
      LOG(ERROR) << *targetThread << " suspend count too deep for method invocation: " << suspend_count;
      return JDWP::ERR_THREAD_SUSPENDED;
    }
    mirror::Object* receiver = gRegistry->Get<mirror::Object*>(object_id, &error);
    if (error != JDWP::ERR_NONE) {
      return JDWP::ERR_INVALID_OBJECT;
    }
    mirror::Object* thread = gRegistry->Get<mirror::Object*>(thread_id, &error);
    if (error != JDWP::ERR_NONE) {
      return JDWP::ERR_INVALID_OBJECT;
    }
    mirror::Class* c = DecodeClass(class_id, &error);
    if (c == nullptr) {
      return error;
    }
    mirror::ArtMethod* m = FromMethodId(method_id);
    if (m->IsStatic() != (receiver == nullptr)) {
      return JDWP::ERR_INVALID_METHODID;
    }
    if (m->IsStatic()) {
      if (m->GetDeclaringClass() != c) {
        return JDWP::ERR_INVALID_METHODID;
      }
    } else {
      if (!m->GetDeclaringClass()->IsAssignableFrom(c)) {
        return JDWP::ERR_INVALID_METHODID;
      }
    }
    uint32_t shorty_len = 0;
    const char* shorty = m->GetShorty(&shorty_len);
    if (shorty_len - 1 != arg_count) {
      return JDWP::ERR_ILLEGAL_ARGUMENT;
    }
    {
      StackHandleScope<3> hs(soa.Self());
      MethodHelper mh(hs.NewHandle(m));
      HandleWrapper<mirror::Object> h_obj(hs.NewHandleWrapper(&receiver));
      HandleWrapper<mirror::Class> h_klass(hs.NewHandleWrapper(&c));
      const DexFile::TypeList* types = m->GetParameterTypeList();
      for (size_t i = 0; i < arg_count; ++i) {
        if (shorty[i + 1] != JdwpTagToShortyChar(arg_types[i])) {
          return JDWP::ERR_ILLEGAL_ARGUMENT;
        }
        if (shorty[i + 1] == 'L') {
          mirror::Class* parameter_type = mh.GetClassFromTypeIdx(types->GetTypeItem(i).type_idx_);
          mirror::Object* argument = gRegistry->Get<mirror::Object*>(arg_values[i], &error);
          if (error != JDWP::ERR_NONE) {
            return JDWP::ERR_INVALID_OBJECT;
          }
          if (argument != nullptr && !argument->InstanceOf(parameter_type)) {
            return JDWP::ERR_ILLEGAL_ARGUMENT;
          }
          jvalue& v = reinterpret_cast<jvalue&>(arg_values[i]);
          v.l = gRegistry->GetJObject(arg_values[i]);
        }
      }
      m = mh.GetMethod();
    }
    req->receiver = receiver;
    req->thread = thread;
    req->klass = c;
    req->method = m;
    req->arg_count = arg_count;
    req->arg_values = arg_values;
    req->options = options;
    req->invoke_needed = true;
  }
  {
    self->TransitionFromRunnableToSuspended(kWaitingForDebuggerSend);
    VLOG(jdwp) << "    Transferring control to event thread";
    {
      MutexLock mu(self, req->lock);
      if ((options & JDWP::INVOKE_SINGLE_THREADED) == 0) {
        VLOG(jdwp) << "      Resuming all threads";
        thread_list->UndoDebuggerSuspensions();
      } else {
        VLOG(jdwp) << "      Resuming event thread only";
        thread_list->Resume(targetThread, true);
      }
      while (req->invoke_needed) {
        req->cond.Wait(self);
      }
    }
    VLOG(jdwp) << "    Control has returned from event thread";
    SuspendThread(thread_id, false );
    self->TransitionFromSuspendedToRunnable();
  }
  if ((options & JDWP::INVOKE_SINGLE_THREADED) == 0) {
    self->TransitionFromRunnableToSuspended(kWaitingForDebuggerSuspension);
    VLOG(jdwp) << "      Suspending all threads";
    thread_list->SuspendAllForDebugger();
    self->TransitionFromSuspendedToRunnable();
    VLOG(jdwp) << "      Resuming event thread to balance the count";
    thread_list->Resume(targetThread, true);
  }
  *pResultTag = req->result_tag;
  if (IsPrimitiveTag(req->result_tag)) {
    *pResultValue = req->result_value.GetJ();
  } else {
    *pResultValue = gRegistry->Add(req->result_value.GetL());
  }
  *pExceptionId = req->exception;
  return req->error;
}
void Dbg::ExecuteMethod(DebugInvokeReq* pReq) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<4> hs(soa.Self());
  auto old_throw_this_object = hs.NewHandle<mirror::Object>(nullptr);
  auto old_throw_method = hs.NewHandle<mirror::ArtMethod>(nullptr);
  auto old_exception = hs.NewHandle<mirror::Throwable>(nullptr);
  uint32_t old_throw_dex_pc;
  bool old_exception_report_flag;
  {
    ThrowLocation old_throw_location;
    mirror::Throwable* old_exception_obj = soa.Self()->GetException(&old_throw_location);
    old_throw_this_object.Assign(old_throw_location.GetThis());
    old_throw_method.Assign(old_throw_location.GetMethod());
    old_exception.Assign(old_exception_obj);
    old_throw_dex_pc = old_throw_location.GetDexPc();
    old_exception_report_flag = soa.Self()->IsExceptionReportedToInstrumentation();
    soa.Self()->ClearException();
  }
  MutableHandle<mirror::ArtMethod> m(hs.NewHandle(pReq->method));
  if ((pReq->options & JDWP::INVOKE_NONVIRTUAL) == 0 && pReq->receiver != nullptr) {
    mirror::ArtMethod* actual_method = pReq->klass->FindVirtualMethodForVirtualOrInterface(m.Get());
    if (actual_method != m.Get()) {
      VLOG(jdwp) << "ExecuteMethod translated " << PrettyMethod(m.Get()) << " to " << PrettyMethod(actual_method);
      m.Assign(actual_method);
    }
  }
  VLOG(jdwp) << "ExecuteMethod " << PrettyMethod(m.Get())
             << " receiver=" << pReq->receiver
             << " arg_count=" << pReq->arg_count;
  CHECK(m.Get() != nullptr);
  CHECK_EQ(sizeof(jvalue), sizeof(uint64_t));
  pReq->result_value = InvokeWithJValues(soa, pReq->receiver, soa.EncodeMethod(m.Get()),
                                         reinterpret_cast<jvalue*>(pReq->arg_values));
  mirror::Throwable* exception = soa.Self()->GetException(nullptr);
  soa.Self()->ClearException();
  pReq->exception = gRegistry->Add(exception);
  pReq->result_tag = BasicTagFromDescriptor(m.Get()->GetShorty());
  if (pReq->exception != 0) {
    VLOG(jdwp) << "  JDWP invocation returning with exception=" << exception
        << " " << exception->Dump();
    pReq->result_value.SetJ(0);
  } else if (pReq->result_tag == JDWP::JT_OBJECT) {
    JDWP::JdwpTag new_tag = TagFromObject(soa, pReq->result_value.GetL());
    if (new_tag != pReq->result_tag) {
      VLOG(jdwp) << "  JDWP promoted result from " << pReq->result_tag << " to " << new_tag;
      pReq->result_tag = new_tag;
    }
    gRegistry->Add(pReq->result_value.GetL());
  }
  if (old_exception.Get() != nullptr) {
    ThrowLocation gc_safe_throw_location(old_throw_this_object.Get(), old_throw_method.Get(),
                                         old_throw_dex_pc);
    soa.Self()->SetException(gc_safe_throw_location, old_exception.Get());
    soa.Self()->SetExceptionReportedToInstrumentation(old_exception_report_flag);
  }
}
bool Dbg::DdmHandlePacket(JDWP::Request* request, uint8_t** pReplyBuf, int* pReplyLen) {
  Thread* self = Thread::Current();
  JNIEnv* env = self->GetJniEnv();
  uint32_t type = request->ReadUnsigned32("type");
  uint32_t length = request->ReadUnsigned32("length");
  size_t request_length = request->size();
  ScopedLocalRef<jbyteArray> dataArray(env, env->NewByteArray(request_length));
  if (dataArray.get() == nullptr) {
    LOG(WARNING) << "byte[] allocation failed: " << request_length;
    env->ExceptionClear();
    return false;
  }
  env->SetByteArrayRegion(dataArray.get(), 0, request_length,
                          reinterpret_cast<const jbyte*>(request->data()));
  request->Skip(request_length);
  ScopedByteArrayRO contents(env, dataArray.get());
  if (length != request_length) {
    LOG(WARNING) << StringPrintf("bad chunk found (len=%u pktLen=%zd)", length, request_length);
    return false;
  }
  ScopedLocalRef<jobject> chunk(env, env->CallStaticObjectMethod(WellKnownClasses::org_apache_harmony_dalvik_ddmc_DdmServer,
                                                                 WellKnownClasses::org_apache_harmony_dalvik_ddmc_DdmServer_dispatch,
                                                                 type, dataArray.get(), 0, length));
  if (env->ExceptionCheck()) {
    LOG(INFO) << StringPrintf("Exception thrown by dispatcher for 0x%08x", type);
    env->ExceptionDescribe();
    env->ExceptionClear();
    return false;
  }
  if (chunk.get() == nullptr) {
    return false;
  }
  ScopedLocalRef<jbyteArray> replyData(env, reinterpret_cast<jbyteArray>(env->GetObjectField(chunk.get(), WellKnownClasses::org_apache_harmony_dalvik_ddmc_Chunk_data)));
  jint offset = env->GetIntField(chunk.get(), WellKnownClasses::org_apache_harmony_dalvik_ddmc_Chunk_offset);
  length = env->GetIntField(chunk.get(), WellKnownClasses::org_apache_harmony_dalvik_ddmc_Chunk_length);
  type = env->GetIntField(chunk.get(), WellKnownClasses::org_apache_harmony_dalvik_ddmc_Chunk_type);
  VLOG(jdwp) << StringPrintf("DDM reply: type=0x%08x data=%p offset=%d length=%d", type, replyData.get(), offset, length);
  if (length == 0 || replyData.get() == nullptr) {
    return false;
  }
  const int kChunkHdrLen = 8;
  uint8_t* reply = new uint8_t[length + kChunkHdrLen];
  if (reply == nullptr) {
    LOG(WARNING) << "malloc failed: " << (length + kChunkHdrLen);
    return false;
  }
  JDWP::Set4BE(reply + 0, type);
  JDWP::Set4BE(reply + 4, length);
  env->GetByteArrayRegion(replyData.get(), offset, length, reinterpret_cast<jbyte*>(reply + kChunkHdrLen));
  *pReplyBuf = reply;
  *pReplyLen = length + kChunkHdrLen;
  VLOG(jdwp) << StringPrintf("dvmHandleDdm returning type=%.4s %p len=%d", reinterpret_cast<char*>(reply), reply, length);
  return true;
}
void Dbg::DdmBroadcast(bool connect) {
  VLOG(jdwp) << "Broadcasting DDM " << (connect ? "connect" : "disconnect") << "...";
  Thread* self = Thread::Current();
  if (self->GetState() != kRunnable) {
    LOG(ERROR) << "DDM broadcast in thread state " << self->GetState();
  }
  JNIEnv* env = self->GetJniEnv();
  jint event = connect ? 1 : 2 ;
  env->CallStaticVoidMethod(WellKnownClasses::org_apache_harmony_dalvik_ddmc_DdmServer,
                            WellKnownClasses::org_apache_harmony_dalvik_ddmc_DdmServer_broadcast,
                            event);
  if (env->ExceptionCheck()) {
    LOG(ERROR) << "DdmServer.broadcast " << event << " failed";
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
}
void Dbg::DdmConnected() {
  Dbg::DdmBroadcast(true);
}
void Dbg::DdmDisconnected() {
  Dbg::DdmBroadcast(false);
  gDdmThreadNotification = false;
}
void Dbg::DdmSendThreadNotification(Thread* t, uint32_t type) {
  if (!gDdmThreadNotification) {
    return;
  }
  if (type == CHUNK_TYPE("THDE")) {
    uint8_t buf[4];
    JDWP::Set4BE(&buf[0], t->GetThreadId());
    Dbg::DdmSendChunk(CHUNK_TYPE("THDE"), 4, buf);
  } else {
    CHECK(type == CHUNK_TYPE("THCR") || type == CHUNK_TYPE("THNM")) << type;
    ScopedObjectAccessUnchecked soa(Thread::Current());
    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::String> name(hs.NewHandle(t->GetThreadName(soa)));
    size_t char_count = (name.Get() != nullptr) ? name->GetLength() : 0;
    const jchar* chars = (name.Get() != nullptr) ? name->GetCharArray()->GetData() : nullptr;
    std::vector<uint8_t> bytes;
    JDWP::Append4BE(bytes, t->GetThreadId());
    JDWP::AppendUtf16BE(bytes, chars, char_count);
    CHECK_EQ(bytes.size(), char_count*2 + sizeof(uint32_t)*2);
    Dbg::DdmSendChunk(type, bytes);
  }
}
void Dbg::DdmSetThreadNotification(bool enable) {
  gDdmThreadNotification = enable;
  if (enable) {
    SuspendVM();
    std::list<Thread*> threads;
    Thread* self = Thread::Current();
    {
      MutexLock mu(self, *Locks::thread_list_lock_);
      threads = Runtime::Current()->GetThreadList()->GetList();
    }
    {
      ScopedObjectAccess soa(self);
      for (Thread* thread : threads) {
        Dbg::DdmSendThreadNotification(thread, CHUNK_TYPE("THCR"));
      }
    }
    ResumeVM();
  }
}
void Dbg::PostThreadStartOrStop(Thread* t, uint32_t type) {
  if (IsDebuggerActive()) {
    gJdwpState->PostThreadChange(t, type == CHUNK_TYPE("THCR"));
  }
  Dbg::DdmSendThreadNotification(t, type);
}
void Dbg::PostThreadStart(Thread* t) {
  Dbg::PostThreadStartOrStop(t, CHUNK_TYPE("THCR"));
}
void Dbg::PostThreadDeath(Thread* t) {
  Dbg::PostThreadStartOrStop(t, CHUNK_TYPE("THDE"));
}
void Dbg::DdmSendChunk(uint32_t type, size_t byte_count, const uint8_t* buf) {
  CHECK(buf != nullptr);
  iovec vec[1];
  vec[0].iov_base = reinterpret_cast<void*>(const_cast<uint8_t*>(buf));
  vec[0].iov_len = byte_count;
  Dbg::DdmSendChunkV(type, vec, 1);
}
void Dbg::DdmSendChunk(uint32_t type, const std::vector<uint8_t>& bytes) {
  DdmSendChunk(type, bytes.size(), &bytes[0]);
}
void Dbg::DdmSendChunkV(uint32_t type, const iovec* iov, int iov_count) {
  if (gJdwpState == nullptr) {
    VLOG(jdwp) << "Debugger thread not active, ignoring DDM send: " << type;
  } else {
    gJdwpState->DdmSendChunkV(type, iov, iov_count);
  }
}
int Dbg::DdmHandleHpifChunk(HpifWhen when) {
  if (when == HPIF_WHEN_NOW) {
    DdmSendHeapInfo(when);
    return true;
  }
  if (when != HPIF_WHEN_NEVER && when != HPIF_WHEN_NEXT_GC && when != HPIF_WHEN_EVERY_GC) {
    LOG(ERROR) << "invalid HpifWhen value: " << static_cast<int>(when);
    return false;
  }
  gDdmHpifWhen = when;
  return true;
}
bool Dbg::DdmHandleHpsgNhsgChunk(Dbg::HpsgWhen when, Dbg::HpsgWhat what, bool native) {
  if (when != HPSG_WHEN_NEVER && when != HPSG_WHEN_EVERY_GC) {
    LOG(ERROR) << "invalid HpsgWhen value: " << static_cast<int>(when);
    return false;
  }
  if (what != HPSG_WHAT_MERGED_OBJECTS && what != HPSG_WHAT_DISTINCT_OBJECTS) {
    LOG(ERROR) << "invalid HpsgWhat value: " << static_cast<int>(what);
    return false;
  }
  if (native) {
    gDdmNhsgWhen = when;
    gDdmNhsgWhat = what;
  } else {
    gDdmHpsgWhen = when;
    gDdmHpsgWhat = what;
  }
  return true;
}
void Dbg::DdmSendHeapInfo(HpifWhen reason) {
  if (reason == gDdmHpifWhen) {
    if (gDdmHpifWhen == HPIF_WHEN_NEXT_GC) {
      gDdmHpifWhen = HPIF_WHEN_NEVER;
    }
  }
  uint8_t heap_count = 1;
  gc::Heap* heap = Runtime::Current()->GetHeap();
  std::vector<uint8_t> bytes;
  JDWP::Append4BE(bytes, heap_count);
  JDWP::Append4BE(bytes, 1);
  JDWP::Append8BE(bytes, MilliTime());
  JDWP::Append1BE(bytes, reason);
  JDWP::Append4BE(bytes, heap->GetMaxMemory());
  JDWP::Append4BE(bytes, heap->GetTotalMemory());
  JDWP::Append4BE(bytes, heap->GetBytesAllocated());
  JDWP::Append4BE(bytes, heap->GetObjectsAllocated());
  CHECK_EQ(bytes.size(), 4U + (heap_count * (4 + 8 + 1 + 4 + 4 + 4 + 4)));
  Dbg::DdmSendChunk(CHUNK_TYPE("HPIF"), bytes);
}
enum HpsgSolidity {
SOLIDITY_FREE = 0,SOLIDITY_HARD = 1,SOLIDITY_SOFT = 2,SOLIDITY_WEAK = 3,SOLIDITY_PHANTOM = 4,SOLIDITY_FINALIZABLE = 5,SOLIDITY_SWEEP = 6,};
enum HpsgKind {
KIND_OBJECT = 0,KIND_CLASS_OBJECT = 1,KIND_ARRAY_1 = 2,KIND_ARRAY_2 = 3,KIND_ARRAY_4 = 4,KIND_ARRAY_8 = 5,KIND_UNKNOWN = 6,KIND_NATIVE = 7,};
#define HPSG_PARTIAL (1<<7)
class HeapChunkContext {
public:
  HeapChunkContext(bool merge, bool native)
      : buf_(16384 - 16),
        type_(0),
        merge_(merge),
        chunk_overhead_(0) {
    Reset();
    if (native) {
      type_ = CHUNK_TYPE("NHSG");
    } else {
      type_ = merge ? CHUNK_TYPE("HPSG") : CHUNK_TYPE("HPSO");
    }
  }
  ~HeapChunkContext() {
    if (p_ > &buf_[0]) {
      Flush();
    }
  }
  void SetChunkOverhead(size_t chunk_overhead) {
    chunk_overhead_ = chunk_overhead;
  }
  void ResetStartOfNextChunk() {
    startOfNextMemoryChunk_ = nullptr;
  }
  void EnsureHeader(const void* chunk_ptr) {
    if (!needHeader_) {
      return;
    }
    JDWP::Write4BE(&p_, 1);
    JDWP::Write1BE(&p_, 8);
    JDWP::Write4BE(&p_, reinterpret_cast<uintptr_t>(chunk_ptr));
    JDWP::Write4BE(&p_, 0);
    pieceLenField_ = p_;
    JDWP::Write4BE(&p_, 0x55555555);
    needHeader_ = false;
  }
  void Flush() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (pieceLenField_ == nullptr) {
      CHECK(needHeader_);
      return;
    }
    CHECK_LE(&buf_[0], pieceLenField_);
    CHECK_LE(pieceLenField_, p_);
    JDWP::Set4BE(pieceLenField_, totalAllocationUnits_);
    Dbg::DdmSendChunk(type_, p_ - &buf_[0], &buf_[0]);
    Reset();
  }
  static void HeapChunkCallback(void* start, void* end, size_t used_bytes, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_,
                            Locks::mutator_lock_) {
      reinterpret_cast<HeapChunkContext*>(arg)->HeapChunkCallback(start, end, used_bytes);
      }
private:
  enum { ALLOCATION_UNIT_SIZE = 8 };
  void Reset() {
    p_ = &buf_[0];
    ResetStartOfNextChunk();
    totalAllocationUnits_ = 0;
    needHeader_ = true;
    pieceLenField_ = nullptr;
  }
  void HeapChunkCallback(void* start, void* , size_t used_bytes)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_,
                            Locks::mutator_lock_) {
      if (used_bytes == 0) {
        if (start == nullptr) {
            startOfNextMemoryChunk_ = nullptr;
            Flush();
        }
        return;
      }
      bool native = type_ == CHUNK_TYPE("NHSG");
      if (startOfNextMemoryChunk_ != nullptr) {
        bool flush = true;
        if (start > startOfNextMemoryChunk_) {
            const size_t kMaxFreeLen = 2 * kPageSize;
            void* freeStart = startOfNextMemoryChunk_;
            void* freeEnd = start;
            size_t freeLen = reinterpret_cast<char*>(freeEnd) - reinterpret_cast<char*>(freeStart);
            if (!native || freeLen < kMaxFreeLen) {
                AppendChunk(HPSG_STATE(SOLIDITY_FREE, 0), freeStart, freeLen);
                flush = false;
            }
        }
        if (flush) {
            startOfNextMemoryChunk_ = nullptr;
            Flush();
        }
      }
      mirror::Object* obj = reinterpret_cast<mirror::Object*>(start);
      uint8_t state = ExamineObject(obj, native);
      AppendChunk(state, start, used_bytes + chunk_overhead_);
      startOfNextMemoryChunk_ = reinterpret_cast<char*>(start) + used_bytes + chunk_overhead_;
      }
  void AppendChunk(uint8_t state, void* ptr, size_t length)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      size_t needed = (((length/ALLOCATION_UNIT_SIZE + 255) / 256) * 2) + 17;
      size_t bytesLeft = buf_.size() - (size_t)(p_ - &buf_[0]);
      if (bytesLeft < needed) {
      Flush();
      }
      bytesLeft = buf_.size() - (size_t)(p_ - &buf_[0]);
      if (bytesLeft < needed) {
      LOG(WARNING) << "Chunk is too big to transmit (chunk_len=" << length << ", "
          << needed << " bytes)";
      return;
      }
      EnsureHeader(ptr);
      length /= ALLOCATION_UNIT_SIZE;
      totalAllocationUnits_ += length;
      while (length > 256) {
      *p_++ = state | HPSG_PARTIAL;
      *p_++ = 255;
      length -= 256;
      }
      *p_++ = state;
      *p_++ = length - 1;
      }
  uint8_t ExamineObject(mirror::Object* o, bool is_native_heap)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
      if (o == nullptr) {
      return HPSG_STATE(SOLIDITY_FREE, 0);
      }
      if (is_native_heap) {
      return HPSG_STATE(SOLIDITY_HARD, KIND_NATIVE);
      }
      if (!Runtime::Current()->GetHeap()->IsLiveObjectLocked(o)) {
      return HPSG_STATE(SOLIDITY_HARD, KIND_NATIVE);
      }
      mirror::Class* c = o->GetClass();
      if (c == nullptr) {
      return HPSG_STATE(SOLIDITY_HARD, KIND_OBJECT);
      }
      if (!Runtime::Current()->GetHeap()->IsValidObjectAddress(c)) {
      LOG(ERROR) << "Invalid class for managed heap object: " << o << " " << c;
      return HPSG_STATE(SOLIDITY_HARD, KIND_UNKNOWN);
      }
      if (c->IsClassClass()) {
      return HPSG_STATE(SOLIDITY_HARD, KIND_CLASS_OBJECT);
      }
      if (c->IsArrayClass()) {
      if (o->IsObjectArray()) {
        return HPSG_STATE(SOLIDITY_HARD, KIND_ARRAY_4);
      }
      switch (c->GetComponentSize()) {
      case 1: return HPSG_STATE(SOLIDITY_HARD, KIND_ARRAY_1);
      case 2: return HPSG_STATE(SOLIDITY_HARD, KIND_ARRAY_2);
      case 4: return HPSG_STATE(SOLIDITY_HARD, KIND_ARRAY_4);
      case 8: return HPSG_STATE(SOLIDITY_HARD, KIND_ARRAY_8);
      }
      }
      return HPSG_STATE(SOLIDITY_HARD, KIND_OBJECT);
      }
  std::vector<uint8_t> buf_;
  uint8_t* p_;
  uint8_t* pieceLenField_;
  void* startOfNextMemoryChunk_;
  size_t totalAllocationUnits_;
  uint32_t type_;
  bool merge_;
  bool needHeader_;
  size_t chunk_overhead_;
  DISALLOW_COPY_AND_ASSIGN(HeapChunkContext);
};
static void BumpPointerSpaceCallback(mirror::Object* obj, void* arg)
                                                EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
                                                const size_t size = RoundUp(obj->SizeOf(), kObjectAlignment);
                                                HeapChunkContext::HeapChunkCallback(
                                                obj, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(obj) + size), size, arg);
                                                }
void Dbg::DdmSendHeapSegments(bool native) {
  Dbg::HpsgWhen when;
  Dbg::HpsgWhat what;
  if (!native) {
    when = gDdmHpsgWhen;
    what = gDdmHpsgWhat;
  } else {
    when = gDdmNhsgWhen;
    what = gDdmNhsgWhat;
  }
  if (when == HPSG_WHEN_NEVER) {
    return;
  }
  CHECK(what == HPSG_WHAT_MERGED_OBJECTS || what == HPSG_WHAT_DISTINCT_OBJECTS) << static_cast<int>(what);
  uint8_t heap_id[4];
  JDWP::Set4BE(&heap_id[0], 1);
  Dbg::DdmSendChunk(native ? CHUNK_TYPE("NHST") : CHUNK_TYPE("HPST"), sizeof(heap_id), heap_id);
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertSharedHeld(self);
  HeapChunkContext context((what == HPSG_WHAT_MERGED_OBJECTS), native);
  if (native) {
#ifdef USE_DLMALLOC
    dlmalloc_inspect_all(HeapChunkContext::HeapChunkCallback, &context);
#else
    UNIMPLEMENTED(WARNING) << "Native heap inspection is only supported with dlmalloc";
#endif
  } else {
    gc::Heap* heap = Runtime::Current()->GetHeap();
    for (const auto& space : heap->GetContinuousSpaces()) {
      if (space->IsDlMallocSpace()) {
        ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
        context.SetChunkOverhead(sizeof(size_t));
        space->AsDlMallocSpace()->Walk(HeapChunkContext::HeapChunkCallback, &context);
      } else if (space->IsRosAllocSpace()) {
        context.SetChunkOverhead(0);
        self->TransitionFromRunnableToSuspended(kSuspended);
        ThreadList* tl = Runtime::Current()->GetThreadList();
        tl->SuspendAll();
        {
          ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
          space->AsRosAllocSpace()->Walk(HeapChunkContext::HeapChunkCallback, &context);
        }
        tl->ResumeAll();
        self->TransitionFromSuspendedToRunnable();
      } else if (space->IsBumpPointerSpace()) {
        ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
        context.SetChunkOverhead(0);
        space->AsBumpPointerSpace()->Walk(BumpPointerSpaceCallback, &context);
      } else {
        UNIMPLEMENTED(WARNING) << "Not counting objects in space " << *space;
      }
      context.ResetStartOfNextChunk();
    }
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    context.SetChunkOverhead(0);
    heap->GetLargeObjectsSpace()->Walk(HeapChunkContext::HeapChunkCallback, &context);
  }
  Dbg::DdmSendChunk(native ? CHUNK_TYPE("NHEN") : CHUNK_TYPE("HPEN"), sizeof(heap_id), heap_id);
}
static size_t GetAllocTrackerMax() {
#ifdef HAVE_ANDROID_OS
  const char* propertyName = "dalvik.vm.allocTrackerMax";
  char allocRecordMaxString[PROPERTY_VALUE_MAX];
  if (property_get(propertyName, allocRecordMaxString, "") > 0) {
    char* end;
    size_t value = strtoul(allocRecordMaxString, &end, 10);
    if (*end != '\0') {
      LOG(ERROR) << "Ignoring  " << propertyName << " '" << allocRecordMaxString
                 << "' --- invalid";
      return kDefaultNumAllocRecords;
    }
    if (!IsPowerOfTwo(value)) {
      LOG(ERROR) << "Ignoring  " << propertyName << " '" << allocRecordMaxString
                 << "' --- not power of two";
      return kDefaultNumAllocRecords;
    }
    return value;
  }
#endif
  return kDefaultNumAllocRecords;
}
void Dbg::SetAllocTrackingEnabled(bool enable) {
  Thread* self = Thread::Current();
  if (enable) {
    {
      MutexLock mu(self, *Locks::alloc_tracker_lock_);
      if (recent_allocation_records_ != nullptr) {
        return;
      }
      alloc_record_max_ = GetAllocTrackerMax();
      LOG(INFO) << "Enabling alloc tracker (" << alloc_record_max_ << " entries of "
                << kMaxAllocRecordStackDepth << " frames, taking "
                << PrettySize(sizeof(AllocRecord) * alloc_record_max_) << ")";
      DCHECK_EQ(alloc_record_head_, 0U);
      DCHECK_EQ(alloc_record_count_, 0U);
      recent_allocation_records_ = new AllocRecord[alloc_record_max_];
      CHECK(recent_allocation_records_ != nullptr);
    }
    Runtime::Current()->GetInstrumentation()->InstrumentQuickAllocEntryPoints();
  } else {
    {
      ScopedObjectAccess soa(self);
      MutexLock mu(self, *Locks::alloc_tracker_lock_);
      if (recent_allocation_records_ == nullptr) {
        return;
      }
      LOG(INFO) << "Disabling alloc tracker";
      delete[] recent_allocation_records_;
      recent_allocation_records_ = nullptr;
      alloc_record_head_ = 0;
      alloc_record_count_ = 0;
      type_cache_.Clear();
    }
    Runtime::Current()->GetInstrumentation()->UninstrumentQuickAllocEntryPoints();
  }
}
struct AllocRecordStackVisitor : public StackVisitor {
  AllocRecordStackVisitor(Thread* thread, AllocRecord* record)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, nullptr), record(record), depth(0) {}
  {
    if (depth >= kMaxAllocRecordStackDepth) {
      return false;
    }
    mirror::ArtMethod* m = GetMethod();
    if (!m->IsRuntimeMethod()) {
      record->StackElement(depth)->SetMethod(m);
      record->StackElement(depth)->SetDexPc(GetDexPc());
      ++depth;
    }
    return true;
  }
  ~AllocRecordStackVisitor() {
    for (; depth < kMaxAllocRecordStackDepth; ++depth) {
      record->StackElement(depth)->SetMethod(nullptr);
      record->StackElement(depth)->SetDexPc(0);
    }
  }
  AllocRecord* record;
  size_t depth;
};
void Dbg::RecordAllocation(Thread* self, mirror::Class* type, size_t byte_count) {
  MutexLock mu(self, *Locks::alloc_tracker_lock_);
  if (recent_allocation_records_ == nullptr) {
    return;
  }
  if (++alloc_record_head_ == alloc_record_max_) {
    alloc_record_head_ = 0;
  }
  AllocRecord* record = &recent_allocation_records_[alloc_record_head_];
  record->SetType(type);
  record->SetByteCount(byte_count);
  record->SetThinLockId(self->GetThreadId());
  AllocRecordStackVisitor visitor(self, record);
  visitor.WalkStack();
  if (alloc_record_count_ < alloc_record_max_) {
    ++alloc_record_count_;
  }
}
size_t Dbg::HeadIndex() {
  return (Dbg::alloc_record_head_ + 1 + Dbg::alloc_record_max_ - Dbg::alloc_record_count_) &
      (Dbg::alloc_record_max_ - 1);
}
void Dbg::DumpRecentAllocations() {
  ScopedObjectAccess soa(Thread::Current());
  MutexLock mu(soa.Self(), *Locks::alloc_tracker_lock_);
  if (recent_allocation_records_ == nullptr) {
    LOG(INFO) << "Not recording tracked allocations";
    return;
  }
  size_t i = HeadIndex();
  const uint16_t capped_count = CappedAllocRecordCount(Dbg::alloc_record_count_);
  uint16_t count = capped_count;
  LOG(INFO) << "Tracked allocations, (head=" << alloc_record_head_ << " count=" << count << ")";
  while (count--) {
    AllocRecord* record = &recent_allocation_records_[i];
    LOG(INFO) << StringPrintf(" Thread %-2d %6zd bytes ", record->ThinLockId(), record->ByteCount())
              << PrettyClass(record->Type());
    for (size_t stack_frame = 0; stack_frame < kMaxAllocRecordStackDepth; ++stack_frame) {
      AllocRecordStackTraceElement* stack_element = record->StackElement(stack_frame);
      mirror::ArtMethod* m = stack_element->Method();
      if (m == nullptr) {
        break;
      }
      LOG(INFO) << "    " << PrettyMethod(m) << " line " << stack_element->LineNumber();
    }
    if ((count % 5) == 0) {
      usleep(40000);
    }
    i = (i + 1) & (alloc_record_max_ - 1);
  }
}
class StringTable {
public:
  StringTable() {
  }
  void Add(const std::string& str) {
    table_.insert(str);
  }
  void Add(const char* str) {
    table_.insert(str);
  }
  size_t IndexOf(const char* s) const {
    auto it = table_.find(s);
    if (it == table_.end()) {
      LOG(FATAL) << "IndexOf(\"" << s << "\") failed";
    }
    return std::distance(table_.begin(), it);
  }
  size_t Size() const {
    return table_.size();
  }
  void WriteTo(std::vector<uint8_t>& bytes) const {
    for (const std::string& str : table_) {
      const char* s = str.c_str();
      size_t s_len = CountModifiedUtf8Chars(s);
      std::unique_ptr<uint16_t> s_utf16(new uint16_t[s_len]);
      ConvertModifiedUtf8ToUtf16(s_utf16.get(), s);
      JDWP::AppendUtf16BE(bytes, s_utf16.get(), s_len);
    }
  }
private:
  std::set<std::string> table_;
  DISALLOW_COPY_AND_ASSIGN(StringTable);
};
static const char* GetMethodSourceFile(mirror::ArtMethod* method)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(method != nullptr);
    const char* source_file = method->GetDeclaringClassSourceFile();
    return (source_file != nullptr) ? source_file : "";
    }
jbyteArray Dbg::GetRecentAllocations() {
  if (false) {
    DumpRecentAllocations();
  }
  Thread* self = Thread::Current();
  std::vector<uint8_t> bytes;
  {
    MutexLock mu(self, *Locks::alloc_tracker_lock_);
    StringTable class_names;
    StringTable method_names;
    StringTable filenames;
    const uint16_t capped_count = CappedAllocRecordCount(Dbg::alloc_record_count_);
    uint16_t count = capped_count;
    size_t idx = HeadIndex();
    while (count--) {
      AllocRecord* record = &recent_allocation_records_[idx];
      std::string temp;
      class_names.Add(record->Type()->GetDescriptor(&temp));
      for (size_t i = 0; i < kMaxAllocRecordStackDepth; i++) {
        mirror::ArtMethod* m = record->StackElement(i)->Method();
        if (m != nullptr) {
          class_names.Add(m->GetDeclaringClassDescriptor());
          method_names.Add(m->GetName());
          filenames.Add(GetMethodSourceFile(m));
        }
      }
      idx = (idx + 1) & (alloc_record_max_ - 1);
    }
    LOG(INFO) << "allocation records: " << capped_count;
    const int kMessageHeaderLen = 15;
    const int kEntryHeaderLen = 9;
    const int kStackFrameLen = 8;
    JDWP::Append1BE(bytes, kMessageHeaderLen);
    JDWP::Append1BE(bytes, kEntryHeaderLen);
    JDWP::Append1BE(bytes, kStackFrameLen);
    JDWP::Append2BE(bytes, capped_count);
    size_t string_table_offset = bytes.size();
    JDWP::Append4BE(bytes, 0);
    JDWP::Append2BE(bytes, class_names.Size());
    JDWP::Append2BE(bytes, method_names.Size());
    JDWP::Append2BE(bytes, filenames.Size());
    idx = HeadIndex();
    std::string temp;
    for (count = capped_count; count != 0; --count) {
      AllocRecord* record = &recent_allocation_records_[idx];
      size_t stack_depth = record->GetDepth();
      size_t allocated_object_class_name_index =
          class_names.IndexOf(record->Type()->GetDescriptor(&temp));
      JDWP::Append4BE(bytes, record->ByteCount());
      JDWP::Append2BE(bytes, record->ThinLockId());
      JDWP::Append2BE(bytes, allocated_object_class_name_index);
      JDWP::Append1BE(bytes, stack_depth);
      for (size_t stack_frame = 0; stack_frame < stack_depth; ++stack_frame) {
        mirror::ArtMethod* m = record->StackElement(stack_frame)->Method();
        size_t class_name_index = class_names.IndexOf(m->GetDeclaringClassDescriptor());
        size_t method_name_index = method_names.IndexOf(m->GetName());
        size_t file_name_index = filenames.IndexOf(GetMethodSourceFile(m));
        JDWP::Append2BE(bytes, class_name_index);
        JDWP::Append2BE(bytes, method_name_index);
        JDWP::Append2BE(bytes, file_name_index);
        JDWP::Append2BE(bytes, record->StackElement(stack_frame)->LineNumber());
      }
      idx = (idx + 1) & (alloc_record_max_ - 1);
    }
    JDWP::Set4BE(&bytes[string_table_offset], bytes.size());
    class_names.WriteTo(bytes);
    method_names.WriteTo(bytes);
    filenames.WriteTo(bytes);
  }
  JNIEnv* env = self->GetJniEnv();
  jbyteArray result = env->NewByteArray(bytes.size());
  if (result != nullptr) {
    env->SetByteArrayRegion(result, 0, bytes.size(), reinterpret_cast<const jbyte*>(&bytes[0]));
  }
  return result;
}
mirror::ArtMethod* DeoptimizationRequest::Method() const {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  return soa.DecodeMethod(method_);
}
void DeoptimizationRequest::SetMethod(mirror::ArtMethod* m) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  method_ = soa.EncodeMethod(m);
}
static mirror::Class* DecodeClass(JDWP::RefTypeId id, JDWP::JdwpError* error)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_){
    mirror::Object* o = Dbg::GetObjectRegistry()->Get<mirror::Object*>(id, error);
    if (o == nullptr) {
    *error = JDWP::ERR_INVALID_OBJECT;
    return nullptr;
    }
    if (!o->IsClass()) {
    *error = JDWP::ERR_INVALID_CLASS;
    return nullptr;
    }
    *error = JDWP::ERR_NONE;
    return o->AsClass();
    }
JDWP::JdwpError Dbg::GetInstanceCounts(const std::vector<JDWP::RefTypeId>& class_ids, std::vector<uint64_t>* counts) {
  gc::Heap* heap = Runtime::Current()->GetHeap();
  heap->CollectGarbage(false);
  std::vector<mirror::Class*> classes;
  counts->clear();
  for (size_t i = 0; i < class_ids.size(); ++i) {
    JDWP::JdwpError error;
    mirror::Class* c = DecodeClass(class_ids[i], &error);
    if (c == nullptr) {
      return error;
    }
    classes.push_back(c);
    counts->push_back(0);
  }
  heap->CountInstances(classes, false, &(*counts)[0]);
  return JDWP::ERR_NONE;
}
JDWP::JdwpError Dbg::GetInstances(JDWP::RefTypeId class_id, int32_t max_count, std::vector<JDWP::ObjectId>* instances) {
  gc::Heap* heap = Runtime::Current()->GetHeap();
  heap->CollectGarbage(false);
  JDWP::JdwpError error;
  mirror::Class* c = DecodeClass(class_id, &error);
  if (c == nullptr) {
    return error;
  }
  std::vector<mirror::Object*> raw_instances;
  Runtime::Current()->GetHeap()->GetInstances(c, max_count, raw_instances);
  for (size_t i = 0; i < raw_instances.size(); ++i) {
    instances->push_back(gRegistry->Add(raw_instances[i]));
  }
  return JDWP::ERR_NONE;
}
JDWP::JdwpError Dbg::GetReferringObjects(JDWP::ObjectId object_id, int32_t max_count, std::vector<JDWP::ObjectId>* referring_objects) {
  gc::Heap* heap = Runtime::Current()->GetHeap();
  heap->CollectGarbage(false);
  JDWP::JdwpError error;
  mirror::Object* o = gRegistry->Get<mirror::Object*>(object_id, &error);
  if (o == nullptr) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  std::vector<mirror::Object*> raw_instances;
  heap->GetReferringObjects(o, max_count, raw_instances);
  for (size_t i = 0; i < raw_instances.size(); ++i) {
    referring_objects->push_back(gRegistry->Add(raw_instances[i]));
  }
  return JDWP::ERR_NONE;
}
JDWP::JdwpError Dbg::DisableCollection(JDWP::ObjectId object_id) {
  JDWP::JdwpError error;
  mirror::Object* o = gRegistry->Get<mirror::Object*>(object_id, &error);
  if (o == nullptr) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  gRegistry->DisableCollection(object_id);
  return JDWP::ERR_NONE;
}
JDWP::JdwpError Dbg::EnableCollection(JDWP::ObjectId object_id) {
  JDWP::JdwpError error;
  mirror::Object* o = gRegistry->Get<mirror::Object*>(object_id, &error);
  if (o == nullptr) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  gRegistry->EnableCollection(object_id);
  return JDWP::ERR_NONE;
}
JDWP::JdwpError Dbg::IsCollected(JDWP::ObjectId object_id, bool* is_collected) {
  *is_collected = true;
  if (object_id == 0) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  JDWP::JdwpError error;
  mirror::Object* o = gRegistry->Get<mirror::Object*>(object_id, &error);
  if (o != nullptr) {
    *is_collected = gRegistry->IsCollected(object_id);
  }
  return JDWP::ERR_NONE;
}
void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count) {
  gRegistry->DisposeObject(object_id, reference_count);
}
}
