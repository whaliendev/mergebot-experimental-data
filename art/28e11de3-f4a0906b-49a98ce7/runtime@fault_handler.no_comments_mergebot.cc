#include "fault_handler.h"
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <atomic>
#include "art_method-inl.h"
#include "base/logging.h"
#include "base/membarrier.h"
#include "base/safe_copy.h"
#include "base/stl_util.h"
#include "dex/dex_file_types.h"
#include "gc/space/bump_pointer_space.h"
#include "gc/heap.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "mirror/class.h"
#include "mirror/object_reference.h"
#include "oat_file.h"
#include "oat_quick_method_header.h"
#include "sigchain.h"
#include "thread-current-inl.h"
#include "verify_object-inl.h"
namespace art {
FaultManager fault_manager;
extern "C" NO_INLINE __attribute__((visibility("default"))) void art_sigsegv_fault() {
  VLOG(signals) << "Caught unknown SIGSEGV in ART fault handler - chaining to next handler.";
}
static bool art_sigsegv_handler(int sig, siginfo_t* info, void* context) {
  return fault_manager.HandleSigsegvFault(sig, info, context);
}
static bool art_sigbus_handler(int sig, siginfo_t* info, void* context) {
  return fault_manager.HandleSigbusFault(sig, info, context);
}
FaultManager::FaultManager()
    : generated_code_ranges_lock_("FaultHandler generated code ranges lock",
                                  LockLevel::kGenericBottomLock),
      initialized_(false) {}
FaultManager::~FaultManager() {}
static const char* SignalCodeName(int sig, int code) {
  if (sig == SIGSEGV) {
    switch (code) {
      case SEGV_MAPERR:
        return "SEGV_MAPERR";
      case SEGV_ACCERR:
        return "SEGV_ACCERR";
      case 8:
        return "SEGV_MTEAERR";
      case 9:
        return "SEGV_MTESERR";
      default:
        return "SEGV_UNKNOWN";
    }
  } else if (sig == SIGBUS) {
    switch (code) {
      case BUS_ADRALN:
        return "BUS_ADRALN";
      case BUS_ADRERR:
        return "BUS_ADRERR";
      case BUS_OBJERR:
        return "BUS_OBJERR";
      default:
        return "BUS_UNKNOWN";
    }
  } else {
    return "UNKNOWN";
  }
}
static std::ostream& PrintSignalInfo(std::ostream& os, siginfo_t* info) {
  os << "  si_signo: " << info->si_signo << " (" << strsignal(info->si_signo) << ")\n"
     << "  si_code: " << info->si_code << " (" << SignalCodeName(info->si_signo, info->si_code)
     << ")";
  if (info->si_signo == SIGSEGV || info->si_signo == SIGBUS) {
    os << "\n"
       << "  si_addr: " << info->si_addr;
  }
  return os;
}
static bool InstallSigbusHandler() {
  return gUseUserfaultfd &&
         Runtime::Current()->GetHeap()->MarkCompactCollector()->IsUsingSigbusFeature();
}
void FaultManager::Init(bool use_sig_chain) {
  CHECK(!initialized_);
  if (use_sig_chain) {
    sigset_t mask;
    sigfillset(&mask);
    sigdelset(&mask, SIGABRT);
    sigdelset(&mask, SIGBUS);
    sigdelset(&mask, SIGFPE);
    sigdelset(&mask, SIGILL);
    sigdelset(&mask, SIGSEGV);
    SigchainAction sa = {
        .sc_sigaction = art_sigsegv_handler,
        .sc_mask = mask,
        .sc_flags = 0UL,
    };
    AddSpecialSignalHandlerFn(SIGSEGV, &sa);
    if (InstallSigbusHandler()) {
      sa.sc_sigaction = art_sigbus_handler;
      AddSpecialSignalHandlerFn(SIGBUS, &sa);
    }
    initialized_ = true;
  } else if (InstallSigbusHandler()) {
    struct sigaction act;
    std::memset(&act, '\0', sizeof(act));
    act.sa_flags = SA_SIGINFO | SA_RESTART;
    act.sa_sigaction = [](int sig, siginfo_t* info, void* context) {
      if (!art_sigbus_handler(sig, info, context)) {
        std::ostringstream oss;
        PrintSignalInfo(oss, info);
        LOG(FATAL) << "Couldn't handle SIGBUS fault:"
                   << "\n"
                   << oss.str();
      }
    };
    if (sigaction(SIGBUS, &act, nullptr)) {
      LOG(FATAL) << "Fault handler for SIGBUS couldn't be setup: " << strerror(errno);
    }
  }
}
void FaultManager::Release() {
  if (initialized_) {
    RemoveSpecialSignalHandlerFn(SIGSEGV, art_sigsegv_handler);
    if (InstallSigbusHandler()) {
      RemoveSpecialSignalHandlerFn(SIGBUS, art_sigbus_handler);
    }
    initialized_ = false;
  }
}
void FaultManager::Shutdown() {
  if (initialized_) {
    Release();
    STLDeleteElements(&generated_code_handlers_);
    STLDeleteElements(&other_handlers_);
    MutexLock lock(Thread::Current(), generated_code_ranges_lock_);
    GeneratedCodeRange* range = generated_code_ranges_.load(std::memory_order_acquire);
    generated_code_ranges_.store(nullptr, std::memory_order_release);
    while (range != nullptr) {
      GeneratedCodeRange* next_range = range->next.load(std::memory_order_relaxed);
      std::less<GeneratedCodeRange*> less;
      if (!less(range, generated_code_ranges_storage_) &&
          less(range, generated_code_ranges_storage_ + kNumLocalGeneratedCodeRanges)) {
      } else {
        delete range;
      }
      range = next_range;
    }
  }
}
bool FaultManager::HandleFaultByOtherHandlers(int sig, siginfo_t* info, void* context) {
  if (other_handlers_.empty()) {
    return false;
  }
  Thread* self = Thread::Current();
  DCHECK(self != nullptr);
  DCHECK(Runtime::Current() != nullptr);
  DCHECK(Runtime::Current()->IsStarted());
  for (const auto& handler : other_handlers_) {
    if (handler->Action(sig, info, context)) {
      return true;
    }
  }
  return false;
}
bool FaultManager::HandleSigbusFault(int sig, siginfo_t* info, void* context ATTRIBUTE_UNUSED) {
  DCHECK_EQ(sig, SIGBUS);
  if (VLOG_IS_ON(signals)) {
    PrintSignalInfo(VLOG_STREAM(signals) << "Handling SIGBUS fault:\n", info);
  }
#ifdef TEST_NESTED_SIGNAL
  raise(SIGBUS);
#endif
  return Runtime::Current()->GetHeap()->MarkCompactCollector()->SigbusHandler(info);
}
bool FaultManager::HandleFault(int sig, siginfo_t* info, void* context) {
  if (VLOG_IS_ON(signals)) {
    PrintSignalInfo(VLOG_STREAM(signals) << "Handling SIGSEGV fault:\n", info);
  }
#ifdef TEST_NESTED_SIGNAL
  raise(SIGSEGV);
#endif
  if (IsInGeneratedCode(info, context)) {
    VLOG(signals) << "in generated code, looking for handler";
    for (const auto& handler : generated_code_handlers_) {
      VLOG(signals) << "invoking Action on handler " << handler;
      if (handler->Action(sig, info, context)) {
        return true;
      }
    }
  }
  if (HandleFaultByOtherHandlers(sig, info, context)) {
    return true;
  }
  art_sigsegv_fault();
  return false;
}
void FaultManager::AddHandler(FaultHandler* handler, bool generated_code) {
  DCHECK(initialized_);
  if (generated_code) {
    generated_code_handlers_.push_back(handler);
  } else {
    other_handlers_.push_back(handler);
  }
}
void FaultManager::RemoveHandler(FaultHandler* handler) {
  auto it = std::find(generated_code_handlers_.begin(), generated_code_handlers_.end(), handler);
  if (it != generated_code_handlers_.end()) {
    generated_code_handlers_.erase(it);
    return;
  }
  auto it2 = std::find(other_handlers_.begin(), other_handlers_.end(), handler);
  if (it2 != other_handlers_.end()) {
    other_handlers_.erase(it2);
    return;
  }
  LOG(FATAL) << "Attempted to remove non existent handler " << handler;
}
inline FaultManager::GeneratedCodeRange* FaultManager::CreateGeneratedCodeRange(const void* start,
                                                                                size_t size) {
  GeneratedCodeRange* range = free_generated_code_ranges_;
  if (range != nullptr) {
    std::less<GeneratedCodeRange*> less;
    DCHECK(!less(range, generated_code_ranges_storage_));
    DCHECK(less(range, generated_code_ranges_storage_ + kNumLocalGeneratedCodeRanges));
    range->start = start;
    range->size = size;
    free_generated_code_ranges_ = range->next.load(std::memory_order_relaxed);
    range->next.store(nullptr, std::memory_order_relaxed);
    return range;
  } else {
    return new GeneratedCodeRange{nullptr, start, size};
  }
}
inline void FaultManager::FreeGeneratedCodeRange(GeneratedCodeRange* range) {
  std::less<GeneratedCodeRange*> less;
  if (!less(range, generated_code_ranges_storage_) &&
      less(range, generated_code_ranges_storage_ + kNumLocalGeneratedCodeRanges)) {
    MutexLock lock(Thread::Current(), generated_code_ranges_lock_);
    range->start = nullptr;
    range->size = 0u;
    range->next.store(free_generated_code_ranges_, std::memory_order_relaxed);
    free_generated_code_ranges_ = range;
  } else {
    delete range;
  }
}
void FaultManager::AddGeneratedCodeRange(const void* start, size_t size) {
  GeneratedCodeRange* new_range = nullptr;
  {
    MutexLock lock(Thread::Current(), generated_code_ranges_lock_);
    new_range = CreateGeneratedCodeRange(start, size);
    GeneratedCodeRange* old_head = generated_code_ranges_.load(std::memory_order_relaxed);
    new_range->next.store(old_head, std::memory_order_relaxed);
    generated_code_ranges_.store(new_range, std::memory_order_release);
  }
  art::membarrier(MembarrierCommand::kPrivateExpedited);
}
void FaultManager::RemoveGeneratedCodeRange(const void* start, size_t size) {
  Thread* self = Thread::Current();
  GeneratedCodeRange* range = nullptr;
  {
    MutexLock lock(self, generated_code_ranges_lock_);
    std::atomic<GeneratedCodeRange*>* before = &generated_code_ranges_;
    range = before->load(std::memory_order_relaxed);
    while (range != nullptr && range->start != start) {
      before = &range->next;
      range = before->load(std::memory_order_relaxed);
    }
    if (range != nullptr) {
      GeneratedCodeRange* next = range->next.load(std::memory_order_relaxed);
      if (before == &generated_code_ranges_) {
        before->store(next, std::memory_order_release);
      } else {
        before->store(next, std::memory_order_relaxed);
      }
    }
  }
  CHECK(range != nullptr);
  DCHECK_EQ(range->start, start);
  CHECK_EQ(range->size, size);
  Runtime* runtime = Runtime::Current();
  CHECK(runtime != nullptr);
  if (runtime->IsStarted() && runtime->GetThreadList() != nullptr) {
    if (Locks::mutator_lock_->IsExclusiveHeld(self)) {
    } else {
      DCHECK(Locks::mutator_lock_->IsSharedHeld(self));
      bool runnable = (self->GetState() == ThreadState::kRunnable);
      if (runnable) {
        self->TransitionFromRunnableToSuspended(ThreadState::kNative);
      } else {
        Locks::mutator_lock_->SharedUnlock(self);
      }
      DCHECK(!Locks::mutator_lock_->IsSharedHeld(self));
      runtime->GetThreadList()->RunEmptyCheckpoint();
      if (runnable) {
        self->TransitionFromSuspendedToRunnable();
      } else {
        Locks::mutator_lock_->SharedLock(self);
      }
    }
  }
  FreeGeneratedCodeRange(range);
}
bool FaultManager::IsInGeneratedCode(siginfo_t* siginfo, void* context) {
  VLOG(signals) << "Checking for generated code";
  Thread* thread = Thread::Current();
  if (thread == nullptr) {
    VLOG(signals) << "no current thread";
    return false;
  }
  ThreadState state = thread->GetState();
  if (state != ThreadState::kRunnable) {
    VLOG(signals) << "not runnable";
    return false;
  }
  if (!Locks::mutator_lock_->IsSharedHeld(thread)) {
    VLOG(signals) << "no lock";
    return false;
  }
  uintptr_t fault_pc = GetFaultPc(siginfo, context);
  if (fault_pc == 0u) {
    VLOG(signals) << "no fault PC";
    return false;
  }
  GeneratedCodeRange* range = generated_code_ranges_.load(std::memory_order_acquire);
  while (range != nullptr) {
    if (fault_pc - reinterpret_cast<uintptr_t>(range->start) < range->size) {
      return true;
    }
    range = range->next.load(std::memory_order_relaxed);
  }
  return false;
}
FaultHandler::FaultHandler(FaultManager* manager) : manager_(manager) {}
NullPointerHandler::NullPointerHandler(FaultManager* manager) : FaultHandler(manager) {
  manager_->AddHandler(this, true);
}
bool NullPointerHandler::IsValidMethod(ArtMethod* method) {
  VLOG(signals) << "potential method: " << method;
  static_assert(IsAligned<sizeof(void*)>(ArtMethod::Size(kRuntimePointerSize)));
  if (method == nullptr || !IsAligned<sizeof(void*)>(method)) {
    VLOG(signals) << ((method == nullptr) ? "null method" : "unaligned method");
    return false;
  }
  mirror::Object* klass = method->GetDeclaringClassAddressWithoutBarrier()->AsMirrorPtr();
  if (klass == nullptr || !IsAligned<kObjectAlignment>(klass)) {
    VLOG(signals) << ((klass == nullptr) ? "null class" : "unaligned class");
    return false;
  }
  mirror::Class* class_class = klass->GetClass<kVerifyNone, kWithoutReadBarrier>();
  if (class_class == nullptr || !IsAligned<kObjectAlignment>(class_class)) {
    VLOG(signals) << ((klass == nullptr) ? "null class_class" : "unaligned class_class");
    return false;
  }
  if (class_class != class_class->GetClass<kVerifyNone, kWithoutReadBarrier>()) {
    VLOG(signals) << "invalid class_class";
    return false;
  }
  return true;
}
bool NullPointerHandler::IsValidReturnPc(ArtMethod** sp, uintptr_t return_pc) {
  ArtMethod* method = *sp;
  const OatQuickMethodHeader* method_header = method->GetOatQuickMethodHeader(return_pc);
  if (method_header == nullptr) {
    VLOG(signals) << "No method header.";
    return false;
  }
  VLOG(signals) << "looking for dex pc for return pc 0x" << std::hex << return_pc
                << " pc offset: 0x" << std::hex
                << (return_pc - reinterpret_cast<uintptr_t>(method_header->GetEntryPoint()));
  uint32_t dexpc = method_header->ToDexPc(reinterpret_cast<ArtMethod**>(sp), return_pc, false);
  VLOG(signals) << "dexpc: " << dexpc;
  return dexpc != dex::kDexNoIndex;
}
SuspensionHandler::SuspensionHandler(FaultManager* manager) : FaultHandler(manager) {
  manager_->AddHandler(this, true);
}
StackOverflowHandler::StackOverflowHandler(FaultManager* manager) : FaultHandler(manager) {
  manager_->AddHandler(this, true);
}
JavaStackTraceHandler::JavaStackTraceHandler(FaultManager* manager) : FaultHandler(manager) {
  manager_->AddHandler(this, false);
}
bool JavaStackTraceHandler::Action(int sig ATTRIBUTE_UNUSED, siginfo_t* siginfo, void* context) {
  bool in_generated_code = manager_->IsInGeneratedCode(siginfo, context);
  if (in_generated_code) {
    LOG(ERROR) << "Dumping java stack trace for crash in generated code";
    Thread* self = Thread::Current();
    uintptr_t sp = FaultManager::GetFaultSp(context);
    CHECK_NE(sp, 0u);
    self->SetTopOfStack(reinterpret_cast<ArtMethod**>(sp));
    self->DumpJavaStack(LOG_STREAM(ERROR));
  }
  return false;
}
}
