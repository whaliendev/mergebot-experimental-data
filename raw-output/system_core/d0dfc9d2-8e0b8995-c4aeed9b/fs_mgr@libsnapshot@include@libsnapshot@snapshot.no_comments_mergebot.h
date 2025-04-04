       
#include <stdint.h>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>
#include <android-base/unique_fd.h>
#include <android/snapshot/snapshot.pb.h>
#include <fs_mgr_dm_linear.h>
#include <libdm/dm.h>
#include <libfiemap/image_manager.h>
#include <liblp/builder.h>
#include <liblp/liblp.h>
#include <update_engine/update_metadata.pb.h>
#include <libsnapshot/auto_device.h>
#include <libsnapshot/return.h>
#ifndef FRIEND_TEST
#define FRIEND_TEST(test_set_name,individual_test) \
    friend class test_set_name##_##individual_test##_Test
#define DEFINED_FRIEND_TEST 
#endif
namespace android {
namespace fiemap {
class IImageManager;
}
namespace fs_mgr {
struct CreateLogicalPartitionParams;
class IPartitionOpener;
}
namespace hardware {
namespace boot {
namespace V1_1 {
enum class MergeStatus : int32_t;
}
}
}
namespace snapshot {
struct AutoDeleteCowImage;
struct AutoDeleteSnapshot;
struct AutoDeviceList;
struct PartitionCowCreator;
class SnapshotStatus;
static constexpr const std::string_view kCowGroupName = "cow";
bool OptimizeSourceCopyOperation(const chromeos_update_engine::InstallOperation& operation,
                                 chromeos_update_engine::InstallOperation* optimized);
enum class CreateResult : unsigned int {
ERROR,CREATED,NOT_CREATED,};
class SnapshotManager final {
    using CreateLogicalPartitionParams = android::fs_mgr::CreateLogicalPartitionParams;
    using IPartitionOpener = android::fs_mgr::IPartitionOpener;
    using LpMetadata = android::fs_mgr::LpMetadata;
    using MetadataBuilder = android::fs_mgr::MetadataBuilder;
    using DeltaArchiveManifest = chromeos_update_engine::DeltaArchiveManifest;
    using MergeStatus = android::hardware::boot::V1_1::MergeStatus;
    using FiemapStatus = android::fiemap::FiemapStatus;
    friend class SnapshotMergeStats;
public:
    class IDeviceInfo {
      public:
        virtual ~IDeviceInfo() {}
        virtual std::string GetGsidDir() const = 0;
        virtual std::string GetMetadataDir() const = 0;
        virtual std::string GetSlotSuffix() const = 0;
        virtual std::string GetOtherSlotSuffix() const = 0;
        virtual std::string GetSuperDevice(uint32_t slot) const = 0;
        virtual const IPartitionOpener& GetPartitionOpener() const = 0;
        virtual bool IsOverlayfsSetup() const = 0;
        virtual bool SetBootControlMergeStatus(MergeStatus status) = 0;
        virtual bool SetSlotAsUnbootable(unsigned int slot) = 0;
        virtual bool IsRecovery() const = 0;
    };
    ~SnapshotManager();
    static std::unique_ptr<SnapshotManager> New(IDeviceInfo* device = nullptr);
    static std::unique_ptr<SnapshotManager> NewForFirstStageMount(IDeviceInfo* device = nullptr);
    static bool IsSnapshotManagerNeeded();
    static std::string GetGlobalRollbackIndicatorPath();
    bool BeginUpdate();
    bool CancelUpdate();
    bool FinishedSnapshotWrites(bool wipe);
    bool InitiateMerge();
    UpdateState ProcessUpdateState(const std::function<bool()>& callback = {},
                                   const std::function<bool()>& before_cancel = {});
    UpdateState GetUpdateState(double* progress = nullptr);
    Return CreateUpdateSnapshots(const DeltaArchiveManifest& manifest);
    bool MapUpdateSnapshot(const CreateLogicalPartitionParams& params, std::string* snapshot_path);
    bool UnmapUpdateSnapshot(const std::string& target_partition_name);
    bool NeedSnapshotsInFirstStageMount();
    bool CreateLogicalAndSnapshotPartitions(const std::string& super_device,
                                            const std::chrono::milliseconds& timeout_ms = {});
    bool HandleImminentDataWipe(const std::function<void()>& callback = {});
    CreateResult RecoveryCreateSnapshotDevices();
    CreateResult RecoveryCreateSnapshotDevices(const std::unique_ptr<AutoDevice>& metadata_device);
    bool Dump(std::ostream& os);
    std::unique_ptr<AutoDevice> EnsureMetadataMounted();
private:
    FRIEND_TEST(SnapshotUpdateTest, SnapshotStatusFileWithoutCow);
    FRIEND_TEST(SnapshotUpdateTest, SnapshotStatusFileWithoutCow);
    FRIEND_TEST(SnapshotUpdateTest, SnapshotStatusFileWithoutCow);
    FRIEND_TEST(SnapshotUpdateTest, SnapshotStatusFileWithoutCow);
    FRIEND_TEST(SnapshotUpdateTest, SnapshotStatusFileWithoutCow);
    FRIEND_TEST(SnapshotUpdateTest, SnapshotStatusFileWithoutCow);
    FRIEND_TEST(SnapshotUpdateTest, SnapshotStatusFileWithoutCow);
    FRIEND_TEST(SnapshotUpdateTest, SnapshotStatusFileWithoutCow);
    FRIEND_TEST(SnapshotUpdateTest, SnapshotStatusFileWithoutCow);
    FRIEND_TEST(SnapshotUpdateTest, SnapshotStatusFileWithoutCow);
    FRIEND_TEST(SnapshotUpdateTest, SnapshotStatusFileWithoutCow);
    FRIEND_TEST(SnapshotUpdateTest, SnapshotStatusFileWithoutCow);
    FRIEND_TEST(SnapshotUpdateTest, SnapshotStatusFileWithoutCow);
    FRIEND_TEST(SnapshotUpdateTest, SnapshotStatusFileWithoutCow);
    FRIEND_TEST(SnapshotUpdateTest, SnapshotStatusFileWithoutCow);
    FRIEND_TEST(SnapshotUpdateTest, SnapshotStatusFileWithoutCow);
    FRIEND_TEST(SnapshotUpdateTest, SnapshotStatusFileWithoutCow);
    friend class SnapshotTest;
    friend class SnapshotUpdateTest;
    friend class FlashAfterUpdateTest;
    friend class LockTestConsumer;
    friend struct AutoDeleteCowImage;
    friend struct AutoDeleteSnapshot;
    friend struct PartitionCowCreator;
    using DmTargetSnapshot = android::dm::DmTargetSnapshot;
    using IImageManager = android::fiemap::IImageManager;
    using TargetInfo = android::dm::DeviceMapper::TargetInfo;
    explicit SnapshotManager(IDeviceInfo* info);
    bool EnsureImageManager();
    bool ForceLocalImageManager();
    IImageManager* image_manager() const { return images_.get(); }
    class LockedFile final {
      public:
        LockedFile(const std::string& path, android::base::unique_fd&& fd, int lock_mode)
            : path_(path), fd_(std::move(fd)), lock_mode_(lock_mode) {}
        ~LockedFile();
        int lock_mode() const { return lock_mode_; }
      private:
        std::string path_;
        android::base::unique_fd fd_;
        int lock_mode_;
    };
    static std::unique_ptr<LockedFile> OpenFile(const std::string& file, int lock_flags);
    bool CreateSnapshot(LockedFile* lock, SnapshotStatus* status);
    Return CreateCowImage(LockedFile* lock, const std::string& name);
    bool MapSnapshot(LockedFile* lock, const std::string& name, const std::string& base_device,
                     const std::string& cow_device, const std::chrono::milliseconds& timeout_ms,
                     std::string* dev_path);
    std::optional<std::string> MapCowImage(const std::string& name,
                                           const std::chrono::milliseconds& timeout_ms);
    bool DeleteSnapshot(LockedFile* lock, const std::string& name);
    bool UnmapSnapshot(LockedFile* lock, const std::string& name);
    bool UnmapCowImage(const std::string& name);
    bool RemoveAllSnapshots(LockedFile* lock);
    bool ListSnapshots(LockedFile* lock, std::vector<std::string>* snapshots);
    bool HandleCancelledUpdate(LockedFile* lock, const std::function<bool()>& before_cancel);
    bool AreAllSnapshotsCancelled(LockedFile* lock);
    bool GetSnapshotFlashingStatus(LockedFile* lock, const std::vector<std::string>& snapshots,
                                   std::map<std::string, bool>* out);
    bool RemoveAllUpdateState(LockedFile* lock, const std::function<bool()>& prolog = {});
    std::unique_ptr<LockedFile> OpenLock(int lock_flags);
    std::unique_ptr<LockedFile> LockShared();
    std::unique_ptr<LockedFile> LockExclusive();
    std::string GetLockPath() const;
    UpdateState ReadUpdateState(LockedFile* file);
    SnapshotUpdateStatus ReadSnapshotUpdateStatus(LockedFile* file);
    bool WriteUpdateState(LockedFile* file, UpdateState state);
    bool WriteSnapshotUpdateStatus(LockedFile* file, const SnapshotUpdateStatus& status);
    std::string GetStateFilePath() const;
    std::string GetMergeStateFilePath() const;
    bool SwitchSnapshotToMerge(LockedFile* lock, const std::string& name);
    bool RewriteSnapshotDeviceTable(const std::string& dm_name);
    bool MarkSnapshotMergeCompleted(LockedFile* snapshot_lock, const std::string& snapshot_name);
    void AcknowledgeMergeSuccess(LockedFile* lock);
    void AcknowledgeMergeFailure();
    std::unique_ptr<LpMetadata> ReadCurrentMetadata();
    enum class MetadataPartitionState {
        None,
        Flashed,
        Updated,
    };
    MetadataPartitionState GetMetadataPartitionState(const LpMetadata& metadata,
                                                     const std::string& name);
    bool QuerySnapshotStatus(const std::string& dm_name, std::string* target_type,
                             DmTargetSnapshot::Status* status);
    bool IsSnapshotDevice(const std::string& dm_name, TargetInfo* target = nullptr);
    bool OnSnapshotMergeComplete(LockedFile* lock, const std::string& name,
                                 const SnapshotStatus& status);
    bool CollapseSnapshotDevice(const std::string& name, const SnapshotStatus& status);
    UpdateState CheckMergeState(const std::function<bool()>& before_cancel);
    UpdateState CheckMergeState(LockedFile* lock, const std::function<bool()>& before_cancel);
    UpdateState CheckTargetMergeState(LockedFile* lock, const std::string& name);
    bool WriteSnapshotStatus(LockedFile* lock, const SnapshotStatus& status);
    bool ReadSnapshotStatus(LockedFile* lock, const std::string& name, SnapshotStatus* status);
    std::string GetSnapshotStatusFilePath(const std::string& name);
    std::string GetSnapshotBootIndicatorPath();
    std::string GetRollbackIndicatorPath();
    std::string GetForwardMergeIndicatorPath();
    std::string GetSnapshotDeviceName(const std::string& snapshot_name,
                                      const SnapshotStatus& status);
    bool MapPartitionWithSnapshot(LockedFile* lock, CreateLogicalPartitionParams params,
                                  std::string* path);
    bool MapCowDevices(LockedFile* lock, const CreateLogicalPartitionParams& params,
                       const SnapshotStatus& snapshot_status, AutoDeviceList* created_devices,
                       std::string* cow_name);
    bool UnmapCowDevices(LockedFile* lock, const std::string& name);
    bool UnmapPartitionWithSnapshot(LockedFile* lock, const std::string& target_partition_name);
    bool TryCancelUpdate(bool* needs_merge);
    Return CreateUpdateSnapshotsInternal(
            LockedFile* lock, const DeltaArchiveManifest& manifest,
            PartitionCowCreator* cow_creator, AutoDeviceList* created_devices,
            std::map<std::string, SnapshotStatus>* all_snapshot_status);
    Return InitializeUpdateSnapshots(
            LockedFile* lock, MetadataBuilder* target_metadata,
            const LpMetadata* exported_target_metadata, const std::string& target_suffix,
            const std::map<std::string, SnapshotStatus>& all_snapshot_status);
    bool UnmapAllPartitions();
    bool EnsureNoOverflowSnapshot(LockedFile* lock);
    enum class Slot { Unknown, Source, Target };
    friend std::ostream& operator<<(std::ostream& os, SnapshotManager::Slot slot);
    Slot GetCurrentSlot();
    std::string ReadUpdateSourceSlotSuffix();
    bool ShouldDeleteSnapshot(LockedFile* lock, const std::map<std::string, bool>& flashing_status,
                              Slot current_slot, const std::string& name);
    bool UpdateForwardMergeIndicator(bool wipe);
    bool ProcessUpdateStateOnDataWipe(bool allow_forward_merge,
                                      const std::function<bool()>& callback);
    std::string gsid_dir_;
    std::string metadata_dir_;
    std::unique_ptr<IDeviceInfo> device_;
    std::unique_ptr<IImageManager> images_;
    bool has_local_image_manager_ = false;
};
}
}
#ifdef DEFINED_FRIEND_TEST
#undef DEFINED_FRIEND_TEST
#undef FRIEND_TEST
#endif
