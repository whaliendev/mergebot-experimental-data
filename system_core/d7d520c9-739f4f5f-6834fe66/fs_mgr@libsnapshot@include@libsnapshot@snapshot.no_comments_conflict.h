       
#include <stdint.h>
#include <unistd.h>
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
#include <libsnapshot/snapshot_writer.h>
#include <snapuserd/snapuserd_client.h>
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
class ISnapshotMergeStats;
class SnapshotMergeStats;
class SnapshotStatus;
static constexpr const std::string_view kCowGroupName = "cow";
static constexpr char kVirtualAbCompressionProp[] = "ro.virtual_ab.compression.enabled";
bool OptimizeSourceCopyOperation(const chromeos_update_engine::InstallOperation& operation,
                                 chromeos_update_engine::InstallOperation* optimized);
enum class CreateResult : unsigned int {
    ERROR,
    CREATED,
    NOT_CREATED,
};
class ISnapshotManager {
  public:
    class IDeviceInfo {
      public:
        using IImageManager = android::fiemap::IImageManager;
        virtual ~IDeviceInfo() {}
        virtual std::string GetMetadataDir() const = 0;
        virtual std::string GetSlotSuffix() const = 0;
        virtual std::string GetOtherSlotSuffix() const = 0;
        virtual std::string GetSuperDevice(uint32_t slot) const = 0;
        virtual const android::fs_mgr::IPartitionOpener& GetPartitionOpener() const = 0;
        virtual bool IsOverlayfsSetup() const = 0;
        virtual bool SetBootControlMergeStatus(
                android::hardware::boot::V1_1::MergeStatus status) = 0;
        virtual bool SetSlotAsUnbootable(unsigned int slot) = 0;
        virtual bool IsRecovery() const = 0;
        virtual bool IsTestDevice() const { return false; }
        virtual bool IsFirstStageInit() const = 0;
        virtual std::unique_ptr<IImageManager> OpenImageManager() const = 0;
        virtual android::dm::IDeviceMapper& GetDeviceMapper() = 0;
        std::unique_ptr<IImageManager> OpenImageManager(const std::string& gsid_dir) const;
    };
    virtual ~ISnapshotManager() = default;
    virtual bool BeginUpdate() = 0;
    virtual bool CancelUpdate() = 0;
    virtual bool FinishedSnapshotWrites(bool wipe) = 0;
    virtual void UpdateCowStats(ISnapshotMergeStats* stats) = 0;
    virtual bool InitiateMerge() = 0;
    virtual UpdateState ProcessUpdateState(const std::function<bool()>& callback = {},
                                           const std::function<bool()>& before_cancel = {}) = 0;
    virtual MergeFailureCode ReadMergeFailureCode() = 0;
    virtual std::string ReadSourceBuildFingerprint() = 0;
    virtual UpdateState GetUpdateState(double* progress = nullptr) = 0;
    virtual bool UpdateUsesCompression() = 0;
    virtual bool UpdateUsesUserSnapshots() = 0;
    virtual Return CreateUpdateSnapshots(
            const chromeos_update_engine::DeltaArchiveManifest& manifest) = 0;
    virtual bool MapUpdateSnapshot(const android::fs_mgr::CreateLogicalPartitionParams& params,
                                   std::string* snapshot_path) = 0;
    virtual std::unique_ptr<ISnapshotWriter> OpenSnapshotWriter(
            const android::fs_mgr::CreateLogicalPartitionParams& params,
            const std::optional<std::string>& source_device) = 0;
    virtual bool UnmapUpdateSnapshot(const std::string& target_partition_name) = 0;
    virtual bool NeedSnapshotsInFirstStageMount() = 0;
    virtual bool CreateLogicalAndSnapshotPartitions(
            const std::string& super_device, const std::chrono::milliseconds& timeout_ms = {}) = 0;
    virtual bool MapAllSnapshots(const std::chrono::milliseconds& timeout_ms = {}) = 0;
    virtual bool UnmapAllSnapshots() = 0;
    virtual bool HandleImminentDataWipe(const std::function<void()>& callback = {}) = 0;
    virtual bool FinishMergeInRecovery() = 0;
    virtual CreateResult RecoveryCreateSnapshotDevices() = 0;
    virtual CreateResult RecoveryCreateSnapshotDevices(
            const std::unique_ptr<AutoDevice>& metadata_device) = 0;
    virtual bool Dump(std::ostream& os) = 0;
    virtual std::unique_ptr<AutoDevice> EnsureMetadataMounted() = 0;
    virtual ISnapshotMergeStats* GetSnapshotMergeStatsInstance() = 0;
};
class SnapshotManager final : public ISnapshotManager {
    using CreateLogicalPartitionParams = android::fs_mgr::CreateLogicalPartitionParams;
    using IPartitionOpener = android::fs_mgr::IPartitionOpener;
    using LpMetadata = android::fs_mgr::LpMetadata;
    using MetadataBuilder = android::fs_mgr::MetadataBuilder;
    using DeltaArchiveManifest = chromeos_update_engine::DeltaArchiveManifest;
    using MergeStatus = android::hardware::boot::V1_1::MergeStatus;
    using FiemapStatus = android::fiemap::FiemapStatus;
    friend class SnapshotMergeStats;
  public:
    ~SnapshotManager();
    static std::unique_ptr<SnapshotManager> New(IDeviceInfo* device = nullptr);
    static std::unique_ptr<SnapshotManager> NewForFirstStageMount(IDeviceInfo* device = nullptr);
    static bool IsSnapshotManagerNeeded();
    static std::string GetGlobalRollbackIndicatorPath();
    bool DetachSnapuserdForSelinux(std::vector<std::string>* snapuserd_argv);
    bool PerformSecondStageInitTransition();
    bool BeginUpdate() override;
    bool CancelUpdate() override;
    bool FinishedSnapshotWrites(bool wipe) override;
    void UpdateCowStats(ISnapshotMergeStats* stats) override;
    MergeFailureCode ReadMergeFailureCode() override;
    bool InitiateMerge() override;
    UpdateState ProcessUpdateState(const std::function<bool()>& callback = {},
                                   const std::function<bool()>& before_cancel = {}) override;
    UpdateState GetUpdateState(double* progress = nullptr) override;
    bool UpdateUsesCompression() override;
    bool UpdateUsesUserSnapshots() override;
    Return CreateUpdateSnapshots(const DeltaArchiveManifest& manifest) override;
    bool MapUpdateSnapshot(const CreateLogicalPartitionParams& params,
                           std::string* snapshot_path) override;
    std::unique_ptr<ISnapshotWriter> OpenSnapshotWriter(
            const android::fs_mgr::CreateLogicalPartitionParams& params,
            const std::optional<std::string>& source_device) override;
    bool UnmapUpdateSnapshot(const std::string& target_partition_name) override;
    bool NeedSnapshotsInFirstStageMount() override;
    bool CreateLogicalAndSnapshotPartitions(
            const std::string& super_device,
            const std::chrono::milliseconds& timeout_ms = {}) override;
    bool HandleImminentDataWipe(const std::function<void()>& callback = {}) override;
    bool FinishMergeInRecovery() override;
    CreateResult RecoveryCreateSnapshotDevices() override;
    CreateResult RecoveryCreateSnapshotDevices(
            const std::unique_ptr<AutoDevice>& metadata_device) override;
    bool Dump(std::ostream& os) override;
    std::unique_ptr<AutoDevice> EnsureMetadataMounted() override;
    ISnapshotMergeStats* GetSnapshotMergeStatsInstance() override;
    bool MapAllSnapshots(const std::chrono::milliseconds& timeout_ms = {}) override;
    bool UnmapAllSnapshots() override;
    std::string ReadSourceBuildFingerprint() override;
    void SetUeventRegenCallback(std::function<bool(const std::string&)> callback) {
        uevent_regen_callback_ = callback;
    }
    bool IsSnapuserdRequired();
<<<<<<< HEAD
    enum class SnapshotDriver {
        DM_SNAPSHOT,
        DM_USER,
    };
    using MergeConsistencyChecker =
            std::function<MergeFailureCode(const std::string& name, const SnapshotStatus& status)>;
    void set_merge_consistency_checker(MergeConsistencyChecker checker) {
        merge_consistency_checker_ = checker;
    }
    MergeConsistencyChecker merge_consistency_checker() const { return merge_consistency_checker_; }
||||||| 6834fe66d
=======
    using MergeConsistencyChecker =
            std::function<MergeFailureCode(const std::string& name, const SnapshotStatus& status)>;
    void set_merge_consistency_checker(MergeConsistencyChecker checker) {
        merge_consistency_checker_ = checker;
    }
    MergeConsistencyChecker merge_consistency_checker() const { return merge_consistency_checker_; }
>>>>>>> 739f4f5f
  private:
    FRIEND_TEST(SnapshotTest, CleanFirstStageMount);
    FRIEND_TEST(SnapshotTest, CreateSnapshot);
    FRIEND_TEST(SnapshotTest, FirstStageMountAfterRollback);
    FRIEND_TEST(SnapshotTest, FirstStageMountAndMerge);
    FRIEND_TEST(SnapshotTest, FlashSuperDuringMerge);
    FRIEND_TEST(SnapshotTest, FlashSuperDuringUpdate);
    FRIEND_TEST(SnapshotTest, MapPartialSnapshot);
    FRIEND_TEST(SnapshotTest, MapSnapshot);
    FRIEND_TEST(SnapshotTest, Merge);
    FRIEND_TEST(SnapshotTest, MergeFailureCode);
    FRIEND_TEST(SnapshotTest, NoMergeBeforeReboot);
    FRIEND_TEST(SnapshotTest, UpdateBootControlHal);
    FRIEND_TEST(SnapshotUpdateTest, AddPartition);
    FRIEND_TEST(SnapshotUpdateTest, ConsistencyCheckResume);
    FRIEND_TEST(SnapshotUpdateTest, DaemonTransition);
    FRIEND_TEST(SnapshotUpdateTest, DataWipeAfterRollback);
    FRIEND_TEST(SnapshotUpdateTest, DataWipeRollbackInRecovery);
    FRIEND_TEST(SnapshotUpdateTest, DataWipeWithStaleSnapshots);
    FRIEND_TEST(SnapshotUpdateTest, FullUpdateFlow);
    FRIEND_TEST(SnapshotUpdateTest, MergeCannotRemoveCow);
    FRIEND_TEST(SnapshotUpdateTest, MergeInRecovery);
    FRIEND_TEST(SnapshotUpdateTest, QueryStatusError);
    FRIEND_TEST(SnapshotUpdateTest, SnapshotStatusFileWithoutCow);
    FRIEND_TEST(SnapshotUpdateTest, SpaceSwapUpdate);
    friend class SnapshotTest;
    friend class SnapshotUpdateTest;
    friend class FlashAfterUpdateTest;
    friend class LockTestConsumer;
    friend class SnapshotFuzzEnv;
    friend struct AutoDeleteCowImage;
    friend struct AutoDeleteSnapshot;
    friend struct PartitionCowCreator;
    using DmTargetSnapshot = android::dm::DmTargetSnapshot;
    using IImageManager = android::fiemap::IImageManager;
    using TargetInfo = android::dm::DeviceMapper::TargetInfo;
    explicit SnapshotManager(IDeviceInfo* info);
    bool EnsureImageManager();
    bool EnsureSnapuserdConnected();
    const std::unique_ptr<IDeviceInfo>& device() const { return device_; }
    IImageManager* image_manager() const { return images_.get(); }
    void set_use_first_stage_snapuserd(bool value) { use_first_stage_snapuserd_ = value; }
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
    SnapshotDriver GetSnapshotDriver(LockedFile* lock);
    bool CreateSnapshot(LockedFile* lock, PartitionCowCreator* cow_creator, SnapshotStatus* status);
    Return CreateCowImage(LockedFile* lock, const std::string& name);
    bool MapSnapshot(LockedFile* lock, const std::string& name, const std::string& base_device,
                     const std::string& cow_device, const std::chrono::milliseconds& timeout_ms,
                     std::string* dev_path);
    bool MapDmUserCow(LockedFile* lock, const std::string& name, const std::string& cow_file,
                      const std::string& base_device, const std::string& base_path_merge,
                      const std::chrono::milliseconds& timeout_ms, std::string* path);
    bool MapSourceDevice(LockedFile* lock, const std::string& name,
                         const std::chrono::milliseconds& timeout_ms, std::string* path);
    std::optional<std::string> MapCowImage(const std::string& name,
                                           const std::chrono::milliseconds& timeout_ms);
    bool DeleteSnapshot(LockedFile* lock, const std::string& name);
    bool UnmapSnapshot(LockedFile* lock, const std::string& name);
    bool UnmapCowImage(const std::string& name);
    void UnmapAndDeleteCowPartition(MetadataBuilder* current_metadata);
    bool RemoveAllSnapshots(LockedFile* lock);
    bool ListSnapshots(LockedFile* lock, std::vector<std::string>* snapshots,
                       const std::string& suffix = "");
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
    bool WriteUpdateState(LockedFile* file, UpdateState state,
                          MergeFailureCode failure_code = MergeFailureCode::Ok);
    bool WriteSnapshotUpdateStatus(LockedFile* file, const SnapshotUpdateStatus& status);
    std::string GetStateFilePath() const;
    std::string GetMergeStateFilePath() const;
    MergeFailureCode MergeSecondPhaseSnapshots(LockedFile* lock);
    MergeFailureCode SwitchSnapshotToMerge(LockedFile* lock, const std::string& name);
    MergeFailureCode RewriteSnapshotDeviceTable(const std::string& dm_name);
    bool MarkSnapshotMergeCompleted(LockedFile* snapshot_lock, const std::string& snapshot_name);
    void AcknowledgeMergeSuccess(LockedFile* lock);
    void AcknowledgeMergeFailure(MergeFailureCode failure_code);
    MergePhase DecideMergePhase(const SnapshotStatus& status);
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
    bool CollapseSnapshotDevice(LockedFile* lock, const std::string& name,
                                const SnapshotStatus& status);
    struct MergeResult {
        explicit MergeResult(UpdateState state,
                             MergeFailureCode failure_code = MergeFailureCode::Ok)
            : state(state), failure_code(failure_code) {}
        UpdateState state;
        MergeFailureCode failure_code;
    };
    MergeResult CheckMergeState(const std::function<bool()>& before_cancel);
    MergeResult CheckMergeState(LockedFile* lock, const std::function<bool()>& before_cancel);
    MergeResult CheckTargetMergeState(LockedFile* lock, const std::string& name,
                                      const SnapshotUpdateStatus& update_status);
    MergeFailureCode CheckMergeConsistency(LockedFile* lock, const std::string& name,
                                           const SnapshotStatus& update_status);
    enum class TableQuery {
        Table,
        Status,
    };
    bool GetSingleTarget(const std::string& dm_name, TableQuery query,
                         android::dm::DeviceMapper::TargetInfo* target);
    bool WriteSnapshotStatus(LockedFile* lock, const SnapshotStatus& status);
    bool ReadSnapshotStatus(LockedFile* lock, const std::string& name, SnapshotStatus* status);
    std::string GetSnapshotStatusFilePath(const std::string& name);
    std::string GetSnapshotBootIndicatorPath();
    std::string GetRollbackIndicatorPath();
    std::string GetForwardMergeIndicatorPath();
    std::string GetOldPartitionMetadataPath();
    const LpMetadata* ReadOldPartitionMetadata(LockedFile* lock);
    bool MapAllPartitions(LockedFile* lock, const std::string& super_device, uint32_t slot,
                          const std::chrono::milliseconds& timeout_ms);
    enum class SnapshotContext {
        Update,
        Mount,
    };
    struct SnapshotPaths {
        std::string target_device;
        std::string cow_device_name;
        std::string snapshot_device;
    };
    std::unique_ptr<ISnapshotWriter> OpenCompressedSnapshotWriter(
            LockedFile* lock, const std::optional<std::string>& source_device,
            const std::string& partition_name, const SnapshotStatus& status,
            const SnapshotPaths& paths);
    std::unique_ptr<ISnapshotWriter> OpenKernelSnapshotWriter(
            LockedFile* lock, const std::optional<std::string>& source_device,
            const std::string& partition_name, const SnapshotStatus& status,
            const SnapshotPaths& paths);
    bool MapPartitionWithSnapshot(LockedFile* lock, CreateLogicalPartitionParams params,
                                  SnapshotContext context, SnapshotPaths* paths);
    bool MapCowDevices(LockedFile* lock, const CreateLogicalPartitionParams& params,
                       const SnapshotStatus& snapshot_status, AutoDeviceList* created_devices,
                       std::string* cow_name);
    bool UnmapCowDevices(LockedFile* lock, const std::string& name);
    bool UnmapPartitionWithSnapshot(LockedFile* lock, const std::string& target_partition_name);
    bool UnmapDmUserDevice(const std::string& dm_user_name);
    bool UnmapUserspaceSnapshotDevice(LockedFile* lock, const std::string& snapshot_name);
    bool TryCancelUpdate(bool* needs_merge);
    Return CreateUpdateSnapshotsInternal(
            LockedFile* lock, const DeltaArchiveManifest& manifest,
            PartitionCowCreator* cow_creator, AutoDeviceList* created_devices,
            std::map<std::string, SnapshotStatus>* all_snapshot_status);
    Return InitializeUpdateSnapshots(
            LockedFile* lock, MetadataBuilder* target_metadata,
            const LpMetadata* exported_target_metadata, const std::string& target_suffix,
            const std::map<std::string, SnapshotStatus>& all_snapshot_status);
    bool UnmapAllSnapshots(LockedFile* lock);
    bool UnmapAllPartitionsInRecovery();
    bool EnsureNoOverflowSnapshot(LockedFile* lock);
    enum class Slot { Unknown, Source, Target };
    friend std::ostream& operator<<(std::ostream& os, SnapshotManager::Slot slot);
    Slot GetCurrentSlot();
    std::string GetSnapshotSlotSuffix();
    std::string ReadUpdateSourceSlotSuffix();
    bool ShouldDeleteSnapshot(const std::map<std::string, bool>& flashing_status, Slot current_slot,
                              const std::string& name);
    bool UpdateForwardMergeIndicator(bool wipe);
    UpdateState ProcessUpdateStateOnDataWipe(bool allow_forward_merge,
                                             const std::function<bool()>& callback);
    bool GetMappedImageDeviceStringOrPath(const std::string& device_name,
                                          std::string* device_string_or_mapped_path);
    bool GetMappedImageDevicePath(const std::string& device_name, std::string* device_path);
    bool WaitForDevice(const std::string& device, std::chrono::milliseconds timeout_ms);
    enum class InitTransition { SELINUX_DETACH, SECOND_STAGE };
    bool PerformInitTransition(InitTransition transition,
                               std::vector<std::string>* snapuserd_argv = nullptr);
    SnapuserdClient* snapuserd_client() const { return snapuserd_client_.get(); }
    bool UpdateUsesCompression(LockedFile* lock);
    bool UpdateUsesUserSnapshots(LockedFile* lock);
    bool DeleteDeviceIfExists(const std::string& name,
                              const std::chrono::milliseconds& timeout_ms = {});
    android::dm::IDeviceMapper& dm_;
    std::unique_ptr<IDeviceInfo> device_;
    std::string metadata_dir_;
    std::unique_ptr<IImageManager> images_;
    bool use_first_stage_snapuserd_ = false;
    bool in_factory_data_reset_ = false;
    std::function<bool(const std::string&)> uevent_regen_callback_;
    std::unique_ptr<SnapuserdClient> snapuserd_client_;
    std::unique_ptr<LpMetadata> old_partition_metadata_;
<<<<<<< HEAD
    std::optional<bool> is_snapshot_userspace_;
    MergeConsistencyChecker merge_consistency_checker_;
||||||| 6834fe66d
=======
    MergeConsistencyChecker merge_consistency_checker_;
>>>>>>> 739f4f5f
};
}
}
#ifdef DEFINED_FRIEND_TEST
#undef DEFINED_FRIEND_TEST
#undef FRIEND_TEST
#endif
