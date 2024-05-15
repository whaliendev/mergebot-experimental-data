#ifndef ART_RUNTIME_GC_HEAP_H_
#define ART_RUNTIME_GC_HEAP_H_ 
#include <iosfwd>
#include <string>
#include <unordered_set>
#include <vector>
#include <android-base/logging.h>
#include "allocator_type.h"
#include "base/atomic.h"
#include "base/histogram.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "base/runtime_debug.h"
#include "base/safe_map.h"
#include "base/time_utils.h"
#include "gc/collector/gc_type.h"
#include "gc/collector/iteration.h"
#include "gc/collector/mark_compact.h"
#include "gc/collector_type.h"
#include "gc/gc_cause.h"
#include "gc/space/large_object_space.h"
#include "handle.h"
#include "obj_ptr.h"
#include "offsets.h"
#include "process_state.h"
#include "read_barrier_config.h"
#include "runtime_globals.h"
#include "verify_object.h"
namespace art {
class ConditionVariable;
enum class InstructionSet;
class IsMarkedVisitor;
class Mutex;
class ReflectiveValueVisitor;
class RootVisitor;
class StackVisitor;
class Thread;
class ThreadPool;
class TimingLogger;
class VariableSizedHandleScope;
namespace mirror {
class Class;
class Object;
}
namespace gc {
class AllocationListener;
class AllocRecordObjectMap;
class GcPauseListener;
class HeapTask;
class ReferenceProcessor;
class TaskProcessor;
class Verification;
namespace accounting {
template <typename T> class AtomicStack;
using ObjectStack = AtomicStack<mirror::Object>;
class CardTable;
class HeapBitmap;
class ModUnionTable;
class ReadBarrierTable;
class RememberedSet;
}
namespace collector {
class ConcurrentCopying;
class GarbageCollector;
class MarkSweep;
class SemiSpace;
}
namespace allocator {
class RosAlloc;
}
namespace space {
class AllocSpace;
class BumpPointerSpace;
class ContinuousMemMapAllocSpace;
class DiscontinuousSpace;
class DlMallocSpace;
class ImageSpace;
class LargeObjectSpace;
class MallocSpace;
class RegionSpace;
class RosAllocSpace;
class Space;
class ZygoteSpace;
}
enum HomogeneousSpaceCompactResult {
  kSuccess,
  kErrorReject,
  kErrorUnsupported,
  kErrorVMShuttingDown,
};
static constexpr bool kUseRosAlloc = true;
static constexpr bool kUseThreadLocalAllocationStack = true;
class Heap {
 public:
  static constexpr size_t kPartialTlabSize = 16 * KB;
  static constexpr bool kUsePartialTlabs = true;
  static constexpr size_t kDefaultStartingSize = kPageSize;
  static constexpr size_t kDefaultInitialSize = 2 * MB;
  static constexpr size_t kDefaultMaximumSize = 256 * MB;
  static constexpr size_t kDefaultNonMovingSpaceCapacity = 64 * MB;
  static constexpr size_t kDefaultMaxFree = 2 * MB;
  static constexpr size_t kDefaultMinFree = kDefaultMaxFree / 4;
  static constexpr size_t kDefaultLongPauseLogThreshold = MsToNs(5);
  static constexpr size_t kDefaultLongPauseLogThresholdGcStress = MsToNs(50);
  static constexpr size_t kDefaultLongGCLogThreshold = MsToNs(100);
  static constexpr size_t kDefaultLongGCLogThresholdGcStress = MsToNs(1000);
  static constexpr size_t kDefaultTLABSize = 32 * KB;
  static constexpr double kDefaultTargetUtilization = 0.75;
  static constexpr double kDefaultHeapGrowthMultiplier = 2.0;
  static constexpr size_t kMinLargeObjectThreshold = 3 * kPageSize;
  static constexpr size_t kDefaultLargeObjectThreshold = kMinLargeObjectThreshold;
  static constexpr bool kDefaultEnableParallelGC = true;
  static uint8_t* const kPreferredAllocSpaceBegin;
  static constexpr space::LargeObjectSpaceType kDefaultLargeObjectSpaceType =
      USE_ART_LOW_4G_ALLOCATOR ?
          space::LargeObjectSpaceType::kFreeList
        : space::LargeObjectSpaceType::kMap;
  static constexpr size_t kTimeAdjust = 1024;
#ifdef __ANDROID__
  static constexpr uint32_t kNotifyNativeInterval = 64;
#else
  static constexpr uint32_t kNotifyNativeInterval = 384;
#endif
  static constexpr size_t kCheckImmediatelyThreshold = 300000;
  static constexpr uint64_t kHeapTrimWait = MsToNs(5000);
  static constexpr uint64_t kCollectorTransitionWait = MsToNs(5000);
  DECLARE_RUNTIME_DEBUG_FLAG(kStressCollectorTransition);
  Heap(size_t initial_size,
       size_t growth_limit,
       size_t min_free,
       size_t max_free,
       double target_utilization,
       double foreground_heap_growth_multiplier,
       size_t stop_for_native_allocs,
       size_t capacity,
       size_t non_moving_space_capacity,
       const std::vector<std::string>& boot_class_path,
       const std::vector<std::string>& boot_class_path_locations,
       const std::vector<int>& boot_class_path_fds,
       const std::vector<int>& boot_class_path_image_fds,
       const std::vector<int>& boot_class_path_vdex_fds,
       const std::vector<int>& boot_class_path_oat_fds,
       const std::vector<std::string>& image_file_names,
       InstructionSet image_instruction_set,
       CollectorType foreground_collector_type,
       CollectorType background_collector_type,
       space::LargeObjectSpaceType large_object_space_type,
       size_t large_object_threshold,
       size_t parallel_gc_threads,
       size_t conc_gc_threads,
       bool low_memory_mode,
       size_t long_pause_threshold,
       size_t long_gc_threshold,
       bool ignore_target_footprint,
       bool always_log_explicit_gcs,
       bool use_tlab,
       bool verify_pre_gc_heap,
       bool verify_pre_sweeping_heap,
       bool verify_post_gc_heap,
       bool verify_pre_gc_rosalloc,
       bool verify_pre_sweeping_rosalloc,
       bool verify_post_gc_rosalloc,
       bool gc_stress_mode,
       bool measure_gc_performance,
       bool use_homogeneous_space_compaction,
       bool use_generational_cc,
       uint64_t min_interval_homogeneous_space_compaction_by_oom,
       bool dump_region_info_before_gc,
       bool dump_region_info_after_gc);
  ~Heap();
  template <bool kInstrumented = true, typename PreFenceVisitor>
  mirror::Object* AllocObject(Thread* self,
                              ObjPtr<mirror::Class> klass,
                              size_t num_bytes,
                              const PreFenceVisitor& pre_fence_visitor)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!*gc_complete_lock_,
               !*pending_task_lock_,
               !*backtrace_lock_,
               !process_state_update_lock_,
               !Roles::uninterruptible_) {
    return AllocObjectWithAllocator<kInstrumented>(self,
                                                   klass,
                                                   num_bytes,
                                                   GetCurrentAllocator(),
                                                   pre_fence_visitor);
  }
  template <bool kInstrumented = true, typename PreFenceVisitor>
  mirror::Object* AllocNonMovableObject(Thread* self,
                                        ObjPtr<mirror::Class> klass,
                                        size_t num_bytes,
                                        const PreFenceVisitor& pre_fence_visitor)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!*gc_complete_lock_,
               !*pending_task_lock_,
               !*backtrace_lock_,
               !process_state_update_lock_,
               !Roles::uninterruptible_) {
    mirror::Object* obj = AllocObjectWithAllocator<kInstrumented>(self,
                                                                  klass,
                                                                  num_bytes,
                                                                  GetCurrentNonMovingAllocator(),
                                                                  pre_fence_visitor);
    JHPCheckNonTlabSampleAllocation(self, obj, num_bytes);
    return obj;
  }
  template <bool kInstrumented = true, bool kCheckLargeObject = true, typename PreFenceVisitor>
  ALWAYS_INLINE mirror::Object* AllocObjectWithAllocator(Thread* self,
                                                         ObjPtr<mirror::Class> klass,
                                                         size_t byte_count,
                                                         AllocatorType allocator,
                                                         const PreFenceVisitor& pre_fence_visitor)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!*gc_complete_lock_,
               !*pending_task_lock_,
               !*backtrace_lock_,
               !process_state_update_lock_,
               !Roles::uninterruptible_);
  AllocatorType GetCurrentAllocator() const {
    return current_allocator_;
  }
  AllocatorType GetCurrentNonMovingAllocator() const {
    return current_non_moving_allocator_;
  }
  AllocatorType GetUpdatedAllocator(AllocatorType old_allocator) {
    return (old_allocator == kAllocatorTypeNonMoving) ?
        GetCurrentNonMovingAllocator() : GetCurrentAllocator();
  }
  template <typename Visitor>
  ALWAYS_INLINE void VisitObjects(Visitor&& visitor)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::heap_bitmap_lock_, !*gc_complete_lock_);
  template <typename Visitor>
  ALWAYS_INLINE void VisitObjectsPaused(Visitor&& visitor)
      REQUIRES(Locks::mutator_lock_, !Locks::heap_bitmap_lock_, !*gc_complete_lock_);
  void VisitReflectiveTargets(ReflectiveValueVisitor* visitor)
      REQUIRES(Locks::mutator_lock_, !Locks::heap_bitmap_lock_, !*gc_complete_lock_);
  void CheckPreconditionsForAllocObject(ObjPtr<mirror::Class> c, size_t byte_count)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void RegisterNativeAllocation(JNIEnv* env, size_t bytes)
      REQUIRES(!*gc_complete_lock_, !*pending_task_lock_, !process_state_update_lock_);
  void RegisterNativeFree(JNIEnv* env, size_t bytes);
  void NotifyNativeAllocations(JNIEnv* env)
      REQUIRES(!*gc_complete_lock_, !*pending_task_lock_, !process_state_update_lock_);
  uint32_t GetNotifyNativeInterval() {
    return kNotifyNativeInterval;
  }
  void ChangeAllocator(AllocatorType allocator)
      REQUIRES(Locks::mutator_lock_, !Locks::runtime_shutdown_lock_);
  void ChangeCollector(CollectorType collector_type)
      REQUIRES(Locks::mutator_lock_, !*gc_complete_lock_);
  void VerifyObjectBody(ObjPtr<mirror::Object> o) NO_THREAD_SAFETY_ANALYSIS;
  void VerifyHeap() REQUIRES(!Locks::heap_bitmap_lock_);
  size_t VerifyHeapReferences(bool verify_referents = true)
      REQUIRES(Locks::mutator_lock_, !*gc_complete_lock_);
  bool VerifyMissingCardMarks()
      REQUIRES(Locks::heap_bitmap_lock_, Locks::mutator_lock_);
  bool IsValidObjectAddress(const void* obj) const REQUIRES_SHARED(Locks::mutator_lock_);
  bool IsNonDiscontinuousSpaceHeapAddress(const void* addr) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  bool IsLiveObjectLocked(ObjPtr<mirror::Object> obj,
                          bool search_allocation_stack = true,
                          bool search_live_stack = true,
                          bool sorted = false)
      REQUIRES_SHARED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);
  bool IsMovableObject(ObjPtr<mirror::Object> obj) const REQUIRES_SHARED(Locks::mutator_lock_);
  void IncrementDisableMovingGC(Thread* self) REQUIRES(!*gc_complete_lock_);
  void DecrementDisableMovingGC(Thread* self) REQUIRES(!*gc_complete_lock_);
  void IncrementDisableThreadFlip(Thread* self) REQUIRES(!*thread_flip_lock_);
  void DecrementDisableThreadFlip(Thread* self) REQUIRES(!*thread_flip_lock_);
  void ThreadFlipBegin(Thread* self) REQUIRES(!*thread_flip_lock_);
  void ThreadFlipEnd(Thread* self) REQUIRES(!*thread_flip_lock_);
  void EnsureObjectUserfaulted(ObjPtr<mirror::Object> obj) REQUIRES_SHARED(Locks::mutator_lock_);
  void ClearMarkedObjects()
      REQUIRES(Locks::heap_bitmap_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void CollectGarbage(bool clear_soft_references, GcCause cause = kGcCauseExplicit)
      REQUIRES(!*gc_complete_lock_, !*pending_task_lock_, !process_state_update_lock_);
  void ConcurrentGC(Thread* self, GcCause cause, bool force_full, uint32_t requested_gc_num)
      REQUIRES(!Locks::runtime_shutdown_lock_, !*gc_complete_lock_,
               !*pending_task_lock_, !process_state_update_lock_);
  void CountInstances(const std::vector<Handle<mirror::Class>>& classes,
                      bool use_is_assignable_from,
                      uint64_t* counts)
      REQUIRES(!Locks::heap_bitmap_lock_, !*gc_complete_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void ClearGrowthLimit() REQUIRES(!*gc_complete_lock_);
  void ClampGrowthLimit() REQUIRES(!Locks::heap_bitmap_lock_);
  double GetTargetHeapUtilization() const {
    return target_utilization_;
  }
  void RegisterGCAllocation(size_t bytes);
  void RegisterGCDeAllocation(size_t bytes);
  void SetSpaceAsDefault(space::ContinuousSpace* continuous_space)
      REQUIRES(!Locks::heap_bitmap_lock_);
  void AddSpace(space::Space* space)
      REQUIRES(!Locks::heap_bitmap_lock_)
      REQUIRES(Locks::mutator_lock_);
  void RemoveSpace(space::Space* space)
    REQUIRES(!Locks::heap_bitmap_lock_)
    REQUIRES(Locks::mutator_lock_);
  double GetPreGcWeightedAllocatedBytes() const {
    return pre_gc_weighted_allocated_bytes_;
  }
  double GetPostGcWeightedAllocatedBytes() const {
    return post_gc_weighted_allocated_bytes_;
  }
  void CalculatePreGcWeightedAllocatedBytes();
  void CalculatePostGcWeightedAllocatedBytes();
  uint64_t GetTotalGcCpuTime();
  uint64_t GetProcessCpuStartTime() const {
    return process_cpu_start_time_ns_;
  }
  uint64_t GetPostGCLastProcessCpuTime() const {
    return post_gc_last_process_cpu_time_ns_;
  }
  void SetTargetHeapUtilization(float target);
  void SetIdealFootprint(size_t max_allowed_footprint);
  collector::GcType WaitForGcToComplete(GcCause cause, Thread* self) REQUIRES(!*gc_complete_lock_);
  void UpdateProcessState(ProcessState old_process_state, ProcessState new_process_state)
      REQUIRES(!*pending_task_lock_, !*gc_complete_lock_, !process_state_update_lock_);
  bool HaveContinuousSpaces() const NO_THREAD_SAFETY_ANALYSIS {
    return !continuous_spaces_.empty();
  }
  const std::vector<space::ContinuousSpace*>& GetContinuousSpaces() const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    return continuous_spaces_;
  }
  const std::vector<space::DiscontinuousSpace*>& GetDiscontinuousSpaces() const {
    return discontinuous_spaces_;
  }
  const collector::Iteration* GetCurrentGcIteration() const {
    return &current_gc_iteration_;
  }
  collector::Iteration* GetCurrentGcIteration() {
    return &current_gc_iteration_;
  }
  void EnableObjectValidation() {
    verify_object_mode_ = kVerifyObjectSupport;
    if (verify_object_mode_ > kVerifyObjectModeDisabled) {
      VerifyHeap();
    }
  }
  void DisableObjectValidation() {
    verify_object_mode_ = kVerifyObjectModeDisabled;
  }
  bool IsObjectValidationEnabled() const {
    return verify_object_mode_ > kVerifyObjectModeDisabled;
  }
  bool IsLowMemoryMode() const {
    return low_memory_mode_;
  }
  double HeapGrowthMultiplier() const;
  void RecordFree(uint64_t freed_objects, int64_t freed_bytes);
  void RecordFreeRevoke();
  accounting::CardTable* GetCardTable() const {
    return card_table_.get();
  }
  accounting::ReadBarrierTable* GetReadBarrierTable() const {
    return rb_table_.get();
  }
  void AddFinalizerReference(Thread* self, ObjPtr<mirror::Object>* object);
  size_t GetBytesAllocated() const {
    return num_bytes_allocated_.load(std::memory_order_relaxed);
  }
  bool GetUseGenerationalCC() const {
    return use_generational_cc_;
  }
  size_t GetObjectsAllocated() const
      REQUIRES(!Locks::heap_bitmap_lock_);
  uint64_t GetObjectsAllocatedEver() const;
  uint64_t GetBytesAllocatedEver() const;
  uint64_t GetObjectsFreedEver(std::memory_order mo = std::memory_order_relaxed) const {
    return total_objects_freed_ever_.load(mo);
  }
  uint64_t GetBytesFreedEver(std::memory_order mo = std::memory_order_relaxed) const {
    return total_bytes_freed_ever_.load(mo);
  }
  space::RegionSpace* GetRegionSpace() const {
    return region_space_;
  }
  space::BumpPointerSpace* GetBumpPointerSpace() const {
    return bump_pointer_space_;
  }
  size_t GetMaxMemory() const {
    return std::max(GetBytesAllocated(), growth_limit_);
  }
  size_t GetTotalMemory() const;
  size_t GetFreeMemoryUntilGC() const {
    return UnsignedDifference(target_footprint_.load(std::memory_order_relaxed),
                              GetBytesAllocated());
  }
  size_t GetFreeMemoryUntilOOME() const {
    return UnsignedDifference(growth_limit_, GetBytesAllocated());
  }
  size_t GetFreeMemory() const {
    return UnsignedDifference(GetTotalMemory(),
                              num_bytes_allocated_.load(std::memory_order_relaxed));
  }
  space::ContinuousSpace* FindContinuousSpaceFromObject(ObjPtr<mirror::Object>, bool fail_ok) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  space::ContinuousSpace* FindContinuousSpaceFromAddress(const mirror::Object* addr) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  space::DiscontinuousSpace* FindDiscontinuousSpaceFromObject(ObjPtr<mirror::Object>,
                                                              bool fail_ok) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  space::Space* FindSpaceFromObject(ObjPtr<mirror::Object> obj, bool fail_ok) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  space::Space* FindSpaceFromAddress(const void* ptr) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  std::string DumpSpaceNameFromAddress(const void* addr) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  void DumpForSigQuit(std::ostream& os) REQUIRES(!*gc_complete_lock_);
  void DoPendingCollectorTransition()
      REQUIRES(!*gc_complete_lock_, !*pending_task_lock_, !process_state_update_lock_);
  void Trim(Thread* self) REQUIRES(!*gc_complete_lock_);
  void RevokeThreadLocalBuffers(Thread* thread);
  void RevokeRosAllocThreadLocalBuffers(Thread* thread);
  void RevokeAllThreadLocalBuffers();
  void AssertThreadLocalBuffersAreRevoked(Thread* thread);
  void AssertAllBumpPointerSpaceThreadLocalBuffersAreRevoked();
  void RosAllocVerification(TimingLogger* timings, const char* name)
      REQUIRES(Locks::mutator_lock_);
  accounting::HeapBitmap* GetLiveBitmap() REQUIRES_SHARED(Locks::heap_bitmap_lock_) {
    return live_bitmap_.get();
  }
  accounting::HeapBitmap* GetMarkBitmap() REQUIRES_SHARED(Locks::heap_bitmap_lock_) {
    return mark_bitmap_.get();
  }
  accounting::ObjectStack* GetLiveStack() REQUIRES_SHARED(Locks::heap_bitmap_lock_) {
    return live_stack_.get();
  }
  accounting::ObjectStack* GetAllocationStack() REQUIRES_SHARED(Locks::heap_bitmap_lock_) {
    return allocation_stack_.get();
  }
  void PreZygoteFork() NO_THREAD_SAFETY_ANALYSIS;
  void FlushAllocStack()
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(Locks::heap_bitmap_lock_);
  void RevokeAllThreadLocalAllocationStacks(Thread* self)
      REQUIRES(Locks::mutator_lock_, !Locks::runtime_shutdown_lock_, !Locks::thread_list_lock_);
  void MarkAllocStack(accounting::SpaceBitmap<kObjectAlignment>* bitmap1,
                      accounting::SpaceBitmap<kObjectAlignment>* bitmap2,
                      accounting::SpaceBitmap<kLargeObjectAlignment>* large_objects,
                      accounting::ObjectStack* stack)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(Locks::heap_bitmap_lock_);
  void MarkAllocStackAsLive(accounting::ObjectStack* stack)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(Locks::heap_bitmap_lock_);
  void UnBindBitmaps()
      REQUIRES(Locks::heap_bitmap_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);
  const std::vector<space::ImageSpace*>& GetBootImageSpaces() const {
    return boot_image_spaces_;
  }
  bool ObjectIsInBootImageSpace(ObjPtr<mirror::Object> obj) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  bool IsInBootImageOatFile(const void* p) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  uint32_t GetBootImagesStartAddress() const {
    return boot_images_start_address_;
  }
  uint32_t GetBootImagesSize() const {
    return boot_images_size_;
  }
  bool IsBootImageAddress(const void* p) const {
    return reinterpret_cast<uintptr_t>(p) - boot_images_start_address_ < boot_images_size_;
  }
  space::DlMallocSpace* GetDlMallocSpace() const {
    return dlmalloc_space_;
  }
  space::RosAllocSpace* GetRosAllocSpace() const {
    return rosalloc_space_;
  }
  space::RosAllocSpace* GetRosAllocSpace(gc::allocator::RosAlloc* rosalloc) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  space::MallocSpace* GetNonMovingSpace() const {
    return non_moving_space_;
  }
  space::LargeObjectSpace* GetLargeObjectsSpace() const {
    return large_object_space_;
  }
  space::MallocSpace* GetPrimaryFreeListSpace() {
    if (kUseRosAlloc) {
      DCHECK(rosalloc_space_ != nullptr);
      return reinterpret_cast<space::MallocSpace*>(rosalloc_space_);
    } else {
      DCHECK(dlmalloc_space_ != nullptr);
      return reinterpret_cast<space::MallocSpace*>(dlmalloc_space_);
    }
  }
  void DumpSpaces(std::ostream& stream) const REQUIRES_SHARED(Locks::mutator_lock_);
  std::string DumpSpaces() const REQUIRES_SHARED(Locks::mutator_lock_);
  void DumpGcPerformanceInfo(std::ostream& os)
      REQUIRES(!*gc_complete_lock_);
  void ResetGcPerformanceInfo() REQUIRES(!*gc_complete_lock_);
  void CreateThreadPool(size_t num_threads = 0);
  void WaitForWorkersToBeCreated();
  void DeleteThreadPool();
  ThreadPool* GetThreadPool() {
    return thread_pool_.get();
  }
  size_t GetParallelGCThreadCount() const {
    return parallel_gc_threads_;
  }
  size_t GetConcGCThreadCount() const {
    return conc_gc_threads_;
  }
  accounting::ModUnionTable* FindModUnionTableFromSpace(space::Space* space);
  void AddModUnionTable(accounting::ModUnionTable* mod_union_table);
  accounting::RememberedSet* FindRememberedSetFromSpace(space::Space* space);
  void AddRememberedSet(accounting::RememberedSet* remembered_set);
  void RemoveRememberedSet(space::Space* space);
  bool IsCompilingBoot() const;
  bool HasBootImageSpace() const {
    return !boot_image_spaces_.empty();
  }
  ReferenceProcessor* GetReferenceProcessor() {
    return reference_processor_.get();
  }
  TaskProcessor* GetTaskProcessor() {
    return task_processor_.get();
  }
  bool HasZygoteSpace() const {
    return zygote_space_ != nullptr;
  }
  collector::ConcurrentCopying* ConcurrentCopyingCollector() {
    collector::ConcurrentCopying* active_collector =
            active_concurrent_copying_collector_.load(std::memory_order_relaxed);
    if (use_generational_cc_) {
      DCHECK((active_collector == concurrent_copying_collector_) ||
             (active_collector == young_concurrent_copying_collector_))
              << "active_concurrent_copying_collector: " << active_collector
              << " young_concurrent_copying_collector: " << young_concurrent_copying_collector_
              << " concurrent_copying_collector: " << concurrent_copying_collector_;
    } else {
      DCHECK_EQ(active_collector, concurrent_copying_collector_);
    }
    return active_collector;
  }
  collector::MarkCompact* MarkCompactCollector() {
    DCHECK(!gUseUserfaultfd || mark_compact_ != nullptr);
    return mark_compact_;
  }
  bool IsPerformingUffdCompaction() { return gUseUserfaultfd && mark_compact_->IsCompacting(); }
  CollectorType CurrentCollectorType() const {
    DCHECK(!gUseUserfaultfd || collector_type_ == kCollectorTypeCMC);
    return collector_type_;
  }
  bool IsMovingGc() const { return IsMovingGc(CurrentCollectorType()); }
  CollectorType GetForegroundCollectorType() const { return foreground_collector_type_; }
  bool IsGcConcurrentAndMoving() const {
    if (IsGcConcurrent() && IsMovingGc(collector_type_)) {
      DCHECK_EQ(collector_type_, foreground_collector_type_);
      return true;
    }
    return false;
  }
  bool IsMovingGCDisabled(Thread* self) REQUIRES(!*gc_complete_lock_) {
    MutexLock mu(self, *gc_complete_lock_);
    return disable_moving_gc_count_ > 0;
  }
  void RequestTrim(Thread* self) REQUIRES(!*pending_task_lock_);
  uint32_t GetCurrentGcNum() {
    return gcs_completed_.load(std::memory_order_acquire);
  }
  bool RequestConcurrentGC(Thread* self, GcCause cause, bool force_full, uint32_t observed_gc_num)
      REQUIRES(!*pending_task_lock_);
  bool MayUseCollector(CollectorType type) const;
  void SetMinIntervalHomogeneousSpaceCompactionByOom(uint64_t interval) {
    min_interval_homogeneous_space_compaction_by_oom_ = interval;
  }
  uint64_t GetGcCount() const;
  uint64_t GetGcTime() const;
  uint64_t GetBlockingGcCount() const;
  uint64_t GetBlockingGcTime() const;
  void DumpGcCountRateHistogram(std::ostream& os) const REQUIRES(!*gc_complete_lock_);
  void DumpBlockingGcCountRateHistogram(std::ostream& os) const REQUIRES(!*gc_complete_lock_);
  uint64_t GetTotalTimeWaitingForGC() const {
    return total_wait_time_;
  }
  uint64_t GetPreOomeGcCount() const;
  HeapSampler& GetHeapSampler() {
    return heap_sampler_;
  }
  void InitPerfettoJavaHeapProf();
  int CheckPerfettoJHPEnabled();
  void JHPCheckNonTlabSampleAllocation(Thread* self,
                                       mirror::Object* ret,
                                       size_t alloc_size);
  size_t JHPCalculateNextTlabSize(Thread* self,
                                  size_t jhp_def_tlab_size,
                                  size_t alloc_size,
                                  bool* take_sample,
                                  size_t* bytes_until_sample);
  void AdjustSampleOffset(size_t adjustment);
  bool IsAllocTrackingEnabled() const {
    return alloc_tracking_enabled_.load(std::memory_order_relaxed);
  }
  void SetAllocTrackingEnabled(bool enabled) REQUIRES(Locks::alloc_tracker_lock_) {
    alloc_tracking_enabled_.store(enabled, std::memory_order_relaxed);
  }
  size_t GetAllocTrackerStackDepth() const {
    return alloc_record_depth_;
  }
  void SetAllocTrackerStackDepth(size_t alloc_record_depth) {
    alloc_record_depth_ = alloc_record_depth;
  }
  AllocRecordObjectMap* GetAllocationRecords() const REQUIRES(Locks::alloc_tracker_lock_) {
    return allocation_records_.get();
  }
  void SetAllocationRecords(AllocRecordObjectMap* records)
      REQUIRES(Locks::alloc_tracker_lock_);
  void VisitAllocationRecords(RootVisitor* visitor) const
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::alloc_tracker_lock_);
  void SweepAllocationRecords(IsMarkedVisitor* visitor) const
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::alloc_tracker_lock_);
  void DisallowNewAllocationRecords() const
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::alloc_tracker_lock_);
  void AllowNewAllocationRecords() const
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::alloc_tracker_lock_);
  void BroadcastForNewAllocationRecords() const
      REQUIRES(!Locks::alloc_tracker_lock_);
  void DisableGCForShutdown() REQUIRES(!*gc_complete_lock_);
  bool IsGCDisabledForShutdown() const REQUIRES(!*gc_complete_lock_);
  HomogeneousSpaceCompactResult PerformHomogeneousSpaceCompact()
      REQUIRES(!*gc_complete_lock_, !process_state_update_lock_);
  bool SupportHomogeneousSpaceCompactAndCollectorTransitions() const;
  void SetAllocationListener(AllocationListener* l);
  void RemoveAllocationListener();
  void SetGcPauseListener(GcPauseListener* l);
  GcPauseListener* GetGcPauseListener() {
    return gc_pause_listener_.load(std::memory_order_acquire);
  }
  void RemoveGcPauseListener();
  const Verification* GetVerification() const;
  void PostForkChildAction(Thread* self) REQUIRES(!*gc_complete_lock_);
  void TraceHeapSize(size_t heap_size);
  bool AddHeapTask(gc::HeapTask* task);
 private:
  class ConcurrentGCTask;
  class CollectorTransitionTask;
  class HeapTrimTask;
  class TriggerPostForkCCGcTask;
  class ReduceTargetFootprintTask;
  collector::GarbageCollector* Compact(space::ContinuousMemMapAllocSpace* target_space,
                                       space::ContinuousMemMapAllocSpace* source_space,
                                       GcCause gc_cause)
      REQUIRES(Locks::mutator_lock_);
  void LogGC(GcCause gc_cause, collector::GarbageCollector* collector);
  void StartGC(Thread* self, GcCause cause, CollectorType collector_type)
      REQUIRES(!*gc_complete_lock_);
  void FinishGC(Thread* self, collector::GcType gc_type) REQUIRES(!*gc_complete_lock_);
  double CalculateGcWeightedAllocatedBytes(uint64_t gc_last_process_cpu_time_ns,
                                           uint64_t current_process_cpu_time) const;
  static MemMap MapAnonymousPreferredAddress(const char* name,
                                             uint8_t* request_begin,
                                             size_t capacity,
                                             std::string* out_error_str);
  bool SupportHSpaceCompaction() const {
    return main_space_backup_ != nullptr;
  }
  static ALWAYS_INLINE size_t UnsignedDifference(size_t x, size_t y) {
    return x > y ? x - y : 0;
  }
  static ALWAYS_INLINE size_t UnsignedSum(size_t x, size_t y) {
    return x + y >= x ? x + y : std::numeric_limits<size_t>::max();
  }
  static ALWAYS_INLINE bool AllocatorHasAllocationStack(AllocatorType allocator_type) {
    return
        allocator_type != kAllocatorTypeRegionTLAB &&
        allocator_type != kAllocatorTypeBumpPointer &&
        allocator_type != kAllocatorTypeTLAB &&
        allocator_type != kAllocatorTypeRegion;
  }
  static bool IsMovingGc(CollectorType collector_type) {
    return
        collector_type == kCollectorTypeCC ||
        collector_type == kCollectorTypeSS ||
        collector_type == kCollectorTypeCMC ||
        collector_type == kCollectorTypeCCBackground ||
        collector_type == kCollectorTypeHomogeneousSpaceCompact;
  }
  bool ShouldAllocLargeObject(ObjPtr<mirror::Class> c, size_t byte_count) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  ALWAYS_INLINE bool ShouldConcurrentGCForJava(size_t new_num_bytes_allocated);
  float NativeMemoryOverTarget(size_t current_native_bytes, bool is_gc_concurrent);
  void CheckGCForNative(Thread* self)
      REQUIRES(!*pending_task_lock_, !*gc_complete_lock_, !process_state_update_lock_);
  accounting::ObjectStack* GetMarkStack() {
    return mark_stack_.get();
  }
  template <bool kInstrumented, typename PreFenceVisitor>
  mirror::Object* AllocLargeObject(Thread* self,
                                   ObjPtr<mirror::Class>* klass,
                                   size_t byte_count,
                                   const PreFenceVisitor& pre_fence_visitor)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!*gc_complete_lock_, !*pending_task_lock_,
               !*backtrace_lock_, !process_state_update_lock_);
  mirror::Object* AllocateInternalWithGc(Thread* self,
                                         AllocatorType allocator,
                                         bool instrumented,
                                         size_t num_bytes,
                                         size_t* bytes_allocated,
                                         size_t* usable_size,
                                         size_t* bytes_tl_bulk_allocated,
                                         ObjPtr<mirror::Class>* klass)
      REQUIRES(!Locks::thread_suspend_count_lock_, !*gc_complete_lock_, !*pending_task_lock_)
      REQUIRES(Roles::uninterruptible_)
      REQUIRES_SHARED(Locks::mutator_lock_);
  mirror::Object* AllocateInto(Thread* self,
                               space::AllocSpace* space,
                               ObjPtr<mirror::Class> c,
                               size_t bytes)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void SwapSemiSpaces() REQUIRES(Locks::mutator_lock_);
  template <const bool kInstrumented, const bool kGrow>
  ALWAYS_INLINE mirror::Object* TryToAllocate(Thread* self,
                                              AllocatorType allocator_type,
                                              size_t alloc_size,
                                              size_t* bytes_allocated,
                                              size_t* usable_size,
                                              size_t* bytes_tl_bulk_allocated)
      REQUIRES_SHARED(Locks::mutator_lock_);
  mirror::Object* AllocWithNewTLAB(Thread* self,
                                   AllocatorType allocator_type,
                                   size_t alloc_size,
                                   bool grow,
                                   size_t* bytes_allocated,
                                   size_t* usable_size,
                                   size_t* bytes_tl_bulk_allocated)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void ThrowOutOfMemoryError(Thread* self, size_t byte_count, AllocatorType allocator_type)
      REQUIRES_SHARED(Locks::mutator_lock_);
  ALWAYS_INLINE bool IsOutOfMemoryOnAllocation(AllocatorType allocator_type,
                                               size_t alloc_size,
                                               bool grow);
  collector::GcType WaitForGcToCompleteLocked(GcCause cause, Thread* self)
      REQUIRES(gc_complete_lock_);
  void RequestCollectorTransition(CollectorType desired_collector_type, uint64_t delta_time)
      REQUIRES(!*pending_task_lock_);
  void RequestConcurrentGCAndSaveObject(Thread* self,
                                        bool force_full,
                                        uint32_t observed_gc_num,
                                        ObjPtr<mirror::Object>* obj)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!*pending_task_lock_);
  static constexpr uint32_t GC_NUM_ANY = std::numeric_limits<uint32_t>::max();
  collector::GcType CollectGarbageInternal(collector::GcType gc_plan,
                                           GcCause gc_cause,
                                           bool clear_soft_references,
                                           uint32_t requested_gc_num)
      REQUIRES(!*gc_complete_lock_, !Locks::heap_bitmap_lock_, !Locks::thread_suspend_count_lock_,
               !*pending_task_lock_, !process_state_update_lock_);
  void PreGcVerification(collector::GarbageCollector* gc)
      REQUIRES(!Locks::mutator_lock_, !*gc_complete_lock_);
  void PreGcVerificationPaused(collector::GarbageCollector* gc)
      REQUIRES(Locks::mutator_lock_, !*gc_complete_lock_);
  void PrePauseRosAllocVerification(collector::GarbageCollector* gc)
      REQUIRES(Locks::mutator_lock_);
  void PreSweepingGcVerification(collector::GarbageCollector* gc)
      REQUIRES(Locks::mutator_lock_, !Locks::heap_bitmap_lock_, !*gc_complete_lock_);
  void PostGcVerification(collector::GarbageCollector* gc)
      REQUIRES(!Locks::mutator_lock_, !*gc_complete_lock_);
  void PostGcVerificationPaused(collector::GarbageCollector* gc)
      REQUIRES(Locks::mutator_lock_, !*gc_complete_lock_);
  collector::GarbageCollector* FindCollectorByGcType(collector::GcType gc_type);
  void CreateMainMallocSpace(MemMap&& mem_map,
                             size_t initial_size,
                             size_t growth_limit,
                             size_t capacity);
  space::MallocSpace* CreateMallocSpaceFromMemMap(MemMap&& mem_map,
                                                  size_t initial_size,
                                                  size_t growth_limit,
                                                  size_t capacity,
                                                  const char* name,
                                                  bool can_move_objects);
  void GrowForUtilization(collector::GarbageCollector* collector_ran,
                          size_t bytes_allocated_before_gc = 0)
      REQUIRES(!process_state_update_lock_);
  size_t GetPercentFree();
  void SwapStacks() REQUIRES_SHARED(Locks::mutator_lock_);
  void ProcessCards(TimingLogger* timings,
                    bool use_rem_sets,
                    bool process_alloc_space_cards,
                    bool clear_alloc_space_cards)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void PushOnAllocationStack(Thread* self, ObjPtr<mirror::Object>* obj)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!*gc_complete_lock_, !*pending_task_lock_, !process_state_update_lock_);
  void PushOnAllocationStackWithInternalGC(Thread* self, ObjPtr<mirror::Object>* obj)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!*gc_complete_lock_, !*pending_task_lock_, !process_state_update_lock_);
  void PushOnThreadLocalAllocationStackWithInternalGC(Thread* thread, ObjPtr<mirror::Object>* obj)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!*gc_complete_lock_, !*pending_task_lock_, !process_state_update_lock_);
  void ClearPendingTrim(Thread* self) REQUIRES(!*pending_task_lock_);
  void ClearPendingCollectorTransition(Thread* self) REQUIRES(!*pending_task_lock_);
  bool IsGcConcurrent() const ALWAYS_INLINE {
    return collector_type_ == kCollectorTypeCC ||
        collector_type_ == kCollectorTypeCMC ||
        collector_type_ == kCollectorTypeCMS ||
        collector_type_ == kCollectorTypeCCBackground;
  }
  void TrimSpaces(Thread* self) REQUIRES(!*gc_complete_lock_);
  void TrimIndirectReferenceTables(Thread* self);
  template <typename Visitor>
  ALWAYS_INLINE void VisitObjectsInternal(Visitor&& visitor)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::heap_bitmap_lock_, !*gc_complete_lock_);
  template <typename Visitor>
  ALWAYS_INLINE void VisitObjectsInternalRegionSpace(Visitor&& visitor)
      REQUIRES(Locks::mutator_lock_, !Locks::heap_bitmap_lock_, !*gc_complete_lock_);
  void UpdateGcCountRateHistograms() REQUIRES(gc_complete_lock_);
  void CheckGcStressMode(Thread* self, ObjPtr<mirror::Object>* obj)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!*gc_complete_lock_, !*pending_task_lock_,
               !*backtrace_lock_, !process_state_update_lock_);
  collector::GcType NonStickyGcType() const {
    return HasZygoteSpace() ? collector::kGcTypePartial : collector::kGcTypeFull;
  }
  ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
    return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
  }
  ALWAYS_INLINE void IncrementNumberOfBytesFreedRevoke(size_t freed_bytes_revoke);
  void GrowHeapOnJankPerceptibleSwitch() REQUIRES(!process_state_update_lock_);
  void IncrementFreedEver();
  static void VlogHeapGrowth(size_t max_allowed_footprint, size_t new_footprint, size_t alloc_size);
  size_t GetNativeBytes();
  void SetDefaultConcurrentStartBytes() REQUIRES(!*gc_complete_lock_);
  void SetDefaultConcurrentStartBytesLocked();
  std::vector<space::ContinuousSpace*> continuous_spaces_ GUARDED_BY(Locks::mutator_lock_);
  std::vector<space::DiscontinuousSpace*> discontinuous_spaces_ GUARDED_BY(Locks::mutator_lock_);
  std::vector<space::AllocSpace*> alloc_spaces_;
  space::MallocSpace* non_moving_space_;
  space::RosAllocSpace* rosalloc_space_;
  space::DlMallocSpace* dlmalloc_space_;
  space::MallocSpace* main_space_;
  space::LargeObjectSpace* large_object_space_;
  std::unique_ptr<accounting::CardTable> card_table_;
  std::unique_ptr<accounting::ReadBarrierTable> rb_table_;
  AllocationTrackingSafeMap<space::Space*, accounting::ModUnionTable*, kAllocatorTagHeap>
      mod_union_tables_;
  AllocationTrackingSafeMap<space::Space*, accounting::RememberedSet*, kAllocatorTagHeap>
      remembered_sets_;
  CollectorType collector_type_;
  const CollectorType foreground_collector_type_;
  CollectorType background_collector_type_;
  CollectorType desired_collector_type_;
  Mutex* pending_task_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  const size_t parallel_gc_threads_;
  const size_t conc_gc_threads_;
  const bool low_memory_mode_;
  const size_t long_pause_log_threshold_;
  const size_t long_gc_log_threshold_;
  uint64_t process_cpu_start_time_ns_;
  uint64_t pre_gc_last_process_cpu_time_ns_;
  uint64_t post_gc_last_process_cpu_time_ns_;
  double pre_gc_weighted_allocated_bytes_;
  double post_gc_weighted_allocated_bytes_;
  const bool ignore_target_footprint_;
  const bool always_log_explicit_gcs_;
  Mutex zygote_creation_lock_;
  space::ZygoteSpace* zygote_space_;
  size_t large_object_threshold_;
  Mutex* gc_complete_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  std::unique_ptr<ConditionVariable> gc_complete_cond_ GUARDED_BY(gc_complete_lock_);
  Mutex* thread_flip_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  std::unique_ptr<ConditionVariable> thread_flip_cond_ GUARDED_BY(thread_flip_lock_);
  size_t disable_thread_flip_count_ GUARDED_BY(thread_flip_lock_);
  bool thread_flip_running_ GUARDED_BY(thread_flip_lock_);
  std::unique_ptr<ReferenceProcessor> reference_processor_;
  std::unique_ptr<TaskProcessor> task_processor_;
  volatile CollectorType collector_type_running_ GUARDED_BY(gc_complete_lock_);
  volatile GcCause last_gc_cause_ GUARDED_BY(gc_complete_lock_);
  volatile Thread* thread_running_gc_ GUARDED_BY(gc_complete_lock_);
  volatile collector::GcType last_gc_type_ GUARDED_BY(gc_complete_lock_);
  collector::GcType next_gc_type_;
  size_t capacity_;
  size_t growth_limit_;
  size_t initial_heap_size_;
  Atomic<size_t> target_footprint_;
  Mutex process_state_update_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  size_t min_foreground_target_footprint_ GUARDED_BY(process_state_update_lock_);
  size_t min_foreground_concurrent_start_bytes_ GUARDED_BY(process_state_update_lock_);
  size_t concurrent_start_bytes_;
  std::atomic<uint64_t> total_bytes_freed_ever_;
  std::atomic<uint64_t> total_objects_freed_ever_;
  Atomic<size_t> num_bytes_allocated_;
  Atomic<size_t> native_bytes_registered_;
  Atomic<size_t> old_native_bytes_allocated_;
  Atomic<uint32_t> native_objects_notified_;
  Atomic<size_t> num_bytes_freed_revoke_;
  size_t num_bytes_alive_after_gc_;
  collector::Iteration current_gc_iteration_;
  const bool verify_missing_card_marks_;
  const bool verify_system_weaks_;
  const bool verify_pre_gc_heap_;
  const bool verify_pre_sweeping_heap_;
  const bool verify_post_gc_heap_;
  const bool verify_mod_union_table_;
  bool verify_pre_gc_rosalloc_;
  bool verify_pre_sweeping_rosalloc_;
  bool verify_post_gc_rosalloc_;
  const bool gc_stress_mode_;
  class ScopedDisableRosAllocVerification {
   private:
    Heap* const heap_;
    const bool orig_verify_pre_gc_;
    const bool orig_verify_pre_sweeping_;
    const bool orig_verify_post_gc_;
   public:
    explicit ScopedDisableRosAllocVerification(Heap* heap)
        : heap_(heap),
          orig_verify_pre_gc_(heap_->verify_pre_gc_rosalloc_),
          orig_verify_pre_sweeping_(heap_->verify_pre_sweeping_rosalloc_),
          orig_verify_post_gc_(heap_->verify_post_gc_rosalloc_) {
      heap_->verify_pre_gc_rosalloc_ = false;
      heap_->verify_pre_sweeping_rosalloc_ = false;
      heap_->verify_post_gc_rosalloc_ = false;
    }
    ~ScopedDisableRosAllocVerification() {
      heap_->verify_pre_gc_rosalloc_ = orig_verify_pre_gc_;
      heap_->verify_pre_sweeping_rosalloc_ = orig_verify_pre_sweeping_;
      heap_->verify_post_gc_rosalloc_ = orig_verify_post_gc_;
    }
  };
  std::unique_ptr<ThreadPool> thread_pool_;
  std::unique_ptr<accounting::HeapBitmap> live_bitmap_ GUARDED_BY(Locks::heap_bitmap_lock_);
  std::unique_ptr<accounting::HeapBitmap> mark_bitmap_ GUARDED_BY(Locks::heap_bitmap_lock_);
  std::unique_ptr<accounting::ObjectStack> mark_stack_;
  const size_t max_allocation_stack_size_;
  std::unique_ptr<accounting::ObjectStack> allocation_stack_;
  std::unique_ptr<accounting::ObjectStack> live_stack_;
  AllocatorType current_allocator_;
  const AllocatorType current_non_moving_allocator_;
  std::vector<collector::GcType> gc_plan_;
  space::BumpPointerSpace* bump_pointer_space_;
  space::BumpPointerSpace* temp_space_;
  space::RegionSpace* region_space_;
  const size_t min_free_;
  const size_t max_free_;
  double target_utilization_;
  double foreground_heap_growth_multiplier_;
  const size_t stop_for_native_allocs_;
  uint64_t total_wait_time_;
  VerifyObjectMode verify_object_mode_;
  size_t disable_moving_gc_count_ GUARDED_BY(gc_complete_lock_);
  std::vector<collector::GarbageCollector*> garbage_collectors_;
  collector::SemiSpace* semi_space_collector_;
  collector::MarkCompact* mark_compact_;
  Atomic<collector::ConcurrentCopying*> active_concurrent_copying_collector_;
  collector::ConcurrentCopying* young_concurrent_copying_collector_;
  collector::ConcurrentCopying* concurrent_copying_collector_;
  const bool is_running_on_memory_tool_;
  const bool use_tlab_;
  std::unique_ptr<space::MallocSpace> main_space_backup_;
  uint64_t min_interval_homogeneous_space_compaction_by_oom_;
  uint64_t last_time_homogeneous_space_compaction_by_oom_;
  Atomic<size_t> count_delayed_oom_;
  Atomic<size_t> count_requested_homogeneous_space_compaction_;
  Atomic<size_t> count_ignored_homogeneous_space_compaction_;
  Atomic<size_t> count_performed_homogeneous_space_compaction_;
  Atomic<uint32_t> gcs_completed_;
  Atomic<uint32_t> max_gc_requested_;
  CollectorTransitionTask* pending_collector_transition_ GUARDED_BY(pending_task_lock_);
  HeapTrimTask* pending_heap_trim_ GUARDED_BY(pending_task_lock_);
  bool use_homogeneous_space_compaction_for_oom_;
  const bool use_generational_cc_;
  bool running_collection_is_blocking_ GUARDED_BY(gc_complete_lock_);
  uint64_t blocking_gc_count_;
  uint64_t blocking_gc_time_;
  static constexpr uint64_t kGcCountRateHistogramWindowDuration = MsToNs(10 * 1000);
  static constexpr uint64_t kGcCountRateHistogramMaxNumMissedWindows = 100;
  uint64_t last_update_time_gc_count_rate_histograms_;
  uint64_t gc_count_last_window_;
  uint64_t blocking_gc_count_last_window_;
  static constexpr size_t kGcCountRateMaxBucketCount = 200;
  Histogram<uint64_t> gc_count_rate_histogram_ GUARDED_BY(gc_complete_lock_);
  Histogram<uint64_t> blocking_gc_count_rate_histogram_ GUARDED_BY(gc_complete_lock_);
  Atomic<bool> alloc_tracking_enabled_;
  std::unique_ptr<AllocRecordObjectMap> allocation_records_;
  size_t alloc_record_depth_;
  HeapSampler heap_sampler_;
  Mutex* backtrace_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  Atomic<uint64_t> seen_backtrace_count_;
  Atomic<uint64_t> unique_backtrace_count_;
  std::unordered_set<uint64_t> seen_backtraces_ GUARDED_BY(backtrace_lock_);
  bool gc_disabled_for_shutdown_ GUARDED_BY(gc_complete_lock_);
  bool dump_region_info_before_gc_;
  bool dump_region_info_after_gc_;
  std::vector<space::ImageSpace*> boot_image_spaces_;
  uint32_t boot_images_start_address_;
  uint32_t boot_images_size_;
  Atomic<uint64_t> pre_oome_gc_count_;
  Atomic<AllocationListener*> alloc_listener_;
  Atomic<GcPauseListener*> gc_pause_listener_;
  std::unique_ptr<Verification> verification_;
  friend class CollectorTransitionTask;
  friend class collector::GarbageCollector;
  friend class collector::ConcurrentCopying;
  friend class collector::MarkCompact;
  friend class collector::MarkSweep;
  friend class collector::SemiSpace;
  friend class GCCriticalSection;
  friend class ReferenceQueue;
  friend class ScopedGCCriticalSection;
  friend class ScopedInterruptibleGCCriticalSection;
  friend class VerifyReferenceCardVisitor;
  friend class VerifyReferenceVisitor;
  friend class VerifyObjectVisitor;
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
};
}
}
#endif
