#define LOG_TAG "perfetto_hprof"
#include "perfetto_hprof.h"
#include <fcntl.h>
#include <fnmatch.h>
#include <inttypes.h>
#include <sched.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <time.h>
#include <limits>
#include <optional>
#include <type_traits>
#include "android-base/file.h"
#include "android-base/logging.h"
#include "android-base/properties.h"
#include "base/fast_exit.h"
#include "base/systrace.h"
#include "gc/heap-visit-objects-inl.h"
#include "gc/heap.h"
#include "gc/scoped_gc_critical_section.h"
#include "mirror/object-refvisitor-inl.h"
#include "nativehelper/scoped_local_ref.h"
#include "perfetto/profiling/parse_smaps.h"
#include "perfetto/trace/interned_data/interned_data.pbzero.h"
#include "perfetto/trace/profiling/heap_graph.pbzero.h"
#include "perfetto/trace/profiling/profile_common.pbzero.h"
#include "perfetto/trace/profiling/smaps.pbzero.h"
#include "perfetto/config/profiling/java_hprof_config.pbzero.h"
#include "perfetto/protozero/packed_repeated_fields.h"
#include "perfetto/tracing.h"
#include "runtime-inl.h"
#include "runtime_callbacks.h"
#include "scoped_thread_state_change-inl.h"
#include "thread_list.h"
#include "well_known_classes.h"
#include "dex/descriptors_names.h"
namespace perfetto_hprof {
constexpr int kJavaHeapprofdSignal = __SIGRTMIN + 6;
constexpr time_t kWatchdogTimeoutSec = 120;
constexpr uint32_t kPacketSizeThreshold = 400000;
constexpr char kByte[1] = {'x'};
static art::Mutex& GetStateMutex() {
  static art::Mutex state_mutex("perfetto_hprof_state_mutex", art::LockLevel::kGenericBottomLock);
  return state_mutex;
}
static art::ConditionVariable& GetStateCV() {
  static art::ConditionVariable state_cv("perfetto_hprof_state_cv", GetStateMutex());
  return state_cv;
}
static int requested_tracing_session_id = 0;
static State g_state = State::kUninitialized;
static bool g_oome_triggered = false;
static uint32_t g_oome_sessions_pending = 0;
int g_signal_pipe_fds[2];
static struct sigaction g_orig_act = {};
template <typename T>
uint64_t FindOrAppend(std::map<T, uint64_t>* m, const T& s) {
  auto it = m->find(s);
  if (it == m->end()) {
    std::tie(it, std::ignore) = m->emplace(s, m->size());
  }
  return it->second;
}
void ArmWatchdogOrDie() {
  timer_t timerid{};
  struct sigevent sev {};
  sev.sigev_notify = SIGEV_SIGNAL;
  sev.sigev_signo = SIGKILL;
  if (timer_create(CLOCK_MONOTONIC, &sev, &timerid) == -1) {
    PLOG(FATAL) << "failed to create watchdog timer";
  }
  struct itimerspec its {};
  its.it_value.tv_sec = kWatchdogTimeoutSec;
  if (timer_settime(timerid, 0, &its, nullptr) == -1) {
    PLOG(FATAL) << "failed to arm watchdog timer";
  }
}
bool StartsWith(const std::string& str, const std::string& prefix) {
  return str.compare(0, prefix.length(), prefix) == 0;
}
bool ShouldSampleSmapsEntry(const perfetto::profiling::SmapsEntry& e) {
  if (StartsWith(e.pathname, "/system/") || StartsWith(e.pathname, "/vendor/") ||
      StartsWith(e.pathname, "/data/app/")) {
    return true;
  }
  if (StartsWith(e.pathname, "[anon:")) {
    if (e.pathname.find("extracted in memory from /system/") != std::string::npos) {
      return true;
    }
    if (e.pathname.find("extracted in memory from /vendor/") != std::string::npos) {
      return true;
    }
    if (e.pathname.find("extracted in memory from /data/app/") != std::string::npos) {
      return true;
    }
  }
  return false;
}
uint64_t GetCurrentBootClockNs() {
  struct timespec ts = {};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0) {
    LOG(FATAL) << "Failed to get boottime.";
  }
  return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
bool IsDebugBuild() {
  std::string build_type = android::base::GetProperty("ro.build.type", "");
  return !build_type.empty() && build_type != "user";
}
bool IsOomeHeapDumpAllowed(const perfetto::DataSourceConfig& ds_config) {
  if (art::Runtime::Current()->IsJavaDebuggable() || IsDebugBuild()) {
    return true;
  }
  if (ds_config.session_initiator() ==
      perfetto::DataSourceConfig::SESSION_INITIATOR_TRUSTED_SYSTEM) {
    return art::Runtime::Current()->IsProfileable() || art::Runtime::Current()->IsSystemServer();
  } else {
    return art::Runtime::Current()->IsProfileableFromShell();
  }
}
class JavaHprofDataSource : public perfetto::DataSource<JavaHprofDataSource> {
 public:
  constexpr static perfetto::BufferExhaustedPolicy kBufferExhaustedPolicy =
    perfetto::BufferExhaustedPolicy::kStall;
  explicit JavaHprofDataSource(bool is_oome_heap) : is_oome_heap_(is_oome_heap) {}
  void OnSetup(const SetupArgs& args) override {
    if (!is_oome_heap_) {
      uint64_t normalized_tracing_session_id =
        args.config->tracing_session_id() % std::numeric_limits<int32_t>::max();
      if (requested_tracing_session_id < 0) {
        LOG(ERROR) << "invalid requested tracing session id " << requested_tracing_session_id;
        return;
      }
      if (static_cast<uint64_t>(requested_tracing_session_id) != normalized_tracing_session_id) {
        return;
      }
    }
    std::unique_ptr<perfetto::protos::pbzero::JavaHprofConfig::Decoder> cfg(
        new perfetto::protos::pbzero::JavaHprofConfig::Decoder(
          args.config->java_hprof_config_raw()));
    dump_smaps_ = cfg->dump_smaps();
    for (auto it = cfg->ignored_types(); it; ++it) {
      std::string name = (*it).ToStdString();
      ignored_types_.emplace_back(std::move(name));
    }
    enabled_ =
        !is_oome_heap_ || (IsOomeHeapDumpAllowed(*args.config) && IsOomeDumpEnabled(*cfg.get()));
  }
  bool dump_smaps() { return dump_smaps_; }
  bool enabled() { return enabled_; }
  void OnStart(const StartArgs&) override {
    art::MutexLock lk(art_thread(), GetStateMutex());
    if (is_oome_heap_ && g_oome_sessions_pending > 0) {
      --g_oome_sessions_pending;
    }
    if (g_state == State::kWaitForStart) {
      if (!is_oome_heap_ || g_oome_sessions_pending == 0) {
        g_state = State::kStart;
        GetStateCV().Broadcast(art_thread());
      }
    }
  }
  void OnStop(const StopArgs& a) override {
    art::MutexLock lk(art_thread(), finish_mutex_);
    if (is_finished_) {
      return;
    }
    is_stopped_ = true;
    async_stop_ = std::move(a.HandleStopAsynchronously());
  }
  static art::Thread* art_thread() {
    return nullptr;
  }
  std::vector<std::string> ignored_types() { return ignored_types_; }
  void Finish() {
    art::MutexLock lk(art_thread(), finish_mutex_);
    if (is_stopped_) {
      async_stop_();
    } else {
      is_finished_ = true;
    }
  }
 private:
  static bool IsOomeDumpEnabled(const perfetto::protos::pbzero::JavaHprofConfig::Decoder& cfg) {
    std::string cmdline;
    if (!android::base::ReadFileToString("/proc/self/cmdline", &cmdline)) {
      return false;
    }
    const char* argv0 = cmdline.c_str();
    for (auto it = cfg.process_cmdline(); it; ++it) {
      std::string pattern = (*it).ToStdString();
      if (fnmatch(pattern.c_str(), argv0, FNM_NOESCAPE) == 0) {
        return true;
      }
    }
    return false;
  }
  bool is_oome_heap_ = false;
  bool enabled_ = false;
  bool dump_smaps_ = false;
  std::vector<std::string> ignored_types_;
  art::Mutex finish_mutex_{"perfetto_hprof_ds_mutex", art::LockLevel::kGenericBottomLock};
  bool is_finished_ = false;
  bool is_stopped_ = false;
  std::function<void()> async_stop_;
};
void SetupDataSource(const std::string& ds_name, bool is_oome_heap) {
  perfetto::TracingInitArgs args;
  args.backends = perfetto::BackendType::kSystemBackend;
  perfetto::Tracing::Initialize(args);
  perfetto::DataSourceDescriptor dsd;
  dsd.set_name(ds_name);
  dsd.set_will_notify_on_stop(true);
  JavaHprofDataSource::Register(dsd, is_oome_heap);
  LOG(INFO) << "registered data source " << ds_name;
}
void WaitForDataSource(art::Thread* self) {
  art::MutexLock lk(self, GetStateMutex());
  while (g_state != State::kStart) {
    GetStateCV().Wait(self);
  }
}
bool TimedWaitForDataSource(art::Thread* self, int64_t timeout_ms) {
  const uint64_t cutoff_ns = GetCurrentBootClockNs() + timeout_ms * 1000000;
  art::MutexLock lk(self, GetStateMutex());
  while (g_state != State::kStart) {
    const uint64_t current_ns = GetCurrentBootClockNs();
    if (current_ns >= cutoff_ns) {
      return false;
    }
    GetStateCV().TimedWait(self, (cutoff_ns - current_ns) / 1000000, 0);
  }
  return true;
}
class Writer {
 public:
  Writer(pid_t pid, JavaHprofDataSource::TraceContext* ctx, uint64_t timestamp)
      : pid_(pid), ctx_(ctx), timestamp_(timestamp),
        last_written_(ctx_->written()) {}
  bool will_create_new_packet() const {
    return !heap_graph_ || ctx_->written() - last_written_ > kPacketSizeThreshold;
  }
  perfetto::protos::pbzero::HeapGraph* GetHeapGraph() {
    if (will_create_new_packet()) {
      CreateNewHeapGraph();
    }
    return heap_graph_;
  }
  void Finalize() {
    if (trace_packet_) {
      trace_packet_->Finalize();
    }
    heap_graph_ = nullptr;
  }
  ~Writer() { Finalize(); }
 private:
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
  Writer(Writer&&) = delete;
  Writer& operator=(Writer&&) = delete;
  void CreateNewHeapGraph() {
    if (heap_graph_) {
      heap_graph_->set_continued(true);
    }
    Finalize();
    uint64_t written = ctx_->written();
    trace_packet_ = ctx_->NewTracePacket();
    trace_packet_->set_timestamp(timestamp_);
    heap_graph_ = trace_packet_->set_heap_graph();
    heap_graph_->set_pid(pid_);
    heap_graph_->set_index(index_++);
    last_written_ = written;
  }
  const pid_t pid_;
  JavaHprofDataSource::TraceContext* const ctx_;
  const uint64_t timestamp_;
  uint64_t last_written_ = 0;
  perfetto::DataSource<JavaHprofDataSource>::TraceContext::TracePacketHandle
      trace_packet_;
  perfetto::protos::pbzero::HeapGraph* heap_graph_ = nullptr;
  uint64_t index_ = 0;
};
class ReferredObjectsFinder {
 public:
  explicit ReferredObjectsFinder(
      std::vector<std::pair<std::string, art::mirror::Object*>>* referred_objects)
      : referred_objects_(referred_objects) {}
  void operator()(art::ObjPtr<art::mirror::Object> obj, art::MemberOffset offset,
                  bool is_static) const
      REQUIRES_SHARED(art::Locks::mutator_lock_) {
    if (offset.Uint32Value() == art::mirror::Object::ClassOffset().Uint32Value()) {
      return;
    }
    art::mirror::Object* ref = obj->GetFieldObject<art::mirror::Object>(offset);
    art::ArtField* field;
    if (is_static) {
      field = art::ArtField::FindStaticFieldWithOffset(obj->AsClass(), offset.Uint32Value());
    } else {
      field = art::ArtField::FindInstanceFieldWithOffset(obj->GetClass(), offset.Uint32Value());
    }
    std::string field_name = "";
    if (field != nullptr) {
      field_name = field->PrettyField( true);
    }
    referred_objects_->emplace_back(std::move(field_name), ref);
  }
  void VisitRootIfNonNull(art::mirror::CompressedReference<art::mirror::Object>* root
                              ATTRIBUTE_UNUSED) const {}
  void VisitRoot(art::mirror::CompressedReference<art::mirror::Object>* root
                     ATTRIBUTE_UNUSED) const {}
 private:
  std::vector<std::pair<std::string, art::mirror::Object*>>* referred_objects_;
};
class RootFinder : public art::SingleRootVisitor {
 public:
  explicit RootFinder(
    std::map<art::RootType, std::vector<art::mirror::Object*>>* root_objects)
      : root_objects_(root_objects) {}
  void VisitRoot(art::mirror::Object* root, const art::RootInfo& info) override {
    (*root_objects_)[info.GetType()].emplace_back(root);
  }
 private:
  std::map<art::RootType, std::vector<art::mirror::Object*>>* root_objects_;
};
perfetto::protos::pbzero::HeapGraphRoot::Type ToProtoType(art::RootType art_type) {
  using perfetto::protos::pbzero::HeapGraphRoot;
  switch (art_type) {
    case art::kRootUnknown:
      return HeapGraphRoot::ROOT_UNKNOWN;
    case art::kRootJNIGlobal:
      return HeapGraphRoot::ROOT_JNI_GLOBAL;
    case art::kRootJNILocal:
      return HeapGraphRoot::ROOT_JNI_LOCAL;
    case art::kRootJavaFrame:
      return HeapGraphRoot::ROOT_JAVA_FRAME;
    case art::kRootNativeStack:
      return HeapGraphRoot::ROOT_NATIVE_STACK;
    case art::kRootStickyClass:
      return HeapGraphRoot::ROOT_STICKY_CLASS;
    case art::kRootThreadBlock:
      return HeapGraphRoot::ROOT_THREAD_BLOCK;
    case art::kRootMonitorUsed:
      return HeapGraphRoot::ROOT_MONITOR_USED;
    case art::kRootThreadObject:
      return HeapGraphRoot::ROOT_THREAD_OBJECT;
    case art::kRootInternedString:
      return HeapGraphRoot::ROOT_INTERNED_STRING;
    case art::kRootFinalizing:
      return HeapGraphRoot::ROOT_FINALIZING;
    case art::kRootDebugger:
      return HeapGraphRoot::ROOT_DEBUGGER;
    case art::kRootReferenceCleanup:
      return HeapGraphRoot::ROOT_REFERENCE_CLEANUP;
    case art::kRootVMInternal:
      return HeapGraphRoot::ROOT_VM_INTERNAL;
    case art::kRootJNIMonitor:
      return HeapGraphRoot::ROOT_JNI_MONITOR;
  }
}
perfetto::protos::pbzero::HeapGraphType::Kind ProtoClassKind(uint32_t class_flags) {
  using perfetto::protos::pbzero::HeapGraphType;
  switch (class_flags) {
    case art::mirror::kClassFlagNormal:
      return HeapGraphType::KIND_NORMAL;
    case art::mirror::kClassFlagNoReferenceFields:
      return HeapGraphType::KIND_NOREFERENCES;
    case art::mirror::kClassFlagString | art::mirror::kClassFlagNoReferenceFields:
      return HeapGraphType::KIND_STRING;
    case art::mirror::kClassFlagObjectArray:
      return HeapGraphType::KIND_ARRAY;
    case art::mirror::kClassFlagClass:
      return HeapGraphType::KIND_CLASS;
    case art::mirror::kClassFlagClassLoader:
      return HeapGraphType::KIND_CLASSLOADER;
    case art::mirror::kClassFlagDexCache:
      return HeapGraphType::KIND_DEXCACHE;
    case art::mirror::kClassFlagSoftReference:
      return HeapGraphType::KIND_SOFT_REFERENCE;
    case art::mirror::kClassFlagWeakReference:
      return HeapGraphType::KIND_WEAK_REFERENCE;
    case art::mirror::kClassFlagFinalizerReference:
      return HeapGraphType::KIND_FINALIZER_REFERENCE;
    case art::mirror::kClassFlagPhantomReference:
      return HeapGraphType::KIND_PHANTOM_REFERENCE;
    default:
      return HeapGraphType::KIND_UNKNOWN;
  }
}
std::string PrettyType(art::mirror::Class* klass) NO_THREAD_SAFETY_ANALYSIS {
  if (klass == nullptr) {
    return "(raw)";
  }
  std::string temp;
  std::string result(art::PrettyDescriptor(klass->GetDescriptor(&temp)));
  return result;
}
void DumpSmaps(JavaHprofDataSource::TraceContext* ctx) {
  FILE* smaps = fopen("/proc/self/smaps", "re");
  if (smaps != nullptr) {
    auto trace_packet = ctx->NewTracePacket();
    auto* smaps_packet = trace_packet->set_smaps_packet();
    smaps_packet->set_pid(getpid());
    perfetto::profiling::ParseSmaps(smaps,
        [&smaps_packet](const perfetto::profiling::SmapsEntry& e) {
      if (ShouldSampleSmapsEntry(e)) {
        auto* smaps_entry = smaps_packet->add_entries();
        smaps_entry->set_path(e.pathname);
        smaps_entry->set_size_kb(e.size_kb);
        smaps_entry->set_private_dirty_kb(e.private_dirty_kb);
        smaps_entry->set_swap_kb(e.swap_kb);
      }
    });
    fclose(smaps);
  } else {
    PLOG(ERROR) << "failed to open smaps";
  }
}
uint64_t GetObjectId(const art::mirror::Object* obj) {
  return reinterpret_cast<uint64_t>(obj) / std::alignment_of<art::mirror::Object>::value;
}
template <typename F>
void ForInstanceReferenceField(art::mirror::Class* klass, F fn) NO_THREAD_SAFETY_ANALYSIS {
  for (art::ArtField& af : klass->GetIFields()) {
    if (af.IsPrimitiveType() ||
        af.GetOffset().Uint32Value() == art::mirror::Object::ClassOffset().Uint32Value()) {
      continue;
    }
    fn(af.GetOffset());
  }
}
size_t EncodedSize(uint64_t n) {
  if (n == 0) return 1;
  return 1 + static_cast<size_t>(art::MostSignificantBit(n)) / 7;
}
std::vector<std::pair<std::string, art::mirror::Object*>> GetReferences(art::mirror::Object* obj,
                                                                        art::mirror::Class* klass)
    REQUIRES_SHARED(art::Locks::mutator_lock_) {
  std::vector<std::pair<std::string, art::mirror::Object*>> referred_objects;
  ReferredObjectsFinder objf(&referred_objects);
  if (klass->GetClassFlags() != art::mirror::kClassFlagNormal &&
      klass->GetClassFlags() != art::mirror::kClassFlagPhantomReference) {
    obj->VisitReferences(objf, art::VoidFunctor());
  } else {
    for (art::mirror::Class* cls = klass; cls != nullptr; cls = cls->GetSuperClass().Ptr()) {
      ForInstanceReferenceField(cls,
                                [obj, objf](art::MemberOffset offset) NO_THREAD_SAFETY_ANALYSIS {
                                  objf(art::ObjPtr<art::mirror::Object>(obj),
                                       offset,
                                                     false);
                                });
    }
  }
  return referred_objects;
}
uint64_t EncodeBaseObjId(
    const std::vector<std::pair<std::string, art::mirror::Object*>>& referred_objects,
    const art::mirror::Object* min_nonnull_ptr) REQUIRES_SHARED(art::Locks::mutator_lock_) {
  uint64_t base_obj_id = GetObjectId(min_nonnull_ptr);
  if (base_obj_id <= 1) {
    return 0;
  }
  base_obj_id--;
  uint64_t bytes_saved = 0;
  for (const auto& p : referred_objects) {
    art::mirror::Object* referred_obj = p.second;
    if (!referred_obj) {
      continue;
    }
    uint64_t referred_obj_id = GetObjectId(referred_obj);
    bytes_saved += EncodedSize(referred_obj_id) - EncodedSize(referred_obj_id - base_obj_id);
  }
  if (bytes_saved <= EncodedSize(base_obj_id) + 1) {
    return 0;
  }
  return base_obj_id;
}
class HeapGraphDumper {
 public:
  explicit HeapGraphDumper(const std::vector<std::string>& ignored_types)
      : ignored_types_(ignored_types),
        reference_field_ids_(std::make_unique<protozero::PackedVarInt>()),
        reference_object_ids_(std::make_unique<protozero::PackedVarInt>()) {}
  void Dump(art::Runtime* runtime, Writer& writer) REQUIRES(art::Locks::mutator_lock_) {
    DumpRootObjects(runtime, writer);
    DumpObjects(runtime, writer);
    WriteInternedData(writer);
  }
 private:
  void DumpRootObjects(art::Runtime* runtime, Writer& writer)
      REQUIRES_SHARED(art::Locks::mutator_lock_) {
    std::map<art::RootType, std::vector<art::mirror::Object*>> root_objects;
    RootFinder rcf(&root_objects);
    runtime->VisitRoots(&rcf);
    std::unique_ptr<protozero::PackedVarInt> object_ids(new protozero::PackedVarInt);
    for (const auto& p : root_objects) {
      const art::RootType root_type = p.first;
      const std::vector<art::mirror::Object*>& children = p.second;
      perfetto::protos::pbzero::HeapGraphRoot* root_proto = writer.GetHeapGraph()->add_roots();
      root_proto->set_root_type(ToProtoType(root_type));
      for (art::mirror::Object* obj : children) {
        if (writer.will_create_new_packet()) {
          root_proto->set_object_ids(*object_ids);
          object_ids->Reset();
          root_proto = writer.GetHeapGraph()->add_roots();
          root_proto->set_root_type(ToProtoType(root_type));
        }
        object_ids->Append(GetObjectId(obj));
      }
      root_proto->set_object_ids(*object_ids);
      object_ids->Reset();
    }
  }
  void DumpObjects(art::Runtime* runtime, Writer& writer) REQUIRES(art::Locks::mutator_lock_) {
    runtime->GetHeap()->VisitObjectsPaused(
        [this, &writer](art::mirror::Object* obj)
            REQUIRES_SHARED(art::Locks::mutator_lock_) { WriteOneObject(obj, writer); });
  }
  void WriteInternedData(Writer& writer) {
    for (const auto& p : interned_locations_) {
      const std::string& str = p.first;
      uint64_t id = p.second;
      perfetto::protos::pbzero::InternedString* location_proto =
          writer.GetHeapGraph()->add_location_names();
      location_proto->set_iid(id);
      location_proto->set_str(reinterpret_cast<const uint8_t*>(str.c_str()), str.size());
    }
    for (const auto& p : interned_fields_) {
      const std::string& str = p.first;
      uint64_t id = p.second;
      perfetto::protos::pbzero::InternedString* field_proto =
          writer.GetHeapGraph()->add_field_names();
      field_proto->set_iid(id);
      field_proto->set_str(reinterpret_cast<const uint8_t*>(str.c_str()), str.size());
    }
  }
  void WriteOneObject(art::mirror::Object* obj, Writer& writer)
      REQUIRES_SHARED(art::Locks::mutator_lock_) {
    if (obj->IsClass()) {
      WriteClass(obj->AsClass().Ptr(), writer);
    }
    art::mirror::Class* klass = obj->GetClass();
    uintptr_t class_ptr = reinterpret_cast<uintptr_t>(klass);
    if (klass->IsClassClass()) {
      class_ptr = WriteSyntheticClassFromObj(obj, writer);
    }
    if (IsIgnored(obj)) {
      return;
    }
    auto class_id = FindOrAppend(&interned_classes_, class_ptr);
    uint64_t object_id = GetObjectId(obj);
    perfetto::protos::pbzero::HeapGraphObject* object_proto = writer.GetHeapGraph()->add_objects();
    if (prev_object_id_ && prev_object_id_ < object_id) {
      object_proto->set_id_delta(object_id - prev_object_id_);
    } else {
      object_proto->set_id(object_id);
    }
    prev_object_id_ = object_id;
    object_proto->set_type_id(class_id);
    if (obj->SizeOf() != klass->GetObjectSize()) {
      object_proto->set_self_size(obj->SizeOf());
    }
    FillReferences(obj, klass, object_proto);
    FillFieldValues(obj, klass, object_proto);
  }
  void WriteClass(art::mirror::Class* klass, Writer& writer)
      REQUIRES_SHARED(art::Locks::mutator_lock_) {
    perfetto::protos::pbzero::HeapGraphType* type_proto = writer.GetHeapGraph()->add_types();
    type_proto->set_id(FindOrAppend(&interned_classes_, reinterpret_cast<uintptr_t>(klass)));
    type_proto->set_class_name(PrettyType(klass));
    type_proto->set_location_id(FindOrAppend(&interned_locations_, klass->GetLocation()));
    type_proto->set_object_size(klass->GetObjectSize());
    type_proto->set_kind(ProtoClassKind(klass->GetClassFlags()));
    type_proto->set_classloader_id(GetObjectId(klass->GetClassLoader().Ptr()));
    if (klass->GetSuperClass().Ptr()) {
      type_proto->set_superclass_id(FindOrAppend(
          &interned_classes_, reinterpret_cast<uintptr_t>(klass->GetSuperClass().Ptr())));
    }
    ForInstanceReferenceField(
        klass, [klass, this](art::MemberOffset offset) NO_THREAD_SAFETY_ANALYSIS {
          auto art_field = art::ArtField::FindInstanceFieldWithOffset(klass, offset.Uint32Value());
          reference_field_ids_->Append(
              FindOrAppend(&interned_fields_, art_field->PrettyField(true)));
        });
    type_proto->set_reference_field_id(*reference_field_ids_);
    reference_field_ids_->Reset();
  }
  uintptr_t WriteSyntheticClassFromObj(art::mirror::Object* obj, Writer& writer)
      REQUIRES_SHARED(art::Locks::mutator_lock_) {
    CHECK(obj->IsClass());
    perfetto::protos::pbzero::HeapGraphType* type_proto = writer.GetHeapGraph()->add_types();
    uintptr_t class_ptr = reinterpret_cast<uintptr_t>(obj) | 1;
    auto class_id = FindOrAppend(&interned_classes_, class_ptr);
    type_proto->set_id(class_id);
    type_proto->set_class_name(obj->PrettyTypeOf());
    type_proto->set_location_id(FindOrAppend(&interned_locations_, obj->AsClass()->GetLocation()));
    return class_ptr;
  }
  void FillReferences(art::mirror::Object* obj,
                      art::mirror::Class* klass,
                      perfetto::protos::pbzero::HeapGraphObject* object_proto)
      REQUIRES_SHARED(art::Locks::mutator_lock_) {
    std::vector<std::pair<std::string, art::mirror::Object*>> referred_objects =
        GetReferences(obj, klass);
    art::mirror::Object* min_nonnull_ptr = FilterIgnoredReferencesAndFindMin(referred_objects);
    uint64_t base_obj_id = EncodeBaseObjId(referred_objects, min_nonnull_ptr);
    const bool emit_field_ids = klass->GetClassFlags() != art::mirror::kClassFlagObjectArray &&
                                klass->GetClassFlags() != art::mirror::kClassFlagNormal &&
                                klass->GetClassFlags() != art::mirror::kClassFlagPhantomReference;
    for (const auto& p : referred_objects) {
      const std::string& field_name = p.first;
      art::mirror::Object* referred_obj = p.second;
      if (emit_field_ids) {
        reference_field_ids_->Append(FindOrAppend(&interned_fields_, field_name));
      }
      uint64_t referred_obj_id = GetObjectId(referred_obj);
      if (referred_obj_id) {
        referred_obj_id -= base_obj_id;
      }
      reference_object_ids_->Append(referred_obj_id);
    }
    if (emit_field_ids) {
      object_proto->set_reference_field_id(*reference_field_ids_);
      reference_field_ids_->Reset();
    }
    if (base_obj_id) {
      object_proto->set_reference_field_id_base(base_obj_id);
    }
    object_proto->set_reference_object_id(*reference_object_ids_);
    reference_object_ids_->Reset();
  }
  art::mirror::Object* FilterIgnoredReferencesAndFindMin(
      std::vector<std::pair<std::string, art::mirror::Object*>>& referred_objects) const
      REQUIRES_SHARED(art::Locks::mutator_lock_) {
    art::mirror::Object* min_nonnull_ptr = nullptr;
    for (auto& p : referred_objects) {
      art::mirror::Object*& referred_obj = p.second;
      if (referred_obj == nullptr)
        continue;
      if (IsIgnored(referred_obj)) {
        referred_obj = nullptr;
        continue;
      }
      if (min_nonnull_ptr == nullptr || min_nonnull_ptr > referred_obj) {
        min_nonnull_ptr = referred_obj;
      }
    }
    return min_nonnull_ptr;
  }
  void FillFieldValues(art::mirror::Object* obj,
                       art::mirror::Class* klass,
                       perfetto::protos::pbzero::HeapGraphObject* object_proto) const
      REQUIRES_SHARED(art::Locks::mutator_lock_) {
    if (obj->IsClass() || klass->IsClassClass()) {
      return;
    }
    for (art::mirror::Class* cls = klass; cls != nullptr; cls = cls->GetSuperClass().Ptr()) {
      if (cls->IsArrayClass()) {
        continue;
      }
      if (cls->DescriptorEquals("Llibcore/util/NativeAllocationRegistry;")) {
        art::ArtField* af = cls->FindDeclaredInstanceField(
            "size", art::Primitive::Descriptor(art::Primitive::kPrimLong));
        if (af) {
          object_proto->set_native_allocation_registry_size_field(af->GetLong(obj));
        }
      }
    }
  }
  bool IsIgnored(art::mirror::Object* obj) const REQUIRES_SHARED(art::Locks::mutator_lock_) {
    if (obj->IsClass()) {
      return false;
    }
    art::mirror::Class* klass = obj->GetClass();
    return std::find(ignored_types_.begin(), ignored_types_.end(), PrettyType(klass)) !=
           ignored_types_.end();
  }
  const std::vector<std::string> ignored_types_;
  std::map<std::string, uint64_t> interned_fields_{{"", 0}};
  std::map<std::string, uint64_t> interned_locations_{{"", 0}};
  std::map<uintptr_t, uint64_t> interned_classes_{{0, 0}};
  std::unique_ptr<protozero::PackedVarInt> reference_field_ids_;
  std::unique_ptr<protozero::PackedVarInt> reference_object_ids_;
  uint64_t prev_object_id_ = 0;
};
void BusyWaitpid(pid_t pid, uint32_t timeout_ms) {
  for (size_t i = 0;; ++i) {
    if (i == timeout_ms) {
      LOG(ERROR) << "perfetto_hprof child timed out. Sending SIGKILL.";
      kill(pid, SIGKILL);
    }
    int stat_loc;
    pid_t wait_result = waitpid(pid, &stat_loc, WNOHANG);
    if (wait_result == -1 && errno != EINTR) {
      if (errno != ECHILD) {
        PLOG(FATAL_WITHOUT_ABORT) << "waitpid";
      }
      break;
    } else if (wait_result > 0) {
      break;
    } else {
      usleep(1000);
    }
  }
}
enum class ResumeParentPolicy {
  IMMEDIATELY,
  DEFERRED
};
void ForkAndRun(art::Thread* self,
                ResumeParentPolicy resume_parent_policy,
                const std::function<void(pid_t child)>& parent_runnable,
                const std::function<void(pid_t parent, uint64_t timestamp)>& child_runnable) {
  pid_t parent_pid = getpid();
  LOG(INFO) << "forking for " << parent_pid;
  std::optional<art::gc::ScopedGCCriticalSection> gcs(std::in_place, self, art::gc::kGcCauseHprof,
                                                      art::gc::kCollectorTypeHprof);
  std::optional<art::ScopedSuspendAll> ssa(std::in_place, __FUNCTION__, true);
  pid_t pid = fork();
  if (pid == -1) {
    PLOG(ERROR) << "fork";
    return;
  }
  if (pid != 0) {
    if (resume_parent_policy == ResumeParentPolicy::IMMEDIATELY) {
      ssa.reset();
      gcs.reset();
    }
    parent_runnable(pid);
    if (resume_parent_policy != ResumeParentPolicy::IMMEDIATELY) {
      ssa.reset();
      gcs.reset();
    }
    return;
  }
  if (sigaction(kJavaHeapprofdSignal, &g_orig_act, nullptr) != 0) {
    close(g_signal_pipe_fds[0]);
    close(g_signal_pipe_fds[1]);
    PLOG(FATAL) << "Failed to sigaction";
    return;
  }
  uint64_t ts = GetCurrentBootClockNs();
  child_runnable(parent_pid, ts);
  art::FastExit(0);
}
void WriteHeapPackets(pid_t parent_pid, uint64_t timestamp) {
  JavaHprofDataSource::Trace(
      [parent_pid, timestamp](JavaHprofDataSource::TraceContext ctx)
          NO_THREAD_SAFETY_ANALYSIS {
            bool dump_smaps;
            std::vector<std::string> ignored_types;
            {
              auto ds = ctx.GetDataSourceLocked();
              if (!ds || !ds->enabled()) {
                if (ds) ds->Finish();
                LOG(INFO) << "skipping irrelevant data source.";
                return;
              }
              dump_smaps = ds->dump_smaps();
              ignored_types = ds->ignored_types();
            }
            LOG(INFO) << "dumping heap for " << parent_pid;
            if (dump_smaps) {
              DumpSmaps(&ctx);
            }
            Writer writer(parent_pid, &ctx, timestamp);
            HeapGraphDumper dumper(ignored_types);
            dumper.Dump(art::Runtime::Current(), writer);
            writer.Finalize();
            ctx.Flush([] {
              art::MutexLock lk(JavaHprofDataSource::art_thread(), GetStateMutex());
              g_state = State::kEnd;
              GetStateCV().Broadcast(JavaHprofDataSource::art_thread());
            });
            {
              art::MutexLock lk(JavaHprofDataSource::art_thread(), GetStateMutex());
              while (g_state != State::kEnd) {
                GetStateCV().Wait(JavaHprofDataSource::art_thread());
              }
            }
            {
              auto ds = ctx.GetDataSourceLocked();
              if (ds) {
                ds->Finish();
              } else {
                LOG(ERROR) << "datasource timed out (duration_ms + datasource_stop_timeout_ms) "
                              "before dump finished";
              }
            }
          });
}
void DumpPerfetto(art::Thread* self) {
  ForkAndRun(
    self,
    ResumeParentPolicy::IMMEDIATELY,
    [](pid_t child) {
      BusyWaitpid(child, 1000);
    },
    [self](pid_t dumped_pid, uint64_t timestamp) {
      if (daemon(0, 0) == -1) {
        PLOG(FATAL) << "daemon";
      }
      ArmWatchdogOrDie();
      SetupDataSource("android.java_hprof", false);
      WaitForDataSource(self);
      WriteHeapPackets(dumped_pid, timestamp);
      LOG(INFO) << "finished dumping heap for " << dumped_pid;
    });
}
void DumpPerfettoOutOfMemory() REQUIRES_SHARED(art::Locks::mutator_lock_) {
  art::Thread* self = art::Thread::Current();
  if (!self) {
    LOG(FATAL_WITHOUT_ABORT) << "no thread in DumpPerfettoOutOfMemory";
    return;
  }
  uint32_t session_cnt =
      android::base::GetUintProperty<uint32_t>("traced.oome_heap_session.count", 0);
  if (session_cnt == 0) {
    return;
  }
  {
    art::MutexLock lk(self, GetStateMutex());
    if (g_oome_triggered) {
      return;
    }
    g_oome_triggered = true;
    g_oome_sessions_pending = session_cnt;
  }
  art::ScopedThreadSuspension sts(self, art::ThreadState::kSuspended);
  ForkAndRun(
    self,
    ResumeParentPolicy::DEFERRED,
    [](pid_t child) {
      BusyWaitpid(child, kWatchdogTimeoutSec * 1000);
    },
    [self](pid_t dumped_pid, uint64_t timestamp) {
      ArmWatchdogOrDie();
      art::ScopedTrace trace("perfetto_hprof oome");
      SetupDataSource("android.java_hprof.oom", true);
      perfetto::Tracing::ActivateTriggers({"com.android.telemetry.art-outofmemory"}, 500);
      if (!TimedWaitForDataSource(self, 1000)) {
        LOG(INFO) << "OOME hprof timeout (state " << g_state << ")";
        return;
      }
      WriteHeapPackets(dumped_pid, timestamp);
      LOG(INFO) << "OOME hprof complete for " << dumped_pid;
    });
}
extern "C" bool ArtPlugin_Initialize() {
  if (art::Runtime::Current() == nullptr) {
    return false;
  }
  art::Thread* self = art::Thread::Current();
  {
    art::MutexLock lk(self, GetStateMutex());
    if (g_state != State::kUninitialized) {
      LOG(ERROR) << "perfetto_hprof already initialized. state: " << g_state;
      return false;
    }
    g_state = State::kWaitForListener;
  }
  if (pipe2(g_signal_pipe_fds, O_CLOEXEC) == -1) {
    PLOG(ERROR) << "Failed to pipe";
    return false;
  }
  struct sigaction act = {};
  act.sa_flags = SA_SIGINFO | SA_RESTART;
  act.sa_sigaction = [](int, siginfo_t* si, void*) {
    requested_tracing_session_id = si->si_value.sival_int;
    if (write(g_signal_pipe_fds[1], kByte, sizeof(kByte)) == -1) {
      PLOG(ERROR) << "Failed to trigger heap dump";
    }
  };
  if (sigaction(kJavaHeapprofdSignal, &act, &g_orig_act) != 0) {
    close(g_signal_pipe_fds[0]);
    close(g_signal_pipe_fds[1]);
    PLOG(ERROR) << "Failed to sigaction";
    return false;
  }
  std::thread th([] {
    art::Runtime* runtime = art::Runtime::Current();
    if (!runtime) {
      LOG(FATAL_WITHOUT_ABORT) << "no runtime in perfetto_hprof_listener";
      return;
    }
    if (!runtime->AttachCurrentThread("perfetto_hprof_listener", true,
                                      runtime->GetSystemThreadGroup(), false)) {
      LOG(ERROR) << "failed to attach thread.";
      {
        art::MutexLock lk(nullptr, GetStateMutex());
        g_state = State::kUninitialized;
        GetStateCV().Broadcast(nullptr);
      }
      return;
    }
    art::Thread* self = art::Thread::Current();
    if (!self) {
      LOG(FATAL_WITHOUT_ABORT) << "no thread in perfetto_hprof_listener";
      return;
    }
    {
      art::MutexLock lk(self, GetStateMutex());
      if (g_state == State::kWaitForListener) {
        g_state = State::kWaitForStart;
        GetStateCV().Broadcast(self);
      }
    }
    char buf[1];
    for (;;) {
      int res;
      do {
        res = read(g_signal_pipe_fds[0], buf, sizeof(buf));
      } while (res == -1 && errno == EINTR);
      if (res <= 0) {
        if (res == -1) {
          PLOG(ERROR) << "failed to read";
        }
        close(g_signal_pipe_fds[0]);
        return;
      }
      perfetto_hprof::DumpPerfetto(self);
    }
  });
  th.detach();
  art::Runtime::Current()->SetOutOfMemoryErrorHook(perfetto_hprof::DumpPerfettoOutOfMemory);
  return true;
}
extern "C" bool ArtPlugin_Deinitialize() {
  art::Runtime::Current()->SetOutOfMemoryErrorHook(nullptr);
  if (sigaction(kJavaHeapprofdSignal, &g_orig_act, nullptr) != 0) {
    PLOG(ERROR) << "failed to reset signal handler";
    return false;
  }
  close(g_signal_pipe_fds[1]);
  art::Thread* self = art::Thread::Current();
  art::MutexLock lk(self, GetStateMutex());
  while (g_state == State::kWaitForListener) {
    GetStateCV().Wait(art::Thread::Current());
  }
  g_state = State::kUninitialized;
  GetStateCV().Broadcast(self);
  return true;
}
}
namespace perfetto {
PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(perfetto_hprof::JavaHprofDataSource);
}
