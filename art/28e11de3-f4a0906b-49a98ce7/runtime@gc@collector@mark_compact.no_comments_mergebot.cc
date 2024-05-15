#include <fcntl.h>
#if !defined(FALLOC_FL_PUNCH_HOLE) || !defined(FALLOC_FL_KEEP_SIZE)
#include <linux/falloc.h>
#endif
#include <linux/userfaultfd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <numeric>
#include "android-base/file.h"
#include "android-base/parsebool.h"
#include "android-base/properties.h"
#include "base/file_utils.h"
#include "base/memfd.h"
#include "base/quasi_atomic.h"
#include "base/systrace.h"
#include "base/utils.h"
#include "gc/accounting/mod_union_table-inl.h"
#include "gc/collector_type.h"
#include "gc/reference_processor.h"
#include "gc/space/bump_pointer_space.h"
#include "gc/task_processor.h"
#include "gc/verification-inl.h"
#include "jit/jit_code_cache.h"
#include "mark_compact-inl.h"
#include "mirror/object-refvisitor-inl.h"
#include "read_barrier_config.h"
#include "scoped_thread_state_change-inl.h"
#include "sigchain.h"
#include "thread_list.h"
#ifdef ART_TARGET_ANDROID
#include "com_android_art.h"
#endif
#ifndef __BIONIC__
#ifndef MREMAP_DONTUNMAP
#define MREMAP_DONTUNMAP 4
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#ifndef __NR_userfaultfd
#if defined(__x86_64__)
#define __NR_userfaultfd 323
#elif defined(__i386__)
#define __NR_userfaultfd 374
#elif defined(__aarch64__)
#define __NR_userfaultfd 282
#elif defined(__arm__)
#define __NR_userfaultfd 388
#else
#error "__NR_userfaultfd undefined"
#endif
#endif
#endif
namespace {
using ::android::base::GetBoolProperty;
using ::android::base::ParseBool;
using ::android::base::ParseBoolResult;
}
namespace art {
static bool HaveMremapDontunmap() {
  void* old = mmap(nullptr, kPageSize, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
  CHECK_NE(old, MAP_FAILED);
  void* addr = mremap(old, kPageSize, kPageSize, MREMAP_MAYMOVE | MREMAP_DONTUNMAP, nullptr);
  CHECK_EQ(munmap(old, kPageSize), 0);
  if (addr != MAP_FAILED) {
    CHECK_EQ(munmap(addr, kPageSize), 0);
    return true;
  } else {
    return false;
  }
}
static bool gHaveMremapDontunmap = IsKernelVersionAtLeast(5, 13) || HaveMremapDontunmap();
static uint64_t gUffdFeatures = 0;
static constexpr uint64_t kUffdFeaturesForMinorFault =
    UFFD_FEATURE_MISSING_SHMEM | UFFD_FEATURE_MINOR_SHMEM;
static constexpr uint64_t kUffdFeaturesForSigbus = UFFD_FEATURE_SIGBUS;
static constexpr uint64_t kUffdFeaturesRequired =
    kUffdFeaturesForMinorFault | kUffdFeaturesForSigbus;
bool KernelSupportsUffd() {
#ifdef __linux__
  if (gHaveMremapDontunmap) {
    int fd = syscall(__NR_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
    if (!kIsTargetAndroid && fd == -1 && errno == EINVAL) {
      fd = syscall(__NR_userfaultfd, O_CLOEXEC);
    }
    if (fd >= 0) {
      struct uffdio_api api = {.api = UFFD_API, .features = 0, .ioctls = 0};
      CHECK_EQ(ioctl(fd, UFFDIO_API, &api), 0) << "ioctl_userfaultfd : API:" << strerror(errno);
      gUffdFeatures = api.features;
      close(fd);
      return (api.features & kUffdFeaturesRequired) == kUffdFeaturesRequired;
    }
  }
#endif
  return false;
}
namespace gc {
namespace collector {
static constexpr bool kCheckLocks = kDebugLocking;
static constexpr bool kVerifyRootsMarked = kIsDebugBuild;
static constexpr size_t kMaxNumUffdWorkers = 2;
static constexpr size_t kMutatorCompactionBufferCount = kIsDebugBuild ? 256 : 512;
static constexpr ssize_t kMinFromSpaceMadviseSize = 1 * MB;
static const bool gKernelHasFaultRetry = IsKernelVersionAtLeast(5, 7);
std::pair<bool, bool> MarkCompact::GetUffdAndMinorFault() {
  bool uffd_available;
  if (UNLIKELY(gUffdFeatures == 0)) {
    uffd_available = KernelSupportsUffd();
  } else {
    uffd_available = true;
  }
  bool minor_fault_available =
      (gUffdFeatures & kUffdFeaturesForMinorFault) == kUffdFeaturesForMinorFault;
  return std::pair<bool, bool>(uffd_available, minor_fault_available);
}
bool MarkCompact::CreateUserfaultfd(bool post_fork) {
  if (post_fork || uffd_ == kFdUnused) {
    if (gHaveMremapDontunmap) {
      uffd_ = syscall(__NR_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
      if (!kIsTargetAndroid && UNLIKELY(uffd_ == -1 && errno == EINVAL)) {
        uffd_ = syscall(__NR_userfaultfd, O_CLOEXEC);
      }
      if (UNLIKELY(uffd_ == -1)) {
        uffd_ = kFallbackMode;
        LOG(WARNING) << "Userfaultfd isn't supported (reason: " << strerror(errno)
                     << ") and therefore falling back to stop-the-world compaction.";
      } else {
        DCHECK(IsValidFd(uffd_));
<<<<<<< HEAD
        struct uffdio_api api = {.api = UFFD_API, .features = gUffdFeatures, .ioctls = 0};
        api.features &= use_uffd_sigbus_ ? kUffdFeaturesRequired : kUffdFeaturesForMinorFault;
        CHECK_EQ(ioctl(uffd_, UFFDIO_API, &api), 0)
            << "ioctl_userfaultfd: API: " << strerror(errno);
      }
    } else {
      uffd_ = kFallbackMode;
|||||||
        struct uffdio_api api = {
            .api = UFFD_API, .features = gUffdFeatures & kUffdFeaturesForMinorFault, .ioctls = 0};
        CHECK_EQ(ioctl(uffd_, UFFDIO_API, &api), 0)
            << "ioctl_userfaultfd: API: " << strerror(errno);
=======
        struct uffdio_api api = {.api = UFFD_API, .features = gUffdFeatures, .ioctls = 0};
        api.features &= use_uffd_sigbus_ ? kUffdFeaturesRequired : kUffdFeaturesForMinorFault;
        CHECK_EQ(ioctl(uffd_, UFFDIO_API, &api), 0)
            << "ioctl_userfaultfd: API: " << strerror(errno);
>>>>>>> f4a0906b4690ed3a710be7c04c989b1e55caa50c
    }
  }
  uffd_initialized_ = !post_fork || uffd_ == kFallbackMode;
  return IsValidFd(uffd_);
}
template <size_t kAlignment>
MarkCompact::LiveWordsBitmap<kAlignment>* MarkCompact::LiveWordsBitmap<kAlignment>::Create(
    uintptr_t begin, uintptr_t end) {
  return static_cast<LiveWordsBitmap<kAlignment>*>(
      MemRangeBitmap::Create("Concurrent Mark Compact live words bitmap", begin, end));
}
static bool IsSigbusFeatureAvailable() {
  MarkCompact::GetUffdAndMinorFault();
  return gUffdFeatures & UFFD_FEATURE_SIGBUS;
}
MarkCompact::MarkCompact(Heap* heap)
    : GarbageCollector(heap, "concurrent mark compact"),
      gc_barrier_(0),
      mark_stack_lock_("mark compact mark stack lock", kMarkSweepMarkStackLock),
      lock_("mark compact lock", kGenericBottomLock),
      bump_pointer_space_(heap->GetBumpPointerSpace()),
      moving_space_bitmap_(bump_pointer_space_->GetMarkBitmap()),
      moving_to_space_fd_(kFdUnused),
      moving_from_space_fd_(kFdUnused),
      uffd_(kFdUnused),
      sigbus_in_progress_count_(kSigbusCounterCompactionDoneMask),
      compaction_in_progress_count_(0),
      thread_pool_counter_(0),
      compacting_(false),
      uffd_initialized_(false),
      uffd_minor_fault_supported_(false),
      use_uffd_sigbus_(IsSigbusFeatureAvailable()),
      minor_fault_initialized_(false),
      map_linear_alloc_shared_(false) {
  if (kIsDebugBuild) {
    updated_roots_.reset(new std::unordered_set<void*>());
  }
  CHECK(!uffd_minor_fault_supported_);
  live_words_bitmap_.reset(LiveWordsBitmap<kAlignment>::Create(
      reinterpret_cast<uintptr_t>(bump_pointer_space_->Begin()),
      reinterpret_cast<uintptr_t>(bump_pointer_space_->Limit())));
  size_t moving_space_size = bump_pointer_space_->Capacity();
  size_t chunk_info_vec_size = moving_space_size / kOffsetChunkSize;
  size_t nr_moving_pages = moving_space_size / kPageSize;
  size_t nr_non_moving_pages = heap->GetNonMovingSpace()->Capacity() / kPageSize;
  std::string err_msg;
  info_map_ = MemMap::MapAnonymous(
      "Concurrent mark-compact chunk-info vector",
      chunk_info_vec_size * sizeof(uint32_t) + nr_non_moving_pages * sizeof(ObjReference) +
          nr_moving_pages * sizeof(ObjReference) + nr_moving_pages * sizeof(uint32_t),
      PROT_READ | PROT_WRITE,
                  false,
      &err_msg);
  if (UNLIKELY(!info_map_.IsValid())) {
    LOG(FATAL) << "Failed to allocate concurrent mark-compact chunk-info vector: " << err_msg;
  } else {
    uint8_t* p = info_map_.Begin();
    chunk_info_vec_ = reinterpret_cast<uint32_t*>(p);
    vector_length_ = chunk_info_vec_size;
    p += chunk_info_vec_size * sizeof(uint32_t);
    first_objs_non_moving_space_ = reinterpret_cast<ObjReference*>(p);
    p += nr_non_moving_pages * sizeof(ObjReference);
    first_objs_moving_space_ = reinterpret_cast<ObjReference*>(p);
    p += nr_moving_pages * sizeof(ObjReference);
    pre_compact_offset_moving_space_ = reinterpret_cast<uint32_t*>(p);
  }
  size_t moving_space_alignment = BestPageTableAlignment(moving_space_size);
  if (!IsAlignedParam(bump_pointer_space_->Begin(), moving_space_alignment)) {
    LOG(WARNING) << "Bump pointer space is not aligned to " << PrettySize(moving_space_alignment)
                 << ". This can lead to longer stop-the-world pauses for compaction";
  }
  from_space_map_ = MemMap::MapAnonymousAligned("Concurrent mark-compact from-space",
                                                moving_space_size,
                                                PROT_NONE,
                                                            kObjPtrPoisoning,
                                                moving_space_alignment,
                                                &err_msg);
  if (UNLIKELY(!from_space_map_.IsValid())) {
    LOG(FATAL) << "Failed to allocate concurrent mark-compact from-space" << err_msg;
  } else {
    from_space_begin_ = from_space_map_.Begin();
  }
  if (!kObjPtrPoisoning && uffd_minor_fault_supported_) {
    shadow_to_space_map_ = MemMap::MapAnonymous("Concurrent mark-compact moving-space shadow",
                                                moving_space_size,
                                                PROT_NONE,
                                                            false,
                                                &err_msg);
    if (!shadow_to_space_map_.IsValid()) {
      LOG(WARNING) << "Failed to allocate concurrent mark-compact moving-space shadow: " << err_msg;
    }
  }
<<<<<<< HEAD
  const size_t num_pages =
      1 + (use_uffd_sigbus_ ? kMutatorCompactionBufferCount :
                              std::min(heap_->GetParallelGCThreadCount(), kMaxNumUffdWorkers));
|||||||
    const size_t num_pages = 1 + std::min(heap_->GetParallelGCThreadCount(), kMaxNumUffdWorkers);
=======
    const size_t num_pages =
        1 + (use_uffd_sigbus_ ? kMutatorCompactionBufferCount :
                                std::min(heap_->GetParallelGCThreadCount(), kMaxNumUffdWorkers));
>>>>>>> f4a0906b4690ed3a710be7c04c989b1e55caa50c
  compaction_buffers_map_ = MemMap::MapAnonymous("Concurrent mark-compact compaction buffers",
                                                 kPageSize * num_pages,
                                                 PROT_READ | PROT_WRITE,
                                                             kObjPtrPoisoning,
                                                 &err_msg);
  if (UNLIKELY(!compaction_buffers_map_.IsValid())) {
    LOG(FATAL) << "Failed to allocate concurrent mark-compact compaction buffers" << err_msg;
  }
  conc_compaction_termination_page_ = compaction_buffers_map_.Begin();
  ForceRead(conc_compaction_termination_page_);
  linear_alloc_spaces_data_.reserve(1);
  metrics::ArtMetrics* metrics = GetMetrics();
  gc_time_histogram_ = metrics->FullGcCollectionTime();
  metrics_gc_count_ = metrics->FullGcCount();
  metrics_gc_count_delta_ = metrics->FullGcCountDelta();
  gc_throughput_histogram_ = metrics->FullGcThroughput();
  gc_tracing_throughput_hist_ = metrics->FullGcTracingThroughput();
  gc_throughput_avg_ = metrics->FullGcThroughputAvg();
  gc_tracing_throughput_avg_ = metrics->FullGcTracingThroughputAvg();
  gc_scanned_bytes_ = metrics->FullGcScannedBytes();
  gc_scanned_bytes_delta_ = metrics->FullGcScannedBytesDelta();
  gc_freed_bytes_ = metrics->FullGcFreedBytes();
  gc_freed_bytes_delta_ = metrics->FullGcFreedBytesDelta();
  gc_duration_ = metrics->FullGcDuration();
  gc_duration_delta_ = metrics->FullGcDurationDelta();
  are_metrics_initialized_ = true;
}
void MarkCompact::AddLinearAllocSpaceData(uint8_t* begin, size_t len) {
  DCHECK_ALIGNED(begin, kPageSize);
  DCHECK_ALIGNED(len, kPageSize);
  DCHECK_GE(len, kPMDSize);
  size_t alignment = BestPageTableAlignment(len);
  bool is_shared = false;
  if (map_linear_alloc_shared_) {
    void* ret = mmap(begin,
                     len,
                     PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
                            -1,
                                0);
    CHECK_EQ(ret, begin) << "mmap failed: " << strerror(errno);
    is_shared = true;
  }
  std::string err_msg;
  MemMap shadow(MemMap::MapAnonymousAligned("linear-alloc shadow map",
                                            len,
                                            PROT_NONE,
                                                        false,
                                            alignment,
                                            &err_msg));
  if (!shadow.IsValid()) {
    LOG(FATAL) << "Failed to allocate linear-alloc shadow map: " << err_msg;
    UNREACHABLE();
  }
  MemMap page_status_map(MemMap::MapAnonymous("linear-alloc page-status map",
                                              len / kPageSize,
                                              PROT_READ | PROT_WRITE,
                                                          false,
                                              &err_msg));
  if (!page_status_map.IsValid()) {
    LOG(FATAL) << "Failed to allocate linear-alloc page-status shadow map: " << err_msg;
    UNREACHABLE();
  }
  linear_alloc_spaces_data_.emplace_back(std::forward<MemMap>(shadow),
                                         std::forward<MemMap>(page_status_map),
                                         begin,
                                         begin + len,
                                         is_shared);
}
void MarkCompact::BindAndResetBitmaps() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  accounting::CardTable* const card_table = heap_->GetCardTable();
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    if (space->GetGcRetentionPolicy() == space::kGcRetentionPolicyNeverCollect ||
        space->GetGcRetentionPolicy() == space::kGcRetentionPolicyFullCollect) {
      CHECK(space->IsZygoteSpace() || space->IsImageSpace());
      immune_spaces_.AddSpace(space);
      accounting::ModUnionTable* table = heap_->FindModUnionTableFromSpace(space);
      if (table != nullptr) {
        table->ProcessCards();
      } else {
        card_table->ModifyCardsAtomic(
            space->Begin(),
            space->End(),
            [](uint8_t card) {
              return (card == gc::accounting::CardTable::kCardClean) ?
                         card :
                         gc::accounting::CardTable::kCardAged;
            },
                                        VoidFunctor());
      }
    } else {
      CHECK(!space->IsZygoteSpace());
      CHECK(!space->IsImageSpace());
      card_table->ClearCardRange(space->Begin(), space->Limit());
      if (space != bump_pointer_space_) {
        CHECK_EQ(space, heap_->GetNonMovingSpace());
        non_moving_space_ = space;
        non_moving_space_bitmap_ = space->GetMarkBitmap();
      }
    }
  }
}
void MarkCompact::MarkZygoteLargeObjects() {
  Thread* self = thread_running_gc_;
  DCHECK_EQ(self, Thread::Current());
  space::LargeObjectSpace* const los = heap_->GetLargeObjectsSpace();
  if (los != nullptr) {
    accounting::LargeObjectBitmap* const live_bitmap = los->GetLiveBitmap();
    accounting::LargeObjectBitmap* const mark_bitmap = los->GetMarkBitmap();
    std::pair<uint8_t*, uint8_t*> range = los->GetBeginEndAtomic();
    live_bitmap->VisitMarkedRange(reinterpret_cast<uintptr_t>(range.first),
                                  reinterpret_cast<uintptr_t>(range.second),
                                  [mark_bitmap, los, self](mirror::Object* obj)
                                      REQUIRES(Locks::heap_bitmap_lock_)
                                          REQUIRES_SHARED(Locks::mutator_lock_) {
                                            if (los->IsZygoteLargeObject(self, obj)) {
                                              mark_bitmap->Set(obj);
                                            }
                                          });
  }
}
void MarkCompact::InitializePhase() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  mark_stack_ = heap_->GetMarkStack();
  CHECK(mark_stack_->IsEmpty());
  immune_spaces_.Reset();
  moving_first_objs_count_ = 0;
  non_moving_first_objs_count_ = 0;
  black_page_count_ = 0;
  freed_objects_ = 0;
  compaction_buffer_counter_ = 1;
  from_space_slide_diff_ = from_space_begin_ - bump_pointer_space_->Begin();
  black_allocations_begin_ = bump_pointer_space_->Limit();
  walk_super_class_cache_ = nullptr;
  pointer_size_ = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
}
class MarkCompact::ThreadFlipVisitor : public Closure {
 public:
  explicitThreadFlipVisitor(MarkCompact* collector) : collector_(collector) {}
  void Run(Thread* thread) override REQUIRES_SHARED(Locks::mutator_lock_) {
    Thread* self = Thread::Current();
    CHECK(thread == self || thread->GetState() != ThreadState::kRunnable)
        << thread->GetState() << " thread " << thread << " self " << self;
    thread->VisitRoots(collector_, kVisitRootFlagAllRoots);
    CHECK(collector_->compacting_);
    thread->SweepInterpreterCache(collector_);
    thread->AdjustTlab(collector_->black_objs_slide_diff_);
    collector_->GetBarrier().Pass(self);
  }
 private:
  MarkCompact* const collector_;
};
class MarkCompact::FlipCallback : public Closure {
 public:
  explicitFlipCallback(MarkCompact* collector) : collector_(collector) {}
  void Run(Thread* thread ATTRIBUTE_UNUSED) override REQUIRES(Locks::mutator_lock_) {
    collector_->CompactionPause();
  }
 private:
  MarkCompact* const collector_;
};
void MarkCompact::RunPhases() {
  Thread* self = Thread::Current();
  thread_running_gc_ = self;
  Runtime* runtime = Runtime::Current();
  InitializePhase();
  GetHeap()->PreGcVerification(this);
  {
    ReaderMutexLock mu(self, *Locks::mutator_lock_);
    MarkingPhase();
  }
  {
    ScopedPause pause(this);
    MarkingPause();
    if (kIsDebugBuild) {
      bump_pointer_space_->AssertAllThreadLocalBuffersAreRevoked();
    }
  }
  if (kIsDebugBuild && heap_->GetTaskProcessor()->GetRunningThread() == thread_running_gc_) {
    usleep(500'000);
  }
  {
    ReaderMutexLock mu(self, *Locks::mutator_lock_);
    ReclaimPhase();
    PrepareForCompaction();
  }
  if (uffd_ != kFallbackMode && !use_uffd_sigbus_) {
    heap_->GetThreadPool()->WaitForWorkersToBeCreated();
  }
  {
    gc_barrier_.Init(self, 0);
    ThreadFlipVisitor visitor(this);
    FlipCallback callback(this);
    size_t barrier_count = runtime->GetThreadList()->FlipThreadRoots(
        &visitor, &callback, this, GetHeap()->GetGcPauseListener());
    {
      ScopedThreadStateChange tsc(self, ThreadState::kWaitingForCheckPointsToRun);
      gc_barrier_.Increment(self, barrier_count);
    }
  }
  if (IsValidFd(uffd_)) {
    ReaderMutexLock mu(self, *Locks::mutator_lock_);
    CompactionPhase();
  }
  FinishPhase();
  thread_running_gc_ = nullptr;
  GetHeap()->PostGcVerification(this);
}
void MarkCompact::InitMovingSpaceFirstObjects(const size_t vec_len) {
  size_t to_space_page_idx = 0;
  uint32_t offset_in_chunk_word;
  uint32_t offset;
  mirror::Object* obj;
  const uintptr_t heap_begin = moving_space_bitmap_->HeapBegin();
  size_t chunk_idx;
  for (chunk_idx = 0; chunk_info_vec_[chunk_idx] == 0; chunk_idx++) {
    if (chunk_idx > vec_len) {
      return;
    }
  }
  offset_in_chunk_word = live_words_bitmap_->FindNthLiveWordOffset(chunk_idx, 0);
  offset = chunk_idx * kBitsPerVectorWord + offset_in_chunk_word;
  DCHECK(live_words_bitmap_->Test(offset))
      << "offset=" << offset << " chunk_idx=" << chunk_idx << " N=0"
      << " offset_in_word=" << offset_in_chunk_word << " word=" << std::hex
      << live_words_bitmap_->GetWord(chunk_idx);
  obj = reinterpret_cast<mirror::Object*>(heap_begin + offset * kAlignment);
  pre_compact_offset_moving_space_[to_space_page_idx] = offset;
  first_objs_moving_space_[to_space_page_idx].Assign(obj);
  to_space_page_idx++;
  uint32_t page_live_bytes = 0;
  while (true) {
    for (; page_live_bytes <= kPageSize; chunk_idx++) {
      if (chunk_idx > vec_len) {
        moving_first_objs_count_ = to_space_page_idx;
        return;
      }
      page_live_bytes += chunk_info_vec_[chunk_idx];
    }
    chunk_idx--;
    page_live_bytes -= kPageSize;
    DCHECK_LE(page_live_bytes, kOffsetChunkSize);
    DCHECK_LE(page_live_bytes, chunk_info_vec_[chunk_idx])
        << " chunk_idx=" << chunk_idx << " to_space_page_idx=" << to_space_page_idx
        << " vec_len=" << vec_len;
    DCHECK(IsAligned<kAlignment>(chunk_info_vec_[chunk_idx] - page_live_bytes));
    offset_in_chunk_word = live_words_bitmap_->FindNthLiveWordOffset(
        chunk_idx, (chunk_info_vec_[chunk_idx] - page_live_bytes) / kAlignment);
    offset = chunk_idx * kBitsPerVectorWord + offset_in_chunk_word;
    DCHECK(live_words_bitmap_->Test(offset))
        << "offset=" << offset << " chunk_idx=" << chunk_idx
        << " N=" << ((chunk_info_vec_[chunk_idx] - page_live_bytes) / kAlignment)
        << " offset_in_word=" << offset_in_chunk_word << " word=" << std::hex
        << live_words_bitmap_->GetWord(chunk_idx);
    obj = moving_space_bitmap_->FindPrecedingObject(heap_begin + offset * kAlignment);
    pre_compact_offset_moving_space_[to_space_page_idx] = offset;
    first_objs_moving_space_[to_space_page_idx].Assign(obj);
    to_space_page_idx++;
    chunk_idx++;
  }
}
void MarkCompact::InitNonMovingSpaceFirstObjects() {
  accounting::ContinuousSpaceBitmap* bitmap = non_moving_space_->GetLiveBitmap();
  uintptr_t begin = reinterpret_cast<uintptr_t>(non_moving_space_->Begin());
  const uintptr_t end = reinterpret_cast<uintptr_t>(non_moving_space_->End());
  mirror::Object* prev_obj;
  size_t page_idx;
  {
    mirror::Object* obj = nullptr;
    bitmap->VisitMarkedRange< true>(
        begin, end, [&obj](mirror::Object* o) { obj = o; });
    if (obj == nullptr) {
      return;
    }
    page_idx = (reinterpret_cast<uintptr_t>(obj) - begin) / kPageSize;
    first_objs_non_moving_space_[page_idx++].Assign(obj);
    prev_obj = obj;
  }
  uintptr_t prev_obj_end = reinterpret_cast<uintptr_t>(prev_obj) +
                           RoundUp(prev_obj->SizeOf<kDefaultVerifyFlags>(), kAlignment);
  begin = RoundDown(reinterpret_cast<uintptr_t>(prev_obj) + kPageSize, kPageSize);
  while (begin < end) {
    if (prev_obj != nullptr && prev_obj_end > begin) {
      DCHECK_LT(prev_obj, reinterpret_cast<mirror::Object*>(begin));
      first_objs_non_moving_space_[page_idx].Assign(prev_obj);
      mirror::Class* klass = prev_obj->GetClass<kVerifyNone, kWithoutReadBarrier>();
      if (bump_pointer_space_->HasAddress(klass)) {
        LOG(WARNING) << "found inter-page object " << prev_obj << " in non-moving space with klass "
                     << klass << " in moving space";
      }
    } else {
      prev_obj_end = 0;
      prev_obj = bitmap->FindPrecedingObject(begin, begin - kPageSize);
      if (prev_obj != nullptr) {
        prev_obj_end = reinterpret_cast<uintptr_t>(prev_obj) +
                       RoundUp(prev_obj->SizeOf<kDefaultVerifyFlags>(), kAlignment);
      }
      if (prev_obj_end > begin) {
        mirror::Class* klass = prev_obj->GetClass<kVerifyNone, kWithoutReadBarrier>();
        if (bump_pointer_space_->HasAddress(klass)) {
          LOG(WARNING) << "found inter-page object " << prev_obj
                       << " in non-moving space with klass " << klass << " in moving space";
        }
        first_objs_non_moving_space_[page_idx].Assign(prev_obj);
      } else {
        bitmap->VisitMarkedRange< true>(
            begin, begin + kPageSize, [this, page_idx](mirror::Object* obj) {
              first_objs_non_moving_space_[page_idx].Assign(obj);
            });
      }
    }
    begin += kPageSize;
    page_idx++;
  }
  non_moving_first_objs_count_ = page_idx;
}
bool MarkCompact::CanCompactMovingSpaceWithMinorFault() {
  size_t min_size = (moving_first_objs_count_ + black_page_count_) * kPageSize;
  return minor_fault_initialized_ && shadow_to_space_map_.IsValid() &&
         shadow_to_space_map_.Size() >= min_size;
}
class MarkCompact::ConcurrentCompactionGcTask : public SelfDeletingTask {
 public:
  explicit ConcurrentCompactionGcTask(MarkCompact* collector, size_t idx)
      : collector_(collector), index_(idx) {}
  void Run(Thread* self ATTRIBUTE_UNUSED) override REQUIRES_SHARED(Locks::mutator_lock_) {
    if (collector_->CanCompactMovingSpaceWithMinorFault()) {
      collector_->ConcurrentCompaction<MarkCompact::kMinorFaultMode>( nullptr);
    } else {
      uint8_t* buf = collector_->compaction_buffers_map_.Begin() + index_ * kPageSize;
      collector_->ConcurrentCompaction<MarkCompact::kCopyMode>(buf);
    }
  }
 private:
  MarkCompact* const collector_;
  size_t index_;
};
void MarkCompact::PrepareForCompaction() {
  uint8_t* space_begin = bump_pointer_space_->Begin();
  size_t vector_len = (black_allocations_begin_ - space_begin) / kOffsetChunkSize;
  DCHECK_LE(vector_len, vector_length_);
  for (size_t i = 0; i < vector_len; i++) {
    DCHECK_LE(chunk_info_vec_[i], kOffsetChunkSize);
    DCHECK_EQ(chunk_info_vec_[i], live_words_bitmap_->LiveBytesInBitmapWord(i));
  }
  InitMovingSpaceFirstObjects(vector_len);
  InitNonMovingSpaceFirstObjects();
  uint32_t total;
  if (vector_len < vector_length_) {
    vector_len++;
    total = 0;
  } else {
    total = chunk_info_vec_[vector_len - 1];
  }
  std::exclusive_scan(chunk_info_vec_, chunk_info_vec_ + vector_len, chunk_info_vec_, 0);
  total += chunk_info_vec_[vector_len - 1];
  for (size_t i = vector_len; i < vector_length_; i++) {
    DCHECK_EQ(chunk_info_vec_[i], 0u);
  }
  post_compact_end_ = AlignUp(space_begin + total, kPageSize);
  CHECK_EQ(post_compact_end_, space_begin + moving_first_objs_count_ * kPageSize);
  black_objs_slide_diff_ = black_allocations_begin_ - post_compact_end_;
  bool is_zygote = Runtime::Current()->IsZygote();
  if (!uffd_initialized_ && CreateUserfaultfd( false)) {
    if (!use_uffd_sigbus_) {
      struct uffdio_register uffd_register;
      uffd_register.range.start = reinterpret_cast<uintptr_t>(conc_compaction_termination_page_);
      uffd_register.range.len = kPageSize;
      uffd_register.mode = UFFDIO_REGISTER_MODE_MISSING;
      CHECK_EQ(ioctl(uffd_, UFFDIO_REGISTER, &uffd_register), 0)
          << "ioctl_userfaultfd: register compaction termination page: " << strerror(errno);
    }
    if (!uffd_minor_fault_supported_ && shadow_to_space_map_.IsValid()) {
      CHECK_EQ(shadow_to_space_map_.Size(), bump_pointer_space_->Capacity());
      shadow_to_space_map_.Reset();
    }
  }
  if (uffd_ != kFallbackMode) {
    if (!use_uffd_sigbus_) {
      ThreadPool* pool = heap_->GetThreadPool();
      if (UNLIKELY(pool == nullptr)) {
        heap_->CreateThreadPool(std::min(heap_->GetParallelGCThreadCount(), kMaxNumUffdWorkers));
        pool = heap_->GetThreadPool();
      }
      size_t num_threads = pool->GetThreadCount();
      thread_pool_counter_ = num_threads;
      for (size_t i = 0; i < num_threads; i++) {
        pool->AddTask(thread_running_gc_, new ConcurrentCompactionGcTask(this, i + 1));
      }
      CHECK_EQ(pool->GetTaskCount(thread_running_gc_), num_threads);
    }
    auto mmap_shadow_map = [this](int flags, int fd) {
      void* ret = mmap(shadow_to_space_map_.Begin(),
                       shadow_to_space_map_.Size(),
                       PROT_READ | PROT_WRITE,
                       flags,
                       fd,
                                  0);
      DCHECK_NE(ret, MAP_FAILED) << "mmap for moving-space shadow failed:" << strerror(errno);
    };
    if (minor_fault_initialized_) {
      DCHECK(!is_zygote);
      if (UNLIKELY(!shadow_to_space_map_.IsValid())) {
        DCHECK_GE(moving_to_space_fd_, 0);
        size_t reqd_size = std::min(moving_first_objs_count_ * kPageSize + 4 * MB,
                                    bump_pointer_space_->Capacity());
        std::string err_msg;
        shadow_to_space_map_ = MemMap::MapAnonymous("moving-space-shadow",
                                                    reqd_size,
                                                    PROT_NONE,
                                                                kObjPtrPoisoning,
                                                    &err_msg);
        if (shadow_to_space_map_.IsValid()) {
          CHECK(!kMemoryToolAddsRedzones || shadow_to_space_map_.GetRedzoneSize() == 0u);
          MemMap temp = shadow_to_space_map_.TakeReservedMemory(shadow_to_space_map_.Size(),
                                                                          true);
          std::swap(temp, shadow_to_space_map_);
          DCHECK(!temp.IsValid());
        } else {
          LOG(WARNING) << "Failed to create moving space's shadow map of " << PrettySize(reqd_size)
                       << " size. " << err_msg;
        }
      }
      if (LIKELY(shadow_to_space_map_.IsValid())) {
        int fd = moving_to_space_fd_;
        int mmap_flags = MAP_SHARED | MAP_FIXED;
        if (fd == kFdUnused) {
          DCHECK_EQ(shadow_to_space_map_.Size(), bump_pointer_space_->Capacity());
          mmap_flags |= MAP_ANONYMOUS;
          fd = -1;
        }
        mmap_shadow_map(mmap_flags, fd);
      }
      for (auto& data : linear_alloc_spaces_data_) {
        DCHECK_EQ(mprotect(data.shadow_.Begin(), data.shadow_.Size(), PROT_READ | PROT_WRITE), 0)
            << "mprotect failed: " << strerror(errno);
      }
    } else if (!is_zygote && uffd_minor_fault_supported_) {
      if (shadow_to_space_map_.IsValid() &&
          shadow_to_space_map_.Size() == bump_pointer_space_->Capacity()) {
        mmap_shadow_map(MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS, -1);
      } else {
        size_t size = bump_pointer_space_->Capacity();
        DCHECK_EQ(moving_to_space_fd_, kFdUnused);
        DCHECK_EQ(moving_from_space_fd_, kFdUnused);
        const char* name = bump_pointer_space_->GetName();
        moving_to_space_fd_ = memfd_create(name, MFD_CLOEXEC);
        CHECK_NE(moving_to_space_fd_, -1)
            << "memfd_create: failed for " << name << ": " << strerror(errno);
        moving_from_space_fd_ = memfd_create(name, MFD_CLOEXEC);
        CHECK_NE(moving_from_space_fd_, -1)
            << "memfd_create: failed for " << name << ": " << strerror(errno);
        bool rlimit_changed = false;
        rlimit rlim_read;
        CHECK_EQ(getrlimit(RLIMIT_FSIZE, &rlim_read), 0) << "getrlimit failed: " << strerror(errno);
        if (rlim_read.rlim_cur < size) {
          rlimit_changed = true;
          rlimit rlim = rlim_read;
          rlim.rlim_cur = size;
          CHECK_EQ(setrlimit(RLIMIT_FSIZE, &rlim), 0) << "setrlimit failed: " << strerror(errno);
        }
        int ret = ftruncate(moving_to_space_fd_, size);
        CHECK_EQ(ret, 0) << "ftruncate failed for moving-space:" << strerror(errno);
        ret = ftruncate(moving_from_space_fd_, size);
        CHECK_EQ(ret, 0) << "ftruncate failed for moving-space:" << strerror(errno);
        if (rlimit_changed) {
          CHECK_EQ(setrlimit(RLIMIT_FSIZE, &rlim_read), 0)
              << "setrlimit failed: " << strerror(errno);
        }
      }
    }
  }
}
class MarkCompact::VerifyRootMarkedVisitor : public SingleRootVisitor {
 public:
  explicit VerifyRootMarkedVisitor(MarkCompact* collector) : collector_(collector) {}
  void VisitRoot(mirror::Object* root, const RootInfo& info) override
      REQUIRES_SHARED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    CHECK(collector_->IsMarked(root) != nullptr) << info.ToString();
  }
 private:
  MarkCompact* const collector_;
};
void MarkCompact::ReMarkRoots(Runtime* runtime) {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  DCHECK_EQ(thread_running_gc_, Thread::Current());
  Locks::mutator_lock_->AssertExclusiveHeld(thread_running_gc_);
  MarkNonThreadRoots(runtime);
  MarkConcurrentRoots(
      static_cast<VisitRootFlags>(kVisitRootFlagNewRoots | kVisitRootFlagStopLoggingNewRoots |
                                  kVisitRootFlagClearRootLog),
      runtime);
  if (kVerifyRootsMarked) {
    TimingLogger::ScopedTiming t2("(Paused)VerifyRoots", GetTimings());
    VerifyRootMarkedVisitor visitor(this);
    runtime->VisitRoots(&visitor);
  }
}
void MarkCompact::MarkingPause() {
  TimingLogger::ScopedTiming t("(Paused)MarkingPause", GetTimings());
  Runtime* runtime = Runtime::Current();
  Locks::mutator_lock_->AssertExclusiveHeld(thread_running_gc_);
  {
    WriterMutexLock mu(thread_running_gc_, *Locks::heap_bitmap_lock_);
    {
      MutexLock mu2(thread_running_gc_, *Locks::runtime_shutdown_lock_);
      MutexLock mu3(thread_running_gc_, *Locks::thread_list_lock_);
      std::list<Thread*> thread_list = runtime->GetThreadList()->GetList();
      for (Thread* thread : thread_list) {
        thread->VisitRoots(this, static_cast<VisitRootFlags>(0));
        DCHECK_EQ(thread->GetThreadLocalGcBuffer(), nullptr);
        thread->RevokeThreadLocalAllocationStack();
        bump_pointer_space_->RevokeThreadLocalBuffers(thread);
      }
    }
    freed_objects_ += bump_pointer_space_->GetAccumulatedObjectsAllocated();
    black_allocations_begin_ = bump_pointer_space_->AlignEnd(thread_running_gc_, kPageSize);
    DCHECK(IsAligned<kAlignment>(black_allocations_begin_));
    black_allocations_begin_ = AlignUp(black_allocations_begin_, kPageSize);
    ReMarkRoots(runtime);
    RecursiveMarkDirtyObjects( true, accounting::CardTable::kCardDirty);
    {
      TimingLogger::ScopedTiming t2("SwapStacks", GetTimings());
      heap_->SwapStacks();
      live_stack_freeze_size_ = heap_->GetLiveStack()->Size();
    }
  }
  runtime->DisallowNewSystemWeaks();
  heap_->GetReferenceProcessor()->EnableSlowPath();
}
void MarkCompact::SweepSystemWeaks(Thread* self, Runtime* runtime, const bool paused) {
  TimingLogger::ScopedTiming t(paused ? "(Paused)SweepSystemWeaks" : "SweepSystemWeaks",
                               GetTimings());
  ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
  runtime->SweepSystemWeaks(this);
}
void MarkCompact::ProcessReferences(Thread* self) {
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  GetHeap()->GetReferenceProcessor()->ProcessReferences(self, GetTimings());
}
void MarkCompact::Sweep(bool swap_bitmaps) {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  CHECK_GE(live_stack_freeze_size_, GetHeap()->GetLiveStack()->Size());
  {
    TimingLogger::ScopedTiming t2("MarkAllocStackAsLive", GetTimings());
    accounting::ObjectStack* live_stack = heap_->GetLiveStack();
    heap_->MarkAllocStackAsLive(live_stack);
    live_stack->Reset();
    DCHECK(mark_stack_->IsEmpty());
  }
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    if (space->IsContinuousMemMapAllocSpace() && space != bump_pointer_space_) {
      space::ContinuousMemMapAllocSpace* alloc_space = space->AsContinuousMemMapAllocSpace();
      TimingLogger::ScopedTiming split(
          alloc_space->IsZygoteSpace() ? "SweepZygoteSpace" : "SweepMallocSpace", GetTimings());
      RecordFree(alloc_space->Sweep(swap_bitmaps));
    }
  }
  SweepLargeObjects(swap_bitmaps);
}
void MarkCompact::SweepLargeObjects(bool swap_bitmaps) {
  space::LargeObjectSpace* los = heap_->GetLargeObjectsSpace();
  if (los != nullptr) {
    TimingLogger::ScopedTiming split(__FUNCTION__, GetTimings());
    RecordFreeLOS(los->Sweep(swap_bitmaps));
  }
}
void MarkCompact::ReclaimPhase() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  DCHECK(thread_running_gc_ == Thread::Current());
  Runtime* const runtime = Runtime::Current();
  ProcessReferences(thread_running_gc_);
  SweepSystemWeaks(thread_running_gc_, runtime, false);
  runtime->AllowNewSystemWeaks();
  runtime->GetClassLinker()->CleanupClassLoaders();
  {
    WriterMutexLock mu(thread_running_gc_, *Locks::heap_bitmap_lock_);
    Sweep(false);
    SwapBitmaps();
    GetHeap()->UnBindBitmaps();
  }
}
template <bool kCheckBegin, bool kCheckEnd>
class MarkCompact::RefsUpdateVisitor {
 public:
  explicit RefsUpdateVisitor(MarkCompact* collector,
                             mirror::Object* obj,
                             uint8_t* begin,
                             uint8_t* end)
      : collector_(collector), obj_(obj), begin_(begin), end_(end) {
    DCHECK(!kCheckBegin || begin != nullptr);
    DCHECK(!kCheckEnd || end != nullptr);
  }
  void operator()(mirror::Object* old ATTRIBUTE_UNUSED,
                  MemberOffset offset,
                  bool ) constprivate : MarkCompact* const collector_;
 public:
  REQUIRES_SHARED(Locks::heap_bitmap_lock_) { collector_->UpdateRef(obj_, offset); }
 private:
  MarkCompact* const collector_;
  MarkCompact* const collector_;
 public:
  REQUIRES_SHARED(Locks::heap_bitmap_lock_) { collector_->UpdateRef(obj_, offset); }
 private:
  MarkCompact* const collector_;
 public:
  ALWAYS_INLINE
  REQUIRES_SHARED(Locks::mutator_lock_) { collector_->UpdateRoot(root); }
 private:
  MarkCompact* const collector_;
 public:
  ALWAYS_INLINE
  REQUIRES_SHARED(Locks::mutator_lock_) { collector_->UpdateRoot(root); }
 private:
  MarkCompact* const collector_;
  mirror::Object* const obj_;
  uint8_t* const begin_;
  uint8_t* const end_;
};
bool MarkCompact::IsValidObject(mirror::Object* obj) const {
  mirror::Class* klass = obj->GetClass<kVerifyNone, kWithoutReadBarrier>();
  if (!heap_->GetVerification()->IsValidHeapObjectAddress(klass)) {
    return false;
  }
  return heap_->GetVerification()->IsValidClassUnchecked<kWithFromSpaceBarrier>(
      obj->GetClass<kVerifyNone, kWithFromSpaceBarrier>());
}
template <typename Callback>
void MarkCompact::VerifyObject(mirror::Object* ref, Callback& callback) const {
  if (kIsDebugBuild) {
    mirror::Class* klass = ref->GetClass<kVerifyNone, kWithFromSpaceBarrier>();
    mirror::Class* pre_compact_klass = ref->GetClass<kVerifyNone, kWithoutReadBarrier>();
    mirror::Class* klass_klass = klass->GetClass<kVerifyNone, kWithFromSpaceBarrier>();
    mirror::Class* klass_klass_klass = klass_klass->GetClass<kVerifyNone, kWithFromSpaceBarrier>();
    if (bump_pointer_space_->HasAddress(pre_compact_klass) &&
        reinterpret_cast<uint8_t*>(pre_compact_klass) < black_allocations_begin_) {
      CHECK(moving_space_bitmap_->Test(pre_compact_klass))
          << "ref=" << ref << " post_compact_end=" << static_cast<void*>(post_compact_end_)
          << " pre_compact_klass=" << pre_compact_klass
          << " black_allocations_begin=" << static_cast<void*>(black_allocations_begin_);
      CHECK(live_words_bitmap_->Test(pre_compact_klass));
    }
    if (!IsValidObject(ref)) {
      std::ostringstream oss;
      oss << "Invalid object: "
          << "ref=" << ref << " klass=" << klass << " klass_klass=" << klass_klass
          << " klass_klass_klass=" << klass_klass_klass
          << " pre_compact_klass=" << pre_compact_klass
          << " from_space_begin=" << static_cast<void*>(from_space_begin_)
          << " pre_compact_begin=" << static_cast<void*>(bump_pointer_space_->Begin())
          << " post_compact_end=" << static_cast<void*>(post_compact_end_)
          << " black_allocations_begin=" << static_cast<void*>(black_allocations_begin_);
      callback(oss);
      oss << " \nobject="
          << heap_->GetVerification()->DumpRAMAroundAddress(reinterpret_cast<uintptr_t>(ref), 128)
          << " \nklass(from)="
          << heap_->GetVerification()->DumpRAMAroundAddress(reinterpret_cast<uintptr_t>(klass), 128)
          << "spaces:\n";
      heap_->DumpSpaces(oss);
      LOG(FATAL) << oss.str();
    }
  }
}
void MarkCompact::CompactPage(mirror::Object* obj,
                              uint32_t offset,
                              uint8_t* addr,
                              bool needs_memset_zero) {
  DCHECK(moving_space_bitmap_->Test(obj) && live_words_bitmap_->Test(obj));
  DCHECK(live_words_bitmap_->Test(offset))
      << "obj=" << obj << " offset=" << offset << " addr=" << static_cast<void*>(addr)
      << " black_allocs_begin=" << static_cast<void*>(black_allocations_begin_)
      << " post_compact_addr=" << static_cast<void*>(post_compact_end_);
  uint8_t* const start_addr = addr;
  size_t stride_count = 0;
  uint8_t* last_stride = addr;
  uint32_t last_stride_begin = 0;
  auto verify_obj_callback = [&](std::ostream& os) {
    os << " stride_count=" << stride_count << " last_stride=" << static_cast<void*>(last_stride)
       << " offset=" << offset << " start_addr=" << static_cast<void*>(start_addr);
  };
  obj = GetFromSpaceAddr(obj);
  live_words_bitmap_->VisitLiveStrides(
      offset,
      black_allocations_begin_,
      kPageSize,
      [&addr, &last_stride, &stride_count, &last_stride_begin, verify_obj_callback, this](
          uint32_t stride_begin, size_t stride_size, bool )
          REQUIRES_SHARED(Locks::mutator_lock_) {
            const size_t stride_in_bytes = stride_size * kAlignment;
            DCHECK_LE(stride_in_bytes, kPageSize);
            last_stride_begin = stride_begin;
            DCHECK(IsAligned<kAlignment>(addr));
            memcpy(addr, from_space_begin_ + stride_begin * kAlignment, stride_in_bytes);
            if (kIsDebugBuild) {
              uint8_t* space_begin = bump_pointer_space_->Begin();
              if (stride_count > 0 || stride_begin * kAlignment < kPageSize) {
                mirror::Object* o =
                    reinterpret_cast<mirror::Object*>(space_begin + stride_begin * kAlignment);
                CHECK(live_words_bitmap_->Test(o)) << "ref=" << o;
                CHECK(moving_space_bitmap_->Test(o))
                    << "ref=" << o << " bitmap: " << moving_space_bitmap_->DumpMemAround(o);
                VerifyObject(reinterpret_cast<mirror::Object*>(addr), verify_obj_callback);
              }
            }
            last_stride = addr;
            addr += stride_in_bytes;
            stride_count++;
          });
  DCHECK_LT(last_stride, start_addr + kPageSize);
  DCHECK_GT(stride_count, 0u);
  size_t obj_size = 0;
  uint32_t offset_within_obj =
      offset * kAlignment - (reinterpret_cast<uint8_t*>(obj) - from_space_begin_);
  if (offset_within_obj > 0) {
    mirror::Object* to_ref = reinterpret_cast<mirror::Object*>(start_addr - offset_within_obj);
    if (stride_count > 1) {
      RefsUpdateVisitor< true, false> visitor(
          this, to_ref, start_addr, nullptr);
      obj_size = obj->VisitRefsForCompaction< true, false>(
          visitor, MemberOffset(offset_within_obj), MemberOffset(-1));
    } else {
      RefsUpdateVisitor< true, true> visitor(
          this, to_ref, start_addr, start_addr + kPageSize);
      obj_size = obj->VisitRefsForCompaction< true, false>(
          visitor, MemberOffset(offset_within_obj), MemberOffset(offset_within_obj + kPageSize));
    }
    obj_size = RoundUp(obj_size, kAlignment);
    DCHECK_GT(obj_size, offset_within_obj);
    obj_size -= offset_within_obj;
    if (stride_count == 1) {
      last_stride_begin += obj_size / kAlignment;
    }
  }
  uint8_t* const end_addr = addr;
  addr = start_addr;
  size_t bytes_done = obj_size;
  DCHECK_LE(addr, last_stride);
  size_t bytes_to_visit = last_stride - addr;
  DCHECK_LE(bytes_to_visit, kPageSize);
  while (bytes_to_visit > bytes_done) {
    mirror::Object* ref = reinterpret_cast<mirror::Object*>(addr + bytes_done);
    VerifyObject(ref, verify_obj_callback);
    RefsUpdateVisitor< false, false> visitor(
        this, ref, nullptr, nullptr);
    obj_size = ref->VisitRefsForCompaction(visitor, MemberOffset(0), MemberOffset(-1));
    obj_size = RoundUp(obj_size, kAlignment);
    bytes_done += obj_size;
  }
  uint8_t* from_addr = from_space_begin_ + last_stride_begin * kAlignment;
  bytes_to_visit = end_addr - addr;
  DCHECK_LE(bytes_to_visit, kPageSize);
  while (bytes_to_visit > bytes_done) {
    mirror::Object* ref = reinterpret_cast<mirror::Object*>(addr + bytes_done);
    obj = reinterpret_cast<mirror::Object*>(from_addr);
    VerifyObject(ref, verify_obj_callback);
    RefsUpdateVisitor< false, true> visitor(
        this, ref, nullptr, start_addr + kPageSize);
    obj_size = obj->VisitRefsForCompaction(
        visitor, MemberOffset(0), MemberOffset(end_addr - (addr + bytes_done)));
    obj_size = RoundUp(obj_size, kAlignment);
    from_addr += obj_size;
    bytes_done += obj_size;
  }
  if (needs_memset_zero && UNLIKELY(bytes_done < kPageSize)) {
    std::memset(addr + bytes_done, 0x0, kPageSize - bytes_done);
  }
}
void MarkCompact::SlideBlackPage(mirror::Object* first_obj,
                                 const size_t page_idx,
                                 uint8_t* const pre_compact_page,
                                 uint8_t* dest,
                                 bool needs_memset_zero) {
  DCHECK(IsAligned<kPageSize>(pre_compact_page));
  size_t bytes_copied;
  const uint32_t first_chunk_size = black_alloc_pages_first_chunk_size_[page_idx];
  mirror::Object* next_page_first_obj = first_objs_moving_space_[page_idx + 1].AsMirrorPtr();
  uint8_t* src_addr = reinterpret_cast<uint8_t*>(GetFromSpaceAddr(first_obj));
  uint8_t* pre_compact_addr = reinterpret_cast<uint8_t*>(first_obj);
  uint8_t* const pre_compact_page_end = pre_compact_page + kPageSize;
  uint8_t* const dest_page_end = dest + kPageSize;
  auto verify_obj_callback = [&](std::ostream& os) {
    os << " first_obj=" << first_obj << " next_page_first_obj=" << next_page_first_obj
       << " first_chunk_sie=" << first_chunk_size << " dest=" << static_cast<void*>(dest)
       << " pre_compact_page=" << static_cast<void* const>(pre_compact_page);
  };
  if (pre_compact_addr > pre_compact_page) {
    bytes_copied = pre_compact_addr - pre_compact_page;
    DCHECK_LT(bytes_copied, kPageSize);
    if (needs_memset_zero) {
      std::memset(dest, 0x0, bytes_copied);
    }
    dest += bytes_copied;
  } else {
    bytes_copied = 0;
    size_t offset = pre_compact_page - pre_compact_addr;
    pre_compact_addr = pre_compact_page;
    src_addr += offset;
    DCHECK(IsAligned<kPageSize>(src_addr));
  }
  std::memcpy(dest, src_addr, first_chunk_size);
  {
    size_t bytes_to_visit = first_chunk_size;
    size_t obj_size;
    DCHECK_LE(reinterpret_cast<uint8_t*>(first_obj), pre_compact_addr);
    size_t offset = pre_compact_addr - reinterpret_cast<uint8_t*>(first_obj);
    if (bytes_copied == 0 && offset > 0) {
      mirror::Object* to_obj = reinterpret_cast<mirror::Object*>(dest - offset);
      mirror::Object* from_obj = reinterpret_cast<mirror::Object*>(src_addr - offset);
      if (next_page_first_obj == nullptr ||
          (first_obj != next_page_first_obj &&
           reinterpret_cast<uint8_t*>(next_page_first_obj) <= pre_compact_page_end)) {
        RefsUpdateVisitor< true, false> visitor(
            this, to_obj, dest, nullptr);
        obj_size = from_obj->VisitRefsForCompaction<
                              true,
                                  false>(visitor, MemberOffset(offset), MemberOffset(-1));
      } else {
        RefsUpdateVisitor< true, true> visitor(
            this, to_obj, dest, dest_page_end);
        obj_size = from_obj->VisitRefsForCompaction<
                              true,
                                  false>(
            visitor, MemberOffset(offset), MemberOffset(offset + kPageSize));
        if (first_obj == next_page_first_obj) {
          return;
        }
      }
      obj_size = RoundUp(obj_size, kAlignment);
      obj_size -= offset;
      dest += obj_size;
      bytes_to_visit -= obj_size;
    }
    bytes_copied += first_chunk_size;
    bool check_last_obj = false;
    if (next_page_first_obj != nullptr &&
        reinterpret_cast<uint8_t*>(next_page_first_obj) < pre_compact_page_end &&
        bytes_copied == kPageSize) {
      size_t diff = pre_compact_page_end - reinterpret_cast<uint8_t*>(next_page_first_obj);
      DCHECK_LE(diff, kPageSize);
      DCHECK_LE(diff, bytes_to_visit);
      bytes_to_visit -= diff;
      check_last_obj = true;
    }
    while (bytes_to_visit > 0) {
      mirror::Object* dest_obj = reinterpret_cast<mirror::Object*>(dest);
      VerifyObject(dest_obj, verify_obj_callback);
      RefsUpdateVisitor< false, false> visitor(
          this, dest_obj, nullptr, nullptr);
      obj_size = dest_obj->VisitRefsForCompaction(visitor, MemberOffset(0), MemberOffset(-1));
      obj_size = RoundUp(obj_size, kAlignment);
      bytes_to_visit -= obj_size;
      dest += obj_size;
    }
    DCHECK_EQ(bytes_to_visit, 0u);
    if (check_last_obj) {
      mirror::Object* dest_obj = reinterpret_cast<mirror::Object*>(dest);
      VerifyObject(dest_obj, verify_obj_callback);
      RefsUpdateVisitor< false, true> visitor(
          this, dest_obj, nullptr, dest_page_end);
      mirror::Object* obj = GetFromSpaceAddr(next_page_first_obj);
      obj->VisitRefsForCompaction< false>(
          visitor, MemberOffset(0), MemberOffset(dest_page_end - dest));
      return;
    }
  }
  if (bytes_copied < kPageSize) {
    src_addr += first_chunk_size;
    pre_compact_addr += first_chunk_size;
    uintptr_t start_visit = reinterpret_cast<uintptr_t>(pre_compact_addr);
    uintptr_t page_end = reinterpret_cast<uintptr_t>(pre_compact_page_end);
    mirror::Object* found_obj = nullptr;
    moving_space_bitmap_->VisitMarkedRange< true>(
        start_visit, page_end, [&found_obj](mirror::Object* obj) { found_obj = obj; });
    size_t remaining_bytes = kPageSize - bytes_copied;
    if (found_obj == nullptr) {
      if (needs_memset_zero) {
        std::memset(dest, 0x0, remaining_bytes);
      }
      return;
    }
    std::memcpy(dest, src_addr, remaining_bytes);
    DCHECK_LT(reinterpret_cast<uintptr_t>(found_obj), page_end);
    moving_space_bitmap_->VisitMarkedRange(
        reinterpret_cast<uintptr_t>(found_obj) + mirror::kObjectHeaderSize,
        page_end,
        [&found_obj, pre_compact_addr, dest, this, verify_obj_callback](mirror::Object* obj)
            REQUIRES_SHARED(Locks::mutator_lock_) {
              ptrdiff_t diff = reinterpret_cast<uint8_t*>(found_obj) - pre_compact_addr;
              mirror::Object* ref = reinterpret_cast<mirror::Object*>(dest + diff);
              VerifyObject(ref, verify_obj_callback);
              RefsUpdateVisitor< false, false> visitor(
                  this, ref, nullptr, nullptr);
              ref->VisitRefsForCompaction< false>(
                  visitor, MemberOffset(0), MemberOffset(-1));
              found_obj = obj;
            });
    DCHECK_GT(reinterpret_cast<uint8_t*>(found_obj), pre_compact_addr);
    DCHECK_LT(reinterpret_cast<uintptr_t>(found_obj), page_end);
    ptrdiff_t diff = reinterpret_cast<uint8_t*>(found_obj) - pre_compact_addr;
    mirror::Object* ref = reinterpret_cast<mirror::Object*>(dest + diff);
    VerifyObject(ref, verify_obj_callback);
    RefsUpdateVisitor< false, true> visitor(
        this, ref, nullptr, dest_page_end);
    ref->VisitRefsForCompaction< false>(
        visitor, MemberOffset(0), MemberOffset(page_end - reinterpret_cast<uintptr_t>(found_obj)));
  }
}
template <bool kFirstPageMapping>
void MarkCompact::MapProcessedPages(uint8_t* to_space_start,
                                    Atomic<PageState>* state_arr,
                                    size_t arr_idx,
                                    size_t arr_len) {
  DCHECK(minor_fault_initialized_);
  DCHECK_LT(arr_idx, arr_len);
  DCHECK_ALIGNED(to_space_start, kPageSize);
  size_t length = kFirstPageMapping ? kPageSize : 0;
  if (kFirstPageMapping) {
    arr_idx++;
  }
  for (; arr_idx < arr_len; arr_idx++, length += kPageSize) {
    PageState expected_state = PageState::kProcessed;
    if (!state_arr[arr_idx].compare_exchange_strong(
            expected_state, PageState::kProcessedAndMapping, std::memory_order_acq_rel)) {
      break;
    }
  }
  if (length > 0) {
    struct uffdio_continue uffd_continue;
    uffd_continue.range.start = reinterpret_cast<uintptr_t>(to_space_start);
    uffd_continue.range.len = length;
    uffd_continue.mode = 0;
    int ret = ioctl(uffd_, UFFDIO_CONTINUE, &uffd_continue);
    if (UNLIKELY(ret == -1 && errno == EAGAIN)) {
      DCHECK(linear_alloc_spaces_data_.end() !=
             std::find_if(linear_alloc_spaces_data_.begin(),
                          linear_alloc_spaces_data_.end(),
                          [to_space_start](const LinearAllocSpaceData& data) {
                            return data.begin_ <= to_space_start && to_space_start < data.end_;
                          }));
      DCHECK_GE(uffd_continue.mapped, 0);
      DCHECK_ALIGNED(uffd_continue.mapped, kPageSize);
      DCHECK_LT(uffd_continue.mapped, static_cast<ssize_t>(length));
      if (kFirstPageMapping) {
        DCHECK_GE(uffd_continue.mapped, static_cast<ssize_t>(kPageSize));
      }
      for (size_t remaining_len = length - uffd_continue.mapped; remaining_len > 0;
           remaining_len -= kPageSize) {
        arr_idx--;
        DCHECK_EQ(state_arr[arr_idx].load(std::memory_order_relaxed),
                  PageState::kProcessedAndMapping);
        state_arr[arr_idx].store(PageState::kProcessed, std::memory_order_release);
      }
      uffd_continue.range.start =
          reinterpret_cast<uintptr_t>(to_space_start) + uffd_continue.mapped;
      uffd_continue.range.len = length - uffd_continue.mapped;
      ret = ioctl(uffd_, UFFDIO_WAKE, &uffd_continue.range);
      CHECK_EQ(ret, 0) << "ioctl_userfaultfd: wake failed: " << strerror(errno);
    } else {
      CHECK(ret == 0 || !kFirstPageMapping || errno == ENOENT)
          << "ioctl_userfaultfd: continue failed: " << strerror(errno);
      if (ret == 0) {
        DCHECK_EQ(uffd_continue.mapped, static_cast<ssize_t>(length));
      }
    }
    if (use_uffd_sigbus_) {
      for (; uffd_continue.mapped > 0; uffd_continue.mapped -= kPageSize) {
        arr_idx--;
        DCHECK_EQ(state_arr[arr_idx].load(std::memory_order_relaxed),
                  PageState::kProcessedAndMapping);
        state_arr[arr_idx].store(PageState::kProcessedAndMapped, std::memory_order_release);
      }
    }
  }
}
void MarkCompact::ZeropageIoctl(void* addr, bool tolerate_eexist, bool tolerate_enoent) {
  struct uffdio_zeropage uffd_zeropage;
  DCHECK(IsAligned<kPageSize>(addr));
  uffd_zeropage.range.start = reinterpret_cast<uintptr_t>(addr);
  uffd_zeropage.range.len = kPageSize;
  uffd_zeropage.mode = 0;
  int ret = ioctl(uffd_, UFFDIO_ZEROPAGE, &uffd_zeropage);
  if (LIKELY(ret == 0)) {
    DCHECK_EQ(uffd_zeropage.zeropage, static_cast<ssize_t>(kPageSize));
  } else {
    CHECK((tolerate_enoent && errno == ENOENT) || (tolerate_eexist && errno == EEXIST))
        << "ioctl_userfaultfd: zeropage failed: " << strerror(errno) << ". addr:" << addr;
  }
}
void MarkCompact::CopyIoctl(void* dst, void* buffer) {
  struct uffdio_copy uffd_copy;
  uffd_copy.src = reinterpret_cast<uintptr_t>(buffer);
  uffd_copy.dst = reinterpret_cast<uintptr_t>(dst);
  uffd_copy.len = kPageSize;
  uffd_copy.mode = 0;
  CHECK_EQ(ioctl(uffd_, UFFDIO_COPY, &uffd_copy), 0)
      << "ioctl_userfaultfd: copy failed: " << strerror(errno) << ". src:" << buffer
      << " dst:" << dst;
  DCHECK_EQ(uffd_copy.copy, static_cast<ssize_t>(kPageSize));
}
template <int kMode, typename CompactionFn>
void MarkCompact::DoPageCompactionWithStateChange(size_t page_idx,
                                                  size_t status_arr_len,
                                                  uint8_t* to_space_page,
                                                  uint8_t* page,
                                                  CompactionFn func) {
  PageState expected_state = PageState::kUnprocessed;
  PageState desired_state =
      kMode == kCopyMode ? PageState::kProcessingAndMapping : PageState::kProcessing;
  if (kMode == kFallbackMode || moving_pages_status_[page_idx].compare_exchange_strong(
                                    expected_state, desired_state, std::memory_order_acquire)) {
    func();
    if (kMode == kCopyMode) {
      CopyIoctl(to_space_page, page);
      if (use_uffd_sigbus_) {
        moving_pages_status_[page_idx].store(PageState::kProcessedAndMapped,
                                             std::memory_order_release);
      }
    } else if (kMode == kMinorFaultMode) {
      expected_state = PageState::kProcessing;
      desired_state = PageState::kProcessed;
      if (!moving_pages_status_[page_idx].compare_exchange_strong(
              expected_state, desired_state, std::memory_order_release)) {
        DCHECK_EQ(expected_state, PageState::kProcessingAndMapping);
        MapProcessedPages< true>(
            to_space_page, moving_pages_status_, page_idx, status_arr_len);
      }
    }
  } else {
    DCHECK_GT(expected_state, PageState::kProcessed);
  }
}
void MarkCompact::FreeFromSpacePages(size_t cur_page_idx) {
  size_t idx = last_checked_reclaim_page_idx_;
  for (; idx > cur_page_idx; idx--) {
    PageState state = moving_pages_status_[idx - 1].load(std::memory_order_acquire);
    if (state == PageState::kMutatorProcessing) {
      break;
    }
    DCHECK(state >= PageState::kProcessed ||
           (state == PageState::kUnprocessed && idx > moving_first_objs_count_));
  }
  uint8_t* reclaim_begin;
  uint8_t* idx_addr;
  if (idx >= moving_first_objs_count_) {
    idx_addr = black_allocations_begin_ + (idx - moving_first_objs_count_) * kPageSize;
    reclaim_begin = idx_addr;
    mirror::Object* first_obj = first_objs_moving_space_[idx].AsMirrorPtr();
    if (first_obj != nullptr && reinterpret_cast<uint8_t*>(first_obj) < reclaim_begin) {
      size_t idx_len = moving_first_objs_count_ + black_page_count_;
      for (size_t i = idx + 1; i < idx_len; i++) {
        mirror::Object* obj = first_objs_moving_space_[i].AsMirrorPtr();
        if (obj != first_obj) {
          reclaim_begin =
              obj != nullptr ?
                  AlignUp(reinterpret_cast<uint8_t*>(obj), kPageSize) :
                  (black_allocations_begin_ + (i - moving_first_objs_count_) * kPageSize);
          break;
        }
      }
    }
  } else {
    DCHECK_GE(pre_compact_offset_moving_space_[idx], 0u);
    idx_addr = bump_pointer_space_->Begin() + pre_compact_offset_moving_space_[idx] * kAlignment;
    reclaim_begin = idx_addr;
    DCHECK_LE(reclaim_begin, black_allocations_begin_);
    mirror::Object* first_obj = first_objs_moving_space_[idx].AsMirrorPtr();
    if (reinterpret_cast<uint8_t*>(first_obj) < reclaim_begin) {
      DCHECK_LT(idx, moving_first_objs_count_);
      mirror::Object* obj = first_obj;
      for (size_t i = idx + 1; i < moving_first_objs_count_; i++) {
        obj = first_objs_moving_space_[i].AsMirrorPtr();
        if (first_obj != obj) {
          DCHECK_LT(first_obj, obj);
          DCHECK_LT(reclaim_begin, reinterpret_cast<uint8_t*>(obj));
          reclaim_begin = reinterpret_cast<uint8_t*>(obj);
          break;
        }
      }
      if (obj == first_obj) {
        reclaim_begin = black_allocations_begin_;
      }
    }
    reclaim_begin = AlignUp(reclaim_begin, kPageSize);
  }
  DCHECK_NE(reclaim_begin, nullptr);
  DCHECK_ALIGNED(reclaim_begin, kPageSize);
  DCHECK_ALIGNED(last_reclaimed_page_, kPageSize);
  for (; class_after_obj_iter_ != class_after_obj_ordered_map_.rend(); class_after_obj_iter_++) {
    mirror::Object* klass = class_after_obj_iter_->first.AsMirrorPtr();
    mirror::Class* from_klass = static_cast<mirror::Class*>(GetFromSpaceAddr(klass));
    uint8_t* klass_end = reinterpret_cast<uint8_t*>(klass) + from_klass->SizeOf<kVerifyNone>();
    DCHECK_LE(klass_end, last_reclaimed_page_);
    if (reinterpret_cast<uint8_t*>(klass_end) >= reclaim_begin) {
      uint8_t* obj_addr = reinterpret_cast<uint8_t*>(class_after_obj_iter_->second.AsMirrorPtr());
      if (obj_addr < idx_addr) {
        reclaim_begin = AlignUp(klass_end, kPageSize);
      } else {
        continue;
      }
    }
    break;
  }
  ssize_t size = last_reclaimed_page_ - reclaim_begin;
  if (size >= kMinFromSpaceMadviseSize) {
    int behavior = minor_fault_initialized_ ? MADV_REMOVE : MADV_DONTNEED;
    CHECK_EQ(madvise(reclaim_begin + from_space_slide_diff_, size, behavior), 0)
        << "madvise of from-space failed: " << strerror(errno);
    last_reclaimed_page_ = reclaim_begin;
  }
  last_checked_reclaim_page_idx_ = idx;
}
void MarkCompact::UpdateClassAfterObjMap() {
  CHECK(class_after_obj_ordered_map_.empty());
  for (const auto& pair : class_after_obj_hash_map_) {
    auto super_class_iter = super_class_after_class_hash_map_.find(pair.first);
    ObjReference key = super_class_iter != super_class_after_class_hash_map_.end() ?
                           super_class_iter->second :
                           pair.first;
    if (std::less<mirror::Object*>{}(pair.second.AsMirrorPtr(), key.AsMirrorPtr()) &&
        bump_pointer_space_->HasAddress(key.AsMirrorPtr())) {
      auto [ret_iter, success] = class_after_obj_ordered_map_.try_emplace(key, pair.second);
      if (!success &&
          std::less<mirror::Object*>{}(pair.second.AsMirrorPtr(), ret_iter->second.AsMirrorPtr())) {
        ret_iter->second = pair.second;
      }
    }
  }
  class_after_obj_hash_map_.clear();
  super_class_after_class_hash_map_.clear();
}
template <int kMode>
void MarkCompact::CompactMovingSpace(uint8_t* page) {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  size_t page_status_arr_len = moving_first_objs_count_ + black_page_count_;
  size_t idx = page_status_arr_len;
  uint8_t* to_space_end = bump_pointer_space_->Begin() + page_status_arr_len * kPageSize;
  uint8_t* shadow_space_end = nullptr;
  if (kMode == kMinorFaultMode) {
    shadow_space_end = shadow_to_space_map_.Begin() + page_status_arr_len * kPageSize;
  }
  uint8_t* pre_compact_page = black_allocations_begin_ + (black_page_count_ * kPageSize);
  DCHECK(IsAligned<kPageSize>(pre_compact_page));
  UpdateClassAfterObjMap();
  last_reclaimed_page_ = pre_compact_page;
  last_checked_reclaim_page_idx_ = idx;
  class_after_obj_iter_ = class_after_obj_ordered_map_.rbegin();
  while (idx > moving_first_objs_count_) {
    idx--;
    pre_compact_page -= kPageSize;
    to_space_end -= kPageSize;
    if (kMode == kMinorFaultMode) {
      shadow_space_end -= kPageSize;
      page = shadow_space_end;
    } else if (kMode == kFallbackMode) {
      page = to_space_end;
    }
    mirror::Object* first_obj = first_objs_moving_space_[idx].AsMirrorPtr();
    if (first_obj != nullptr) {
      DoPageCompactionWithStateChange<kMode>(
          idx,
          page_status_arr_len,
          to_space_end,
          page,
          [&]() REQUIRES_SHARED(Locks::mutator_lock_) {
            SlideBlackPage(first_obj, idx, pre_compact_page, page, kMode == kCopyMode);
          });
      if (idx % (kMinFromSpaceMadviseSize / kPageSize) == 0) {
        FreeFromSpacePages(idx);
      }
    }
  }
  DCHECK_EQ(pre_compact_page, black_allocations_begin_);
  while (idx > 0) {
    idx--;
    to_space_end -= kPageSize;
    if (kMode == kMinorFaultMode) {
      shadow_space_end -= kPageSize;
      page = shadow_space_end;
    } else if (kMode == kFallbackMode) {
      page = to_space_end;
    }
    mirror::Object* first_obj = first_objs_moving_space_[idx].AsMirrorPtr();
    DoPageCompactionWithStateChange<kMode>(
        idx, page_status_arr_len, to_space_end, page, [&]() REQUIRES_SHARED(Locks::mutator_lock_) {
          CompactPage(first_obj, pre_compact_offset_moving_space_[idx], page, kMode == kCopyMode);
        });
    FreeFromSpacePages(idx);
  }
  DCHECK_EQ(to_space_end, bump_pointer_space_->Begin());
}
void MarkCompact::UpdateNonMovingPage(mirror::Object* first, uint8_t* page) {
  DCHECK_LT(reinterpret_cast<uint8_t*>(first), page + kPageSize);
  mirror::Object* curr_obj = first;
  non_moving_space_bitmap_->VisitMarkedRange(
      reinterpret_cast<uintptr_t>(first) + mirror::kObjectHeaderSize,
      reinterpret_cast<uintptr_t>(page + kPageSize),
      [&](mirror::Object* next_obj) {
        if (reinterpret_cast<uint8_t*>(curr_obj) < page) {
          RefsUpdateVisitor< true, false> visitor(
              this, curr_obj, page, page + kPageSize);
          MemberOffset begin_offset(page - reinterpret_cast<uint8_t*>(curr_obj));
          curr_obj->VisitRefsForCompaction< false, false>(
              visitor, begin_offset, MemberOffset(-1));
        } else {
          RefsUpdateVisitor< false, false> visitor(
              this, curr_obj, page, page + kPageSize);
          curr_obj->VisitRefsForCompaction< false>(
              visitor, MemberOffset(0), MemberOffset(-1));
        }
        curr_obj = next_obj;
      });
  MemberOffset end_offset(page + kPageSize - reinterpret_cast<uint8_t*>(curr_obj));
  if (reinterpret_cast<uint8_t*>(curr_obj) < page) {
    RefsUpdateVisitor< true, true> visitor(
        this, curr_obj, page, page + kPageSize);
    curr_obj->VisitRefsForCompaction< false, false>(
        visitor, MemberOffset(page - reinterpret_cast<uint8_t*>(curr_obj)), end_offset);
  } else {
    RefsUpdateVisitor< false, true> visitor(
        this, curr_obj, page, page + kPageSize);
    curr_obj->VisitRefsForCompaction< false>(visitor, MemberOffset(0), end_offset);
  }
}
void MarkCompact::UpdateNonMovingSpace() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  uint8_t* page = non_moving_space_->Begin() + non_moving_first_objs_count_ * kPageSize;
  for (ssize_t i = non_moving_first_objs_count_ - 1; i >= 0; i--) {
    mirror::Object* obj = first_objs_non_moving_space_[i].AsMirrorPtr();
    page -= kPageSize;
    if (obj != nullptr) {
      UpdateNonMovingPage(obj, page);
    }
  }
}
void MarkCompact::UpdateMovingSpaceBlackAllocations() {
  uint8_t* const begin = bump_pointer_space_->Begin();
  uint8_t* black_allocs = black_allocations_begin_;
  DCHECK_LE(begin, black_allocs);
  size_t consumed_blocks_count = 0;
  size_t first_block_size;
  std::vector<size_t>* block_sizes =
      bump_pointer_space_->GetBlockSizes(thread_running_gc_, &first_block_size);
  DCHECK_LE(first_block_size, (size_t)(black_allocs - begin));
  if (block_sizes != nullptr) {
    size_t black_page_idx = moving_first_objs_count_;
    uint8_t* block_end = begin + first_block_size;
    uint32_t remaining_chunk_size = 0;
    uint32_t first_chunk_size = 0;
    mirror::Object* first_obj = nullptr;
    for (size_t block_size : *block_sizes) {
      block_end += block_size;
      if (black_allocs >= block_end) {
        consumed_blocks_count++;
        continue;
      }
      mirror::Object* obj = reinterpret_cast<mirror::Object*>(black_allocs);
      bool set_mark_bit = remaining_chunk_size > 0;
      while (black_allocs < block_end &&
             obj->GetClass<kDefaultVerifyFlags, kWithoutReadBarrier>() != nullptr) {
        size_t obj_size = RoundUp(obj->SizeOf(), kAlignment);
        UpdateClassAfterObjectMap(obj);
        if (first_obj == nullptr) {
          first_obj = obj;
        }
        if (set_mark_bit) {
          moving_space_bitmap_->Set(obj);
        }
        if (remaining_chunk_size + obj_size >= kPageSize) {
          set_mark_bit = false;
          first_chunk_size += kPageSize - remaining_chunk_size;
          remaining_chunk_size += obj_size;
          if (black_alloc_pages_first_chunk_size_[black_page_idx] == 0) {
            black_alloc_pages_first_chunk_size_[black_page_idx] = first_chunk_size;
            first_objs_moving_space_[black_page_idx].Assign(first_obj);
          }
          black_page_idx++;
          remaining_chunk_size -= kPageSize;
          while (remaining_chunk_size >= kPageSize) {
            black_alloc_pages_first_chunk_size_[black_page_idx] = kPageSize;
            first_objs_moving_space_[black_page_idx].Assign(obj);
            black_page_idx++;
            remaining_chunk_size -= kPageSize;
          }
          first_obj = remaining_chunk_size > 0 ? obj : nullptr;
          first_chunk_size = remaining_chunk_size;
        } else {
          DCHECK_LE(first_chunk_size, remaining_chunk_size);
          first_chunk_size += obj_size;
          remaining_chunk_size += obj_size;
        }
        black_allocs += obj_size;
        obj = reinterpret_cast<mirror::Object*>(black_allocs);
      }
      DCHECK_LE(black_allocs, block_end);
      DCHECK_LT(remaining_chunk_size, kPageSize);
      if (black_allocs < block_end) {
        if (first_chunk_size > 0 && black_alloc_pages_first_chunk_size_[black_page_idx] == 0) {
          black_alloc_pages_first_chunk_size_[black_page_idx] = first_chunk_size;
          first_objs_moving_space_[black_page_idx].Assign(first_obj);
        }
        first_chunk_size = 0;
        first_obj = nullptr;
        size_t page_remaining = kPageSize - remaining_chunk_size;
        size_t block_remaining = block_end - black_allocs;
        if (page_remaining <= block_remaining) {
          block_remaining -= page_remaining;
          black_page_idx += 1 + block_remaining / kPageSize;
          remaining_chunk_size = block_remaining % kPageSize;
        } else {
          remaining_chunk_size += block_remaining;
        }
        black_allocs = block_end;
      }
    }
    if (black_alloc_pages_first_chunk_size_[black_page_idx] > 0) {
      black_page_idx++;
    } else if (first_chunk_size > 0) {
      black_alloc_pages_first_chunk_size_[black_page_idx] = first_chunk_size;
      first_objs_moving_space_[black_page_idx].Assign(first_obj);
      black_page_idx++;
    }
    black_page_count_ = black_page_idx - moving_first_objs_count_;
    delete block_sizes;
  }
  bump_pointer_space_->SetBlockSizes(
      thread_running_gc_, post_compact_end_ - begin, consumed_blocks_count);
}
void MarkCompact::UpdateNonMovingSpaceBlackAllocations() {
  accounting::ObjectStack* stack = heap_->GetAllocationStack();
  const StackReference<mirror::Object>* limit = stack->End();
  uint8_t* const space_begin = non_moving_space_->Begin();
  for (StackReference<mirror::Object>* it = stack->Begin(); it != limit; ++it) {
    mirror::Object* obj = it->AsMirrorPtr();
    if (obj != nullptr && non_moving_space_bitmap_->HasAddress(obj)) {
      non_moving_space_bitmap_->Set(obj);
      it->Clear();
      size_t idx = (reinterpret_cast<uint8_t*>(obj) - space_begin) / kPageSize;
      uint8_t* page_begin = AlignDown(reinterpret_cast<uint8_t*>(obj), kPageSize);
      mirror::Object* first_obj = first_objs_non_moving_space_[idx].AsMirrorPtr();
      if (first_obj == nullptr ||
          (obj<first_obj&& reinterpret_cast<uint8_t*>(first_obj)> page_begin)) {
        first_objs_non_moving_space_[idx].Assign(obj);
      }
      mirror::Object* next_page_first_obj = first_objs_non_moving_space_[++idx].AsMirrorPtr();
      uint8_t* next_page_begin = page_begin + kPageSize;
      if (next_page_first_obj == nullptr ||
          reinterpret_cast<uint8_t*>(next_page_first_obj) > next_page_begin) {
        size_t obj_size = RoundUp(obj->SizeOf<kDefaultVerifyFlags>(), kAlignment);
        uint8_t* obj_end = reinterpret_cast<uint8_t*>(obj) + obj_size;
        while (next_page_begin < obj_end) {
          first_objs_non_moving_space_[idx++].Assign(obj);
          next_page_begin += kPageSize;
        }
      }
      non_moving_first_objs_count_ = std::max(non_moving_first_objs_count_, idx);
    }
  }
}
class MarkCompact::ImmuneSpaceUpdateObjVisitor {
 public:
  ImmuneSpaceUpdateObjVisitor(MarkCompact* collector, bool visit_native_roots)
      : collector_(collector), visit_native_roots_(visit_native_roots) {}
  ALWAYS_INLINE void operator()(mirror::Object* obj) const REQUIRES(Locks::mutator_lock_) {
    RefsUpdateVisitor< false, false> visitor(collector_,
                                                                          obj,
                                                                                     nullptr,
                                                                                   nullptr);
    if (visit_native_roots_) {
      obj->VisitRefsForCompaction< false, true>(
          visitor, MemberOffset(0), MemberOffset(-1));
    } else {
      obj->VisitRefsForCompaction< false>(
          visitor, MemberOffset(0), MemberOffset(-1));
    }
  }
  static void Callback(mirror::Object* obj, void* arg) REQUIRES(Locks::mutator_lock_) {
    reinterpret_cast<ImmuneSpaceUpdateObjVisitor*>(arg)->operator()(obj);
  }
 private:
  MarkCompact* const collector_;
  const bool visit_native_roots_;
};
class MarkCompact::ClassLoaderRootsUpdater : public ClassLoaderVisitor {
 public:
  explicit ClassLoaderRootsUpdater(MarkCompact* collector) : collector_(collector) {}
  void Visit(ObjPtr<mirror::ClassLoader> class_loader) override
      REQUIRES_SHARED(Locks::classlinker_classes_lock_, Locks::mutator_lock_) {
    ClassTable* const class_table = class_loader->GetClassTable();
    if (class_table != nullptr) {
      class_table->VisitRoots(*this);
    }
  }
  REQUIRES(Locks::heap_bitmap_lock_)
  REQUIRES(Locks::heap_bitmap_lock_) REQUIRES_SHARED(Locks::mutator_lock_) {
    if (!root->IsNull()) {
      VisitRoot(root);
    }
  }
  REQUIRES(Locks::heap_bitmap_lock_)
  REQUIRES(Locks::heap_bitmap_lock_) REQUIRES_SHARED(Locks::mutator_lock_) {
    collector_->VisitRoots(&root, 1, RootInfo(RootType::kRootVMInternal));
  }
 private:
  MarkCompact* collector_;
};
class MarkCompact::LinearAllocPageUpdater {
 public:
  explicit LinearAllocPageUpdater(MarkCompact* collector) : collector_(collector) {}
  void operator()(uint8_t* page_begin, uint8_t* first_obj) ALWAYS_INLINE
      REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK_ALIGNED(page_begin, kPageSize);
    uint8_t* page_end = page_begin + kPageSize;
    uint32_t obj_size;
    for (uint8_t* byte = first_obj; byte < page_end;) {
      TrackingHeader* header = reinterpret_cast<TrackingHeader*>(byte);
      obj_size = header->GetSize();
      if (UNLIKELY(obj_size == 0)) {
        last_page_touched_ = byte >= page_begin;
        return;
      }
      uint8_t* obj = byte + sizeof(TrackingHeader);
      uint8_t* obj_end = byte + obj_size;
      if (header->Is16Aligned()) {
        obj = AlignUp(obj, 16);
      }
      uint8_t* begin_boundary = std::max(obj, page_begin);
      uint8_t* end_boundary = std::min(obj_end, page_end);
      if (begin_boundary < end_boundary) {
        VisitObject(header->GetKind(), obj, begin_boundary, end_boundary);
      }
      if (ArenaAllocator::IsRunningOnMemoryTool()) {
        obj_size += ArenaAllocator::kMemoryToolRedZoneBytes;
      }
      byte += RoundUp(obj_size, LinearAlloc::kAlignment);
    }
    last_page_touched_ = true;
  }
 private:
  class LinearAllocPageUpdater;
  bool last_page_touched_;
 public:
  ALWAYS_INLINE REQUIRES_SHARED(Locks::mutator_lock_) {
    mirror::Object* old_ref = root->AsMirrorPtr();
    DCHECK_NE(old_ref, nullptr);
    if (collector_->live_words_bitmap_->HasAddress(old_ref)) {
      mirror::Object* new_ref = old_ref;
      if (reinterpret_cast<uint8_t*>(old_ref) >= collector_->black_allocations_begin_) {
        new_ref = collector_->PostCompactBlackObjAddr(old_ref);
      } else if (collector_->live_words_bitmap_->Test(old_ref)) {
        DCHECK(collector_->moving_space_bitmap_->Test(old_ref)) << old_ref;
        new_ref = collector_->PostCompactOldObjAddr(old_ref);
      }
      if (old_ref != new_ref) {
        root->Assign(new_ref);
      }
    }
  }
 private:
  class LinearAllocPageUpdater;
 public:
  ALWAYS_INLINE REQUIRES_SHARED(Locks::mutator_lock_) {
    mirror::Object* old_ref = root->AsMirrorPtr();
    DCHECK_NE(old_ref, nullptr);
    if (collector_->live_words_bitmap_->HasAddress(old_ref)) {
      mirror::Object* new_ref = old_ref;
      if (reinterpret_cast<uint8_t*>(old_ref) >= collector_->black_allocations_begin_) {
        new_ref = collector_->PostCompactBlackObjAddr(old_ref);
      } else if (collector_->live_words_bitmap_->Test(old_ref)) {
        DCHECK(collector_->moving_space_bitmap_->Test(old_ref)) << old_ref;
        new_ref = collector_->PostCompactOldObjAddr(old_ref);
      }
      if (old_ref != new_ref) {
        root->Assign(new_ref);
      }
    }
  }
 private:
  class LinearAllocPageUpdater;
  REQUIRES_SHARED(Locks::mutator_lock_) {
    switch (kind) {
      case LinearAllocKind::kNoGCRoots:
        break;
      case LinearAllocKind::kGCRootArray: {
        GcRoot<mirror::Object>* root = reinterpret_cast<GcRoot<mirror::Object>*>(start_boundary);
        GcRoot<mirror::Object>* last = reinterpret_cast<GcRoot<mirror::Object>*>(end_boundary);
        for (; root < last; root++) {
          VisitRootIfNonNull(root->AddressWithoutBarrier());
        }
      } break;
      case LinearAllocKind::kArtMethodArray: {
        LengthPrefixedArray<ArtMethod>* array = static_cast<LengthPrefixedArray<ArtMethod>*>(obj);
        if (array->size() > 0) {
          if (collector_->pointer_size_ == PointerSize::k64) {
            ArtMethod::VisitArrayRoots<PointerSize::k64>(
                *this, start_boundary, end_boundary, array);
          } else {
            DCHECK_EQ(collector_->pointer_size_, PointerSize::k32);
            ArtMethod::VisitArrayRoots<PointerSize::k32>(
                *this, start_boundary, end_boundary, array);
          }
        }
      } break;
      case LinearAllocKind::kArtMethod:
        ArtMethod::VisitRoots(*this, start_boundary, end_boundary, static_cast<ArtMethod*>(obj));
        break;
      case LinearAllocKind::kArtFieldArray:
        ArtField::VisitArrayRoots(
            *this, start_boundary, end_boundary, static_cast<LengthPrefixedArray<ArtField>*>(obj));
        break;
      case LinearAllocKind::kDexCacheArray: {
        mirror::DexCachePair<mirror::Object>* first =
            reinterpret_cast<mirror::DexCachePair<mirror::Object>*>(start_boundary);
        mirror::DexCachePair<mirror::Object>* last =
            reinterpret_cast<mirror::DexCachePair<mirror::Object>*>(end_boundary);
        mirror::DexCache::VisitDexCachePairRoots(*this, first, last);
      }
    }
  }
  class LinearAllocPageUpdater;
};
void MarkCompact::CompactionPause() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  Runtime* runtime = Runtime::Current();
  non_moving_space_bitmap_ = non_moving_space_->GetLiveBitmap();
  if (kIsDebugBuild) {
    DCHECK_EQ(thread_running_gc_, Thread::Current());
    stack_low_addr_ = thread_running_gc_->GetStackEnd();
    stack_high_addr_ =
        reinterpret_cast<char*>(stack_low_addr_) + thread_running_gc_->GetStackSize();
  }
  {
    TimingLogger::ScopedTiming t2("(Paused)UpdateCompactionDataStructures", GetTimings());
    ReaderMutexLock rmu(thread_running_gc_, *Locks::heap_bitmap_lock_);
    UpdateMovingSpaceBlackAllocations();
    moving_pages_status_ = new Atomic<PageState>[moving_first_objs_count_ + black_page_count_];
    if (kIsDebugBuild) {
      size_t len = moving_first_objs_count_ + black_page_count_;
      for (size_t i = 0; i < len; i++) {
        CHECK_EQ(moving_pages_status_[i].load(std::memory_order_relaxed), PageState::kUnprocessed);
      }
    }
    UpdateNonMovingSpaceBlackAllocations();
    compacting_ = true;
    heap_->GetReferenceProcessor()->UpdateRoots(this);
  }
  {
    TimingLogger::ScopedTiming t2("(Paused)UpdateClassLoaderRoots", GetTimings());
    ReaderMutexLock rmu(thread_running_gc_, *Locks::classlinker_classes_lock_);
    {
      ClassLoaderRootsUpdater updater(this);
      runtime->GetClassLinker()->VisitClassLoaders(&updater);
    }
  }
  bool has_zygote_space = heap_->HasZygoteSpace();
  GcVisitedArenaPool* arena_pool =
      static_cast<GcVisitedArenaPool*>(runtime->GetLinearAllocArenaPool());
  if (uffd_ == kFallbackMode || (!has_zygote_space && runtime->IsZygote())) {
    if (kIsDebugBuild && IsValidFd(uffd_)) {
      arena_pool->ForEachAllocatedArena(
          [](const TrackedArena& arena)
              REQUIRES_SHARED(Locks::mutator_lock_) { CHECK(arena.IsPreZygoteForkArena()); });
    }
    LinearAllocPageUpdater updater(this);
    arena_pool->VisitRoots(updater);
  } else {
    arena_pool->ForEachAllocatedArena(
        [this](const TrackedArena& arena) REQUIRES_SHARED(Locks::mutator_lock_) {
          if (!arena.IsPreZygoteForkArena()) {
            uint8_t* last_byte = arena.GetLastUsedByte();
            CHECK(linear_alloc_arenas_.insert({&arena, last_byte}).second);
          } else {
            LinearAllocPageUpdater updater(this);
            arena.VisitRoots(updater);
          }
        });
  }
  SweepSystemWeaks(thread_running_gc_, runtime, true);
  {
    TimingLogger::ScopedTiming t2("(Paused)UpdateConcurrentRoots", GetTimings());
    runtime->VisitConcurrentRoots(this, kVisitRootFlagAllRoots);
  }
  {
    TimingLogger::ScopedTiming t2("(Paused)UpdateNonThreadRoots", GetTimings());
    runtime->VisitNonThreadRoots(this);
  }
  {
    TimingLogger::ScopedTiming t2("(Paused)UpdateImmuneSpaces", GetTimings());
    accounting::CardTable* const card_table = heap_->GetCardTable();
    for (auto& space : immune_spaces_.GetSpaces()) {
      DCHECK(space->IsImageSpace() || space->IsZygoteSpace());
      accounting::ContinuousSpaceBitmap* live_bitmap = space->GetLiveBitmap();
      accounting::ModUnionTable* table = heap_->FindModUnionTableFromSpace(space);
      ImmuneSpaceUpdateObjVisitor visitor(this, false);
      if (table != nullptr) {
        table->ProcessCards();
        table->VisitObjects(ImmuneSpaceUpdateObjVisitor::Callback, &visitor);
      } else {
        WriterMutexLock wmu(thread_running_gc_, *Locks::heap_bitmap_lock_);
        card_table->Scan<false>(live_bitmap,
                                space->Begin(),
                                space->Limit(),
                                visitor,
                                accounting::CardTable::kCardDirty - 1);
      }
    }
  }
  if (use_uffd_sigbus_) {
    sigbus_in_progress_count_.store(0, std::memory_order_release);
  }
  KernelPreparation();
  UpdateNonMovingSpace();
  if (uffd_ == kFallbackMode) {
    CompactMovingSpace<kFallbackMode>(nullptr);
    int32_t freed_bytes = black_objs_slide_diff_;
    bump_pointer_space_->RecordFree(freed_objects_, freed_bytes);
    RecordFree(ObjectBytePair(freed_objects_, freed_bytes));
  } else {
    DCHECK_EQ(compaction_in_progress_count_.load(std::memory_order_relaxed), 0u);
    if (!use_uffd_sigbus_) {
      heap_->GetThreadPool()->StartWorkers(thread_running_gc_);
    }
  }
  stack_low_addr_ = nullptr;
}
void MarkCompact::KernelPrepareRangeForUffd(
    uint8_t* to_addr, uint8_t* from_addr, size_t map_size, int fd, uint8_t* shadow_addr) {
  int mremap_flags = MREMAP_MAYMOVE | MREMAP_FIXED;
  if (gHaveMremapDontunmap) {
    mremap_flags |= MREMAP_DONTUNMAP;
  }
  void* ret = mremap(to_addr, map_size, map_size, mremap_flags, from_addr);
  CHECK_EQ(ret, static_cast<void*>(from_addr))
      << "mremap to move pages failed: " << strerror(errno)
      << ". space-addr=" << reinterpret_cast<void*>(to_addr) << " size=" << PrettySize(map_size);
  if (shadow_addr != nullptr) {
    DCHECK_EQ(fd, kFdUnused);
    DCHECK(gHaveMremapDontunmap);
    ret = mremap(shadow_addr, map_size, map_size, mremap_flags, to_addr);
    CHECK_EQ(ret, static_cast<void*>(to_addr))
        << "mremap from shadow to to-space map failed: " << strerror(errno);
  } else if (!gHaveMremapDontunmap || fd > kFdUnused) {
    int mmap_flags = MAP_FIXED;
    if (fd == kFdUnused) {
      mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE;
      fd = -1;
    } else if (IsValidFd(fd)) {
      mmap_flags |= MAP_SHARED;
    } else {
      DCHECK_EQ(fd, kFdSharedAnon);
      mmap_flags |= MAP_SHARED | MAP_ANONYMOUS;
    }
    ret = mmap(to_addr, map_size, PROT_READ | PROT_WRITE, mmap_flags, fd, 0);
    CHECK_EQ(ret, static_cast<void*>(to_addr))
        << "mmap for moving space failed: " << strerror(errno);
  }
}
void MarkCompact::KernelPreparation() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  uint8_t* moving_space_begin = bump_pointer_space_->Begin();
  size_t moving_space_size = bump_pointer_space_->Capacity();
  int mode = kCopyMode;
  size_t moving_space_register_sz;
  if (minor_fault_initialized_) {
    moving_space_register_sz = (moving_first_objs_count_ + black_page_count_) * kPageSize;
    if (shadow_to_space_map_.IsValid()) {
      size_t shadow_size = shadow_to_space_map_.Size();
      void* addr = shadow_to_space_map_.Begin();
      if (shadow_size < moving_space_register_sz) {
        addr = mremap(addr,
                      shadow_size,
                      moving_space_register_sz,
                      kObjPtrPoisoning ? 0 : MREMAP_MAYMOVE,
                                      nullptr);
        if (addr != MAP_FAILED) {
          MemMap temp = MemMap::MapPlaceholder(
              "moving-space-shadow", static_cast<uint8_t*>(addr), moving_space_register_sz);
          std::swap(shadow_to_space_map_, temp);
        }
      }
      if (addr != MAP_FAILED) {
        mode = kMinorFaultMode;
      } else {
        DCHECK_EQ(mprotect(shadow_to_space_map_.Begin(), shadow_to_space_map_.Size(), PROT_NONE), 0)
            << "mprotect failed: " << strerror(errno);
      }
    }
  } else {
    moving_space_register_sz = moving_space_size;
  }
  bool map_shared =
      minor_fault_initialized_ || (!Runtime::Current()->IsZygote() && uffd_minor_fault_supported_);
  uint8_t* shadow_addr = nullptr;
  if (moving_to_space_fd_ == kFdUnused && map_shared) {
    DCHECK(gHaveMremapDontunmap);
    DCHECK(shadow_to_space_map_.IsValid());
    DCHECK_EQ(shadow_to_space_map_.Size(), moving_space_size);
    shadow_addr = shadow_to_space_map_.Begin();
  }
  KernelPrepareRangeForUffd(
      moving_space_begin, from_space_begin_, moving_space_size, moving_to_space_fd_, shadow_addr);
  if (IsValidFd(uffd_)) {
    RegisterUffd(moving_space_begin, moving_space_register_sz, mode);
    for (auto& data : linear_alloc_spaces_data_) {
      bool mmap_again = map_shared && !data.already_shared_;
      DCHECK_EQ(static_cast<ssize_t>(data.shadow_.Size()), data.end_ - data.begin_);
      if (!mmap_again) {
        RegisterUffd(data.begin_,
                     data.shadow_.Size(),
                     minor_fault_initialized_ ? kMinorFaultMode : kCopyMode);
      }
      KernelPrepareRangeForUffd(data.begin_,
                                data.shadow_.Begin(),
                                data.shadow_.Size(),
                                mmap_again ? kFdSharedAnon : kFdUnused);
      if (mmap_again) {
        data.already_shared_ = true;
        RegisterUffd(data.begin_,
                     data.shadow_.Size(),
                     minor_fault_initialized_ ? kMinorFaultMode : kCopyMode);
      }
    }
  }
  if (map_shared) {
    map_linear_alloc_shared_ = true;
  }
}
template <int kMode>
void MarkCompact::ConcurrentCompaction(uint8_t* buf) {
  DCHECK_NE(kMode, kFallbackMode);
  DCHECK(kMode != kCopyMode || buf != nullptr);
  size_t nr_moving_space_used_pages = moving_first_objs_count_ + black_page_count_;
  while (true) {
    struct uffd_msg msg;
    ssize_t nread = read(uffd_, &msg, sizeof(msg));
    CHECK_GT(nread, 0);
    CHECK_EQ(msg.event, UFFD_EVENT_PAGEFAULT);
    DCHECK_EQ(nread, static_cast<ssize_t>(sizeof(msg)));
    uint8_t* fault_addr = reinterpret_cast<uint8_t*>(msg.arg.pagefault.address);
    if (fault_addr == conc_compaction_termination_page_) {
      uint8_t ret = thread_pool_counter_--;
      if (!gKernelHasFaultRetry || ret == 1) {
        ZeropageIoctl(fault_addr, false, false);
      } else {
        struct uffdio_range uffd_range;
        uffd_range.start = msg.arg.pagefault.address;
        uffd_range.len = kPageSize;
        CHECK_EQ(ioctl(uffd_, UFFDIO_WAKE, &uffd_range), 0)
            << "ioctl_userfaultfd: wake failed for concurrent-compaction termination page: "
            << strerror(errno);
      }
      break;
    }
    uint8_t* fault_page = AlignDown(fault_addr, kPageSize);
    if (bump_pointer_space_->HasAddress(reinterpret_cast<mirror::Object*>(fault_addr))) {
      ConcurrentlyProcessMovingPage<kMode>(fault_page, buf, nr_moving_space_used_pages);
    } else if (minor_fault_initialized_) {
      ConcurrentlyProcessLinearAllocPage<kMinorFaultMode>(
          fault_page, (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_MINOR) != 0);
    } else {
      ConcurrentlyProcessLinearAllocPage<kCopyMode>(
          fault_page, (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_MINOR) != 0);
    }
  }
}
bool MarkCompact::SigbusHandler(siginfo_t* info) {
  class ScopedInProgressCount {
   public:
    explicit ScopedInProgressCount(MarkCompact* collector) : collector_(collector) {
      SigbusCounterType prev =
          collector_->sigbus_in_progress_count_.load(std::memory_order_relaxed);
      while ((prev & kSigbusCounterCompactionDoneMask) == 0) {
        if (collector_->sigbus_in_progress_count_.compare_exchange_strong(
                prev, prev + 1, std::memory_order_acquire)) {
          DCHECK_LT(prev, kSigbusCounterCompactionDoneMask - 1);
          compaction_done_ = false;
          return;
        }
      }
      compaction_done_ = true;
    }
    bool IsCompactionDone() const { return compaction_done_; }
    ~ScopedInProgressCount() {
      if (!IsCompactionDone()) {
        collector_->sigbus_in_progress_count_.fetch_sub(1, std::memory_order_release);
      }
    }
   private:
    MarkCompact* const collector_;
    bool compaction_done_;
  };
  DCHECK(use_uffd_sigbus_);
  if (info->si_code != BUS_ADRERR) {
    return false;
  }
  ScopedInProgressCount spc(this);
  uint8_t* fault_page = AlignDown(reinterpret_cast<uint8_t*>(info->si_addr), kPageSize);
  if (!spc.IsCompactionDone()) {
    if (bump_pointer_space_->HasAddress(reinterpret_cast<mirror::Object*>(fault_page))) {
      Thread* self = Thread::Current();
      Locks::mutator_lock_->AssertSharedHeld(self);
      size_t nr_moving_space_used_pages = moving_first_objs_count_ + black_page_count_;
      if (minor_fault_initialized_) {
        ConcurrentlyProcessMovingPage<kMinorFaultMode>(
            fault_page, nullptr, nr_moving_space_used_pages);
      } else {
        uint8_t* buf = self->GetThreadLocalGcBuffer();
        if (buf == nullptr) {
          uint16_t idx = compaction_buffer_counter_.fetch_add(1, std::memory_order_relaxed);
          CHECK_LE(idx, kMutatorCompactionBufferCount);
          buf = compaction_buffers_map_.Begin() + idx * kPageSize;
          DCHECK(compaction_buffers_map_.HasAddress(buf));
          self->SetThreadLocalGcBuffer(buf);
        }
        ConcurrentlyProcessMovingPage<kCopyMode>(fault_page, buf, nr_moving_space_used_pages);
      }
      return true;
    } else {
      for (auto& data : linear_alloc_spaces_data_) {
        if (data.begin_ <= fault_page && data.end_ > fault_page) {
          if (minor_fault_initialized_) {
            ConcurrentlyProcessLinearAllocPage<kMinorFaultMode>(fault_page, false);
          } else {
            ConcurrentlyProcessLinearAllocPage<kCopyMode>(fault_page, false);
          }
          return true;
        }
      }
      return false;
    }
  } else {
    return bump_pointer_space_->HasAddress(reinterpret_cast<mirror::Object*>(fault_page)) ||
           linear_alloc_spaces_data_.end() !=
               std::find_if(linear_alloc_spaces_data_.begin(),
                            linear_alloc_spaces_data_.end(),
                            [fault_page](const LinearAllocSpaceData& data) {
                              return data.begin_ <= fault_page && data.end_ > fault_page;
                            });
  }
}
static void BackOff(uint32_t i) {
  static constexpr uint32_t kYieldMax = 5;
  if (i <= kYieldMax) {
    sched_yield();
  } else {
    NanoSleep(10000ull * (i - kYieldMax));
  }
}
template <int kMode>
void MarkCompact::ConcurrentlyProcessMovingPage(uint8_t* fault_page,
                                                uint8_t* buf,
                                                size_t nr_moving_space_used_pages) {
  class ScopedInProgressCount {
   public:
    explicit ScopedInProgressCount(MarkCompact* collector) : collector_(collector) {
      collector_->compaction_in_progress_count_.fetch_add(1, std::memory_order_relaxed);
    }
    ~ScopedInProgressCount() {
      collector_->compaction_in_progress_count_.fetch_sub(1, std::memory_order_relaxed);
    }
   private:
    MarkCompact* collector_;
  };
  uint8_t* unused_space_begin =
      bump_pointer_space_->Begin() + nr_moving_space_used_pages * kPageSize;
  DCHECK(IsAligned<kPageSize>(unused_space_begin));
  DCHECK(kMode == kCopyMode || fault_page < unused_space_begin);
  if (kMode == kCopyMode && fault_page >= unused_space_begin) {
    ZeropageIoctl(fault_page, true, true);
    return;
  }
  size_t page_idx = (fault_page - bump_pointer_space_->Begin()) / kPageSize;
  mirror::Object* first_obj = first_objs_moving_space_[page_idx].AsMirrorPtr();
  if (first_obj == nullptr) {
    PageState expected_state = PageState::kUnprocessed;
    if (moving_pages_status_[page_idx].compare_exchange_strong(
            expected_state, PageState::kProcessedAndMapping, std::memory_order_relaxed)) {
      ZeropageIoctl(fault_page, false, true);
    } else {
      DCHECK_EQ(expected_state, PageState::kProcessedAndMapping);
    }
    return;
  }
  PageState state = moving_pages_status_[page_idx].load(
      use_uffd_sigbus_ ? std::memory_order_acquire : std::memory_order_relaxed);
  uint32_t backoff_count = 0;
  while (true) {
    switch (state) {
      case PageState::kUnprocessed: {
        ScopedInProgressCount spc(this);
        if (moving_pages_status_[page_idx].compare_exchange_strong(
                state, PageState::kMutatorProcessing, std::memory_order_acq_rel)) {
          if (kMode == kMinorFaultMode) {
            DCHECK_EQ(buf, nullptr);
            buf = shadow_to_space_map_.Begin() + page_idx * kPageSize;
          }
          if (fault_page < post_compact_end_) {
            CompactPage(
                first_obj, pre_compact_offset_moving_space_[page_idx], buf, kMode == kCopyMode);
          } else {
            DCHECK_NE(first_obj, nullptr);
            DCHECK_GT(pre_compact_offset_moving_space_[page_idx], 0u);
            uint8_t* pre_compact_page = black_allocations_begin_ + (fault_page - post_compact_end_);
            DCHECK(IsAligned<kPageSize>(pre_compact_page));
            SlideBlackPage(first_obj, page_idx, pre_compact_page, buf, kMode == kCopyMode);
          }
          moving_pages_status_[page_idx].store(PageState::kProcessedAndMapping,
                                               std::memory_order_release);
          if (kMode == kCopyMode) {
            CopyIoctl(fault_page, buf);
            if (use_uffd_sigbus_) {
              moving_pages_status_[page_idx].store(PageState::kProcessedAndMapped,
                                                   std::memory_order_release);
            }
            return;
          } else {
            break;
          }
        }
      }
        continue;
      case PageState::kProcessing:
        DCHECK_EQ(kMode, kMinorFaultMode);
        if (moving_pages_status_[page_idx].compare_exchange_strong(
                state, PageState::kProcessingAndMapping, std::memory_order_relaxed) &&
            !use_uffd_sigbus_) {
          return;
        }
        continue;
      case PageState::kProcessed:
        break;
      case PageState::kProcessingAndMapping:
      case PageState::kMutatorProcessing:
      case PageState::kProcessedAndMapping:
        if (use_uffd_sigbus_) {
          BackOff(backoff_count++);
          state = moving_pages_status_[page_idx].load(std::memory_order_acquire);
          continue;
        }
        return;
      case PageState::kProcessedAndMapped:
        return;
    }
    break;
  }
  DCHECK_EQ(kMode, kMinorFaultMode);
  if (state == PageState::kUnprocessed) {
    MapProcessedPages< true>(
        fault_page, moving_pages_status_, page_idx, nr_moving_space_used_pages);
  } else {
    DCHECK_EQ(state, PageState::kProcessed);
    MapProcessedPages< false>(
        fault_page, moving_pages_status_, page_idx, nr_moving_space_used_pages);
  }
}
void MarkCompact::MapUpdatedLinearAllocPage(uint8_t* page,
                                            uint8_t* shadow_page,
                                            Atomic<PageState>& state,
                                            bool page_touched) {
  DCHECK(!minor_fault_initialized_);
  if (page_touched) {
    CopyIoctl(page, shadow_page);
  } else {
    ZeropageIoctl(page, false, false);
  }
  if (use_uffd_sigbus_) {
    state.store(PageState::kProcessedAndMapped, std::memory_order_release);
  }
}
template <int kMode>
void MarkCompact::ConcurrentlyProcessLinearAllocPage(uint8_t* fault_page, bool is_minor_fault) {
  DCHECK(!is_minor_fault || kMode == kMinorFaultMode);
  auto arena_iter = linear_alloc_arenas_.end();
  {
    TrackedArena temp_arena(fault_page);
    arena_iter = linear_alloc_arenas_.upper_bound(&temp_arena);
    arena_iter = arena_iter != linear_alloc_arenas_.begin() ? std::prev(arena_iter) :
                                                              linear_alloc_arenas_.end();
  }
  if (arena_iter == linear_alloc_arenas_.end() || arena_iter->second <= fault_page) {
    ZeropageIoctl(fault_page, true, false);
  } else {
    DCHECK(arena_iter != linear_alloc_arenas_.end())
        << "fault_page:" << static_cast<void*>(fault_page) << "is_minor_fault:" << is_minor_fault;
    LinearAllocSpaceData* space_data = nullptr;
    for (auto& data : linear_alloc_spaces_data_) {
      if (data.begin_ <= fault_page && fault_page < data.end_) {
        space_data = &data;
        break;
      }
    }
    DCHECK_NE(space_data, nullptr);
    ptrdiff_t diff = space_data->shadow_.Begin() - space_data->begin_;
    size_t page_idx = (fault_page - space_data->begin_) / kPageSize;
    Atomic<PageState>* state_arr =
        reinterpret_cast<Atomic<PageState>*>(space_data->page_status_map_.Begin());
    PageState state = state_arr[page_idx].load(use_uffd_sigbus_ ? std::memory_order_acquire :
                                                                  std::memory_order_relaxed);
    uint32_t backoff_count = 0;
    while (true) {
      switch (state) {
        case PageState::kUnprocessed: {
          if (state_arr[page_idx].compare_exchange_strong(
                  state, PageState::kProcessingAndMapping, std::memory_order_acquire)) {
            if (kMode == kCopyMode || is_minor_fault) {
              uint8_t* first_obj = arena_iter->first->GetFirstObject(fault_page);
              DCHECK_NE(first_obj, nullptr);
              LinearAllocPageUpdater updater(this);
              updater(fault_page + diff, first_obj + diff);
              if (kMode == kCopyMode) {
                CopyIoctl(fault_page, fault_page + diff);
                if (use_uffd_sigbus_) {
                  state_arr[page_idx].store(PageState::kProcessedAndMapped,
                                            std::memory_order_release);
                }
                return;
              }
            } else {
              ForceRead(fault_page + diff);
            }
            MapProcessedPages< true>(
                fault_page, state_arr, page_idx, space_data->page_status_map_.Size());
            return;
          }
        }
          continue;
        case PageState::kProcessing:
          DCHECK_EQ(kMode, kMinorFaultMode);
          if (state_arr[page_idx].compare_exchange_strong(
                  state, PageState::kProcessingAndMapping, std::memory_order_relaxed) &&
              !use_uffd_sigbus_) {
            return;
          }
          continue;
        case PageState::kProcessed:
          break;
        case PageState::kMutatorProcessing:
          UNREACHABLE();
        case PageState::kProcessingAndMapping:
        case PageState::kProcessedAndMapping:
          if (use_uffd_sigbus_) {
            BackOff(backoff_count++);
            state = state_arr[page_idx].load(std::memory_order_acquire);
            continue;
          }
          return;
        case PageState::kProcessedAndMapped:
          return;
      }
      break;
    }
    DCHECK_EQ(kMode, kMinorFaultMode);
    DCHECK_EQ(state, PageState::kProcessed);
    if (!is_minor_fault) {
      ForceRead(fault_page + diff);
    }
    MapProcessedPages< false>(
        fault_page, state_arr, page_idx, space_data->page_status_map_.Size());
  }
}
void MarkCompact::ProcessLinearAlloc() {
  for (auto& pair : linear_alloc_arenas_) {
    const TrackedArena* arena = pair.first;
    uint8_t* last_byte = pair.second;
    DCHECK_ALIGNED(last_byte, kPageSize);
    bool others_processing = false;
    LinearAllocSpaceData* space_data = nullptr;
    for (auto& data : linear_alloc_spaces_data_) {
      if (data.begin_ <= arena->Begin() && arena->Begin() < data.end_) {
        space_data = &data;
        break;
      }
    }
    DCHECK_NE(space_data, nullptr);
    ptrdiff_t diff = space_data->shadow_.Begin() - space_data->begin_;
    auto visitor = [space_data, last_byte, diff, this, &others_processing](
                       uint8_t* page_begin,
                       uint8_t* first_obj) REQUIRES_SHARED(Locks::mutator_lock_) {
      if (page_begin >= last_byte) {
        return;
      }
      LinearAllocPageUpdater updater(this);
      size_t page_idx = (page_begin - space_data->begin_) / kPageSize;
      DCHECK_LT(page_idx, space_data->page_status_map_.Size());
      Atomic<PageState>* state_arr =
          reinterpret_cast<Atomic<PageState>*>(space_data->page_status_map_.Begin());
      PageState expected_state = PageState::kUnprocessed;
      PageState desired_state =
          minor_fault_initialized_ ? PageState::kProcessing : PageState::kProcessingAndMapping;
      if (state_arr[page_idx].compare_exchange_strong(
              expected_state, desired_state, std::memory_order_acquire)) {
        updater(page_begin + diff, first_obj + diff);
        expected_state = PageState::kProcessing;
        if (!minor_fault_initialized_) {
<<<<<<< HEAD
          MapUpdatedLinearAllocPage(
              page_begin, page_begin + diff, state_arr[page_idx], updater.WasLastPageTouched());
|||||||
            struct uffdio_copy uffd_copy;
            uffd_copy.src = reinterpret_cast<uintptr_t>(page_begin + diff);
            uffd_copy.dst = reinterpret_cast<uintptr_t>(page_begin);
            uffd_copy.len = kPageSize;
            uffd_copy.mode = 0;
            CHECK_EQ(ioctl(uffd_, UFFDIO_COPY, &uffd_copy), 0)
                << "ioctl_userfaultfd: linear-alloc copy failed:" << strerror(errno)
                << ". dst:" << static_cast<void*>(page_begin);
            DCHECK_EQ(uffd_copy.copy, static_cast<ssize_t>(kPageSize));
=======
            CopyIoctl(page_begin, page_begin + diff);
            if (use_uffd_sigbus_) {
              state_arr[page_idx].store(PageState::kProcessedAndMapped, std::memory_order_release);
            }
>>>>>>> f4a0906b4690ed3a710be7c04c989b1e55caa50c
        } else if (!state_arr[page_idx].compare_exchange_strong(
                       expected_state, PageState::kProcessed, std::memory_order_release)) {
          DCHECK_EQ(expected_state, PageState::kProcessingAndMapping);
          ForceRead(page_begin + diff);
          MapProcessedPages< true>(
              page_begin, state_arr, page_idx, space_data->page_status_map_.Size());
        }
      } else {
        others_processing = true;
      }
    };
    arena->VisitRoots(visitor);
    if (!minor_fault_initialized_ && !others_processing) {
      ZeroAndReleasePages(arena->Begin() + diff, arena->Size());
    }
  }
}
void MarkCompact::RegisterUffd(void* addr, size_t size, int mode) {
  DCHECK(IsValidFd(uffd_));
  struct uffdio_register uffd_register;
  uffd_register.range.start = reinterpret_cast<uintptr_t>(addr);
  uffd_register.range.len = size;
  uffd_register.mode = UFFDIO_REGISTER_MODE_MISSING;
  if (mode == kMinorFaultMode) {
    uffd_register.mode |= UFFDIO_REGISTER_MODE_MINOR;
  }
  CHECK_EQ(ioctl(uffd_, UFFDIO_REGISTER, &uffd_register), 0)
      << "ioctl_userfaultfd: register failed: " << strerror(errno)
      << ". start:" << static_cast<void*>(addr) << " len:" << PrettySize(size);
}
void MarkCompact::UnregisterUffd(uint8_t* start, size_t len) {
  DCHECK(IsValidFd(uffd_));
  struct uffdio_range range;
  range.start = reinterpret_cast<uintptr_t>(start);
  range.len = len;
  CHECK_EQ(ioctl(uffd_, UFFDIO_UNREGISTER, &range), 0)
      << "ioctl_userfaultfd: unregister failed: " << strerror(errno)
      << ". addr:" << static_cast<void*>(start) << " len:" << PrettySize(len);
  if (minor_fault_initialized_) {
    CHECK_EQ(ioctl(uffd_, UFFDIO_WAKE, &range), 0)
        << "ioctl_userfaultfd: wake failed: " << strerror(errno)
        << ". addr:" << static_cast<void*>(start) << " len:" << PrettySize(len);
  }
}
void MarkCompact::CompactionPhase() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  {
    int32_t freed_bytes = black_objs_slide_diff_;
    bump_pointer_space_->RecordFree(freed_objects_, freed_bytes);
    RecordFree(ObjectBytePair(freed_objects_, freed_bytes));
  }
  if (CanCompactMovingSpaceWithMinorFault()) {
    CompactMovingSpace<kMinorFaultMode>( nullptr);
  } else {
    CompactMovingSpace<kCopyMode>(compaction_buffers_map_.Begin());
  }
  for (uint32_t i = 0; compaction_in_progress_count_.load(std::memory_order_acquire) > 0; i++) {
    BackOff(i);
  }
  size_t moving_space_size = bump_pointer_space_->Capacity();
  UnregisterUffd(bump_pointer_space_->Begin(),
                 minor_fault_initialized_ ?
                     (moving_first_objs_count_ + black_page_count_) * kPageSize :
                     moving_space_size);
  if (minor_fault_initialized_) {
    if (IsValidFd(moving_from_space_fd_)) {
      int ret = mprotect(from_space_begin_, moving_space_size, PROT_NONE);
      CHECK_EQ(ret, 0) << "mprotect(PROT_NONE) for from-space failed: " << strerror(errno);
      ret = fallocate(moving_from_space_fd_,
                      FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                                 0,
                      moving_space_size);
      CHECK_EQ(ret, 0) << "fallocate for from-space failed: " << strerror(errno);
    } else {
      int ret = madvise(from_space_begin_, moving_space_size, MADV_REMOVE);
      CHECK_EQ(ret, 0) << "madvise(MADV_REMOVE) failed for from-space map:" << strerror(errno);
    }
  } else {
    from_space_map_.MadviseDontNeedAndZero();
  }
  if (shadow_to_space_map_.IsValid()) {
    DCHECK_EQ(mprotect(shadow_to_space_map_.Begin(), shadow_to_space_map_.Size(), PROT_NONE), 0)
        << "mprotect(PROT_NONE) for shadow-map failed:" << strerror(errno);
  }
  if (!IsValidFd(moving_from_space_fd_)) {
    DCHECK_EQ(mprotect(from_space_begin_, moving_space_size, PROT_NONE), 0)
        << "mprotect(PROT_NONE) for from-space failed: " << strerror(errno);
  }
  ProcessLinearAlloc();
  if (use_uffd_sigbus_) {
    SigbusCounterType count = sigbus_in_progress_count_.fetch_or(kSigbusCounterCompactionDoneMask,
                                                                 std::memory_order_acq_rel);
    for (uint32_t i = 0; count > 0; i++) {
      BackOff(i);
      count = sigbus_in_progress_count_.load(std::memory_order_acquire);
      count &= ~kSigbusCounterCompactionDoneMask;
    }
  } else {
    DCHECK(IsAligned<kPageSize>(conc_compaction_termination_page_));
    do {
      ZeroAndReleasePages(conc_compaction_termination_page_, kPageSize);
      ForceRead(conc_compaction_termination_page_);
    } while (thread_pool_counter_ > 0);
  }
  for (auto& data : linear_alloc_spaces_data_) {
    DCHECK_EQ(data.end_ - data.begin_, static_cast<ssize_t>(data.shadow_.Size()));
    UnregisterUffd(data.begin_, data.shadow_.Size());
    data.page_status_map_.MadviseDontNeedAndZero();
    data.shadow_.MadviseDontNeedAndZero();
    if (minor_fault_initialized_) {
      DCHECK_EQ(mprotect(data.shadow_.Begin(), data.shadow_.Size(), PROT_NONE), 0)
          << "mprotect failed: " << strerror(errno);
    }
  }
  if (!use_uffd_sigbus_) {
    heap_->GetThreadPool()->StopWorkers(thread_running_gc_);
  }
}
template <size_t kBufferSize>
class MarkCompact::ThreadRootsVisitor : public RootVisitor {
 public:
  explicit ThreadRootsVisitor(MarkCompact* mark_compact, Thread* const self)
      : mark_compact_(mark_compact), self_(self) {}
  ~ThreadRootsVisitor() { Flush(); }
  REQUIRES_SHARED(Locks::mutator_lock_)
  REQUIRES_SHARED(Locks::mutator_lock_)
  REQUIRES(Locks::heap_bitmap_lock_) {
    for (size_t i = 0; i < count; i++) {
      mirror::Object* obj = *roots[i];
      if (mark_compact_->MarkObjectNonNullNoPush< true>(obj)) {
        Push(obj);
      }
    }
  }
  REQUIRES_SHARED(Locks::mutator_lock_)
  REQUIRES_SHARED(Locks::mutator_lock_)
  REQUIRES(Locks::heap_bitmap_lock_) {
    for (size_t i = 0; i < count; i++) {
      mirror::Object* obj = roots[i]->AsMirrorPtr();
      if (mark_compact_->MarkObjectNonNullNoPush< true>(obj)) {
        Push(obj);
      }
    }
  }
 private:
  void Flush() REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(Locks::heap_bitmap_lock_) {
    StackReference<mirror::Object>* start;
    StackReference<mirror::Object>* end;
    {
      MutexLock mu(self_, mark_compact_->lock_);
      while (!mark_compact_->mark_stack_->BumpBack(idx_, &start, &end)) {
        mark_compact_->ExpandMarkStack();
      }
    }
    while (idx_ > 0) {
      *start++ = roots_[--idx_];
    }
    DCHECK_EQ(start, end);
  }
  REQUIRES_SHARED(Locks::mutator_lock_)
  REQUIRES_SHARED(Locks::mutator_lock_)
  REQUIRES(Locks::heap_bitmap_lock_) {
    if (UNLIKELY(idx_ >= kBufferSize)) {
      Flush();
    }
    roots_[idx_++].Assign(obj);
  }
  StackReference<mirror::Object> roots_[kBufferSize];
  size_t idx_ = 0;
  MarkCompact* const mark_compact_;
  Thread* const self_;
};
class MarkCompact::CheckpointMarkThreadRoots : public Closure {
 public:
  explicit CheckpointMarkThreadRoots(MarkCompact* mark_compact) : mark_compact_(mark_compact) {}
  void Run(Thread* thread) override NO_THREAD_SAFETY_ANALYSIS {
    ScopedTrace trace("Marking thread roots");
    Thread* const self = Thread::Current();
    CHECK(thread == self || thread->IsSuspended() ||
          thread->GetState() == ThreadState::kWaitingPerformingGc)
        << thread->GetState() << " thread " << thread << " self " << self;
    {
      ThreadRootsVisitor< 20> visitor(mark_compact_, self);
      thread->VisitRoots(&visitor, kVisitRootFlagAllRoots);
    }
    thread->SetThreadLocalGcBuffer(nullptr);
    mark_compact_->GetBarrier().Pass(self);
  }
 private:
  MarkCompact* const mark_compact_;
};
void MarkCompact::MarkRootsCheckpoint(Thread* self, Runtime* runtime) {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  CheckpointMarkThreadRoots check_point(this);
  ThreadList* thread_list = runtime->GetThreadList();
  gc_barrier_.Init(self, 0);
  size_t barrier_count = thread_list->RunCheckpoint(&check_point);
  if (barrier_count == 0) {
    return;
  }
  Locks::heap_bitmap_lock_->ExclusiveUnlock(self);
  Locks::mutator_lock_->SharedUnlock(self);
  {
    ScopedThreadStateChange tsc(self, ThreadState::kWaitingForCheckPointsToRun);
    gc_barrier_.Increment(self, barrier_count);
  }
  Locks::mutator_lock_->SharedLock(self);
  Locks::heap_bitmap_lock_->ExclusiveLock(self);
}
void MarkCompact::MarkNonThreadRoots(Runtime* runtime) {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  runtime->VisitNonThreadRoots(this);
}
void MarkCompact::MarkConcurrentRoots(VisitRootFlags flags, Runtime* runtime) {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  runtime->VisitConcurrentRoots(this, flags);
}
void MarkCompact::RevokeAllThreadLocalBuffers() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  bump_pointer_space_->RevokeAllThreadLocalBuffers();
}
class MarkCompact::ScanObjectVisitor {
 public:
  REQUIRES_SHARED(Locks::mutator_lock_) {
    mark_compact_->ScanObject< false>(obj.Ptr());
  }
 private:
  class ScanObjectVisitor;
  class ScanObjectVisitor;
 public:
  REQUIRES_SHARED(Locks::mutator_lock_) {
    mark_compact_->ScanObject< false>(obj.Ptr());
  }
 private:
  class ScanObjectVisitor;
};
void MarkCompact::UpdateAndMarkModUnion() {
  accounting::CardTable* const card_table = heap_->GetCardTable();
  for (const auto& space : immune_spaces_.GetSpaces()) {
    const char* name = space->IsZygoteSpace() ? "UpdateAndMarkZygoteModUnionTable" :
                                                "UpdateAndMarkImageModUnionTable";
    DCHECK(space->IsZygoteSpace() || space->IsImageSpace()) << *space;
    TimingLogger::ScopedTiming t(name, GetTimings());
    accounting::ModUnionTable* table = heap_->FindModUnionTableFromSpace(space);
    if (table != nullptr) {
      table->UpdateAndMarkReferences(this);
    } else {
      card_table->Scan< false>(space->GetMarkBitmap(),
                                             space->Begin(),
                                             space->End(),
                                             ScanObjectVisitor(this),
                                             gc::accounting::CardTable::kCardAged);
    }
  }
}
void MarkCompact::MarkReachableObjects() {
  UpdateAndMarkModUnion();
  ProcessMarkStack();
}
class MarkCompact::CardModifiedVisitor {
 public:
  explicit CardModifiedVisitor(MarkCompact* const mark_compact,
                               accounting::ContinuousSpaceBitmap* const bitmap,
                               accounting::CardTable* const card_table)
      : visitor_(mark_compact), bitmap_(bitmap), card_table_(card_table) {}
  void operator()(uint8_t* card, uint8_t expected_value, uint8_t new_value ATTRIBUTE_UNUSED) const {
    if (expected_value == accounting::CardTable::kCardDirty) {
      uintptr_t start = reinterpret_cast<uintptr_t>(card_table_->AddrFromCard(card));
      bitmap_->VisitMarkedRange(start, start + accounting::CardTable::kCardSize, visitor_);
    }
  }
 private:
  ScanObjectVisitor visitor_;
  accounting::ContinuousSpaceBitmap* bitmap_;
  accounting::CardTable* const card_table_;
};
void MarkCompact::ScanDirtyObjects(bool paused, uint8_t minimum_age) {
  accounting::CardTable* card_table = heap_->GetCardTable();
  for (const auto& space : heap_->GetContinuousSpaces()) {
    const char* name = nullptr;
    switch (space->GetGcRetentionPolicy()) {
      case space::kGcRetentionPolicyNeverCollect:
        name = paused ? "(Paused)ScanGrayImmuneSpaceObjects" : "ScanGrayImmuneSpaceObjects";
        break;
      case space::kGcRetentionPolicyFullCollect:
        name = paused ? "(Paused)ScanGrayZygoteSpaceObjects" : "ScanGrayZygoteSpaceObjects";
        break;
      case space::kGcRetentionPolicyAlwaysCollect:
        name = paused ? "(Paused)ScanGrayAllocSpaceObjects" : "ScanGrayAllocSpaceObjects";
        break;
      default:
        LOG(FATAL) << "Unreachable";
        UNREACHABLE();
    }
    TimingLogger::ScopedTiming t(name, GetTimings());
    ScanObjectVisitor visitor(this);
    const bool is_immune_space = space->IsZygoteSpace() || space->IsImageSpace();
    if (paused) {
      DCHECK_EQ(minimum_age, gc::accounting::CardTable::kCardDirty);
      if (is_immune_space) {
        card_table->Scan< false>(
            space->GetMarkBitmap(), space->Begin(), space->End(), visitor, minimum_age);
      } else {
        card_table->Scan< true>(
            space->GetMarkBitmap(), space->Begin(), space->End(), visitor, minimum_age);
      }
    } else {
      DCHECK_EQ(minimum_age, gc::accounting::CardTable::kCardAged);
      accounting::ModUnionTable* table = heap_->FindModUnionTableFromSpace(space);
      if (table) {
        table->ProcessCards();
        card_table->Scan< false>(
            space->GetMarkBitmap(), space->Begin(), space->End(), visitor, minimum_age);
      } else {
        CardModifiedVisitor card_modified_visitor(this, space->GetMarkBitmap(), card_table);
        if (is_immune_space) {
          card_table->ModifyCardsAtomic(
              space->Begin(),
              space->End(),
              [](uint8_t card) {
                return (card == gc::accounting::CardTable::kCardClean) ?
                           card :
                           gc::accounting::CardTable::kCardAged;
              },
              card_modified_visitor);
        } else {
          card_table->ModifyCardsAtomic(
              space->Begin(), space->End(), AgeCardVisitor(), card_modified_visitor);
        }
      }
    }
  }
}
void MarkCompact::RecursiveMarkDirtyObjects(bool paused, uint8_t minimum_age) {
  ScanDirtyObjects(paused, minimum_age);
  ProcessMarkStack();
}
void MarkCompact::MarkRoots(VisitRootFlags flags) {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  Runtime* runtime = Runtime::Current();
  MarkRootsCheckpoint(thread_running_gc_, runtime);
  MarkNonThreadRoots(runtime);
  MarkConcurrentRoots(flags, runtime);
}
void MarkCompact::PreCleanCards() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  CHECK(!Locks::mutator_lock_->IsExclusiveHeld(thread_running_gc_));
  MarkRoots(static_cast<VisitRootFlags>(kVisitRootFlagClearRootLog | kVisitRootFlagNewRoots));
  RecursiveMarkDirtyObjects( false, accounting::CardTable::kCardDirty - 1);
}
void MarkCompact::MarkingPhase() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  DCHECK_EQ(thread_running_gc_, Thread::Current());
  WriterMutexLock mu(thread_running_gc_, *Locks::heap_bitmap_lock_);
  BindAndResetBitmaps();
  MarkZygoteLargeObjects();
  MarkRoots(
      static_cast<VisitRootFlags>(kVisitRootFlagAllRoots | kVisitRootFlagStartLoggingNewRoots));
  MarkReachableObjects();
  PreCleanCards();
  ReferenceProcessor* rp = GetHeap()->GetReferenceProcessor();
  bool clear_soft_references = GetCurrentIteration()->GetClearSoftReferences();
  rp->Setup(thread_running_gc_, this, true, clear_soft_references);
  if (!clear_soft_references) {
    rp->ForwardSoftReferences(GetTimings());
  }
}
class MarkCompact::RefFieldsVisitor {
 private:
  class RefFieldsVisitor;
 public:
  RefFieldsVisitor(MarkCompact* const mark_compact) : mark_compact_(mark_compact) {}
  REQUIRES(Locks::heap_bitmap_lock_)
  REQUIRES(Locks::heap_bitmap_lock_)
  REQUIRES_SHARED(Locks::mutator_lock_) {
    if (kCheckLocks) {
      Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
      Locks::heap_bitmap_lock_->AssertExclusiveHeld(Thread::Current());
    }
    mark_compact_->MarkObject(obj->GetFieldObject<mirror::Object>(offset), obj, offset);
  }
  REQUIRES(Locks::heap_bitmap_lock_)
  REQUIRES(Locks::heap_bitmap_lock_)
  REQUIRES_SHARED(Locks::mutator_lock_) { mark_compact_->DelayReferenceReferent(klass, ref); }
  REQUIRES(Locks::heap_bitmap_lock_)
  REQUIRES(Locks::heap_bitmap_lock_)
  REQUIRES_SHARED(Locks::mutator_lock_) {
    if (!root->IsNull()) {
      VisitRoot(root);
    }
  }
  REQUIRES(Locks::heap_bitmap_lock_)
  REQUIRES(Locks::heap_bitmap_lock_)
  REQUIRES_SHARED(Locks::mutator_lock_) {
    if (kCheckLocks) {
      Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
      Locks::heap_bitmap_lock_->AssertExclusiveHeld(Thread::Current());
    }
    mark_compact_->MarkObject(root->AsMirrorPtr());
  }
 private:
  MarkCompact* const mark_compact_;
};
template <size_t kAlignment>
size_t MarkCompact::LiveWordsBitmap<kAlignment>::LiveBytesInBitmapWord(size_t chunk_idx) const {
  const size_t index = chunk_idx * kBitmapWordsPerVectorWord;
  size_t words = 0;
  for (uint32_t i = 0; i < kBitmapWordsPerVectorWord; i++) {
    words += POPCOUNT(Bitmap::Begin()[index + i]);
  }
  return words * kAlignment;
}
void MarkCompact::UpdateLivenessInfo(mirror::Object* obj) {
  DCHECK(obj != nullptr);
  uintptr_t obj_begin = reinterpret_cast<uintptr_t>(obj);
  UpdateClassAfterObjectMap(obj);
  size_t size = RoundUp(obj->SizeOf<kDefaultVerifyFlags>(), kAlignment);
  uintptr_t bit_index = live_words_bitmap_->SetLiveWords(obj_begin, size);
  size_t chunk_idx = (obj_begin - live_words_bitmap_->Begin()) / kOffsetChunkSize;
  bit_index %= kBitsPerVectorWord;
  size_t first_chunk_portion = std::min(size, (kBitsPerVectorWord - bit_index) * kAlignment);
  chunk_info_vec_[chunk_idx++] += first_chunk_portion;
  DCHECK_LE(first_chunk_portion, size);
  for (size -= first_chunk_portion; size > kOffsetChunkSize; size -= kOffsetChunkSize) {
    DCHECK_EQ(chunk_info_vec_[chunk_idx], 0u);
    chunk_info_vec_[chunk_idx++] = kOffsetChunkSize;
  }
  chunk_info_vec_[chunk_idx] += size;
  freed_objects_--;
}
template <bool kUpdateLiveWords>
void MarkCompact::ScanObject(mirror::Object* obj) {
  RefFieldsVisitor visitor(this);
  DCHECK(IsMarked(obj)) << "Scanning marked object " << obj << "\n" << heap_->DumpSpaces();
  if (kUpdateLiveWords && moving_space_bitmap_->HasAddress(obj)) {
    UpdateLivenessInfo(obj);
  }
  obj->VisitReferences(visitor, visitor);
}
void MarkCompact::ProcessMarkStack() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  while (!mark_stack_->IsEmpty()) {
    mirror::Object* obj = mark_stack_->PopBack();
    DCHECK(obj != nullptr);
    ScanObject< true>(obj);
  }
}
void MarkCompact::ExpandMarkStack() {
  const size_t new_size = mark_stack_->Capacity() * 2;
  std::vector<StackReference<mirror::Object>> temp(mark_stack_->Begin(), mark_stack_->End());
  mark_stack_->Resize(new_size);
  for (auto& ref : temp) {
    mark_stack_->PushBack(ref.AsMirrorPtr());
  }
  DCHECK(!mark_stack_->IsFull());
}
inline void MarkCompact::PushOnMarkStack(mirror::Object* obj) {
  if (UNLIKELY(mark_stack_->IsFull())) {
    ExpandMarkStack();
  }
  mark_stack_->PushBack(obj);
}
inline void MarkCompact::MarkObjectNonNull(mirror::Object* obj,
                                           mirror::Object* holder,
                                           MemberOffset offset) {
  DCHECK(obj != nullptr);
  if (MarkObjectNonNullNoPush< false>(obj, holder, offset)) {
    PushOnMarkStack(obj);
  }
}
template <bool kParallel>
inline bool MarkCompact::MarkObjectNonNullNoPush(mirror::Object* obj,
                                                 mirror::Object* holder,
                                                 MemberOffset offset) {
  if (LIKELY(moving_space_bitmap_->HasAddress(obj))) {
    return kParallel ? !moving_space_bitmap_->AtomicTestAndSet(obj) :
                       !moving_space_bitmap_->Set(obj);
  } else if (non_moving_space_bitmap_->HasAddress(obj)) {
    return kParallel ? !non_moving_space_bitmap_->AtomicTestAndSet(obj) :
                       !non_moving_space_bitmap_->Set(obj);
  } else if (immune_spaces_.ContainsObject(obj)) {
    DCHECK(IsMarked(obj) != nullptr);
    return false;
  } else {
    if (!IsAligned<kPageSize>(obj)) {
      heap_->GetVerification()->LogHeapCorruption(holder, offset, obj, true);
    }
    DCHECK_NE(heap_->GetLargeObjectsSpace(), nullptr)
        << "ref=" << obj
        << " doesn't belong to any of the spaces and large object space doesn't exist";
    accounting::LargeObjectBitmap* los_bitmap = heap_->GetLargeObjectsSpace()->GetMarkBitmap();
    DCHECK(los_bitmap->HasAddress(obj));
    if (kParallel) {
      los_bitmap->AtomicTestAndSet(obj);
    } else {
      los_bitmap->Set(obj);
    }
    DCHECK(obj->IsString() || (obj->IsArrayInstance() && !obj->IsObjectArray()));
    return false;
  }
}
inline void MarkCompact::MarkObject(mirror::Object* obj,
                                    mirror::Object* holder,
                                    MemberOffset offset) {
  if (obj != nullptr) {
    MarkObjectNonNull(obj, holder, offset);
  }
}
mirror::Object* MarkCompact::MarkObject(mirror::Object* obj) {
  MarkObject(obj, nullptr, MemberOffset(0));
  return obj;
}
void MarkCompact::MarkHeapReference(mirror::HeapReference<mirror::Object>* obj,
                                    bool do_atomic_update ATTRIBUTE_UNUSED) {
  MarkObject(obj->AsMirrorPtr(), nullptr, MemberOffset(0));
}
void MarkCompact::VisitRoots(mirror::Object*** roots, size_t count, const RootInfo& info) {
  if (compacting_) {
    for (size_t i = 0; i < count; ++i) {
      UpdateRoot(roots[i], info);
    }
  } else {
    for (size_t i = 0; i < count; ++i) {
      MarkObjectNonNull(*roots[i]);
    }
  }
}
void MarkCompact::VisitRoots(mirror::CompressedReference<mirror::Object>** roots,
                             size_t count,
                             const RootInfo& info) {
  if (compacting_) {
    for (size_t i = 0; i < count; ++i) {
      UpdateRoot(roots[i], info);
    }
  } else {
    for (size_t i = 0; i < count; ++i) {
      MarkObjectNonNull(roots[i]->AsMirrorPtr());
    }
  }
}
mirror::Object* MarkCompact::IsMarked(mirror::Object* obj) {
  if (moving_space_bitmap_->HasAddress(obj)) {
    const bool is_black = reinterpret_cast<uint8_t*>(obj) >= black_allocations_begin_;
    if (compacting_) {
      if (is_black) {
        return PostCompactBlackObjAddr(obj);
      } else if (live_words_bitmap_->Test(obj)) {
        return PostCompactOldObjAddr(obj);
      } else {
        return nullptr;
      }
    }
    return (is_black || moving_space_bitmap_->Test(obj)) ? obj : nullptr;
  } else if (non_moving_space_bitmap_->HasAddress(obj)) {
    return non_moving_space_bitmap_->Test(obj) ? obj : nullptr;
  } else if (immune_spaces_.ContainsObject(obj)) {
    return obj;
  } else {
    DCHECK(heap_->GetLargeObjectsSpace())
        << "ref=" << obj
        << " doesn't belong to any of the spaces and large object space doesn't exist";
    accounting::LargeObjectBitmap* los_bitmap = heap_->GetLargeObjectsSpace()->GetMarkBitmap();
    if (los_bitmap->HasAddress(obj)) {
      DCHECK(IsAligned<kPageSize>(obj));
      return los_bitmap->Test(obj) ? obj : nullptr;
    } else {
      return nullptr;
    }
  }
}
bool MarkCompact::IsNullOrMarkedHeapReference(mirror::HeapReference<mirror::Object>* obj,
                                              bool do_atomic_update ATTRIBUTE_UNUSED) {
  mirror::Object* ref = obj->AsMirrorPtr();
  if (ref == nullptr) {
    return true;
  }
  return IsMarked(ref);
}
void MarkCompact::DelayReferenceReferent(ObjPtr<mirror::Class> klass,
                                         ObjPtr<mirror::Reference> ref) {
  heap_->GetReferenceProcessor()->DelayReferenceReferent(klass, ref, this);
}
void MarkCompact::FinishPhase() {
  bool is_zygote = Runtime::Current()->IsZygote();
  compacting_ = false;
  minor_fault_initialized_ = !is_zygote && uffd_minor_fault_supported_;
  if (use_uffd_sigbus_ || !minor_fault_initialized_ || !shadow_to_space_map_.IsValid() ||
      shadow_to_space_map_.Size() < (moving_first_objs_count_ + black_page_count_) * kPageSize) {
    size_t adjustment = use_uffd_sigbus_ ? 0 : kPageSize;
    ZeroAndReleasePages(compaction_buffers_map_.Begin() + adjustment,
                        compaction_buffers_map_.Size() - adjustment);
  } else if (shadow_to_space_map_.Size() == bump_pointer_space_->Capacity()) {
    compaction_buffers_map_.SetSize(kPageSize);
  }
  info_map_.MadviseDontNeedAndZero();
  live_words_bitmap_->ClearBitmap();
  moving_space_bitmap_->Clear();
  if (UNLIKELY(is_zygote && IsValidFd(uffd_))) {
    heap_->DeleteThreadPool();
    close(uffd_);
    uffd_ = kFdUnused;
    uffd_initialized_ = false;
  }
  CHECK(mark_stack_->IsEmpty());
  mark_stack_->Reset();
  DCHECK_EQ(thread_running_gc_, Thread::Current());
  if (kIsDebugBuild) {
    MutexLock mu(thread_running_gc_, lock_);
    if (updated_roots_.get() != nullptr) {
      updated_roots_->clear();
    }
  }
  class_after_obj_ordered_map_.clear();
  delete[] moving_pages_status_;
  linear_alloc_arenas_.clear();
  {
    ReaderMutexLock mu(thread_running_gc_, *Locks::mutator_lock_);
    WriterMutexLock mu2(thread_running_gc_, *Locks::heap_bitmap_lock_);
    heap_->ClearMarkedObjects();
  }
  std::swap(moving_to_space_fd_, moving_from_space_fd_);
  if (IsValidFd(moving_to_space_fd_)) {
    struct stat buf;
    DCHECK_EQ(fstat(moving_to_space_fd_, &buf), 0) << "fstat failed: " << strerror(errno);
    DCHECK_EQ(buf.st_blocks, 0u);
  }
}
}
}
