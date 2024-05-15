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
template <typename T>
class AtomicStack;
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
      USE_ART_LOW_4G_ALLOCATOR ? space::LargeObjectSpaceType::kFreeList :
                                 space::LargeObjectSpaceType::kMap;
  static constexpr size_t kTimeAdjust = 1024;
#ifdef __ANDROID__
  static constexpr uint32_t kNotifyNativeInterval = 64;
#else
  static constexpr uint32_t kNotifyNativeInterval = 384;
#endif
  static constexpr size_t kCheckImmediatelyThreshold = 300000;
  static constexpr uint64_t kHeapTrimWait = MsToNs(5000);
  static constexpr uint64_t kCollectorTransitionWait = MsToNs(5000);
 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  size_t min_foreground_concurrent_start_bytes_public :
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
 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
    return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
  }
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
    return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
  }
 public:
  AllocatorType GetCurrentAllocator() const { return current_allocator_; }
  AllocatorType GetCurrentNonMovingAllocator() const { return current_non_moving_allocator_; }
  AllocatorType GetUpdatedAllocator(AllocatorType old_allocator) {
    return (old_allocator == kAllocatorTypeNonMoving) ? GetCurrentNonMovingAllocator() :
                                                        GetCurrentAllocator();
  }
  void VisitReflectiveTargets(ReflectiveValueVisitor* visitor) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void CheckPreconditionsForAllocObject(ObjPtr<mirror::Class> c, size_t byte_count) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void RegisterNativeAllocation(JNIEnv* env, size_t bytes) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void RegisterNativeFree(JNIEnv* env, size_t bytes);
  void NotifyNativeAllocations(JNIEnv* env) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  uint32_t GetNotifyNativeInterval() { return kNotifyNativeInterval; }
  void ChangeAllocator(AllocatorType allocator) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void ChangeCollector(CollectorType collector_type) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void VerifyObjectBody(ObjPtr<mirror::Object> o) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void VerifyHeap() private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  size_t VerifyHeapReferences(bool verify_referents = true) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  bool VerifyMissingCardMarks() private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  bool IsValidObjectAddress(const void* obj) constprivate : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  bool IsNonDiscontinuousSpaceHeapAddress(const void* addr) const private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  bool IsLiveObjectLocked(ObjPtr<mirror::Object> obj,
                          bool search_allocation_stack = true,
                          bool search_live_stack = true,
                          bool sorted = false) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  bool IsMovableObject(ObjPtr<mirror::Object> obj) constprivate
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void IncrementDisableMovingGC(Thread* self) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void DecrementDisableMovingGC(Thread* self) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void IncrementDisableThreadFlip(Thread* self) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void DecrementDisableThreadFlip(Thread* self) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void ThreadFlipBegin(Thread* self) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void ThreadFlipEnd(Thread* self) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void EnsureObjectUserfaulted(ObjPtr<mirror::Object> obj) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void ClearMarkedObjects() private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void CollectGarbage(bool clear_soft_references, GcCause cause = kGcCauseExplicit) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void ConcurrentGC(Thread* self, GcCause cause, bool force_full, uint32_t requested_gc_num) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void CountInstances(const std::vector<Handle<mirror::Class>>& classes,
                      bool use_is_assignable_from,
                      uint64_t* counts) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void ClearGrowthLimit() private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void ClampGrowthLimit() private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  double GetTargetHeapUtilization() const { return target_utilization_; }
  void RegisterGCAllocation(size_t bytes);
  void RegisterGCDeAllocation(size_t bytes);
  void SetSpaceAsDefault(space::ContinuousSpace* continuous_space) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void AddSpace(space::Space* space) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void RemoveSpace(space::Space* space) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  double GetPreGcWeightedAllocatedBytes() const { return pre_gc_weighted_allocated_bytes_; }
  double GetPostGcWeightedAllocatedBytes() const { return post_gc_weighted_allocated_bytes_; }
  void CalculatePreGcWeightedAllocatedBytes();
  void CalculatePostGcWeightedAllocatedBytes();
  uint64_t GetTotalGcCpuTime();
  uint64_t GetProcessCpuStartTime() const { return process_cpu_start_time_ns_; }
  uint64_t GetPostGCLastProcessCpuTime() const { return post_gc_last_process_cpu_time_ns_; }
  void SetTargetHeapUtilization(float target);
  void SetIdealFootprint(size_t max_allowed_footprint);
  collector::GcType WaitForGcToComplete(GcCause cause, Thread* self) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void UpdateProcessState(ProcessState old_process_state, ProcessState new_process_state) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  bool HaveContinuousSpaces() const NO_THREAD_SAFETY_ANALYSIS {
    return !continuous_spaces_.empty();
  }
 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  REQUIRES(Locks::alloc_tracker_lock_) { return allocation_records_.get(); }
 private:
  ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
    return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
  }
  ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
    return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
  }
  ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
    return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
  }
  ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
    return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
  }
  ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
    return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
  }
  ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
    return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
  }
  ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
    return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
  }
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void RecordFree(uint64_t freed_objects, int64_t freed_bytes);
  void RecordFreeRevoke();
  accounting::CardTable* GetCardTable() const { return card_table_.get(); }
  accounting::ReadBarrierTable* GetReadBarrierTable() const { return rb_table_.get(); }
  void AddFinalizerReference(Thread* self, ObjPtr<mirror::Object>* object);
  size_t GetBytesAllocated() const { return num_bytes_allocated_.load(std::memory_order_relaxed); }
  bool GetUseGenerationalCC() const { return use_generational_cc_; }
  size_t GetObjectsAllocated() const private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  uint64_t GetObjectsAllocatedEver() const;
  uint64_t GetBytesAllocatedEver() const;
  uint64_t GetObjectsFreedEver(std::memory_order mo = std::memory_order_relaxed) const {
    return total_objects_freed_ever_.load(mo);
  }
  uint64_t GetBytesFreedEver(std::memory_order mo = std::memory_order_relaxed) const {
    return total_bytes_freed_ever_.load(mo);
  }
  space::RegionSpace* GetRegionSpace() const { return region_space_; }
  space::BumpPointerSpace* GetBumpPointerSpace() const { return bump_pointer_space_; }
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
      private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  space::ContinuousSpace* FindContinuousSpaceFromAddress(const mirror::Object* addr) const private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  space::DiscontinuousSpace* FindDiscontinuousSpaceFromObject(ObjPtr<mirror::Object>,
                                                              bool fail_ok) const private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  space::Space* FindSpaceFromObject(ObjPtr<mirror::Object> obj, bool fail_ok) const private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  space::Space* FindSpaceFromAddress(const void* ptr) const private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  std::string DumpSpaceNameFromAddress(const void* addr) const private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void DumpForSigQuit(std::ostream& os) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void DoPendingCollectorTransition() private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void Trim(Thread* self) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void RevokeThreadLocalBuffers(Thread* thread);
  void RevokeRosAllocThreadLocalBuffers(Thread* thread);
  void RevokeAllThreadLocalBuffers();
  void AssertThreadLocalBuffersAreRevoked(Thread* thread);
  void AssertAllBumpPointerSpaceThreadLocalBuffersAreRevoked();
  void RosAllocVerification(TimingLogger* timings, const char* name) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  accounting::HeapBitmap* GetLiveBitmap() REQUIRES(Locks::alloc_tracker_lock_) {
    return allocation_records_.get();
  }
 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  REQUIRES(Locks::alloc_tracker_lock_) { return allocation_records_.get(); }
 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  REQUIRES(Locks::alloc_tracker_lock_) { return allocation_records_.get(); }
 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  REQUIRES(Locks::alloc_tracker_lock_) { return allocation_records_.get(); }
 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void FlushAllocStack() private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void RevokeAllThreadLocalAllocationStacks(Thread* self) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void MarkAllocStack(accounting::SpaceBitmap<kObjectAlignment>* bitmap1,
                      accounting::SpaceBitmap<kObjectAlignment>* bitmap2,
                      accounting::SpaceBitmap<kLargeObjectAlignment>* large_objects,
                      accounting::ObjectStack* stack) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void MarkAllocStackAsLive(accounting::ObjectStack* stack) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void UnBindBitmaps() private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  const std::vector<space::ImageSpace*>& GetBootImageSpaces() const { return boot_image_spaces_; }
  bool ObjectIsInBootImageSpace(ObjPtr<mirror::Object> obj) const private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  bool IsInBootImageOatFile(const void* p) const private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  uint32_t GetBootImagesStartAddress() const { return boot_images_start_address_; }
  uint32_t GetBootImagesSize() const { return boot_images_size_; }
  bool IsBootImageAddress(const void* p) const {
    return reinterpret_cast<uintptr_t>(p) - boot_images_start_address_ < boot_images_size_;
  }
  space::DlMallocSpace* GetDlMallocSpace() const { return dlmalloc_space_; }
  space::RosAllocSpace* GetRosAllocSpace() const { return rosalloc_space_; }
  space::RosAllocSpace* GetRosAllocSpace(gc::allocator::RosAlloc* rosalloc) const private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  space::MallocSpace* GetNonMovingSpace() const { return non_moving_space_; }
  space::LargeObjectSpace* GetLargeObjectsSpace() const { return large_object_space_; }
  space::MallocSpace* GetPrimaryFreeListSpace() {
    if (kUseRosAlloc) {
      DCHECK(rosalloc_space_ != nullptr);
      return reinterpret_cast<space::MallocSpace*>(rosalloc_space_);
    } else {
      DCHECK(dlmalloc_space_ != nullptr);
      return reinterpret_cast<space::MallocSpace*>(dlmalloc_space_);
    }
  }
  void DumpSpaces(std::ostream& stream) constprivate : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  std::string DumpSpaces() constprivate : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void DumpGcPerformanceInfo(std::ostream& os) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void ResetGcPerformanceInfo() private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
 public:
  void CreateThreadPool(size_t num_threads = 0);
  void WaitForWorkersToBeCreated();
  void DeleteThreadPool();
  ThreadPool* GetThreadPool() { return thread_pool_.get(); }
  size_t GetParallelGCThreadCount() const { return parallel_gc_threads_; }
  size_t GetConcGCThreadCount() const { return conc_gc_threads_; }
  accounting::ModUnionTable* FindModUnionTableFromSpace(space::Space* space);
  void AddModUnionTable(accounting::ModUnionTable* mod_union_table);
  accounting::RememberedSet* FindRememberedSetFromSpace(space::Space* space);
  void AddRememberedSet(accounting::RememberedSet* remembered_set);
  void RemoveRememberedSet(space::Space* space);
  bool IsCompilingBoot() const;
  bool HasBootImageSpace() const { return !boot_image_spaces_.empty(); }
  ReferenceProcessor* GetReferenceProcessor() { return reference_processor_.get(); }
  TaskProcessor* GetTaskProcessor() { return task_processor_.get(); }
  bool HasZygoteSpace() const { return zygote_space_ != nullptr; }
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
  bool IsMovingGc() const {
    return IsMovingGc(CurrentCollectorType()) { return IsMovingGc(CurrentCollectorType()); }
    CollectorType GetForegroundCollectorType() const { return foreground_collector_type_; }
    bool IsGcConcurrentAndMoving() const {
      if (IsGcConcurrent() && IsMovingGc(collector_type_)) {
        DCHECK_EQ(collector_type_, foreground_collector_type_);
        return true;
      }
      return false;
    }
    bool IsMovingGCDisabled(Thread * self) private :
        ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
      return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
    }
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
   public:
    uint32_t GetCurrentGcNum() { return gcs_completed_.load(std::memory_order_acquire); }
    bool RequestConcurrentGC(
        Thread * self, GcCause cause, bool force_full, uint32_t observed_gc_num) private
        : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
   public:
    bool MayUseCollector(CollectorType type) const;
    void SetMinIntervalHomogeneousSpaceCompactionByOom(uint64_t interval) {
      min_interval_homogeneous_space_compaction_by_oom_ = interval;
    }
    uint64_t GetGcCount() const;
    uint64_t GetGcTime() const;
    uint64_t GetBlockingGcCount() const;
    uint64_t GetBlockingGcTime() const;
    void DumpGcCountRateHistogram(std::ostream & os) constprivate
        : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
   public:
    void DumpBlockingGcCountRateHistogram(std::ostream & os) constprivate
        : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
   public:
    uint64_t GetTotalTimeWaitingForGC() const { return total_wait_time_; }
    uint64_t GetPreOomeGcCount() const;
    HeapSampler& GetHeapSampler() { return heap_sampler_; }
    void InitPerfettoJavaHeapProf();
    int CheckPerfettoJHPEnabled();
    void JHPCheckNonTlabSampleAllocation(Thread * self, mirror::Object * ret, size_t alloc_size);
    size_t JHPCalculateNextTlabSize(Thread * self,
                                    size_t jhp_def_tlab_size,
                                    size_t alloc_size,
                                    bool* take_sample,
                                    size_t* bytes_until_sample);
    void AdjustSampleOffset(size_t adjustment);
    bool IsAllocTrackingEnabled() const {
      return alloc_tracking_enabled_.load(std::memory_order_relaxed);
    }
    void SetAllocTrackingEnabled(bool enabled) REQUIRES(Locks::alloc_tracker_lock_) {
      return allocation_records_.get();
    }
   private:
    ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
      return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
    }
    ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
      return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
    }
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
   public:
    REQUIRES(Locks::alloc_tracker_lock_) { return allocation_records_.get(); }
   private:
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
   public:
    void VisitAllocationRecords(RootVisitor * visitor) const private
        : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
   public:
    void SweepAllocationRecords(IsMarkedVisitor * visitor) const private
        : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
   public:
    void DisallowNewAllocationRecords() const private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
   public:
    void AllowNewAllocationRecords() const private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
   public:
    void BroadcastForNewAllocationRecords() const private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
   public:
    void DisableGCForShutdown() private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
   public:
    bool IsGCDisabledForShutdown() constprivate : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
   public:
    HomogeneousSpaceCompactResult PerformHomogeneousSpaceCompact() private
        : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
   public:
    bool SupportHomogeneousSpaceCompactAndCollectorTransitions() const;
    void SetAllocationListener(AllocationListener * l);
    void RemoveAllocationListener();
    void SetGcPauseListener(GcPauseListener * l);
    GcPauseListener* GetGcPauseListener() {
      return gc_pause_listener_.load(std::memory_order_acquire);
    }
    void RemoveGcPauseListener();
    const Verification* GetVerification() const;
    void PostForkChildAction(Thread * self) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
   public:
    void TraceHeapSize(size_t heap_size);
    bool AddHeapTask(gc::HeapTask * task);
   private:
    class ConcurrentGCTask;
    class CollectorTransitionTask;
    class HeapTrimTask;
    class TriggerPostForkCCGcTask;
    class ReduceTargetFootprintTask;
    collector::GarbageCollector* Compact(space::ContinuousMemMapAllocSpace * target_space,
                                         space::ContinuousMemMapAllocSpace * source_space,
                                         GcCause gc_cause) DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void LogGC(GcCause gc_cause, collector::GarbageCollector * collector);
    void StartGC(Thread * self, GcCause cause, CollectorType collector_type)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void FinishGC(Thread * self, collector::GcType gc_type) DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    double CalculateGcWeightedAllocatedBytes(uint64_t gc_last_process_cpu_time_ns,
                                             uint64_t current_process_cpu_time) const;
    static MemMap MapAnonymousPreferredAddress(
        const char* name, uint8_t* request_begin, size_t capacity, std::string* out_error_str);
    bool SupportHSpaceCompaction() const {
      return main_space_backup_ != nullptr;
    }
    ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
      return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
    }
    ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
      return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
    }
    ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
      return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
    }
    ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
      return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
    }
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    ALWAYS_INLINE bool ShouldConcurrentGCForJava(size_t new_num_bytes_allocated);
    float NativeMemoryOverTarget(size_t current_native_bytes, bool is_gc_concurrent);
    void CheckGCForNative(Thread * self) DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    accounting::ObjectStack* GetMarkStack() { return mark_stack_.get(); }
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    mirror::Object* AllocateInternalWithGc(Thread * self,
                                           AllocatorType allocator,
                                           bool instrumented,
                                           size_t num_bytes,
                                           size_t* bytes_allocated,
                                           size_t* usable_size,
                                           size_t* bytes_tl_bulk_allocated,
                                           ObjPtr<mirror::Class>* klass)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    mirror::Object* AllocateInto(
        Thread * self, space::AllocSpace * space, ObjPtr<mirror::Class> c, size_t bytes)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void SwapSemiSpaces() DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    mirror::Object* AllocWithNewTLAB(Thread * self,
                                     AllocatorType allocator_type,
                                     size_t alloc_size,
                                     bool grow,
                                     size_t* bytes_allocated,
                                     size_t* usable_size,
                                     size_t* bytes_tl_bulk_allocated)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void ThrowOutOfMemoryError(Thread * self, size_t byte_count, AllocatorType allocator_type)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    ALWAYS_INLINE bool IsOutOfMemoryOnAllocation(
        AllocatorType allocator_type, size_t alloc_size, bool grow);
    collector::GcType WaitForGcToCompleteLocked(GcCause cause, Thread * self)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void RequestCollectorTransition(CollectorType desired_collector_type, uint64_t delta_time)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void RequestConcurrentGCAndSaveObject(
        Thread * self, bool force_full, uint32_t observed_gc_num, ObjPtr<mirror::Object>* obj)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    static constexpr uint32_t GC_NUM_ANY = std::numeric_limits<uint32_t>::max();
    collector::GcType CollectGarbageInternal(collector::GcType gc_plan,
                                             GcCause gc_cause,
                                             bool clear_soft_references,
                                             uint32_t requested_gc_num)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void PreGcVerification(collector::GarbageCollector * gc) DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void PreGcVerificationPaused(collector::GarbageCollector * gc)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void PrePauseRosAllocVerification(collector::GarbageCollector * gc)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void PreSweepingGcVerification(collector::GarbageCollector * gc)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void PostGcVerification(collector::GarbageCollector * gc) DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void PostGcVerificationPaused(collector::GarbageCollector * gc)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    collector::GarbageCollector* FindCollectorByGcType(collector::GcType gc_type);
    void CreateMainMallocSpace(
        MemMap && mem_map, size_t initial_size, size_t growth_limit, size_t capacity);
    space::MallocSpace* CreateMallocSpaceFromMemMap(MemMap && mem_map,
                                                    size_t initial_size,
                                                    size_t growth_limit,
                                                    size_t capacity,
                                                    const char* name,
                                                    bool can_move_objects);
    void GrowForUtilization(collector::GarbageCollector * collector_ran,
                            size_t bytes_allocated_before_gc = 0)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    size_t GetPercentFree();
    void SwapStacks() DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void ProcessCards(TimingLogger * timings,
                      bool use_rem_sets,
                      bool process_alloc_space_cards,
                      bool clear_alloc_space_cards) DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void PushOnAllocationStack(Thread * self, ObjPtr<mirror::Object> * obj)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void PushOnAllocationStackWithInternalGC(Thread * self, ObjPtr<mirror::Object> * obj)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void PushOnThreadLocalAllocationStackWithInternalGC(
        Thread * thread, ObjPtr<mirror::Object> * obj) DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void ClearPendingTrim(Thread * self) DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void ClearPendingCollectorTransition(Thread * self) DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    bool IsGcConcurrent() const ALWAYS_INLINE {
      return collector_type_ == kCollectorTypeCC || collector_type_ == kCollectorTypeCMC ||
             collector_type_ == kCollectorTypeCMS || collector_type_ == kCollectorTypeCCBackground;
    }
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void TrimIndirectReferenceTables(Thread * self);
    void UpdateGcCountRateHistograms() DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void CheckGcStressMode(Thread * self, ObjPtr<mirror::Object> * obj)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    collector::GcType NonStickyGcType() const {
      return HasZygoteSpace() ? collector::kGcTypePartial : collector::kGcTypeFull;
    }
    ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
      return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
    }
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void GrowHeapOnJankPerceptibleSwitch() DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void IncrementFreedEver();
    static void VlogHeapGrowth(
        size_t max_allowed_footprint, size_t new_footprint, size_t alloc_size);
    size_t GetNativeBytes();
    void SetDefaultConcurrentStartBytes() DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void SetDefaultConcurrentStartBytesLocked();
    std::vector<space::ContinuousSpace*> continuous_spaces_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    std::vector<space::DiscontinuousSpace*> discontinuous_spaces_ DISALLOW_IMPLICIT_CONSTRUCTORS(
        Heap);
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
    Mutex* pending_task_lock_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
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
    Mutex* gc_complete_lock_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    std::unique_ptr<ConditionVariable> gc_complete_cond_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    Mutex* thread_flip_lock_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    std::unique_ptr<ConditionVariable> thread_flip_cond_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    size_t disable_thread_flip_count_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    bool thread_flip_running_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    std::unique_ptr<ReferenceProcessor> reference_processor_;
    std::unique_ptr<TaskProcessor> task_processor_;
    volatile CollectorType collector_type_running_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    volatile GcCause last_gc_cause_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    volatile Thread* thread_running_gc_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    volatile collector::GcType last_gc_type_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    collector::GcType next_gc_type_;
    size_t capacity_;
    size_t growth_limit_;
    size_t initial_heap_size_;
    Atomic<size_t> target_footprint_;
    Mutex process_state_update_lock_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    size_t min_foreground_target_footprint_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
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
    std::unique_ptr<accounting::HeapBitmap> live_bitmap_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    std::unique_ptr<accounting::HeapBitmap> mark_bitmap_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
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
    size_t disable_moving_gc_count_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
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
    CollectorTransitionTask* pending_collector_transition_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    HeapTrimTask* pending_heap_trim_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    bool use_homogeneous_space_compaction_for_oom_;
    const bool use_generational_cc_;
    bool running_collection_is_blocking_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    uint64_t blocking_gc_count_;
    uint64_t blocking_gc_time_;
    static constexpr uint64_t kGcCountRateHistogramWindowDuration =
        MsToNs(10 * 1000);
    static constexpr uint64_t kGcCountRateHistogramMaxNumMissedWindows = 100;
    uint64_t last_update_time_gc_count_rate_histograms_;
    uint64_t gc_count_last_window_;
    uint64_t blocking_gc_count_last_window_;
    static constexpr size_t kGcCountRateMaxBucketCount = 200;
    Histogram<uint64_t> gc_count_rate_histogram_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    Histogram<uint64_t> blocking_gc_count_rate_histogram_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    Atomic<bool> alloc_tracking_enabled_;
    std::unique_ptr<AllocRecordObjectMap> allocation_records_;
    size_t alloc_record_depth_;
    HeapSampler heap_sampler_;
    Mutex* backtrace_lock_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    Atomic<uint64_t> seen_backtrace_count_;
    Atomic<uint64_t> unique_backtrace_count_;
    std::unordered_set<uint64_t> seen_backtraces_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    bool gc_disabled_for_shutdown_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
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
