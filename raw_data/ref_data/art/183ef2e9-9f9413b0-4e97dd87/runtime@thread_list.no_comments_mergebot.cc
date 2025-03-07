#include "thread_list.h"
#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>
#include <map>
#include <sstream>
#include <tuple>
#include <vector>
#include "android-base/stringprintf.h"
#include "backtrace/BacktraceMap.h"
#include "nativehelper/scoped_local_ref.h"
#include "nativehelper/scoped_utf_chars.h"
#include "unwindstack/AndroidUnwinder.h"
#include "art_field-inl.h"
#include "base/aborting.h"
#include "base/histogram-inl.h"
#include "base/mutex-inl.h"
#include "base/systrace.h"
#include "base/time_utils.h"
#include "base/timing_logger.h"
#include "debugger.h"
#include "gc/collector/concurrent_copying.h"
#include "gc/gc_pause_listener.h"
#include "gc/heap.h"
#include "gc/reference_processor.h"
#include "gc_root.h"
#include "jni/jni_internal.h"
#include "lock_word.h"
#include "mirror/string.h"
#include "monitor.h"
#include "native_stack_dump.h"
#include "obj_ptr-inl.h"
#include "scoped_thread_state_change-inl.h"
#include "thread.h"
#include "trace.h"
#include "well_known_classes.h"
#if ART_USE_FUTEXES
#include "linux/futex.h"
#include "sys/syscall.h"
#ifndef SYS_futex
#define SYS_futex __NR_futex
#endif
#endif
namespace art {
using android::base::StringPrintf;
static constexpr uint64_t kLongThreadSuspendThreshold = MsToNs(5);
static constexpr useconds_t kThreadSuspendInitialSleepUs = 0;
static constexpr useconds_t kThreadSuspendMaxYieldUs = 3000;
static constexpr useconds_t kThreadSuspendMaxSleepUs = 5000;
static constexpr bool kDumpUnattachedThreadNativeStackForSigQuit = true;
ThreadList::ThreadList(uint64_t thread_suspend_timeout_ns)
    : suspend_all_count_(0),
      unregistering_count_(0),
      suspend_all_historam_("suspend all histogram", 16, 64),
      long_suspend_(false),
      shut_down_(false),
      thread_suspend_timeout_ns_(thread_suspend_timeout_ns),
      empty_checkpoint_barrier_(new Barrier(0)) {
  CHECK(Monitor::IsValidLockWord(LockWord::FromThinLockId(kMaxThreadId, 1, 0U)));
}
ThreadList::~ThreadList() {
  CHECK(shut_down_);
}
void ThreadList::ShutDown() {
  ScopedTrace trace(__PRETTY_FUNCTION__);
  bool contains = false;
  Thread* self = Thread::Current();
  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    contains = Contains(self);
  }
  if (contains) {
    Runtime::Current()->DetachCurrentThread();
  }
  WaitForOtherNonDaemonThreadsToExit();
  gc::Heap* const heap = Runtime::Current()->GetHeap();
  CHECK(heap->IsGCDisabledForShutdown());
  SuspendAllDaemonThreadsForShutdown();
  shut_down_ = true;
}
bool ThreadList::Contains(Thread* thread) {
  return find(list_.begin(), list_.end(), thread) != list_.end();
}
pid_t ThreadList::GetLockOwner() {
  return Locks::thread_list_lock_->GetExclusiveOwnerTid();
}
void ThreadList::DumpNativeStacks(std::ostream& os) {
  MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
  unwindstack::AndroidLocalUnwinder unwinder;
  for (const auto& thread : list_) {
    os << "DUMPING THREAD " << thread->GetTid() << "\n";
    DumpNativeStack(os, unwinder, thread->GetTid(), "\t");
    os << "\n";
  }
}
void ThreadList::DumpForSigQuit(std::ostream& os) {
  {
    ScopedObjectAccess soa(Thread::Current());
    if (suspend_all_historam_.SampleSize() > 0) {
      Histogram<uint64_t>::CumulativeData data;
      suspend_all_historam_.CreateHistogram(&data);
      suspend_all_historam_.PrintConfidenceIntervals(os, 0.99, data);
    }
  }
  bool dump_native_stack = Runtime::Current()->GetDumpNativeStackOnSigQuit();
  Dump(os, dump_native_stack);
  DumpUnattachedThreads(os, dump_native_stack && kDumpUnattachedThreadNativeStackForSigQuit);
}
static void DumpUnattachedThread(std::ostream& os, pid_t tid, bool dump_native_stack)
    NO_THREAD_SAFETY_ANALYSIS {
  Thread::DumpState(os, nullptr, tid);
  if (dump_native_stack) {
    DumpNativeStack(os, tid, "  native: ");
  }
  os << std::endl;
}
void ThreadList::DumpUnattachedThreads(std::ostream& os, bool dump_native_stack) {
  DIR* d = opendir("/proc/self/task");
  if (!d) {
    return;
  }
  Thread* self = Thread::Current();
  dirent* e;
  while ((e = readdir(d)) != nullptr) {
    char* end;
    pid_t tid = strtol(e->d_name, &end, 10);
    if (!*end) {
      Thread* thread;
      {
        MutexLock mu(self, *Locks::thread_list_lock_);
        thread = FindThreadByTid(tid);
      }
      if (thread == nullptr) {
        DumpUnattachedThread(os, tid, dump_native_stack);
      }
    }
  }
  closedir(d);
}
void ThreadList::DumpUnattachedThreads(std::ostream& os, bool dump_native_stack) {
  DIR* d = opendir("/proc/self/task");
  if (!d) {
    return;
  }
  Thread* self = Thread::Current();
  dirent* e;
  while ((e = readdir(d)) != nullptr) {
    char* end;
    pid_t tid = strtol(e->d_name, &end, 10);
    if (!*end) {
      Thread* thread;
      {
        MutexLock mu(self, *Locks::thread_list_lock_);
        thread = FindThreadByTid(tid);
      }
      if (thread == nullptr) {
        DumpUnattachedThread(os, tid, dump_native_stack);
      }
    }
  }
  closedir(d);
}
static constexpr uint32_t kDumpWaitTimeout = kIsTargetBuild ? 100000 : 20000;
class DumpCheckpoint final : public Closure {
private:
  Mutex lock_;
  std::multimap<std::pair<Thread::DumpOrder, uint32_t>, std::ostringstream> os_ GUARDED_BY(lock_);
public:
  DumpCheckpoint(bool dump_native_stack): lock_("Dump checkpoint lock", kGenericBottomLock), os_(),
        barrier_(0, false), unwinder_(std::vector<std::string> {
  }
  void Run(Thread* thread) override {
    Thread* self = Thread::Current();
    CHECK(self != nullptr);
    std::ostringstream local_os;
    Thread::DumpOrder dump_order;
    {
      ScopedObjectAccess soa(self);
      dump_order = thread->Dump(local_os, unwinder_, dump_native_stack_);
    }
    {
      MutexLock mu(self, lock_);
      std::pair<Thread::DumpOrder, uint32_t> sort_key(dump_order, thread->GetThreadId());
      os_.emplace(sort_key, std::move(local_os));
    }
    barrier_.Pass(self);
  }
  void Dump(Thread* self, std::ostream& os) {
    MutexLock mu(self, lock_);
    for (const auto& it : os_) {
      os << it.second.str() << std::endl;
    }
  }
  void WaitForThreadsToRunThroughCheckpoint(size_t threads_running_checkpoint) {
    Thread* self = Thread::Current();
    ScopedThreadStateChange tsc(self, ThreadState::kWaitingForCheckPointsToRun);
    bool timed_out = barrier_.Increment(self, threads_running_checkpoint, kDumpWaitTimeout);
    if (timed_out) {
      LOG((kIsDebugBuild && (gAborting == 0)) ? ::android::base::FATAL : ::android::base::ERROR)
          << "Unexpected time out during dump checkpoint.";
    }
  }
private:
  Barrier barrier_;
  unwindstack::AndroidLocalUnwinder unwinder_;
  const bool dump_native_stack_;
};
void ThreadList::Dump(std::ostream& os, bool dump_native_stack) {
  Thread* self = Thread::Current();
  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    os << "DALVIK THREADS (" << list_.size() << "):\n";
  }
  if (self != nullptr) {
    DumpCheckpoint checkpoint(dump_native_stack);
    size_t threads_running_checkpoint;
    {
      ScopedObjectAccess soa(self);
      threads_running_checkpoint = RunCheckpoint(&checkpoint);
    }
    if (threads_running_checkpoint != 0) {
      checkpoint.WaitForThreadsToRunThroughCheckpoint(threads_running_checkpoint);
    }
    checkpoint.Dump(self, os);
  } else {
    DumpUnattachedThreads(os, dump_native_stack);
  }
}
void ThreadList::AssertThreadsAreSuspended(Thread* self, Thread* ignore1, Thread* ignore2) {
  MutexLock mu(self, *Locks::thread_list_lock_);
  MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
  for (const auto& thread : list_) {
    if (thread != ignore1 && thread != ignore2) {
      CHECK(thread->IsSuspended())
            << "\nUnsuspended thread: <<" << *thread << "\n"
            << "self: <<" << *Thread::Current();
    }
  }
}
static void ThreadSuspendSleep(useconds_t delay_us) {
  if (delay_us == 0) {
    sched_yield();
  } else {
    usleep(delay_us);
  }
}
size_t ThreadList::RunCheckpoint(Closure* checkpoint_function, Closure* callback) {
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertNotExclusiveHeld(self);
  Locks::thread_list_lock_->AssertNotHeld(self);
  Locks::thread_suspend_count_lock_->AssertNotHeld(self);
  std::vector<Thread*> suspended_count_modified_threads;
  size_t count = 0;
  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    count = list_.size();
    for (const auto& thread : list_) {
      if (thread != self) {
        bool requested_suspend = false;
        while (true) {
          if (thread->RequestCheckpoint(checkpoint_function)) {
            if (requested_suspend) {
              bool updated =
                  thread->ModifySuspendCount(self, -1, nullptr, SuspendReason::kInternal);
              DCHECK(updated);
              requested_suspend = false;
            }
            break;
          } else {
            if (thread->GetState() == ThreadState::kRunnable) {
              continue;
            }
            if (!requested_suspend) {
              bool updated =
                  thread->ModifySuspendCount(self, +1, nullptr, SuspendReason::kInternal);
              DCHECK(updated);
              requested_suspend = true;
              if (thread->IsSuspended()) {
                break;
              }
            } else {
              DCHECK(thread->IsSuspended());
              break;
            }
          }
        }
        if (requested_suspend) {
          suspended_count_modified_threads.push_back(thread);
        }
      }
    }
    if (callback != nullptr) {
      callback->Run(self);
    }
  }
  checkpoint_function->Run(self);
  for (const auto& thread : suspended_count_modified_threads) {
    DCHECK(thread->IsSuspended());
    checkpoint_function->Run(thread);
    {
      MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
      bool updated = thread->ModifySuspendCount(self, -1, nullptr, SuspendReason::kInternal);
      DCHECK(updated);
    }
  }
  {
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    Thread::resume_cond_->Broadcast(self);
  }
  return count;
}
void ThreadList::RunEmptyCheckpoint() {
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertNotExclusiveHeld(self);
  Locks::thread_list_lock_->AssertNotHeld(self);
  Locks::thread_suspend_count_lock_->AssertNotHeld(self);
  std::vector<uint32_t> runnable_thread_ids;
  size_t count = 0;
  Barrier* barrier = empty_checkpoint_barrier_.get();
  barrier->Init(self, 0);
  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    for (Thread* thread : list_) {
      if (thread != self) {
        while (true) {
          if (thread->RequestEmptyCheckpoint()) {
            ++count;
            if (kIsDebugBuild) {
              runnable_thread_ids.push_back(thread->GetThreadId());
            }
            break;
          }
          if (thread->GetState() != ThreadState::kRunnable) {
            break;
          }
        }
      }
    }
  }
  Runtime::Current()->GetHeap()->GetReferenceProcessor()->BroadcastForSlowPath(self);
  Runtime::Current()->BroadcastForNewSystemWeaks( true);
  {
    ScopedThreadStateChange tsc(self, ThreadState::kWaitingForCheckPointsToRun);
    uint64_t total_wait_time = 0;
    bool first_iter = true;
    while (true) {
      for (BaseMutex* mutex : Locks::expected_mutexes_on_weak_ref_access_) {
        mutex->WakeupToRespondToEmptyCheckpoint();
      }
      static constexpr uint64_t kEmptyCheckpointPeriodicTimeoutMs = 100;
      static constexpr uint64_t kEmptyCheckpointTotalTimeoutMs = 600 * 1000;
      size_t barrier_count = first_iter ? count : 0;
      first_iter = false;
      bool timed_out = barrier->Increment(self, barrier_count, kEmptyCheckpointPeriodicTimeoutMs);
      if (!timed_out) {
        break;
      }
      total_wait_time += kEmptyCheckpointPeriodicTimeoutMs;
      if (kIsDebugBuild && total_wait_time > kEmptyCheckpointTotalTimeoutMs) {
        std::ostringstream ss;
        ss << "Empty checkpoint timeout\n";
        ss << "Barrier count " << barrier->GetCount(self) << "\n";
        ss << "Runnable thread IDs";
        for (uint32_t tid : runnable_thread_ids) {
          ss << " " << tid;
        }
        ss << "\n";
        Locks::mutator_lock_->Dump(ss);
        ss << "\n";
        LOG(FATAL_WITHOUT_ABORT) << ss.str();
        {
          ScopedObjectAccess soa(self);
          MutexLock mu1(self, *Locks::thread_list_lock_);
          for (Thread* thread : GetList()) {
            uint32_t tid = thread->GetThreadId();
            bool is_in_runnable_thread_ids =
                std::find(runnable_thread_ids.begin(), runnable_thread_ids.end(), tid) !=
                runnable_thread_ids.end();
            if (is_in_runnable_thread_ids &&
                thread->ReadFlag(ThreadFlag::kEmptyCheckpointRequest)) {
              thread->Dump(LOG_STREAM(FATAL_WITHOUT_ABORT),
                                                  true,
                                                 true);
            }
          }
        }
        LOG(FATAL_WITHOUT_ABORT)
            << "Dumped runnable threads that haven't responded to empty checkpoint.";
        Dump(LOG_STREAM(FATAL_WITHOUT_ABORT));
        LOG(FATAL) << "Dumped all threads.";
      }
    }
  }
}
size_t ThreadList::FlipThreadRoots(Closure* thread_flip_visitor,
                                   Closure* flip_callback,
                                   gc::collector::GarbageCollector* collector,
                                   gc::GcPauseListener* pause_listener) {
  TimingLogger::ScopedTiming split("ThreadListFlip", collector->GetTimings());
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertNotHeld(self);
  Locks::thread_list_lock_->AssertNotHeld(self);
  Locks::thread_suspend_count_lock_->AssertNotHeld(self);
  CHECK_NE(self->GetState(), ThreadState::kRunnable);
  size_t runnable_thread_count = 0;
  std::vector<Thread*> other_threads;
  collector->GetHeap()->ThreadFlipBegin(self);
  const uint64_t suspend_start_time = NanoTime();
  SuspendAllInternal(self, self, nullptr);
  if (pause_listener != nullptr) {
    pause_listener->StartPause();
  }
  Locks::mutator_lock_->ExclusiveLock(self);
  suspend_all_historam_.AdjustAndAddValue(NanoTime() - suspend_start_time);
  flip_callback->Run(self);
<<<<<<< HEAD
|||||||
  Locks::mutator_lock_->ExclusiveUnlock(self);
  collector->RegisterPause(NanoTime() - suspend_start_time);
  if (pause_listener != nullptr) {
    pause_listener->EndPause();
  }
=======
  auto fake_mutator_lock_acquire = []() ACQUIRE(*Locks::mutator_lock_) NO_THREAD_SAFETY_ANALYSIS {};
  auto fake_mutator_lock_release = []() RELEASE(*Locks::mutator_lock_) NO_THREAD_SAFETY_ANALYSIS {};
  if (!gUseUserfaultfd) {
    Locks::mutator_lock_->ExclusiveUnlock(self);
    fake_mutator_lock_acquire();
  }
>>>>>>> 9f9413b04b404f1eb0cdd283ccc9685b854aa906
  {
    TimingLogger::ScopedTiming split2("ResumeRunnableThreads", collector->GetTimings());
    MutexLock mu(self, *Locks::thread_list_lock_);
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    --suspend_all_count_;
    for (Thread* thread : list_) {
      thread->SetFlipFunction(thread_flip_visitor);
      if (thread == self) {
        continue;
      }
      ThreadState state = thread->GetState();
      if ((state == ThreadState::kWaitingForGcThreadFlip || thread->IsTransitioningToRunnable()) &&
          thread->GetSuspendCount() == 1) {
        bool updated = thread->ModifySuspendCount(self, -1, nullptr, SuspendReason::kInternal);
        DCHECK(updated);
        ++runnable_thread_count;
      } else {
        other_threads.push_back(thread);
      }
    }
    Thread::resume_cond_->Broadcast(self);
  }
  collector->RegisterPause(NanoTime() - suspend_start_time);
  if (pause_listener != nullptr) {
    pause_listener->EndPause();
  }
  collector->GetHeap()->ThreadFlipEnd(self);
  {
    TimingLogger::ScopedTiming split3("FlipOtherThreads", collector->GetTimings());
<<<<<<< HEAD
|||||||
    ReaderMutexLock mu(self, *Locks::mutator_lock_);
=======
    if (gUseUserfaultfd) {
      Locks::mutator_lock_->AssertExclusiveHeld(self);
      for (Thread* thread : other_threads) {
        thread->EnsureFlipFunctionStarted(self);
        DCHECK(!thread->ReadFlag(ThreadFlag::kPendingFlipFunction));
      }
      self->EnsureFlipFunctionStarted(self);
      DCHECK(!self->ReadFlag(ThreadFlag::kPendingFlipFunction));
      Locks::mutator_lock_->ExclusiveUnlock(self);
    } else {
      fake_mutator_lock_release();
      ReaderMutexLock mu(self, *Locks::mutator_lock_);
>>>>>>> 9f9413b04b404f1eb0cdd283ccc9685b854aa906
    for (Thread* thread : other_threads) {
      thread->EnsureFlipFunctionStarted(self);
      DCHECK(!thread->ReadFlag(ThreadFlag::kPendingFlipFunction));
    }
    self->EnsureFlipFunctionStarted(self);
    DCHECK(!self->ReadFlag(ThreadFlag::kPendingFlipFunction));
  }
  }
  Locks::mutator_lock_->ExclusiveUnlock(self);
  {
    TimingLogger::ScopedTiming split4("ResumeOtherThreads", collector->GetTimings());
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    for (const auto& thread : other_threads) {
      bool updated = thread->ModifySuspendCount(self, -1, nullptr, SuspendReason::kInternal);
      DCHECK(updated);
    }
    Thread::resume_cond_->Broadcast(self);
  }
  return runnable_thread_count + other_threads.size() + 1;
}
void ThreadList::SuspendAll(const char* cause, bool long_suspend) {
  Thread* self = Thread::Current();
  if (self != nullptr) {
    VLOG(threads) << *self << " SuspendAll for " << cause << " starting...";
  } else {
    VLOG(threads) << "Thread[null] SuspendAll for " << cause << " starting...";
  }
  {
    ScopedTrace trace("Suspending mutator threads");
    const uint64_t start_time = NanoTime();
    SuspendAllInternal(self, self);
#if HAVE_TIMED_RWLOCK
    while (true) {
      if (Locks::mutator_lock_->ExclusiveLockWithTimeout(self,
                                                         NsToMs(thread_suspend_timeout_ns_),
                                                         0)) {
        break;
      } else if (!long_suspend_) {
        UnsafeLogFatalForThreadSuspendAllTimeout();
      }
    }
#else
    Locks::mutator_lock_->ExclusiveLock(self);
#endif
    long_suspend_ = long_suspend;
    const uint64_t end_time = NanoTime();
    const uint64_t suspend_time = end_time - start_time;
    suspend_all_historam_.AdjustAndAddValue(suspend_time);
    if (suspend_time > kLongThreadSuspendThreshold) {
      LOG(WARNING) << "Suspending all threads took: " << PrettyDuration(suspend_time);
    }
    if (kDebugLocking) {
      AssertThreadsAreSuspended(self, self);
    }
  }
  ATraceBegin((std::string("Mutator threads suspended for ") + cause).c_str());
  if (self != nullptr) {
    VLOG(threads) << *self << " SuspendAll complete";
  } else {
    VLOG(threads) << "Thread[null] SuspendAll complete";
  }
}
void ThreadList::SuspendAllInternal(Thread* self,
                                    Thread* ignore1,
                                    Thread* ignore2,
                                    SuspendReason reason) {
  Locks::mutator_lock_->AssertNotExclusiveHeld(self);
  Locks::thread_list_lock_->AssertNotHeld(self);
  Locks::thread_suspend_count_lock_->AssertNotHeld(self);
  if (kDebugLocking && self != nullptr) {
    CHECK_NE(self->GetState(), ThreadState::kRunnable);
  }
  AtomicInteger pending_threads;
  uint32_t num_ignored = 0;
  if (ignore1 != nullptr) {
    ++num_ignored;
  }
  if (ignore2 != nullptr && ignore1 != ignore2) {
    ++num_ignored;
  }
  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    ++suspend_all_count_;
    pending_threads.store(list_.size() - num_ignored, std::memory_order_relaxed);
    for (const auto& thread : list_) {
      if (thread == ignore1 || thread == ignore2) {
        continue;
      }
      VLOG(threads) << "requesting thread suspend: " << *thread;
      bool updated = thread->ModifySuspendCount(self, +1, &pending_threads, reason);
      DCHECK(updated);
      if (thread->IsSuspended()) {
        thread->ClearSuspendBarrier(&pending_threads);
        pending_threads.fetch_sub(1, std::memory_order_seq_cst);
      }
    }
  }
#if ART_USE_FUTEXES
  timespec wait_timeout;
  InitTimeSpec(false, CLOCK_MONOTONIC, NsToMs(thread_suspend_timeout_ns_), 0, &wait_timeout);
#endif
  const uint64_t start_time = NanoTime();
  while (true) {
    int32_t cur_val = pending_threads.load(std::memory_order_relaxed);
    if (LIKELY(cur_val > 0)) {
#if ART_USE_FUTEXES
      if (futex(pending_threads.Address(), FUTEX_WAIT_PRIVATE, cur_val, &wait_timeout, nullptr, 0)
          != 0) {
        if ((errno == EAGAIN) || (errno == EINTR)) {
          continue;
        }
        if (errno == ETIMEDOUT) {
          const uint64_t wait_time = NanoTime() - start_time;
          MutexLock mu(self, *Locks::thread_list_lock_);
          MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
          std::ostringstream oss;
          for (const auto& thread : list_) {
            if (thread == ignore1 || thread == ignore2) {
              continue;
            }
            if (!thread->IsSuspended()) {
              oss << std::endl << "Thread not suspended: " << *thread;
            }
          }
          LOG(kIsDebugBuild ? ::android::base::FATAL : ::android::base::ERROR)
              << "Timed out waiting for threads to suspend, waited for "
              << PrettyDuration(wait_time)
              << oss.str();
        } else {
          PLOG(FATAL) << "futex wait failed for SuspendAllInternal()";
        }
      }
#else
      UNUSED(start_time);
#endif
    } else {
      CHECK_EQ(cur_val, 0);
      break;
    }
  }
}
void ThreadList::ResumeAll() {
  Thread* self = Thread::Current();
  if (self != nullptr) {
    VLOG(threads) << *self << " ResumeAll starting";
  } else {
    VLOG(threads) << "Thread[null] ResumeAll starting";
  }
  ATraceEnd();
  ScopedTrace trace("Resuming mutator threads");
  if (kDebugLocking) {
    AssertThreadsAreSuspended(self, self);
  }
  long_suspend_ = false;
  Locks::mutator_lock_->ExclusiveUnlock(self);
  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    --suspend_all_count_;
    for (const auto& thread : list_) {
      if (thread == self) {
        continue;
      }
      bool updated = thread->ModifySuspendCount(self, -1, nullptr, SuspendReason::kInternal);
      DCHECK(updated);
    }
    if (self != nullptr) {
      VLOG(threads) << *self << " ResumeAll waking others";
    } else {
      VLOG(threads) << "Thread[null] ResumeAll waking others";
    }
    Thread::resume_cond_->Broadcast(self);
  }
  if (self != nullptr) {
    VLOG(threads) << *self << " ResumeAll complete";
  } else {
    VLOG(threads) << "Thread[null] ResumeAll complete";
  }
}
bool ThreadList::Resume(Thread* thread, SuspendReason reason) {
  ATraceEnd();
  Thread* self = Thread::Current();
  DCHECK_NE(thread, self);
  VLOG(threads) << "Resume(" << reinterpret_cast<void*>(thread) << ") starting..." << reason;
  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    if (UNLIKELY(!thread->IsSuspended())) {
      LOG(ERROR) << "Resume(" << reinterpret_cast<void*>(thread)
          << ") thread not suspended";
      return false;
    }
    if (!Contains(thread)) {
      LOG(ERROR) << "Resume(" << reinterpret_cast<void*>(thread)
          << ") thread not within thread list";
      return false;
    }
    if (UNLIKELY(!thread->ModifySuspendCount(self, -1, nullptr, reason))) {
      LOG(ERROR) << "Resume(" << reinterpret_cast<void*>(thread)
                 << ") could not modify suspend count.";
      return false;
    }
  }
  {
    VLOG(threads) << "Resume(" << reinterpret_cast<void*>(thread) << ") waking others";
    MutexLock mu(self, *Locks::thread_suspend_count_lock_);
    Thread::resume_cond_->Broadcast(self);
  }
  VLOG(threads) << "Resume(" << reinterpret_cast<void*>(thread) << ") complete";
  return true;
}
static void ThreadSuspendByPeerWarning(ScopedObjectAccess& soa,
                                       LogSeverity severity,
                                       const char* message,
                                       jobject peer) REQUIRES_SHARED(Locks::mutator_lock_){
                                                     ObjPtr<mirror::Object> name =
                                                     WellKnownClasses::java_lang_Thread_name->GetObject(soa.Decode<mirror::Object>(peer));
                                                     if (name == nullptr) {
                                                     LOG(severity) << message << ": " << peer;
                                                     } else {
                                                     LOG(severity) << message << ": " << peer << ":" << name->AsString()->ToModifiedUtf8();
                                                     }
                                                     }
Thread* ThreadList::SuspendThreadByPeer(jobject peer,
                                        SuspendReason reason,
                                        bool* timed_out) {
  bool request_suspension = true;
  const uint64_t start_time = NanoTime();
  int self_suspend_count = 0;
  useconds_t sleep_us = kThreadSuspendInitialSleepUs;
  *timed_out = false;
  Thread* const self = Thread::Current();
  Thread* suspended_thread = nullptr;
  VLOG(threads) << "SuspendThreadByPeer starting";
  while (true) {
    Thread* thread;
    {
      ScopedObjectAccess soa(self);
      MutexLock thread_list_mu(self, *Locks::thread_list_lock_);
      thread = Thread::FromManagedThread(soa, peer);
      if (thread == nullptr) {
        if (suspended_thread != nullptr) {
          MutexLock suspend_count_mu(self, *Locks::thread_suspend_count_lock_);
          bool updated = suspended_thread->ModifySuspendCount(soa.Self(),
                                                              -1,
                                                              nullptr,
                                                              reason);
          DCHECK(updated);
        }
        ThreadSuspendByPeerWarning(soa,
                                   ::android::base::WARNING,
                                    "No such thread for suspend",
                                    peer);
        return nullptr;
      }
      if (!Contains(thread)) {
        CHECK(suspended_thread == nullptr);
        VLOG(threads) << "SuspendThreadByPeer failed for unattached thread: "
            << reinterpret_cast<void*>(thread);
        return nullptr;
      }
      VLOG(threads) << "SuspendThreadByPeer found thread: " << *thread;
      {
        MutexLock suspend_count_mu(self, *Locks::thread_suspend_count_lock_);
        if (request_suspension) {
          if (self->GetSuspendCount() > 0) {
            ++self_suspend_count;
            continue;
          }
          CHECK(suspended_thread == nullptr);
          suspended_thread = thread;
          bool updated = suspended_thread->ModifySuspendCount(self, +1, nullptr, reason);
          DCHECK(updated);
          request_suspension = false;
        } else {
          CHECK_GT(thread->GetSuspendCount(), 0);
        }
        CHECK_NE(thread, self) << "Attempt to suspend the current thread for the debugger";
        if (thread->IsSuspended()) {
          VLOG(threads) << "SuspendThreadByPeer thread suspended: " << *thread;
          if (ATraceEnabled()) {
            std::string name;
            thread->GetThreadName(name);
            ATraceBegin(StringPrintf("SuspendThreadByPeer suspended %s for peer=%p", name.c_str(),
                                      peer).c_str());
          }
          return thread;
        }
        const uint64_t total_delay = NanoTime() - start_time;
        if (total_delay >= thread_suspend_timeout_ns_) {
          if (suspended_thread == nullptr) {
            ThreadSuspendByPeerWarning(soa,
                                       ::android::base::FATAL,
                                       "Failed to issue suspend request",
                                       peer);
          } else {
            CHECK_EQ(suspended_thread, thread);
            LOG(WARNING) << "Suspended thread state_and_flags: "
                         << suspended_thread->StateAndFlagsAsHexString()
                         << ", self_suspend_count = " << self_suspend_count;
            Locks::thread_suspend_count_lock_->Unlock(self);
            ThreadSuspendByPeerWarning(soa,
                                       ::android::base::FATAL,
                                       "Thread suspension timed out",
                                       peer);
          }
          UNREACHABLE();
        } else if (sleep_us == 0 &&
            total_delay > static_cast<uint64_t>(kThreadSuspendMaxYieldUs) * 1000) {
          sleep_us = kThreadSuspendMaxYieldUs / 2;
        }
      }
    }
    VLOG(threads) << "SuspendThreadByPeer waiting to allow thread chance to suspend";
    ThreadSuspendSleep(sleep_us);
    sleep_us = std::min(sleep_us * 2, kThreadSuspendMaxSleepUs);
  }
}
static void ThreadSuspendByThreadIdWarning(LogSeverity severity,
                                           const char* message,
                                           uint32_t thread_id) {
  LOG(severity) << StringPrintf("%s: %d", message, thread_id);
}
Thread* ThreadList::SuspendThreadByThreadId(uint32_t thread_id,
                                            SuspendReason reason,
                                            bool* timed_out) {
  const uint64_t start_time = NanoTime();
  useconds_t sleep_us = kThreadSuspendInitialSleepUs;
  *timed_out = false;
  Thread* suspended_thread = nullptr;
  Thread* const self = Thread::Current();
  CHECK_NE(thread_id, kInvalidThreadId);
  VLOG(threads) << "SuspendThreadByThreadId starting";
  while (true) {
    {
      ScopedObjectAccess soa(self);
      MutexLock thread_list_mu(self, *Locks::thread_list_lock_);
      Thread* thread = nullptr;
      for (const auto& it : list_) {
        if (it->GetThreadId() == thread_id) {
          thread = it;
          break;
        }
      }
      if (thread == nullptr) {
        CHECK(suspended_thread == nullptr) << "Suspended thread " << suspended_thread
            << " no longer in thread list";
        ThreadSuspendByThreadIdWarning(::android::base::WARNING,
                                       "No such thread id for suspend",
                                       thread_id);
        return nullptr;
      }
      VLOG(threads) << "SuspendThreadByThreadId found thread: " << *thread;
      DCHECK(Contains(thread));
      {
        MutexLock suspend_count_mu(self, *Locks::thread_suspend_count_lock_);
        if (suspended_thread == nullptr) {
          if (self->GetSuspendCount() > 0) {
            continue;
          }
          bool updated = thread->ModifySuspendCount(self, +1, nullptr, reason);
          DCHECK(updated);
          suspended_thread = thread;
        } else {
          CHECK_EQ(suspended_thread, thread);
          CHECK_GT(thread->GetSuspendCount(), 0);
        }
        CHECK_NE(thread, self) << "Attempt to suspend the current thread for the debugger";
        if (thread->IsSuspended()) {
          if (ATraceEnabled()) {
            std::string name;
            thread->GetThreadName(name);
            ATraceBegin(StringPrintf("SuspendThreadByThreadId suspended %s id=%d",
                                      name.c_str(), thread_id).c_str());
          }
          VLOG(threads) << "SuspendThreadByThreadId thread suspended: " << *thread;
          return thread;
        }
        const uint64_t total_delay = NanoTime() - start_time;
        if (total_delay >= thread_suspend_timeout_ns_) {
          ThreadSuspendByThreadIdWarning(::android::base::WARNING,
                                         "Thread suspension timed out",
                                         thread_id);
          if (suspended_thread != nullptr) {
            bool updated = thread->ModifySuspendCount(soa.Self(), -1, nullptr, reason);
            DCHECK(updated);
          }
          *timed_out = true;
          return nullptr;
        } else if (sleep_us == 0 &&
            total_delay > static_cast<uint64_t>(kThreadSuspendMaxYieldUs) * 1000) {
          sleep_us = kThreadSuspendMaxYieldUs / 2;
        }
      }
    }
    VLOG(threads) << "SuspendThreadByThreadId waiting to allow thread chance to suspend";
    ThreadSuspendSleep(sleep_us);
    sleep_us = std::min(sleep_us * 2, kThreadSuspendMaxSleepUs);
  }
}
Thread* ThreadList::FindThreadByThreadId(uint32_t thread_id) {
  for (const auto& thread : list_) {
    if (thread->GetThreadId() == thread_id) {
      return thread;
    }
  }
  return nullptr;
}
Thread* ThreadList::FindThreadByTid(int tid) {
  for (const auto& thread : list_) {
    if (thread->GetTid() == tid) {
      return thread;
    }
  }
  return nullptr;
}
void ThreadList::WaitForOtherNonDaemonThreadsToExit(bool check_no_birth) {
  ScopedTrace trace(__PRETTY_FUNCTION__);
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertNotHeld(self);
  while (true) {
    Locks::runtime_shutdown_lock_->Lock(self);
    if (check_no_birth) {
      CHECK(Runtime::Current()->IsShuttingDownLocked());
      CHECK_EQ(Runtime::Current()->NumberOfThreadsBeingBorn(), 0U);
    } else {
      if (Runtime::Current()->NumberOfThreadsBeingBorn() != 0U) {
        Locks::runtime_shutdown_lock_->Unlock(self);
        usleep(1000);
        continue;
      }
    }
    MutexLock mu(self, *Locks::thread_list_lock_);
    Locks::runtime_shutdown_lock_->Unlock(self);
    bool done = unregistering_count_ == 0;
    if (done) {
      for (const auto& thread : list_) {
        if (thread != self && !thread->IsDaemon()) {
          done = false;
          break;
        }
      }
    }
    if (done) {
      break;
    }
    Locks::thread_exit_cond_->Wait(self);
  }
}
void ThreadList::SuspendAllDaemonThreadsForShutdown() {
  ScopedTrace trace(__PRETTY_FUNCTION__);
  Thread* self = Thread::Current();
  size_t daemons_left = 0;
  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    for (const auto& thread : list_) {
      CHECK(thread->IsDaemon()) << *thread;
      if (thread != self) {
        bool updated = thread->ModifySuspendCount(self, +1, nullptr, SuspendReason::kInternal);
        DCHECK(updated);
        ++daemons_left;
      }
      thread->GetJniEnv()->SetFunctionsToRuntimeShutdownFunctions();
    }
  }
  if (daemons_left == 0) {
    return;
  }
  bool have_complained = false;
  static constexpr size_t kTimeoutMicroseconds = 2000 * 1000;
  static constexpr size_t kSleepMicroseconds = 1000;
  bool all_suspended = false;
  for (size_t i = 0; !all_suspended && i < kTimeoutMicroseconds / kSleepMicroseconds; ++i) {
    bool found_running = false;
    {
      MutexLock mu(self, *Locks::thread_list_lock_);
      for (const auto& thread : list_) {
        if (thread != self && thread->GetState() == ThreadState::kRunnable) {
          if (!have_complained) {
            LOG(WARNING) << "daemon thread not yet suspended: " << *thread;
            have_complained = true;
          }
          found_running = true;
        }
      }
    }
    if (found_running) {
      usleep(kSleepMicroseconds);
    } else {
      all_suspended = true;
    }
  }
  if (!all_suspended) {
    LOG(WARNING) << "timed out suspending all daemon threads";
  }
  static constexpr size_t kDaemonSleepTime = 400'000;
  usleep(kDaemonSleepTime);
  std::list<Thread*> list_copy;
  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    for (const auto& thread : list_) {
      DCHECK(thread == self || !all_suspended || thread->GetState() != ThreadState::kRunnable);
      thread->GetJniEnv()->SetRuntimeDeleted();
    }
  }
  usleep(kDaemonSleepTime);
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(hwaddress_sanitizer)
  usleep(2 * kDaemonSleepTime);
#endif
#endif
}
void ThreadList::Register(Thread* self) {
  DCHECK_EQ(self, Thread::Current());
  CHECK(!shut_down_);
  if (VLOG_IS_ON(threads)) {
    std::ostringstream oss;
    self->ShortDump(oss);
    LOG(INFO) << "ThreadList::Register() " << *self << "\n" << oss.str();
  }
  MutexLock mu(self, *Locks::thread_list_lock_);
  MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
  for (int delta = suspend_all_count_; delta > 0; delta--) {
    bool updated = self->ModifySuspendCount(self, +1, nullptr, SuspendReason::kInternal);
    DCHECK(updated);
  }
  CHECK(!Contains(self));
  list_.push_back(self);
  if (gUseReadBarrier) {
    gc::collector::ConcurrentCopying* const cc =
        Runtime::Current()->GetHeap()->ConcurrentCopyingCollector();
    self->SetIsGcMarkingAndUpdateEntrypoints(cc->IsMarking());
    if (cc->IsUsingReadBarrierEntrypoints()) {
      self->SetReadBarrierEntrypoints();
    }
    self->SetWeakRefAccessEnabled(cc->IsWeakRefAccessEnabled());
  }
}
void ThreadList::Unregister(Thread* self, bool should_run_callbacks) {
  DCHECK_EQ(self, Thread::Current());
  CHECK_NE(self->GetState(), ThreadState::kRunnable);
  Locks::mutator_lock_->AssertNotHeld(self);
  if (self->tls32_.disable_thread_flip_count != 0) {
    LOG(FATAL) << "Incomplete PrimitiveArrayCritical section at exit: " << *self << "count = "
               << self->tls32_.disable_thread_flip_count;
  }
  VLOG(threads) << "ThreadList::Unregister() " << *self;
  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    ++unregistering_count_;
  }
  self->Destroy(should_run_callbacks);
  Trace::StoreExitingThreadInfo(self);
  uint32_t thin_lock_id = self->GetThreadId();
  while (true) {
    {
      MutexLock mu(self, *Locks::thread_list_lock_);
      if (!Contains(self)) {
        std::string thread_name;
        self->GetThreadName(thread_name);
        std::ostringstream os;
        DumpNativeStack(os, GetTid(), "  native: ", nullptr);
        LOG(ERROR) << "Request to unregister unattached thread " << thread_name << "\n" << os.str();
        break;
      } else {
        MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
        if (!self->IsSuspended()) {
          list_.remove(self);
          break;
        }
      }
    }
    usleep(1);
  }
  delete self;
  ReleaseThreadId(nullptr, thin_lock_id);
#ifdef __BIONIC__
  __get_tls()[TLS_SLOT_ART_THREAD_SELF] = nullptr;
#else
  CHECK_PTHREAD_CALL(pthread_setspecific, (Thread::pthread_key_self_, nullptr), "detach self");
  Thread::self_tls_ = nullptr;
#endif
  MutexLock mu(nullptr, *Locks::thread_list_lock_);
  --unregistering_count_;
  Locks::thread_exit_cond_->Broadcast(nullptr);
}
void ThreadList::ForEach(void (*callback)(Thread*, void*), void* context) {
  for (const auto& thread : list_) {
    callback(thread, context);
  }
}
void ThreadList::VisitRootsForSuspendedThreads(RootVisitor* visitor) {
  Thread* const self = Thread::Current();
  std::vector<Thread*> threads_to_visit;
  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    for (Thread* thread : list_) {
      bool suspended = thread->ModifySuspendCount(self, +1, nullptr, SuspendReason::kInternal);
      DCHECK(suspended);
      if (thread == self || thread->IsSuspended()) {
        threads_to_visit.push_back(thread);
      } else {
        bool resumed = thread->ModifySuspendCount(self, -1, nullptr, SuspendReason::kInternal);
        DCHECK(resumed);
      }
    }
  }
  for (Thread* thread : threads_to_visit) {
    thread->VisitRoots(visitor, kVisitRootFlagAllRoots);
  }
  {
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    for (Thread* thread : threads_to_visit) {
      bool updated = thread->ModifySuspendCount(self, -1, nullptr, SuspendReason::kInternal);
      DCHECK(updated);
    }
  }
}
void ThreadList::VisitRoots(RootVisitor* visitor, VisitRootFlags flags) const {
  MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
  for (const auto& thread : list_) {
    thread->VisitRoots(visitor, flags);
  }
}
void ThreadList::VisitReflectiveTargets(ReflectiveValueVisitor *visitor) const {
  MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
  for (const auto& thread : list_) {
    thread->VisitReflectiveTargets(visitor);
  }
}
void ThreadList::SweepInterpreterCaches(IsMarkedVisitor* visitor) const {
  MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
  for (const auto& thread : list_) {
    thread->SweepInterpreterCache(visitor);
  }
}
uint32_t ThreadList::AllocThreadId(Thread* self) {
  MutexLock mu(self, *Locks::allocated_thread_ids_lock_);
  for (size_t i = 0; i < allocated_ids_.size(); ++i) {
    if (!allocated_ids_[i]) {
      allocated_ids_.set(i);
      return i + 1;
    }
  }
  LOG(FATAL) << "Out of internal thread ids";
  UNREACHABLE();
}
void ThreadList::ReleaseThreadId(Thread* self, uint32_t id) {
  MutexLock mu(self, *Locks::allocated_thread_ids_lock_);
  --id;
  DCHECK(allocated_ids_[id]) << id;
  allocated_ids_.reset(id);
}
ScopedSuspendAll::ScopedSuspendAll(const char* cause, bool long_suspend) {
  Runtime::Current()->GetThreadList()->SuspendAll(cause, long_suspend);
}
ScopedSuspendAll::~ScopedSuspendAll() {
  Runtime::Current()->GetThreadList()->ResumeAll();
}
}
