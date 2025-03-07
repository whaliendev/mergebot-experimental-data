#include "heap.h"
#include <limits>
#include "android-base/thread_annotations.h"
#if defined(__BIONIC__) || defined(__GLIBC__)
#include <malloc.h>
#endif
#include <memory>
#include <random>
#include <unistd.h>
#include <sys/types.h>
#include <vector>
#include "android-base/stringprintf.h"
#include "allocation_listener.h"
#include "art_field-inl.h"
#include "backtrace_helper.h"
#include "base/allocator.h"
#include "base/arena_allocator.h"
#include "base/dumpable.h"
#include "base/file_utils.h"
#include "base/histogram-inl.h"
#include "base/logging.h"
#include "base/memory_tool.h"
#include "base/mutex.h"
#include "base/os.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "base/time_utils.h"
#include "base/utils.h"
#include "class_root-inl.h"
#include "common_throws.h"
#include "debugger.h"
#include "dex/dex_file-inl.h"
#include "entrypoints/quick/quick_alloc_entrypoints.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/accounting/heap_bitmap-inl.h"
#include "gc/accounting/mod_union_table-inl.h"
#include "gc/accounting/read_barrier_table.h"
#include "gc/accounting/remembered_set.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/collector/concurrent_copying.h"
#include "gc/collector/mark_compact.h"
#include "gc/collector/mark_sweep.h"
#include "gc/collector/partial_mark_sweep.h"
#include "gc/collector/semi_space.h"
#include "gc/collector/sticky_mark_sweep.h"
#include "gc/racing_check.h"
#include "gc/reference_processor.h"
#include "gc/scoped_gc_critical_section.h"
#include "gc/space/bump_pointer_space.h"
#include "gc/space/dlmalloc_space-inl.h"
#include "gc/space/image_space.h"
#include "gc/space/large_object_space.h"
#include "gc/space/region_space.h"
#include "gc/space/rosalloc_space-inl.h"
#include "gc/space/space-inl.h"
#include "gc/space/zygote_space.h"
#include "gc/task_processor.h"
#include "gc/verification.h"
#include "gc_pause_listener.h"
#include "gc_root.h"
#include "handle_scope-inl.h"
#include "heap-inl.h"
#include "heap-visit-objects-inl.h"
#include "image.h"
#include "intern_table.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "jni/java_vm_ext.h"
#include "mirror/class-inl.h"
#include "mirror/executable-inl.h"
#include "mirror/field.h"
#include "mirror/method_handle_impl.h"
#include "mirror/object-inl.h"
#include "mirror/object-refvisitor-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/reference-inl.h"
#include "mirror/var_handle.h"
#include "nativehelper/scoped_local_ref.h"
#include "obj_ptr-inl.h"
#ifdef ART_TARGET_ANDROID
#include "perfetto/heap_profile.h"
#endif
#include "reflection.h"
#include "runtime.h"
#include "javaheapprof/javaheapsampler.h"
#include "scoped_thread_state_change-inl.h"
#include "thread-inl.h"
#include "thread_list.h"
#include "verify_object-inl.h"
#include "well_known_classes.h"
namespace art {
#ifdef ART_TARGET_ANDROID
namespace {
void EnableHeapSamplerCallback(void* enable_ptr,
                               const AHeapProfileEnableCallbackInfo* enable_info_ptr) {
  HeapSampler* sampler_self = reinterpret_cast<HeapSampler*>(enable_ptr);
  uint64_t interval = AHeapProfileEnableCallbackInfo_getSamplingInterval(enable_info_ptr);
  if (interval > 0) {
    sampler_self->SetSamplingInterval(interval);
  }
  sampler_self->EnableHeapSampler();
}
void DisableHeapSamplerCallback(void* disable_ptr,
                                const AHeapProfileDisableCallbackInfo* info_ptr ATTRIBUTE_UNUSED) {
  HeapSampler* sampler_self = reinterpret_cast<HeapSampler*>(disable_ptr);
  sampler_self->DisableHeapSampler();
}
}
#endif
namespace gc {
DEFINE_RUNTIME_DEBUG_FLAG(Heap, kStressCollectorTransition);
static constexpr size_t kMinConcurrentRemainingBytes = 128 * KB;
static constexpr size_t kMaxConcurrentRemainingBytes = 512 * KB;
static double GetStickyGcThroughputAdjustment(bool use_generational_cc) {
  return use_generational_cc ? 0.5 : 1.0;
}
static constexpr bool kCompactZygote = kMovingCollector;
static constexpr size_t kAllocationStackReserveSize = 1024;
static const size_t kDefaultMarkStackSize = 64 * KB;
static const char* kDlMallocSpaceName[2] = {"main dlmalloc space", "main dlmalloc space 1"};
static const char* kRosAllocSpaceName[2] = {"main rosalloc space", "main rosalloc space 1"};
static const char* kMemMapSpaceName[2] = {"main space", "main space 1"};
static const char* kNonMovingSpaceName = "non moving space";
static const char* kZygoteSpaceName = "zygote space";
static constexpr bool kGCALotMode = false;
static constexpr size_t kGcAlotAllocationStackSize = 4 * KB /
    sizeof(mirror::HeapReference<mirror::Object>);
static constexpr size_t kVerifyObjectAllocationStackSize = 16 * KB /
    sizeof(mirror::HeapReference<mirror::Object>);
static constexpr size_t kDefaultAllocationStackSize = 8 * MB /
    sizeof(mirror::HeapReference<mirror::Object>);
static constexpr double kMinFreeHeapAfterGcForAlloc = 0.01;
static constexpr uint32_t kAllocSpaceBeginForDeterministicAoT = 0x40000000;
static constexpr bool kDumpRosAllocStatsOnSigQuit = false;
static const char* kRegionSpaceName = "main space (region space)";
static constexpr bool kLogAllGCs = false;
static constexpr size_t kPostForkMaxHeapDurationMS = 2000;
#if defined(__LP64__) || !defined(ADDRESS_SANITIZER)
uint8_t* const Heap::kPreferredAllocSpaceBegin =
    reinterpret_cast<uint8_t*>(300 * MB - kDefaultNonMovingSpaceCapacity);
#else
#ifdef __ANDROID__
uint8_t* const Heap::kPreferredAllocSpaceBegin = reinterpret_cast<uint8_t*>(0x20000000);
#else
uint8_t* const Heap::kPreferredAllocSpaceBegin = reinterpret_cast<uint8_t*>(0x40000000);
#endif
#endif
static constexpr int64_t kGcStressModeGcLogSampleFrequencyNs = MsToNs(10000);
static inline bool CareAboutPauseTimes() {
  return Runtime::Current()->InJankPerceptibleProcessState();
}
static void VerifyBootImagesContiguity(const std::vector<gc::space::ImageSpace*>& image_spaces) {
  uint32_t boot_image_size = 0u;
  for (size_t i = 0u, num_spaces = image_spaces.size(); i != num_spaces; ) {
    const ImageHeader& image_header = image_spaces[i]->GetImageHeader();
    uint32_t reservation_size = image_header.GetImageReservationSize();
    uint32_t image_count = image_header.GetImageSpaceCount();
    CHECK_NE(image_count, 0u);
    CHECK_LE(image_count, num_spaces - i);
    CHECK_NE(reservation_size, 0u);
    for (size_t j = 1u; j != image_count; ++j) {
      CHECK_EQ(image_spaces[i + j]->GetImageHeader().GetComponentCount(), 0u);
      CHECK_EQ(image_spaces[i + j]->GetImageHeader().GetImageReservationSize(), 0u);
    }
    CHECK_EQ(image_spaces[0]->Begin() + boot_image_size, image_spaces[i]->Begin());
    const uint8_t* current_heap = image_spaces[i]->Begin();
    const uint8_t* current_oat = image_spaces[i]->GetImageHeader().GetOatFileBegin();
    for (size_t j = 0u; j != image_count; ++j) {
      const ImageHeader& current_header = image_spaces[i + j]->GetImageHeader();
      CHECK_EQ(current_heap, image_spaces[i + j]->Begin());
      CHECK_EQ(current_oat, current_header.GetOatFileBegin());
      current_heap += RoundUp(current_header.GetImageSize(), kPageSize);
      CHECK_GT(current_header.GetOatFileEnd(), current_header.GetOatFileBegin());
      current_oat = current_header.GetOatFileEnd();
    }
    CHECK_EQ(current_heap, image_spaces[i]->GetImageHeader().GetOatFileBegin());
    CHECK_EQ(reservation_size, static_cast<size_t>(current_oat - image_spaces[i]->Begin()));
    boot_image_size += reservation_size;
    i += image_count;
  }
}
Heap::Heap(size_t initial_size,
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
           const InstructionSet image_instruction_set,
           CollectorType foreground_collector_type,
           CollectorType background_collector_type,
           space::LargeObjectSpaceType large_object_space_type,
           size_t large_object_threshold,
           size_t parallel_gc_threads,
           size_t conc_gc_threads,
           bool low_memory_mode,
           size_t long_pause_log_threshold,
           size_t long_gc_log_threshold,
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
           bool use_homogeneous_space_compaction_for_oom,
           bool use_generational_cc,
           uint64_t min_interval_homogeneous_space_compaction_by_oom,
           bool dump_region_info_before_gc,
           bool dump_region_info_after_gc)
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
      process_cpu_start_time_ns_(ProcessCpuNanoTime()),
      pre_gc_last_process_cpu_time_ns_(process_cpu_start_time_ns_),
      post_gc_last_process_cpu_time_ns_(process_cpu_start_time_ns_),
      pre_gc_weighted_allocated_bytes_(0.0),
      post_gc_weighted_allocated_bytes_(0.0),
      ignore_target_footprint_(ignore_target_footprint),
      always_log_explicit_gcs_(always_log_explicit_gcs),
      zygote_creation_lock_("zygote creation lock", kZygoteCreationLock),
      zygote_space_(nullptr),
      large_object_threshold_(large_object_threshold),
      disable_thread_flip_count_(0),
      thread_flip_running_(false),
      collector_type_running_(kCollectorTypeNone),
      last_gc_cause_(kGcCauseNone),
      thread_running_gc_(nullptr),
      last_gc_type_(collector::kGcTypeNone),
      next_gc_type_(collector::kGcTypePartial),
      capacity_(capacity),
      growth_limit_(growth_limit),
      initial_heap_size_(initial_size),
      target_footprint_(initial_size),
      process_state_update_lock_("process state update lock", kPostMonitorLock),
      min_foreground_target_footprint_(0),
      min_foreground_concurrent_start_bytes_(0),
      concurrent_start_bytes_(std::numeric_limits<size_t>::max()),
      total_bytes_freed_ever_(0),
      total_objects_freed_ever_(0),
      num_bytes_allocated_(0),
      native_bytes_registered_(0),
      old_native_bytes_allocated_(0),
      native_objects_notified_(0),
      num_bytes_freed_revoke_(0),
      num_bytes_alive_after_gc_(0),
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
      stop_for_native_allocs_(stop_for_native_allocs),
      total_wait_time_(0),
      verify_object_mode_(kVerifyObjectModeDisabled),
      disable_moving_gc_count_(0),
      semi_space_collector_(nullptr),
      active_concurrent_copying_collector_(nullptr),
      young_concurrent_copying_collector_(nullptr),
      concurrent_copying_collector_(nullptr),
      is_running_on_memory_tool_(Runtime::Current()->IsRunningOnMemoryTool()),
      use_tlab_(use_tlab),
      main_space_backup_(nullptr),
      min_interval_homogeneous_space_compaction_by_oom_(
          min_interval_homogeneous_space_compaction_by_oom),
      last_time_homogeneous_space_compaction_by_oom_(NanoTime()),
      gcs_completed_(0u),
      max_gc_requested_(0u),
      pending_collector_transition_(nullptr),
      pending_heap_trim_(nullptr),
      use_homogeneous_space_compaction_for_oom_(use_homogeneous_space_compaction_for_oom),
      use_generational_cc_(use_generational_cc),
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
      alloc_record_depth_(AllocRecordObjectMap::kDefaultAllocStackDepth),
      backtrace_lock_(nullptr),
      seen_backtrace_count_(0u),
      unique_backtrace_count_(0u),
      gc_disabled_for_shutdown_(false),
      dump_region_info_before_gc_(dump_region_info_before_gc),
      dump_region_info_after_gc_(dump_region_info_after_gc),
      boot_image_spaces_(),
      boot_images_start_address_(0u),
      boot_images_size_(0u),
      pre_oome_gc_count_(0u) {
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "Heap() entering";
  }
  LOG(INFO) << "Using " << foreground_collector_type_ << " GC.";
  if (!gUseUserfaultfd) {
    auto [uffd_supported, minor_fault_supported] = collector::MarkCompact::GetUffdAndMinorFault();
    CHECK_IMPLIES(minor_fault_supported, uffd_supported);
  }
  if (gUseReadBarrier) {
    CHECK_EQ(foreground_collector_type_, kCollectorTypeCC);
    CHECK_EQ(background_collector_type_, kCollectorTypeCCBackground);
  } else if (background_collector_type_ != gc::kCollectorTypeHomogeneousSpaceCompact) {
    CHECK_EQ(IsMovingGc(foreground_collector_type_), IsMovingGc(background_collector_type_))
        << "Changing from " << foreground_collector_type_ << " to "
        << background_collector_type_ << " (or visa versa) is not supported.";
  }
  verification_.reset(new Verification(this));
  CHECK_GE(large_object_threshold, kMinLargeObjectThreshold);
  ScopedTrace trace(__FUNCTION__);
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
  if (foreground_collector_type_ == kCollectorTypeCC
      || foreground_collector_type_ == kCollectorTypeCMC) {
    use_homogeneous_space_compaction_for_oom_ = false;
  }
  bool support_homogeneous_space_compaction =
      background_collector_type_ == gc::kCollectorTypeHomogeneousSpaceCompact ||
      use_homogeneous_space_compaction_for_oom_;
  bool separate_non_moving_space = is_zygote ||
      support_homogeneous_space_compaction || IsMovingGc(foreground_collector_type_) ||
      IsMovingGc(background_collector_type_);
  uint8_t* request_begin = nullptr;
  size_t heap_reservation_size = 0u;
  if (separate_non_moving_space) {
    heap_reservation_size = non_moving_space_capacity;
  } else if (foreground_collector_type_ != kCollectorTypeCC && is_zygote) {
    heap_reservation_size = capacity_;
  }
  heap_reservation_size = RoundUp(heap_reservation_size, kPageSize);
  std::vector<std::unique_ptr<space::ImageSpace>> boot_image_spaces;
  MemMap heap_reservation;
  if (space::ImageSpace::LoadBootImage(boot_class_path,
                                       boot_class_path_locations,
                                       boot_class_path_fds,
                                       boot_class_path_image_fds,
                                       boot_class_path_vdex_fds,
                                       boot_class_path_oat_fds,
                                       image_file_names,
                                       image_instruction_set,
                                       runtime->ShouldRelocate(),
                                                       !runtime->IsAotCompiler(),
                                       heap_reservation_size,
                                       runtime->AllowInMemoryCompilation(),
                                       &boot_image_spaces,
                                       &heap_reservation)) {
    DCHECK_EQ(heap_reservation_size, heap_reservation.IsValid() ? heap_reservation.Size() : 0u);
    DCHECK(!boot_image_spaces.empty());
    request_begin = boot_image_spaces.back()->GetImageHeader().GetOatFileEnd();
    DCHECK_IMPLIES(heap_reservation.IsValid(), request_begin == heap_reservation.Begin())
        << "request_begin=" << static_cast<const void*>(request_begin)
        << " heap_reservation.Begin()=" << static_cast<const void*>(heap_reservation.Begin());
    for (std::unique_ptr<space::ImageSpace>& space : boot_image_spaces) {
      boot_image_spaces_.push_back(space.get());
      AddSpace(space.release());
    }
    boot_images_start_address_ = PointerToLowMemUInt32(boot_image_spaces_.front()->Begin());
    uint32_t boot_images_end =
        PointerToLowMemUInt32(boot_image_spaces_.back()->GetImageHeader().GetOatFileEnd());
    boot_images_size_ = boot_images_end - boot_images_start_address_;
    if (kIsDebugBuild) {
      VerifyBootImagesContiguity(boot_image_spaces_);
    }
  } else {
    if (foreground_collector_type_ == kCollectorTypeCC) {
      request_begin = kPreferredAllocSpaceBegin;
    }
    if (foreground_collector_type_ == kCollectorTypeMS && Runtime::Current()->IsAotCompiler()) {
      request_begin = reinterpret_cast<uint8_t*>(kAllocSpaceBeginForDeterministicAoT);
    }
  }
  MemMap main_mem_map_1;
  MemMap main_mem_map_2;
  std::string error_str;
  MemMap non_moving_space_mem_map;
  if (separate_non_moving_space) {
    ScopedTrace trace2("Create separate non moving space");
    const char* space_name = is_zygote ? kZygoteSpaceName : kNonMovingSpaceName;
    DCHECK_EQ(heap_reservation.IsValid(), !boot_image_spaces_.empty());
    if (heap_reservation.IsValid()) {
      non_moving_space_mem_map = heap_reservation.RemapAtEnd(
          heap_reservation.Begin(), space_name, PROT_READ | PROT_WRITE, &error_str);
    } else {
      non_moving_space_mem_map = MapAnonymousPreferredAddress(
          space_name, request_begin, non_moving_space_capacity, &error_str);
    }
    CHECK(non_moving_space_mem_map.IsValid()) << error_str;
    DCHECK(!heap_reservation.IsValid());
    request_begin = kPreferredAllocSpaceBegin + non_moving_space_capacity;
  }
  if (foreground_collector_type_ != kCollectorTypeCC) {
    ScopedTrace trace2("Create main mem map");
    if (separate_non_moving_space || !is_zygote) {
      main_mem_map_1 = MapAnonymousPreferredAddress(
          kMemMapSpaceName[0], request_begin, capacity_, &error_str);
    } else {
      DCHECK_EQ(heap_reservation.IsValid(), !boot_image_spaces_.empty());
      main_mem_map_1 = MemMap::MapAnonymous(
          kMemMapSpaceName[0],
          request_begin,
          capacity_,
          PROT_READ | PROT_WRITE,
                         true,
                       false,
          heap_reservation.IsValid() ? &heap_reservation : nullptr,
          &error_str);
    }
    CHECK(main_mem_map_1.IsValid()) << error_str;
    DCHECK(!heap_reservation.IsValid());
  }
  if (support_homogeneous_space_compaction ||
      background_collector_type_ == kCollectorTypeSS ||
      foreground_collector_type_ == kCollectorTypeSS) {
    ScopedTrace trace2("Create main mem map 2");
    main_mem_map_2 = MapAnonymousPreferredAddress(
        kMemMapSpaceName[1], main_mem_map_1.End(), capacity_, &error_str);
    CHECK(main_mem_map_2.IsValid()) << error_str;
  }
  if (separate_non_moving_space) {
    ScopedTrace trace2("Add non moving space");
    const size_t size = non_moving_space_mem_map.Size();
    const void* non_moving_space_mem_map_begin = non_moving_space_mem_map.Begin();
    non_moving_space_ = space::DlMallocSpace::CreateFromMemMap(std::move(non_moving_space_mem_map),
                                                               "zygote / non moving space",
                                                               kDefaultStartingSize,
                                                               initial_size,
                                                               size,
                                                               size,
                                                                                       false);
    CHECK(non_moving_space_ != nullptr) << "Failed creating non moving space "
        << non_moving_space_mem_map_begin;
    non_moving_space_->SetFootprintLimit(non_moving_space_->Capacity());
    AddSpace(non_moving_space_);
  }
  if (foreground_collector_type_ == kCollectorTypeCC) {
    CHECK(separate_non_moving_space);
    MemMap region_space_mem_map =
        space::RegionSpace::CreateMemMap(kRegionSpaceName, capacity_ * 2, request_begin);
    CHECK(region_space_mem_map.IsValid()) << "No region space mem map";
    region_space_ = space::RegionSpace::Create(
        kRegionSpaceName, std::move(region_space_mem_map), use_generational_cc_);
    AddSpace(region_space_);
  } else if (IsMovingGc(foreground_collector_type_)) {
    bump_pointer_space_ = space::BumpPointerSpace::CreateFromMemMap("Bump pointer space 1",
                                                                    std::move(main_mem_map_1));
    CHECK(bump_pointer_space_ != nullptr) << "Failed to create bump pointer space";
    AddSpace(bump_pointer_space_);
    if (foreground_collector_type_ != kCollectorTypeCMC) {
      temp_space_ = space::BumpPointerSpace::CreateFromMemMap("Bump pointer space 2",
                                                              std::move(main_mem_map_2));
      CHECK(temp_space_ != nullptr) << "Failed to create bump pointer space";
      AddSpace(temp_space_);
    }
    CHECK(separate_non_moving_space);
  } else {
    CreateMainMallocSpace(std::move(main_mem_map_1), initial_size, growth_limit_, capacity_);
    CHECK(main_space_ != nullptr);
    AddSpace(main_space_);
    if (!separate_non_moving_space) {
      non_moving_space_ = main_space_;
      CHECK(!non_moving_space_->CanMoveObjects());
    }
    if (main_mem_map_2.IsValid()) {
      const char* name = kUseRosAlloc ? kRosAllocSpaceName[1] : kDlMallocSpaceName[1];
      main_space_backup_.reset(CreateMallocSpaceFromMemMap(std::move(main_mem_map_2),
                                                           initial_size,
                                                           growth_limit_,
                                                           capacity_,
                                                           name,
                                                                                   true));
      CHECK(main_space_backup_.get() != nullptr);
      AddSpace(main_space_backup_.get());
    }
  }
  CHECK(non_moving_space_ != nullptr);
  CHECK(!non_moving_space_->CanMoveObjects());
  if (large_object_space_type == space::LargeObjectSpaceType::kFreeList) {
    large_object_space_ = space::FreeListSpace::Create("free list large object space", capacity_);
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
  UNUSED(heap_capacity);
  static constexpr size_t kMinHeapAddress = 4 * KB;
  card_table_.reset(accounting::CardTable::Create(reinterpret_cast<uint8_t*>(kMinHeapAddress),
                                                  4 * GB - kMinHeapAddress));
  CHECK(card_table_.get() != nullptr) << "Failed to create card table";
  if (foreground_collector_type_ == kCollectorTypeCC && kUseTableLookupReadBarrier) {
    rb_table_.reset(new accounting::ReadBarrierTable());
    DCHECK(rb_table_->IsAllCleared());
  }
  if (HasBootImageSpace()) {
    for (space::ImageSpace* image_space : GetBootImageSpaces()) {
      accounting::ModUnionTable* mod_union_table = new accounting::ModUnionTableToZygoteAllocspace(
          "Image mod-union table", this, image_space);
      CHECK(mod_union_table != nullptr) << "Failed to create image mod-union table";
      AddModUnionTable(mod_union_table);
    }
  }
  if (collector::SemiSpace::kUseRememberedSet && non_moving_space_ != main_space_) {
    accounting::RememberedSet* non_moving_space_rem_set =
        new accounting::RememberedSet("Non-moving space remembered set", this, non_moving_space_);
    CHECK(non_moving_space_rem_set != nullptr) << "Failed to create non-moving space remembered set";
    AddRememberedSet(non_moving_space_rem_set);
  }
  num_bytes_allocated_.store(0, std::memory_order_relaxed);
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
  thread_flip_lock_ = new Mutex("GC thread flip lock");
  thread_flip_cond_.reset(new ConditionVariable("GC thread flip condition variable",
                                                *thread_flip_lock_));
  task_processor_.reset(new TaskProcessor());
  reference_processor_.reset(new ReferenceProcessor());
  pending_task_lock_ = new Mutex("Pending task lock");
  if (ignore_target_footprint_) {
    SetIdealFootprint(std::numeric_limits<size_t>::max());
    concurrent_start_bytes_ = std::numeric_limits<size_t>::max();
  }
  CHECK_NE(target_footprint_.load(std::memory_order_relaxed), 0U);
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
    if (MayUseCollector(kCollectorTypeSS) ||
        MayUseCollector(kCollectorTypeHomogeneousSpaceCompact) ||
        use_homogeneous_space_compaction_for_oom_) {
      semi_space_collector_ = new collector::SemiSpace(this);
      garbage_collectors_.push_back(semi_space_collector_);
    }
    if (MayUseCollector(kCollectorTypeCMC)) {
      mark_compact_ = new collector::MarkCompact(this);
      garbage_collectors_.push_back(mark_compact_);
    }
    if (MayUseCollector(kCollectorTypeCC)) {
      concurrent_copying_collector_ = new collector::ConcurrentCopying(this,
                                                                                     false,
                                                                       use_generational_cc_,
                                                                       "",
                                                                       measure_gc_performance);
      if (use_generational_cc_) {
        young_concurrent_copying_collector_ = new collector::ConcurrentCopying(
            this,
                          true,
            use_generational_cc_,
            "young",
            measure_gc_performance);
      }
      active_concurrent_copying_collector_.store(concurrent_copying_collector_,
                                                 std::memory_order_relaxed);
      DCHECK(region_space_ != nullptr);
      concurrent_copying_collector_->SetRegionSpace(region_space_);
      if (use_generational_cc_) {
        young_concurrent_copying_collector_->SetRegionSpace(region_space_);
        DCHECK(non_moving_space_ != nullptr);
        concurrent_copying_collector_->CreateInterRegionRefBitmaps();
      }
      garbage_collectors_.push_back(concurrent_copying_collector_);
      if (use_generational_cc_) {
        garbage_collectors_.push_back(young_concurrent_copying_collector_);
      }
    }
  }
  if (!GetBootImageSpaces().empty() && non_moving_space_ != nullptr &&
      (is_zygote || separate_non_moving_space)) {
    space::ImageSpace* first_space = nullptr;
    for (space::ImageSpace* space : boot_image_spaces_) {
      if (first_space == nullptr || space->Begin() < first_space->Begin()) {
        first_space = space;
      }
    }
    bool no_gap = MemMap::CheckNoGaps(*first_space->GetMemMap(), *non_moving_space_->GetMemMap());
    if (!no_gap) {
      PrintFileToLog("/proc/self/maps", LogSeverity::ERROR);
      MemMap::DumpMaps(LOG_STREAM(ERROR), true);
      LOG(FATAL) << "There's a gap between the image space and the non-moving space";
    }
  }
  if (runtime->IsPerfettoJavaHeapStackProfEnabled()) {
    InitPerfettoJavaHeapProf();
  } else {
    GetHeapSampler().DisableHeapSampler();
  }
  instrumentation::Instrumentation* const instrumentation = runtime->GetInstrumentation();
  if (gc_stress_mode_) {
    backtrace_lock_ = new Mutex("GC complete lock");
  }
  if (is_running_on_memory_tool_ || gc_stress_mode_) {
    instrumentation->InstrumentQuickAllocEntryPoints();
  }
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "Heap() exiting";
  }
}
MemMap Heap::MapAnonymousPreferredAddress(const char* name,
                                          uint8_t* request_begin,
                                          size_t capacity,
                                          std::string* out_error_str) {
  while (true) {
    MemMap map = MemMap::MapAnonymous(name,
                                      request_begin,
                                      capacity,
                                      PROT_READ | PROT_WRITE,
                                                   true,
                                                 false,
                                                       nullptr,
                                      out_error_str);
    if (map.IsValid() || request_begin == nullptr) {
      return map;
    }
    request_begin = nullptr;
  }
}
bool Heap::MayUseCollector(CollectorType type) const {
  return foreground_collector_type_ == type || background_collector_type_ == type;
}
space::MallocSpace* Heap::CreateMallocSpaceFromMemMap(MemMap&& mem_map,
                                                      size_t initial_size,
                                                      size_t growth_limit,
                                                      size_t capacity,
                                                      const char* name,
                                                      bool can_move_objects) {
  space::MallocSpace* malloc_space = nullptr;
  if (kUseRosAlloc) {
    malloc_space = space::RosAllocSpace::CreateFromMemMap(std::move(mem_map),
                                                          name,
                                                          kDefaultStartingSize,
                                                          initial_size,
                                                          growth_limit,
                                                          capacity,
                                                          low_memory_mode_,
                                                          can_move_objects);
  } else {
    malloc_space = space::DlMallocSpace::CreateFromMemMap(std::move(mem_map),
                                                          name,
                                                          kDefaultStartingSize,
                                                          initial_size,
                                                          growth_limit,
                                                          capacity,
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
void Heap::CreateMainMallocSpace(MemMap&& mem_map,
                                 size_t initial_size,
                                 size_t growth_limit,
                                 size_t capacity) {
  bool can_move_objects = IsMovingGc(background_collector_type_) !=
      IsMovingGc(foreground_collector_type_) || use_homogeneous_space_compaction_for_oom_;
  if (kCompactZygote && Runtime::Current()->IsZygote() && !can_move_objects) {
    can_move_objects = !HasZygoteSpace();
  }
  if (collector::SemiSpace::kUseRememberedSet && main_space_ != nullptr) {
    RemoveRememberedSet(main_space_);
  }
  const char* name = kUseRosAlloc ? kRosAllocSpaceName[0] : kDlMallocSpaceName[0];
  main_space_ = CreateMallocSpaceFromMemMap(std::move(mem_map),
                                            initial_size,
                                            growth_limit,
                                            capacity, name,
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
bool Heap::IsCompilingBoot() const {
  if (!Runtime::Current()->IsAotCompiler()) {
    return false;
  }
  ScopedObjectAccess soa(Thread::Current());
  for (const auto& space : continuous_spaces_) {
    if (space->IsImageSpace() || space->IsZygoteSpace()) {
      return false;
    }
  }
  return true;
}
void Heap::IncrementDisableMovingGC(Thread* self) {
  ScopedThreadStateChange tsc(self, ThreadState::kWaitingForGcToComplete);
  MutexLock mu(self, *gc_complete_lock_);
  ++disable_moving_gc_count_;
  if (IsMovingGc(collector_type_running_)) {
    WaitForGcToCompleteLocked(kGcCauseDisableMovingGc, self);
  }
}
void Heap::DecrementDisableMovingGC(Thread* self) {
  MutexLock mu(self, *gc_complete_lock_);
  CHECK_GT(disable_moving_gc_count_, 0U);
  --disable_moving_gc_count_;
}
void Heap::IncrementDisableThreadFlip(Thread* self) {
  bool is_nested = self->GetDisableThreadFlipCount() > 0;
  self->IncrementDisableThreadFlipCount();
  if (is_nested) {
    return;
  }
  ScopedThreadStateChange tsc(self, ThreadState::kWaitingForGcThreadFlip);
  MutexLock mu(self, *thread_flip_lock_);
  thread_flip_cond_->CheckSafeToWait(self);
  bool has_waited = false;
  uint64_t wait_start = 0;
  if (thread_flip_running_) {
    wait_start = NanoTime();
    ScopedTrace trace("IncrementDisableThreadFlip");
    while (thread_flip_running_) {
      has_waited = true;
      thread_flip_cond_->Wait(self);
    }
  }
  ++disable_thread_flip_count_;
  if (has_waited) {
    uint64_t wait_time = NanoTime() - wait_start;
    total_wait_time_ += wait_time;
    if (wait_time > long_pause_log_threshold_) {
      LOG(INFO) << __FUNCTION__ << " blocked for " << PrettyDuration(wait_time);
    }
  }
}
void Heap::EnsureObjectUserfaulted(ObjPtr<mirror::Object> obj) {
  if (gUseUserfaultfd) {
    const uint8_t* start = reinterpret_cast<uint8_t*>(obj.Ptr());
    const uint8_t* end = AlignUp(start + obj->SizeOf(), kPageSize);
    start += kPageSize;
    while (start < end) {
      ForceRead(start);
      start += kPageSize;
    }
  }
}
void Heap::DecrementDisableThreadFlip(Thread* self) {
  self->DecrementDisableThreadFlipCount();
  bool is_outermost = self->GetDisableThreadFlipCount() == 0;
  if (!is_outermost) {
    return;
  }
  MutexLock mu(self, *thread_flip_lock_);
  CHECK_GT(disable_thread_flip_count_, 0U);
  --disable_thread_flip_count_;
  if (disable_thread_flip_count_ == 0) {
    thread_flip_cond_->Broadcast(self);
  }
}
void Heap::ThreadFlipBegin(Thread* self) {
  ScopedThreadStateChange tsc(self, ThreadState::kWaitingForGcThreadFlip);
  MutexLock mu(self, *thread_flip_lock_);
  thread_flip_cond_->CheckSafeToWait(self);
  bool has_waited = false;
  uint64_t wait_start = NanoTime();
  CHECK(!thread_flip_running_);
  thread_flip_running_ = true;
  while (disable_thread_flip_count_ > 0) {
    has_waited = true;
    thread_flip_cond_->Wait(self);
  }
  if (has_waited) {
    uint64_t wait_time = NanoTime() - wait_start;
    total_wait_time_ += wait_time;
    if (wait_time > long_pause_log_threshold_) {
      LOG(INFO) << __FUNCTION__ << " blocked for " << PrettyDuration(wait_time);
    }
  }
}
void Heap::ThreadFlipEnd(Thread* self) {
  MutexLock mu(self, *thread_flip_lock_);
  CHECK(thread_flip_running_);
  thread_flip_running_ = false;
  thread_flip_cond_->Broadcast(self);
}
void Heap::GrowHeapOnJankPerceptibleSwitch() {
  MutexLock mu(Thread::Current(), process_state_update_lock_);
  size_t orig_target_footprint = target_footprint_.load(std::memory_order_relaxed);
  if (orig_target_footprint < min_foreground_target_footprint_) {
    target_footprint_.compare_exchange_strong(orig_target_footprint,
                                              min_foreground_target_footprint_,
                                              std::memory_order_relaxed);
  }
  if (IsGcConcurrent() && concurrent_start_bytes_ < min_foreground_concurrent_start_bytes_) {
    concurrent_start_bytes_ = min_foreground_concurrent_start_bytes_;
  }
}
void Heap::UpdateProcessState(ProcessState old_process_state, ProcessState new_process_state) {
  if (old_process_state != new_process_state) {
    const bool jank_perceptible = new_process_state == kProcessStateJankPerceptible;
    if (jank_perceptible) {
      RequestCollectorTransition(foreground_collector_type_, 0);
      GrowHeapOnJankPerceptibleSwitch();
    } else {
      RequestCollectorTransition(background_collector_type_, 0);
    }
  }
}
void Heap::CreateThreadPool(size_t num_threads) {
  if (num_threads == 0) {
    num_threads = std::max(parallel_gc_threads_, conc_gc_threads_);
  }
  if (num_threads != 0) {
    thread_pool_.reset(new ThreadPool("Heap thread pool", num_threads));
  }
}
void Heap::WaitForWorkersToBeCreated() {
  DCHECK(!Runtime::Current()->IsShuttingDown(Thread::Current()))
      << "Cannot create new threads during runtime shutdown";
  if (thread_pool_ != nullptr) {
    thread_pool_->WaitForWorkersToBeCreated();
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
    if (live_bitmap != nullptr && !space->IsRegionSpace()) {
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
    if (live_bitmap != nullptr && !space->IsRegionSpace()) {
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
double Heap::CalculateGcWeightedAllocatedBytes(uint64_t gc_last_process_cpu_time_ns,
                                               uint64_t current_process_cpu_time) const {
  uint64_t bytes_allocated = GetBytesAllocated();
  double weight = current_process_cpu_time - gc_last_process_cpu_time_ns;
  return weight * bytes_allocated;
}
void Heap::CalculatePreGcWeightedAllocatedBytes() {
  uint64_t current_process_cpu_time = ProcessCpuNanoTime();
  pre_gc_weighted_allocated_bytes_ +=
    CalculateGcWeightedAllocatedBytes(pre_gc_last_process_cpu_time_ns_, current_process_cpu_time);
  pre_gc_last_process_cpu_time_ns_ = current_process_cpu_time;
}
void Heap::CalculatePostGcWeightedAllocatedBytes() {
  uint64_t current_process_cpu_time = ProcessCpuNanoTime();
  post_gc_weighted_allocated_bytes_ +=
    CalculateGcWeightedAllocatedBytes(post_gc_last_process_cpu_time_ns_, current_process_cpu_time);
  post_gc_last_process_cpu_time_ns_ = current_process_cpu_time;
}
uint64_t Heap::GetTotalGcCpuTime() {
  uint64_t sum = 0;
  for (auto* collector : garbage_collectors_) {
    sum += collector->GetTotalCpuTime();
  }
  return sum;
}
void Heap::DumpGcPerformanceInfo(std::ostream& os) {
  os << "Dumping cumulative Gc timings\n";
  uint64_t total_duration = 0;
  uint64_t total_paused_time = 0;
  for (auto* collector : garbage_collectors_) {
    total_duration += collector->GetCumulativeTimings().GetTotalNs();
    total_paused_time += collector->GetTotalPausedTimeNs();
    collector->DumpPerformanceInfo(os);
  }
  if (total_duration != 0) {
    const double total_seconds = total_duration / 1.0e9;
    const double total_cpu_seconds = GetTotalGcCpuTime() / 1.0e9;
    os << "Total time spent in GC: " << PrettyDuration(total_duration) << "\n";
    os << "Mean GC size throughput: "
       << PrettySize(GetBytesFreedEver() / total_seconds) << "/s"
       << " per cpu-time: "
       << PrettySize(GetBytesFreedEver() / total_cpu_seconds) << "/s\n";
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
  if (HasZygoteSpace()) {
    os << "Zygote space size " << PrettySize(zygote_space_->Size()) << "\n";
  }
  os << "Total mutator paused time: " << PrettyDuration(total_paused_time) << "\n";
  os << "Total time waiting for GC to complete: " << PrettyDuration(total_wait_time_) << "\n";
  os << "Total GC count: " << GetGcCount() << "\n";
  os << "Total GC time: " << PrettyDuration(GetGcTime()) << "\n";
  os << "Total blocking GC count: " << GetBlockingGcCount() << "\n";
  os << "Total blocking GC time: " << PrettyDuration(GetBlockingGcTime()) << "\n";
  os << "Total pre-OOME GC count: " << GetPreOomeGcCount() << "\n";
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
  if (kDumpRosAllocStatsOnSigQuit && rosalloc_space_ != nullptr) {
    rosalloc_space_->DumpStats(os);
  }
  os << "Native bytes total: " << GetNativeBytes()
     << " registered: " << native_bytes_registered_.load(std::memory_order_relaxed) << "\n";
  os << "Total native bytes at last GC: "
     << old_native_bytes_allocated_.load(std::memory_order_relaxed) << "\n";
  BaseMutex::DumpAll(os);
}
void Heap::ResetGcPerformanceInfo() {
  for (auto* collector : garbage_collectors_) {
    collector->ResetMeasurements();
  }
  process_cpu_start_time_ns_ = ProcessCpuNanoTime();
  pre_gc_last_process_cpu_time_ns_ = process_cpu_start_time_ns_;
  pre_gc_weighted_allocated_bytes_ = 0u;
  post_gc_last_process_cpu_time_ns_ = process_cpu_start_time_ns_;
  post_gc_weighted_allocated_bytes_ = 0u;
  total_bytes_freed_ever_.store(0);
  total_objects_freed_ever_.store(0);
  total_wait_time_ = 0;
  blocking_gc_count_ = 0;
  blocking_gc_time_ = 0;
  pre_oome_gc_count_.store(0, std::memory_order_relaxed);
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
  for (auto* collector : garbage_collectors_) {
    gc_count += collector->GetCumulativeTimings().GetIterations();
  }
  return gc_count;
}
uint64_t Heap::GetGcTime() const {
  uint64_t gc_time = 0U;
  for (auto* collector : garbage_collectors_) {
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
uint64_t Heap::GetPreOomeGcCount() const {
  return pre_oome_gc_count_.load(std::memory_order_relaxed);
}
ALWAYS_INLINE
static inline AllocationListener* GetAndOverwriteAllocationListener(
    Atomic<AllocationListener*>* storage, AllocationListener* new_value) {
  return storage->exchange(new_value);
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
  delete thread_flip_lock_;
  delete pending_task_lock_;
  delete backtrace_lock_;
  uint64_t unique_count = unique_backtrace_count_.load();
  uint64_t seen_count = seen_backtrace_count_.load();
  if (unique_count != 0 || seen_count != 0) {
    LOG(INFO) << "gc stress unique=" << unique_count << " total=" << (unique_count + seen_count);
  }
  VLOG(heap) << "Finished ~Heap()";
}
space::ContinuousSpace* Heap::FindContinuousSpaceFromAddress(const mirror::Object* addr) const {
  for (const auto& space : continuous_spaces_) {
    if (space->Contains(addr)) {
      return space;
    }
  }
  return nullptr;
}
space::ContinuousSpace* Heap::FindContinuousSpaceFromObject(ObjPtr<mirror::Object> obj,
                                                            bool fail_ok) const {
  space::ContinuousSpace* space = FindContinuousSpaceFromAddress(obj.Ptr());
  if (space != nullptr) {
    return space;
  }
  if (!fail_ok) {
    LOG(FATAL) << "object " << obj << " not inside any spaces!";
  }
  return nullptr;
}
space::DiscontinuousSpace* Heap::FindDiscontinuousSpaceFromObject(ObjPtr<mirror::Object> obj,
                                                                  bool fail_ok) const {
  for (const auto& space : discontinuous_spaces_) {
    if (space->Contains(obj.Ptr())) {
      return space;
    }
  }
  if (!fail_ok) {
    LOG(FATAL) << "object " << obj << " not inside any spaces!";
  }
  return nullptr;
}
space::Space* Heap::FindSpaceFromObject(ObjPtr<mirror::Object> obj, bool fail_ok) const {
  space::Space* result = FindContinuousSpaceFromObject(obj, true);
  if (result != nullptr) {
    return result;
  }
  return FindDiscontinuousSpaceFromObject(obj, fail_ok);
}
space::Space* Heap::FindSpaceFromAddress(const void* addr) const {
  for (const auto& space : continuous_spaces_) {
    if (space->Contains(reinterpret_cast<const mirror::Object*>(addr))) {
      return space;
    }
  }
  for (const auto& space : discontinuous_spaces_) {
    if (space->Contains(reinterpret_cast<const mirror::Object*>(addr))) {
      return space;
    }
  }
  return nullptr;
}
std::string Heap::DumpSpaceNameFromAddress(const void* addr) const {
  space::Space* space = FindSpaceFromAddress(addr);
  return (space != nullptr) ? space->GetName() : "no space";
}
void Heap::ThrowOutOfMemoryError(Thread* self, size_t byte_count, AllocatorType allocator_type) {
  if (self->IsHandlingStackOverflow()) {
    self->SetException(
        Runtime::Current()->GetPreAllocatedOutOfMemoryErrorWhenHandlingStackOverflow());
    return;
  }
  Runtime::Current()->OutOfMemoryErrorHook();
  std::ostringstream oss;
  size_t total_bytes_free = GetFreeMemory();
  oss << "Failed to allocate a " << byte_count << " byte allocation with " << total_bytes_free
      << " free bytes and " << PrettySize(GetFreeMemoryUntilOOME()) << " until OOM,"
      << " target footprint " << target_footprint_.load(std::memory_order_relaxed)
      << ", growth limit "
      << growth_limit_;
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
    if (allocator_type != kAllocatorTypeLOS) {
      CHECK(space != nullptr) << "allocator_type:" << allocator_type
                              << " byte_count:" << byte_count
                              << " total_bytes_free:" << total_bytes_free;
      if (!space->LogFragmentationAllocFailure(oss, byte_count)) {
        oss << "; giving up on allocation because <"
            << kMinFreeHeapAfterGcForAlloc * 100
            << "% of heap free after GC.";
      }
    }
  }
  self->ThrowOutOfMemoryError(oss.str().c_str());
}
void Heap::DoPendingCollectorTransition() {
  CollectorType desired_collector_type = desired_collector_type_;
  if (collector_type_ == kCollectorTypeCC || collector_type_ == kCollectorTypeCMC) {
    size_t num_bytes_allocated_since_gc =
        UnsignedDifference(GetBytesAllocated(), num_bytes_alive_after_gc_);
    if (num_bytes_allocated_since_gc <
        (UnsignedDifference(target_footprint_.load(std::memory_order_relaxed),
                            num_bytes_alive_after_gc_)/4)
        && !kStressCollectorTransition
        && !IsLowMemoryMode()) {
      return;
    }
  }
  if (desired_collector_type == kCollectorTypeHomogeneousSpaceCompact) {
    if (!CareAboutPauseTimes()) {
      PerformHomogeneousSpaceCompact();
    } else {
      VLOG(gc) << "Homogeneous compaction ignored due to jank perceptible process state";
    }
  } else if (desired_collector_type == kCollectorTypeCCBackground ||
             desired_collector_type == kCollectorTypeCMC) {
    if (!CareAboutPauseTimes()) {
      CollectGarbageInternal(collector::kGcTypeFull,
                             kGcCauseCollectorTransition,
                                                       false, GetCurrentGcNum() + 1);
    } else {
      VLOG(gc) << "background compaction ignored due to jank perceptible process state";
    }
  } else {
    CHECK_EQ(desired_collector_type, collector_type_) << "Unsupported collector transition";
  }
}
void Heap::Trim(Thread* self) {
  Runtime* const runtime = Runtime::Current();
  if (!CareAboutPauseTimes()) {
    ScopedTrace trace("Deflating monitors");
    ScopedGCCriticalSection gcs(self, kGcCauseTrim, kCollectorTypeHeapTrim);
    ScopedSuspendAll ssa(__FUNCTION__);
    uint64_t start_time = NanoTime();
    size_t count = runtime->GetMonitorList()->DeflateMonitors();
    VLOG(heap) << "Deflating " << count << " monitors took "
        << PrettyDuration(NanoTime() - start_time);
  }
  TrimIndirectReferenceTables(self);
  TrimSpaces(self);
  runtime->GetArenaPool()->TrimMaps();
}
class TrimIndirectReferenceTableClosure : public Closure {
 public:
  explicit TrimIndirectReferenceTableClosure(Barrier* barrier) : barrier_(barrier) {
  }
  void Run(Thread* thread) override NO_THREAD_SAFETY_ANALYSIS {
    thread->GetJniEnv()->TrimLocals();
    barrier_->Pass(Thread::Current());
  }
 private:
  Barrier* const barrier_;
};
void Heap::TrimIndirectReferenceTables(Thread* self) {
  ScopedObjectAccess soa(self);
  ScopedTrace trace(__PRETTY_FUNCTION__);
  JavaVMExt* vm = soa.Vm();
  vm->TrimGlobals();
  Barrier barrier(0);
  TrimIndirectReferenceTableClosure closure(&barrier);
  ScopedThreadStateChange tsc(self, ThreadState::kWaitingForCheckPointsToRun);
  size_t barrier_count = Runtime::Current()->GetThreadList()->RunCheckpoint(&closure);
  if (barrier_count != 0) {
    barrier.Increment(self, barrier_count);
  }
}
void Heap::StartGC(Thread* self, GcCause cause, CollectorType collector_type) {
  ScopedThreadStateChange tsc(self, ThreadState::kWaitingForGcToComplete);
  MutexLock mu(self, *gc_complete_lock_);
  WaitForGcToCompleteLocked(cause, self);
  collector_type_running_ = collector_type;
  last_gc_cause_ = cause;
  thread_running_gc_ = self;
}
void Heap::TrimSpaces(Thread* self) {
  StartGC(self, kGcCauseTrim, kCollectorTypeHeapTrim);
  ScopedTrace trace(__PRETTY_FUNCTION__);
  const uint64_t start_ns = NanoTime();
  uint64_t total_alloc_space_allocated = 0;
  uint64_t total_alloc_space_size = 0;
  uint64_t managed_reclaimed = 0;
  {
    ScopedObjectAccess soa(self);
    for (const auto& space : continuous_spaces_) {
      if (space->IsMallocSpace()) {
        gc::space::MallocSpace* malloc_space = space->AsMallocSpace();
        if (malloc_space->IsRosAllocSpace() || !CareAboutPauseTimes()) {
          managed_reclaimed += malloc_space->Trim();
        }
        total_alloc_space_size += malloc_space->Size();
      }
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
  VLOG(heap) << "Heap trim of managed (duration=" << PrettyDuration(gc_heap_end_ns - start_ns)
      << ", advised=" << PrettySize(managed_reclaimed) << ") heap. Managed heap utilization of "
      << static_cast<int>(100 * managed_utilization) << "%.";
}
bool Heap::IsValidObjectAddress(const void* addr) const {
  if (addr == nullptr) {
    return true;
  }
  return IsAligned<kObjectAlignment>(addr) && FindSpaceFromAddress(addr) != nullptr;
}
bool Heap::IsNonDiscontinuousSpaceHeapAddress(const void* addr) const {
  return FindContinuousSpaceFromAddress(reinterpret_cast<const mirror::Object*>(addr)) != nullptr;
}
bool Heap::IsLiveObjectLocked(ObjPtr<mirror::Object> obj,
                              bool search_allocation_stack,
                              bool search_live_stack,
                              bool sorted) {
  if (UNLIKELY(!IsAligned<kObjectAlignment>(obj.Ptr()))) {
    return false;
  }
  if (bump_pointer_space_ != nullptr && bump_pointer_space_->HasAddress(obj.Ptr())) {
    mirror::Class* klass = obj->GetClass<kVerifyNone>();
    if (obj == klass) {
      return true;
    }
    return VerifyClassClass(klass) && IsLiveObjectLocked(klass);
  } else if (temp_space_ != nullptr && temp_space_->HasAddress(obj.Ptr())) {
    return temp_space_->Contains(obj.Ptr());
  }
  if (region_space_ != nullptr && region_space_->HasAddress(obj.Ptr())) {
    return true;
  }
  space::ContinuousSpace* c_space = FindContinuousSpaceFromObject(obj, true);
  space::DiscontinuousSpace* d_space = nullptr;
  if (c_space != nullptr) {
    if (c_space->GetLiveBitmap()->Test(obj.Ptr())) {
      return true;
    }
  } else {
    d_space = FindDiscontinuousSpaceFromObject(obj, true);
    if (d_space != nullptr) {
      if (d_space->GetLiveBitmap()->Test(obj.Ptr())) {
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
        if (allocation_stack_->ContainsSorted(obj.Ptr())) {
          return true;
        }
      } else if (allocation_stack_->Contains(obj.Ptr())) {
        return true;
      }
    }
    if (search_live_stack) {
      if (sorted) {
        if (live_stack_->ContainsSorted(obj.Ptr())) {
          return true;
        }
      } else if (live_stack_->Contains(obj.Ptr())) {
        return true;
      }
    }
  }
  if (c_space != nullptr) {
    if (c_space->GetLiveBitmap()->Test(obj.Ptr())) {
      return true;
    }
  } else {
    d_space = FindDiscontinuousSpaceFromObject(obj, true);
    if (d_space != nullptr && d_space->GetLiveBitmap()->Test(obj.Ptr())) {
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
void Heap::VerifyObjectBody(ObjPtr<mirror::Object> obj) {
  if (verify_object_mode_ == kVerifyObjectModeDisabled) {
    return;
  }
  if (UNLIKELY(num_bytes_allocated_.load(std::memory_order_relaxed) < 10 * KB)) {
    return;
  }
  CHECK_ALIGNED(obj.Ptr(), kObjectAlignment) << "Object isn't aligned";
  mirror::Class* c = obj->GetFieldObject<mirror::Class, kVerifyNone>(mirror::Object::ClassOffset());
  CHECK(c != nullptr) << "Null class in object " << obj;
  CHECK_ALIGNED(c, kObjectAlignment) << "Class " << c << " not aligned in object " << obj;
  CHECK(VerifyClassClass(c));
  if (verify_object_mode_ > kVerifyObjectModeFast) {
    CHECK(IsLiveObjectLocked(obj)) << "Object is dead " << obj << "\n" << DumpSpaces();
  }
}
void Heap::VerifyHeap() {
  ReaderMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  auto visitor = [&](mirror::Object* obj) NO_THREAD_SAFETY_ANALYSIS {
    VerifyObjectBody(obj);
  };
  auto no_thread_safety_analysis = [&]() NO_THREAD_SAFETY_ANALYSIS {
    GetLiveBitmap()->Visit(visitor);
  };
  no_thread_safety_analysis();
}
void Heap::RecordFree(uint64_t freed_objects, int64_t freed_bytes) {
  RACING_DCHECK_LE(freed_bytes,
                   static_cast<int64_t>(num_bytes_allocated_.load(std::memory_order_relaxed)));
  num_bytes_allocated_.fetch_sub(static_cast<ssize_t>(freed_bytes), std::memory_order_relaxed);
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
  size_t bytes_freed = num_bytes_freed_revoke_.load(std::memory_order_relaxed);
  CHECK_GE(num_bytes_freed_revoke_.fetch_sub(bytes_freed, std::memory_order_relaxed),
           bytes_freed) << "num_bytes_freed_revoke_ underflow";
  CHECK_GE(num_bytes_allocated_.fetch_sub(bytes_freed, std::memory_order_relaxed),
           bytes_freed) << "num_bytes_allocated_ underflow";
  GetCurrentGcIteration()->SetFreedRevoke(bytes_freed);
}
space::RosAllocSpace* Heap::GetRosAllocSpace(gc::allocator::RosAlloc* rosalloc) const {
  if (rosalloc_space_ != nullptr && rosalloc_space_->GetRosAlloc() == rosalloc) {
    return rosalloc_space_;
  }
  for (const auto& space : continuous_spaces_) {
    if (space->AsContinuousSpace()->IsRosAllocSpace()) {
      if (space->AsContinuousSpace()->AsRosAllocSpace()->GetRosAlloc() == rosalloc) {
        return space->AsContinuousSpace()->AsRosAllocSpace();
      }
    }
  }
  return nullptr;
}
static inline bool EntrypointsInstrumented() REQUIRES_SHARED(Locks::mutator_lock_) {
  instrumentation::Instrumentation* const instrumentation =
      Runtime::Current()->GetInstrumentation();
  return instrumentation != nullptr && instrumentation->AllocEntrypointsInstrumented();
}
mirror::Object* Heap::AllocateInternalWithGc(Thread* self,
                                             AllocatorType allocator,
                                             bool instrumented,
                                             size_t alloc_size,
                                             size_t* bytes_allocated,
                                             size_t* usable_size,
                                             size_t* bytes_tl_bulk_allocated,
                                             ObjPtr<mirror::Class>* klass) {
  bool was_default_allocator = allocator == GetCurrentAllocator();
  self->AssertNoPendingException();
  DCHECK(klass != nullptr);
  StackHandleScope<1> hs(self);
  HandleWrapperObjPtr<mirror::Class> h_klass(hs.NewHandleWrapper(klass));
  auto send_object_pre_alloc =
      [&]() REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!Roles::uninterruptible_) {
        if (UNLIKELY(instrumented)) {
          AllocationListener* l = alloc_listener_.load(std::memory_order_seq_cst);
          if (UNLIKELY(l != nullptr) && UNLIKELY(l->HasPreAlloc())) {
            l->PreObjectAllocated(self, h_klass, &alloc_size);
          }
        }
      };
#define PERFORM_SUSPENDING_OPERATION(op) \
  [&]() REQUIRES(Roles::uninterruptible_) REQUIRES_SHARED(Locks::mutator_lock_) { \
    ScopedAllowThreadSuspension ats; \
    auto res = (op); \
    send_object_pre_alloc(); \
    return res; \
  }()
  collector::GcType last_gc =
      PERFORM_SUSPENDING_OPERATION(WaitForGcToComplete(kGcCauseForAlloc, self));
  if ((was_default_allocator && allocator != GetCurrentAllocator()) ||
      (!instrumented && EntrypointsInstrumented())) {
    return nullptr;
  }
  uint32_t starting_gc_num = GetCurrentGcNum();
  if (last_gc != collector::kGcTypeNone) {
    mirror::Object* ptr = TryToAllocate<true, false>(self, allocator, alloc_size, bytes_allocated,
                                                     usable_size, bytes_tl_bulk_allocated);
    if (ptr != nullptr) {
      return ptr;
    }
  }
  auto have_reclaimed_enough = [&]() {
    size_t curr_bytes_allocated = GetBytesAllocated();
    double curr_free_heap =
        static_cast<double>(growth_limit_ - curr_bytes_allocated) / growth_limit_;
    return curr_free_heap >= kMinFreeHeapAfterGcForAlloc;
  };
  collector::GcType tried_type = next_gc_type_;
  if (last_gc < tried_type) {
    const bool gc_ran = PERFORM_SUSPENDING_OPERATION(
        CollectGarbageInternal(tried_type, kGcCauseForAlloc, false, starting_gc_num + 1)
        != collector::kGcTypeNone);
    if ((was_default_allocator && allocator != GetCurrentAllocator()) ||
        (!instrumented && EntrypointsInstrumented())) {
      return nullptr;
    }
    if (gc_ran && have_reclaimed_enough()) {
      mirror::Object* ptr = TryToAllocate<true, false>(self, allocator,
                                                       alloc_size, bytes_allocated,
                                                       usable_size, bytes_tl_bulk_allocated);
      if (ptr != nullptr) {
        return ptr;
      }
    }
  }
  VLOG(gc) << "Forcing collection of SoftReferences for " << PrettySize(alloc_size)
           << " allocation";
  DCHECK(!gc_plan_.empty());
  pre_oome_gc_count_.fetch_add(1, std::memory_order_relaxed);
  PERFORM_SUSPENDING_OPERATION(
      CollectGarbageInternal(gc_plan_.back(), kGcCauseForAlloc, true, GC_NUM_ANY));
  if ((was_default_allocator && allocator != GetCurrentAllocator()) ||
      (!instrumented && EntrypointsInstrumented())) {
    return nullptr;
  }
  mirror::Object* ptr = nullptr;
  if (have_reclaimed_enough()) {
    ptr = TryToAllocate<true, true>(self, allocator, alloc_size, bytes_allocated,
                                    usable_size, bytes_tl_bulk_allocated);
  }
  if (ptr == nullptr) {
    const uint64_t current_time = NanoTime();
    switch (allocator) {
      case kAllocatorTypeRosAlloc:
      case kAllocatorTypeDlMalloc: {
        if (use_homogeneous_space_compaction_for_oom_ &&
            current_time - last_time_homogeneous_space_compaction_by_oom_ >
            min_interval_homogeneous_space_compaction_by_oom_) {
          last_time_homogeneous_space_compaction_by_oom_ = current_time;
          HomogeneousSpaceCompactResult result =
              PERFORM_SUSPENDING_OPERATION(PerformHomogeneousSpaceCompact());
          if ((was_default_allocator && allocator != GetCurrentAllocator()) ||
              (!instrumented && EntrypointsInstrumented())) {
            return nullptr;
          }
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
                    << count_requested_homogeneous_space_compaction_.load()
                    << " performed defragmentation "
                    << count_performed_homogeneous_space_compaction_.load()
                    << " ignored homogeneous space compaction "
                    << count_ignored_homogeneous_space_compaction_.load()
                    << " delayed count = "
                    << count_delayed_oom_.load();
        }
        break;
      }
      default: {
      }
    }
  }
#undef PERFORM_SUSPENDING_OPERATION
  if (ptr == nullptr) {
    ScopedAllowThreadSuspension ats;
    ThrowOutOfMemoryError(self, alloc_size, allocator);
  }
  return ptr;
}
void Heap::SetTargetHeapUtilization(float target) {
  DCHECK_GT(target, 0.1f);
  DCHECK_LT(target, 1.0f);
  target_utilization_ = target;
}
size_t Heap::GetObjectsAllocated() const {
  Thread* const self = Thread::Current();
  ScopedThreadStateChange tsc(self, ThreadState::kWaitingForGetObjectsAllocated);
  gc::ScopedGCCriticalSection gcs(Thread::Current(),
                                  gc::kGcCauseGetObjectsAllocated,
                                  gc::kCollectorTypeGetObjectsAllocated);
  ScopedSuspendAll ssa(__FUNCTION__);
  ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
  size_t total = 0;
  for (space::AllocSpace* space : alloc_spaces_) {
    total += space->GetObjectsAllocated();
  }
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
  static std::atomic<uint64_t> max_bytes_so_far(0);
  uint64_t so_far = max_bytes_so_far.load(std::memory_order_relaxed);
  uint64_t current_bytes = GetBytesFreedEver(std::memory_order_acquire);
  current_bytes += GetBytesAllocated();
  do {
    if (current_bytes <= so_far) {
      return so_far;
    }
  } while (!max_bytes_so_far.compare_exchange_weak(so_far ,
                                                   current_bytes, std::memory_order_relaxed));
  return current_bytes;
}
static bool MatchesClass(mirror::Object* obj,
                         Handle<mirror::Class> h_class,
                         bool use_is_assignable_from) REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Class* instance_class = obj->GetClass();
  CHECK(instance_class != nullptr);
  ObjPtr<mirror::Class> klass = h_class.Get();
  if (use_is_assignable_from) {
    return klass != nullptr && klass->IsAssignableFrom(instance_class);
  }
  return instance_class == klass;
}
void Heap::CountInstances(const std::vector<Handle<mirror::Class>>& classes,
                          bool use_is_assignable_from,
                          uint64_t* counts) {
  auto instance_counter = [&](mirror::Object* obj) REQUIRES_SHARED(Locks::mutator_lock_) {
    for (size_t i = 0; i < classes.size(); ++i) {
      if (MatchesClass(obj, classes[i], use_is_assignable_from)) {
        ++counts[i];
      }
    }
  };
  VisitObjects(instance_counter);
}
void Heap::CollectGarbage(bool clear_soft_references, GcCause cause) {
  CollectGarbageInternal(gc_plan_.back(), cause, clear_soft_references, GC_NUM_ANY);
}
bool Heap::SupportHomogeneousSpaceCompactAndCollectorTransitions() const {
  return main_space_backup_.get() != nullptr && main_space_ != nullptr &&
      foreground_collector_type_ == kCollectorTypeCMS;
}
HomogeneousSpaceCompactResult Heap::PerformHomogeneousSpaceCompact() {
  Thread* self = Thread::Current();
  count_requested_homogeneous_space_compaction_++;
  ScopedThreadStateChange tsc(self, ThreadState::kWaitingPerformingGc);
  Locks::mutator_lock_->AssertNotHeld(self);
  {
    ScopedThreadStateChange tsc2(self, ThreadState::kWaitingForGcToComplete);
    MutexLock mu(self, *gc_complete_lock_);
    WaitForGcToCompleteLocked(kGcCauseHomogeneousSpaceCompact, self);
    if (disable_moving_gc_count_ != 0 || IsMovingGc(collector_type_) ||
        !main_space_->CanMoveObjects()) {
      return kErrorReject;
    }
    if (!SupportHomogeneousSpaceCompactAndCollectorTransitions()) {
      return kErrorUnsupported;
    }
    collector_type_running_ = kCollectorTypeHomogeneousSpaceCompact;
  }
  if (Runtime::Current()->IsShuttingDown(self)) {
    FinishGC(self, collector::kGcTypeNone);
    return HomogeneousSpaceCompactResult::kErrorVMShuttingDown;
  }
  collector::GarbageCollector* collector;
  {
    ScopedSuspendAll ssa(__FUNCTION__);
    uint64_t start_time = NanoTime();
    space::MallocSpace* to_space = main_space_backup_.release();
    space::MallocSpace* from_space = main_space_;
    to_space->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
    const uint64_t space_size_before_compaction = from_space->Size();
    AddSpace(to_space);
    CHECK_GE(to_space->GetFootprintLimit(), from_space->GetFootprintLimit());
    collector = Compact(to_space, from_space, kGcCauseHomogeneousSpaceCompact);
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
  }
  SelfDeletingTask* clear = reference_processor_->CollectClearedReferences(self);
  GrowForUtilization(semi_space_collector_);
  LogGC(kGcCauseHomogeneousSpaceCompact, collector);
  FinishGC(self, collector::kGcTypeFull);
  clear->Run(self);
  clear->Finalize();
  {
    ScopedObjectAccess soa(self);
    soa.Vm()->UnloadNativeLibraries();
  }
  return HomogeneousSpaceCompactResult::kSuccess;
}
void Heap::SetDefaultConcurrentStartBytes() {
  MutexLock mu(Thread::Current(), *gc_complete_lock_);
  if (collector_type_running_ != kCollectorTypeNone) {
    return;
  }
  SetDefaultConcurrentStartBytesLocked();
}
void Heap::SetDefaultConcurrentStartBytesLocked() {
  if (IsGcConcurrent()) {
    size_t target_footprint = target_footprint_.load(std::memory_order_relaxed);
    size_t reserve_bytes = target_footprint / 4;
    reserve_bytes = std::min(reserve_bytes, kMaxConcurrentRemainingBytes);
    reserve_bytes = std::max(reserve_bytes, kMinConcurrentRemainingBytes);
    concurrent_start_bytes_ = UnsignedDifference(target_footprint, reserve_bytes);
  } else {
    concurrent_start_bytes_ = std::numeric_limits<size_t>::max();
  }
}
void Heap::ChangeCollector(CollectorType collector_type) {
  if (collector_type != collector_type_) {
    collector_type_ = collector_type;
    gc_plan_.clear();
    switch (collector_type_) {
      case kCollectorTypeCC: {
        if (use_generational_cc_) {
          gc_plan_.push_back(collector::kGcTypeSticky);
        }
        gc_plan_.push_back(collector::kGcTypeFull);
        if (use_tlab_) {
          ChangeAllocator(kAllocatorTypeRegionTLAB);
        } else {
          ChangeAllocator(kAllocatorTypeRegion);
        }
        break;
      }
      case kCollectorTypeCMC: {
        gc_plan_.push_back(collector::kGcTypeFull);
        if (use_tlab_) {
          ChangeAllocator(kAllocatorTypeTLAB);
        } else {
          ChangeAllocator(kAllocatorTypeBumpPointer);
        }
        break;
      }
      case kCollectorTypeSS: {
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
    SetDefaultConcurrentStartBytesLocked();
  }
}
class ZygoteCompactingCollector final : public collector::SemiSpace {
 public:
  ZygoteCompactingCollector(gc::Heap* heap, bool is_running_on_memory_tool)
      : SemiSpace(heap, "zygote collector"),
        bin_live_bitmap_(nullptr),
        bin_mark_bitmap_(nullptr),
        is_running_on_memory_tool_(is_running_on_memory_tool) {}
  void BuildBins(space::ContinuousSpace* space) REQUIRES_SHARED(Locks::mutator_lock_) {
    bin_live_bitmap_ = space->GetLiveBitmap();
    bin_mark_bitmap_ = space->GetMarkBitmap();
    uintptr_t prev = reinterpret_cast<uintptr_t>(space->Begin());
    WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
    auto visitor = [&](mirror::Object* obj) REQUIRES_SHARED(Locks::mutator_lock_) {
      uintptr_t object_addr = reinterpret_cast<uintptr_t>(obj);
      size_t bin_size = object_addr - prev;
      AddBin(bin_size, prev);
      prev = object_addr + RoundUp(obj->SizeOf<kDefaultVerifyFlags>(), kObjectAlignment);
    };
    bin_live_bitmap_->Walk(visitor);
    AddBin(reinterpret_cast<uintptr_t>(space->End()) - prev, prev);
  }
 private:
  std::multimap<size_t, uintptr_t> bins_;
  accounting::ContinuousSpaceBitmap* bin_live_bitmap_;
  accounting::ContinuousSpaceBitmap* bin_mark_bitmap_;
  const bool is_running_on_memory_tool_;
  void AddBin(size_t size, uintptr_t position) {
    if (is_running_on_memory_tool_) {
      MEMORY_TOOL_MAKE_DEFINED(reinterpret_cast<void*>(position), size);
    }
    if (size != 0) {
      bins_.insert(std::make_pair(size, position));
    }
  }
  bool ShouldSweepSpace(space::ContinuousSpace* space ATTRIBUTE_UNUSED) const override {
    return false;
  }
  mirror::Object* MarkNonForwardedObject(mirror::Object* obj) override
      REQUIRES(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
    size_t obj_size = obj->SizeOf<kDefaultVerifyFlags>();
    size_t alloc_size = RoundUp(obj_size, kObjectAlignment);
    mirror::Object* forward_address;
    auto it = bins_.lower_bound(alloc_size);
    if (it == bins_.end()) {
      size_t bytes_allocated, unused_bytes_tl_bulk_allocated;
      forward_address = to_space_->Alloc(
          self_, alloc_size, &bytes_allocated, nullptr, &unused_bytes_tl_bulk_allocated);
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
    if (kUseBakerReadBarrier) {
      obj->AssertReadBarrierState();
      forward_address->AssertReadBarrierState();
    }
    return forward_address;
  }
};
void Heap::UnBindBitmaps() {
  TimingLogger::ScopedTiming t("UnBindBitmaps", GetCurrentGcIteration()->GetTimings());
  for (const auto& space : GetContinuousSpaces()) {
    if (space->IsContinuousMemMapAllocSpace()) {
      space::ContinuousMemMapAllocSpace* alloc_space = space->AsContinuousMemMapAllocSpace();
      if (alloc_space->GetLiveBitmap() != nullptr && alloc_space->HasBoundBitmaps()) {
        alloc_space->UnBindBitmaps();
      }
    }
  }
}
void Heap::IncrementFreedEver() {
  total_objects_freed_ever_.store(total_objects_freed_ever_.load(std::memory_order_relaxed)
                                  + GetCurrentGcIteration()->GetFreedObjects()
                                  + GetCurrentGcIteration()->GetFreedLargeObjects(),
                                  std::memory_order_release);
  total_bytes_freed_ever_.store(total_bytes_freed_ever_.load(std::memory_order_relaxed)
                                + GetCurrentGcIteration()->GetFreedBytes()
                                + GetCurrentGcIteration()->GetFreedLargeObjectBytes(),
                                std::memory_order_release);
}
#pragma clang diagnostic push
#if !ART_USE_FUTEXES
#pragma clang diagnostic ignored "-Wframe-larger-than="
#endif
#pragma clang diagnostic ignored "-Wframe-larger-than="
void Heap::PreZygoteFork() {
  if (!HasZygoteSpace()) {
    CollectGarbageInternal(collector::kGcTypeFull, kGcCauseBackground, false, GC_NUM_ANY);
    non_moving_space_->Trim();
  }
  Thread* self = Thread::Current();
  MutexLock mu(self, zygote_creation_lock_);
  if (HasZygoteSpace()) {
    return;
  }
  Runtime* runtime = Runtime::Current();
  runtime->GetInternTable()->AddNewTable();
  runtime->GetClassLinker()->MoveClassTableToPreZygote();
  runtime->SetupLinearAllocForPostZygoteFork(self);
  VLOG(heap) << "Starting PreZygoteFork";
  non_moving_space_->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
  const bool same_space = non_moving_space_ == main_space_;
  if (kCompactZygote) {
    ScopedDisableRosAllocVerification disable_rosalloc_verif(this);
    ZygoteCompactingCollector zygote_collector(this, is_running_on_memory_tool_);
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
      MemMap mem_map = main_space_->ReleaseMemMap();
      RemoveSpace(main_space_);
      space::Space* old_main_space = main_space_;
      CreateMainMallocSpace(std::move(mem_map),
                            kDefaultInitialSize,
                            std::min(mem_map.Size(), growth_limit_),
                            mem_map.Size());
      delete old_main_space;
      AddSpace(main_space_);
    } else {
      if (collector_type_ == kCollectorTypeCC) {
        region_space_->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
        region_space_->GetMarkBitmap()->Clear();
      } else {
        bump_pointer_space_->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
      }
    }
    if (temp_space_ != nullptr) {
      CHECK(temp_space_->IsEmpty());
    }
    IncrementFreedEver();
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
  constexpr bool set_mark_bit = kUseBakerReadBarrier
                                && gc::collector::ConcurrentCopying::kGrayDirtyImmuneObjects;
  if (set_mark_bit) {
    zygote_space_->SetMarkBitInLiveObjects();
  }
  accounting::ModUnionTable* mod_union_table =
      new accounting::ModUnionTableCardCache("zygote space mod-union table", this, zygote_space_);
  CHECK(mod_union_table != nullptr) << "Failed to create zygote space mod-union table";
  if (collector_type_ != kCollectorTypeCC && collector_type_ != kCollectorTypeCMC) {
    mod_union_table->SetCards();
  } else {
    mod_union_table->ProcessCards();
    mod_union_table->ClearTable();
    for (auto& pair : mod_union_tables_) {
      CHECK(pair.first->IsImageSpace());
      CHECK(!pair.first->AsImageSpace()->GetImageHeader().IsAppImage());
      accounting::ModUnionTable* table = pair.second;
      table->ClearTable();
    }
  }
  AddModUnionTable(mod_union_table);
  large_object_space_->SetAllLargeObjectsAsZygoteObjects(self, set_mark_bit);
  if (collector::SemiSpace::kUseRememberedSet) {
    accounting::RememberedSet* post_zygote_non_moving_space_rem_set =
        new accounting::RememberedSet("Post-zygote non-moving space remembered set", this,
                                      non_moving_space_);
    CHECK(post_zygote_non_moving_space_rem_set != nullptr)
        << "Failed to create post-zygote non-moving space remembered set";
    AddRememberedSet(post_zygote_non_moving_space_rem_set);
  }
}
#pragma clang diagnostic pop
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
  }
  LOG(FATAL) << "Unsupported";
  UNREACHABLE();
}
void Heap::TraceHeapSize(size_t heap_size) {
  ATraceIntegerValue("Heap size (KB)", heap_size / KB);
}
#if defined(__GLIBC__)
#define IF_GLIBC(x) x
#else
#define IF_GLIBC(x) 
#endif
size_t Heap::GetNativeBytes() {
  size_t malloc_bytes;
#if defined(__BIONIC__) || defined(__GLIBC__)
  IF_GLIBC(size_t mmapped_bytes;)
  struct mallinfo mi = mallinfo();
  if (sizeof(size_t) > sizeof(mi.uordblks) && sizeof(size_t) > sizeof(mi.hblkhd)) {
    malloc_bytes = (unsigned int)mi.uordblks;
    IF_GLIBC(mmapped_bytes = (unsigned int)mi.hblkhd;)
  } else {
    malloc_bytes = mi.uordblks;
    IF_GLIBC(mmapped_bytes = mi.hblkhd;)
  }
#if defined(__GLIBC__)
  if (mmapped_bytes > malloc_bytes) {
    malloc_bytes = mmapped_bytes;
  }
#endif
#else
  malloc_bytes = 1000;
#endif
  return malloc_bytes + native_bytes_registered_.load(std::memory_order_relaxed);
}
static inline bool GCNumberLt(uint32_t gc_num1, uint32_t gc_num2) {
  uint32_t difference = gc_num2 - gc_num1;
  bool completed_more_than_requested = difference > 0x80000000;
  return difference > 0 && !completed_more_than_requested;
}
collector::GcType Heap::CollectGarbageInternal(collector::GcType gc_type,
                                               GcCause gc_cause,
                                               bool clear_soft_references,
                                               uint32_t requested_gc_num) {
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
  ScopedThreadStateChange tsc(self, ThreadState::kWaitingPerformingGc);
  Locks::mutator_lock_->AssertNotHeld(self);
  if (self->IsHandlingStackOverflow()) {
    gcs_completed_.fetch_add(1, std::memory_order_release);
    return collector::kGcTypeNone;
  }
  bool compacting_gc;
  {
    gc_complete_lock_->AssertNotHeld(self);
    ScopedThreadStateChange tsc2(self, ThreadState::kWaitingForGcToComplete);
    MutexLock mu(self, *gc_complete_lock_);
    WaitForGcToCompleteLocked(gc_cause, self);
    if (requested_gc_num != GC_NUM_ANY && !GCNumberLt(GetCurrentGcNum(), requested_gc_num)) {
      return collector::kGcTypeNone;
    }
    compacting_gc = IsMovingGc(collector_type_);
    if (compacting_gc && disable_moving_gc_count_ != 0) {
      LOG(WARNING) << "Skipping GC due to disable moving GC count " << disable_moving_gc_count_;
      gcs_completed_.fetch_add(1, std::memory_order_release);
      return collector::kGcTypeNone;
    }
    if (gc_disabled_for_shutdown_) {
      gcs_completed_.fetch_add(1, std::memory_order_release);
      return collector::kGcTypeNone;
    }
    collector_type_running_ = collector_type_;
    last_gc_cause_ = gc_cause;
  }
  if (gc_cause == kGcCauseForAlloc && runtime->HasStatsEnabled()) {
    ++runtime->GetStats()->gc_for_alloc_count;
    ++self->GetStats()->gc_for_alloc_count;
  }
  const size_t bytes_allocated_before_gc = GetBytesAllocated();
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
        semi_space_collector_->SetFromSpace(bump_pointer_space_);
        semi_space_collector_->SetToSpace(temp_space_);
        semi_space_collector_->SetSwapSemiSpaces(true);
        collector = semi_space_collector_;
        break;
      case kCollectorTypeCMC:
        collector = mark_compact_;
        break;
      case kCollectorTypeCC:
        collector::ConcurrentCopying* active_cc_collector;
        if (use_generational_cc_) {
          active_cc_collector = (gc_type == collector::kGcTypeSticky) ?
                  young_concurrent_copying_collector_ : concurrent_copying_collector_;
          active_concurrent_copying_collector_.store(active_cc_collector,
                                                     std::memory_order_relaxed);
          DCHECK(active_cc_collector->RegionSpace() == region_space_);
          collector = active_cc_collector;
        } else {
          collector = active_concurrent_copying_collector_.load(std::memory_order_relaxed);
        }
        break;
      default:
        LOG(FATAL) << "Invalid collector type " << static_cast<size_t>(collector_type_);
    }
    if (temp_space_ != nullptr
        && collector != active_concurrent_copying_collector_.load(std::memory_order_relaxed)) {
      temp_space_->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
      if (kIsDebugBuild) {
        temp_space_->GetMemMap()->TryReadable();
      }
      CHECK(temp_space_->IsEmpty());
    }
  } else if (current_allocator_ == kAllocatorTypeRosAlloc ||
      current_allocator_ == kAllocatorTypeDlMalloc) {
    collector = FindCollectorByGcType(gc_type);
  } else {
    LOG(FATAL) << "Invalid current allocator " << current_allocator_;
  }
  CHECK(collector != nullptr)
      << "Could not find garbage collector with collector_type="
      << static_cast<size_t>(collector_type_) << " and gc_type=" << gc_type;
  collector->Run(gc_cause, clear_soft_references || runtime->IsZygote());
  IncrementFreedEver();
  RequestTrim(self);
  SelfDeletingTask* clear = reference_processor_->CollectClearedReferences(self);
  GrowForUtilization(collector, bytes_allocated_before_gc);
  old_native_bytes_allocated_.store(GetNativeBytes());
  LogGC(gc_cause, collector);
  FinishGC(self, gc_type);
  clear->Run(self);
  clear->Finalize();
  Dbg::GcDidFinish();
  {
    ScopedObjectAccess soa(self);
    soa.Vm()->UnloadNativeLibraries();
  }
  return gc_type;
}
void Heap::LogGC(GcCause gc_cause, collector::GarbageCollector* collector) {
  const size_t duration = GetCurrentGcIteration()->GetDurationNs();
  const std::vector<uint64_t>& pause_times = GetCurrentGcIteration()->GetPauseTimes();
  bool log_gc = kLogAllGCs || (gc_cause == kGcCauseExplicit && always_log_explicit_gcs_);
  if (!log_gc && CareAboutPauseTimes()) {
    log_gc = duration > long_gc_log_threshold_ ||
        (gc_cause == kGcCauseForAlloc && duration > long_pause_log_threshold_);
    for (uint64_t pause : pause_times) {
      log_gc = log_gc || pause >= long_pause_log_threshold_;
    }
  }
  bool is_sampled = false;
  if (UNLIKELY(gc_stress_mode_)) {
    static std::atomic_int64_t accumulated_duration_ns = 0;
    accumulated_duration_ns += duration;
    if (accumulated_duration_ns >= kGcStressModeGcLogSampleFrequencyNs) {
      accumulated_duration_ns -= kGcStressModeGcLogSampleFrequencyNs;
      log_gc = true;
      is_sampled = true;
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
              << (is_sampled ? " (sampled)" : "")
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
  thread_running_gc_ = nullptr;
  if (gc_type != collector::kGcTypeNone) {
    gcs_completed_.fetch_add(1, std::memory_order_release);
  }
  gc_complete_cond_->Broadcast(self);
}
void Heap::UpdateGcCountRateHistograms() {
  DCHECK_EQ(last_update_time_gc_count_rate_histograms_ % kGcCountRateHistogramWindowDuration, 0U);
  uint64_t now = NanoTime();
  DCHECK_GE(now, last_update_time_gc_count_rate_histograms_);
  uint64_t time_since_last_update = now - last_update_time_gc_count_rate_histograms_;
  uint64_t num_of_windows = time_since_last_update / kGcCountRateHistogramWindowDuration;
  if (num_of_windows > kGcCountRateHistogramMaxNumMissedWindows) {
    LOG(WARNING) << "Reducing the number of considered missed Gc histogram windows from "
                 << num_of_windows << " to " << kGcCountRateHistogramMaxNumMissedWindows;
    num_of_windows = kGcCountRateHistogramMaxNumMissedWindows;
  }
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
      override REQUIRES_SHARED(Locks::mutator_lock_) {
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
  VerifyReferenceVisitor(Thread* self, Heap* heap, size_t* fail_count, bool verify_referent)
      REQUIRES_SHARED(Locks::mutator_lock_)
      : self_(self), heap_(heap), fail_count_(fail_count), verify_referent_(verify_referent) {
    CHECK_EQ(self_, Thread::Current());
  }
  void operator()(ObjPtr<mirror::Class> klass ATTRIBUTE_UNUSED, ObjPtr<mirror::Reference> ref) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (verify_referent_) {
      VerifyReference(ref.Ptr(), ref->GetReferent(), mirror::Reference::ReferentOffset());
    }
  }
  void operator()(ObjPtr<mirror::Object> obj,
                  MemberOffset offset,
                  bool is_static ATTRIBUTE_UNUSED) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    VerifyReference(obj.Ptr(), obj->GetFieldObject<mirror::Object>(offset), offset);
  }
  bool IsLive(ObjPtr<mirror::Object> obj) const NO_THREAD_SAFETY_ANALYSIS {
    return heap_->IsLiveObjectLocked(obj, true, false, true);
  }
  void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (!root->IsNull()) {
      VisitRoot(root);
    }
  }
  void VisitRoot(mirror::CompressedReference<mirror::Object>* root) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    const_cast<VerifyReferenceVisitor*>(this)->VisitRoot(
        root->AsMirrorPtr(), RootInfo(kRootVMInternal));
  }
  void VisitRoot(mirror::Object* root, const RootInfo& root_info) override
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (root == nullptr) {
      LOG(ERROR) << "Root is null with info " << root_info.GetType();
    } else if (!VerifyReference(nullptr, root, MemberOffset(0))) {
      LOG(ERROR) << "Root " << root << " is dead with type " << mirror::Object::PrettyTypeOf(root)
          << " thread_id= " << root_info.GetThreadId() << " root_type= " << root_info.GetType();
    }
  }
 private:
  bool VerifyReference(mirror::Object* obj, mirror::Object* ref, MemberOffset offset) const
      NO_THREAD_SAFETY_ANALYSIS {
    if (ref == nullptr || IsLive(ref)) {
      return true;
    }
    CHECK_EQ(self_, Thread::Current());
    *fail_count_ += 1;
    if (*fail_count_ == 1) {
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
        LOG(ERROR) << "Obj type " << obj->PrettyTypeOf();
      } else {
        LOG(ERROR) << "Object " << obj << " class(" << obj->GetClass() << ") not a heap address";
      }
      space::ContinuousSpace* ref_space = heap_->FindContinuousSpaceFromObject(ref, true);
      if (ref_space != nullptr && ref_space->IsMallocSpace()) {
        space::MallocSpace* space = ref_space->AsMallocSpace();
        mirror::Class* ref_class = space->FindRecentFreedObject(ref);
        if (ref_class != nullptr) {
          LOG(ERROR) << "Reference " << ref << " found as a recently freed object with class "
                     << ref_class->PrettyClass();
        } else {
          LOG(ERROR) << "Reference " << ref << " not found as a recently freed object";
        }
      }
      if (ref->GetClass() != nullptr && heap_->IsValidObjectAddress(ref->GetClass()) &&
          ref->GetClass()->IsClass()) {
        LOG(ERROR) << "Ref type " << ref->PrettyTypeOf();
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
  Thread* const self_;
  Heap* const heap_;
  size_t* const fail_count_;
  const bool verify_referent_;
};
class VerifyObjectVisitor {
 public:
  VerifyObjectVisitor(Thread* self, Heap* heap, size_t* fail_count, bool verify_referent)
      : self_(self), heap_(heap), fail_count_(fail_count), verify_referent_(verify_referent) {}
  void operator()(mirror::Object* obj) REQUIRES_SHARED(Locks::mutator_lock_) {
    VerifyReferenceVisitor visitor(self_, heap_, fail_count_, verify_referent_);
    obj->VisitReferences(visitor, visitor);
  }
  void VerifyRoots() REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!Locks::heap_bitmap_lock_) {
    ReaderMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
    VerifyReferenceVisitor visitor(self_, heap_, fail_count_, verify_referent_);
    Runtime::Current()->VisitRoots(&visitor);
  }
  uint32_t GetFailureCount() const REQUIRES(Locks::mutator_lock_) {
    CHECK_EQ(self_, Thread::Current());
    return *fail_count_;
  }
 private:
  Thread* const self_;
  Heap* const heap_;
  size_t* const fail_count_;
  const bool verify_referent_;
};
void Heap::PushOnAllocationStackWithInternalGC(Thread* self, ObjPtr<mirror::Object>* obj) {
  DCHECK(!allocation_stack_->AtomicPushBack(obj->Ptr()));
  do {
    StackHandleScope<1> hs(self);
    HandleWrapperObjPtr<mirror::Object> wrapper(hs.NewHandleWrapper(obj));
    CHECK(allocation_stack_->AtomicPushBackIgnoreGrowthLimit(obj->Ptr()));
    CollectGarbageInternal(collector::kGcTypeSticky,
                           kGcCauseForAlloc,
                           false,
                           GetCurrentGcNum() + 1);
  } while (!allocation_stack_->AtomicPushBack(obj->Ptr()));
}
void Heap::PushOnThreadLocalAllocationStackWithInternalGC(Thread* self,
                                                          ObjPtr<mirror::Object>* obj) {
  DCHECK(!self->PushOnThreadLocalAllocationStack(obj->Ptr()));
  StackReference<mirror::Object>* start_address;
  StackReference<mirror::Object>* end_address;
  while (!allocation_stack_->AtomicBumpBack(kThreadLocalAllocationStackSize, &start_address,
                                            &end_address)) {
    StackHandleScope<1> hs(self);
    HandleWrapperObjPtr<mirror::Object> wrapper(hs.NewHandleWrapper(obj));
    CHECK(allocation_stack_->AtomicPushBackIgnoreGrowthLimit(obj->Ptr()));
    CollectGarbageInternal(collector::kGcTypeSticky,
                           kGcCauseForAlloc,
                           false,
                           GetCurrentGcNum() + 1);
  }
  self->SetThreadLocalAllocationStack(start_address, end_address);
  CHECK(self->PushOnThreadLocalAllocationStack(obj->Ptr()));
}
size_t Heap::VerifyHeapReferences(bool verify_referents) {
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertExclusiveHeld(self);
  allocation_stack_->Sort();
  live_stack_->Sort();
  RevokeAllThreadLocalAllocationStacks(self);
  size_t fail_count = 0;
  VerifyObjectVisitor visitor(self, this, &fail_count, verify_referents);
  VisitObjectsPaused(visitor);
  visitor.VerifyRoots();
  if (visitor.GetFailureCount() > 0) {
    for (const auto& table_pair : mod_union_tables_) {
      accounting::ModUnionTable* mod_union_table = table_pair.second;
      mod_union_table->Dump(LOG_STREAM(ERROR) << mod_union_table->GetName() << ": ");
    }
    for (const auto& table_pair : remembered_sets_) {
      accounting::RememberedSet* remembered_set = table_pair.second;
      remembered_set->Dump(LOG_STREAM(ERROR) << remembered_set->GetName() << ": ");
    }
    DumpSpaces(LOG_STREAM(ERROR));
  }
  return visitor.GetFailureCount();
}
class VerifyReferenceCardVisitor {
 public:
  VerifyReferenceCardVisitor(Heap* heap, bool* failed)
      REQUIRES_SHARED(Locks::mutator_lock_,
                            Locks::heap_bitmap_lock_)
      : heap_(heap), failed_(failed) {
  }
  void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root ATTRIBUTE_UNUSED)
      const {}
  void VisitRoot(mirror::CompressedReference<mirror::Object>* root ATTRIBUTE_UNUSED) const {}
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
          LOG(ERROR) << "Object " << obj << " " << mirror::Object::PrettyTypeOf(obj)
                    << " references " << ref << " " << mirror::Object::PrettyTypeOf(ref)
                    << " in live stack";
          if (!obj->IsObjectArray()) {
            ObjPtr<mirror::Class> klass = is_static ? obj->AsClass() : obj->GetClass();
            CHECK(klass != nullptr);
            for (ArtField& field : (is_static ? klass->GetSFields() : klass->GetIFields())) {
              if (field.GetOffset().Int32Value() == offset.Int32Value()) {
                LOG(ERROR) << (is_static ? "Static " : "") << "field in the live stack is "
                           << field.PrettyField();
                break;
              }
            }
          } else {
            ObjPtr<mirror::ObjectArray<mirror::Object>> object_array =
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
      REQUIRES_SHARED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    VerifyReferenceCardVisitor visitor(heap_, const_cast<bool*>(&failed_));
    obj->VisitReferences(visitor, VoidFunctor());
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
void Heap::SwapStacks() {
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
void Heap::ProcessCards(TimingLogger* timings,
                        bool use_rem_sets,
                        bool process_alloc_space_cards,
                        bool clear_alloc_space_cards) {
  TimingLogger::ScopedTiming t(__FUNCTION__, timings);
  for (const auto& space : continuous_spaces_) {
    accounting::ModUnionTable* table = FindModUnionTableFromSpace(space);
    accounting::RememberedSet* rem_set = FindRememberedSetFromSpace(space);
    if (table != nullptr) {
      const char* name = space->IsZygoteSpace() ? "ZygoteModUnionClearCards" :
          "ImageModUnionClearCards";
      TimingLogger::ScopedTiming t2(name, timings);
      table->ProcessCards();
    } else if (use_rem_sets && rem_set != nullptr) {
      DCHECK(collector::SemiSpace::kUseRememberedSet) << static_cast<int>(collector_type_);
      TimingLogger::ScopedTiming t2("AllocSpaceRemSetClearCards", timings);
      rem_set->ClearCards();
    } else if (process_alloc_space_cards) {
      TimingLogger::ScopedTiming t2("AllocSpaceClearCards", timings);
      if (clear_alloc_space_cards) {
        uint8_t* end = space->End();
        if (space->IsImageSpace()) {
          end = AlignUp(end, accounting::CardTable::kCardSize);
        }
        card_table_->ClearCardRange(space->Begin(), end);
      } else {
        card_table_->ModifyCardsAtomic(space->Begin(), space->End(), AgeCardVisitor(),
                                       VoidFunctor());
      }
    }
  }
}
struct IdentityMarkHeapReferenceVisitor : public MarkObjectVisitor {
  mirror::Object* MarkObject(mirror::Object* obj) override {
    return obj;
  }
  void MarkHeapReference(mirror::HeapReference<mirror::Object>*, bool) override {
  }
};
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
    SwapStacks();
    CHECK(VerifyMissingCardMarks()) << "Pre " << gc->GetName()
                                    << " missing card mark verification failed\n" << DumpSpaces();
    SwapStacks();
  }
  if (verify_mod_union_table_) {
    TimingLogger::ScopedTiming t2("(Paused)PreGcVerifyModUnionTables", timings);
    ReaderMutexLock reader_lock(self, *Locks::heap_bitmap_lock_);
    for (const auto& table_pair : mod_union_tables_) {
      accounting::ModUnionTable* mod_union_table = table_pair.second;
      IdentityMarkHeapReferenceVisitor visitor;
      mod_union_table->UpdateAndMarkReferences(&visitor);
      mod_union_table->Verify();
    }
  }
}
void Heap::PreGcVerification(collector::GarbageCollector* gc) {
  if (verify_pre_gc_heap_ || verify_missing_card_marks_ || verify_mod_union_table_) {
    collector::GarbageCollector::ScopedPause pause(gc, false);
    PreGcVerificationPaused(gc);
  }
}
void Heap::PrePauseRosAllocVerification(collector::GarbageCollector* gc ATTRIBUTE_UNUSED) {
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
    CHECK_NE(self->GetState(), ThreadState::kRunnable);
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
    collector::GarbageCollector::ScopedPause pause(gc, false);
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
  ScopedThreadStateChange tsc(self, ThreadState::kWaitingForGcToComplete);
  MutexLock mu(self, *gc_complete_lock_);
  return WaitForGcToCompleteLocked(cause, self);
}
collector::GcType Heap::WaitForGcToCompleteLocked(GcCause cause, Thread* self) {
  gc_complete_cond_->CheckSafeToWait(self);
  collector::GcType last_gc_type = collector::kGcTypeNone;
  GcCause last_gc_cause = kGcCauseNone;
  uint64_t wait_start = NanoTime();
  while (collector_type_running_ != kCollectorTypeNone) {
    if (self != task_processor_->GetRunningThread()) {
      running_collection_is_blocking_ = true;
      VLOG(gc) << "Waiting for a blocking GC " << cause;
    }
    SCOPED_TRACE << "GC: Wait For Completion " << cause;
    gc_complete_cond_->Wait(self);
    last_gc_type = last_gc_type_;
    last_gc_cause = last_gc_cause_;
  }
  uint64_t wait_time = NanoTime() - wait_start;
  total_wait_time_ += wait_time;
  if (wait_time > long_pause_log_threshold_) {
    LOG(INFO) << "WaitForGcToComplete blocked " << cause << " on " << last_gc_cause << " for "
              << PrettyDuration(wait_time);
  }
  if (self != task_processor_->GetRunningThread()) {
    running_collection_is_blocking_ = true;
    if (cause == kGcCauseForAlloc ||
        cause == kGcCauseDisableMovingGc) {
      VLOG(gc) << "Starting a blocking GC " << cause;
    }
  }
  return last_gc_type;
}
void Heap::DumpForSigQuit(std::ostream& os) {
  os << "Heap: " << GetPercentFree() << "% free, " << PrettySize(GetBytesAllocated()) << "/"
     << PrettySize(GetTotalMemory()) << "; " << GetObjectsAllocated() << " objects\n";
  {
    os << "Image spaces:\n";
    ScopedObjectAccess soa(Thread::Current());
    for (const auto& space : continuous_spaces_) {
      if (space->IsImageSpace()) {
        os << space->GetName() << "\n";
      }
    }
  }
  DumpGcPerformanceInfo(os);
}
size_t Heap::GetPercentFree() {
  return static_cast<size_t>(100.0f * static_cast<float>(
      GetFreeMemory()) / target_footprint_.load(std::memory_order_relaxed));
}
void Heap::SetIdealFootprint(size_t target_footprint) {
  if (target_footprint > GetMaxMemory()) {
    VLOG(gc) << "Clamp target GC heap from " << PrettySize(target_footprint) << " to "
             << PrettySize(GetMaxMemory());
    target_footprint = GetMaxMemory();
  }
  target_footprint_.store(target_footprint, std::memory_order_relaxed);
}
bool Heap::IsMovableObject(ObjPtr<mirror::Object> obj) const {
  if (kMovingCollector) {
    space::Space* space = FindContinuousSpaceFromObject(obj.Ptr(), true);
    if (space != nullptr) {
      return space->CanMoveObjects();
    }
  }
  return false;
}
collector::GarbageCollector* Heap::FindCollectorByGcType(collector::GcType gc_type) {
  for (auto* collector : garbage_collectors_) {
    if (collector->GetCollectorType() == collector_type_ &&
        collector->GetGcType() == gc_type) {
      return collector;
    }
  }
  return nullptr;
}
double Heap::HeapGrowthMultiplier() const {
  if (!CareAboutPauseTimes()) {
    return 1.0;
  }
  return foreground_heap_growth_multiplier_;
}
void Heap::GrowForUtilization(collector::GarbageCollector* collector_ran,
                              size_t bytes_allocated_before_gc) {
  const size_t bytes_allocated = GetBytesAllocated();
  TraceHeapSize(bytes_allocated);
  uint64_t target_size, grow_bytes;
  collector::GcType gc_type = collector_ran->GetGcType();
  MutexLock mu(Thread::Current(), process_state_update_lock_);
  const double multiplier = HeapGrowthMultiplier();
  if (gc_type != collector::kGcTypeSticky) {
    uint64_t delta = bytes_allocated * (1.0 / GetTargetHeapUtilization() - 1.0);
    DCHECK_LE(delta, std::numeric_limits<size_t>::max()) << "bytes_allocated=" << bytes_allocated
        << " target_utilization_=" << target_utilization_;
    grow_bytes = std::min(delta, static_cast<uint64_t>(max_free_));
    grow_bytes = std::max(grow_bytes, static_cast<uint64_t>(min_free_));
    target_size = bytes_allocated + static_cast<uint64_t>(grow_bytes * multiplier);
    next_gc_type_ = collector::kGcTypeSticky;
  } else {
    collector::GcType non_sticky_gc_type = NonStickyGcType();
    collector::GarbageCollector* non_sticky_collector = FindCollectorByGcType(non_sticky_gc_type);
    if (use_generational_cc_) {
      if (non_sticky_collector == nullptr) {
        non_sticky_collector = FindCollectorByGcType(collector::kGcTypePartial);
      }
      CHECK(non_sticky_collector != nullptr);
    }
    double sticky_gc_throughput_adjustment = GetStickyGcThroughputAdjustment(use_generational_cc_);
    size_t target_footprint = target_footprint_.load(std::memory_order_relaxed);
    if (current_gc_iteration_.GetEstimatedThroughput() * sticky_gc_throughput_adjustment >=
        non_sticky_collector->GetEstimatedMeanThroughput() &&
        non_sticky_collector->NumberOfIterations() > 0 &&
        bytes_allocated <= (IsGcConcurrent() ? concurrent_start_bytes_ : target_footprint)) {
      next_gc_type_ = collector::kGcTypeSticky;
    } else {
      next_gc_type_ = non_sticky_gc_type;
    }
    const size_t adjusted_max_free = static_cast<size_t>(max_free_ * multiplier);
    if (bytes_allocated + adjusted_max_free < target_footprint) {
      target_size = bytes_allocated + adjusted_max_free;
      grow_bytes = max_free_;
    } else {
      target_size = std::max(bytes_allocated, target_footprint);
      grow_bytes = 0;
    }
  }
  CHECK_LE(target_size, std::numeric_limits<size_t>::max());
  if (!ignore_target_footprint_) {
    SetIdealFootprint(target_size);
    min_foreground_target_footprint_ =
        (multiplier <= 1.0 && grow_bytes > 0)
        ? std::min(
          bytes_allocated + static_cast<size_t>(grow_bytes * foreground_heap_growth_multiplier_),
          GetMaxMemory())
        : 0;
    if (IsGcConcurrent()) {
      const uint64_t freed_bytes = current_gc_iteration_.GetFreedBytes() +
          current_gc_iteration_.GetFreedLargeObjectBytes() +
          current_gc_iteration_.GetFreedRevokeBytes();
      num_bytes_alive_after_gc_ = UnsignedDifference(bytes_allocated_before_gc, freed_bytes);
      const size_t bytes_allocated_during_gc =
          UnsignedDifference(bytes_allocated + freed_bytes, bytes_allocated_before_gc);
      size_t remaining_bytes = bytes_allocated_during_gc;
      remaining_bytes = std::min(remaining_bytes, kMaxConcurrentRemainingBytes);
      remaining_bytes = std::max(remaining_bytes, kMinConcurrentRemainingBytes);
      size_t target_footprint = target_footprint_.load(std::memory_order_relaxed);
      if (UNLIKELY(remaining_bytes > target_footprint)) {
        remaining_bytes = std::min(kMinConcurrentRemainingBytes, target_footprint);
      }
      DCHECK_LE(target_footprint_.load(std::memory_order_relaxed), GetMaxMemory());
      concurrent_start_bytes_ = std::max(target_footprint - remaining_bytes, bytes_allocated);
      min_foreground_concurrent_start_bytes_ =
          min_foreground_target_footprint_ != 0
          ? std::max(min_foreground_target_footprint_ - remaining_bytes, bytes_allocated)
          : 0;
    }
  }
}
void Heap::ClampGrowthLimit() {
  ScopedObjectAccess soa(Thread::Current());
  WriterMutexLock mu(soa.Self(), *Locks::heap_bitmap_lock_);
  capacity_ = growth_limit_;
  for (const auto& space : continuous_spaces_) {
    if (space->IsMallocSpace()) {
      gc::space::MallocSpace* malloc_space = space->AsMallocSpace();
      malloc_space->ClampGrowthLimit();
    }
  }
  if (collector_type_ == kCollectorTypeCC) {
    DCHECK(region_space_ != nullptr);
    region_space_->ClampGrowthLimit(2 * capacity_);
  }
  if (main_space_backup_.get() != nullptr) {
    main_space_backup_->ClampGrowthLimit();
  }
}
void Heap::ClearGrowthLimit() {
  if (target_footprint_.load(std::memory_order_relaxed) == growth_limit_
      && growth_limit_ < capacity_) {
    target_footprint_.store(capacity_, std::memory_order_relaxed);
    SetDefaultConcurrentStartBytes();
  }
  growth_limit_ = capacity_;
  ScopedObjectAccess soa(Thread::Current());
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
void Heap::AddFinalizerReference(Thread* self, ObjPtr<mirror::Object>* object) {
  ScopedObjectAccess soa(self);
  StackHandleScope<1u> hs(self);
  HandleWrapperObjPtr<mirror::Object> h_object = hs.NewHandleWrapper(object);
  WellKnownClasses::java_lang_ref_FinalizerReference_add->InvokeStatic<'V', 'L'>(
      self, h_object.Get());
}
void Heap::RequestConcurrentGCAndSaveObject(Thread* self,
                                            bool force_full,
                                            uint32_t observed_gc_num,
                                            ObjPtr<mirror::Object>* obj) {
  StackHandleScope<1> hs(self);
  HandleWrapperObjPtr<mirror::Object> wrapper(hs.NewHandleWrapper(obj));
  RequestConcurrentGC(self, kGcCauseBackground, force_full, observed_gc_num);
}
class Heap::ConcurrentGCTask : public HeapTask {
 public:
  ConcurrentGCTask(uint64_t target_time, GcCause cause, bool force_full, uint32_t gc_num)
      : HeapTask(target_time), cause_(cause), force_full_(force_full), my_gc_num_(gc_num) {}
  void Run(Thread* self) override {
    Runtime* runtime = Runtime::Current();
    gc::Heap* heap = runtime->GetHeap();
    DCHECK(GCNumberLt(my_gc_num_, heap->GetCurrentGcNum() + 2));
    heap->ConcurrentGC(self, cause_, force_full_, my_gc_num_);
    CHECK_IMPLIES(GCNumberLt(heap->GetCurrentGcNum(), my_gc_num_), runtime->IsShuttingDown(self));
  }
 private:
  const GcCause cause_;
  const bool force_full_;
  const uint32_t my_gc_num_;
};
static bool CanAddHeapTask(Thread* self) REQUIRES(!Locks::runtime_shutdown_lock_) {
  Runtime* runtime = Runtime::Current();
  return runtime != nullptr && runtime->IsFinishedStarting() && !runtime->IsShuttingDown(self) &&
      !self->IsHandlingStackOverflow();
}
bool Heap::RequestConcurrentGC(Thread* self,
                               GcCause cause,
                               bool force_full,
                               uint32_t observed_gc_num) {
  uint32_t max_gc_requested = max_gc_requested_.load(std::memory_order_relaxed);
  if (!GCNumberLt(observed_gc_num, max_gc_requested)) {
    if (CanAddHeapTask(self)) {
      if (max_gc_requested_.CompareAndSetStrongRelaxed(max_gc_requested, observed_gc_num + 1)) {
        task_processor_->AddTask(self, new ConcurrentGCTask(NanoTime(),
                                                            cause,
                                                            force_full,
                                                            observed_gc_num + 1));
      }
      DCHECK(GCNumberLt(observed_gc_num, max_gc_requested_.load(std::memory_order_relaxed)));
      return true;
    }
    return false;
  }
  return true;
}
void Heap::ConcurrentGC(Thread* self, GcCause cause, bool force_full, uint32_t requested_gc_num) {
  if (!Runtime::Current()->IsShuttingDown(self)) {
    WaitForGcToComplete(cause, self);
    if (GCNumberLt(GetCurrentGcNum(), requested_gc_num)) {
      collector::GcType next_gc_type = next_gc_type_;
      if (force_full && next_gc_type == collector::kGcTypeSticky) {
        next_gc_type = NonStickyGcType();
      }
      if (CollectGarbageInternal(next_gc_type, cause, false, requested_gc_num)
          == collector::kGcTypeNone) {
        for (collector::GcType gc_type : gc_plan_) {
          if (!GCNumberLt(GetCurrentGcNum(), requested_gc_num)) {
            break;
          }
          if (gc_type > next_gc_type &&
              CollectGarbageInternal(gc_type, cause, false, requested_gc_num)
              != collector::kGcTypeNone) {
            break;
          }
        }
      }
    }
  }
}
class Heap::CollectorTransitionTask : public HeapTask {
 public:
  explicit CollectorTransitionTask(uint64_t target_time) : HeapTask(target_time) {}
  void Run(Thread* self) override {
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
  if (collector_type_ == kCollectorTypeCC) {
    DCHECK_EQ(desired_collector_type_, kCollectorTypeCCBackground);
<<<<<<< HEAD
||||||| f5107e929f
    size_t num_bytes_allocated_since_gc = GetBytesAllocated() - num_bytes_alive_after_gc_;
    if (num_bytes_allocated_since_gc <
        (UnsignedDifference(target_footprint_.load(std::memory_order_relaxed),
                            num_bytes_alive_after_gc_)/4)
        && !kStressCollectorTransition
        && !IsLowMemoryMode()) {
      return;
    }
=======
  }
  if (collector_type_ == kCollectorTypeCC || collector_type_ == kCollectorTypeCMC) {
    size_t num_bytes_allocated_since_gc = GetBytesAllocated() - num_bytes_alive_after_gc_;
    if (num_bytes_allocated_since_gc <
        (UnsignedDifference(target_footprint_.load(std::memory_order_relaxed),
                            num_bytes_alive_after_gc_)/4)
        && !kStressCollectorTransition
        && !IsLowMemoryMode()) {
      return;
    }
>>>>>>> 344f4e7e
  }
  DCHECK_NE(collector_type_, kCollectorTypeCCBackground);
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
  void Run(Thread* self) override {
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
void Heap::IncrementNumberOfBytesFreedRevoke(size_t freed_bytes_revoke) {
  size_t previous_num_bytes_freed_revoke =
      num_bytes_freed_revoke_.fetch_add(freed_bytes_revoke, std::memory_order_relaxed);
  CHECK_GE(num_bytes_allocated_.load(std::memory_order_relaxed),
           previous_num_bytes_freed_revoke + freed_bytes_revoke);
}
void Heap::RevokeThreadLocalBuffers(Thread* thread) {
  if (rosalloc_space_ != nullptr) {
    size_t freed_bytes_revoke = rosalloc_space_->RevokeThreadLocalBuffers(thread);
    if (freed_bytes_revoke > 0U) {
      IncrementNumberOfBytesFreedRevoke(freed_bytes_revoke);
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
      IncrementNumberOfBytesFreedRevoke(freed_bytes_revoke);
    }
  }
}
void Heap::RevokeAllThreadLocalBuffers() {
  if (rosalloc_space_ != nullptr) {
    size_t freed_bytes_revoke = rosalloc_space_->RevokeAllThreadLocalBuffers();
    if (freed_bytes_revoke > 0U) {
      IncrementNumberOfBytesFreedRevoke(freed_bytes_revoke);
    }
  }
  if (bump_pointer_space_ != nullptr) {
    CHECK_EQ(bump_pointer_space_->RevokeAllThreadLocalBuffers(), 0U);
  }
  if (region_space_ != nullptr) {
    CHECK_EQ(region_space_->RevokeAllThreadLocalBuffers(), 0U);
  }
}
static constexpr size_t kOldNativeDiscountFactor = 65536;
static constexpr size_t kNewNativeDiscountFactor = 2;
static constexpr float kStopForNativeFactor = 4.0;
inline float Heap::NativeMemoryOverTarget(size_t current_native_bytes, bool is_gc_concurrent) {
  size_t old_native_bytes = old_native_bytes_allocated_.load(std::memory_order_relaxed);
  if (old_native_bytes > current_native_bytes) {
    old_native_bytes_allocated_.store(current_native_bytes, std::memory_order_relaxed);
    return 0.0;
  } else {
    size_t new_native_bytes = UnsignedDifference(current_native_bytes, old_native_bytes);
    size_t weighted_native_bytes = new_native_bytes / kNewNativeDiscountFactor
        + old_native_bytes / kOldNativeDiscountFactor;
    size_t add_bytes_allowed = static_cast<size_t>(
        NativeAllocationGcWatermark() * HeapGrowthMultiplier());
    size_t java_gc_start_bytes = is_gc_concurrent
        ? concurrent_start_bytes_
        : target_footprint_.load(std::memory_order_relaxed);
    size_t adj_start_bytes = UnsignedSum(java_gc_start_bytes,
                                         add_bytes_allowed / kNewNativeDiscountFactor);
    return static_cast<float>(GetBytesAllocated() + weighted_native_bytes)
         / static_cast<float>(adj_start_bytes);
  }
}
inline void Heap::CheckGCForNative(Thread* self) {
  bool is_gc_concurrent = IsGcConcurrent();
  uint32_t starting_gc_num = GetCurrentGcNum();
  size_t current_native_bytes = GetNativeBytes();
  float gc_urgency = NativeMemoryOverTarget(current_native_bytes, is_gc_concurrent);
  if (UNLIKELY(gc_urgency >= 1.0)) {
    if (is_gc_concurrent) {
      bool requested =
          RequestConcurrentGC(self, kGcCauseForNativeAlloc, true, starting_gc_num);
      if (requested && gc_urgency > kStopForNativeFactor
          && current_native_bytes > stop_for_native_allocs_) {
        if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
          LOG(INFO) << "Stopping for native allocation, urgency: " << gc_urgency;
        }
        static constexpr int kGcWaitIters = 20;
        for (int i = 1; i <= kGcWaitIters; ++i) {
          if (!GCNumberLt(GetCurrentGcNum(), max_gc_requested_.load(std::memory_order_relaxed))
              || WaitForGcToComplete(kGcCauseForNativeAlloc, self) != collector::kGcTypeNone) {
            break;
          }
          CHECK(GCNumberLt(starting_gc_num, max_gc_requested_.load(std::memory_order_relaxed)));
          if (i % 10 == 0) {
            LOG(WARNING) << "Slept " << i << " times in native allocation, waiting for GC";
          }
          static constexpr int kGcWaitSleepMicros = 2000;
          usleep(kGcWaitSleepMicros);
        }
      }
    } else {
      CollectGarbageInternal(NonStickyGcType(), kGcCauseForNativeAlloc, false, starting_gc_num + 1);
    }
  }
}
void Heap::NotifyNativeAllocations(JNIEnv* env) {
  native_objects_notified_.fetch_add(kNotifyNativeInterval, std::memory_order_relaxed);
  CheckGCForNative(Thread::ForEnv(env));
}
void Heap::RegisterNativeAllocation(JNIEnv* env, size_t bytes) {
  DCHECK(sizeof(size_t) < 8 || bytes < (std::numeric_limits<size_t>::max() / 2));
  native_bytes_registered_.fetch_add(bytes, std::memory_order_relaxed);
  uint32_t objects_notified =
      native_objects_notified_.fetch_add(1, std::memory_order_relaxed);
  if (objects_notified % kNotifyNativeInterval == kNotifyNativeInterval - 1
      || bytes > kCheckImmediatelyThreshold) {
    CheckGCForNative(Thread::ForEnv(env));
  }
  JHPCheckNonTlabSampleAllocation(Thread::Current(), nullptr, bytes);
}
void Heap::RegisterNativeFree(JNIEnv*, size_t bytes) {
  size_t allocated;
  size_t new_freed_bytes;
  do {
    allocated = native_bytes_registered_.load(std::memory_order_relaxed);
    new_freed_bytes = std::min(allocated, bytes);
    DCHECK_EQ(new_freed_bytes, bytes);
  } while (!native_bytes_registered_.CompareAndSetWeakRelaxed(allocated,
                                                              allocated - new_freed_bytes));
}
size_t Heap::GetTotalMemory() const {
  return std::max(target_footprint_.load(std::memory_order_relaxed), GetBytesAllocated());
}
void Heap::AddModUnionTable(accounting::ModUnionTable* mod_union_table) {
  DCHECK(mod_union_table != nullptr);
  mod_union_tables_.Put(mod_union_table->GetSpace(), mod_union_table);
}
void Heap::CheckPreconditionsForAllocObject(ObjPtr<mirror::Class> c, size_t byte_count) {
  CHECK(c == nullptr || (c->IsClassClass() && byte_count >= sizeof(mirror::Class)) ||
        (c->IsVariableSize() ||
            RoundUp(c->GetObjectSize(), kObjectAlignment) ==
                RoundUp(byte_count, kObjectAlignment)))
      << "ClassFlags=" << c->GetClassFlags()
      << " IsClassClass=" << c->IsClassClass()
      << " byte_count=" << byte_count
      << " IsVariableSize=" << c->IsVariableSize()
      << " ObjectSize=" << c->GetObjectSize()
      << " sizeof(Class)=" << sizeof(mirror::Class)
      << " " << verification_->DumpObjectInfo(c.Ptr(), "klass");
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
    if (space->GetLiveBitmap() != nullptr && !space->HasBoundBitmaps()) {
      space->GetMarkBitmap()->Clear();
    }
  }
  for (const auto& space : GetDiscontinuousSpaces()) {
    space->GetMarkBitmap()->Clear();
  }
}
void Heap::SetAllocationRecords(AllocRecordObjectMap* records) {
  allocation_records_.reset(records);
}
void Heap::VisitAllocationRecords(RootVisitor* visitor) const {
  if (IsAllocTrackingEnabled()) {
    MutexLock mu(Thread::Current(), *Locks::alloc_tracker_lock_);
    if (IsAllocTrackingEnabled()) {
      GetAllocationRecords()->VisitRoots(visitor);
    }
  }
}
void Heap::SweepAllocationRecords(IsMarkedVisitor* visitor) const {
  if (IsAllocTrackingEnabled()) {
    MutexLock mu(Thread::Current(), *Locks::alloc_tracker_lock_);
    if (IsAllocTrackingEnabled()) {
      GetAllocationRecords()->SweepAllocationRecords(visitor);
    }
  }
}
void Heap::AllowNewAllocationRecords() const {
  CHECK(!gUseReadBarrier);
  MutexLock mu(Thread::Current(), *Locks::alloc_tracker_lock_);
  AllocRecordObjectMap* allocation_records = GetAllocationRecords();
  if (allocation_records != nullptr) {
    allocation_records->AllowNewAllocationRecords();
  }
}
void Heap::DisallowNewAllocationRecords() const {
  CHECK(!gUseReadBarrier);
  MutexLock mu(Thread::Current(), *Locks::alloc_tracker_lock_);
  AllocRecordObjectMap* allocation_records = GetAllocationRecords();
  if (allocation_records != nullptr) {
    allocation_records->DisallowNewAllocationRecords();
  }
}
void Heap::BroadcastForNewAllocationRecords() const {
  MutexLock mu(Thread::Current(), *Locks::alloc_tracker_lock_);
  AllocRecordObjectMap* allocation_records = GetAllocationRecords();
  if (allocation_records != nullptr) {
    allocation_records->BroadcastForNewAllocationRecords();
  }
}
void Heap::InitPerfettoJavaHeapProf() {
  uint32_t heap_id = 1;
#ifdef ART_TARGET_ANDROID
  AHeapInfo* info = AHeapInfo_create("com.android.art");
  AHeapInfo_setEnabledCallback(info, &EnableHeapSamplerCallback, &heap_sampler_);
  AHeapInfo_setDisabledCallback(info, &DisableHeapSamplerCallback, &heap_sampler_);
  heap_id = AHeapProfile_registerHeap(info);
#else
  heap_sampler_.EnableHeapSampler();
#endif
  heap_sampler_.SetHeapID(heap_id);
  VLOG(heap) << "Java Heap Profiler Initialized";
}
int Heap::CheckPerfettoJHPEnabled() {
  return GetHeapSampler().IsEnabled();
}
void Heap::JHPCheckNonTlabSampleAllocation(Thread* self, mirror::Object* obj, size_t alloc_size) {
  bool take_sample = false;
  size_t bytes_until_sample = 0;
  HeapSampler& prof_heap_sampler = GetHeapSampler();
  if (prof_heap_sampler.IsEnabled()) {
    prof_heap_sampler.GetSampleOffset(alloc_size,
                                      self->GetTlabPosOffset(),
                                      &take_sample,
                                      &bytes_until_sample);
    prof_heap_sampler.SetBytesUntilSample(bytes_until_sample);
    if (take_sample) {
      prof_heap_sampler.ReportSample(obj, alloc_size);
    }
    VLOG(heap) << "JHP:NonTlab Non-moving or Large Allocation or RegisterNativeAllocation";
  }
}
size_t Heap::JHPCalculateNextTlabSize(Thread* self,
                                      size_t jhp_def_tlab_size,
                                      size_t alloc_size,
                                      bool* take_sample,
                                      size_t* bytes_until_sample) {
  size_t next_tlab_size = jhp_def_tlab_size;
  if (CheckPerfettoJHPEnabled()) {
    size_t next_sample_point =
        GetHeapSampler().GetSampleOffset(alloc_size,
                                         self->GetTlabPosOffset(),
                                         take_sample,
                                         bytes_until_sample);
    next_tlab_size = std::min(next_sample_point, jhp_def_tlab_size);
  }
  return next_tlab_size;
}
void Heap::AdjustSampleOffset(size_t adjustment) {
  GetHeapSampler().AdjustSampleOffset(adjustment);
}
void Heap::CheckGcStressMode(Thread* self, ObjPtr<mirror::Object>* obj) {
  DCHECK(gc_stress_mode_);
  auto* const runtime = Runtime::Current();
  if (runtime->GetClassLinker()->IsInitialized() && !runtime->IsActiveTransaction()) {
    bool new_backtrace = false;
    {
      static constexpr size_t kMaxFrames = 16u;
      MutexLock mu(self, *backtrace_lock_);
      FixedSizeBacktrace<kMaxFrames> backtrace;
      backtrace.Collect( 2);
      uint64_t hash = backtrace.Hash();
      new_backtrace = seen_backtraces_.find(hash) == seen_backtraces_.end();
      if (new_backtrace) {
        seen_backtraces_.insert(hash);
      }
    }
    if (new_backtrace) {
      StackHandleScope<1> hs(self);
      auto h = hs.NewHandleWrapper(obj);
      CollectGarbage( false);
      unique_backtrace_count_.fetch_add(1);
    } else {
      seen_backtrace_count_.fetch_add(1);
    }
  }
}
void Heap::DisableGCForShutdown() {
  MutexLock mu(Thread::Current(), *gc_complete_lock_);
  gc_disabled_for_shutdown_ = true;
}
bool Heap::IsGCDisabledForShutdown() const {
  MutexLock mu(Thread::Current(), *gc_complete_lock_);
  return gc_disabled_for_shutdown_;
}
bool Heap::ObjectIsInBootImageSpace(ObjPtr<mirror::Object> obj) const {
  DCHECK_EQ(IsBootImageAddress(obj.Ptr()),
            any_of(boot_image_spaces_.begin(),
                   boot_image_spaces_.end(),
                   [obj](gc::space::ImageSpace* space) REQUIRES_SHARED(Locks::mutator_lock_) {
                     return space->HasAddress(obj.Ptr());
                   }));
  return IsBootImageAddress(obj.Ptr());
}
bool Heap::IsInBootImageOatFile(const void* p) const {
  DCHECK_EQ(IsBootImageAddress(p),
            any_of(boot_image_spaces_.begin(),
                   boot_image_spaces_.end(),
                   [p](gc::space::ImageSpace* space) REQUIRES_SHARED(Locks::mutator_lock_) {
                     return space->GetOatFile()->Contains(p);
                   }));
  return IsBootImageAddress(p);
}
void Heap::SetAllocationListener(AllocationListener* l) {
  AllocationListener* old = GetAndOverwriteAllocationListener(&alloc_listener_, l);
  if (old == nullptr) {
    Runtime::Current()->GetInstrumentation()->InstrumentQuickAllocEntryPoints();
  }
}
void Heap::RemoveAllocationListener() {
  AllocationListener* old = GetAndOverwriteAllocationListener(&alloc_listener_, nullptr);
  if (old != nullptr) {
    Runtime::Current()->GetInstrumentation()->UninstrumentQuickAllocEntryPoints();
  }
}
void Heap::SetGcPauseListener(GcPauseListener* l) {
  gc_pause_listener_.store(l, std::memory_order_relaxed);
}
void Heap::RemoveGcPauseListener() {
  gc_pause_listener_.store(nullptr, std::memory_order_relaxed);
}
mirror::Object* Heap::AllocWithNewTLAB(Thread* self,
                                       AllocatorType allocator_type,
                                       size_t alloc_size,
                                       bool grow,
                                       size_t* bytes_allocated,
                                       size_t* usable_size,
                                       size_t* bytes_tl_bulk_allocated) {
  mirror::Object* ret = nullptr;
  bool take_sample = false;
  size_t bytes_until_sample = 0;
  if (kUsePartialTlabs && alloc_size <= self->TlabRemainingCapacity()) {
    DCHECK_GT(alloc_size, self->TlabSize());
    const size_t min_expand_size = alloc_size - self->TlabSize();
    size_t next_tlab_size = JHPCalculateNextTlabSize(self,
                                                     kPartialTlabSize,
                                                     alloc_size,
                                                     &take_sample,
                                                     &bytes_until_sample);
    const size_t expand_bytes = std::max(
        min_expand_size,
        std::min(self->TlabRemainingCapacity() - self->TlabSize(), next_tlab_size));
    if (UNLIKELY(IsOutOfMemoryOnAllocation(allocator_type, expand_bytes, grow))) {
      return nullptr;
    }
    *bytes_tl_bulk_allocated = expand_bytes;
    self->ExpandTlab(expand_bytes);
    DCHECK_LE(alloc_size, self->TlabSize());
  } else if (allocator_type == kAllocatorTypeTLAB) {
    DCHECK(bump_pointer_space_ != nullptr);
    size_t def_pr_tlab_size = RoundDown(alloc_size + kDefaultTLABSize, kPageSize) - alloc_size;
    size_t next_tlab_size = JHPCalculateNextTlabSize(self,
                                                     def_pr_tlab_size,
                                                     alloc_size,
                                                     &take_sample,
                                                     &bytes_until_sample);
    const size_t new_tlab_size = alloc_size + next_tlab_size;
    if (UNLIKELY(IsOutOfMemoryOnAllocation(allocator_type, new_tlab_size, grow))) {
      return nullptr;
    }
    if (!bump_pointer_space_->AllocNewTlab(self, new_tlab_size)) {
      return nullptr;
    }
    *bytes_tl_bulk_allocated = new_tlab_size;
    if (CheckPerfettoJHPEnabled()) {
      VLOG(heap) << "JHP:kAllocatorTypeTLAB, New Tlab bytes allocated= " << new_tlab_size;
    }
  } else {
    DCHECK(allocator_type == kAllocatorTypeRegionTLAB);
    DCHECK(region_space_ != nullptr);
    if (space::RegionSpace::kRegionSize >= alloc_size) {
      if (LIKELY(!IsOutOfMemoryOnAllocation(allocator_type,
                                            space::RegionSpace::kRegionSize,
                                            grow))) {
        size_t def_pr_tlab_size = kUsePartialTlabs
                                      ? kPartialTlabSize
                                      : gc::space::RegionSpace::kRegionSize;
        size_t next_pr_tlab_size = JHPCalculateNextTlabSize(self,
                                                            def_pr_tlab_size,
                                                            alloc_size,
                                                            &take_sample,
                                                            &bytes_until_sample);
        const size_t new_tlab_size = kUsePartialTlabs
            ? std::max(alloc_size, next_pr_tlab_size)
            : next_pr_tlab_size;
        if (!region_space_->AllocNewTlab(self, new_tlab_size, bytes_tl_bulk_allocated)) {
          ret = region_space_->AllocNonvirtual<false>(alloc_size,
                                                      bytes_allocated,
                                                      usable_size,
                                                      bytes_tl_bulk_allocated);
          JHPCheckNonTlabSampleAllocation(self, ret, alloc_size);
          return ret;
        }
      } else {
        if (!IsOutOfMemoryOnAllocation(allocator_type, alloc_size, grow)) {
          ret = region_space_->AllocNonvirtual<false>(alloc_size,
                                                      bytes_allocated,
                                                      usable_size,
                                                      bytes_tl_bulk_allocated);
          JHPCheckNonTlabSampleAllocation(self, ret, alloc_size);
          return ret;
        }
        return nullptr;
      }
    } else {
      if (LIKELY(!IsOutOfMemoryOnAllocation(allocator_type, alloc_size, grow))) {
        ret = region_space_->AllocNonvirtual<false>(alloc_size,
                                                    bytes_allocated,
                                                    usable_size,
                                                    bytes_tl_bulk_allocated);
        JHPCheckNonTlabSampleAllocation(self, ret, alloc_size);
        return ret;
      }
      return nullptr;
    }
  }
  ret = self->AllocTlab(alloc_size);
  DCHECK(ret != nullptr);
  *bytes_allocated = alloc_size;
  *usable_size = alloc_size;
  if (CheckPerfettoJHPEnabled()) {
    if (take_sample) {
      GetHeapSampler().ReportSample(ret, alloc_size);
      GetHeapSampler().SetBytesUntilSample(bytes_until_sample);
    }
    VLOG(heap) << "JHP:Fallthrough Tlab allocation";
  }
  return ret;
}
const Verification* Heap::GetVerification() const {
  return verification_.get();
}
void Heap::VlogHeapGrowth(size_t old_footprint, size_t new_footprint, size_t alloc_size) {
  VLOG(heap) << "Growing heap from " << PrettySize(old_footprint) << " to "
             << PrettySize(new_footprint) << " for a " << PrettySize(alloc_size) << " allocation";
}
class Heap::TriggerPostForkCCGcTask : public HeapTask {
 public:
  explicit TriggerPostForkCCGcTask(uint64_t target_time, uint32_t initial_gc_num) :
      HeapTask(target_time), initial_gc_num_(initial_gc_num) {}
  void Run(Thread* self) override {
    gc::Heap* heap = Runtime::Current()->GetHeap();
    if (heap->GetCurrentGcNum() == initial_gc_num_) {
      if (kLogAllGCs) {
        LOG(INFO) << "Forcing GC for allocation-inactive process";
      }
      heap->RequestConcurrentGC(self, kGcCauseBackground, false, initial_gc_num_);
    }
  }
 private:
  uint32_t initial_gc_num_;
};
class Heap::ReduceTargetFootprintTask : public HeapTask {
 public:
  explicit ReduceTargetFootprintTask(uint64_t target_time, size_t new_target_sz,
                                     uint32_t initial_gc_num) :
      HeapTask(target_time), new_target_sz_(new_target_sz), initial_gc_num_(initial_gc_num) {}
  void Run(Thread* self) override {
    gc::Heap* heap = Runtime::Current()->GetHeap();
    MutexLock mu(self, *(heap->gc_complete_lock_));
    if (heap->GetCurrentGcNum() == initial_gc_num_
        && heap->collector_type_running_ == kCollectorTypeNone) {
      size_t target_footprint = heap->target_footprint_.load(std::memory_order_relaxed);
      if (target_footprint > new_target_sz_) {
        if (heap->target_footprint_.CompareAndSetStrongRelaxed(target_footprint, new_target_sz_)) {
          heap->SetDefaultConcurrentStartBytesLocked();
        }
      }
    }
  }
 private:
  size_t new_target_sz_;
  uint32_t initial_gc_num_;
};
static uint32_t GetPseudoRandomFromUid() {
  std::default_random_engine rng(getuid());
  std::uniform_int_distribution<int> dist(0, 19999);
  return dist(rng);
}
void Heap::PostForkChildAction(Thread* self) {
  uint32_t starting_gc_num = GetCurrentGcNum();
  uint64_t last_adj_time = NanoTime();
  next_gc_type_ = NonStickyGcType();
  LOG(INFO) << "Using " << foreground_collector_type_ << " GC.";
  if (gUseUserfaultfd) {
    DCHECK_NE(mark_compact_, nullptr);
    mark_compact_->CreateUserfaultfd( true);
  }
  SetIdealFootprint(growth_limit_);
  SetDefaultConcurrentStartBytes();
  if (initial_heap_size_ < growth_limit_) {
    size_t first_shrink_size = std::max(growth_limit_ / 4, initial_heap_size_);
    last_adj_time += MsToNs(kPostForkMaxHeapDurationMS);
    GetTaskProcessor()->AddTask(
        self, new ReduceTargetFootprintTask(last_adj_time, first_shrink_size, starting_gc_num));
    if (initial_heap_size_ < first_shrink_size) {
      last_adj_time += MsToNs(4 * kPostForkMaxHeapDurationMS);
      GetTaskProcessor()->AddTask(
          self,
          new ReduceTargetFootprintTask(last_adj_time, initial_heap_size_, starting_gc_num));
    }
  }
  uint64_t post_fork_gc_time = last_adj_time
      + MsToNs(4 * kPostForkMaxHeapDurationMS + GetPseudoRandomFromUid());
  GetTaskProcessor()->AddTask(self,
                              new TriggerPostForkCCGcTask(post_fork_gc_time, starting_gc_num));
}
void Heap::VisitReflectiveTargets(ReflectiveValueVisitor *visit) {
  VisitObjectsPaused([&visit](mirror::Object* ref) NO_THREAD_SAFETY_ANALYSIS {
    art::ObjPtr<mirror::Class> klass(ref->GetClass());
    if (!klass->IsBootStrapClassLoaded()) {
      return;
    }
    if (GetClassRoot<mirror::Method>()->IsAssignableFrom(klass) ||
        GetClassRoot<mirror::Constructor>()->IsAssignableFrom(klass)) {
      down_cast<mirror::Executable*>(ref)->VisitTarget(visit);
    } else if (art::GetClassRoot<art::mirror::Field>() == klass) {
      down_cast<mirror::Field*>(ref)->VisitTarget(visit);
    } else if (art::GetClassRoot<art::mirror::MethodHandle>()->IsAssignableFrom(klass)) {
      down_cast<mirror::MethodHandle*>(ref)->VisitTarget(visit);
    } else if (art::GetClassRoot<art::mirror::StaticFieldVarHandle>()->IsAssignableFrom(klass)) {
      down_cast<mirror::StaticFieldVarHandle*>(ref)->VisitTarget(visit);
    } else if (art::GetClassRoot<art::mirror::FieldVarHandle>()->IsAssignableFrom(klass)) {
      down_cast<mirror::FieldVarHandle*>(ref)->VisitTarget(visit);
    } else if (art::GetClassRoot<art::mirror::DexCache>()->IsAssignableFrom(klass)) {
      down_cast<mirror::DexCache*>(ref)->VisitReflectiveTargets(visit);
    }
  });
}
bool Heap::AddHeapTask(gc::HeapTask* task) {
  Thread* const self = Thread::Current();
  if (!CanAddHeapTask(self)) {
    return false;
  }
  GetTaskProcessor()->AddTask(self, task);
  return true;
}
}
}
