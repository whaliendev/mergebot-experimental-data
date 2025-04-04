#ifndef ART_RUNTIME_GC_HEAP_H_
#define ART_RUNTIME_GC_HEAP_H_ 
#include <iosfwd>
#include <string>
#include <unordered_set>
#include <vector>
#include "allocator_type.h"
#include "arch/instruction_set.h"
#include "atomic.h"
#include "base/time_utils.h"
#include "base/timing_logger.h"
#include "gc/accounting/atomic_stack.h"
#include "gc/accounting/card_table.h"
#include "gc/accounting/read_barrier_table.h"
#include "gc/gc_cause.h"
#include "gc/collector/garbage_collector.h"
#include "gc/collector/gc_type.h"
#include "gc/collector_type.h"
#include "gc/space/large_object_space.h"
#include "globals.h"
#include "jni.h"
#include "object_callbacks.h"
#include "offsets.h"
#include "reference_processor.h"
#include "safe_map.h"
#include "thread_pool.h"
#include "verify_object.h"
namespace art {
class ConditionVariable;
class Mutex;
class StackVisitor;
class Thread;
class TimingLogger;
namespace mirror {
  class Class;
  class Object;
}
namespace gc {
class AllocRecordObjectMap;
class ReferenceProcessor;
class TaskProcessor;
namespace accounting {
  class HeapBitmap;
  class ModUnionTable;
  class RememberedSet;
}
namespace collector {
  class ConcurrentCopying;
  class GarbageCollector;
  class MarkCompact;
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
  class SpaceTest;
  class ZygoteSpace;
}
class AgeCardVisitor {
public:
  uint8_t operator()(uint8_t card) const {
    if (card == accounting::CardTable::kCardDirty) {
      return card - 1;
    } else {
      return 0;
    }
  }
};
enum HomogeneousSpaceCompactResult {
kSuccess,
kErrorReject,
kErrorVMShuttingDown,};
static constexpr bool kUseRosAlloc = true;
static constexpr bool kUseThreadLocalAllocationStack = true;
enum ProcessState {
kProcessStateJankPerceptible = 0,kProcessStateJankImperceptible = 1,};
std::ostream& operator<<(std::ostream& os, const ProcessState& process_state);
class Heap {
public:
  static constexpr bool kMeasureAllocationTime = false;
  static constexpr size_t kDefaultStartingSize = kPageSize;
  static constexpr size_t kDefaultInitialSize = 2 * MB;
  static constexpr size_t kDefaultMaximumSize = 256 * MB;
  static constexpr size_t kDefaultNonMovingSpaceCapacity = 64 * MB;
  static constexpr size_t kDefaultMaxFree = 2 * MB;
  static constexpr size_t kDefaultMinFree = kDefaultMaxFree / 4;
  static constexpr size_t kDefaultLongPauseLogThreshold = MsToNs(5);
  static constexpr size_t kDefaultLongGCLogThreshold = MsToNs(100);
  static constexpr size_t kDefaultTLABSize = 256 * KB;
  static constexpr double kDefaultTargetUtilization = 0.5;
  static constexpr double kDefaultHeapGrowthMultiplier = 2.0;
  static constexpr size_t kDefaultLargeObjectThreshold = 3 * kPageSize;
  static constexpr bool kDefaultEnableParallelGC = false;
  static constexpr space::LargeObjectSpaceType kDefaultLargeObjectSpaceType =
      USE_ART_LOW_4G_ALLOCATOR ?
          space::LargeObjectSpaceType::kFreeList
        : space::LargeObjectSpaceType::kMap;
  static constexpr size_t kTimeAdjust = 1024;
  static constexpr uint64_t kHeapTrimWait = MsToNs(5000);
  static constexpr uint64_t kCollectorTransitionWait = MsToNs(5000);
  explicit Heap(size_t initial_size, size_t growth_limit, size_t min_free,
                size_t max_free, double target_utilization,
                double foreground_heap_growth_multiplier, size_t capacity,
                size_t non_moving_space_capacity,
                const std::string& original_image_file_name,
                InstructionSet image_instruction_set,
                CollectorType foreground_collector_type, CollectorType background_collector_type,
                space::LargeObjectSpaceType large_object_space_type, size_t large_object_threshold,
                size_t parallel_gc_threads, size_t conc_gc_threads, bool low_memory_mode,
                size_t long_pause_threshold, size_t long_gc_threshold,
                bool ignore_max_footprint, bool use_tlab,
                bool verify_pre_gc_heap, bool verify_pre_sweeping_heap, bool verify_post_gc_heap,
                bool verify_pre_gc_rosalloc, bool verify_pre_sweeping_rosalloc,
                bool verify_post_gc_rosalloc, bool gc_stress_mode,
                bool use_homogeneous_space_compaction,
                uint64_t min_interval_homogeneous_space_compaction_by_oom);
  ~Heap();
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      return AllocObjectWithAllocator<kInstrumented, true>(self, klass, num_bytes,
                                                         GetCurrentAllocator(),
                                                         pre_fence_visitor);
      }
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      return AllocObjectWithAllocator<kInstrumented, true>(self, klass, num_bytes,
                                                         GetCurrentNonMovingAllocator(),
                                                         pre_fence_visitor);
      }
  AllocatorType GetCurrentAllocator() const {
    return current_allocator_;
  }
  AllocatorType GetCurrentNonMovingAllocator() const {
    return current_non_moving_allocator_;
  }
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void RegisterNativeAllocation(JNIEnv* env, size_t bytes);
  void RegisterNativeFree(JNIEnv* env, size_t bytes);
      LOCKS_EXCLUDED(Locks::runtime_shutdown_lock_);
      LOCKS_EXCLUDED(Locks::runtime_shutdown_lock_);
      LOCKS_EXCLUDED(Locks::runtime_shutdown_lock_);
  void TransitionCollector(CollectorType collector_type);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
                                           NO_THREAD_SAFETY_ANALYSIS;
                                           NO_THREAD_SAFETY_ANALYSIS;
                    LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
                    LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);
                                                        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                                                        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void IncrementDisableMovingGC(Thread* self);
  void DecrementDisableMovingGC(Thread* self);
                            EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);
                            EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);
  void CollectGarbage(bool clear_soft_references);
                                                   LOCKS_EXCLUDED(Locks::runtime_shutdown_lock_);
                                                   LOCKS_EXCLUDED(Locks::runtime_shutdown_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void ClearGrowthLimit();
                          LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
                          LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
  double GetTargetHeapUtilization() const {
    return target_utilization_;
  }
  void RegisterGCAllocation(size_t bytes);
  void RegisterGCDeAllocation(size_t bytes);
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
                                     LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
                                     LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
                                        LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
                                        LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
  void SetTargetHeapUtilization(float target);
  void SetIdealFootprint(size_t max_allowed_footprint);
      LOCKS_EXCLUDED(gc_complete_lock_);
      LOCKS_EXCLUDED(gc_complete_lock_);
  void UpdateProcessState(ProcessState process_state);
  const std::vector<space::ContinuousSpace*>& GetContinuousSpaces() const {
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
  ALWAYS_INLINE void WriteBarrierField(const mirror::Object* dst, MemberOffset ,
                                       const mirror::Object* ) {
    card_table_->MarkCard(dst);
  }
  ALWAYS_INLINE void WriteBarrierArray(const mirror::Object* dst, int ,
                                       size_t ) {
    card_table_->MarkCard(dst);
  }
  ALWAYS_INLINE void WriteBarrierEveryFieldOf(const mirror::Object* obj) {
    card_table_->MarkCard(obj);
  }
  accounting::CardTable* GetCardTable() const {
    return card_table_.get();
  }
  accounting::ReadBarrierTable* GetReadBarrierTable() const {
    return rb_table_.get();
  }
  void AddFinalizerReference(Thread* self, mirror::Object** object);
  size_t GetBytesAllocated() const {
    return num_bytes_allocated_.LoadSequentiallyConsistent();
  }
                                     LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
                                     LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
  uint64_t GetObjectsAllocatedEver() const;
  uint64_t GetBytesAllocatedEver() const;
  uint64_t GetObjectsFreedEver() const {
    return total_objects_freed_ever_;
  }
  uint64_t GetBytesFreedEver() const {
    return total_bytes_freed_ever_;
  }
  size_t GetMaxMemory() const {
    return std::max(GetBytesAllocated(), growth_limit_);
  }
  size_t GetTotalMemory() const;
  size_t GetFreeMemoryUntilGC() const {
    return max_allowed_footprint_ - GetBytesAllocated();
  }
  size_t GetFreeMemoryUntilOOME() const {
    return growth_limit_ - GetBytesAllocated();
  }
  size_t GetFreeMemory() const {
    size_t byte_allocated = num_bytes_allocated_.LoadSequentiallyConsistent();
    size_t total_memory = GetTotalMemory();
    return total_memory - std::min(total_memory, byte_allocated);
  }
  space::ContinuousSpace* FindContinuousSpaceFromObject(const mirror::Object*, bool fail_ok) const;
  space::DiscontinuousSpace* FindDiscontinuousSpaceFromObject(const mirror::Object*,
                                                              bool fail_ok) const;
  space::Space* FindSpaceFromObject(const mirror::Object*, bool fail_ok) const;
  void DumpForSigQuit(std::ostream& os);
  void DoPendingCollectorTransition();
                          LOCKS_EXCLUDED(gc_complete_lock_);
                          LOCKS_EXCLUDED(gc_complete_lock_);
  void RevokeThreadLocalBuffers(Thread* thread);
  void RevokeRosAllocThreadLocalBuffers(Thread* thread);
  void RevokeAllThreadLocalBuffers();
  void AssertThreadLocalBuffersAreRevoked(Thread* thread);
  void AssertAllBumpPointerSpaceThreadLocalBuffersAreRevoked();
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
accounting::HeapBitmap* GetLiveBitmap() SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
                                          return live_bitmap_.get();
                                          }
accounting::HeapBitmap* GetMarkBitmap() SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
                                          return mark_bitmap_.get();
                                          }
accounting::ObjectStack* GetLiveStack() SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
                                          return live_stack_.get();
                                          }
                       NO_THREAD_SAFETY_ANALYSIS;
                       NO_THREAD_SAFETY_ANALYSIS;
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);
      LOCKS_EXCLUDED(Locks::runtime_shutdown_lock_, Locks::thread_list_lock_);
      LOCKS_EXCLUDED(Locks::runtime_shutdown_lock_, Locks::thread_list_lock_);
      LOCKS_EXCLUDED(Locks::runtime_shutdown_lock_, Locks::thread_list_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);
                       EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);
                       EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);
  space::ImageSpace* GetImageSpace() const;
  void DisableMovingGc();
  space::DlMallocSpace* GetDlMallocSpace() const {
    return dlmalloc_space_;
  }
  space::RosAllocSpace* GetRosAllocSpace() const {
    return rosalloc_space_;
  }
  space::RosAllocSpace* GetRosAllocSpace(gc::allocator::RosAlloc* rosalloc) const;
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
                                 WARN_UNUSED;
                                 WARN_UNUSED;
  void DumpSpaces(std::ostream& stream) const;
                                                             NO_THREAD_SAFETY_ANALYSIS;
                                                             NO_THREAD_SAFETY_ANALYSIS;
                                                           NO_THREAD_SAFETY_ANALYSIS;
                                                           NO_THREAD_SAFETY_ANALYSIS;
                                                    NO_THREAD_SAFETY_ANALYSIS;
                                                    NO_THREAD_SAFETY_ANALYSIS;
  void DumpGcPerformanceInfo(std::ostream& os);
  void ResetGcPerformanceInfo();
  bool CareAboutPauseTimes() const {
    return process_state_ == kProcessStateJankPerceptible;
  }
  void CreateThreadPool();
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
  bool HasImageSpace() const;
  ReferenceProcessor* GetReferenceProcessor() {
    return &reference_processor_;
  }
  TaskProcessor* GetTaskProcessor() {
    return task_processor_.get();
  }
  bool HasZygoteSpace() const {
    return zygote_space_ != nullptr;
  }
  collector::ConcurrentCopying* ConcurrentCopyingCollector() {
    return concurrent_copying_collector_;
  }
  CollectorType CurrentCollectorType() {
    return collector_type_;
  }
  bool IsGcConcurrentAndMoving() const {
    if (IsGcConcurrent() && IsMovingGc(collector_type_)) {
      DCHECK_EQ(collector_type_, foreground_collector_type_);
      DCHECK_EQ(foreground_collector_type_, background_collector_type_)
          << "Assume no transition such that collector_type_ won't change";
      return true;
    }
    return false;
  }
  bool IsMovingGCDisabled(Thread* self) {
    MutexLock mu(self, *gc_complete_lock_);
    return disable_moving_gc_count_ > 0;
  }
                                 LOCKS_EXCLUDED(pending_task_lock_);
                                 LOCKS_EXCLUDED(pending_task_lock_);
                                                          LOCKS_EXCLUDED(pending_task_lock_);
                                                          LOCKS_EXCLUDED(pending_task_lock_);
  bool MayUseCollector(CollectorType type) const;
  void SetMinIntervalHomogeneousSpaceCompactionByOom(uint64_t interval) {
    min_interval_homogeneous_space_compaction_by_oom_ = interval;
  }
  uint64_t GetGcCount() const;
  uint64_t GetGcTime() const;
  uint64_t GetBlockingGcCount() const;
  uint64_t GetBlockingGcTime() const;
  void DumpGcCountRateHistogram(std::ostream& os) const;
  void DumpBlockingGcCountRateHistogram(std::ostream& os) const;
  bool IsAllocTrackingEnabled() const {
    return alloc_tracking_enabled_.LoadRelaxed();
  }
void SetAllocTrackingEnabled(bool enabled) EXCLUSIVE_LOCKS_REQUIRED(Locks::alloc_tracker_lock_){
                                             alloc_tracking_enabled_.StoreRelaxed(enabled);
                                             }
  AllocRecordObjectMap* GetAllocationRecords() const
      EXCLUSIVE_LOCKS_REQUIRED(Locks::alloc_tracker_lock_){
      return allocation_records_.get();
      }
      EXCLUSIVE_LOCKS_REQUIRED(Locks::alloc_tracker_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
private:
  class ConcurrentGCTask;
  class CollectorTransitionTask;
  class HeapTrimTask;
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
  void LogGC(GcCause gc_cause, collector::GarbageCollector* collector);
                                                         LOCKS_EXCLUDED(gc_complete_lock_);
                                                         LOCKS_EXCLUDED(gc_complete_lock_);
  static MemMap* MapAnonymousPreferredAddress(const char* name, uint8_t* request_begin,
                                              size_t capacity, std::string* out_error_str);
  bool SupportHSpaceCompaction() const {
    return main_space_backup_ != nullptr;
  }
  static ALWAYS_INLINE bool AllocatorHasAllocationStack(AllocatorType allocator_type) {
    return
        allocator_type != kAllocatorTypeBumpPointer &&
        allocator_type != kAllocatorTypeTLAB &&
        allocator_type != kAllocatorTypeRegion &&
        allocator_type != kAllocatorTypeRegionTLAB;
  }
  static ALWAYS_INLINE bool AllocatorMayHaveConcurrentGC(AllocatorType allocator_type) {
    return
        allocator_type != kAllocatorTypeBumpPointer &&
        allocator_type != kAllocatorTypeTLAB;
  }
  static bool IsMovingGc(CollectorType collector_type) {
    return collector_type == kCollectorTypeSS || collector_type == kCollectorTypeGSS ||
        collector_type == kCollectorTypeCC || collector_type == kCollectorTypeMC ||
        collector_type == kCollectorTypeHomogeneousSpaceCompact;
  }
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  accounting::ObjectStack* GetMarkStack() {
    return mark_stack_.get();
  }
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                        EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
                        EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void RunFinalization(JNIEnv* env, uint64_t timeout);
      EXCLUSIVE_LOCKS_REQUIRED(gc_complete_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(gc_complete_lock_);
      LOCKS_EXCLUDED(pending_task_lock_);
      LOCKS_EXCLUDED(pending_task_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool IsGCRequestPending() const;
      LOCKS_EXCLUDED(gc_complete_lock_,
                     Locks::heap_bitmap_lock_,
                     Locks::thread_suspend_count_lock_);
      LOCKS_EXCLUDED(gc_complete_lock_,
                     Locks::heap_bitmap_lock_,
                     Locks::thread_suspend_count_lock_);
      LOCKS_EXCLUDED(Locks::mutator_lock_);
      LOCKS_EXCLUDED(Locks::mutator_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
      LOCKS_EXCLUDED(Locks::mutator_lock_);
      LOCKS_EXCLUDED(Locks::mutator_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
  void UpdateMaxNativeFootprint();
  collector::GarbageCollector* FindCollectorByGcType(collector::GcType gc_type);
  HomogeneousSpaceCompactResult PerformHomogeneousSpaceCompact();
  void CreateMainMallocSpace(MemMap* mem_map, size_t initial_size, size_t growth_limit,
                             size_t capacity);
  space::MallocSpace* CreateMallocSpaceFromMemMap(MemMap* mem_map, size_t initial_size,
                                                  size_t growth_limit, size_t capacity,
                                                  const char* name, bool can_move_objects);
  void GrowForUtilization(collector::GarbageCollector* collector_ran,
                          uint64_t bytes_allocated_before_gc = 0);
  size_t GetPercentFree();
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);
                                SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                                SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void ProcessCards(TimingLogger* timings, bool use_rem_sets, bool process_alloc_space_cards,
                    bool clear_alloc_space_cards);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void ClearConcurrentGCRequest();
                                      LOCKS_EXCLUDED(pending_task_lock_);
                                      LOCKS_EXCLUDED(pending_task_lock_);
                                                     LOCKS_EXCLUDED(pending_task_lock_);
                                                     LOCKS_EXCLUDED(pending_task_lock_);
  bool IsGcConcurrent() const ALWAYS_INLINE {
    return collector_type_ == kCollectorTypeCMS || collector_type_ == kCollectorTypeCC;
  }
                                LOCKS_EXCLUDED(gc_complete_lock_);
                                LOCKS_EXCLUDED(gc_complete_lock_);
  void TrimIndirectReferenceTables(Thread* self);
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
                                     EXCLUSIVE_LOCKS_REQUIRED(gc_complete_lock_);
                                     EXCLUSIVE_LOCKS_REQUIRED(gc_complete_lock_);
  void CheckGcStressMode(Thread* self, mirror::Object** obj)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  std::vector<space::ContinuousSpace*> continuous_spaces_;
  std::vector<space::DiscontinuousSpace*> discontinuous_spaces_;
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
  CollectorType foreground_collector_type_;
  CollectorType background_collector_type_;
  CollectorType desired_collector_type_;
Mutex* pending_task_lock_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  Atomic<bool> alloc_tracking_enabled_;
  std::unique_ptr<AllocRecordObjectMap> allocation_records_
  const size_t parallel_gc_threads_;
  const size_t conc_gc_threads_;
  const bool low_memory_mode_;
  const size_t long_pause_log_threshold_;
  const size_t long_gc_log_threshold_;
  const bool ignore_max_footprint_;
  Mutex zygote_creation_lock_;
  space::ZygoteSpace* zygote_space_;
  size_t large_object_threshold_;
Mutex* gc_complete_lock_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
std::unique_ptr<ConditionVariable> gc_complete_cond_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  ReferenceProcessor reference_processor_;
  std::unique_ptr<TaskProcessor> task_processor_;
volatile CollectorType collector_type_running_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
volatile collector::GcType last_gc_type_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  collector::GcType next_gc_type_;
  size_t capacity_;
  size_t growth_limit_;
  size_t max_allowed_footprint_;
  size_t native_footprint_gc_watermark_;
  bool native_need_to_run_finalization_;
  ProcessState process_state_;
  size_t concurrent_start_bytes_;
  uint64_t total_bytes_freed_ever_;
  uint64_t total_objects_freed_ever_;
  Atomic<size_t> num_bytes_allocated_;
  Atomic<size_t> native_bytes_allocated_;
  Atomic<size_t> num_bytes_freed_revoke_;
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
  uint64_t allocation_rate_;
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
  size_t min_free_;
  size_t max_free_;
  double target_utilization_;
  double foreground_heap_growth_multiplier_;
  uint64_t total_wait_time_;
  AtomicInteger total_allocation_time_;
  VerifyObjectMode verify_object_mode_;
size_t disable_moving_gc_count_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  std::vector<collector::GarbageCollector*> garbage_collectors_;
  collector::SemiSpace* semi_space_collector_;
  collector::MarkCompact* mark_compact_collector_;
  collector::ConcurrentCopying* concurrent_copying_collector_;
  const bool running_on_valgrind_;
  const bool use_tlab_;
  std::unique_ptr<space::MallocSpace> main_space_backup_;
  uint64_t min_interval_homogeneous_space_compaction_by_oom_;
  uint64_t last_time_homogeneous_space_compaction_by_oom_;
  Atomic<size_t> count_delayed_oom_;
  Atomic<size_t> count_requested_homogeneous_space_compaction_;
  Atomic<size_t> count_ignored_homogeneous_space_compaction_;
  Atomic<size_t> count_performed_homogeneous_space_compaction_;
  Atomic<bool> concurrent_gc_pending_;
CollectorTransitionTask* pending_collector_transition_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
HeapTrimTask* pending_heap_trim_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  bool use_homogeneous_space_compaction_for_oom_;
bool running_collection_is_blocking_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  uint64_t blocking_gc_count_;
  uint64_t blocking_gc_time_;
static constexpr uint64_t kGcCountRateHistogramWindowDuration = MsToNs(10 * 1000);
  uint64_t last_update_time_gc_count_rate_histograms_;
  uint64_t gc_count_last_window_;
  uint64_t blocking_gc_count_last_window_;
  static constexpr size_t kGcCountRateMaxBucketCount = 200;
Histogram<uint64_t> gc_count_rate_histogram_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
Histogram<uint64_t> blocking_gc_count_rate_histogram_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
Mutex* backtrace_lock_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  Atomic<uint64_t> seen_backtrace_count_;
  Atomic<uint64_t> unique_backtrace_count_;
std::unordered_set<uint64_t> seen_backtraces_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  friend class CollectorTransitionTask;
  friend class collector::GarbageCollector;
  friend class collector::MarkCompact;
  friend class collector::ConcurrentCopying;
  friend class collector::MarkSweep;
  friend class collector::SemiSpace;
  friend class ReferenceQueue;
  friend class VerifyReferenceCardVisitor;
  friend class VerifyReferenceVisitor;
  friend class VerifyObjectVisitor;
  friend class ScopedHeapFill;
  friend class space::SpaceTest;
  class AllocationTimer {
   public:
    ALWAYS_INLINE AllocationTimer(Heap* heap, mirror::Object** allocated_obj_ptr);
    ALWAYS_INLINE ~AllocationTimer();
   private:
    Heap* const heap_;
    mirror::Object** allocated_obj_ptr_;
    const uint64_t allocation_start_time_;
    DISALLOW_IMPLICIT_CONSTRUCTORS(AllocationTimer);
  };
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
};
}
}
#endif
