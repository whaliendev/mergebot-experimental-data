#include "heap.h"
#define ATRACE_TAG ATRACE_TAG_DALVIK
#include <cutils/trace.h>
#include <limits>
#include <memory>
#include <unwind.h>
#include <vector>
#include "art_field-inl.h"
#include "base/allocator.h"
#include "base/dumpable.h"
#include "base/histogram-inl.h"
#include "base/stl_util.h"
#include "base/time_utils.h"
#include "common_throws.h"
#include "cutils/sched_policy.h"
#include "debugger.h"
#include "dex_file-inl.h"
#include "gc/accounting/atomic_stack.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/accounting/heap_bitmap-inl.h"
#include "gc/accounting/mod_union_table.h"
#include "gc/accounting/mod_union_table-inl.h"
#include "gc/accounting/remembered_set.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/collector/concurrent_copying.h"
#include "gc/collector/mark_compact.h"
#include "gc/collector/mark_sweep-inl.h"
#include "gc/collector/partial_mark_sweep.h"
#include "gc/collector/semi_space.h"
#include "gc/collector/sticky_mark_sweep.h"
#include "gc/reference_processor.h"
#include "gc/space/bump_pointer_space.h"
#include "gc/space/dlmalloc_space-inl.h"
#include "gc/space/image_space.h"
#include "gc/space/large_object_space.h"
#include "gc/space/region_space.h"
#include "gc/space/rosalloc_space-inl.h"
#include "gc/space/space-inl.h"
#include "gc/space/zygote_space.h"
#include "gc/task_processor.h"
#include "entrypoints/quick/quick_alloc_entrypoints.h"
#include "heap-inl.h"
#include "image.h"
#include "intern_table.h"
#include "mirror/class-inl.h"
#include "mirror/object.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/reference-inl.h"
#include "os.h"
#include "reflection.h"
#include "runtime.h"
#include "ScopedLocalRef.h"
#include "scoped_thread_state_change.h"
#include "handle_scope-inl.h"
#include "thread_list.h"
#include "well_known_classes.h"
namespace art {
namespace gc {
static constexpr size_t kCollectorTransitionStressIterations = 0;
static constexpr size_t kCollectorTransitionStressWait = 10 * 1000;
static constexpr size_t kMinConcurrentRemainingBytes = 128 * KB;
static constexpr size_t kMaxConcurrentRemainingBytes = 512 * KB;
static constexpr double kStickyGcThroughputAdjustment = 1.0;
static constexpr bool kCompactZygote = kMovingCollector;
static constexpr size_t kAllocationStackReserveSize = 1024;
static const size_t kDefaultMarkStackSize = 64 * KB;
static const char* kDlMallocSpaceName[2] = {"main dlmalloc space", "main dlmalloc space 1"};
static const char* kRosAllocSpaceName[2] = {"main rosalloc space", "main rosalloc space 1"};
static const char* kMemMapSpaceName[2] = {"main space", "main space 1"};
static const char* kNonMovingSpaceName = "non moving space";
static const char* kZygoteSpaceName = "zygote space";
static constexpr size_t kGSSBumpPointerSpaceCapacity = 32 * MB;
static constexpr bool kGCALotMode = false;
static constexpr size_t kGcAlotAllocationStackSize = 4 * KB /
    sizeof(mirror::HeapReference<mirror::Object>);
static constexpr size_t kVerifyObjectAllocationStackSize = 16 * KB /
    sizeof(mirror::HeapReference<mirror::Object>);
static constexpr size_t kDefaultAllocationStackSize = 8 * MB /
    sizeof(mirror::HeapReference<mirror::Object>);
static constexpr uint64_t kNativeAllocationFinalizeTimeout = MsToNs(250u);
Heap::Heap(size_t initial_size, size_t growth_limit, size_t min_free, size_t max_free,
           double target_utilization, double foreground_heap_growth_multiplier,
           size_t capacity, size_t non_moving_space_capacity, const std::string& image_file_name,
           const InstructionSet image_instruction_set, CollectorType foreground_collector_type,
           CollectorType background_collector_type,
           space::LargeObjectSpaceType large_object_space_type, size_t large_object_threshold,
           size_t parallel_gc_threads, size_t conc_gc_threads, bool low_memory_mode,
           size_t long_pause_log_threshold, size_t long_gc_log_threshold,
           bool ignore_max_footprint, bool use_tlab,
           bool verify_pre_gc_heap, bool verify_pre_sweeping_heap, bool verify_post_gc_heap,
           bool verify_pre_gc_rosalloc, bool verify_pre_sweeping_rosalloc,
           bool verify_post_gc_rosalloc, bool gc_stress_mode,
           bool use_homogeneous_space_compaction_for_oom,
           uint64_t min_interval_homogeneous_space_compaction_by_oom)
    : non_moving_space_(nullptr),
      rosalloc_space_(nullptr),
      dlmalloc_space_(nullptr),
      main_space_(nullptr),
      collector_type_(kCollectorTypeNone),
      foreground_collector_type_(foreground_collector_type),
      background_collector_type_(background_collector_type),
      desired_collector_type_(foreground_collector_type_),
      pending_task_lock_(nullptr),
      parallel_gc_threads_(parallel_gc_threads),
      conc_gc_threads_(conc_gc_threads),
      low_memory_mode_(low_memory_mode),
      long_pause_log_threshold_(long_pause_log_threshold),
      long_gc_log_threshold_(long_gc_log_threshold),
      ignore_max_footprint_(ignore_max_footprint),
      zygote_creation_lock_("zygote creation lock", kZygoteCreationLock),
      zygote_space_(nullptr),
      large_object_threshold_(large_object_threshold),
      collector_type_running_(kCollectorTypeNone),
      last_gc_type_(collector::kGcTypeNone),
      next_gc_type_(collector::kGcTypePartial),
      capacity_(capacity),
      growth_limit_(growth_limit),
      max_allowed_footprint_(initial_size),
      native_footprint_gc_watermark_(initial_size),
      native_need_to_run_finalization_(false),
      process_state_(kProcessStateJankPerceptible),
      concurrent_start_bytes_(std::numeric_limits<size_t>::max()),
      total_bytes_freed_ever_(0),
      total_objects_freed_ever_(0),
      num_bytes_allocated_(0),
      native_bytes_allocated_(0),
      num_bytes_freed_revoke_(0),
      verify_missing_card_marks_(false),
      verify_system_weaks_(false),
      verify_pre_gc_heap_(verify_pre_gc_heap),
      verify_pre_sweeping_heap_(verify_pre_sweeping_heap),
      verify_post_gc_heap_(verify_post_gc_heap),
      verify_mod_union_table_(false),
      verify_pre_gc_rosalloc_(verify_pre_gc_rosalloc),
      verify_pre_sweeping_rosalloc_(verify_pre_sweeping_rosalloc),
      verify_post_gc_rosalloc_(verify_post_gc_rosalloc),
      gc_stress_mode_(gc_stress_mode),
      max_allocation_stack_size_(kGCALotMode ? kGcAlotAllocationStackSize
          : (kVerifyObjectSupport > kVerifyObjectModeFast) ? kVerifyObjectAllocationStackSize :
          kDefaultAllocationStackSize),
      current_allocator_(kAllocatorTypeDlMalloc),
      current_non_moving_allocator_(kAllocatorTypeNonMoving),
      bump_pointer_space_(nullptr),
      temp_space_(nullptr),
      region_space_(nullptr),
      min_free_(min_free),
      max_free_(max_free),
      target_utilization_(target_utilization),
      foreground_heap_growth_multiplier_(foreground_heap_growth_multiplier),
      total_wait_time_(0),
      total_allocation_time_(0),
      verify_object_mode_(kVerifyObjectModeDisabled),
      disable_moving_gc_count_(0),
      running_on_valgrind_(Runtime::Current()->RunningOnValgrind()),
      use_tlab_(use_tlab),
      main_space_backup_(nullptr),
      min_interval_homogeneous_space_compaction_by_oom_(
          min_interval_homogeneous_space_compaction_by_oom),
      last_time_homogeneous_space_compaction_by_oom_(NanoTime()),
      pending_collector_transition_(nullptr),
      pending_heap_trim_(nullptr),
      use_homogeneous_space_compaction_for_oom_(use_homogeneous_space_compaction_for_oom),
      running_collection_is_blocking_(false),
      blocking_gc_count_(0U),
      blocking_gc_time_(0U),
      last_update_time_gc_count_rate_histograms_(
          (NanoTime() / kGcCountRateHistogramWindowDuration) * kGcCountRateHistogramWindowDuration),
      gc_count_last_window_(0U),
      blocking_gc_count_last_window_(0U),
      gc_count_rate_histogram_("gc count rate histogram", 1U, kGcCountRateMaxBucketCount),
      blocking_gc_count_rate_histogram_("blocking gc count rate histogram", 1U,
                                        kGcCountRateMaxBucketCount),
      alloc_tracking_enabled_(false),
      backtrace_lock_(nullptr),
      seen_backtrace_count_(0u),
      unique_backtrace_count_(0u) {
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "Heap() entering";
  }
  Runtime* const runtime = Runtime::Current();
  const bool is_zygote = runtime->IsZygote();
  if (!is_zygote) {
    if (background_collector_type_ != foreground_collector_type_) {
      VLOG(heap) << "Disabling background compaction for non zygote";
      background_collector_type_ = foreground_collector_type_;
    }
  }
  ChangeCollector(desired_collector_type_);
  live_bitmap_.reset(new accounting::HeapBitmap(this));
  mark_bitmap_.reset(new accounting::HeapBitmap(this));
  uint8_t* requested_alloc_space_begin = nullptr;
  if (foreground_collector_type_ == kCollectorTypeCC) {
    CHECK_GE(300 * MB, non_moving_space_capacity);
    requested_alloc_space_begin = reinterpret_cast<uint8_t*>(300 * MB) - non_moving_space_capacity;
  }
  if (!image_file_name.empty()) {
    ATRACE_BEGIN("ImageSpace::Create");
    std::string error_msg;
    auto* image_space = space::ImageSpace::Create(image_file_name.c_str(), image_instruction_set,
                                                  &error_msg);
    ATRACE_END();
    if (image_space != nullptr) {
      AddSpace(image_space);
      uint8_t* oat_file_end_addr = image_space->GetImageHeader().GetOatFileEnd();
      CHECK_GT(oat_file_end_addr, image_space->End());
      requested_alloc_space_begin = AlignUp(oat_file_end_addr, kPageSize);
    } else {
      LOG(ERROR) << "Could not create image space with image file '" << image_file_name << "'. "
                   << "Attempting to fall back to imageless running. Error was: " << error_msg;
    }
  }
  if (foreground_collector_type_ == kCollectorTypeGSS ||
      foreground_collector_type_ == kCollectorTypeCC) {
    use_homogeneous_space_compaction_for_oom_ = false;
  }
  bool support_homogeneous_space_compaction =
      background_collector_type_ == gc::kCollectorTypeHomogeneousSpaceCompact ||
      use_homogeneous_space_compaction_for_oom_;
  bool separate_non_moving_space = is_zygote ||
      support_homogeneous_space_compaction || IsMovingGc(foreground_collector_type_) ||
      IsMovingGc(background_collector_type_);
  if (foreground_collector_type == kCollectorTypeGSS) {
    separate_non_moving_space = false;
  }
  std::unique_ptr<MemMap> main_mem_map_1;
  std::unique_ptr<MemMap> main_mem_map_2;
  uint8_t* request_begin = requested_alloc_space_begin;
  if (request_begin != nullptr && separate_non_moving_space) {
    request_begin += non_moving_space_capacity;
  }
  std::string error_str;
  std::unique_ptr<MemMap> non_moving_space_mem_map;
  ATRACE_BEGIN("Create heap maps");
  if (separate_non_moving_space) {
    const char* space_name = is_zygote ? kZygoteSpaceName: kNonMovingSpaceName;
    non_moving_space_mem_map.reset(
        MemMap::MapAnonymous(space_name, requested_alloc_space_begin,
                             non_moving_space_capacity, PROT_READ | PROT_WRITE, true, false,
                             &error_str));
    CHECK(non_moving_space_mem_map != nullptr) << error_str;
    request_begin = reinterpret_cast<uint8_t*>(300 * MB);
  }
  if (foreground_collector_type_ != kCollectorTypeCC) {
    if (separate_non_moving_space) {
      main_mem_map_1.reset(MapAnonymousPreferredAddress(kMemMapSpaceName[0], request_begin,
                                                        capacity_, &error_str));
    } else {
      main_mem_map_1.reset(MemMap::MapAnonymous(kMemMapSpaceName[0], request_begin, capacity_,
                                                PROT_READ | PROT_WRITE, true, false,
                                                &error_str));
    }
    CHECK(main_mem_map_1.get() != nullptr) << error_str;
  }
  if (support_homogeneous_space_compaction ||
      background_collector_type_ == kCollectorTypeSS ||
      foreground_collector_type_ == kCollectorTypeSS) {
    main_mem_map_2.reset(MapAnonymousPreferredAddress(kMemMapSpaceName[1], main_mem_map_1->End(),
                                                      capacity_, &error_str));
    CHECK(main_mem_map_2.get() != nullptr) << error_str;
  }
  ATRACE_END();
  ATRACE_BEGIN("Create spaces");
  if (separate_non_moving_space) {
    const size_t size = non_moving_space_mem_map->Size();
    non_moving_space_ = space::DlMallocSpace::CreateFromMemMap(
        non_moving_space_mem_map.release(), "zygote / non moving space", kDefaultStartingSize,
        initial_size, size, size, false);
    non_moving_space_->SetFootprintLimit(non_moving_space_->Capacity());
    CHECK(non_moving_space_ != nullptr) << "Failed creating non moving space "
        << requested_alloc_space_begin;
    AddSpace(non_moving_space_);
  }
  if (foreground_collector_type_ == kCollectorTypeCC) {
    region_space_ = space::RegionSpace::Create("Region space", capacity_ * 2, request_begin);
    AddSpace(region_space_);
  } else if (IsMovingGc(foreground_collector_type_) &&
      foreground_collector_type_ != kCollectorTypeGSS) {
    bump_pointer_space_ = space::BumpPointerSpace::CreateFromMemMap("Bump pointer space 1",
                                                                    main_mem_map_1.release());
    CHECK(bump_pointer_space_ != nullptr) << "Failed to create bump pointer space";
    AddSpace(bump_pointer_space_);
    temp_space_ = space::BumpPointerSpace::CreateFromMemMap("Bump pointer space 2",
                                                            main_mem_map_2.release());
    CHECK(temp_space_ != nullptr) << "Failed to create bump pointer space";
    AddSpace(temp_space_);
    CHECK(separate_non_moving_space);
  } else {
    CreateMainMallocSpace(main_mem_map_1.release(), initial_size, growth_limit_, capacity_);
    CHECK(main_space_ != nullptr);
    AddSpace(main_space_);
    if (!separate_non_moving_space) {
      non_moving_space_ = main_space_;
      CHECK(!non_moving_space_->CanMoveObjects());
    }
    if (foreground_collector_type_ == kCollectorTypeGSS) {
      CHECK_EQ(foreground_collector_type_, background_collector_type_);
      main_mem_map_2.release();
      bump_pointer_space_ = space::BumpPointerSpace::Create("Bump pointer space 1",
                                                            kGSSBumpPointerSpaceCapacity, nullptr);
      CHECK(bump_pointer_space_ != nullptr);
      AddSpace(bump_pointer_space_);
      temp_space_ = space::BumpPointerSpace::Create("Bump pointer space 2",
                                                    kGSSBumpPointerSpaceCapacity, nullptr);
      CHECK(temp_space_ != nullptr);
      AddSpace(temp_space_);
    } else if (main_mem_map_2.get() != nullptr) {
      const char* name = kUseRosAlloc ? kRosAllocSpaceName[1] : kDlMallocSpaceName[1];
      main_space_backup_.reset(CreateMallocSpaceFromMemMap(main_mem_map_2.release(), initial_size,
                                                           growth_limit_, capacity_, name, true));
      CHECK(main_space_backup_.get() != nullptr);
      AddSpace(main_space_backup_.get());
    }
  }
  CHECK(non_moving_space_ != nullptr);
  CHECK(!non_moving_space_->CanMoveObjects());
  if (large_object_space_type == space::LargeObjectSpaceType::kFreeList) {
    large_object_space_ = space::FreeListSpace::Create("free list large object space", nullptr,
                                                       capacity_);
    CHECK(large_object_space_ != nullptr) << "Failed to create large object space";
  } else if (large_object_space_type == space::LargeObjectSpaceType::kMap) {
    large_object_space_ = space::LargeObjectMapSpace::Create("mem map large object space");
    CHECK(large_object_space_ != nullptr) << "Failed to create large object space";
  } else {
    large_object_threshold_ = std::numeric_limits<size_t>::max();
    large_object_space_ = nullptr;
  }
  if (large_object_space_ != nullptr) {
    AddSpace(large_object_space_);
  }
  CHECK(!continuous_spaces_.empty());
  uint8_t* heap_begin = continuous_spaces_.front()->Begin();
  uint8_t* heap_end = continuous_spaces_.back()->Limit();
  size_t heap_capacity = heap_end - heap_begin;
  if (main_space_backup_.get() != nullptr) {
    RemoveSpace(main_space_backup_.get());
  }
  ATRACE_END();
  ATRACE_BEGIN("Create card table");
  card_table_.reset(accounting::CardTable::Create(heap_begin, heap_capacity));
  CHECK(card_table_.get() != nullptr) << "Failed to create card table";
  ATRACE_END();
  if (foreground_collector_type_ == kCollectorTypeCC && kUseTableLookupReadBarrier) {
    rb_table_.reset(new accounting::ReadBarrierTable());
    DCHECK(rb_table_->IsAllCleared());
  }
  if (GetImageSpace() != nullptr) {
    accounting::ModUnionTable* mod_union_table = new accounting::ModUnionTableToZygoteAllocspace(
        "Image mod-union table", this, GetImageSpace());
    CHECK(mod_union_table != nullptr) << "Failed to create image mod-union table";
    AddModUnionTable(mod_union_table);
  }
  if (collector::SemiSpace::kUseRememberedSet && non_moving_space_ != main_space_) {
    accounting::RememberedSet* non_moving_space_rem_set =
        new accounting::RememberedSet("Non-moving space remembered set", this, non_moving_space_);
    CHECK(non_moving_space_rem_set != nullptr) << "Failed to create non-moving space remembered set";
    AddRememberedSet(non_moving_space_rem_set);
  }
  num_bytes_allocated_.StoreRelaxed(0);
  mark_stack_.reset(accounting::ObjectStack::Create("mark stack", kDefaultMarkStackSize,
                                                    kDefaultMarkStackSize));
  const size_t alloc_stack_capacity = max_allocation_stack_size_ + kAllocationStackReserveSize;
  allocation_stack_.reset(accounting::ObjectStack::Create(
      "allocation stack", max_allocation_stack_size_, alloc_stack_capacity));
  live_stack_.reset(accounting::ObjectStack::Create(
      "live stack", max_allocation_stack_size_, alloc_stack_capacity));
  gc_complete_lock_ = new Mutex("GC complete lock");
  gc_complete_cond_.reset(new ConditionVariable("GC complete condition variable",
                                                *gc_complete_lock_));
  task_processor_.reset(new TaskProcessor());
  pending_task_lock_ = new Mutex("Pending task lock");
  if (ignore_max_footprint_) {
    SetIdealFootprint(std::numeric_limits<size_t>::max());
    concurrent_start_bytes_ = std::numeric_limits<size_t>::max();
  }
  CHECK_NE(max_allowed_footprint_, 0U);
  for (size_t i = 0; i < 2; ++i) {
    const bool concurrent = i != 0;
    if ((MayUseCollector(kCollectorTypeCMS) && concurrent) ||
        (MayUseCollector(kCollectorTypeMS) && !concurrent)) {
      garbage_collectors_.push_back(new collector::MarkSweep(this, concurrent));
      garbage_collectors_.push_back(new collector::PartialMarkSweep(this, concurrent));
      garbage_collectors_.push_back(new collector::StickyMarkSweep(this, concurrent));
    }
  }
  if (kMovingCollector) {
    if (MayUseCollector(kCollectorTypeSS) || MayUseCollector(kCollectorTypeGSS) ||
        MayUseCollector(kCollectorTypeHomogeneousSpaceCompact) ||
        use_homogeneous_space_compaction_for_oom_) {
      const bool generational = foreground_collector_type_ == kCollectorTypeGSS;
      semi_space_collector_ = new collector::SemiSpace(this, generational,
                                                       generational ? "generational" : "");
      garbage_collectors_.push_back(semi_space_collector_);
    }
    if (MayUseCollector(kCollectorTypeCC)) {
      concurrent_copying_collector_ = new collector::ConcurrentCopying(this);
      garbage_collectors_.push_back(concurrent_copying_collector_);
    }
    if (MayUseCollector(kCollectorTypeMC)) {
      mark_compact_collector_ = new collector::MarkCompact(this);
      garbage_collectors_.push_back(mark_compact_collector_);
    }
  }
  if (GetImageSpace() != nullptr && non_moving_space_ != nullptr &&
      (is_zygote || separate_non_moving_space || foreground_collector_type_ == kCollectorTypeGSS)) {
    bool no_gap = MemMap::CheckNoGaps(GetImageSpace()->GetMemMap(),
                                      non_moving_space_->GetMemMap());
    if (!no_gap) {
      PrintFileToLog("/proc/self/maps", LogSeverity::ERROR);
      MemMap::DumpMaps(LOG(ERROR), true);
      LOG(FATAL) << "There's a gap between the image space and the non-moving space";
    }
  }
  instrumentation::Instrumentation* const instrumentation = runtime->GetInstrumentation();
  if (gc_stress_mode_) {
    backtrace_lock_ = new Mutex("GC complete lock");
  }
  if (running_on_valgrind_ || gc_stress_mode_) {
    instrumentation->InstrumentQuickAllocEntryPoints();
  }
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "Heap() exiting";
  }
}
MemMap* Heap::MapAnonymousPreferredAddress(const char* name, uint8_t* request_begin,
                                           size_t capacity, std::string* out_error_str) {
  while (true) {
    MemMap* map = MemMap::MapAnonymous(name, request_begin, capacity,
                                       PROT_READ | PROT_WRITE, true, false, out_error_str);
    if (map != nullptr || request_begin == nullptr) {
      return map;
    }
    request_begin = nullptr;
  }
}
bool Heap::MayUseCollector(CollectorType type) const {
  return foreground_collector_type_ == type || background_collector_type_ == type;
}
space::MallocSpace* Heap::CreateMallocSpaceFromMemMap(MemMap* mem_map, size_t initial_size,
                                                      size_t growth_limit, size_t capacity,
                                                      const char* name, bool can_move_objects) {
  space::MallocSpace* malloc_space = nullptr;
  if (kUseRosAlloc) {
    malloc_space = space::RosAllocSpace::CreateFromMemMap(mem_map, name, kDefaultStartingSize,
                                                          initial_size, growth_limit, capacity,
                                                          low_memory_mode_, can_move_objects);
  } else {
    malloc_space = space::DlMallocSpace::CreateFromMemMap(mem_map, name, kDefaultStartingSize,
                                                          initial_size, growth_limit, capacity,
                                                          can_move_objects);
  }
  if (collector::SemiSpace::kUseRememberedSet) {
    accounting::RememberedSet* rem_set =
        new accounting::RememberedSet(std::string(name) + " remembered set", this, malloc_space);
    CHECK(rem_set != nullptr) << "Failed to create main space remembered set";
    AddRememberedSet(rem_set);
  }
  CHECK(malloc_space != nullptr) << "Failed to create " << name;
  malloc_space->SetFootprintLimit(malloc_space->Capacity());
  return malloc_space;
}
void Heap::CreateMainMallocSpace(MemMap* mem_map, size_t initial_size, size_t growth_limit,
                                 size_t capacity) {
  bool can_move_objects = IsMovingGc(background_collector_type_) !=
      IsMovingGc(foreground_collector_type_) || use_homogeneous_space_compaction_for_oom_;
  if (kCompactZygote && Runtime::Current()->IsZygote() && !can_move_objects) {
    can_move_objects = !HasZygoteSpace() && foreground_collector_type_ != kCollectorTypeGSS;
  }
  if (collector::SemiSpace::kUseRememberedSet && main_space_ != nullptr) {
    RemoveRememberedSet(main_space_);
  }
  const char* name = kUseRosAlloc ? kRosAllocSpaceName[0] : kDlMallocSpaceName[0];
  main_space_ = CreateMallocSpaceFromMemMap(mem_map, initial_size, growth_limit, capacity, name,
                                            can_move_objects);
  SetSpaceAsDefault(main_space_);
  VLOG(heap) << "Created main space " << main_space_;
}
void Heap::ChangeAllocator(AllocatorType allocator) {
  if (current_allocator_ != allocator) {
    CHECK_NE(allocator, kAllocatorTypeLOS);
    CHECK_NE(allocator, kAllocatorTypeNonMoving);
    current_allocator_ = allocator;
    MutexLock mu(nullptr, *Locks::runtime_shutdown_lock_);
    SetQuickAllocEntryPointsAllocator(current_allocator_);
    Runtime::Current()->GetInstrumentation()->ResetQuickAllocEntryPoints();
  }
}
void Heap::DisableMovingGc() {
  if (IsMovingGc(foreground_collector_type_)) {
    foreground_collector_type_ = kCollectorTypeCMS;
  }
  if (IsMovingGc(background_collector_type_)) {
    background_collector_type_ = foreground_collector_type_;
  }
  TransitionCollector(foreground_collector_type_);
  ThreadList* tl = Runtime::Current()->GetThreadList();
  Thread* self = Thread::Current();
  ScopedThreadStateChange tsc(self, kSuspended);
  tl->SuspendAll(__FUNCTION__);
  if (!IsMovingGc(collector_type_) && non_moving_space_ != main_space_) {
    CHECK(main_space_ != nullptr);
    {
      WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
      FlushAllocStack();
    }
    main_space_->DisableMovingObjects();
    non_moving_space_ = main_space_;
    CHECK(!non_moving_space_->CanMoveObjects());
  }
  tl->ResumeAll();
}
std::string Heap::SafeGetClassDescriptor(mirror::Class* klass) {
  if (!IsValidContinuousSpaceObjectAddress(klass)) {
    return StringPrintf("<non heap address klass %p>", klass);
  }
  mirror::Class* component_type = klass->GetComponentType<kVerifyNone>();
  if (IsValidContinuousSpaceObjectAddress(component_type) && klass->IsArrayClass<kVerifyNone>()) {
    std::string result("[");
    result += SafeGetClassDescriptor(component_type);
    return result;
  } else if (UNLIKELY(klass->IsPrimitive<kVerifyNone>())) {
    return Primitive::Descriptor(klass->GetPrimitiveType<kVerifyNone>());
  } else if (UNLIKELY(klass->IsProxyClass<kVerifyNone>())) {
    return Runtime::Current()->GetClassLinker()->GetDescriptorForProxy(klass);
  } else {
    mirror::DexCache* dex_cache = klass->GetDexCache<kVerifyNone>();
    if (!IsValidContinuousSpaceObjectAddress(dex_cache)) {
      return StringPrintf("<non heap address dex_cache %p>", dex_cache);
    }
    const DexFile* dex_file = dex_cache->GetDexFile();
    uint16_t class_def_idx = klass->GetDexClassDefIndex();
    if (class_def_idx == DexFile::kDexNoIndex16) {
      return "<class def not found>";
    }
    const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_def_idx);
    const DexFile::TypeId& type_id = dex_file->GetTypeId(class_def.class_idx_);
    return dex_file->GetTypeDescriptor(type_id);
  }
}
std::string Heap::SafePrettyTypeOf(mirror::Object* obj) {
  if (obj == nullptr) {
    return "null";
  }
  mirror::Class* klass = obj->GetClass<kVerifyNone>();
  if (klass == nullptr) {
    return "(class=null)";
  }
  std::string result(SafeGetClassDescriptor(klass));
  if (obj->IsClass()) {
    result += "<" + SafeGetClassDescriptor(obj->AsClass<kVerifyNone>()) + ">";
  }
  return result;
}
void Heap::DumpObject(std::ostream& stream, mirror::Object* obj) {
  if (obj == nullptr) {
    stream << "(obj=null)";
    return;
  }
  if (IsAligned<kObjectAlignment>(obj)) {
    space::Space* space = nullptr;
    for (const auto& cur_space : continuous_spaces_) {
      if (cur_space->HasAddress(obj)) {
        space = cur_space;
        break;
      }
    }
    for (const auto& con_space : continuous_spaces_) {
      mprotect(con_space->Begin(), con_space->Capacity(), PROT_READ | PROT_WRITE);
    }
    stream << "Object " << obj;
    if (space != nullptr) {
      stream << " in space " << *space;
    }
    mirror::Class* klass = obj->GetClass<kVerifyNone>();
    stream << "\nclass=" << klass;
    if (klass != nullptr) {
      stream << " type= " << SafePrettyTypeOf(obj);
    }
    mprotect(AlignDown(obj, kPageSize), kPageSize, PROT_NONE);
  }
}
bool Heap::IsCompilingBoot() const {
  if (!Runtime::Current()->IsAotCompiler()) {
    return false;
  }
  for (const auto& space : continuous_spaces_) {
    if (space->IsImageSpace() || space->IsZygoteSpace()) {
      return false;
    }
  }
  return true;
}
bool Heap::HasImageSpace() const {
  for (const auto& space : continuous_spaces_) {
    if (space->IsImageSpace()) {
      return true;
    }
  }
  return false;
}
void Heap::IncrementDisableMovingGC(Thread* self) {
  ScopedThreadStateChange tsc(self, kWaitingForGcToComplete);
  MutexLock mu(self, *gc_complete_lock_);
  ++disable_moving_gc_count_;
  if (IsMovingGc(collector_type_running_)) {
    WaitForGcToCompleteLocked(kGcCauseDisableMovingGc, self);
  }
}
void Heap::DecrementDisableMovingGC(Thread* self) {
  MutexLock mu(self, *gc_complete_lock_);
  CHECK_GE(disable_moving_gc_count_, 0U);
  --disable_moving_gc_count_;
}
void Heap::UpdateProcessState(ProcessState process_state) {
  if (process_state_ != process_state) {
    process_state_ = process_state;
    for (size_t i = 1; i <= kCollectorTransitionStressIterations; ++i) {
      TransitionCollector((((i & 1) == 1) == (process_state_ == kProcessStateJankPerceptible))
                          ? foreground_collector_type_ : background_collector_type_);
      usleep(kCollectorTransitionStressWait);
    }
    if (process_state_ == kProcessStateJankPerceptible) {
      RequestCollectorTransition(foreground_collector_type_, 0);
    } else {
      RequestCollectorTransition(background_collector_type_,
                                 kIsDebugBuild ? 0 : kCollectorTransitionWait);
    }
  }
}
void Heap::CreateThreadPool() {
  const size_t num_threads = std::max(parallel_gc_threads_, conc_gc_threads_);
  if (num_threads != 0) {
    thread_pool_.reset(new ThreadPool("Heap thread pool", num_threads));
  }
}
void Heap::VisitObjects(ObjectCallback callback, void* arg) {
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertSharedHeld(self);
  DCHECK(!Locks::mutator_lock_->IsExclusiveHeld(self)) << "Call VisitObjectsPaused() instead";
  if (IsGcConcurrentAndMoving()) {
    IncrementDisableMovingGC(self);
    self->TransitionFromRunnableToSuspended(kWaitingForVisitObjects);
    ThreadList* tl = Runtime::Current()->GetThreadList();
    tl->SuspendAll(__FUNCTION__);
    VisitObjectsInternalRegionSpace(callback, arg);
    VisitObjectsInternal(callback, arg);
    tl->ResumeAll();
    self->TransitionFromSuspendedToRunnable();
    DecrementDisableMovingGC(self);
  } else {
    ScopedAssertNoThreadSuspension ants(self, "Visiting objects");
    DCHECK(region_space_ == nullptr);
    VisitObjectsInternal(callback, arg);
  }
}
void Heap::VisitObjectsPaused(ObjectCallback callback, void* arg) {
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertExclusiveHeld(self);
  VisitObjectsInternalRegionSpace(callback, arg);
  VisitObjectsInternal(callback, arg);
}
void Heap::VisitObjectsInternalRegionSpace(ObjectCallback callback, void* arg) {
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertExclusiveHeld(self);
  if (region_space_ != nullptr) {
    DCHECK(IsGcConcurrentAndMoving());
    if (!zygote_creation_lock_.IsExclusiveHeld(self)) {
      DCHECK(IsMovingGCDisabled(self));
    }
    region_space_->Walk(callback, arg);
  }
}
void Heap::VisitObjectsInternal(ObjectCallback callback, void* arg) {
  if (bump_pointer_space_ != nullptr) {
    bump_pointer_space_->Walk(callback, arg);
  }
  for (auto* it = allocation_stack_->Begin(), *end = allocation_stack_->End(); it < end; ++it) {
    mirror::Object* const obj = it->AsMirrorPtr();
    if (obj != nullptr && obj->GetClass() != nullptr) {
      callback(obj, arg);
    }
  }
  {
    ReaderMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
    GetLiveBitmap()->Walk(callback, arg);
  }
}
void Heap::MarkAllocStackAsLive(accounting::ObjectStack* stack) {
  space::ContinuousSpace* space1 = main_space_ != nullptr ? main_space_ : non_moving_space_;
  space::ContinuousSpace* space2 = non_moving_space_;
  CHECK(space1 != nullptr);
  CHECK(space2 != nullptr);
  MarkAllocStack(space1->GetLiveBitmap(), space2->GetLiveBitmap(),
                 (large_object_space_ != nullptr ? large_object_space_->GetLiveBitmap() : nullptr),
                 stack);
}
void Heap::DeleteThreadPool() {
  thread_pool_.reset(nullptr);
}
void Heap::AddSpace(space::Space* space) {
  CHECK(space != nullptr);
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  if (space->IsContinuousSpace()) {
    DCHECK(!space->IsDiscontinuousSpace());
    space::ContinuousSpace* continuous_space = space->AsContinuousSpace();
    accounting::ContinuousSpaceBitmap* live_bitmap = continuous_space->GetLiveBitmap();
    accounting::ContinuousSpaceBitmap* mark_bitmap = continuous_space->GetMarkBitmap();
    if (live_bitmap != nullptr) {
      CHECK(mark_bitmap != nullptr);
      live_bitmap_->AddContinuousSpaceBitmap(live_bitmap);
      mark_bitmap_->AddContinuousSpaceBitmap(mark_bitmap);
    }
    continuous_spaces_.push_back(continuous_space);
    std::sort(continuous_spaces_.begin(), continuous_spaces_.end(),
              [](const space::ContinuousSpace* a, const space::ContinuousSpace* b) {
      return a->Begin() < b->Begin();
    });
  } else {
    CHECK(space->IsDiscontinuousSpace());
    space::DiscontinuousSpace* discontinuous_space = space->AsDiscontinuousSpace();
    live_bitmap_->AddLargeObjectBitmap(discontinuous_space->GetLiveBitmap());
    mark_bitmap_->AddLargeObjectBitmap(discontinuous_space->GetMarkBitmap());
    discontinuous_spaces_.push_back(discontinuous_space);
  }
  if (space->IsAllocSpace()) {
    alloc_spaces_.push_back(space->AsAllocSpace());
  }
}
void Heap::SetSpaceAsDefault(space::ContinuousSpace* continuous_space) {
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  if (continuous_space->IsDlMallocSpace()) {
    dlmalloc_space_ = continuous_space->AsDlMallocSpace();
  } else if (continuous_space->IsRosAllocSpace()) {
    rosalloc_space_ = continuous_space->AsRosAllocSpace();
  }
}
void Heap::RemoveSpace(space::Space* space) {
  DCHECK(space != nullptr);
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  if (space->IsContinuousSpace()) {
    DCHECK(!space->IsDiscontinuousSpace());
    space::ContinuousSpace* continuous_space = space->AsContinuousSpace();
    accounting::ContinuousSpaceBitmap* live_bitmap = continuous_space->GetLiveBitmap();
    accounting::ContinuousSpaceBitmap* mark_bitmap = continuous_space->GetMarkBitmap();
    if (live_bitmap != nullptr) {
      DCHECK(mark_bitmap != nullptr);
      live_bitmap_->RemoveContinuousSpaceBitmap(live_bitmap);
      mark_bitmap_->RemoveContinuousSpaceBitmap(mark_bitmap);
    }
    auto it = std::find(continuous_spaces_.begin(), continuous_spaces_.end(), continuous_space);
    DCHECK(it != continuous_spaces_.end());
    continuous_spaces_.erase(it);
  } else {
    DCHECK(space->IsDiscontinuousSpace());
    space::DiscontinuousSpace* discontinuous_space = space->AsDiscontinuousSpace();
    live_bitmap_->RemoveLargeObjectBitmap(discontinuous_space->GetLiveBitmap());
    mark_bitmap_->RemoveLargeObjectBitmap(discontinuous_space->GetMarkBitmap());
    auto it = std::find(discontinuous_spaces_.begin(), discontinuous_spaces_.end(),
                        discontinuous_space);
    DCHECK(it != discontinuous_spaces_.end());
    discontinuous_spaces_.erase(it);
  }
  if (space->IsAllocSpace()) {
    auto it = std::find(alloc_spaces_.begin(), alloc_spaces_.end(), space->AsAllocSpace());
    DCHECK(it != alloc_spaces_.end());
    alloc_spaces_.erase(it);
  }
}
void Heap::DumpGcPerformanceInfo(std::ostream& os) {
  os << "Dumping cumulative Gc timings\n";
  uint64_t total_duration = 0;
  uint64_t total_paused_time = 0;
  for (auto& collector : garbage_collectors_) {
    total_duration += collector->GetCumulativeTimings().GetTotalNs();
    total_paused_time += collector->GetTotalPausedTimeNs();
    collector->DumpPerformanceInfo(os);
  }
  uint64_t allocation_time =
      static_cast<uint64_t>(total_allocation_time_.LoadRelaxed()) * kTimeAdjust;
  if (total_duration != 0) {
    const double total_seconds = static_cast<double>(total_duration / 1000) / 1000000.0;
    os << "Total time spent in GC: " << PrettyDuration(total_duration) << "\n";
    os << "Mean GC size throughput: "
       << PrettySize(GetBytesFreedEver() / total_seconds) << "/s\n";
    os << "Mean GC object throughput: "
       << (GetObjectsFreedEver() / total_seconds) << " objects/s\n";
  }
  uint64_t total_objects_allocated = GetObjectsAllocatedEver();
  os << "Total number of allocations " << total_objects_allocated << "\n";
  os << "Total bytes allocated " << PrettySize(GetBytesAllocatedEver()) << "\n";
  os << "Total bytes freed " << PrettySize(GetBytesFreedEver()) << "\n";
  os << "Free memory " << PrettySize(GetFreeMemory()) << "\n";
  os << "Free memory until GC " << PrettySize(GetFreeMemoryUntilGC()) << "\n";
  os << "Free memory until OOME " << PrettySize(GetFreeMemoryUntilOOME()) << "\n";
  os << "Total memory " << PrettySize(GetTotalMemory()) << "\n";
  os << "Max memory " << PrettySize(GetMaxMemory()) << "\n";
  if (kMeasureAllocationTime) {
    os << "Total time spent allocating: " << PrettyDuration(allocation_time) << "\n";
    os << "Mean allocation time: " << PrettyDuration(allocation_time / total_objects_allocated)
       << "\n";
  }
  if (HasZygoteSpace()) {
    os << "Zygote space size " << PrettySize(zygote_space_->Size()) << "\n";
  }
  os << "Total mutator paused time: " << PrettyDuration(total_paused_time) << "\n";
  os << "Total time waiting for GC to complete: " << PrettyDuration(total_wait_time_) << "\n";
  os << "Total GC count: " << GetGcCount() << "\n";
  os << "Total GC time: " << PrettyDuration(GetGcTime()) << "\n";
  os << "Total blocking GC count: " << GetBlockingGcCount() << "\n";
  os << "Total blocking GC time: " << PrettyDuration(GetBlockingGcTime()) << "\n";
  {
    MutexLock mu(Thread::Current(), *gc_complete_lock_);
    if (gc_count_rate_histogram_.SampleSize() > 0U) {
      os << "Histogram of GC count per " << NsToMs(kGcCountRateHistogramWindowDuration) << " ms: ";
      gc_count_rate_histogram_.DumpBins(os);
      os << "\n";
    }
    if (blocking_gc_count_rate_histogram_.SampleSize() > 0U) {
      os << "Histogram of blocking GC count per "
         << NsToMs(kGcCountRateHistogramWindowDuration) << " ms: ";
      blocking_gc_count_rate_histogram_.DumpBins(os);
      os << "\n";
    }
  }
  BaseMutex::DumpAll(os);
}
void Heap::ResetGcPerformanceInfo() {
  for (auto& collector : garbage_collectors_) {
    collector->ResetMeasurements();
  }
  total_allocation_time_.StoreRelaxed(0);
  total_bytes_freed_ever_ = 0;
  total_objects_freed_ever_ = 0;
  total_wait_time_ = 0;
  blocking_gc_count_ = 0;
  blocking_gc_time_ = 0;
  gc_count_last_window_ = 0;
  blocking_gc_count_last_window_ = 0;
  last_update_time_gc_count_rate_histograms_ =
      (NanoTime() / kGcCountRateHistogramWindowDuration) * kGcCountRateHistogramWindowDuration;
  {
    MutexLock mu(Thread::Current(), *gc_complete_lock_);
    gc_count_rate_histogram_.Reset();
    blocking_gc_count_rate_histogram_.Reset();
  }
}
uint64_t Heap::GetGcCount() const {
  uint64_t gc_count = 0U;
  for (auto& collector : garbage_collectors_) {
    gc_count += collector->GetCumulativeTimings().GetIterations();
  }
  return gc_count;
}
uint64_t Heap::GetGcTime() const {
  uint64_t gc_time = 0U;
  for (auto& collector : garbage_collectors_) {
    gc_time += collector->GetCumulativeTimings().GetTotalNs();
  }
  return gc_time;
}
uint64_t Heap::GetBlockingGcCount() const {
  return blocking_gc_count_;
}
uint64_t Heap::GetBlockingGcTime() const {
  return blocking_gc_time_;
}
void Heap::DumpGcCountRateHistogram(std::ostream& os) const {
  MutexLock mu(Thread::Current(), *gc_complete_lock_);
  if (gc_count_rate_histogram_.SampleSize() > 0U) {
    gc_count_rate_histogram_.DumpBins(os);
  }
}
void Heap::DumpBlockingGcCountRateHistogram(std::ostream& os) const {
  MutexLock mu(Thread::Current(), *gc_complete_lock_);
  if (blocking_gc_count_rate_histogram_.SampleSize() > 0U) {
    blocking_gc_count_rate_histogram_.DumpBins(os);
  }
}
Heap::~Heap() {
  VLOG(heap) << "Starting ~Heap()";
  STLDeleteElements(&garbage_collectors_);
  allocation_stack_->Reset();
  allocation_records_.reset();
  live_stack_->Reset();
  STLDeleteValues(&mod_union_tables_);
  STLDeleteValues(&remembered_sets_);
  STLDeleteElements(&continuous_spaces_);
  STLDeleteElements(&discontinuous_spaces_);
  delete gc_complete_lock_;
  delete pending_task_lock_;
  delete backtrace_lock_;
  if (unique_backtrace_count_.LoadRelaxed() != 0 || seen_backtrace_count_.LoadRelaxed() != 0) {
    LOG(INFO) << "gc stress unique=" << unique_backtrace_count_.LoadRelaxed()
        << " total=" << seen_backtrace_count_.LoadRelaxed() +
            unique_backtrace_count_.LoadRelaxed();
  }
  VLOG(heap) << "Finished ~Heap()";
}
space::ContinuousSpace* Heap::FindContinuousSpaceFromObject(const mirror::Object* obj,
                                                            bool fail_ok) const {
  for (const auto& space : continuous_spaces_) {
    if (space->Contains(obj)) {
      return space;
    }
  }
  if (!fail_ok) {
    LOG(FATAL) << "object " << reinterpret_cast<const void*>(obj) << " not inside any spaces!";
  }
  return nullptr;
}
space::DiscontinuousSpace* Heap::FindDiscontinuousSpaceFromObject(const mirror::Object* obj,
                                                                  bool fail_ok) const {
  for (const auto& space : discontinuous_spaces_) {
    if (space->Contains(obj)) {
      return space;
    }
  }
  if (!fail_ok) {
    LOG(FATAL) << "object " << reinterpret_cast<const void*>(obj) << " not inside any spaces!";
  }
  return nullptr;
}
space::Space* Heap::FindSpaceFromObject(const mirror::Object* obj, bool fail_ok) const {
  space::Space* result = FindContinuousSpaceFromObject(obj, true);
  if (result != nullptr) {
    return result;
  }
  return FindDiscontinuousSpaceFromObject(obj, fail_ok);
}
space::ImageSpace* Heap::GetImageSpace() const {
  for (const auto& space : continuous_spaces_) {
    if (space->IsImageSpace()) {
      return space->AsImageSpace();
    }
  }
  return nullptr;
}
void Heap::ThrowOutOfMemoryError(Thread* self, size_t byte_count, AllocatorType allocator_type) {
  std::ostringstream oss;
  size_t total_bytes_free = GetFreeMemory();
  oss << "Failed to allocate a " << byte_count << " byte allocation with " << total_bytes_free
      << " free bytes and " << PrettySize(GetFreeMemoryUntilOOME()) << " until OOM";
  if (total_bytes_free >= byte_count) {
    space::AllocSpace* space = nullptr;
    if (allocator_type == kAllocatorTypeNonMoving) {
      space = non_moving_space_;
    } else if (allocator_type == kAllocatorTypeRosAlloc ||
               allocator_type == kAllocatorTypeDlMalloc) {
      space = main_space_;
    } else if (allocator_type == kAllocatorTypeBumpPointer ||
               allocator_type == kAllocatorTypeTLAB) {
      space = bump_pointer_space_;
    } else if (allocator_type == kAllocatorTypeRegion ||
               allocator_type == kAllocatorTypeRegionTLAB) {
      space = region_space_;
    }
    if (space != nullptr) {
      space->LogFragmentationAllocFailure(oss, byte_count);
    }
  }
  self->ThrowOutOfMemoryError(oss.str().c_str());
}
void Heap::DoPendingCollectorTransition() {
  CollectorType desired_collector_type = desired_collector_type_;
  if (desired_collector_type == kCollectorTypeHomogeneousSpaceCompact) {
    if (!CareAboutPauseTimes()) {
      PerformHomogeneousSpaceCompact();
    } else {
      VLOG(gc) << "Homogeneous compaction ignored due to jank perceptible process state";
    }
  } else {
    TransitionCollector(desired_collector_type);
  }
}
void Heap::Trim(Thread* self) {
  if (!CareAboutPauseTimes()) {
    ATRACE_BEGIN("Deflating monitors");
    Runtime* runtime = Runtime::Current();
    runtime->GetThreadList()->SuspendAll(__FUNCTION__);
    uint64_t start_time = NanoTime();
    size_t count = runtime->GetMonitorList()->DeflateMonitors();
    VLOG(heap) << "Deflating " << count << " monitors took "
        << PrettyDuration(NanoTime() - start_time);
    runtime->GetThreadList()->ResumeAll();
    ATRACE_END();
  }
  TrimIndirectReferenceTables(self);
  TrimSpaces(self);
}
class TrimIndirectReferenceTableClosure : public Closure {
 public:
  explicit TrimIndirectReferenceTableClosure(Barrier* barrier) : barrier_(barrier) {
  }
  virtual void Run(Thread* thread) OVERRIDE NO_THREAD_SAFETY_ANALYSIS {
    ATRACE_BEGIN("Trimming reference table");
    thread->GetJniEnv()->locals.Trim();
    ATRACE_END();
    if (thread->GetState() == kRunnable) {
      barrier_->Pass(Thread::Current());
    }
  }
 private:
  Barrier* const barrier_;
};
void Heap::TrimIndirectReferenceTables(Thread* self) {
  ScopedObjectAccess soa(self);
  ATRACE_BEGIN(__FUNCTION__);
  JavaVMExt* vm = soa.Vm();
  vm->TrimGlobals();
  Barrier barrier(0);
  TrimIndirectReferenceTableClosure closure(&barrier);
  ScopedThreadStateChange tsc(self, kWaitingForCheckPointsToRun);
  size_t barrier_count = Runtime::Current()->GetThreadList()->RunCheckpoint(&closure);
  if (barrier_count != 0) {
    barrier.Increment(self, barrier_count);
  }
  ATRACE_END();
}
void Heap::TrimSpaces(Thread* self) {
  {
    ScopedThreadStateChange tsc(self, kWaitingForGcToComplete);
    MutexLock mu(self, *gc_complete_lock_);
    WaitForGcToCompleteLocked(kGcCauseTrim, self);
    collector_type_running_ = kCollectorTypeHeapTrim;
  }
  ATRACE_BEGIN(__FUNCTION__);
  const uint64_t start_ns = NanoTime();
  uint64_t total_alloc_space_allocated = 0;
  uint64_t total_alloc_space_size = 0;
  uint64_t managed_reclaimed = 0;
  for (const auto& space : continuous_spaces_) {
    if (space->IsMallocSpace()) {
      gc::space::MallocSpace* malloc_space = space->AsMallocSpace();
      if (malloc_space->IsRosAllocSpace() || !CareAboutPauseTimes()) {
        managed_reclaimed += malloc_space->Trim();
      }
      total_alloc_space_size += malloc_space->Size();
    }
  }
  total_alloc_space_allocated = GetBytesAllocated();
  if (large_object_space_ != nullptr) {
    total_alloc_space_allocated -= large_object_space_->GetBytesAllocated();
  }
  if (bump_pointer_space_ != nullptr) {
    total_alloc_space_allocated -= bump_pointer_space_->Size();
  }
  if (region_space_ != nullptr) {
    total_alloc_space_allocated -= region_space_->GetBytesAllocated();
  }
  const float managed_utilization = static_cast<float>(total_alloc_space_allocated) /
      static_cast<float>(total_alloc_space_size);
  uint64_t gc_heap_end_ns = NanoTime();
  FinishGC(self, collector::kGcTypeNone);
  size_t native_reclaimed = 0;
#ifdef HAVE_ANDROID_OS
  if (!CareAboutPauseTimes()) {
#if defined(USE_DLMALLOC)
    dlmalloc_trim(0);
    dlmalloc_inspect_all(DlmallocMadviseCallback, &native_reclaimed);
#elif defined(USE_JEMALLOC)
#else
    UNIMPLEMENTED(WARNING) << "Add trimming support";
#endif
  }
#endif
  uint64_t end_ns = NanoTime();
  VLOG(heap) << "Heap trim of managed (duration=" << PrettyDuration(gc_heap_end_ns - start_ns)
      << ", advised=" << PrettySize(managed_reclaimed) << ") and native (duration="
      << PrettyDuration(end_ns - gc_heap_end_ns) << ", advised=" << PrettySize(native_reclaimed)
      << ") heaps. Managed heap utilization of " << static_cast<int>(100 * managed_utilization)
      << "%.";
  ATRACE_END();
}
bool Heap::IsValidObjectAddress(const mirror::Object* obj) const {
  if (obj == nullptr) {
    return true;
  }
  return IsAligned<kObjectAlignment>(obj) && FindSpaceFromObject(obj, true) != nullptr;
}
bool Heap::IsNonDiscontinuousSpaceHeapAddress(const mirror::Object* obj) const {
  return FindContinuousSpaceFromObject(obj, true) != nullptr;
}
bool Heap::IsValidContinuousSpaceObjectAddress(const mirror::Object* obj) const {
  if (obj == nullptr || !IsAligned<kObjectAlignment>(obj)) {
    return false;
  }
  for (const auto& space : continuous_spaces_) {
    if (space->HasAddress(obj)) {
      return true;
    }
  }
  return false;
}
bool Heap::IsLiveObjectLocked(mirror::Object* obj, bool search_allocation_stack,
                              bool search_live_stack, bool sorted) {
  if (UNLIKELY(!IsAligned<kObjectAlignment>(obj))) {
    return false;
  }
  if (bump_pointer_space_ != nullptr && bump_pointer_space_->HasAddress(obj)) {
    mirror::Class* klass = obj->GetClass<kVerifyNone>();
    if (obj == klass) {
      return true;
    }
    return VerifyClassClass(klass) && IsLiveObjectLocked(klass);
  } else if (temp_space_ != nullptr && temp_space_->HasAddress(obj)) {
    return temp_space_->Contains(obj);
  }
  if (region_space_ != nullptr && region_space_->HasAddress(obj)) {
    return true;
  }
  space::ContinuousSpace* c_space = FindContinuousSpaceFromObject(obj, true);
  space::DiscontinuousSpace* d_space = nullptr;
  if (c_space != nullptr) {
    if (c_space->GetLiveBitmap()->Test(obj)) {
      return true;
    }
  } else {
    d_space = FindDiscontinuousSpaceFromObject(obj, true);
    if (d_space != nullptr) {
      if (d_space->GetLiveBitmap()->Test(obj)) {
        return true;
      }
    }
  }
  for (size_t i = 0; i < (sorted ? 1 : 5); ++i) {
    if (i > 0) {
      NanoSleep(MsToNs(10));
    }
    if (search_allocation_stack) {
      if (sorted) {
        if (allocation_stack_->ContainsSorted(obj)) {
          return true;
        }
      } else if (allocation_stack_->Contains(obj)) {
        return true;
      }
    }
    if (search_live_stack) {
      if (sorted) {
        if (live_stack_->ContainsSorted(obj)) {
          return true;
        }
      } else if (live_stack_->Contains(obj)) {
        return true;
      }
    }
  }
  if (c_space != nullptr) {
    if (c_space->GetLiveBitmap()->Test(obj)) {
      return true;
    }
  } else {
    d_space = FindDiscontinuousSpaceFromObject(obj, true);
    if (d_space != nullptr && d_space->GetLiveBitmap()->Test(obj)) {
      return true;
    }
  }
  return false;
}
std::string Heap::DumpSpaces() const {
  std::ostringstream oss;
  DumpSpaces(oss);
  return oss.str();
}
void Heap::DumpSpaces(std::ostream& stream) const {
  for (const auto& space : continuous_spaces_) {
    accounting::ContinuousSpaceBitmap* live_bitmap = space->GetLiveBitmap();
    accounting::ContinuousSpaceBitmap* mark_bitmap = space->GetMarkBitmap();
    stream << space << " " << *space << "\n";
    if (live_bitmap != nullptr) {
      stream << live_bitmap << " " << *live_bitmap << "\n";
    }
    if (mark_bitmap != nullptr) {
      stream << mark_bitmap << " " << *mark_bitmap << "\n";
    }
  }
  for (const auto& space : discontinuous_spaces_) {
    stream << space << " " << *space << "\n";
  }
}
void Heap::VerifyObjectBody(mirror::Object* obj) {
  if (verify_object_mode_ == kVerifyObjectModeDisabled) {
    return;
  }
  if (UNLIKELY(static_cast<size_t>(num_bytes_allocated_.LoadRelaxed()) < 10 * KB)) {
    return;
  }
  CHECK(IsAligned<kObjectAlignment>(obj)) << "Object isn't aligned: " << obj;
  mirror::Class* c = obj->GetFieldObject<mirror::Class, kVerifyNone>(mirror::Object::ClassOffset());
  CHECK(c != nullptr) << "Null class in object " << obj;
  CHECK(IsAligned<kObjectAlignment>(c)) << "Class " << c << " not aligned in object " << obj;
  CHECK(VerifyClassClass(c));
  if (verify_object_mode_ > kVerifyObjectModeFast) {
    CHECK(IsLiveObjectLocked(obj)) << "Object is dead " << obj << "\n" << DumpSpaces();
  }
}
void Heap::VerificationCallback(mirror::Object* obj, void* arg) {
  reinterpret_cast<Heap*>(arg)->VerifyObjectBody(obj);
}
void Heap::VerifyHeap() {
  ReaderMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  GetLiveBitmap()->Walk(Heap::VerificationCallback, this);
}
void Heap::RecordFree(uint64_t freed_objects, int64_t freed_bytes) {
  DCHECK_LE(freed_bytes, static_cast<int64_t>(num_bytes_allocated_.LoadRelaxed()));
  num_bytes_allocated_.FetchAndSubSequentiallyConsistent(static_cast<ssize_t>(freed_bytes));
  if (Runtime::Current()->HasStatsEnabled()) {
    RuntimeStats* thread_stats = Thread::Current()->GetStats();
    thread_stats->freed_objects += freed_objects;
    thread_stats->freed_bytes += freed_bytes;
    RuntimeStats* global_stats = Runtime::Current()->GetStats();
    global_stats->freed_objects += freed_objects;
    global_stats->freed_bytes += freed_bytes;
  }
}
void Heap::RecordFreeRevoke() {
  size_t bytes_freed = num_bytes_freed_revoke_.LoadSequentiallyConsistent();
  CHECK_GE(num_bytes_freed_revoke_.FetchAndSubSequentiallyConsistent(bytes_freed),
           bytes_freed) << "num_bytes_freed_revoke_ underflow";
  CHECK_GE(num_bytes_allocated_.FetchAndSubSequentiallyConsistent(bytes_freed),
           bytes_freed) << "num_bytes_allocated_ underflow";
  GetCurrentGcIteration()->SetFreedRevoke(bytes_freed);
}
space::RosAllocSpace* Heap::GetRosAllocSpace(gc::allocator::RosAlloc* rosalloc) const {
  for (const auto& space : continuous_spaces_) {
    if (space->AsContinuousSpace()->IsRosAllocSpace()) {
      if (space->AsContinuousSpace()->AsRosAllocSpace()->GetRosAlloc() == rosalloc) {
        return space->AsContinuousSpace()->AsRosAllocSpace();
      }
    }
  }
  return nullptr;
}
mirror::Object* Heap::AllocateInternalWithGc(Thread* self, AllocatorType allocator,
                                             size_t alloc_size, size_t* bytes_allocated,
                                             size_t* usable_size,
                                             size_t* bytes_tl_bulk_allocated,
                                             mirror::Class** klass) {
  bool was_default_allocator = allocator == GetCurrentAllocator();
  self->AssertNoPendingException();
  DCHECK(klass != nullptr);
  StackHandleScope<1> hs(self);
  HandleWrapper<mirror::Class> h(hs.NewHandleWrapper(klass));
  klass = nullptr;
  collector::GcType last_gc = WaitForGcToComplete(kGcCauseForAlloc, self);
  if (last_gc != collector::kGcTypeNone) {
    if (was_default_allocator && allocator != GetCurrentAllocator()) {
      return nullptr;
    }
    mirror::Object* ptr = TryToAllocate<true, false>(self, allocator, alloc_size, bytes_allocated,
                                                     usable_size, bytes_tl_bulk_allocated);
    if (ptr != nullptr) {
      return ptr;
    }
  }
  collector::GcType tried_type = next_gc_type_;
  const bool gc_ran =
      CollectGarbageInternal(tried_type, kGcCauseForAlloc, false) != collector::kGcTypeNone;
  if (was_default_allocator && allocator != GetCurrentAllocator()) {
    return nullptr;
  }
  if (gc_ran) {
    mirror::Object* ptr = TryToAllocate<true, false>(self, allocator, alloc_size, bytes_allocated,
                                                     usable_size, bytes_tl_bulk_allocated);
    if (ptr != nullptr) {
      return ptr;
    }
  }
  for (collector::GcType gc_type : gc_plan_) {
    if (gc_type == tried_type) {
      continue;
    }
    const bool plan_gc_ran =
        CollectGarbageInternal(gc_type, kGcCauseForAlloc, false) != collector::kGcTypeNone;
    if (was_default_allocator && allocator != GetCurrentAllocator()) {
      return nullptr;
    }
    if (plan_gc_ran) {
      mirror::Object* ptr = TryToAllocate<true, false>(self, allocator, alloc_size, bytes_allocated,
                                                       usable_size, bytes_tl_bulk_allocated);
      if (ptr != nullptr) {
        return ptr;
      }
    }
  }
  mirror::Object* ptr = TryToAllocate<true, true>(self, allocator, alloc_size, bytes_allocated,
                                                  usable_size, bytes_tl_bulk_allocated);
  if (ptr != nullptr) {
    return ptr;
  }
  VLOG(gc) << "Forcing collection of SoftReferences for " << PrettySize(alloc_size)
           << " allocation";
  DCHECK(!gc_plan_.empty());
  CollectGarbageInternal(gc_plan_.back(), kGcCauseForAlloc, true);
  if (was_default_allocator && allocator != GetCurrentAllocator()) {
    return nullptr;
  }
  ptr = TryToAllocate<true, true>(self, allocator, alloc_size, bytes_allocated, usable_size,
                                  bytes_tl_bulk_allocated);
  if (ptr == nullptr) {
    const uint64_t current_time = NanoTime();
    switch (allocator) {
      case kAllocatorTypeRosAlloc:
      case kAllocatorTypeDlMalloc: {
        if (use_homogeneous_space_compaction_for_oom_ &&
            current_time - last_time_homogeneous_space_compaction_by_oom_ >
            min_interval_homogeneous_space_compaction_by_oom_) {
          last_time_homogeneous_space_compaction_by_oom_ = current_time;
          HomogeneousSpaceCompactResult result = PerformHomogeneousSpaceCompact();
          switch (result) {
            case HomogeneousSpaceCompactResult::kSuccess:
              ptr = TryToAllocate<true, true>(self, allocator, alloc_size, bytes_allocated,
                                              usable_size, bytes_tl_bulk_allocated);
              if (ptr != nullptr) {
                count_delayed_oom_++;
              }
              break;
            case HomogeneousSpaceCompactResult::kErrorReject:
              break;
            case HomogeneousSpaceCompactResult::kErrorVMShuttingDown:
              break;
            default: {
              UNIMPLEMENTED(FATAL) << "homogeneous space compaction result: "
                  << static_cast<size_t>(result);
              UNREACHABLE();
            }
          }
          VLOG(heap) << "Ran heap homogeneous space compaction, "
                    << " requested defragmentation "
                    << count_requested_homogeneous_space_compaction_.LoadSequentiallyConsistent()
                    << " performed defragmentation "
                    << count_performed_homogeneous_space_compaction_.LoadSequentiallyConsistent()
                    << " ignored homogeneous space compaction "
                    << count_ignored_homogeneous_space_compaction_.LoadSequentiallyConsistent()
                    << " delayed count = "
                    << count_delayed_oom_.LoadSequentiallyConsistent();
        }
        break;
      }
      case kAllocatorTypeNonMoving: {
        if (!IsOutOfMemoryOnAllocation<false>(allocator, alloc_size)) {
          DisableMovingGc();
          if (IsMovingGc(collector_type_)) {
            MutexLock mu(self, *gc_complete_lock_);
            LOG(WARNING) << "Couldn't disable moving GC with disable GC count "
                         << disable_moving_gc_count_;
          } else {
            LOG(WARNING) << "Disabled moving GC due to the non moving space being full";
            ptr = TryToAllocate<true, true>(self, allocator, alloc_size, bytes_allocated,
                                            usable_size, bytes_tl_bulk_allocated);
          }
        }
        break;
      }
      default: {
      }
    }
  }
  if (ptr == nullptr) {
    ThrowOutOfMemoryError(self, alloc_size, allocator);
  }
  return ptr;
}
void Heap::SetTargetHeapUtilization(float target) {
  DCHECK_GT(target, 0.0f);
  DCHECK_LT(target, 1.0f);
  target_utilization_ = target;
}
size_t Heap::GetObjectsAllocated() const {
  Thread* self = Thread::Current();
  ScopedThreadStateChange tsc(self, kWaitingForGetObjectsAllocated);
  auto* tl = Runtime::Current()->GetThreadList();
  tl->SuspendAll(__FUNCTION__);
  size_t total = 0;
  {
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    for (space::AllocSpace* space : alloc_spaces_) {
      total += space->GetObjectsAllocated();
    }
  }
  tl->ResumeAll();
  return total;
}
uint64_t Heap::GetObjectsAllocatedEver() const {
  uint64_t total = GetObjectsFreedEver();
  if (Thread::Current() != nullptr) {
    total += GetObjectsAllocated();
  }
  return total;
}
uint64_t Heap::GetBytesAllocatedEver() const {
  return GetBytesFreedEver() + GetBytesAllocated();
}
class InstanceCounter {
 public:
  InstanceCounter(const std::vector<mirror::Class*>& classes, bool use_is_assignable_from, uint64_t* counts)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : classes_(classes), use_is_assignable_from_(use_is_assignable_from), counts_(counts) {
  }
  static void Callback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    InstanceCounter* instance_counter = reinterpret_cast<InstanceCounter*>(arg);
    mirror::Class* instance_class = obj->GetClass();
    CHECK(instance_class != nullptr);
    for (size_t i = 0; i < instance_counter->classes_.size(); ++i) {
      if (instance_counter->use_is_assignable_from_) {
        if (instance_counter->classes_[i]->IsAssignableFrom(instance_class)) {
          ++instance_counter->counts_[i];
        }
      } else if (instance_class == instance_counter->classes_[i]) {
        ++instance_counter->counts_[i];
      }
    }
  }
 private:
  const std::vector<mirror::Class*>& classes_;
  bool use_is_assignable_from_;
  uint64_t* const counts_;
  DISALLOW_COPY_AND_ASSIGN(InstanceCounter);
};
void Heap::CountInstances(const std::vector<mirror::Class*>& classes, bool use_is_assignable_from,
                          uint64_t* counts) {
  InstanceCounter counter(classes, use_is_assignable_from, counts);
  VisitObjects(InstanceCounter::Callback, &counter);
}
class InstanceCollector {
 public:
  InstanceCollector(mirror::Class* c, int32_t max_count, std::vector<mirror::Object*>& instances)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : class_(c), max_count_(max_count), instances_(instances) {
  }
  static void Callback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    DCHECK(arg != nullptr);
    InstanceCollector* instance_collector = reinterpret_cast<InstanceCollector*>(arg);
    if (obj->GetClass() == instance_collector->class_) {
      if (instance_collector->max_count_ == 0 ||
          instance_collector->instances_.size() < instance_collector->max_count_) {
        instance_collector->instances_.push_back(obj);
      }
    }
  }
 private:
  const mirror::Class* const class_;
  const uint32_t max_count_;
  std::vector<mirror::Object*>& instances_;
  DISALLOW_COPY_AND_ASSIGN(InstanceCollector);
};
void Heap::GetInstances(mirror::Class* c, int32_t max_count,
                        std::vector<mirror::Object*>& instances) {
  InstanceCollector collector(c, max_count, instances);
  VisitObjects(&InstanceCollector::Callback, &collector);
}
class ReferringObjectsFinder {
 public:
  ReferringObjectsFinder(mirror::Object* object, int32_t max_count,
                         std::vector<mirror::Object*>& referring_objects)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : object_(object), max_count_(max_count), referring_objects_(referring_objects) {
  }
  static void Callback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    reinterpret_cast<ReferringObjectsFinder*>(arg)->operator()(obj);
  }
  void operator()(mirror::Object* o) const NO_THREAD_SAFETY_ANALYSIS {
    o->VisitReferences<true>(*this, VoidFunctor());
  }
  void operator()(mirror::Object* obj, MemberOffset offset, bool ) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::Object* ref = obj->GetFieldObject<mirror::Object>(offset);
    if (ref == object_ && (max_count_ == 0 || referring_objects_.size() < max_count_)) {
      referring_objects_.push_back(obj);
    }
  }
 private:
  const mirror::Object* const object_;
  const uint32_t max_count_;
  std::vector<mirror::Object*>& referring_objects_;
  DISALLOW_COPY_AND_ASSIGN(ReferringObjectsFinder);
};
void Heap::GetReferringObjects(mirror::Object* o, int32_t max_count,
                               std::vector<mirror::Object*>& referring_objects) {
  ReferringObjectsFinder finder(o, max_count, referring_objects);
  VisitObjects(&ReferringObjectsFinder::Callback, &finder);
}
void Heap::CollectGarbage(bool clear_soft_references) {
  CollectGarbageInternal(gc_plan_.back(), kGcCauseExplicit, clear_soft_references);
}
HomogeneousSpaceCompactResult Heap::PerformHomogeneousSpaceCompact() {
  Thread* self = Thread::Current();
  count_requested_homogeneous_space_compaction_++;
  ThreadList* tl = Runtime::Current()->GetThreadList();
  ScopedThreadStateChange tsc(self, kWaitingPerformingGc);
  Locks::mutator_lock_->AssertNotHeld(self);
  {
    ScopedThreadStateChange tsc2(self, kWaitingForGcToComplete);
    MutexLock mu(self, *gc_complete_lock_);
    WaitForGcToCompleteLocked(kGcCauseHomogeneousSpaceCompact, self);
    if (disable_moving_gc_count_ != 0 || IsMovingGc(collector_type_) ||
        !main_space_->CanMoveObjects()) {
      return HomogeneousSpaceCompactResult::kErrorReject;
    }
    collector_type_running_ = kCollectorTypeHomogeneousSpaceCompact;
  }
  if (Runtime::Current()->IsShuttingDown(self)) {
    FinishGC(self, collector::kGcTypeNone);
    return HomogeneousSpaceCompactResult::kErrorVMShuttingDown;
  }
  tl->SuspendAll(__FUNCTION__);
  uint64_t start_time = NanoTime();
  space::MallocSpace* to_space = main_space_backup_.release();
  space::MallocSpace* from_space = main_space_;
  to_space->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
  const uint64_t space_size_before_compaction = from_space->Size();
  AddSpace(to_space);
  CHECK_GE(to_space->GetFootprintLimit(), from_space->GetFootprintLimit());
  collector::GarbageCollector* collector = Compact(to_space, from_space,
                                                   kGcCauseHomogeneousSpaceCompact);
  const uint64_t space_size_after_compaction = to_space->Size();
  main_space_ = to_space;
  main_space_backup_.reset(from_space);
  RemoveSpace(from_space);
  SetSpaceAsDefault(main_space_);
  count_performed_homogeneous_space_compaction_++;
  uint64_t duration = NanoTime() - start_time;
  VLOG(heap) << "Heap homogeneous space compaction took " << PrettyDuration(duration) << " size: "
             << PrettySize(space_size_before_compaction) << " -> "
             << PrettySize(space_size_after_compaction) << " compact-ratio: "
             << std::fixed << static_cast<double>(space_size_after_compaction) /
             static_cast<double>(space_size_before_compaction);
  tl->ResumeAll();
  reference_processor_.EnqueueClearedReferences(self);
  GrowForUtilization(semi_space_collector_);
  LogGC(kGcCauseHomogeneousSpaceCompact, collector);
  FinishGC(self, collector::kGcTypeFull);
  return HomogeneousSpaceCompactResult::kSuccess;
}
void Heap::TransitionCollector(CollectorType collector_type) {
  if (collector_type == collector_type_) {
    return;
  }
  VLOG(heap) << "TransitionCollector: " << static_cast<int>(collector_type_)
             << " -> " << static_cast<int>(collector_type);
  uint64_t start_time = NanoTime();
  uint32_t before_allocated = num_bytes_allocated_.LoadSequentiallyConsistent();
  Runtime* const runtime = Runtime::Current();
  ThreadList* const tl = runtime->GetThreadList();
  Thread* const self = Thread::Current();
  ScopedThreadStateChange tsc(self, kWaitingPerformingGc);
  Locks::mutator_lock_->AssertNotHeld(self);
  for (;;) {
    {
      ScopedThreadStateChange tsc2(self, kWaitingForGcToComplete);
      MutexLock mu(self, *gc_complete_lock_);
      WaitForGcToCompleteLocked(kGcCauseCollectorTransition, self);
      const bool copying_transition = IsMovingGc(collector_type_) != IsMovingGc(collector_type);
      if (collector_type == collector_type_) {
        return;
      }
      if (!copying_transition || disable_moving_gc_count_ == 0) {
        collector_type_running_ = copying_transition ? kCollectorTypeSS : collector_type;
        break;
      }
    }
    usleep(1000);
  }
  if (runtime->IsShuttingDown(self)) {
    FinishGC(self, collector::kGcTypeNone);
    return;
  }
  collector::GarbageCollector* collector = nullptr;
  tl->SuspendAll(__FUNCTION__);
  switch (collector_type) {
    case kCollectorTypeSS: {
      if (!IsMovingGc(collector_type_)) {
        CHECK(main_space_backup_ != nullptr);
        std::unique_ptr<MemMap> mem_map(main_space_backup_->ReleaseMemMap());
        CHECK(mem_map != nullptr);
        mem_map->Protect(PROT_READ | PROT_WRITE);
        bump_pointer_space_ = space::BumpPointerSpace::CreateFromMemMap("Bump pointer space",
                                                                        mem_map.release());
        AddSpace(bump_pointer_space_);
        collector = Compact(bump_pointer_space_, main_space_, kGcCauseCollectorTransition);
        mem_map.reset(main_space_->ReleaseMemMap());
        if (dlmalloc_space_ == main_space_) {
          dlmalloc_space_ = nullptr;
        } else if (rosalloc_space_ == main_space_) {
          rosalloc_space_ = nullptr;
        }
        RemoveSpace(main_space_);
        RemoveRememberedSet(main_space_);
        delete main_space_;
        main_space_ = nullptr;
        RemoveRememberedSet(main_space_backup_.get());
        main_space_backup_.reset(nullptr);
        temp_space_ = space::BumpPointerSpace::CreateFromMemMap("Bump pointer space 2",
                                                                mem_map.release());
        AddSpace(temp_space_);
      }
      break;
    }
    case kCollectorTypeMS:
    case kCollectorTypeCMS: {
      if (IsMovingGc(collector_type_)) {
        CHECK(temp_space_ != nullptr);
        std::unique_ptr<MemMap> mem_map(temp_space_->ReleaseMemMap());
        RemoveSpace(temp_space_);
        temp_space_ = nullptr;
        mem_map->Protect(PROT_READ | PROT_WRITE);
        CreateMainMallocSpace(mem_map.get(), kDefaultInitialSize,
                              std::min(mem_map->Size(), growth_limit_), mem_map->Size());
        mem_map.release();
        AddSpace(main_space_);
        collector = Compact(main_space_, bump_pointer_space_, kGcCauseCollectorTransition);
        mem_map.reset(bump_pointer_space_->ReleaseMemMap());
        RemoveSpace(bump_pointer_space_);
        bump_pointer_space_ = nullptr;
        const char* name = kUseRosAlloc ? kRosAllocSpaceName[1] : kDlMallocSpaceName[1];
        if (kIsDebugBuild && kUseRosAlloc) {
          mem_map->Protect(PROT_READ | PROT_WRITE);
        }
        main_space_backup_.reset(CreateMallocSpaceFromMemMap(
            mem_map.get(), kDefaultInitialSize, std::min(mem_map->Size(), growth_limit_),
            mem_map->Size(), name, true));
        if (kIsDebugBuild && kUseRosAlloc) {
          mem_map->Protect(PROT_NONE);
        }
        mem_map.release();
      }
      break;
    }
    default: {
      LOG(FATAL) << "Attempted to transition to invalid collector type "
                 << static_cast<size_t>(collector_type);
      break;
    }
  }
  ChangeCollector(collector_type);
  tl->ResumeAll();
  reference_processor_.EnqueueClearedReferences(self);
  uint64_t duration = NanoTime() - start_time;
  GrowForUtilization(semi_space_collector_);
  DCHECK(collector != nullptr);
  LogGC(kGcCauseCollectorTransition, collector);
  FinishGC(self, collector::kGcTypeFull);
  int32_t after_allocated = num_bytes_allocated_.LoadSequentiallyConsistent();
  int32_t delta_allocated = before_allocated - after_allocated;
  std::string saved_str;
  if (delta_allocated >= 0) {
    saved_str = " saved at least " + PrettySize(delta_allocated);
  } else {
    saved_str = " expanded " + PrettySize(-delta_allocated);
  }
  VLOG(heap) << "Heap transition to " << process_state_ << " took "
      << PrettyDuration(duration) << saved_str;
}
void Heap::ChangeCollector(CollectorType collector_type) {
  if (collector_type != collector_type_) {
    if (collector_type == kCollectorTypeMC) {
      CHECK(kMarkCompactSupport);
    }
    collector_type_ = collector_type;
    gc_plan_.clear();
    switch (collector_type_) {
      case kCollectorTypeCC: {
        gc_plan_.push_back(collector::kGcTypeFull);
        if (use_tlab_) {
          ChangeAllocator(kAllocatorTypeRegionTLAB);
        } else {
          ChangeAllocator(kAllocatorTypeRegion);
        }
        break;
      }
      case kCollectorTypeMC:
      case kCollectorTypeSS:
      case kCollectorTypeGSS: {
        gc_plan_.push_back(collector::kGcTypeFull);
        if (use_tlab_) {
          ChangeAllocator(kAllocatorTypeTLAB);
        } else {
          ChangeAllocator(kAllocatorTypeBumpPointer);
        }
        break;
      }
      case kCollectorTypeMS: {
        gc_plan_.push_back(collector::kGcTypeSticky);
        gc_plan_.push_back(collector::kGcTypePartial);
        gc_plan_.push_back(collector::kGcTypeFull);
        ChangeAllocator(kUseRosAlloc ? kAllocatorTypeRosAlloc : kAllocatorTypeDlMalloc);
        break;
      }
      case kCollectorTypeCMS: {
        gc_plan_.push_back(collector::kGcTypeSticky);
        gc_plan_.push_back(collector::kGcTypePartial);
        gc_plan_.push_back(collector::kGcTypeFull);
        ChangeAllocator(kUseRosAlloc ? kAllocatorTypeRosAlloc : kAllocatorTypeDlMalloc);
        break;
      }
      default: {
        UNIMPLEMENTED(FATAL);
        UNREACHABLE();
      }
    }
    if (IsGcConcurrent()) {
      concurrent_start_bytes_ =
          std::max(max_allowed_footprint_, kMinConcurrentRemainingBytes) - kMinConcurrentRemainingBytes;
    } else {
      concurrent_start_bytes_ = std::numeric_limits<size_t>::max();
    }
  }
}
class ZygoteCompactingCollector FINAL : public collector::SemiSpace {
 public:
  explicit ZygoteCompactingCollector(gc::Heap* heap) : SemiSpace(heap, false, "zygote collector"),
      bin_live_bitmap_(nullptr), bin_mark_bitmap_(nullptr) {
  }
  void BuildBins(space::ContinuousSpace* space) {
    bin_live_bitmap_ = space->GetLiveBitmap();
    bin_mark_bitmap_ = space->GetMarkBitmap();
    BinContext context;
    context.prev_ = reinterpret_cast<uintptr_t>(space->Begin());
    context.collector_ = this;
    WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
    bin_live_bitmap_->Walk(Callback, reinterpret_cast<void*>(&context));
    AddBin(reinterpret_cast<uintptr_t>(space->End()) - context.prev_, context.prev_);
  }
 private:
  struct BinContext {
    uintptr_t prev_;
    ZygoteCompactingCollector* collector_;
  };
  std::multimap<size_t, uintptr_t> bins_;
  accounting::ContinuousSpaceBitmap* bin_live_bitmap_;
  accounting::ContinuousSpaceBitmap* bin_mark_bitmap_;
  static void Callback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(arg != nullptr);
    BinContext* context = reinterpret_cast<BinContext*>(arg);
    ZygoteCompactingCollector* collector = context->collector_;
    uintptr_t object_addr = reinterpret_cast<uintptr_t>(obj);
    size_t bin_size = object_addr - context->prev_;
    collector->AddBin(bin_size, context->prev_);
    context->prev_ = object_addr + RoundUp(obj->SizeOf(), kObjectAlignment);
  }
  void AddBin(size_t size, uintptr_t position) {
    if (size != 0) {
      bins_.insert(std::make_pair(size, position));
    }
  }
  virtual bool ShouldSweepSpace(space::ContinuousSpace* space) const {
    UNUSED(space);
    return false;
  }
  virtual mirror::Object* MarkNonForwardedObject(mirror::Object* obj)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
    size_t obj_size = obj->SizeOf();
    size_t alloc_size = RoundUp(obj_size, kObjectAlignment);
    mirror::Object* forward_address;
    auto it = bins_.lower_bound(alloc_size);
    if (it == bins_.end()) {
      size_t bytes_allocated, dummy;
      forward_address = to_space_->Alloc(self_, alloc_size, &bytes_allocated, nullptr, &dummy);
      if (to_space_live_bitmap_ != nullptr) {
        to_space_live_bitmap_->Set(forward_address);
      } else {
        GetHeap()->GetNonMovingSpace()->GetLiveBitmap()->Set(forward_address);
        GetHeap()->GetNonMovingSpace()->GetMarkBitmap()->Set(forward_address);
      }
    } else {
      size_t size = it->first;
      uintptr_t pos = it->second;
      bins_.erase(it);
      forward_address = reinterpret_cast<mirror::Object*>(pos);
      bin_live_bitmap_->Set(forward_address);
      bin_mark_bitmap_->Set(forward_address);
      DCHECK_GE(size, alloc_size);
      AddBin(size - alloc_size, pos + alloc_size);
    }
    memcpy(reinterpret_cast<void*>(forward_address), obj, obj_size);
    if (kUseBakerOrBrooksReadBarrier) {
      obj->AssertReadBarrierPointer();
      if (kUseBrooksReadBarrier) {
        DCHECK_EQ(forward_address->GetReadBarrierPointer(), obj);
        forward_address->SetReadBarrierPointer(forward_address);
      }
      forward_address->AssertReadBarrierPointer();
    }
    return forward_address;
  }
};
void Heap::UnBindBitmaps() {
  TimingLogger::ScopedTiming t("UnBindBitmaps", GetCurrentGcIteration()->GetTimings());
  for (const auto& space : GetContinuousSpaces()) {
    if (space->IsContinuousMemMapAllocSpace()) {
      space::ContinuousMemMapAllocSpace* alloc_space = space->AsContinuousMemMapAllocSpace();
      if (alloc_space->HasBoundBitmaps()) {
        alloc_space->UnBindBitmaps();
      }
    }
  }
}
void Heap::PreZygoteFork() {
  if (!HasZygoteSpace()) {
    CollectGarbageInternal(collector::kGcTypeFull, kGcCauseBackground, false);
  }
  Thread* self = Thread::Current();
  MutexLock mu(self, zygote_creation_lock_);
  if (HasZygoteSpace()) {
    return;
  }
  Runtime::Current()->GetInternTable()->SwapPostZygoteWithPreZygote();
  Runtime::Current()->GetClassLinker()->MoveClassTableToPreZygote();
  VLOG(heap) << "Starting PreZygoteFork";
  non_moving_space_->Trim();
  non_moving_space_->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
  const bool same_space = non_moving_space_ == main_space_;
  if (kCompactZygote) {
    ScopedDisableRosAllocVerification disable_rosalloc_verif(this);
    ZygoteCompactingCollector zygote_collector(this);
    zygote_collector.BuildBins(non_moving_space_);
    space::BumpPointerSpace target_space("zygote bump space", non_moving_space_->End(),
                                         non_moving_space_->Limit());
    bool reset_main_space = false;
    if (IsMovingGc(collector_type_)) {
      if (collector_type_ == kCollectorTypeCC) {
        zygote_collector.SetFromSpace(region_space_);
      } else {
        zygote_collector.SetFromSpace(bump_pointer_space_);
      }
    } else {
      CHECK(main_space_ != nullptr);
      CHECK_NE(main_space_, non_moving_space_)
          << "Does not make sense to compact within the same space";
      zygote_collector.SetFromSpace(main_space_);
      reset_main_space = true;
    }
    zygote_collector.SetToSpace(&target_space);
    zygote_collector.SetSwapSemiSpaces(false);
    zygote_collector.Run(kGcCauseCollectorTransition, false);
    if (reset_main_space) {
      main_space_->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
      madvise(main_space_->Begin(), main_space_->Capacity(), MADV_DONTNEED);
      MemMap* mem_map = main_space_->ReleaseMemMap();
      RemoveSpace(main_space_);
      space::Space* old_main_space = main_space_;
      CreateMainMallocSpace(mem_map, kDefaultInitialSize, std::min(mem_map->Size(), growth_limit_),
                            mem_map->Size());
      delete old_main_space;
      AddSpace(main_space_);
    } else {
      if (collector_type_ == kCollectorTypeCC) {
        region_space_->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
      } else {
        bump_pointer_space_->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
      }
    }
    if (temp_space_ != nullptr) {
      CHECK(temp_space_->IsEmpty());
    }
    total_objects_freed_ever_ += GetCurrentGcIteration()->GetFreedObjects();
    total_bytes_freed_ever_ += GetCurrentGcIteration()->GetFreedBytes();
    non_moving_space_->SetEnd(target_space.End());
    non_moving_space_->SetLimit(target_space.Limit());
    VLOG(heap) << "Create zygote space with size=" << non_moving_space_->Size() << " bytes";
  }
  ChangeCollector(foreground_collector_type_);
  space::MallocSpace* old_alloc_space = non_moving_space_;
  RemoveSpace(old_alloc_space);
  if (collector::SemiSpace::kUseRememberedSet) {
    FindRememberedSetFromSpace(old_alloc_space)->AssertAllDirtyCardsAreWithinSpace();
    RemoveRememberedSet(old_alloc_space);
  }
  zygote_space_ = old_alloc_space->CreateZygoteSpace(kNonMovingSpaceName, low_memory_mode_,
                                                     &non_moving_space_);
  CHECK(!non_moving_space_->CanMoveObjects());
  if (same_space) {
    main_space_ = non_moving_space_;
    SetSpaceAsDefault(main_space_);
  }
  delete old_alloc_space;
  CHECK(HasZygoteSpace()) << "Failed creating zygote space";
  AddSpace(zygote_space_);
  non_moving_space_->SetFootprintLimit(non_moving_space_->Capacity());
  AddSpace(non_moving_space_);
  accounting::ModUnionTable* mod_union_table =
      new accounting::ModUnionTableCardCache("zygote space mod-union table", this,
                                             zygote_space_);
  CHECK(mod_union_table != nullptr) << "Failed to create zygote space mod-union table";
  mod_union_table->SetCards();
  AddModUnionTable(mod_union_table);
  large_object_space_->SetAllLargeObjectsAsZygoteObjects(self);
  if (collector::SemiSpace::kUseRememberedSet) {
    accounting::RememberedSet* post_zygote_non_moving_space_rem_set =
        new accounting::RememberedSet("Post-zygote non-moving space remembered set", this,
                                      non_moving_space_);
    CHECK(post_zygote_non_moving_space_rem_set != nullptr)
        << "Failed to create post-zygote non-moving space remembered set";
    AddRememberedSet(post_zygote_non_moving_space_rem_set);
  }
}
void Heap::FlushAllocStack() {
  MarkAllocStackAsLive(allocation_stack_.get());
  allocation_stack_->Reset();
}
void Heap::MarkAllocStack(accounting::ContinuousSpaceBitmap* bitmap1,
                          accounting::ContinuousSpaceBitmap* bitmap2,
                          accounting::LargeObjectBitmap* large_objects,
                          accounting::ObjectStack* stack) {
  DCHECK(bitmap1 != nullptr);
  DCHECK(bitmap2 != nullptr);
  const auto* limit = stack->End();
  for (auto* it = stack->Begin(); it != limit; ++it) {
    const mirror::Object* obj = it->AsMirrorPtr();
    if (!kUseThreadLocalAllocationStack || obj != nullptr) {
      if (bitmap1->HasAddress(obj)) {
        bitmap1->Set(obj);
      } else if (bitmap2->HasAddress(obj)) {
        bitmap2->Set(obj);
      } else {
        DCHECK(large_objects != nullptr);
        large_objects->Set(obj);
      }
    }
  }
}
void Heap::SwapSemiSpaces() {
  CHECK(bump_pointer_space_ != nullptr);
  CHECK(temp_space_ != nullptr);
  std::swap(bump_pointer_space_, temp_space_);
}
collector::GarbageCollector* Heap::Compact(space::ContinuousMemMapAllocSpace* target_space,
                                           space::ContinuousMemMapAllocSpace* source_space,
                                           GcCause gc_cause) {
  CHECK(kMovingCollector);
  if (target_space != source_space) {
    semi_space_collector_->SetSwapSemiSpaces(false);
    semi_space_collector_->SetFromSpace(source_space);
    semi_space_collector_->SetToSpace(target_space);
    semi_space_collector_->Run(gc_cause, false);
    return semi_space_collector_;
  } else {
    CHECK(target_space->IsBumpPointerSpace())
        << "In-place compaction is only supported for bump pointer spaces";
    mark_compact_collector_->SetSpace(target_space->AsBumpPointerSpace());
    mark_compact_collector_->Run(kGcCauseCollectorTransition, false);
    return mark_compact_collector_;
  }
}
collector::GcType Heap::CollectGarbageInternal(collector::GcType gc_type, GcCause gc_cause,
                                               bool clear_soft_references) {
  Thread* self = Thread::Current();
  Runtime* runtime = Runtime::Current();
  switch (gc_type) {
    case collector::kGcTypePartial: {
      if (!HasZygoteSpace()) {
        return collector::kGcTypeNone;
      }
      break;
    }
    default: {
    }
  }
  ScopedThreadStateChange tsc(self, kWaitingPerformingGc);
  Locks::mutator_lock_->AssertNotHeld(self);
  if (self->IsHandlingStackOverflow()) {
    return collector::kGcTypeNone;
  }
  bool compacting_gc;
  {
    gc_complete_lock_->AssertNotHeld(self);
    ScopedThreadStateChange tsc2(self, kWaitingForGcToComplete);
    MutexLock mu(self, *gc_complete_lock_);
    WaitForGcToCompleteLocked(gc_cause, self);
    compacting_gc = IsMovingGc(collector_type_);
    if (compacting_gc && disable_moving_gc_count_ != 0) {
      LOG(WARNING) << "Skipping GC due to disable moving GC count " << disable_moving_gc_count_;
      return collector::kGcTypeNone;
    }
    collector_type_running_ = collector_type_;
  }
  if (gc_cause == kGcCauseForAlloc && runtime->HasStatsEnabled()) {
    ++runtime->GetStats()->gc_for_alloc_count;
    ++self->GetStats()->gc_for_alloc_count;
  }
  const uint64_t bytes_allocated_before_gc = GetBytesAllocated();
  ATRACE_INT("Heap size (KB)", bytes_allocated_before_gc / KB);
  DCHECK_LT(gc_type, collector::kGcTypeMax);
  DCHECK_NE(gc_type, collector::kGcTypeNone);
  collector::GarbageCollector* collector = nullptr;
  if (compacting_gc) {
    DCHECK(current_allocator_ == kAllocatorTypeBumpPointer ||
           current_allocator_ == kAllocatorTypeTLAB ||
           current_allocator_ == kAllocatorTypeRegion ||
           current_allocator_ == kAllocatorTypeRegionTLAB);
    switch (collector_type_) {
      case kCollectorTypeSS:
      case kCollectorTypeGSS:
        semi_space_collector_->SetFromSpace(bump_pointer_space_);
        semi_space_collector_->SetToSpace(temp_space_);
        semi_space_collector_->SetSwapSemiSpaces(true);
        collector = semi_space_collector_;
        break;
      case kCollectorTypeCC:
        concurrent_copying_collector_->SetRegionSpace(region_space_);
        collector = concurrent_copying_collector_;
        break;
      case kCollectorTypeMC:
        mark_compact_collector_->SetSpace(bump_pointer_space_);
        collector = mark_compact_collector_;
        break;
      default:
        LOG(FATAL) << "Invalid collector type " << static_cast<size_t>(collector_type_);
    }
    if (collector != mark_compact_collector_ && collector != concurrent_copying_collector_) {
      temp_space_->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
      CHECK(temp_space_->IsEmpty());
    }
    gc_type = collector::kGcTypeFull;
  } else if (current_allocator_ == kAllocatorTypeRosAlloc ||
      current_allocator_ == kAllocatorTypeDlMalloc) {
    collector = FindCollectorByGcType(gc_type);
  } else {
    LOG(FATAL) << "Invalid current allocator " << current_allocator_;
  }
  if (IsGcConcurrent()) {
    concurrent_start_bytes_ = std::numeric_limits<size_t>::max();
  }
  CHECK(collector != nullptr)
      << "Could not find garbage collector with collector_type="
      << static_cast<size_t>(collector_type_) << " and gc_type=" << gc_type;
  collector->Run(gc_cause, clear_soft_references || runtime->IsZygote());
  total_objects_freed_ever_ += GetCurrentGcIteration()->GetFreedObjects();
  total_bytes_freed_ever_ += GetCurrentGcIteration()->GetFreedBytes();
  RequestTrim(self);
  reference_processor_.EnqueueClearedReferences(self);
  GrowForUtilization(collector, bytes_allocated_before_gc);
  LogGC(gc_cause, collector);
  FinishGC(self, gc_type);
  Dbg::GcDidFinish();
  return gc_type;
}
void Heap::LogGC(GcCause gc_cause, collector::GarbageCollector* collector) {
  const size_t duration = GetCurrentGcIteration()->GetDurationNs();
  const std::vector<uint64_t>& pause_times = GetCurrentGcIteration()->GetPauseTimes();
  bool log_gc = gc_cause == kGcCauseExplicit;
  if (!log_gc && CareAboutPauseTimes()) {
    log_gc = duration > long_gc_log_threshold_ ||
        (gc_cause == kGcCauseForAlloc && duration > long_pause_log_threshold_);
    for (uint64_t pause : pause_times) {
      log_gc = log_gc || pause >= long_pause_log_threshold_;
    }
  }
  if (log_gc) {
    const size_t percent_free = GetPercentFree();
    const size_t current_heap_size = GetBytesAllocated();
    const size_t total_memory = GetTotalMemory();
    std::ostringstream pause_string;
    for (size_t i = 0; i < pause_times.size(); ++i) {
      pause_string << PrettyDuration((pause_times[i] / 1000) * 1000)
                   << ((i != pause_times.size() - 1) ? "," : "");
    }
    LOG(INFO) << gc_cause << " " << collector->GetName()
              << " GC freed " << current_gc_iteration_.GetFreedObjects() << "("
              << PrettySize(current_gc_iteration_.GetFreedBytes()) << ") AllocSpace objects, "
              << current_gc_iteration_.GetFreedLargeObjects() << "("
              << PrettySize(current_gc_iteration_.GetFreedLargeObjectBytes()) << ") LOS objects, "
              << percent_free << "% free, " << PrettySize(current_heap_size) << "/"
              << PrettySize(total_memory) << ", " << "paused " << pause_string.str()
              << " total " << PrettyDuration((duration / 1000) * 1000);
    VLOG(heap) << Dumpable<TimingLogger>(*current_gc_iteration_.GetTimings());
  }
}
void Heap::FinishGC(Thread* self, collector::GcType gc_type) {
  MutexLock mu(self, *gc_complete_lock_);
  collector_type_running_ = kCollectorTypeNone;
  if (gc_type != collector::kGcTypeNone) {
    last_gc_type_ = gc_type;
    ++gc_count_last_window_;
    if (running_collection_is_blocking_) {
      ++blocking_gc_count_;
      blocking_gc_time_ += GetCurrentGcIteration()->GetDurationNs();
      ++blocking_gc_count_last_window_;
    }
    UpdateGcCountRateHistograms();
  }
  running_collection_is_blocking_ = false;
  gc_complete_cond_->Broadcast(self);
}
void Heap::UpdateGcCountRateHistograms() {
  DCHECK_EQ(last_update_time_gc_count_rate_histograms_ % kGcCountRateHistogramWindowDuration, 0U);
  uint64_t now = NanoTime();
  DCHECK_GE(now, last_update_time_gc_count_rate_histograms_);
  uint64_t time_since_last_update = now - last_update_time_gc_count_rate_histograms_;
  uint64_t num_of_windows = time_since_last_update / kGcCountRateHistogramWindowDuration;
  if (time_since_last_update >= kGcCountRateHistogramWindowDuration) {
    gc_count_rate_histogram_.AddValue(gc_count_last_window_ - 1);
    blocking_gc_count_rate_histogram_.AddValue(running_collection_is_blocking_ ?
        blocking_gc_count_last_window_ - 1 : blocking_gc_count_last_window_);
    for (uint64_t i = 0; i < num_of_windows - 1; ++i) {
      gc_count_rate_histogram_.AddValue(0);
      blocking_gc_count_rate_histogram_.AddValue(0);
    }
    last_update_time_gc_count_rate_histograms_ =
        (now / kGcCountRateHistogramWindowDuration) * kGcCountRateHistogramWindowDuration;
    gc_count_last_window_ = 1;
    blocking_gc_count_last_window_ = running_collection_is_blocking_ ? 1 : 0;
  }
  DCHECK_EQ(last_update_time_gc_count_rate_histograms_ % kGcCountRateHistogramWindowDuration, 0U);
}
class RootMatchesObjectVisitor : public SingleRootVisitor {
 public:
  explicit RootMatchesObjectVisitor(const mirror::Object* obj) : obj_(obj) { }
  void VisitRoot(mirror::Object* root, const RootInfo& info)
      OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (root == obj_) {
      LOG(INFO) << "Object " << obj_ << " is a root " << info.ToString();
    }
  }
 private:
  const mirror::Object* const obj_;
};
class ScanVisitor {
 public:
  void operator()(const mirror::Object* obj) const {
    LOG(ERROR) << "Would have rescanned object " << obj;
  }
};
class VerifyReferenceVisitor : public SingleRootVisitor {
 public:
  explicit VerifyReferenceVisitor(Heap* heap, Atomic<size_t>* fail_count, bool verify_referent)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_)
      : heap_(heap), fail_count_(fail_count), verify_referent_(verify_referent) {}
  size_t GetFailureCount() const {
    return fail_count_->LoadSequentiallyConsistent();
  }
  void operator()(mirror::Class* klass, mirror::Reference* ref) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    UNUSED(klass);
    if (verify_referent_) {
      VerifyReference(ref, ref->GetReferent(), mirror::Reference::ReferentOffset());
    }
  }
  void operator()(mirror::Object* obj, MemberOffset offset, bool ) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    VerifyReference(obj, obj->GetFieldObject<mirror::Object>(offset), offset);
  }
  bool IsLive(mirror::Object* obj) const NO_THREAD_SAFETY_ANALYSIS {
    return heap_->IsLiveObjectLocked(obj, true, false, true);
  }
  void VisitRoot(mirror::Object* root, const RootInfo& root_info) OVERRIDE
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (root == nullptr) {
      LOG(ERROR) << "Root is null with info " << root_info.GetType();
    } else if (!VerifyReference(nullptr, root, MemberOffset(0))) {
      LOG(ERROR) << "Root " << root << " is dead with type " << PrettyTypeOf(root)
          << " thread_id= " << root_info.GetThreadId() << " root_type= " << root_info.GetType();
    }
  }
 private:
  bool VerifyReference(mirror::Object* obj, mirror::Object* ref, MemberOffset offset) const
      NO_THREAD_SAFETY_ANALYSIS {
    if (ref == nullptr || IsLive(ref)) {
      return true;
    }
    if (fail_count_->FetchAndAddSequentiallyConsistent(1) == 0) {
      LOG(ERROR) << "!!!!!!!!!!!!!!Heap corruption detected!!!!!!!!!!!!!!!!!!!";
    }
    if (obj != nullptr) {
      accounting::CardTable* card_table = heap_->GetCardTable();
      accounting::ObjectStack* alloc_stack = heap_->allocation_stack_.get();
      accounting::ObjectStack* live_stack = heap_->live_stack_.get();
      uint8_t* card_addr = card_table->CardFromAddr(obj);
      LOG(ERROR) << "Object " << obj << " references dead object " << ref << " at offset "
                 << offset << "\n card value = " << static_cast<int>(*card_addr);
      if (heap_->IsValidObjectAddress(obj->GetClass())) {
        LOG(ERROR) << "Obj type " << PrettyTypeOf(obj);
      } else {
        LOG(ERROR) << "Object " << obj << " class(" << obj->GetClass() << ") not a heap address";
      }
      space::ContinuousSpace* ref_space = heap_->FindContinuousSpaceFromObject(ref, true);
      if (ref_space != nullptr && ref_space->IsMallocSpace()) {
        space::MallocSpace* space = ref_space->AsMallocSpace();
        mirror::Class* ref_class = space->FindRecentFreedObject(ref);
        if (ref_class != nullptr) {
          LOG(ERROR) << "Reference " << ref << " found as a recently freed object with class "
                     << PrettyClass(ref_class);
        } else {
          LOG(ERROR) << "Reference " << ref << " not found as a recently freed object";
        }
      }
      if (ref->GetClass() != nullptr && heap_->IsValidObjectAddress(ref->GetClass()) &&
          ref->GetClass()->IsClass()) {
        LOG(ERROR) << "Ref type " << PrettyTypeOf(ref);
      } else {
        LOG(ERROR) << "Ref " << ref << " class(" << ref->GetClass()
                   << ") is not a valid heap address";
      }
      card_table->CheckAddrIsInCardTable(reinterpret_cast<const uint8_t*>(obj));
      void* cover_begin = card_table->AddrFromCard(card_addr);
      void* cover_end = reinterpret_cast<void*>(reinterpret_cast<size_t>(cover_begin) +
          accounting::CardTable::kCardSize);
      LOG(ERROR) << "Card " << reinterpret_cast<void*>(card_addr) << " covers " << cover_begin
          << "-" << cover_end;
      accounting::ContinuousSpaceBitmap* bitmap =
          heap_->GetLiveBitmap()->GetContinuousSpaceBitmap(obj);
      if (bitmap == nullptr) {
        LOG(ERROR) << "Object " << obj << " has no bitmap";
        if (!VerifyClassClass(obj->GetClass())) {
          LOG(ERROR) << "Object " << obj << " failed class verification!";
        }
      } else {
        if (bitmap->Test(obj)) {
          LOG(ERROR) << "Object " << obj << " found in live bitmap";
        }
        if (alloc_stack->Contains(const_cast<mirror::Object*>(obj))) {
          LOG(ERROR) << "Object " << obj << " found in allocation stack";
        }
        if (live_stack->Contains(const_cast<mirror::Object*>(obj))) {
          LOG(ERROR) << "Object " << obj << " found in live stack";
        }
        if (alloc_stack->Contains(const_cast<mirror::Object*>(ref))) {
          LOG(ERROR) << "Ref " << ref << " found in allocation stack";
        }
        if (live_stack->Contains(const_cast<mirror::Object*>(ref))) {
          LOG(ERROR) << "Ref " << ref << " found in live stack";
        }
        ScanVisitor scan_visitor;
        uint8_t* byte_cover_begin = reinterpret_cast<uint8_t*>(card_table->AddrFromCard(card_addr));
        card_table->Scan<false>(bitmap, byte_cover_begin,
                                byte_cover_begin + accounting::CardTable::kCardSize, scan_visitor);
      }
      RootMatchesObjectVisitor visitor1(obj);
      Runtime::Current()->VisitRoots(&visitor1);
      RootMatchesObjectVisitor visitor2(ref);
      Runtime::Current()->VisitRoots(&visitor2);
    }
    return false;
  }
  Heap* const heap_;
  Atomic<size_t>* const fail_count_;
  const bool verify_referent_;
};
class VerifyObjectVisitor {
 public:
  explicit VerifyObjectVisitor(Heap* heap, Atomic<size_t>* fail_count, bool verify_referent)
      : heap_(heap), fail_count_(fail_count), verify_referent_(verify_referent) {
  }
  void operator()(mirror::Object* obj) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    VerifyReferenceVisitor visitor(heap_, fail_count_, verify_referent_);
    obj->VisitReferences<true>(visitor, visitor);
  }
  static void VisitCallback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    VerifyObjectVisitor* visitor = reinterpret_cast<VerifyObjectVisitor*>(arg);
    visitor->operator()(obj);
  }
  void VerifyRoots() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_) {
    ReaderMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
    VerifyReferenceVisitor visitor(heap_, fail_count_, verify_referent_);
    Runtime::Current()->VisitRoots(&visitor);
  }
  size_t GetFailureCount() const {
    return fail_count_->LoadSequentiallyConsistent();
  }
 private:
  Heap* const heap_;
  Atomic<size_t>* const fail_count_;
  const bool verify_referent_;
};
void Heap::PushOnAllocationStackWithInternalGC(Thread* self, mirror::Object** obj) {
  DCHECK(!allocation_stack_->AtomicPushBack(*obj));
  do {
    StackHandleScope<1> hs(self);
    HandleWrapper<mirror::Object> wrapper(hs.NewHandleWrapper(obj));
    CHECK(allocation_stack_->AtomicPushBackIgnoreGrowthLimit(*obj));
    CollectGarbageInternal(collector::kGcTypeSticky, kGcCauseForAlloc, false);
  } while (!allocation_stack_->AtomicPushBack(*obj));
}
void Heap::PushOnThreadLocalAllocationStackWithInternalGC(Thread* self, mirror::Object** obj) {
  DCHECK(!self->PushOnThreadLocalAllocationStack(*obj));
  StackReference<mirror::Object>* start_address;
  StackReference<mirror::Object>* end_address;
  while (!allocation_stack_->AtomicBumpBack(kThreadLocalAllocationStackSize, &start_address,
                                            &end_address)) {
    StackHandleScope<1> hs(self);
    HandleWrapper<mirror::Object> wrapper(hs.NewHandleWrapper(obj));
    CHECK(allocation_stack_->AtomicPushBackIgnoreGrowthLimit(*obj));
    CollectGarbageInternal(collector::kGcTypeSticky, kGcCauseForAlloc, false);
  }
  self->SetThreadLocalAllocationStack(start_address, end_address);
  CHECK(self->PushOnThreadLocalAllocationStack(*obj));
}
size_t Heap::VerifyHeapReferences(bool verify_referents) {
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertExclusiveHeld(self);
  allocation_stack_->Sort();
  live_stack_->Sort();
  RevokeAllThreadLocalAllocationStacks(self);
  Atomic<size_t> fail_count_(0);
  VerifyObjectVisitor visitor(this, &fail_count_, verify_referents);
  VisitObjectsPaused(VerifyObjectVisitor::VisitCallback, &visitor);
  visitor.VerifyRoots();
  if (visitor.GetFailureCount() > 0) {
    for (const auto& table_pair : mod_union_tables_) {
      accounting::ModUnionTable* mod_union_table = table_pair.second;
      mod_union_table->Dump(LOG(ERROR) << mod_union_table->GetName() << ": ");
    }
    for (const auto& table_pair : remembered_sets_) {
      accounting::RememberedSet* remembered_set = table_pair.second;
      remembered_set->Dump(LOG(ERROR) << remembered_set->GetName() << ": ");
    }
    DumpSpaces(LOG(ERROR));
  }
  return visitor.GetFailureCount();
}
class VerifyReferenceCardVisitor {
 public:
  VerifyReferenceCardVisitor(Heap* heap, bool* failed)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_,
                            Locks::heap_bitmap_lock_)
      : heap_(heap), failed_(failed) {
  }
  void operator()(mirror::Object* obj, MemberOffset offset, bool is_static) const
      NO_THREAD_SAFETY_ANALYSIS {
    mirror::Object* ref = obj->GetFieldObject<mirror::Object>(offset);
    if (ref != nullptr && !ref->IsClass()) {
      accounting::CardTable* card_table = heap_->GetCardTable();
      if (!card_table->AddrIsInCardTable(obj)) {
        LOG(ERROR) << "Object " << obj << " is not in the address range of the card table";
        *failed_ = true;
      } else if (!card_table->IsDirty(obj)) {
        accounting::ObjectStack* live_stack = heap_->live_stack_.get();
        if (live_stack->ContainsSorted(ref)) {
          if (live_stack->ContainsSorted(obj)) {
            LOG(ERROR) << "Object " << obj << " found in live stack";
          }
          if (heap_->GetLiveBitmap()->Test(obj)) {
            LOG(ERROR) << "Object " << obj << " found in live bitmap";
          }
          LOG(ERROR) << "Object " << obj << " " << PrettyTypeOf(obj)
                    << " references " << ref << " " << PrettyTypeOf(ref) << " in live stack";
          if (!obj->IsObjectArray()) {
            mirror::Class* klass = is_static ? obj->AsClass() : obj->GetClass();
            CHECK(klass != nullptr);
            auto* fields = is_static ? klass->GetSFields() : klass->GetIFields();
            auto num_fields = is_static ? klass->NumStaticFields() : klass->NumInstanceFields();
            CHECK_EQ(fields == nullptr, num_fields == 0u);
            for (size_t i = 0; i < num_fields; ++i) {
              ArtField* cur = &fields[i];
              if (cur->GetOffset().Int32Value() == offset.Int32Value()) {
                LOG(ERROR) << (is_static ? "Static " : "") << "field in the live stack is "
                          << PrettyField(cur);
                break;
              }
            }
          } else {
            mirror::ObjectArray<mirror::Object>* object_array =
                obj->AsObjectArray<mirror::Object>();
            for (int32_t i = 0; i < object_array->GetLength(); ++i) {
              if (object_array->Get(i) == ref) {
                LOG(ERROR) << (is_static ? "Static " : "") << "obj[" << i << "] = ref";
              }
            }
          }
          *failed_ = true;
        }
      }
    }
  }
 private:
  Heap* const heap_;
  bool* const failed_;
};
class VerifyLiveStackReferences {
 public:
  explicit VerifyLiveStackReferences(Heap* heap)
      : heap_(heap),
        failed_(false) {}
  void operator()(mirror::Object* obj) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    VerifyReferenceCardVisitor visitor(heap_, const_cast<bool*>(&failed_));
    obj->VisitReferences<true>(visitor, VoidFunctor());
  }
  bool Failed() const {
    return failed_;
  }
 private:
  Heap* const heap_;
  bool failed_;
};
bool Heap::VerifyMissingCardMarks() {
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertExclusiveHeld(self);
  live_stack_->Sort();
  RevokeAllThreadLocalAllocationStacks(self);
  VerifyLiveStackReferences visitor(this);
  GetLiveBitmap()->Visit(visitor);
  for (auto* it = live_stack_->Begin(); it != live_stack_->End(); ++it) {
    if (!kUseThreadLocalAllocationStack || it->AsMirrorPtr() != nullptr) {
      visitor(it->AsMirrorPtr());
    }
  }
  return !visitor.Failed();
}
void Heap::SwapStacks(Thread* self) {
  UNUSED(self);
  if (kUseThreadLocalAllocationStack) {
    live_stack_->AssertAllZero();
  }
  allocation_stack_.swap(live_stack_);
}
void Heap::RevokeAllThreadLocalAllocationStacks(Thread* self) {
  DCHECK(Locks::mutator_lock_->IsExclusiveHeld(self));
  MutexLock mu(self, *Locks::runtime_shutdown_lock_);
  MutexLock mu2(self, *Locks::thread_list_lock_);
  std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
  for (Thread* t : thread_list) {
    t->RevokeThreadLocalAllocationStack();
  }
}
void Heap::AssertThreadLocalBuffersAreRevoked(Thread* thread) {
  if (kIsDebugBuild) {
    if (rosalloc_space_ != nullptr) {
      rosalloc_space_->AssertThreadLocalBuffersAreRevoked(thread);
    }
    if (bump_pointer_space_ != nullptr) {
      bump_pointer_space_->AssertThreadLocalBuffersAreRevoked(thread);
    }
  }
}
void Heap::AssertAllBumpPointerSpaceThreadLocalBuffersAreRevoked() {
  if (kIsDebugBuild) {
    if (bump_pointer_space_ != nullptr) {
      bump_pointer_space_->AssertAllThreadLocalBuffersAreRevoked();
    }
  }
}
accounting::ModUnionTable* Heap::FindModUnionTableFromSpace(space::Space* space) {
  auto it = mod_union_tables_.find(space);
  if (it == mod_union_tables_.end()) {
    return nullptr;
  }
  return it->second;
}
accounting::RememberedSet* Heap::FindRememberedSetFromSpace(space::Space* space) {
  auto it = remembered_sets_.find(space);
  if (it == remembered_sets_.end()) {
    return nullptr;
  }
  return it->second;
}
void Heap::ProcessCards(TimingLogger* timings, bool use_rem_sets, bool process_alloc_space_cards,
                        bool clear_alloc_space_cards) {
  TimingLogger::ScopedTiming t(__FUNCTION__, timings);
  for (const auto& space : continuous_spaces_) {
    accounting::ModUnionTable* table = FindModUnionTableFromSpace(space);
    accounting::RememberedSet* rem_set = FindRememberedSetFromSpace(space);
    if (table != nullptr) {
      const char* name = space->IsZygoteSpace() ? "ZygoteModUnionClearCards" :
          "ImageModUnionClearCards";
      TimingLogger::ScopedTiming t2(name, timings);
      table->ClearCards();
    } else if (use_rem_sets && rem_set != nullptr) {
      DCHECK(collector::SemiSpace::kUseRememberedSet && collector_type_ == kCollectorTypeGSS)
          << static_cast<int>(collector_type_);
      TimingLogger::ScopedTiming t2("AllocSpaceRemSetClearCards", timings);
      rem_set->ClearCards();
    } else if (process_alloc_space_cards) {
      TimingLogger::ScopedTiming t2("AllocSpaceClearCards", timings);
      if (clear_alloc_space_cards) {
        card_table_->ClearCardRange(space->Begin(), space->End());
      } else {
        card_table_->ModifyCardsAtomic(space->Begin(), space->End(), AgeCardVisitor(),
                                       VoidFunctor());
      }
    }
  }
}
static void IdentityMarkHeapReferenceCallback(mirror::HeapReference<mirror::Object>*, void*) {
}
void Heap::PreGcVerificationPaused(collector::GarbageCollector* gc) {
  Thread* const self = Thread::Current();
  TimingLogger* const timings = current_gc_iteration_.GetTimings();
  TimingLogger::ScopedTiming t(__FUNCTION__, timings);
  if (verify_pre_gc_heap_) {
    TimingLogger::ScopedTiming t2("(Paused)PreGcVerifyHeapReferences", timings);
    size_t failures = VerifyHeapReferences();
    if (failures > 0) {
      LOG(FATAL) << "Pre " << gc->GetName() << " heap verification failed with " << failures
          << " failures";
    }
  }
  if (verify_missing_card_marks_) {
    TimingLogger::ScopedTiming t2("(Paused)PreGcVerifyMissingCardMarks", timings);
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    SwapStacks(self);
    CHECK(VerifyMissingCardMarks()) << "Pre " << gc->GetName()
                                    << " missing card mark verification failed\n" << DumpSpaces();
    SwapStacks(self);
  }
  if (verify_mod_union_table_) {
    TimingLogger::ScopedTiming t2("(Paused)PreGcVerifyModUnionTables", timings);
    ReaderMutexLock reader_lock(self, *Locks::heap_bitmap_lock_);
    for (const auto& table_pair : mod_union_tables_) {
      accounting::ModUnionTable* mod_union_table = table_pair.second;
      mod_union_table->UpdateAndMarkReferences(IdentityMarkHeapReferenceCallback, nullptr);
      mod_union_table->Verify();
    }
  }
}
void Heap::PreGcVerification(collector::GarbageCollector* gc) {
  if (verify_pre_gc_heap_ || verify_missing_card_marks_ || verify_mod_union_table_) {
    collector::GarbageCollector::ScopedPause pause(gc);
    PreGcVerificationPaused(gc);
  }
}
void Heap::PrePauseRosAllocVerification(collector::GarbageCollector* gc) {
  UNUSED(gc);
  if (verify_pre_gc_rosalloc_) {
    RosAllocVerification(current_gc_iteration_.GetTimings(), "PreGcRosAllocVerification");
  }
}
void Heap::PreSweepingGcVerification(collector::GarbageCollector* gc) {
  Thread* const self = Thread::Current();
  TimingLogger* const timings = current_gc_iteration_.GetTimings();
  TimingLogger::ScopedTiming t(__FUNCTION__, timings);
  if (verify_pre_sweeping_heap_) {
    TimingLogger::ScopedTiming t2("(Paused)PostSweepingVerifyHeapReferences", timings);
    CHECK_NE(self->GetState(), kRunnable);
    {
      WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
      gc->SwapBitmaps();
    }
    size_t failures = VerifyHeapReferences(false);
    if (failures > 0) {
      LOG(FATAL) << "Pre sweeping " << gc->GetName() << " GC verification failed with " << failures
          << " failures";
    }
    {
      WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
      gc->SwapBitmaps();
    }
  }
  if (verify_pre_sweeping_rosalloc_) {
    RosAllocVerification(timings, "PreSweepingRosAllocVerification");
  }
}
void Heap::PostGcVerificationPaused(collector::GarbageCollector* gc) {
  Thread* const self = Thread::Current();
  TimingLogger* const timings = GetCurrentGcIteration()->GetTimings();
  TimingLogger::ScopedTiming t(__FUNCTION__, timings);
  if (verify_system_weaks_) {
    ReaderMutexLock mu2(self, *Locks::heap_bitmap_lock_);
    collector::MarkSweep* mark_sweep = down_cast<collector::MarkSweep*>(gc);
    mark_sweep->VerifySystemWeaks();
  }
  if (verify_post_gc_rosalloc_) {
    RosAllocVerification(timings, "(Paused)PostGcRosAllocVerification");
  }
  if (verify_post_gc_heap_) {
    TimingLogger::ScopedTiming t2("(Paused)PostGcVerifyHeapReferences", timings);
    size_t failures = VerifyHeapReferences();
    if (failures > 0) {
      LOG(FATAL) << "Pre " << gc->GetName() << " heap verification failed with " << failures
          << " failures";
    }
  }
}
void Heap::PostGcVerification(collector::GarbageCollector* gc) {
  if (verify_system_weaks_ || verify_post_gc_rosalloc_ || verify_post_gc_heap_) {
    collector::GarbageCollector::ScopedPause pause(gc);
    PostGcVerificationPaused(gc);
  }
}
void Heap::RosAllocVerification(TimingLogger* timings, const char* name) {
  TimingLogger::ScopedTiming t(name, timings);
  for (const auto& space : continuous_spaces_) {
    if (space->IsRosAllocSpace()) {
      VLOG(heap) << name << " : " << space->GetName();
      space->AsRosAllocSpace()->Verify();
    }
  }
}
collector::GcType Heap::WaitForGcToComplete(GcCause cause, Thread* self) {
  ScopedThreadStateChange tsc(self, kWaitingForGcToComplete);
  MutexLock mu(self, *gc_complete_lock_);
  return WaitForGcToCompleteLocked(cause, self);
}
collector::GcType Heap::WaitForGcToCompleteLocked(GcCause cause, Thread* self) {
  collector::GcType last_gc_type = collector::kGcTypeNone;
  uint64_t wait_start = NanoTime();
  while (collector_type_running_ != kCollectorTypeNone) {
    if (self != task_processor_->GetRunningThread()) {
      running_collection_is_blocking_ = true;
      VLOG(gc) << "Waiting for a blocking GC " << cause;
    }
    ATRACE_BEGIN("GC: Wait For Completion");
    gc_complete_cond_->Wait(self);
    last_gc_type = last_gc_type_;
    ATRACE_END();
  }
  uint64_t wait_time = NanoTime() - wait_start;
  total_wait_time_ += wait_time;
  if (wait_time > long_pause_log_threshold_) {
    LOG(INFO) << "WaitForGcToComplete blocked for " << PrettyDuration(wait_time)
        << " for cause " << cause;
  }
  if (self != task_processor_->GetRunningThread()) {
    running_collection_is_blocking_ = true;
    VLOG(gc) << "Starting a blocking GC " << cause;
  }
  return last_gc_type;
}
void Heap::DumpForSigQuit(std::ostream& os) {
  os << "Heap: " << GetPercentFree() << "% free, " << PrettySize(GetBytesAllocated()) << "/"
     << PrettySize(GetTotalMemory()) << "; " << GetObjectsAllocated() << " objects\n";
  DumpGcPerformanceInfo(os);
}
size_t Heap::GetPercentFree() {
  return static_cast<size_t>(100.0f * static_cast<float>(GetFreeMemory()) / max_allowed_footprint_);
}
void Heap::SetIdealFootprint(size_t max_allowed_footprint) {
  if (max_allowed_footprint > GetMaxMemory()) {
    VLOG(gc) << "Clamp target GC heap from " << PrettySize(max_allowed_footprint) << " to "
             << PrettySize(GetMaxMemory());
    max_allowed_footprint = GetMaxMemory();
  }
  max_allowed_footprint_ = max_allowed_footprint;
}
bool Heap::IsMovableObject(const mirror::Object* obj) const {
  if (kMovingCollector) {
    space::Space* space = FindContinuousSpaceFromObject(obj, true);
    if (space != nullptr) {
      return space->CanMoveObjects();
    }
  }
  return false;
}
void Heap::UpdateMaxNativeFootprint() {
  size_t native_size = native_bytes_allocated_.LoadRelaxed();
  size_t target_size = native_size / GetTargetHeapUtilization();
  if (target_size > native_size + max_free_) {
    target_size = native_size + max_free_;
  } else if (target_size < native_size + min_free_) {
    target_size = native_size + min_free_;
  }
  native_footprint_gc_watermark_ = std::min(growth_limit_, target_size);
}
collector::GarbageCollector* Heap::FindCollectorByGcType(collector::GcType gc_type) {
  for (const auto& collector : garbage_collectors_) {
    if (collector->GetCollectorType() == collector_type_ &&
        collector->GetGcType() == gc_type) {
      return collector;
    }
  }
  return nullptr;
}
double Heap::HeapGrowthMultiplier() const {
  if (!CareAboutPauseTimes() || IsLowMemoryMode()) {
    return 1.0;
  }
  return foreground_heap_growth_multiplier_;
}
void Heap::GrowForUtilization(collector::GarbageCollector* collector_ran,
                              uint64_t bytes_allocated_before_gc) {
  const uint64_t bytes_allocated = GetBytesAllocated();
  uint64_t target_size;
  collector::GcType gc_type = collector_ran->GetGcType();
  const double multiplier = HeapGrowthMultiplier();
  const uint64_t adjusted_min_free = static_cast<uint64_t>(min_free_ * multiplier);
  const uint64_t adjusted_max_free = static_cast<uint64_t>(max_free_ * multiplier);
  if (gc_type != collector::kGcTypeSticky) {
    ssize_t delta = bytes_allocated / GetTargetHeapUtilization() - bytes_allocated;
    CHECK_GE(delta, 0);
    target_size = bytes_allocated + delta * multiplier;
    target_size = std::min(target_size, bytes_allocated + adjusted_max_free);
    target_size = std::max(target_size, bytes_allocated + adjusted_min_free);
    native_need_to_run_finalization_ = true;
    next_gc_type_ = collector::kGcTypeSticky;
  } else {
    collector::GcType non_sticky_gc_type =
        HasZygoteSpace() ? collector::kGcTypePartial : collector::kGcTypeFull;
    collector::GarbageCollector* non_sticky_collector = FindCollectorByGcType(non_sticky_gc_type);
    if (current_gc_iteration_.GetEstimatedThroughput() * kStickyGcThroughputAdjustment >=
        non_sticky_collector->GetEstimatedMeanThroughput() &&
        non_sticky_collector->NumberOfIterations() > 0 &&
        bytes_allocated <= max_allowed_footprint_) {
      next_gc_type_ = collector::kGcTypeSticky;
    } else {
      next_gc_type_ = non_sticky_gc_type;
    }
    if (bytes_allocated + adjusted_max_free < max_allowed_footprint_) {
      target_size = bytes_allocated + adjusted_max_free;
    } else {
      target_size = std::max(bytes_allocated, static_cast<uint64_t>(max_allowed_footprint_));
    }
  }
  if (!ignore_max_footprint_) {
    SetIdealFootprint(target_size);
    if (IsGcConcurrent()) {
      const uint64_t freed_bytes = current_gc_iteration_.GetFreedBytes() +
          current_gc_iteration_.GetFreedLargeObjectBytes() +
          current_gc_iteration_.GetFreedRevokeBytes();
      CHECK_GE(bytes_allocated + freed_bytes, bytes_allocated_before_gc);
      const uint64_t bytes_allocated_during_gc = bytes_allocated + freed_bytes -
          bytes_allocated_before_gc;
      const double gc_duration_seconds = NsToMs(current_gc_iteration_.GetDurationNs()) / 1000.0;
      size_t remaining_bytes = bytes_allocated_during_gc * gc_duration_seconds;
      remaining_bytes = std::min(remaining_bytes, kMaxConcurrentRemainingBytes);
      remaining_bytes = std::max(remaining_bytes, kMinConcurrentRemainingBytes);
      if (UNLIKELY(remaining_bytes > max_allowed_footprint_)) {
        remaining_bytes = kMinConcurrentRemainingBytes;
      }
      DCHECK_LE(remaining_bytes, max_allowed_footprint_);
      DCHECK_LE(max_allowed_footprint_, GetMaxMemory());
      concurrent_start_bytes_ = std::max(max_allowed_footprint_ - remaining_bytes,
                                         static_cast<size_t>(bytes_allocated));
    }
  }
}
void Heap::ClampGrowthLimit() {
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  capacity_ = growth_limit_;
  for (const auto& space : continuous_spaces_) {
    if (space->IsMallocSpace()) {
      gc::space::MallocSpace* malloc_space = space->AsMallocSpace();
      malloc_space->ClampGrowthLimit();
    }
  }
  if (main_space_backup_.get() != nullptr) {
    main_space_backup_->ClampGrowthLimit();
  }
}
void Heap::ClearGrowthLimit() {
  growth_limit_ = capacity_;
  for (const auto& space : continuous_spaces_) {
    if (space->IsMallocSpace()) {
      gc::space::MallocSpace* malloc_space = space->AsMallocSpace();
      malloc_space->ClearGrowthLimit();
      malloc_space->SetFootprintLimit(malloc_space->Capacity());
    }
  }
  if (main_space_backup_.get() != nullptr) {
    main_space_backup_->ClearGrowthLimit();
    main_space_backup_->SetFootprintLimit(main_space_backup_->Capacity());
  }
}
void Heap::AddFinalizerReference(Thread* self, mirror::Object** object) {
  ScopedObjectAccess soa(self);
  ScopedLocalRef<jobject> arg(self->GetJniEnv(), soa.AddLocalReference<jobject>(*object));
  jvalue args[1];
  args[0].l = arg.get();
  InvokeWithJValues(soa, nullptr, WellKnownClasses::java_lang_ref_FinalizerReference_add, args);
  *object = soa.Decode<mirror::Object*>(arg.get());
}
void Heap::RequestConcurrentGCAndSaveObject(Thread* self, bool force_full, mirror::Object** obj) {
  StackHandleScope<1> hs(self);
  HandleWrapper<mirror::Object> wrapper(hs.NewHandleWrapper(obj));
  RequestConcurrentGC(self, force_full);
}
class Heap::ConcurrentGCTask : public HeapTask {
 public:
  explicit ConcurrentGCTask(uint64_t target_time, bool force_full)
    : HeapTask(target_time), force_full_(force_full) { }
  virtual void Run(Thread* self) OVERRIDE {
    gc::Heap* heap = Runtime::Current()->GetHeap();
    heap->ConcurrentGC(self, force_full_);
    heap->ClearConcurrentGCRequest();
  }
 private:
  const bool force_full_;
};
static bool CanAddHeapTask(Thread* self) LOCKS_EXCLUDED(Locks::runtime_shutdown_lock_) {
  Runtime* runtime = Runtime::Current();
  return runtime != nullptr && runtime->IsFinishedStarting() && !runtime->IsShuttingDown(self) &&
      !self->IsHandlingStackOverflow();
}
void Heap::ClearConcurrentGCRequest() {
  concurrent_gc_pending_.StoreRelaxed(false);
}
void Heap::RequestConcurrentGC(Thread* self, bool force_full) {
  if (CanAddHeapTask(self) &&
      concurrent_gc_pending_.CompareExchangeStrongSequentiallyConsistent(false, true)) {
    task_processor_->AddTask(self, new ConcurrentGCTask(NanoTime(),
                                                        force_full));
  }
}
void Heap::ConcurrentGC(Thread* self, bool force_full) {
  if (!Runtime::Current()->IsShuttingDown(self)) {
    if (WaitForGcToComplete(kGcCauseBackground, self) == collector::kGcTypeNone) {
      collector::GcType next_gc_type = next_gc_type_;
      if (force_full && next_gc_type == collector::kGcTypeSticky) {
        next_gc_type = HasZygoteSpace() ? collector::kGcTypePartial : collector::kGcTypeFull;
      }
      if (CollectGarbageInternal(next_gc_type, kGcCauseBackground, false) ==
          collector::kGcTypeNone) {
        for (collector::GcType gc_type : gc_plan_) {
          if (gc_type > next_gc_type &&
              CollectGarbageInternal(gc_type, kGcCauseBackground, false) !=
                  collector::kGcTypeNone) {
            break;
          }
        }
      }
    }
  }
}
class Heap::CollectorTransitionTask : public HeapTask {
 public:
  explicit CollectorTransitionTask(uint64_t target_time) : HeapTask(target_time) { }
  virtual void Run(Thread* self) OVERRIDE {
    gc::Heap* heap = Runtime::Current()->GetHeap();
    heap->DoPendingCollectorTransition();
    heap->ClearPendingCollectorTransition(self);
  }
};
void Heap::ClearPendingCollectorTransition(Thread* self) {
  MutexLock mu(self, *pending_task_lock_);
  pending_collector_transition_ = nullptr;
}
void Heap::RequestCollectorTransition(CollectorType desired_collector_type, uint64_t delta_time) {
  Thread* self = Thread::Current();
  desired_collector_type_ = desired_collector_type;
  if (desired_collector_type_ == collector_type_ || !CanAddHeapTask(self)) {
    return;
  }
  CollectorTransitionTask* added_task = nullptr;
  const uint64_t target_time = NanoTime() + delta_time;
  {
    MutexLock mu(self, *pending_task_lock_);
    if (pending_collector_transition_ != nullptr) {
      task_processor_->UpdateTargetRunTime(self, pending_collector_transition_, target_time);
      return;
    }
    added_task = new CollectorTransitionTask(target_time);
    pending_collector_transition_ = added_task;
  }
  task_processor_->AddTask(self, added_task);
}
class Heap::HeapTrimTask : public HeapTask {
 public:
  explicit HeapTrimTask(uint64_t delta_time) : HeapTask(NanoTime() + delta_time) { }
  virtual void Run(Thread* self) OVERRIDE {
    gc::Heap* heap = Runtime::Current()->GetHeap();
    heap->Trim(self);
    heap->ClearPendingTrim(self);
  }
};
void Heap::ClearPendingTrim(Thread* self) {
  MutexLock mu(self, *pending_task_lock_);
  pending_heap_trim_ = nullptr;
}
void Heap::RequestTrim(Thread* self) {
  if (!CanAddHeapTask(self)) {
    return;
  }
  HeapTrimTask* added_task = nullptr;
  {
    MutexLock mu(self, *pending_task_lock_);
    if (pending_heap_trim_ != nullptr) {
      return;
    }
    added_task = new HeapTrimTask(kHeapTrimWait);
    pending_heap_trim_ = added_task;
  }
  task_processor_->AddTask(self, added_task);
}
void Heap::RevokeThreadLocalBuffers(Thread* thread) {
  if (rosalloc_space_ != nullptr) {
    size_t freed_bytes_revoke = rosalloc_space_->RevokeThreadLocalBuffers(thread);
    if (freed_bytes_revoke > 0U) {
      num_bytes_freed_revoke_.FetchAndAddSequentiallyConsistent(freed_bytes_revoke);
      CHECK_GE(num_bytes_allocated_.LoadRelaxed(), num_bytes_freed_revoke_.LoadRelaxed());
    }
  }
  if (bump_pointer_space_ != nullptr) {
    CHECK_EQ(bump_pointer_space_->RevokeThreadLocalBuffers(thread), 0U);
  }
  if (region_space_ != nullptr) {
    CHECK_EQ(region_space_->RevokeThreadLocalBuffers(thread), 0U);
  }
}
void Heap::RevokeRosAllocThreadLocalBuffers(Thread* thread) {
  if (rosalloc_space_ != nullptr) {
    size_t freed_bytes_revoke = rosalloc_space_->RevokeThreadLocalBuffers(thread);
    if (freed_bytes_revoke > 0U) {
      num_bytes_freed_revoke_.FetchAndAddSequentiallyConsistent(freed_bytes_revoke);
      CHECK_GE(num_bytes_allocated_.LoadRelaxed(), num_bytes_freed_revoke_.LoadRelaxed());
    }
  }
}
void Heap::RevokeAllThreadLocalBuffers() {
  if (rosalloc_space_ != nullptr) {
    size_t freed_bytes_revoke = rosalloc_space_->RevokeAllThreadLocalBuffers();
    if (freed_bytes_revoke > 0U) {
      num_bytes_freed_revoke_.FetchAndAddSequentiallyConsistent(freed_bytes_revoke);
      CHECK_GE(num_bytes_allocated_.LoadRelaxed(), num_bytes_freed_revoke_.LoadRelaxed());
    }
  }
  if (bump_pointer_space_ != nullptr) {
    CHECK_EQ(bump_pointer_space_->RevokeAllThreadLocalBuffers(), 0U);
  }
  if (region_space_ != nullptr) {
    CHECK_EQ(region_space_->RevokeAllThreadLocalBuffers(), 0U);
  }
}
bool Heap::IsGCRequestPending() const {
  return concurrent_gc_pending_.LoadRelaxed();
}
void Heap::RunFinalization(JNIEnv* env, uint64_t timeout) {
  env->CallStaticVoidMethod(WellKnownClasses::dalvik_system_VMRuntime,
                            WellKnownClasses::dalvik_system_VMRuntime_runFinalization,
                            static_cast<jlong>(timeout));
}
void Heap::RegisterNativeAllocation(JNIEnv* env, size_t bytes) {
  Thread* self = ThreadForEnv(env);
  if (native_need_to_run_finalization_) {
    RunFinalization(env, kNativeAllocationFinalizeTimeout);
    UpdateMaxNativeFootprint();
    native_need_to_run_finalization_ = false;
  }
  size_t new_native_bytes_allocated = native_bytes_allocated_.FetchAndAddSequentiallyConsistent(bytes);
  new_native_bytes_allocated += bytes;
  if (new_native_bytes_allocated > native_footprint_gc_watermark_) {
    collector::GcType gc_type = HasZygoteSpace() ? collector::kGcTypePartial :
        collector::kGcTypeFull;
    if (new_native_bytes_allocated > growth_limit_) {
      if (WaitForGcToComplete(kGcCauseForNativeAlloc, self) != collector::kGcTypeNone) {
        RunFinalization(env, kNativeAllocationFinalizeTimeout);
        CHECK(!env->ExceptionCheck());
        new_native_bytes_allocated = native_bytes_allocated_.LoadRelaxed();
      }
      if (new_native_bytes_allocated > growth_limit_) {
        CollectGarbageInternal(gc_type, kGcCauseForNativeAlloc, false);
        RunFinalization(env, kNativeAllocationFinalizeTimeout);
        native_need_to_run_finalization_ = false;
        CHECK(!env->ExceptionCheck());
      }
      UpdateMaxNativeFootprint();
    } else if (!IsGCRequestPending()) {
      if (IsGcConcurrent()) {
        RequestConcurrentGC(self, true);
      } else {
        CollectGarbageInternal(gc_type, kGcCauseForNativeAlloc, false);
      }
    }
  }
}
void Heap::RegisterNativeFree(JNIEnv* env, size_t bytes) {
  size_t expected_size;
  do {
    expected_size = native_bytes_allocated_.LoadRelaxed();
    if (UNLIKELY(bytes > expected_size)) {
      ScopedObjectAccess soa(env);
      env->ThrowNew(WellKnownClasses::java_lang_RuntimeException,
                    StringPrintf("Attempted to free %zd native bytes with only %zd native bytes "
                                 "registered as allocated", bytes, expected_size).c_str());
      break;
    }
  } while (!native_bytes_allocated_.CompareExchangeWeakRelaxed(expected_size,
                                                               expected_size - bytes));
}
size_t Heap::GetTotalMemory() const {
  return std::max(max_allowed_footprint_, GetBytesAllocated());
}
void Heap::AddModUnionTable(accounting::ModUnionTable* mod_union_table) {
  DCHECK(mod_union_table != nullptr);
  mod_union_tables_.Put(mod_union_table->GetSpace(), mod_union_table);
}
void Heap::CheckPreconditionsForAllocObject(mirror::Class* c, size_t byte_count) {
  CHECK(c == nullptr || (c->IsClassClass() && byte_count >= sizeof(mirror::Class)) ||
        (c->IsVariableSize() || c->GetObjectSize() == byte_count));
  CHECK_GE(byte_count, sizeof(mirror::Object));
}
void Heap::AddRememberedSet(accounting::RememberedSet* remembered_set) {
  CHECK(remembered_set != nullptr);
  space::Space* space = remembered_set->GetSpace();
  CHECK(space != nullptr);
  CHECK(remembered_sets_.find(space) == remembered_sets_.end()) << space;
  remembered_sets_.Put(space, remembered_set);
  CHECK(remembered_sets_.find(space) != remembered_sets_.end()) << space;
}
void Heap::RemoveRememberedSet(space::Space* space) {
  CHECK(space != nullptr);
  auto it = remembered_sets_.find(space);
  CHECK(it != remembered_sets_.end());
  delete it->second;
  remembered_sets_.erase(it);
  CHECK(remembered_sets_.find(space) == remembered_sets_.end());
}
void Heap::ClearMarkedObjects() {
  for (const auto& space : GetContinuousSpaces()) {
    accounting::ContinuousSpaceBitmap* mark_bitmap = space->GetMarkBitmap();
    if (space->GetLiveBitmap() != mark_bitmap) {
      mark_bitmap->Clear();
    }
  }
  for (const auto& space : GetDiscontinuousSpaces()) {
    space->GetMarkBitmap()->Clear();
  }
}
void Heap::SetAllocationRecords(AllocRecordObjectMap* records) {
  allocation_records_.reset(records);
}
void Heap::SweepAllocationRecords(IsMarkedCallback* visitor, void* arg) const {
  if (IsAllocTrackingEnabled()) {
    MutexLock mu(Thread::Current(), *Locks::alloc_tracker_lock_);
    if (IsAllocTrackingEnabled()) {
      GetAllocationRecords()->SweepAllocationRecords(visitor, arg);
    }
  }
}
class StackCrawlState {
 public:
  StackCrawlState(uintptr_t* frames, size_t max_depth, size_t skip_count)
      : frames_(frames), frame_count_(0), max_depth_(max_depth), skip_count_(skip_count) {
  }
  size_t GetFrameCount() const {
    return frame_count_;
  }
  static _Unwind_Reason_Code Callback(_Unwind_Context* context, void* arg) {
    auto* const state = reinterpret_cast<StackCrawlState*>(arg);
    const uintptr_t ip = _Unwind_GetIP(context);
    if (ip != 0 && state->skip_count_ > 0) {
      --state->skip_count_;
      return _URC_NO_REASON;
    }
    state->frames_[state->frame_count_] = ip;
    state->frame_count_++;
    return state->frame_count_ >= state->max_depth_ ? _URC_END_OF_STACK : _URC_NO_REASON;
  }
 private:
  uintptr_t* const frames_;
  size_t frame_count_;
  const size_t max_depth_;
  size_t skip_count_;
};
static size_t get_backtrace(uintptr_t* frames, size_t max_depth) {
  StackCrawlState state(frames, max_depth, 0u);
  _Unwind_Backtrace(&StackCrawlState::Callback, &state);
  return state.GetFrameCount();
}
void Heap::CheckGcStressMode(Thread* self, mirror::Object** obj) {
  auto* const runtime = Runtime::Current();
  if (gc_stress_mode_ && runtime->GetClassLinker()->IsInitialized() &&
      !runtime->IsActiveTransaction() && mirror::Class::HasJavaLangClass()) {
    bool new_backtrace = false;
    {
      static constexpr size_t kMaxFrames = 16u;
      uintptr_t backtrace[kMaxFrames];
      const size_t frames = get_backtrace(backtrace, kMaxFrames);
      uint64_t hash = 0;
      for (size_t i = 0; i < frames; ++i) {
        hash = hash * 2654435761 + backtrace[i];
        hash += (hash >> 13) ^ (hash << 6);
      }
      MutexLock mu(self, *backtrace_lock_);
      new_backtrace = seen_backtraces_.find(hash) == seen_backtraces_.end();
      if (new_backtrace) {
        seen_backtraces_.insert(hash);
      }
    }
    if (new_backtrace) {
      StackHandleScope<1> hs(self);
      auto h = hs.NewHandleWrapper(obj);
      CollectGarbage(false);
      unique_backtrace_count_.FetchAndAddSequentiallyConsistent(1);
    } else {
      seen_backtrace_count_.FetchAndAddSequentiallyConsistent(1);
    }
  }
}
}
}
