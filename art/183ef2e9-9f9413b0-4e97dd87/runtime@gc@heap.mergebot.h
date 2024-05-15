/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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
}  // namespace mirror

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
}  // namespace accounting

namespace collector {
class ConcurrentCopying;
class GarbageCollector;
class MarkSweep;
class SemiSpace;
}  // namespace collector

namespace allocator {
class RosAlloc;
}  // namespace allocator

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
}  // namespace space

enum HomogeneousSpaceCompactResult {
  // Success.
  kSuccess,           // Reject due to disabled moving GC.
  kErrorReject,       // Unsupported due to the current configuration.
  kErrorUnsupported,  // System is shutting down.
  kErrorVMShuttingDown,
};

// If true, use rosalloc/RosAllocSpace instead of dlmalloc/DlMallocSpace
static constexpr bool kUseRosAlloc = true;

// If true, use thread-local allocation stack.
static constexpr bool kUseThreadLocalAllocationStack = true;

class Heap {
 public:
  // How much we grow the TLAB if we can do it.
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
  // Primitive arrays larger than this size are put in the large object space.
  static constexpr size_t kMinLargeObjectThreshold = 3 * kPageSize;
  static constexpr size_t kDefaultLargeObjectThreshold = kMinLargeObjectThreshold;
  // Whether or not parallel GC is enabled. If not, then we never create the thread pool.
  static constexpr bool kDefaultEnableParallelGC = true;
  static uint8_t* const kPreferredAllocSpaceBegin;

  // Whether or not we use the free list large object space. Only use it if USE_ART_LOW_4G_ALLOCATOR
  // since this means that we have to use the slow msync loop in MemMap::MapAnonymous.
  static constexpr space::LargeObjectSpaceType kDefaultLargeObjectSpaceType =
      USE_ART_LOW_4G_ALLOCATOR ? space::LargeObjectSpaceType::kFreeList :
                                 space::LargeObjectSpaceType::kMap;

  // Used so that we don't overflow the allocation time atomic integer.
  static constexpr size_t kTimeAdjust = 1024;

// Client should call NotifyNativeAllocation every kNotifyNativeInterval allocations.
// Should be chosen so that time_to_call_mallinfo / kNotifyNativeInterval is on the same order
// as object allocation time. time_to_call_mallinfo seems to be on the order of 1 usec
// on Android.
#ifdef __ANDROID__
  static constexpr uint32_t kNotifyNativeInterval = 64;
#else
  // Some host mallinfo() implementations are slow. And memory is less scarce.
  static constexpr uint32_t kNotifyNativeInterval = 384;
#endif

  // RegisterNativeAllocation checks immediately whether GC is needed if size exceeds the
  // following. kCheckImmediatelyThreshold * kNotifyNativeInterval should be small enough to
  // make it safe to allocate that many bytes between checks.
  static constexpr size_t kCheckImmediatelyThreshold = 300000;

  // How often we allow heap trimming to happen (nanoseconds).
  static constexpr uint64_t kHeapTrimWait = MsToNs(5000);
  // How long we wait after a transition request to perform a collector transition (nanoseconds).
  static constexpr uint64_t kCollectorTransitionWait = MsToNs(5000);

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  size_t min_foreground_concurrent_start_bytes_public :
      // Create a heap with the requested sizes. The possible empty
      // image_file_names names specify Spaces to load based on
      // ImageWriter output.
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
  // Return the amount of space we allow for native memory when deciding whether to
  // collect. We collect when a weighted sum of Java memory plus native memory exceeds
  // the similarly weighted sum of the Java heap size target and this value.
  ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
    // We keep the traditional limit of max_free_ in place for small heaps,
    // but allow it to be adjusted upward for large heaps to limit GC overhead.
    return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
  }

  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  // Return the amount of space we allow for native memory when deciding whether to
  // collect. We collect when a weighted sum of Java memory plus native memory exceeds
  // the similarly weighted sum of the Java heap size target and this value.
  ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
    // We keep the traditional limit of max_free_ in place for small heaps,
    // but allow it to be adjusted upward for large heaps to limit GC overhead.
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
  // Inform the garbage collector of a non-malloc allocated native memory that might become
  // reclaimable in the future as a result of Java garbage collection.
  void RegisterNativeAllocation(JNIEnv* env, size_t bytes) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  void RegisterNativeFree(JNIEnv* env, size_t bytes);

  // Notify the garbage collector of malloc allocations that might be reclaimable
  // as a result of Java garbage collection. Each such call represents approximately
  // kNotifyNativeInterval such allocations.
  void NotifyNativeAllocations(JNIEnv* env) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  uint32_t GetNotifyNativeInterval() { return kNotifyNativeInterval; }

  // Change the allocator, updates entrypoints.
  void ChangeAllocator(AllocatorType allocator) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Change the collector to be one of the possible options (MS, CMS, SS). Only safe when no
  // concurrent accesses to the heap are possible.
  void ChangeCollector(CollectorType collector_type) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // The given reference is believed to be to an object in the Java heap, check the soundness of it.
  // TODO: NO_THREAD_SAFETY_ANALYSIS since we call this everywhere and it is impossible to find a
  // proper lock ordering for it.
  void VerifyObjectBody(ObjPtr<mirror::Object> o) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Consistency check of all live references.
  void VerifyHeap() private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Returns how many failures occured.
  size_t VerifyHeapReferences(bool verify_referents = true) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  bool VerifyMissingCardMarks() private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // A weaker test than IsLiveObject or VerifyObject that doesn't require the heap lock,
  // and doesn't abort on error, allowing the caller to report more
  // meaningful diagnostics.
  bool IsValidObjectAddress(const void* obj) constprivate : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Faster alternative to IsHeapAddress since finding if an object is in the large object space is
  // very slow.
  bool IsNonDiscontinuousSpaceHeapAddress(const void* addr) const private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Returns true if 'obj' is a live heap object, false otherwise (including for invalid addresses).
  // Requires the heap lock to be held.
  bool IsLiveObjectLocked(ObjPtr<mirror::Object> obj,
                          bool search_allocation_stack = true,
                          bool search_live_stack = true,
                          bool sorted = false) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Returns true if there is any chance that the object (obj) will move.
  bool IsMovableObject(ObjPtr<mirror::Object> obj) constprivate
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Enables us to compacting GC until objects are released.
  void IncrementDisableMovingGC(Thread* self) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  void DecrementDisableMovingGC(Thread* self) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Temporarily disable thread flip for JNI critical calls.
  void IncrementDisableThreadFlip(Thread* self) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  void DecrementDisableThreadFlip(Thread* self) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  void ThreadFlipBegin(Thread* self) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  void ThreadFlipEnd(Thread* self) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Ensures that the obj doesn't cause userfaultfd in JNI critical calls.
  void EnsureObjectUserfaulted(ObjPtr<mirror::Object> obj) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Clear all of the mark bits, doesn't clear bitmaps which have the same live bits as mark bits.
  // Mutator lock is required for GetContinuousSpaces.
  void ClearMarkedObjects() private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Initiates an explicit garbage collection. Guarantees that a GC started after this call has
  // completed.
  void CollectGarbage(bool clear_soft_references, GcCause cause = kGcCauseExplicit) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Does a concurrent GC, provided the GC numbered requested_gc_num has not already been
  // completed. Should only be called by the GC daemon thread through runtime.
  void ConcurrentGC(Thread* self, GcCause cause, bool force_full, uint32_t requested_gc_num) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Implements VMDebug.countInstancesOfClass and JDWP VM_InstanceCount.
  // The boolean decides whether to use IsAssignableFrom or == when comparing classes.
  void CountInstances(const std::vector<Handle<mirror::Class>>& classes,
                      bool use_is_assignable_from,
                      uint64_t* counts) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Removes the growth limit on the alloc space so it may grow to its maximum capacity. Used to
  // implement dalvik.system.VMRuntime.clearGrowthLimit.
  void ClearGrowthLimit() private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Make the current growth limit the new maximum capacity, unmaps pages at the end of spaces
  // which will never be used. Used to implement dalvik.system.VMRuntime.clampGrowthLimit.
  void ClampGrowthLimit() private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Target ideal heap utilization ratio, implements
  // dalvik.system.VMRuntime.getTargetHeapUtilization.
  double GetTargetHeapUtilization() const { return target_utilization_; }

  // Data structure memory usage tracking.
  void RegisterGCAllocation(size_t bytes);
  void RegisterGCDeAllocation(size_t bytes);

  // Set the heap's private space pointers to be the same as the space based on it's type. Public
  // due to usage by tests.
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

  // Set target ideal heap utilization ratio, implements
  // dalvik.system.VMRuntime.setTargetHeapUtilization.
  void SetTargetHeapUtilization(float target);

  // For the alloc space, sets the maximum number of bytes that the heap is allowed to allocate
  // from the system. Doesn't allow the space to exceed its growth limit.
  // Set while we hold gc_complete_lock or collector_type_running_ != kCollectorTypeNone.
  void SetIdealFootprint(size_t max_allowed_footprint);

  // Blocks the caller until the garbage collector becomes idle and returns the type of GC we
  // waited for. Only waits for running collections, ignoring a requested but unstarted GC. Only
  // heuristic, since a new GC may have started by the time we return.
  collector::GcType WaitForGcToComplete(GcCause cause, Thread* self) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Update the heap's process state to a new value, may cause compaction to occur.
  void UpdateProcessState(ProcessState old_process_state, ProcessState new_process_state) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  bool HaveContinuousSpaces() const NO_THREAD_SAFETY_ANALYSIS {
    // No lock since vector empty is thread safe.
    return !continuous_spaces_.empty();
  }

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  REQUIRES(Locks::alloc_tracker_lock_) { return allocation_records_.get(); }

 private:
  // Return the amount of space we allow for native memory when deciding whether to
  // collect. We collect when a weighted sum of Java memory plus native memory exceeds
  // the similarly weighted sum of the Java heap size target and this value.
  ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
    // We keep the traditional limit of max_free_ in place for small heaps,
    // but allow it to be adjusted upward for large heaps to limit GC overhead.
    return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
  }

  // Return the amount of space we allow for native memory when deciding whether to
  // collect. We collect when a weighted sum of Java memory plus native memory exceeds
  // the similarly weighted sum of the Java heap size target and this value.
  ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
    // We keep the traditional limit of max_free_ in place for small heaps,
    // but allow it to be adjusted upward for large heaps to limit GC overhead.
    return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
  }

  // Return the amount of space we allow for native memory when deciding whether to
  // collect. We collect when a weighted sum of Java memory plus native memory exceeds
  // the similarly weighted sum of the Java heap size target and this value.
  ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
    // We keep the traditional limit of max_free_ in place for small heaps,
    // but allow it to be adjusted upward for large heaps to limit GC overhead.
    return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
  }

  // Return the amount of space we allow for native memory when deciding whether to
  // collect. We collect when a weighted sum of Java memory plus native memory exceeds
  // the similarly weighted sum of the Java heap size target and this value.
  ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
    // We keep the traditional limit of max_free_ in place for small heaps,
    // but allow it to be adjusted upward for large heaps to limit GC overhead.
    return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
  }

  // Return the amount of space we allow for native memory when deciding whether to
  // collect. We collect when a weighted sum of Java memory plus native memory exceeds
  // the similarly weighted sum of the Java heap size target and this value.
  ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
    // We keep the traditional limit of max_free_ in place for small heaps,
    // but allow it to be adjusted upward for large heaps to limit GC overhead.
    return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
  }

  // Return the amount of space we allow for native memory when deciding whether to
  // collect. We collect when a weighted sum of Java memory plus native memory exceeds
  // the similarly weighted sum of the Java heap size target and this value.
  ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
    // We keep the traditional limit of max_free_ in place for small heaps,
    // but allow it to be adjusted upward for large heaps to limit GC overhead.
    return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
  }

  // Return the amount of space we allow for native memory when deciding whether to
  // collect. We collect when a weighted sum of Java memory plus native memory exceeds
  // the similarly weighted sum of the Java heap size target and this value.
  ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
    // We keep the traditional limit of max_free_ in place for small heaps,
    // but allow it to be adjusted upward for large heaps to limit GC overhead.
    return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
  }

  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Freed bytes can be negative in cases where we copy objects from a compacted space to a
  // free-list backed space.
  void RecordFree(uint64_t freed_objects, int64_t freed_bytes);

  // Record the bytes freed by thread-local buffer revoke.
  void RecordFreeRevoke();

  accounting::CardTable* GetCardTable() const { return card_table_.get(); }

  accounting::ReadBarrierTable* GetReadBarrierTable() const { return rb_table_.get(); }

  void AddFinalizerReference(Thread* self, ObjPtr<mirror::Object>* object);

  // Returns the number of bytes currently allocated.
  // The result should be treated as an approximation, if it is being concurrently updated.
  size_t GetBytesAllocated() const { return num_bytes_allocated_.load(std::memory_order_relaxed); }

  bool GetUseGenerationalCC() const { return use_generational_cc_; }

  // Returns the number of objects currently allocated.
  size_t GetObjectsAllocated() const private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Returns the total number of objects allocated since the heap was created.
  uint64_t GetObjectsAllocatedEver() const;

  // Returns the total number of bytes allocated since the heap was created.
  uint64_t GetBytesAllocatedEver() const;

  // Returns the total number of objects freed since the heap was created.
  // With default memory order, this should be viewed only as a hint.
  uint64_t GetObjectsFreedEver(std::memory_order mo = std::memory_order_relaxed) const {
    return total_objects_freed_ever_.load(mo);
  }

  // Returns the total number of bytes freed since the heap was created.
  // With default memory order, this should be viewed only as a hint.
  uint64_t GetBytesFreedEver(std::memory_order mo = std::memory_order_relaxed) const {
    return total_bytes_freed_ever_.load(mo);
  }

  space::RegionSpace* GetRegionSpace() const { return region_space_; }

  space::BumpPointerSpace* GetBumpPointerSpace() const { return bump_pointer_space_; }
  // Implements java.lang.Runtime.maxMemory, returning the maximum amount of memory a program can
  // consume. For a regular VM this would relate to the -Xmx option and would return -1 if no Xmx
  // were specified. Android apps start with a growth limit (small heap size) which is
  // cleared/extended for large apps.
  size_t GetMaxMemory() const {
    // There are some race conditions in the allocation code that can cause bytes allocated to
    // become larger than growth_limit_ in rare cases.
    return std::max(GetBytesAllocated(), growth_limit_);
  }

  // Implements java.lang.Runtime.totalMemory, returning approximate amount of memory currently
  // consumed by an application.
  size_t GetTotalMemory() const;

  // Returns approximately how much free memory we have until the next GC happens.
  size_t GetFreeMemoryUntilGC() const {
    return UnsignedDifference(target_footprint_.load(std::memory_order_relaxed),
                              GetBytesAllocated());
  }

  // Returns approximately how much free memory we have until the next OOME happens.
  size_t GetFreeMemoryUntilOOME() const {
    return UnsignedDifference(growth_limit_, GetBytesAllocated());
  }

  // Returns how much free memory we have until we need to grow the heap to perform an allocation.
  // Similar to GetFreeMemoryUntilGC. Implements java.lang.Runtime.freeMemory.
  size_t GetFreeMemory() const {
    return UnsignedDifference(GetTotalMemory(),
                              num_bytes_allocated_.load(std::memory_order_relaxed));
  }

  // Get the space that corresponds to an object's address. Current implementation searches all
  // spaces in turn. If fail_ok is false then failing to find a space will cause an abort.
  // TODO: consider using faster data structure like binary tree.
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
  // Do a pending collector transition.
  void DoPendingCollectorTransition() private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Deflate monitors, ... and trim the spaces.
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
  // Mark and empty stack.
  void FlushAllocStack() private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Revoke all the thread-local allocation stacks.
  void RevokeAllThreadLocalAllocationStacks(Thread* self) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Mark all the objects in the allocation stack in the specified bitmap.
  // TODO: Refactor?
  void MarkAllocStack(accounting::SpaceBitmap<kObjectAlignment>* bitmap1,
                      accounting::SpaceBitmap<kObjectAlignment>* bitmap2,
                      accounting::SpaceBitmap<kLargeObjectAlignment>* large_objects,
                      accounting::ObjectStack* stack) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Mark the specified allocation stack as live.
  void MarkAllocStackAsLive(accounting::ObjectStack* stack) private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Unbind any bound bitmaps.
  void UnBindBitmaps() private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Returns the boot image spaces. There may be multiple boot image spaces.
  const std::vector<space::ImageSpace*>& GetBootImageSpaces() const { return boot_image_spaces_; }

  bool ObjectIsInBootImageSpace(ObjPtr<mirror::Object> obj) const private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  bool IsInBootImageOatFile(const void* p) const private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Get the start address of the boot images if any; otherwise returns 0.
  uint32_t GetBootImagesStartAddress() const { return boot_images_start_address_; }

  // Get the size of all boot images, including the heap and oat areas.
  uint32_t GetBootImagesSize() const { return boot_images_size_; }

  // Check if a pointer points to a boot image.
  bool IsBootImageAddress(const void* p) const {
    return reinterpret_cast<uintptr_t>(p) - boot_images_start_address_ < boot_images_size_;
  }

  space::DlMallocSpace* GetDlMallocSpace() const { return dlmalloc_space_; }

  space::RosAllocSpace* GetRosAllocSpace() const { return rosalloc_space_; }

  // Return the corresponding rosalloc space.
  space::RosAllocSpace* GetRosAllocSpace(gc::allocator::RosAlloc* rosalloc) const private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  space::MallocSpace* GetNonMovingSpace() const { return non_moving_space_; }

  space::LargeObjectSpace* GetLargeObjectsSpace() const { return large_object_space_; }

  // Returns the free list space that may contain movable objects (the
  // one that's not the non-moving space), either rosalloc_space_ or
  // dlmalloc_space_.
  space::MallocSpace* GetPrimaryFreeListSpace() {
    if (kUseRosAlloc) {
      DCHECK(rosalloc_space_ != nullptr);
      // reinterpret_cast is necessary as the space class hierarchy
      // isn't known (#included) yet here.
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
  // GC performance measuring
  void DumpGcPerformanceInfo(std::ostream& os) private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  void ResetGcPerformanceInfo() private : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

 public:
  // Thread pool. Create either the given number of threads, or as per the
  // values of conc_gc_threads_ and parallel_gc_threads_.
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
  // Also deletes the remebered set.
  void RemoveRememberedSet(space::Space* space);

  bool IsCompilingBoot() const;
  bool HasBootImageSpace() const { return !boot_image_spaces_.empty(); }

  ReferenceProcessor* GetReferenceProcessor() { return reference_processor_.get(); }
  TaskProcessor* GetTaskProcessor() { return task_processor_.get(); }

  bool HasZygoteSpace() const { return zygote_space_ != nullptr; }

  // Returns the active concurrent copying collector.
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
        // Assume no transition when a concurrent moving collector is used.
        DCHECK_EQ(collector_type_, foreground_collector_type_);
        return true;
      }
      return false;
    }

    bool IsMovingGCDisabled(Thread * self) private :
        // Return the amount of space we allow for native memory when deciding whether to
        // collect. We collect when a weighted sum of Java memory plus native memory exceeds
        // the similarly weighted sum of the Java heap size target and this value.
        ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
      // We keep the traditional limit of max_free_ in place for small heaps,
      // but allow it to be adjusted upward for large heaps to limit GC overhead.
      return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
    }

    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

   public:
    // Retrieve the current GC number, i.e. the number n such that we completed n GCs so far.
    // Provides acquire ordering, so that if we read this first, and then check whether a GC is
    // required, we know that the GC number read actually preceded the test.
    uint32_t GetCurrentGcNum() { return gcs_completed_.load(std::memory_order_acquire); }

    // Request asynchronous GC. Observed_gc_num is the value of GetCurrentGcNum() when we started to
    // evaluate the GC triggering condition. If a GC has been completed since then, we consider our
    // job done. If we return true, then we ensured that gcs_completed_ will eventually be
    // incremented beyond observed_gc_num. We return false only in corner cases in which we cannot
    // ensure that.
    bool RequestConcurrentGC(
        Thread * self, GcCause cause, bool force_full, uint32_t observed_gc_num) private
        : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

   public:
    // Whether or not we may use a garbage collector, used so that we only create collectors we
    // need.
    bool MayUseCollector(CollectorType type) const;

    // Used by tests to reduce timinig-dependent flakiness in OOME behavior.
    void SetMinIntervalHomogeneousSpaceCompactionByOom(uint64_t interval) {
      min_interval_homogeneous_space_compaction_by_oom_ = interval;
    }

    // Helpers for android.os.Debug.getRuntimeStat().
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

    // Perfetto Art Heap Profiler Support.
    HeapSampler& GetHeapSampler() { return heap_sampler_; }

    void InitPerfettoJavaHeapProf();
    int CheckPerfettoJHPEnabled();
    // In NonTlab case: Check whether we should report a sample allocation and if so report it.
    // Also update state (bytes_until_sample).
    // By calling JHPCheckNonTlabSampleAllocation from different functions for Large allocations and
    // non-moving allocations we are able to use the stack to identify these allocations separately.
    void JHPCheckNonTlabSampleAllocation(Thread * self, mirror::Object * ret, size_t alloc_size);
    // In Tlab case: Calculate the next tlab size (location of next sample point) and whether
    // a sample should be taken.
    size_t JHPCalculateNextTlabSize(Thread * self,
                                    size_t jhp_def_tlab_size,
                                    size_t alloc_size,
                                    bool* take_sample,
                                    size_t* bytes_until_sample);
    // Reduce the number of bytes to the next sample position by this adjustment.
    void AdjustSampleOffset(size_t adjustment);

    // Allocation tracking support
    // Callers to this function use double-checked locking to ensure safety on allocation_records_
    bool IsAllocTrackingEnabled() const {
      return alloc_tracking_enabled_.load(std::memory_order_relaxed);
    }

    void SetAllocTrackingEnabled(bool enabled) REQUIRES(Locks::alloc_tracker_lock_) {
      return allocation_records_.get();
    }

   private:
    // Return the amount of space we allow for native memory when deciding whether to
    // collect. We collect when a weighted sum of Java memory plus native memory exceeds
    // the similarly weighted sum of the Java heap size target and this value.
    ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
      // We keep the traditional limit of max_free_ in place for small heaps,
      // but allow it to be adjusted upward for large heaps to limit GC overhead.
      return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
    }

    // Return the amount of space we allow for native memory when deciding whether to
    // collect. We collect when a weighted sum of Java memory plus native memory exceeds
    // the similarly weighted sum of the Java heap size target and this value.
    ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
      // We keep the traditional limit of max_free_ in place for small heaps,
      // but allow it to be adjusted upward for large heaps to limit GC overhead.
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
    // Create a new alloc space and compact default alloc space to it.
    HomogeneousSpaceCompactResult PerformHomogeneousSpaceCompact() private
        : DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);

   public:
    bool SupportHomogeneousSpaceCompactAndCollectorTransitions() const;

    // Install an allocation listener.
    void SetAllocationListener(AllocationListener * l);
    // Remove an allocation listener. Note: the listener must not be deleted, as for performance
    // reasons, we assume it stays valid when we read it (so that we don't require a lock).
    void RemoveAllocationListener();

    // Install a gc pause listener.
    void SetGcPauseListener(GcPauseListener * l);
    // Get the currently installed gc pause listener, or null.
    GcPauseListener* GetGcPauseListener() {
      return gc_pause_listener_.load(std::memory_order_acquire);
    }
    // Remove a gc pause listener. Note: the listener must not be deleted, as for performance
    // reasons, we assume it stays valid when we read it (so that we don't require a lock).
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

    // Compact source space to target space. Returns the collector used.
    collector::GarbageCollector* Compact(space::ContinuousMemMapAllocSpace * target_space,
                                         space::ContinuousMemMapAllocSpace * source_space,
                                         GcCause gc_cause) DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void LogGC(GcCause gc_cause, collector::GarbageCollector * collector);
    void StartGC(Thread * self, GcCause cause, CollectorType collector_type)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void FinishGC(Thread * self, collector::GcType gc_type) DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    double CalculateGcWeightedAllocatedBytes(uint64_t gc_last_process_cpu_time_ns,
                                             uint64_t current_process_cpu_time) const;

    // Create a mem map with a preferred base address.
    static MemMap MapAnonymousPreferredAddress(
        const char* name, uint8_t* request_begin, size_t capacity, std::string* out_error_str);

    bool SupportHSpaceCompaction() const {
      // Returns true if we can do hspace compaction
      return main_space_backup_ != nullptr;
    }

    // Return the amount of space we allow for native memory when deciding whether to
    // collect. We collect when a weighted sum of Java memory plus native memory exceeds
    // the similarly weighted sum of the Java heap size target and this value.
    ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
      // We keep the traditional limit of max_free_ in place for small heaps,
      // but allow it to be adjusted upward for large heaps to limit GC overhead.
      return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
    }

    // Return the amount of space we allow for native memory when deciding whether to
    // collect. We collect when a weighted sum of Java memory plus native memory exceeds
    // the similarly weighted sum of the Java heap size target and this value.
    ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
      // We keep the traditional limit of max_free_ in place for small heaps,
      // but allow it to be adjusted upward for large heaps to limit GC overhead.
      return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
    }

    // Return the amount of space we allow for native memory when deciding whether to
    // collect. We collect when a weighted sum of Java memory plus native memory exceeds
    // the similarly weighted sum of the Java heap size target and this value.
    ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
      // We keep the traditional limit of max_free_ in place for small heaps,
      // but allow it to be adjusted upward for large heaps to limit GC overhead.
      return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
    }

    // Return the amount of space we allow for native memory when deciding whether to
    // collect. We collect when a weighted sum of Java memory plus native memory exceeds
    // the similarly weighted sum of the Java heap size target and this value.
    ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
      // We keep the traditional limit of max_free_ in place for small heaps,
      // but allow it to be adjusted upward for large heaps to limit GC overhead.
      return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
    }

    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // Checks whether we should garbage collect:
    ALWAYS_INLINE bool ShouldConcurrentGCForJava(size_t new_num_bytes_allocated);
    float NativeMemoryOverTarget(size_t current_native_bytes, bool is_gc_concurrent);
    void CheckGCForNative(Thread * self) DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    accounting::ObjectStack* GetMarkStack() { return mark_stack_.get(); }

    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // Handles Allocate()'s slow allocation path with GC involved after an initial allocation
    // attempt failed.
    // Called with thread suspension disallowed, but re-enables it, and may suspend, internally.
    // Returns null if instrumentation or the allocator changed.
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
    // Allocate into a specific space.
    mirror::Object* AllocateInto(
        Thread * self, space::AllocSpace * space, ObjPtr<mirror::Class> c, size_t bytes)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // Need to do this with mutators paused so that somebody doesn't accidentally allocate into the
    // wrong space.
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
    // Are we out of memory, and thus should force a GC or fail?
    // For concurrent collectors, out of memory is defined by growth_limit_.
    // For nonconcurrent collectors it is defined by target_footprint_ unless grow is
    // set. If grow is set, the limit is growth_limit_ and we adjust target_footprint_
    // to accomodate the allocation.
    ALWAYS_INLINE bool IsOutOfMemoryOnAllocation(
        AllocatorType allocator_type, size_t alloc_size, bool grow);

    // Blocks the caller until the garbage collector becomes idle and returns the type of GC we
    // waited for.
    collector::GcType WaitForGcToCompleteLocked(GcCause cause, Thread * self)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void RequestCollectorTransition(CollectorType desired_collector_type, uint64_t delta_time)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    void RequestConcurrentGCAndSaveObject(
        Thread * self, bool force_full, uint32_t observed_gc_num, ObjPtr<mirror::Object>* obj)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    static constexpr uint32_t GC_NUM_ANY = std::numeric_limits<uint32_t>::max();

    // Sometimes CollectGarbageInternal decides to run a different Gc than you requested. Returns
    // which type of Gc was actually run.
    // We pass in the intended GC sequence number to ensure that multiple approximately concurrent
    // requests result in a single GC; clearly redundant request will be pruned.  A requested_gc_num
    // of GC_NUM_ANY indicates that we should not prune redundant requests.  (In the unlikely case
    // that gcs_completed_ gets this big, we just accept a potential extra GC or two.)
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
    // Find a collector based on GC type.
    collector::GarbageCollector* FindCollectorByGcType(collector::GcType gc_type);

    // Create the main free list malloc space, either a RosAlloc space or DlMalloc space.
    void CreateMainMallocSpace(
        MemMap && mem_map, size_t initial_size, size_t growth_limit, size_t capacity);

    // Create a malloc space based on a mem map. Does not set the space as default.
    space::MallocSpace* CreateMallocSpaceFromMemMap(MemMap && mem_map,
                                                    size_t initial_size,
                                                    size_t growth_limit,
                                                    size_t capacity,
                                                    const char* name,
                                                    bool can_move_objects);

    // Given the current contents of the alloc space, increase the allowed heap footprint to match
    // the target utilization ratio.  This should only be called immediately after a full garbage
    // collection. bytes_allocated_before_gc is used to measure bytes / second for the period which
    // the GC was run.
    // This is only called by the thread that set collector_type_running_ to a value other than
    // kCollectorTypeNone, or while holding gc_complete_lock, and ensuring that
    // collector_type_running_ is kCollectorTypeNone.
    void GrowForUtilization(collector::GarbageCollector * collector_ran,
                            size_t bytes_allocated_before_gc = 0)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    size_t GetPercentFree();

    // Swap the allocation stack with the live stack.
    void SwapStacks() DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // Clear cards and update the mod union table. When process_alloc_space_cards is true,
    // if clear_alloc_space_cards is true, then we clear cards instead of ageing them. We do
    // not process the alloc space if process_alloc_space_cards is false.
    void ProcessCards(TimingLogger * timings,
                      bool use_rem_sets,
                      bool process_alloc_space_cards,
                      bool clear_alloc_space_cards) DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // Push an object onto the allocation stack.
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
    // What kind of concurrency behavior is the runtime after? Currently true for concurrent mark
    // sweep GC, false for other GC types.
    bool IsGcConcurrent() const ALWAYS_INLINE {
      return collector_type_ == kCollectorTypeCC || collector_type_ == kCollectorTypeCMC ||
             collector_type_ == kCollectorTypeCMS || collector_type_ == kCollectorTypeCCBackground;
    }

    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // Trim 0 pages at the end of reference tables.
    void TrimIndirectReferenceTables(Thread * self);

    void UpdateGcCountRateHistograms() DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // GC stress mode attempts to do one GC per unique backtrace.
    void CheckGcStressMode(Thread * self, ObjPtr<mirror::Object> * obj)
        DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    collector::GcType NonStickyGcType() const {
      return HasZygoteSpace() ? collector::kGcTypePartial : collector::kGcTypeFull;
    }

    // Return the amount of space we allow for native memory when deciding whether to
    // collect. We collect when a weighted sum of Java memory plus native memory exceeds
    // the similarly weighted sum of the Java heap size target and this value.
    ALWAYS_INLINE size_t NativeAllocationGcWatermark() const {
      // We keep the traditional limit of max_free_ in place for small heaps,
      // but allow it to be adjusted upward for large heaps to limit GC overhead.
      return target_footprint_.load(std::memory_order_relaxed) / 8 + max_free_;
    }

    DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // On switching app from background to foreground, grow the heap size
    // to incorporate foreground heap growth multiplier.
    void GrowHeapOnJankPerceptibleSwitch() DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // Update *_freed_ever_ counters to reflect current GC values.
    void IncrementFreedEver();

    // Remove a vlog code from heap-inl.h which is transitively included in half the world.
    static void VlogHeapGrowth(
        size_t max_allowed_footprint, size_t new_footprint, size_t alloc_size);

    // Return our best approximation of the number of bytes of native memory that
    // are currently in use, and could possibly be reclaimed as an indirect result
    // of a garbage collection.
    size_t GetNativeBytes();

    // Set concurrent_start_bytes_ to a reasonable guess, given target_footprint_ .
    void SetDefaultConcurrentStartBytes() DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // This version assumes no concurrent updaters.
    void SetDefaultConcurrentStartBytesLocked();

    // All-known continuous spaces, where objects lie within fixed bounds.
    std::vector<space::ContinuousSpace*> continuous_spaces_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // All-known discontinuous spaces, where objects may be placed throughout virtual memory.
    std::vector<space::DiscontinuousSpace*> discontinuous_spaces_ DISALLOW_IMPLICIT_CONSTRUCTORS(
        Heap);
    // All-known alloc spaces, where objects may be or have been allocated.
    std::vector<space::AllocSpace*> alloc_spaces_;

    // A space where non-movable objects are allocated, when compaction is enabled it contains
    // Classes, ArtMethods, ArtFields, and non moving objects.
    space::MallocSpace* non_moving_space_;

    // Space which we use for the kAllocatorTypeROSAlloc.
    space::RosAllocSpace* rosalloc_space_;

    // Space which we use for the kAllocatorTypeDlMalloc.
    space::DlMallocSpace* dlmalloc_space_;

    // The main space is the space which the GC copies to and from on process state updates. This
    // space is typically either the dlmalloc_space_ or the rosalloc_space_.
    space::MallocSpace* main_space_;

    // The large object space we are currently allocating into.
    space::LargeObjectSpace* large_object_space_;

    // The card table, dirtied by the write barrier.
    std::unique_ptr<accounting::CardTable> card_table_;

    std::unique_ptr<accounting::ReadBarrierTable> rb_table_;

    // A mod-union table remembers all of the references from the it's space to other spaces.
    AllocationTrackingSafeMap<space::Space*, accounting::ModUnionTable*, kAllocatorTagHeap>
        mod_union_tables_;

    // A remembered set remembers all of the references from the it's space to the target space.
    AllocationTrackingSafeMap<space::Space*, accounting::RememberedSet*, kAllocatorTagHeap>
        remembered_sets_;

    // The current collector type.
    CollectorType collector_type_;
    // Which collector we use when the app is in the foreground.
    const CollectorType foreground_collector_type_;
    // Which collector we will use when the app is notified of a transition to background.
    CollectorType background_collector_type_;
    // Desired collector type, heap trimming daemon transitions the heap if it is !=
    // collector_type_.
    CollectorType desired_collector_type_;

    // Lock which guards pending tasks.
    Mutex* pending_task_lock_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // How many GC threads we may use for paused parts of garbage collection.
    const size_t parallel_gc_threads_;

    // How many GC threads we may use for unpaused parts of garbage collection.
    const size_t conc_gc_threads_;

    // Boolean for if we are in low memory mode.
    const bool low_memory_mode_;

    // If we get a pause longer than long pause log threshold, then we print out the GC after it
    // finishes.
    const size_t long_pause_log_threshold_;

    // If we get a GC longer than long GC log threshold, then we print out the GC after it finishes.
    const size_t long_gc_log_threshold_;

    // Starting time of the new process; meant to be used for measuring total process CPU time.
    uint64_t process_cpu_start_time_ns_;

    // Last time (before and after) GC started; meant to be used to measure the
    // duration between two GCs.
    uint64_t pre_gc_last_process_cpu_time_ns_;
    uint64_t post_gc_last_process_cpu_time_ns_;

    // allocated_bytes * (current_process_cpu_time - [pre|post]_gc_last_process_cpu_time)
    double pre_gc_weighted_allocated_bytes_;
    double post_gc_weighted_allocated_bytes_;

    // If we ignore the target footprint it lets the heap grow until it hits the heap capacity, this
    // is useful for benchmarking since it reduces time spent in GC to a low %.
    const bool ignore_target_footprint_;

    // If we are running tests or some other configurations we might not actually
    // want logs for explicit gcs since they can get spammy.
    const bool always_log_explicit_gcs_;

    // Lock which guards zygote space creation.
    Mutex zygote_creation_lock_;

    // Non-null iff we have a zygote space. Doesn't contain the large objects allocated before
    // zygote space creation.
    space::ZygoteSpace* zygote_space_;

    // Minimum allocation size of large object.
    size_t large_object_threshold_;

    // Guards access to the state of GC, associated conditional variable is used to signal when a GC
    // completes.
    Mutex* gc_complete_lock_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    std::unique_ptr<ConditionVariable> gc_complete_cond_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // Used to synchronize between JNI critical calls and the thread flip of the CC collector.
    Mutex* thread_flip_lock_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    std::unique_ptr<ConditionVariable> thread_flip_cond_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // This counter keeps track of how many threads are currently in a JNI critical section. This is
    // incremented once per thread even with nested enters.
    size_t disable_thread_flip_count_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    bool thread_flip_running_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // Reference processor;
    std::unique_ptr<ReferenceProcessor> reference_processor_;

    // Task processor, proxies heap trim requests to the daemon threads.
    std::unique_ptr<TaskProcessor> task_processor_;

    // The following are declared volatile only for debugging purposes; it shouldn't otherwise
    // matter.
    // Collector type of the running GC.
    volatile CollectorType collector_type_running_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // Cause of the last running GC.
    volatile GcCause last_gc_cause_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // The thread currently running the GC.
    volatile Thread* thread_running_gc_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // Last Gc type we ran. Used by WaitForConcurrentGc to know which Gc was waited on.
    volatile collector::GcType last_gc_type_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    collector::GcType next_gc_type_;

    // Maximum size that the heap can reach.
    size_t capacity_;

    // The size the heap is limited to. This is initially smaller than capacity, but for largeHeap
    // programs it is "cleared" making it the same as capacity.
    // Only weakly enforced for simultaneous allocations.
    size_t growth_limit_;

    // Requested initial heap size. Temporarily ignored after a fork, but then reestablished after
    // a while to usually trigger the initial GC.
    size_t initial_heap_size_;

    // Target size (as in maximum allocatable bytes) for the heap. Weakly enforced as a limit for
    // non-concurrent GC. Used as a guideline for computing concurrent_start_bytes_ in the
    // concurrent GC case. Updates normally occur while collector_type_running_ is not none.
    Atomic<size_t> target_footprint_;

    Mutex process_state_update_lock_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // Computed with foreground-multiplier in GrowForUtilization() when run in
    // jank non-perceptible state. On update to process state from background to
    // foreground we set target_footprint_ and concurrent_start_bytes_ to the corresponding value.
    size_t min_foreground_target_footprint_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // When num_bytes_allocated_ exceeds this amount then a concurrent GC should be requested so
    // that it completes ahead of an allocation failing. A multiple of this is also used to
    // determine when to trigger a GC in response to native allocation. After initialization, this
    // is only updated by the thread that set collector_type_running_ to a value other than
    // kCollectorTypeNone, or while holding gc_complete_lock, and ensuring that
    // collector_type_running_ is kCollectorTypeNone.
    size_t concurrent_start_bytes_;

    // Since the heap was created, how many bytes have been freed.
    std::atomic<uint64_t> total_bytes_freed_ever_;

    // Since the heap was created, how many objects have been freed.
    std::atomic<uint64_t> total_objects_freed_ever_;

    // Number of bytes currently allocated and not yet reclaimed. Includes active
    // TLABS in their entirety, even if they have not yet been parceled out.
    Atomic<size_t> num_bytes_allocated_;

    // Number of registered native bytes allocated. Adjusted after each RegisterNativeAllocation and
    // RegisterNativeFree. Used to  help determine when to trigger GC for native allocations. Should
    // not include bytes allocated through the system malloc, since those are implicitly included.
    Atomic<size_t> native_bytes_registered_;

    // Approximately the smallest value of GetNativeBytes() we've seen since the last GC.
    Atomic<size_t> old_native_bytes_allocated_;

    // Total number of native objects of which we were notified since the beginning of time, mod
    // 2^32. Allows us to check for GC only roughly every kNotifyNativeInterval allocations.
    Atomic<uint32_t> native_objects_notified_;

    // Number of bytes freed by thread local buffer revokes. This will
    // cancel out the ahead-of-time bulk counting of bytes allocated in
    // rosalloc thread-local buffers.  It is temporarily accumulated
    // here to be subtracted from num_bytes_allocated_ later at the next
    // GC.
    Atomic<size_t> num_bytes_freed_revoke_;

    // Records the number of bytes allocated at the time of GC, which is used later to calculate
    // how many bytes have been allocated since the last GC
    size_t num_bytes_alive_after_gc_;

    // Info related to the current or previous GC iteration.
    collector::Iteration current_gc_iteration_;

    // Heap verification flags.
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

    // RAII that temporarily disables the rosalloc verification during
    // the zygote fork.
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

    // Parallel GC data structures.
    std::unique_ptr<ThreadPool> thread_pool_;

    // A bitmap that is set corresponding to the known live objects since the last GC cycle.
    std::unique_ptr<accounting::HeapBitmap> live_bitmap_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // A bitmap that is set corresponding to the marked objects in the current GC cycle.
    std::unique_ptr<accounting::HeapBitmap> mark_bitmap_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // Mark stack that we reuse to avoid re-allocating the mark stack.
    std::unique_ptr<accounting::ObjectStack> mark_stack_;

    // Allocation stack, new allocations go here so that we can do sticky mark bits. This enables us
    // to use the live bitmap as the old mark bitmap.
    const size_t max_allocation_stack_size_;
    std::unique_ptr<accounting::ObjectStack> allocation_stack_;

    // Second allocation stack so that we can process allocation with the heap unlocked.
    std::unique_ptr<accounting::ObjectStack> live_stack_;

    // Allocator type.
    AllocatorType current_allocator_;
    const AllocatorType current_non_moving_allocator_;

    // Which GCs we run in order when an allocation fails.
    std::vector<collector::GcType> gc_plan_;

    // Bump pointer spaces.
    space::BumpPointerSpace* bump_pointer_space_;
    // Temp space is the space which the semispace collector copies to.
    space::BumpPointerSpace* temp_space_;

    // Region space, used by the concurrent collector.
    space::RegionSpace* region_space_;

    // Minimum free guarantees that you always have at least min_free_ free bytes after growing for
    // utilization, regardless of target utilization ratio.
    const size_t min_free_;

    // The ideal maximum free size, when we grow the heap for utilization.
    const size_t max_free_;

    // Target ideal heap utilization ratio.
    double target_utilization_;

    // How much more we grow the heap when we are a foreground app instead of background.
    double foreground_heap_growth_multiplier_;

    // The amount of native memory allocation since the last GC required to cause us to wait for a
    // collection as a result of native allocation. Very large values can cause the device to run
    // out of memory, due to lack of finalization to reclaim native memory.  Making it too small can
    // cause jank in apps like launcher that intentionally allocate large amounts of memory in rapid
    // succession. (b/122099093) 1/4 to 1/3 of physical memory seems to be a good number.
    const size_t stop_for_native_allocs_;

    // Total time which mutators are paused or waiting for GC to complete.
    uint64_t total_wait_time_;

    // The current state of heap verification, may be enabled or disabled.
    VerifyObjectMode verify_object_mode_;

    // Compacting GC disable count, prevents compacting GC from running iff > 0.
    size_t disable_moving_gc_count_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    std::vector<collector::GarbageCollector*> garbage_collectors_;
    collector::SemiSpace* semi_space_collector_;
    collector::MarkCompact* mark_compact_;
    Atomic<collector::ConcurrentCopying*> active_concurrent_copying_collector_;
    collector::ConcurrentCopying* young_concurrent_copying_collector_;
    collector::ConcurrentCopying* concurrent_copying_collector_;

    const bool is_running_on_memory_tool_;
    const bool use_tlab_;

    // Pointer to the space which becomes the new main space when we do homogeneous space
    // compaction. Use unique_ptr since the space is only added during the homogeneous compaction
    // phase.
    std::unique_ptr<space::MallocSpace> main_space_backup_;

    // Minimal interval allowed between two homogeneous space compactions caused by OOM.
    uint64_t min_interval_homogeneous_space_compaction_by_oom_;

    // Times of the last homogeneous space compaction caused by OOM.
    uint64_t last_time_homogeneous_space_compaction_by_oom_;

    // Saved OOMs by homogeneous space compaction.
    Atomic<size_t> count_delayed_oom_;

    // Count for requested homogeneous space compaction.
    Atomic<size_t> count_requested_homogeneous_space_compaction_;

    // Count for ignored homogeneous space compaction.
    Atomic<size_t> count_ignored_homogeneous_space_compaction_;

    // Count for performed homogeneous space compaction.
    Atomic<size_t> count_performed_homogeneous_space_compaction_;

    // The number of garbage collections (either young or full, not trims or the like) we have
    // completed since heap creation. We include requests that turned out to be impossible
    // because they were disabled. We guard against wrapping, though that's unlikely.
    // Increment is guarded by gc_complete_lock_.
    Atomic<uint32_t> gcs_completed_;

    // The number of the last garbage collection that has been requested.  A value of gcs_completed
    // + 1 indicates that another collection is needed or in progress. A value of gcs_completed_ or
    // (logically) less means that no new GC has been requested.
    Atomic<uint32_t> max_gc_requested_;

    // Active tasks which we can modify (change target time, desired collector type, etc..).
    CollectorTransitionTask* pending_collector_transition_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    HeapTrimTask* pending_heap_trim_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // Whether or not we use homogeneous space compaction to avoid OOM errors.
    bool use_homogeneous_space_compaction_for_oom_;

    // If true, enable generational collection when using the Concurrent Copying
    // (CC) collector, i.e. use sticky-bit CC for minor collections and (full) CC
    // for major collections. Set in Heap constructor.
    const bool use_generational_cc_;

    // True if the currently running collection has made some thread wait.
    bool running_collection_is_blocking_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // The number of blocking GC runs.
    uint64_t blocking_gc_count_;
    // The total duration of blocking GC runs.
    uint64_t blocking_gc_time_;
    // The duration of the window for the GC count rate histograms.
    static constexpr uint64_t kGcCountRateHistogramWindowDuration =
        MsToNs(10 * 1000);  // 10s.
                            // Maximum number of missed histogram windows for which statistics will
                            // be collected.
    static constexpr uint64_t kGcCountRateHistogramMaxNumMissedWindows = 100;
    // The last time when the GC count rate histograms were updated.
    // This is rounded by kGcCountRateHistogramWindowDuration (a multiple of 10s).
    uint64_t last_update_time_gc_count_rate_histograms_;
    // The running count of GC runs in the last window.
    uint64_t gc_count_last_window_;
    // The running count of blocking GC runs in the last window.
    uint64_t blocking_gc_count_last_window_;
    // The maximum number of buckets in the GC count rate histograms.
    static constexpr size_t kGcCountRateMaxBucketCount = 200;
    // The histogram of the number of GC invocations per window duration.
    Histogram<uint64_t> gc_count_rate_histogram_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // The histogram of the number of blocking GC invocations per window duration.
    Histogram<uint64_t> blocking_gc_count_rate_histogram_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // Allocation tracking support
    Atomic<bool> alloc_tracking_enabled_;
    std::unique_ptr<AllocRecordObjectMap> allocation_records_;
    size_t alloc_record_depth_;

    // Perfetto Java Heap Profiler support.
    HeapSampler heap_sampler_;

    // GC stress related data structures.
    Mutex* backtrace_lock_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // Debugging variables, seen backtraces vs unique backtraces.
    Atomic<uint64_t> seen_backtrace_count_;
    Atomic<uint64_t> unique_backtrace_count_;
    // Stack trace hashes that we already saw,
    std::unordered_set<uint64_t> seen_backtraces_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // We disable GC when we are shutting down the runtime in case there are daemon threads still
    // allocating.
    bool gc_disabled_for_shutdown_ DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
    // Turned on by -XX:DumpRegionInfoBeforeGC and -XX:DumpRegionInfoAfterGC to
    // emit region info before and after each GC cycle.
    bool dump_region_info_before_gc_;
    bool dump_region_info_after_gc_;

    // Boot image spaces.
    std::vector<space::ImageSpace*> boot_image_spaces_;

    // Boot image address range. Includes images and oat files.
    uint32_t boot_images_start_address_;
    uint32_t boot_images_size_;

    // The number of times we initiated a GC of last resort to try to avoid an OOME.
    Atomic<uint64_t> pre_oome_gc_count_;

    // An installed allocation listener.
    Atomic<AllocationListener*> alloc_listener_;
    // An installed GC Pause listener.
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

}  // namespace gc

}  // namespace art

#endif  // ART_RUNTIME_GC_HEAP_H_