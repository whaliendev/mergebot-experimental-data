#ifndef ART_RUNTIME_GC_COLLECTOR_MARK_COMPACT_H_
#define ART_RUNTIME_GC_COLLECTOR_MARK_COMPACT_H_ 
#include <signal.h>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include "barrier.h"
#include "base/atomic.h"
#include "base/gc_visited_arena_pool.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "garbage_collector.h"
#include "gc/accounting/atomic_stack.h"
#include "gc/accounting/bitmap-inl.h"
#include "gc/accounting/heap_bitmap.h"
#include "gc_root.h"
#include "immune_spaces.h"
#include "offsets.h"
namespace art {
bool KernelSupportsUffd();
namespace mirror {
class DexCache;
}
namespace gc {
class Heap;
namespace space {
class BumpPointerSpace;
}
namespace collector {
class MarkCompact final : public GarbageCollector {
 private:
  using ObjReference = mirror::CompressedReference<mirror::Object>;
 public:
  using SigbusCounterType = uint32_t;
  static constexpr size_t kAlignment = kObjectAlignment;
  static constexpr int kCopyMode = -1;
  static constexpr int kMinorFaultMode = -2;
  static constexpr int kFallbackMode = -3;
  static constexpr int kFdSharedAnon = -1;
  static constexpr int kFdUnused = -2;
  static constexpr SigbusCounterType kSigbusCounterCompactionDoneMask =
      1u << (BitSizeOf<SigbusCounterType>() - 1);
  explicit MarkCompact(Heap* heap);
  ~MarkCompact() {}
  void RunPhases() overrideprivate : DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
 public:
  bool IsCompacting() const { return compacting_; }
  bool IsUsingSigbusFeature() const { return use_uffd_sigbus_; }
  bool SigbusHandler(siginfo_t* info) private : DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
 public:
  GcType GetGcType() const override { return kGcTypeFull; }
  CollectorType GetCollectorType() const override { return kCollectorTypeCMC; }
  Barrier& GetBarrier() { return gc_barrier_; }
  mirror::Object* MarkObject(mirror::Object* obj) override private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
 public:
  void MarkHeapReference(mirror::HeapReference<mirror::Object>* obj, bool do_atomic_update) override
      private : DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
 public:
  void VisitRoots(mirror::Object*** roots, size_t count, const RootInfo& info) override private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
 public:
  void VisitRoots(mirror::CompressedReference<mirror::Object>** roots,
                  size_t count,
                  const RootInfo& info) override private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
 public:
  bool IsNullOrMarkedHeapReference(mirror::HeapReference<mirror::Object>* obj,
                                   bool do_atomic_update) override private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
 public:
  void RevokeAllThreadLocalBuffers() override;
  void DelayReferenceReferent(ObjPtr<mirror::Class> klass,
                              ObjPtr<mirror::Reference> reference) override private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
 public:
  mirror::Object* IsMarked(mirror::Object* obj) override private
      : DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
 public:
  void CompactionPause() private : DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
 public:
  mirror::Object* GetFromSpaceAddrFromBarrier(mirror::Object* old_ref) {
    CHECK(compacting_);
    if (live_words_bitmap_->HasAddress(old_ref)) {
      return GetFromSpaceAddr(old_ref);
    }
    return old_ref;
  }
  bool CreateUserfaultfd(bool post_fork);
  static std::pair<bool, bool> GetUffdAndMinorFault();
  void AddLinearAllocSpaceData(uint8_t* begin, size_t len);
  enum class PageState : uint8_t {
    kUnprocessed = 0,
    kProcessing = 1,
    kProcessed = 2,
    kProcessingAndMapping = 3,
    kMutatorProcessing = 4,
    kProcessedAndMapping = 5,
    kProcessedAndMapped = 6
  };
 private:
  static constexpr uint32_t kBitsPerVectorWord = kBitsPerIntPtrT;
  static constexpr uint32_t kOffsetChunkSize = kBitsPerVectorWord * kAlignment;
  static_assert(kOffsetChunkSize < kPageSize);
  template <size_t kAlignment>
  class LiveWordsBitmap : private accounting::MemoryRangeBitmap<kAlignment> {
    using Bitmap = accounting::Bitmap;
    using MemRangeBitmap = accounting::MemoryRangeBitmap<kAlignment>;
   public:
    static_assert(IsPowerOfTwo(kBitsPerVectorWord));
    static_assert(IsPowerOfTwo(Bitmap::kBitsPerBitmapWord));
    static_assert(kBitsPerVectorWord >= Bitmap::kBitsPerBitmapWord);
    static constexpr uint32_t kBitmapWordsPerVectorWord =
        kBitsPerVectorWord / Bitmap::kBitsPerBitmapWord;
    static_assert(IsPowerOfTwo(kBitmapWordsPerVectorWord));
    static LiveWordsBitmap* Create(uintptr_t begin, uintptr_t end);
    uint32_t FindNthLiveWordOffset(size_t chunk_idx, uint32_t n) const;
    REQUIRES_SHARED(Locks::mutator_lock_);
    REQUIRES_SHARED(Locks::mutator_lock_);
    REQUIRES_SHARED(Locks::mutator_lock_);
    size_t LiveBytesInBitmapWord(size_t chunk_idx) const;
    void ClearBitmap() { Bitmap::Clear(); }
    ALWAYS_INLINE uintptr_t GetWord(size_t index) const {
      static_assert(kBitmapWordsPerVectorWord == 1);
      return Bitmap::Begin()[index * kBitmapWordsPerVectorWord];
    }
    ALWAYS_INLINE uintptr_t GetWord(size_t index) const {
      static_assert(kBitmapWordsPerVectorWord == 1);
      return Bitmap::Begin()[index * kBitmapWordsPerVectorWord];
    }
    ALWAYS_INLINE uintptr_t GetWord(size_t index) const {
      static_assert(kBitmapWordsPerVectorWord == 1);
      return Bitmap::Begin()[index * kBitmapWordsPerVectorWord];
    }
    ALWAYS_INLINE uintptr_t GetWord(size_t index) const {
      static_assert(kBitmapWordsPerVectorWord == 1);
      return Bitmap::Begin()[index * kBitmapWordsPerVectorWord];
    }
    ALWAYS_INLINE uintptr_t GetWord(size_t index) const {
      static_assert(kBitmapWordsPerVectorWord == 1);
      return Bitmap::Begin()[index * kBitmapWordsPerVectorWord];
    }
  };
  mirror::Object* GetFromSpaceAddr(mirror::Object* obj) const {
    DCHECK(live_words_bitmap_->HasAddress(obj)) << " obj=" << obj;
    return reinterpret_cast<mirror::Object*>(reinterpret_cast<uintptr_t>(obj) +
                                             from_space_slide_diff_);
  }
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  bool IsValidObject(mirror::Object* obj) const DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void InitializePhase();
  void FinishPhase() DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void MarkingPhase() DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void CompactionPhase() DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void SweepSystemWeaks(Thread* self, Runtime* runtime, const bool paused)
      DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  ALWAYS_INLINE bool VerifyRootSingleUpdate(void* root,
                                            mirror::Object* old_ref,
                                            const RootInfo& info)
      DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  ALWAYS_INLINE void UpdateRoot(mirror::CompressedReference<mirror::Object>* root,
                                const RootInfo& info = RootInfo(RootType::kRootUnknown))
      DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  ALWAYS_INLINE void UpdateRoot(mirror::Object** root,
                                const RootInfo& info = RootInfo(RootType::kRootUnknown))
      DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  ALWAYS_INLINE mirror::Object* PostCompactAddress(mirror::Object* old_ref)
      DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void BindAndResetBitmaps() DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void MarkingPause() DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void PrepareForCompaction() DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void CompactPage(mirror::Object* obj, uint32_t offset, uint8_t* addr, bool needs_memset_zero)
      DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void UpdateNonMovingPage(mirror::Object* first, uint8_t* page)
      DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void UpdateNonMovingSpace() DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void InitNonMovingSpaceFirstObjects() DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void InitMovingSpaceFirstObjects(const size_t vec_len)
      DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void UpdateMovingSpaceBlackAllocations() DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void UpdateNonMovingSpaceBlackAllocations() DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void SlideBlackPage(mirror::Object* first_obj,
                      const size_t page_idx,
                      uint8_t* const pre_compact_page,
                      uint8_t* dest,
                      bool needs_memset_zero) DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void ReclaimPhase() DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void ReMarkRoots(Runtime* runtime) DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void MarkRoots(VisitRootFlags flags) DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void MarkRootsCheckpoint(Thread* self, Runtime* runtime)
      DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void PreCleanCards() DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void MarkNonThreadRoots(Runtime* runtime) DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void MarkConcurrentRoots(VisitRootFlags flags, Runtime* runtime)
      DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void MarkReachableObjects() DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void UpdateAndMarkModUnion() DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void ScanDirtyObjects(bool paused, uint8_t minimum_age)
      DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void RecursiveMarkDirtyObjects(bool paused, uint8_t minimum_age)
      DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void ProcessMarkStack() override DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void ExpandMarkStack() DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void PushOnMarkStack(mirror::Object* obj) DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void UpdateLivenessInfo(mirror::Object* obj) DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void ProcessReferences(Thread* self) DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void MarkObjectNonNull(mirror::Object* obj,
                         mirror::Object* holder = nullptr,
                         MemberOffset offset = MemberOffset(0))
      DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void MarkObject(mirror::Object* obj, mirror::Object* holder, MemberOffset offset)
      DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void Sweep(bool swap_bitmaps) DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void SweepLargeObjects(bool swap_bitmaps) DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void KernelPreparation();
  void KernelPrepareRangeForUffd(uint8_t* to_addr,
                                 uint8_t* from_addr,
                                 size_t map_size,
                                 int fd,
                                 uint8_t* shadow_addr = nullptr);
  void RegisterUffd(void* addr, size_t size, int mode);
  void UnregisterUffd(uint8_t* start, size_t len);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void ProcessLinearAlloc() DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  bool CanCompactMovingSpaceWithMinorFault();
  void FreeFromSpacePages(size_t cur_page_idx) DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  bool IsValidFd(int fd) const { return fd >= 0; }
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void UpdateClassAfterObjMap();
  void MarkZygoteLargeObjects() DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
  void ZeropageIoctl(void* addr, bool tolerate_eexist, bool tolerate_enoent);
  void CopyIoctl(void* dst, void* buffer);
  void MapUpdatedLinearAllocPage(uint8_t* page,
                                 uint8_t* shadow_page,
                                 Atomic<PageState>& state,
                                 bool page_touched);
  Barrier gc_barrier_;
  ImmuneSpaces immune_spaces_;
  Mutex lock_;
  accounting::ObjectStack* mark_stack_;
  std::unique_ptr<LiveWordsBitmap<kAlignment>> live_words_bitmap_;
  std::unique_ptr<std::unordered_set<void*>> updated_roots_ MemMap from_space_map_;
  MemMap shadow_to_space_map_;
  MemMap info_map_;
  MemMap compaction_buffers_map_;
  class LessByArenaAddr {
   public:
    bool operator()(const TrackedArena* a, const TrackedArena* b) const {
      return std::less<uint8_t*>{}(a->Begin(), b->Begin());
    }
  };
  std::map<const TrackedArena*, uint8_t*, LessByArenaAddr> linear_alloc_arenas_;
  class LinearAllocSpaceData {
   public:
    LinearAllocSpaceData(MemMap&& shadow,
                         MemMap&& page_status_map,
                         uint8_t* begin,
                         uint8_t* end,
                         bool already_shared)
        : shadow_(std::move(shadow)),
          page_status_map_(std::move(page_status_map)),
          begin_(begin),
          end_(end),
          already_shared_(already_shared) {}
    MemMap shadow_;
    MemMap page_status_map_;
    uint8_t* begin_;
    uint8_t* end_;
    bool already_shared_;
  };
  std::vector<LinearAllocSpaceData> linear_alloc_spaces_data_;
  class ObjReferenceHash {
   public:
    uint32_t operator()(const ObjReference& ref) const {
      return ref.AsVRegValue() >> kObjectAlignmentShift;
    }
  };
  class ObjReferenceEqualFn {
   public:
    bool operator()(const ObjReference& a, const ObjReference& b) const {
      return a.AsMirrorPtr() == b.AsMirrorPtr();
    }
  };
  class LessByObjReference {
   public:
    bool operator()(const ObjReference& a, const ObjReference& b) const {
      return std::less<mirror::Object*>{}(a.AsMirrorPtr(), b.AsMirrorPtr());
    }
  };
  using ObjObjOrderedMap = std::map<ObjReference, ObjReference, LessByObjReference>;
  using ObjObjUnorderedMap =
      std::unordered_map<ObjReference, ObjReference, ObjReferenceHash, ObjReferenceEqualFn>;
  ObjObjUnorderedMap super_class_after_class_hash_map_;
  ObjObjUnorderedMap class_after_obj_hash_map_;
  ObjObjOrderedMap class_after_obj_ordered_map_;
  ObjObjOrderedMap::const_reverse_iterator class_after_obj_iter_;
  mirror::Class* walk_super_class_cache_;
  size_t last_checked_reclaim_page_idx_;
  uint8_t* last_reclaimed_page_;
  space::ContinuousSpace* non_moving_space_;
  space::BumpPointerSpace* const bump_pointer_space_;
  accounting::ContinuousSpaceBitmap* const moving_space_bitmap_;
  accounting::ContinuousSpaceBitmap* non_moving_space_bitmap_;
  Thread* thread_running_gc_;
  Atomic<PageState>* moving_pages_status_;
  size_t vector_length_;
  size_t live_stack_freeze_size_;
  uint32_t* chunk_info_vec_;
  union {
    uint32_t* pre_compact_offset_moving_space_;
    uint32_t* black_alloc_pages_first_chunk_size_;
  };
  ObjReference* first_objs_moving_space_;
  ObjReference* first_objs_non_moving_space_;
  size_t non_moving_first_objs_count_;
  size_t moving_first_objs_count_;
  size_t black_page_count_;
  uint8_t* from_space_begin_;
  uint8_t* black_allocations_begin_;
  uint8_t* post_compact_end_;
  ptrdiff_t black_objs_slide_diff_;
  ptrdiff_t from_space_slide_diff_;
  void* stack_high_addr_;
  void* stack_low_addr_;
  uint8_t* conc_compaction_termination_page_;
  PointerSize pointer_size_;
  int32_t freed_objects_;
  int moving_to_space_fd_;
  int moving_from_space_fd_;
  int uffd_;
  std::atomic<SigbusCounterType> sigbus_in_progress_count_;
  std::atomic<uint16_t> compaction_in_progress_count_;
  std::atomic<uint16_t> compaction_buffer_counter_;
  uint8_t thread_pool_counter_;
  bool compacting_;
  bool uffd_initialized_;
  const bool uffd_minor_fault_supported_;
  const bool use_uffd_sigbus_;
  bool minor_fault_initialized_;
  bool map_linear_alloc_shared_;
  class FlipCallback;
  class ThreadFlipVisitor;
  class VerifyRootMarkedVisitor;
  class ScanObjectVisitor;
  class CheckpointMarkThreadRoots;
  template <size_t kBufferSize>
  class ThreadRootsVisitor;
  class CardModifiedVisitor;
  class RefFieldsVisitor;
  template <bool kCheckBegin, bool kCheckEnd>
  class RefsUpdateVisitor;
  class ArenaPoolPageUpdater;
  class ClassLoaderRootsUpdater;
  class LinearAllocPageUpdater;
  class ImmuneSpaceUpdateObjVisitor;
  class ConcurrentCompactionGcTask;
  DISALLOW_IMPLICIT_CONSTRUCTORS(MarkCompact);
};
std::ostream& operator<<(std::ostream& os, MarkCompact::PageState value);
}
}
}
#endif
