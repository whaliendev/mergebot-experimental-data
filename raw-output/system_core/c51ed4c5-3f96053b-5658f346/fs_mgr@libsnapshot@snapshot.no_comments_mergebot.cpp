#include <libsnapshot/snapshot.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/unistd.h>
#include <filesystem>
#include <optional>
#include <thread>
#include <unordered_set>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <cutils/sockets.h>
#include <ext4_utils/ext4_utils.h>
#include <fs_mgr.h>
#include <fs_mgr/file_wait.h>
#include <fs_mgr_dm_linear.h>
#include <fstab/fstab.h>
#include <libdm/dm.h>
#include <libfiemap/image_manager.h>
#include <liblp/liblp.h>
#include <android/snapshot/snapshot.pb.h>
#include <libsnapshot/snapshot_stats.h>
#include "device_info.h"
#include "partition_cow_creator.h"
#include "snapshot_metadata_updater.h"
#include "snapshot_reader.h"
#include "utility.h"
namespace android {
namespace snapshot {
using aidl::android::hardware::boot::MergeStatus;
using android::base::unique_fd;
using android::dm::DeviceMapper;
using android::dm::DmDeviceState;
using android::dm::DmTable;
using android::dm::DmTargetLinear;
using android::dm::DmTargetSnapshot;
using android::dm::DmTargetUser;
using android::dm::kSectorSize;
using android::dm::SnapshotStorageMode;
using android::fiemap::FiemapStatus;
using android::fiemap::IImageManager;
using android::fs_mgr::CreateDmTable;
using android::fs_mgr::CreateLogicalPartition;
using android::fs_mgr::CreateLogicalPartitionParams;
using android::fs_mgr::GetPartitionGroupName;
using android::fs_mgr::GetPartitionName;
using android::fs_mgr::LpMetadata;
using android::fs_mgr::MetadataBuilder;
using android::fs_mgr::SlotNumberForSlotSuffix;
using chromeos_update_engine::DeltaArchiveManifest;
using chromeos_update_engine::Extent;
using chromeos_update_engine::FileDescriptor;
using chromeos_update_engine::PartitionUpdate;
using std::chrono::duration_cast;
using namespace std::chrono_literals;
using namespace std::string_literals;
static constexpr char kBootIndicatorPath[] = "/metadata/ota/snapshot-boot";
static constexpr char kRollbackIndicatorPath[] = "/metadata/ota/rollback-indicator";
static constexpr auto kUpdateStateCheckInterval = 2s;
MergeFailureCode CheckMergeConsistency(const std::string& name, const SnapshotStatus& status);
SnapshotManager::~SnapshotManager() {}
std::unique_ptr<SnapshotManager> SnapshotManager::New(IDeviceInfo* info) {
    if (!info) {
        info = new DeviceInfo();
    }
    return std::unique_ptr<SnapshotManager>(new SnapshotManager(info));
}
std::unique_ptr<SnapshotManager> SnapshotManager::NewForFirstStageMount(IDeviceInfo* info) {
    if (!info) {
        DeviceInfo* impl = new DeviceInfo();
        impl->set_first_stage_init(true);
        info = impl;
    }
    auto sm = New(info);
    if (!sm->device()->IsTestDevice()) {
        sm->use_first_stage_snapuserd_ = true;
    }
    return sm;
}
SnapshotManager::SnapshotManager(IDeviceInfo* device)
    : dm_(device->GetDeviceMapper()), device_(device), metadata_dir_(device_->GetMetadataDir()) {
    merge_consistency_checker_ = android::snapshot::CheckMergeConsistency;
}
static std::string GetCowName(const std::string& snapshot_name) {
    return snapshot_name + "-cow";
}
SnapshotManager::SnapshotDriver SnapshotManager::GetSnapshotDriver(LockedFile* lock) {
    if (UpdateUsesUserSnapshots(lock)) {
        return SnapshotManager::SnapshotDriver::DM_USER;
    } else {
        return SnapshotManager::SnapshotDriver::DM_SNAPSHOT;
    }
}
static std::string GetDmUserCowName(const std::string& snapshot_name,
                                    SnapshotManager::SnapshotDriver driver) {
    switch (driver) {
        case SnapshotManager::SnapshotDriver::DM_USER: {
            return snapshot_name;
        }
        case SnapshotManager::SnapshotDriver::DM_SNAPSHOT: {
            return snapshot_name + "-user-cow";
        }
        default: {
            LOG(ERROR) << "Invalid snapshot driver";
            return "";
        }
    }
}
static std::string GetCowImageDeviceName(const std::string& snapshot_name) {
    return snapshot_name + "-cow-img";
}
static std::string GetBaseDeviceName(const std::string& partition_name) {
    return partition_name + "-base";
}
static std::string GetSourceDeviceName(const std::string& partition_name) {
    return partition_name + "-src";
}
bool SnapshotManager::BeginUpdate() {
    bool needs_merge = false;
    if (!TryCancelUpdate(&needs_merge)) {
        return false;
    }
    if (needs_merge) {
        LOG(INFO) << "Wait for merge (if any) before beginning a new update.";
        auto state = ProcessUpdateState();
        LOG(INFO) << "Merged with state = " << state;
    }
    auto file = LockExclusive();
    if (!file) return false;
    if (EnsureImageManager()) {
        images_->RemoveAllImages();
    }
    old_partition_metadata_ = nullptr;
    auto state = ReadUpdateState(file.get());
    if (state != UpdateState::None) {
        LOG(ERROR) << "An update is already in progress, cannot begin a new update";
        return false;
    }
    return WriteUpdateState(file.get(), UpdateState::Initiated);
}
bool SnapshotManager::CancelUpdate() {
    bool needs_merge = false;
    if (!TryCancelUpdate(&needs_merge)) {
        return false;
    }
    if (needs_merge) {
        LOG(ERROR) << "Cannot cancel update after it has completed or started merging";
    }
    return !needs_merge;
}
bool SnapshotManager::TryCancelUpdate(bool* needs_merge) {
    *needs_merge = false;
    auto file = LockExclusive();
    if (!file) return false;
    UpdateState state = ReadUpdateState(file.get());
    if (state == UpdateState::None) {
        RemoveInvalidSnapshots(file.get());
        return true;
    }
    if (state == UpdateState::Initiated) {
        LOG(INFO) << "Update has been initiated, now canceling";
        return RemoveAllUpdateState(file.get());
    }
    if (state == UpdateState::Unverified) {
        auto slot = GetCurrentSlot();
        if (slot != Slot::Target) {
            LOG(INFO) << "Canceling previously completed updates (if any)";
            return RemoveAllUpdateState(file.get());
        }
    }
    *needs_merge = true;
    return true;
}
std::string SnapshotManager::ReadUpdateSourceSlotSuffix() {
    auto boot_file = GetSnapshotBootIndicatorPath();
    std::string contents;
    if (!android::base::ReadFileToString(boot_file, &contents)) {
        PLOG(WARNING) << "Cannot read " << boot_file;
        return {};
    }
    return contents;
}
SnapshotManager::Slot SnapshotManager::GetCurrentSlot() {
    auto contents = ReadUpdateSourceSlotSuffix();
    if (contents.empty()) {
        return Slot::Unknown;
    }
    if (device_->GetSlotSuffix() == contents) {
        return Slot::Source;
    }
    return Slot::Target;
}
std::string SnapshotManager::GetSnapshotSlotSuffix() {
    switch (GetCurrentSlot()) {
        case Slot::Target:
            return device_->GetSlotSuffix();
        default:
            return device_->GetOtherSlotSuffix();
    }
}
static bool RemoveFileIfExists(const std::string& path) {
    std::string message;
    if (!android::base::RemoveFileIfExists(path, &message)) {
        LOG(ERROR) << "Remove failed: " << path << ": " << message;
        return false;
    }
    return true;
}
bool SnapshotManager::RemoveAllUpdateState(LockedFile* lock, const std::function<bool()>& prolog) {
    if (prolog && !prolog()) {
        LOG(WARNING) << "Can't RemoveAllUpdateState: prolog failed.";
        return false;
    }
    LOG(INFO) << "Removing all update state.";
    if (!RemoveAllSnapshots(lock)) {
        LOG(ERROR) << "Could not remove all snapshots";
        return false;
    }
    std::vector<std::string> files = {
            GetSnapshotBootIndicatorPath(),
            GetRollbackIndicatorPath(),
            GetForwardMergeIndicatorPath(),
            GetOldPartitionMetadataPath(),
    };
    for (const auto& file : files) {
        RemoveFileIfExists(file);
    }
    return WriteUpdateState(lock, UpdateState::None);
}
bool SnapshotManager::FinishedSnapshotWrites(bool wipe) {
    auto lock = LockExclusive();
    if (!lock) return false;
    auto update_state = ReadUpdateState(lock.get());
    if (update_state == UpdateState::Unverified) {
        LOG(INFO) << "FinishedSnapshotWrites already called before. Ignored.";
        return true;
    }
    if (update_state != UpdateState::Initiated) {
        LOG(ERROR) << "Can only transition to the Unverified state from the Initiated state.";
        return false;
    }
    if (!EnsureNoOverflowSnapshot(lock.get())) {
        LOG(ERROR) << "Cannot ensure there are no overflow snapshots.";
        return false;
    }
    if (!UpdateForwardMergeIndicator(wipe)) {
        return false;
    }
    if (!RemoveFileIfExists(GetRollbackIndicatorPath())) {
        return false;
    }
    auto contents = device_->GetSlotSuffix();
    auto boot_file = GetSnapshotBootIndicatorPath();
    if (!WriteStringToFileAtomic(contents, boot_file)) {
        PLOG(ERROR) << "write failed: " << boot_file;
        return false;
    }
    return WriteUpdateState(lock.get(), UpdateState::Unverified);
}
bool SnapshotManager::CreateSnapshot(LockedFile* lock, PartitionCowCreator* cow_creator,
                                     SnapshotStatus* status) {
    CHECK(lock);
    CHECK(lock->lock_mode() == LOCK_EX);
    CHECK(status);
    if (status->name().empty()) {
        LOG(ERROR) << "SnapshotStatus has no name.";
        return false;
    }
    if (status->device_size() % kSectorSize != 0) {
        LOG(ERROR) << "Snapshot " << status->name()
                   << " device size is not a multiple of the sector size: "
                   << status->device_size();
        return false;
    }
    if (status->snapshot_size() % kSectorSize != 0) {
        LOG(ERROR) << "Snapshot " << status->name()
                   << " snapshot size is not a multiple of the sector size: "
                   << status->snapshot_size();
        return false;
    }
    if (status->cow_partition_size() % kSectorSize != 0) {
        LOG(ERROR) << "Snapshot " << status->name()
                   << " cow partition size is not a multiple of the sector size: "
                   << status->cow_partition_size();
        return false;
    }
    if (status->cow_file_size() % kSectorSize != 0) {
        LOG(ERROR) << "Snapshot " << status->name()
                   << " cow file size is not a multiple of the sector size: "
                   << status->cow_file_size();
        return false;
    }
    status->set_state(SnapshotState::CREATED);
    status->set_sectors_allocated(0);
    status->set_metadata_sectors(0);
    status->set_using_snapuserd(cow_creator->using_snapuserd);
    status->set_compression_algorithm(cow_creator->compression_algorithm);
    if (cow_creator->enable_threading) {
        status->set_enable_threading(cow_creator->enable_threading);
    }
    if (cow_creator->batched_writes) {
        status->set_batched_writes(cow_creator->batched_writes);
    }
    if (!WriteSnapshotStatus(lock, *status)) {
        PLOG(ERROR) << "Could not write snapshot status: " << status->name();
        return false;
    }
    return true;
}
Return SnapshotManager::CreateCowImage(LockedFile* lock, const std::string& name) {
    CHECK(lock);
    CHECK(lock->lock_mode() == LOCK_EX);
    if (!EnsureImageManager()) return Return::Error();
    SnapshotStatus status;
    if (!ReadSnapshotStatus(lock, name, &status)) {
        return Return::Error();
    }
    if (status.cow_file_size() % kSectorSize != 0) {
        LOG(ERROR) << "Snapshot " << name << " COW file size is not a multiple of the sector size: "
                   << status.cow_file_size();
        return Return::Error();
    }
    std::string cow_image_name = GetCowImageDeviceName(name);
    int cow_flags = IImageManager::CREATE_IMAGE_DEFAULT;
    return Return(images_->CreateBackingImage(cow_image_name, status.cow_file_size(), cow_flags));
}
bool SnapshotManager::MapDmUserCow(LockedFile* lock, const std::string& name,
                                   const std::string& cow_file, const std::string& base_device,
                                   const std::string& base_path_merge,
                                   const std::chrono::milliseconds& timeout_ms, std::string* path) {
    CHECK(lock);
    if (UpdateUsesUserSnapshots(lock)) {
        SnapshotStatus status;
        if (!ReadSnapshotStatus(lock, name, &status)) {
            LOG(ERROR) << "MapDmUserCow: ReadSnapshotStatus failed...";
            return false;
        }
        if (status.state() == SnapshotState::NONE ||
            status.state() == SnapshotState::MERGE_COMPLETED) {
            LOG(ERROR) << "Should not create a snapshot device for " << name
                       << " after merging has completed.";
            return false;
        }
        SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock);
        if (update_status.state() == UpdateState::MergeCompleted ||
            update_status.state() == UpdateState::MergeNeedsReboot) {
            LOG(ERROR) << "Should not create a snapshot device for " << name
                       << " after global merging has completed.";
            return false;
        }
    }
    std::string misc_name = name;
    if (use_first_stage_snapuserd_) {
        misc_name += "-init";
    }
    if (!EnsureSnapuserdConnected()) {
        return false;
    }
    uint64_t base_sectors = 0;
    if (!UpdateUsesUserSnapshots(lock)) {
        base_sectors = snapuserd_client_->InitDmUserCow(misc_name, cow_file, base_device);
        if (base_sectors == 0) {
            LOG(ERROR) << "Failed to retrieve base_sectors from Snapuserd";
            return false;
        }
    } else {
        unique_fd fd(TEMP_FAILURE_RETRY(open(base_path_merge.c_str(), O_RDONLY | O_CLOEXEC)));
        if (fd < 0) {
            LOG(ERROR) << "Cannot open block device: " << base_path_merge;
            return false;
        }
        uint64_t dev_sz = get_block_device_size(fd.get());
        if (!dev_sz) {
            LOG(ERROR) << "Failed to find block device size: " << base_path_merge;
            return false;
        }
        base_sectors = dev_sz >> 9;
    }
    DmTable table;
    table.Emplace<DmTargetUser>(0, base_sectors, misc_name);
    if (!dm_.CreateDevice(name, table, path, timeout_ms)) {
        LOG(ERROR) << " dm-user: CreateDevice failed... ";
        return false;
    }
    if (!WaitForDevice(*path, timeout_ms)) {
        LOG(ERROR) << " dm-user: timeout: Failed to create block device for: " << name;
        return false;
    }
    auto control_device = "/dev/dm-user/" + misc_name;
    if (!WaitForDevice(control_device, timeout_ms)) {
        return false;
    }
    if (UpdateUsesUserSnapshots(lock)) {
        if (!snapuserd_client_->InitDmUserCow(misc_name, cow_file, base_device, base_path_merge)) {
            LOG(ERROR) << "InitDmUserCow failed";
            return false;
        }
    }
    return snapuserd_client_->AttachDmUser(misc_name);
}
bool SnapshotManager::MapSnapshot(LockedFile* lock, const std::string& name,
                                  const std::string& base_device, const std::string& cow_device,
                                  const std::chrono::milliseconds& timeout_ms,
                                  std::string* dev_path) {
    CHECK(lock);
    SnapshotStatus status;
    if (!ReadSnapshotStatus(lock, name, &status)) {
        return false;
    }
    if (status.state() == SnapshotState::NONE || status.state() == SnapshotState::MERGE_COMPLETED) {
        LOG(ERROR) << "Should not create a snapshot device for " << name
                   << " after merging has completed.";
        return false;
    }
    if (android::base::StartsWith(base_device, "/")) {
        unique_fd fd(open(base_device.c_str(), O_RDONLY | O_CLOEXEC));
        if (fd < 0) {
            PLOG(ERROR) << "open failed: " << base_device;
            return false;
        }
        auto dev_size = get_block_device_size(fd);
        if (!dev_size) {
            PLOG(ERROR) << "Could not determine block device size: " << base_device;
            return false;
        }
        if (status.device_size() != dev_size) {
            LOG(ERROR) << "Block device size for " << base_device << " does not match"
                       << "(expected " << status.device_size() << ", got " << dev_size << ")";
            return false;
        }
    }
    if (status.device_size() % kSectorSize != 0) {
        LOG(ERROR) << "invalid blockdev size for " << base_device << ": " << status.device_size();
        return false;
    }
    if (status.snapshot_size() % kSectorSize != 0 ||
        status.snapshot_size() > status.device_size()) {
        LOG(ERROR) << "Invalid snapshot size for " << base_device << ": " << status.snapshot_size();
        return false;
    }
    if (status.device_size() != status.snapshot_size()) {
        LOG(ERROR) << "Device size and snapshot size must be the same (device size = "
                   << status.device_size() << ", snapshot size = " << status.snapshot_size();
        return false;
    }
    uint64_t snapshot_sectors = status.snapshot_size() / kSectorSize;
    SnapshotStorageMode mode;
    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock);
    switch (update_status.state()) {
        case UpdateState::MergeCompleted:
        case UpdateState::MergeNeedsReboot:
            LOG(ERROR) << "Should not create a snapshot device for " << name
                       << " after global merging has completed.";
            return false;
        case UpdateState::Merging:
        case UpdateState::MergeFailed:
            if (DecideMergePhase(status) == update_status.merge_phase()) {
                mode = SnapshotStorageMode::Merge;
            } else {
                mode = SnapshotStorageMode::Persistent;
            }
            break;
        default:
            mode = SnapshotStorageMode::Persistent;
            break;
    }
    if (mode == SnapshotStorageMode::Persistent && status.state() == SnapshotState::MERGING) {
        LOG(ERROR) << "Snapshot: " << name
                   << " has snapshot status Merging but mode set to Persistent."
                   << " Changing mode to Snapshot-Merge.";
        mode = SnapshotStorageMode::Merge;
    }
    DmTable table;
    table.Emplace<DmTargetSnapshot>(0, snapshot_sectors, base_device, cow_device, mode,
                                    kSnapshotChunkSize);
    if (!dm_.CreateDevice(name, table, dev_path, timeout_ms)) {
        LOG(ERROR) << "Could not create snapshot device: " << name;
        return false;
    }
    return true;
}
std::optional<std::string> SnapshotManager::MapCowImage(
        const std::string& name, const std::chrono::milliseconds& timeout_ms) {
    if (!EnsureImageManager()) return std::nullopt;
    auto cow_image_name = GetCowImageDeviceName(name);
    bool ok;
    std::string cow_dev;
    if (device_->IsRecovery() || device_->IsFirstStageInit()) {
        const auto& opener = device_->GetPartitionOpener();
        ok = images_->MapImageWithDeviceMapper(opener, cow_image_name, &cow_dev);
    } else {
        ok = images_->MapImageDevice(cow_image_name, timeout_ms, &cow_dev);
    }
    if (ok) {
        LOG(INFO) << "Mapped " << cow_image_name << " to " << cow_dev;
        return cow_dev;
    }
    LOG(ERROR) << "Could not map image device: " << cow_image_name;
    return std::nullopt;
}
bool SnapshotManager::MapSourceDevice(LockedFile* lock, const std::string& name,
                                      const std::chrono::milliseconds& timeout_ms,
                                      std::string* path) {
    CHECK(lock);
    auto metadata = ReadOldPartitionMetadata(lock);
    if (!metadata) {
        LOG(ERROR) << "Could not map source device due to missing or corrupt metadata";
        return false;
    }
    auto old_name = GetOtherPartitionName(name);
    auto slot_suffix = device_->GetSlotSuffix();
    auto slot = SlotNumberForSlotSuffix(slot_suffix);
    CreateLogicalPartitionParams params = {
            .block_device = device_->GetSuperDevice(slot),
            .metadata = metadata,
            .partition_name = old_name,
            .timeout_ms = timeout_ms,
            .device_name = GetSourceDeviceName(name),
            .partition_opener = &device_->GetPartitionOpener(),
    };
    if (!CreateLogicalPartition(std::move(params), path)) {
        LOG(ERROR) << "Could not create source device for snapshot " << name;
        return false;
    }
    return true;
}
bool SnapshotManager::UnmapSnapshot(LockedFile* lock, const std::string& name) {
    CHECK(lock);
    if (UpdateUsesUserSnapshots(lock)) {
        if (!UnmapUserspaceSnapshotDevice(lock, name)) {
            return false;
        }
    } else {
        if (!DeleteDeviceIfExists(name)) {
            LOG(ERROR) << "Could not delete snapshot device: " << name;
            return false;
        }
    }
    return true;
}
bool SnapshotManager::UnmapCowImage(const std::string& name) {
    if (!EnsureImageManager()) return false;
    return images_->UnmapImageIfExists(GetCowImageDeviceName(name));
}
bool SnapshotManager::DeleteSnapshot(LockedFile* lock, const std::string& name) {
    CHECK(lock);
    CHECK(lock->lock_mode() == LOCK_EX);
    if (!EnsureImageManager()) return false;
    if (!UnmapCowDevices(lock, name)) {
        return false;
    }
    if (device_->IsRecovery()) {
        LOG(INFO) << "Skipping delete of snapshot " << name << " in recovery.";
        return true;
    }
    auto cow_image_name = GetCowImageDeviceName(name);
    if (images_->BackingImageExists(cow_image_name)) {
        if (!images_->DeleteBackingImage(cow_image_name)) {
            return false;
        }
    }
    std::string error;
    auto file_path = GetSnapshotStatusFilePath(name);
    if (!android::base::RemoveFileIfExists(file_path, &error)) {
        LOG(ERROR) << "Failed to remove status file " << file_path << ": " << error;
        return false;
    }
    return true;
}
bool SnapshotManager::InitiateMerge() {
    auto lock = LockExclusive();
    if (!lock) return false;
    UpdateState state = ReadUpdateState(lock.get());
    if (state != UpdateState::Unverified) {
        LOG(ERROR) << "Cannot begin a merge if an update has not been verified";
        return false;
    }
    auto slot = GetCurrentSlot();
    if (slot != Slot::Target) {
        LOG(ERROR) << "Device cannot merge while not booting from new slot";
        return false;
    }
    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock.get(), &snapshots)) {
        LOG(ERROR) << "Could not list snapshots";
        return false;
    }
    auto other_suffix = device_->GetOtherSlotSuffix();
    for (const auto& snapshot : snapshots) {
        if (android::base::EndsWith(snapshot, other_suffix)) {
            LOG(ERROR) << "Unexpected snapshot found during merge: " << snapshot;
            continue;
        }
        if (dm_.GetState(snapshot) == DmDeviceState::INVALID) {
            LOG(ERROR) << "Cannot begin merge; device " << snapshot << " is not mapped.";
            return false;
        }
    }
    auto metadata = ReadCurrentMetadata();
    for (auto it = snapshots.begin(); it != snapshots.end();) {
        switch (GetMetadataPartitionState(*metadata, *it)) {
            case MetadataPartitionState::Flashed:
                LOG(WARNING) << "Detected re-flashing for partition " << *it
                             << ". Skip merging it.";
                [[fallthrough]];
            case MetadataPartitionState::None: {
                LOG(WARNING) << "Deleting snapshot for partition " << *it;
                if (!DeleteSnapshot(lock.get(), *it)) {
                    LOG(WARNING) << "Cannot delete snapshot for partition " << *it
                                 << ". Skip merging it anyways.";
                }
                it = snapshots.erase(it);
            } break;
            case MetadataPartitionState::Updated: {
                ++it;
            } break;
        }
    }
    bool using_snapuserd = false;
    std::vector<std::string> first_merge_group;
    DmTargetSnapshot::Status initial_target_values = {};
    for (const auto& snapshot : snapshots) {
        if (!UpdateUsesUserSnapshots(lock.get())) {
            DmTargetSnapshot::Status current_status;
            if (!QuerySnapshotStatus(snapshot, nullptr, &current_status)) {
                return false;
            }
            initial_target_values.sectors_allocated += current_status.sectors_allocated;
            initial_target_values.total_sectors += current_status.total_sectors;
            initial_target_values.metadata_sectors += current_status.metadata_sectors;
        }
        SnapshotStatus snapshot_status;
        if (!ReadSnapshotStatus(lock.get(), snapshot, &snapshot_status)) {
            return false;
        }
        using_snapuserd |= snapshot_status.using_snapuserd();
        if (DecideMergePhase(snapshot_status) == MergePhase::FIRST_PHASE) {
            first_merge_group.emplace_back(snapshot);
        }
    }
    SnapshotUpdateStatus initial_status = ReadSnapshotUpdateStatus(lock.get());
    initial_status.set_state(UpdateState::Merging);
    initial_status.set_using_snapuserd(using_snapuserd);
    if (!UpdateUsesUserSnapshots(lock.get())) {
        initial_status.set_sectors_allocated(initial_target_values.sectors_allocated);
        initial_status.set_total_sectors(initial_target_values.total_sectors);
        initial_status.set_metadata_sectors(initial_target_values.metadata_sectors);
    }
    const std::vector<std::string>* merge_group;
    if (first_merge_group.empty()) {
        merge_group = &snapshots;
        initial_status.set_merge_phase(MergePhase::SECOND_PHASE);
    } else {
        merge_group = &first_merge_group;
        initial_status.set_merge_phase(MergePhase::FIRST_PHASE);
    }
    if (!WriteSnapshotUpdateStatus(lock.get(), initial_status)) {
        return false;
    }
    auto reported_code = MergeFailureCode::Ok;
    for (const auto& snapshot : *merge_group) {
        auto code = SwitchSnapshotToMerge(lock.get(), snapshot);
        if (code != MergeFailureCode::Ok) {
            LOG(ERROR) << "Failed to switch snapshot to a merge target: " << snapshot;
            if (reported_code == MergeFailureCode::Ok) {
                reported_code = code;
            }
        }
    }
    if (reported_code != MergeFailureCode::Ok) {
        WriteUpdateState(lock.get(), UpdateState::MergeFailed, reported_code);
    }
    return true;
}
MergeFailureCode SnapshotManager::SwitchSnapshotToMerge(LockedFile* lock, const std::string& name) {
    SnapshotStatus status;
    if (!ReadSnapshotStatus(lock, name, &status)) {
        return MergeFailureCode::ReadStatus;
    }
    if (status.state() != SnapshotState::CREATED) {
        LOG(WARNING) << "Snapshot " << name
                     << " has unexpected state: " << SnapshotState_Name(status.state());
    }
    if (UpdateUsesUserSnapshots(lock)) {
        if (EnsureSnapuserdConnected()) {
            if (!snapuserd_client_->InitiateMerge(name)) {
                return MergeFailureCode::UnknownTable;
            }
        } else {
            LOG(ERROR) << "Failed to connect to snapuserd daemon to initiate merge";
            return MergeFailureCode::UnknownTable;
        }
    } else {
        if (auto code = RewriteSnapshotDeviceTable(name); code != MergeFailureCode::Ok) {
            return code;
        }
    }
    status.set_state(SnapshotState::MERGING);
    if (!UpdateUsesUserSnapshots(lock)) {
        DmTargetSnapshot::Status dm_status;
        if (!QuerySnapshotStatus(name, nullptr, &dm_status)) {
            LOG(ERROR) << "Could not query merge status for snapshot: " << name;
        }
        status.set_sectors_allocated(dm_status.sectors_allocated);
        status.set_metadata_sectors(dm_status.metadata_sectors);
    }
    if (!WriteSnapshotStatus(lock, status)) {
        LOG(ERROR) << "Could not update status file for snapshot: " << name;
    }
    return MergeFailureCode::Ok;
}
MergeFailureCode SnapshotManager::RewriteSnapshotDeviceTable(const std::string& name) {
    std::vector<DeviceMapper::TargetInfo> old_targets;
    if (!dm_.GetTableInfo(name, &old_targets)) {
        LOG(ERROR) << "Could not read snapshot device table: " << name;
        return MergeFailureCode::GetTableInfo;
    }
    if (old_targets.size() != 1 || DeviceMapper::GetTargetType(old_targets[0].spec) != "snapshot") {
        LOG(ERROR) << "Unexpected device-mapper table for snapshot: " << name;
        return MergeFailureCode::UnknownTable;
    }
    std::string base_device, cow_device;
    if (!DmTargetSnapshot::GetDevicesFromParams(old_targets[0].data, &base_device, &cow_device)) {
        LOG(ERROR) << "Could not derive underlying devices for snapshot: " << name;
        return MergeFailureCode::GetTableParams;
    }
    DmTable table;
    table.Emplace<DmTargetSnapshot>(0, old_targets[0].spec.length, base_device, cow_device,
                                    SnapshotStorageMode::Merge, kSnapshotChunkSize);
    if (!dm_.LoadTableAndActivate(name, table)) {
        LOG(ERROR) << "Could not swap device-mapper tables on snapshot device " << name;
        return MergeFailureCode::ActivateNewTable;
    }
    LOG(INFO) << "Successfully switched snapshot device to a merge target: " << name;
    return MergeFailureCode::Ok;
}
bool SnapshotManager::GetSingleTarget(const std::string& dm_name, TableQuery query,
                                      DeviceMapper::TargetInfo* target) {
    if (dm_.GetState(dm_name) == DmDeviceState::INVALID) {
        return false;
    }
    std::vector<DeviceMapper::TargetInfo> targets;
    bool result;
    if (query == TableQuery::Status) {
        result = dm_.GetTableStatus(dm_name, &targets);
    } else {
        result = dm_.GetTableInfo(dm_name, &targets);
    }
    if (!result) {
        LOG(ERROR) << "Could not query device: " << dm_name;
        return false;
    }
    if (targets.size() != 1) {
        return false;
    }
    *target = std::move(targets[0]);
    return true;
}
bool SnapshotManager::IsSnapshotDevice(const std::string& dm_name, TargetInfo* target) {
    DeviceMapper::TargetInfo snap_target;
    if (!GetSingleTarget(dm_name, TableQuery::Status, &snap_target)) {
        return false;
    }
    auto type = DeviceMapper::GetTargetType(snap_target.spec);
    if (type != "user") {
        if (type != "snapshot" && type != "snapshot-merge") {
            return false;
        }
    }
    if (target) {
        *target = std::move(snap_target);
    }
    return true;
}
auto SnapshotManager::UpdateStateToStr(const enum UpdateState state) {
    switch (state) {
        case None:
            return "None";
        case Initiated:
            return "Initiated";
        case Unverified:
            return "Unverified";
        case Merging:
            return "Merging";
        case MergeNeedsReboot:
            return "MergeNeedsReboot";
        case MergeCompleted:
            return "MergeCompleted";
        case MergeFailed:
            return "MergeFailed";
        case Cancelled:
            return "Cancelled";
        default:
            return "Unknown";
    }
}
bool SnapshotManager::QuerySnapshotStatus(const std::string& dm_name, std::string* target_type,
                                          DmTargetSnapshot::Status* status) {
    DeviceMapper::TargetInfo target;
    if (!IsSnapshotDevice(dm_name, &target)) {
        LOG(ERROR) << "Device " << dm_name << " is not a snapshot or snapshot-merge device";
        return false;
    }
    if (!DmTargetSnapshot::ParseStatusText(target.data, status)) {
        LOG(ERROR) << "Could not parse snapshot status text: " << dm_name;
        return false;
    }
    if (target_type) {
        *target_type = DeviceMapper::GetTargetType(target.spec);
    }
    if (!status->error.empty()) {
        LOG(ERROR) << "Snapshot: " << dm_name << " returned error code: " << status->error;
        return false;
    }
    return true;
}
UpdateState SnapshotManager::ProcessUpdateState(const std::function<bool()>& callback,
                                                const std::function<bool()>& before_cancel) {
    while (true) {
        auto result = CheckMergeState(before_cancel);
        LOG(INFO) << "ProcessUpdateState handling state: " << UpdateStateToStr(result.state);
        if (result.state == UpdateState::MergeFailed) {
            AcknowledgeMergeFailure(result.failure_code);
        }
        if (result.state != UpdateState::Merging) {
            return result.state;
        }
        if (callback && !callback()) {
            return result.state;
        }
        std::this_thread::sleep_for(kUpdateStateCheckInterval);
    }
}
auto SnapshotManager::CheckMergeState(const std::function<bool()>& before_cancel) -> MergeResult {
    auto lock = LockExclusive();
    if (!lock) {
        return MergeResult(UpdateState::MergeFailed, MergeFailureCode::AcquireLock);
    }
    auto result = CheckMergeState(lock.get(), before_cancel);
    LOG(INFO) << "CheckMergeState for snapshots returned: " << UpdateStateToStr(result.state);
    if (result.state == UpdateState::MergeCompleted) {
        AcknowledgeMergeSuccess(lock.get());
    } else if (result.state == UpdateState::Cancelled) {
        if (!device_->IsRecovery() && !RemoveAllUpdateState(lock.get(), before_cancel)) {
            LOG(ERROR) << "Failed to remove all update state after acknowleding cancelled update.";
        }
    }
    return result;
}
auto SnapshotManager::CheckMergeState(LockedFile* lock, const std::function<bool()>& before_cancel)
        -> MergeResult {
    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock);
    switch (update_status.state()) {
        case UpdateState::None:
        case UpdateState::MergeCompleted:
            return MergeResult(update_status.state());
        case UpdateState::Merging:
        case UpdateState::MergeNeedsReboot:
        case UpdateState::MergeFailed:
            break;
        case UpdateState::Unverified:
            if (HandleCancelledUpdate(lock, before_cancel)) {
                return MergeResult(UpdateState::Cancelled);
            }
            return MergeResult(update_status.state());
        default:
            return MergeResult(update_status.state());
    }
    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock, &snapshots)) {
        return MergeResult(UpdateState::MergeFailed, MergeFailureCode::ListSnapshots);
    }
    auto other_suffix = device_->GetOtherSlotSuffix();
    bool cancelled = false;
    bool merging = false;
    bool needs_reboot = false;
    bool wrong_phase = false;
    MergeFailureCode failure_code = MergeFailureCode::Ok;
    for (const auto& snapshot : snapshots) {
        if (android::base::EndsWith(snapshot, other_suffix)) {
            LOG(INFO) << "Skipping merge validation of unexpected snapshot: " << snapshot;
            continue;
        }
        auto result = CheckTargetMergeState(lock, snapshot, update_status);
        LOG(INFO) << "CheckTargetMergeState for " << snapshot
                  << " returned: " << UpdateStateToStr(result.state);
        switch (result.state) {
            case UpdateState::MergeFailed:
                if (failure_code == MergeFailureCode::Ok) {
                    failure_code = result.failure_code;
                }
                break;
            case UpdateState::Merging:
                merging = true;
                break;
            case UpdateState::MergeNeedsReboot:
                needs_reboot = true;
                break;
            case UpdateState::MergeCompleted:
                break;
            case UpdateState::Cancelled:
                cancelled = true;
                break;
            case UpdateState::None:
                wrong_phase = true;
                break;
            default:
                LOG(ERROR) << "Unknown merge status for \"" << snapshot << "\": "
                           << "\"" << result.state << "\"";
                if (failure_code == MergeFailureCode::Ok) {
                    failure_code = MergeFailureCode::UnexpectedMergeState;
                }
                break;
        }
    }
    if (merging) {
        return MergeResult(UpdateState::Merging);
    }
    if (failure_code != MergeFailureCode::Ok) {
        return MergeResult(UpdateState::MergeFailed, failure_code);
    }
    if (wrong_phase) {
        auto code = MergeSecondPhaseSnapshots(lock);
        if (code != MergeFailureCode::Ok) {
            return MergeResult(UpdateState::MergeFailed, code);
        }
        return MergeResult(UpdateState::Merging);
    }
    if (needs_reboot) {
        WriteUpdateState(lock, UpdateState::MergeNeedsReboot);
        return MergeResult(UpdateState::MergeNeedsReboot);
    }
    if (cancelled) {
        return MergeResult(UpdateState::Cancelled);
    }
    return MergeResult(UpdateState::MergeCompleted);
}
auto SnapshotManager::CheckTargetMergeState(LockedFile* lock, const std::string& name,
                                            const SnapshotUpdateStatus& update_status)
        -> MergeResult {
    SnapshotStatus snapshot_status;
    if (!ReadSnapshotStatus(lock, name, &snapshot_status)) {
        return MergeResult(UpdateState::MergeFailed, MergeFailureCode::ReadStatus);
    }
    std::unique_ptr<LpMetadata> current_metadata;
    if (!IsSnapshotDevice(name)) {
        if (!current_metadata) {
            current_metadata = ReadCurrentMetadata();
        }
        if (!current_metadata ||
            GetMetadataPartitionState(*current_metadata, name) != MetadataPartitionState::Updated) {
            DeleteSnapshot(lock, name);
            return MergeResult(UpdateState::Cancelled);
        }
        if (snapshot_status.state() == SnapshotState::MERGE_COMPLETED) {
            OnSnapshotMergeComplete(lock, name, snapshot_status);
            return MergeResult(UpdateState::MergeCompleted);
        }
        LOG(ERROR) << "Expected snapshot or snapshot-merge for device: " << name;
        return MergeResult(UpdateState::MergeFailed, MergeFailureCode::UnknownTargetType);
    }
    DCHECK((current_metadata = ReadCurrentMetadata()) &&
           GetMetadataPartitionState(*current_metadata, name) == MetadataPartitionState::Updated);
    if (UpdateUsesUserSnapshots(lock)) {
        std::string merge_status;
        if (EnsureSnapuserdConnected()) {
            merge_status = snapuserd_client_->QuerySnapshotStatus(name);
        } else {
            MergeResult(UpdateState::MergeFailed, MergeFailureCode::QuerySnapshotStatus);
        }
        if (merge_status == "snapshot-merge-failed") {
            return MergeResult(UpdateState::MergeFailed, MergeFailureCode::UnknownTargetType);
        }
        if (merge_status == "snapshot" && snapshot_status.state() == SnapshotState::MERGING) {
            if (!snapuserd_client_->InitiateMerge(name)) {
                return MergeResult(UpdateState::MergeFailed, MergeFailureCode::UnknownTargetType);
            }
            return MergeResult(UpdateState::Merging);
        }
        if (merge_status == "snapshot" &&
            DecideMergePhase(snapshot_status) == MergePhase::SECOND_PHASE &&
            update_status.merge_phase() == MergePhase::FIRST_PHASE) {
            return MergeResult(UpdateState::None);
        }
        if (merge_status == "snapshot-merge") {
            if (snapshot_status.state() == SnapshotState::MERGE_COMPLETED) {
                LOG(ERROR) << "Snapshot " << name
                           << " is merging after being marked merge-complete.";
                return MergeResult(UpdateState::MergeFailed,
                                   MergeFailureCode::UnmergedSectorsAfterCompletion);
            }
            return MergeResult(UpdateState::Merging);
        }
        if (merge_status != "snapshot-merge-complete") {
            LOG(ERROR) << "Snapshot " << name << " has incorrect status: " << merge_status;
            return MergeResult(UpdateState::MergeFailed, MergeFailureCode::ExpectedMergeTarget);
        }
    } else {
        std::string target_type;
        DmTargetSnapshot::Status status;
        if (!QuerySnapshotStatus(name, &target_type, &status)) {
            return MergeResult(UpdateState::MergeFailed, MergeFailureCode::QuerySnapshotStatus);
        }
        if (target_type == "snapshot" &&
            DecideMergePhase(snapshot_status) == MergePhase::SECOND_PHASE &&
            update_status.merge_phase() == MergePhase::FIRST_PHASE) {
            return MergeResult(UpdateState::None);
        }
        if (target_type != "snapshot-merge") {
            LOG(ERROR) << "Snapshot " << name << " has incorrect target type: " << target_type;
            return MergeResult(UpdateState::MergeFailed, MergeFailureCode::ExpectedMergeTarget);
        }
        if (status.sectors_allocated != status.metadata_sectors) {
            if (snapshot_status.state() == SnapshotState::MERGE_COMPLETED) {
                LOG(ERROR) << "Snapshot " << name
                           << " is merging after being marked merge-complete.";
                return MergeResult(UpdateState::MergeFailed,
                                   MergeFailureCode::UnmergedSectorsAfterCompletion);
            }
            return MergeResult(UpdateState::Merging);
        }
    }
    auto code = CheckMergeConsistency(lock, name, snapshot_status);
    if (code != MergeFailureCode::Ok) {
        return MergeResult(UpdateState::MergeFailed, code);
    }
    snapshot_status.set_state(SnapshotState::MERGE_COMPLETED);
    if (!WriteSnapshotStatus(lock, snapshot_status)) {
        return MergeResult(UpdateState::MergeFailed, MergeFailureCode::WriteStatus);
    }
    if (!OnSnapshotMergeComplete(lock, name, snapshot_status)) {
        return MergeResult(UpdateState::MergeNeedsReboot);
    }
    return MergeResult(UpdateState::MergeCompleted, MergeFailureCode::Ok);
}
static std::string GetMappedCowDeviceName(const std::string& snapshot,
                                          const SnapshotStatus& status) {
    if (status.cow_partition_size() == 0) {
        return GetCowImageDeviceName(snapshot);
    }
    return GetCowName(snapshot);
}
MergeFailureCode SnapshotManager::CheckMergeConsistency(LockedFile* lock, const std::string& name,
                                                        const SnapshotStatus& status) {
    CHECK(lock);
    return merge_consistency_checker_(name, status);
}
MergeFailureCode CheckMergeConsistency(const std::string& name, const SnapshotStatus& status) {
    if (!status.using_snapuserd()) {
        return MergeFailureCode::Ok;
    }
    auto& dm = DeviceMapper::Instance();
    std::string cow_image_name = GetMappedCowDeviceName(name, status);
    std::string cow_image_path;
    if (!dm.GetDmDevicePathByName(cow_image_name, &cow_image_path)) {
        LOG(ERROR) << "Failed to get path for cow device: " << cow_image_name;
        return MergeFailureCode::GetCowPathConsistencyCheck;
    }
    size_t num_ops = 0;
    {
        unique_fd fd(open(cow_image_path.c_str(), O_RDONLY | O_CLOEXEC));
        if (fd < 0) {
            PLOG(ERROR) << "Failed to open " << cow_image_name;
            return MergeFailureCode::OpenCowConsistencyCheck;
        }
        CowReader reader;
        if (!reader.Parse(std::move(fd))) {
            LOG(ERROR) << "Failed to parse cow " << cow_image_path;
            return MergeFailureCode::ParseCowConsistencyCheck;
        }
        num_ops = reader.get_num_total_data_ops();
    }
    unique_fd fd(open(cow_image_path.c_str(), O_RDONLY | O_DIRECT | O_SYNC | O_CLOEXEC));
    if (fd < 0) {
        PLOG(ERROR) << "Failed to open direct " << cow_image_name;
        return MergeFailureCode::OpenCowDirectConsistencyCheck;
    }
    void* addr;
    size_t page_size = getpagesize();
    if (posix_memalign(&addr, page_size, page_size) < 0) {
        PLOG(ERROR) << "posix_memalign with page size " << page_size;
        return MergeFailureCode::MemAlignConsistencyCheck;
    }
    std::unique_ptr<void, decltype(&::free)> buffer(addr, ::free);
    if (!android::base::ReadFully(fd, buffer.get(), page_size)) {
        PLOG(ERROR) << "Direct read failed " << cow_image_name;
        return MergeFailureCode::DirectReadConsistencyCheck;
    }
    auto header = reinterpret_cast<CowHeader*>(buffer.get());
    if (header->num_merge_ops != num_ops) {
        LOG(ERROR) << "COW consistency check failed, expected " << num_ops << " to be merged, "
                   << "but " << header->num_merge_ops << " were actually recorded.";
        LOG(ERROR) << "Aborting merge progress for snapshot " << name
                   << ", will try again next boot";
        return MergeFailureCode::WrongMergeCountConsistencyCheck;
    }
    return MergeFailureCode::Ok;
}
MergeFailureCode SnapshotManager::MergeSecondPhaseSnapshots(LockedFile* lock) {
    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock, &snapshots)) {
        return MergeFailureCode::ListSnapshots;
    }
    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock);
    CHECK(update_status.state() == UpdateState::Merging ||
          update_status.state() == UpdateState::MergeFailed);
    CHECK(update_status.merge_phase() == MergePhase::FIRST_PHASE);
    update_status.set_state(UpdateState::Merging);
    update_status.set_merge_phase(MergePhase::SECOND_PHASE);
    if (!WriteSnapshotUpdateStatus(lock, update_status)) {
        return MergeFailureCode::WriteStatus;
    }
    MergeFailureCode result = MergeFailureCode::Ok;
    for (const auto& snapshot : snapshots) {
        SnapshotStatus snapshot_status;
        if (!ReadSnapshotStatus(lock, snapshot, &snapshot_status)) {
            return MergeFailureCode::ReadStatus;
        }
        if (DecideMergePhase(snapshot_status) != MergePhase::SECOND_PHASE) {
            continue;
        }
        auto code = SwitchSnapshotToMerge(lock, snapshot);
        if (code != MergeFailureCode::Ok) {
            LOG(ERROR) << "Failed to switch snapshot to a second-phase merge target: " << snapshot;
            if (result == MergeFailureCode::Ok) {
                result = code;
            }
        }
    }
    return result;
}
std::string SnapshotManager::GetSnapshotBootIndicatorPath() {
    return metadata_dir_ + "/" + android::base::Basename(kBootIndicatorPath);
}
std::string SnapshotManager::GetRollbackIndicatorPath() {
    return metadata_dir_ + "/" + android::base::Basename(kRollbackIndicatorPath);
}
std::string SnapshotManager::GetForwardMergeIndicatorPath() {
    return metadata_dir_ + "/allow-forward-merge";
}
std::string SnapshotManager::GetOldPartitionMetadataPath() {
    return metadata_dir_ + "/old-partition-metadata";
}
void SnapshotManager::AcknowledgeMergeSuccess(LockedFile* lock) {
    if (device_->IsRecovery()) {
        WriteUpdateState(lock, UpdateState::MergeCompleted);
        return;
    }
    RemoveAllUpdateState(lock);
    if (UpdateUsesUserSnapshots(lock) && !device()->IsTestDevice()) {
        if (snapuserd_client_) {
            snapuserd_client_->DetachSnapuserd();
            snapuserd_client_->RemoveTransitionedDaemonIndicator();
            snapuserd_client_ = nullptr;
        }
    }
}
void SnapshotManager::AcknowledgeMergeFailure(MergeFailureCode failure_code) {
    LOG(ERROR) << "Merge could not be completed and will be marked as failed.";
    auto lock = LockExclusive();
    if (!lock) return;
    UpdateState state = ReadUpdateState(lock.get());
    if (state != UpdateState::Merging && state != UpdateState::MergeNeedsReboot) {
        return;
    }
    WriteUpdateState(lock.get(), UpdateState::MergeFailed, failure_code);
}
bool SnapshotManager::OnSnapshotMergeComplete(LockedFile* lock, const std::string& name,
                                              const SnapshotStatus& status) {
    if (!UpdateUsesUserSnapshots(lock)) {
        if (IsSnapshotDevice(name)) {
            std::string target_type;
            DmTargetSnapshot::Status dm_status;
            if (!QuerySnapshotStatus(name, &target_type, &dm_status)) {
                return false;
            }
            if (target_type != "snapshot-merge") {
                LOG(ERROR) << "Unexpected target type " << target_type
                           << " for snapshot device: " << name;
                return false;
            }
            if (dm_status.sectors_allocated != dm_status.metadata_sectors) {
                LOG(ERROR) << "Merge is unexpectedly incomplete for device " << name;
                return false;
            }
            if (!CollapseSnapshotDevice(lock, name, status)) {
                LOG(ERROR) << "Unable to collapse snapshot: " << name;
                return false;
            }
        }
    } else {
        if (!CollapseSnapshotDevice(lock, name, status)) {
            LOG(ERROR) << "Unable to collapse snapshot: " << name;
            return false;
        }
    }
    if (!DeleteSnapshot(lock, name)) {
        LOG(ERROR) << "Could not delete snapshot: " << name;
        return false;
    }
    return true;
}
bool SnapshotManager::CollapseSnapshotDevice(LockedFile* lock, const std::string& name,
                                             const SnapshotStatus& status) {
    if (!UpdateUsesUserSnapshots(lock)) {
        DeviceMapper::TargetInfo target;
        if (!GetSingleTarget(name, TableQuery::Table, &target)) {
            return false;
        }
        if (DeviceMapper::GetTargetType(target.spec) != "snapshot-merge") {
            LOG(ERROR) << "Snapshot device has invalid target type: " << name;
            return false;
        }
        std::string base_device, cow_device;
        if (!DmTargetSnapshot::GetDevicesFromParams(target.data, &base_device, &cow_device)) {
            LOG(ERROR) << "Could not parse snapshot device " << name
                       << " parameters: " << target.data;
            return false;
        }
    }
    uint64_t snapshot_sectors = status.snapshot_size() / kSectorSize;
    if (snapshot_sectors * kSectorSize != status.snapshot_size()) {
        LOG(ERROR) << "Snapshot " << name
                   << " size is not sector aligned: " << status.snapshot_size();
        return false;
    }
    uint32_t slot = SlotNumberForSlotSuffix(device_->GetSlotSuffix());
    CreateLogicalPartitionParams base_device_params{
            .block_device = device_->GetSuperDevice(slot),
            .metadata_slot = slot,
            .partition_name = name,
            .partition_opener = &device_->GetPartitionOpener(),
    };
    DmTable table;
    if (!CreateDmTable(base_device_params, &table)) {
        LOG(ERROR) << "Could not create a DmTable for partition: " << name;
        return false;
    }
    if (!dm_.LoadTableAndActivate(name, table)) {
        return false;
    }
    if (!UpdateUsesUserSnapshots(lock)) {
        if (status.using_snapuserd()) {
            auto dm_user_name = GetDmUserCowName(name, GetSnapshotDriver(lock));
            UnmapDmUserDevice(dm_user_name);
        }
    }
    if (UpdateUsesUserSnapshots(lock) && EnsureSnapuserdConnected()) {
        if (!snapuserd_client_->WaitForDeviceDelete(name)) {
            LOG(ERROR) << "Failed to wait for " << name << " control device to delete";
        }
    }
    auto base_name = GetBaseDeviceName(name);
    if (!DeleteDeviceIfExists(base_name)) {
        LOG(ERROR) << "Unable to delete base device for snapshot: " << base_name;
    }
    if (!DeleteDeviceIfExists(GetSourceDeviceName(name), 4000ms)) {
        LOG(ERROR) << "Unable to delete source device for snapshot: " << GetSourceDeviceName(name);
    }
    return true;
}
bool SnapshotManager::HandleCancelledUpdate(LockedFile* lock,
                                            const std::function<bool()>& before_cancel) {
    auto slot = GetCurrentSlot();
    if (slot == Slot::Unknown) {
        return false;
    }
    if (AreAllSnapshotsCancelled(lock)) {
        LOG(WARNING) << "Detected re-flashing, cancelling unverified update.";
        return RemoveAllUpdateState(lock, before_cancel);
    }
    auto current_slot = GetCurrentSlot();
    if (current_slot != Slot::Source) {
        LOG(INFO) << "Update state is being processed while booting at " << current_slot
                  << " slot, taking no action.";
        return false;
    }
    if (access(GetRollbackIndicatorPath().c_str(), F_OK) != 0) {
        PLOG(INFO) << "Rollback indicator not detected. "
                   << "Update state is being processed before reboot, taking no action.";
        return false;
    }
    LOG(WARNING) << "Detected rollback, cancelling unverified update.";
    return RemoveAllUpdateState(lock, before_cancel);
}
bool SnapshotManager::PerformInitTransition(InitTransition transition,
                                            std::vector<std::string>* snapuserd_argv) {
    LOG(INFO) << "Performing transition for snapuserd.";
    if (!snapuserd_client_ && transition != InitTransition::SELINUX_DETACH) {
        snapuserd_client_ = SnapuserdClient::Connect(kSnapuserdSocket, 10s);
        if (!snapuserd_client_) {
            LOG(ERROR) << "Unable to connect to snapuserd";
            return false;
        }
    }
    auto lock = LockExclusive();
    if (!lock) return false;
    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock.get(), &snapshots)) {
        LOG(ERROR) << "Failed to list snapshots.";
        return false;
    }
    if (UpdateUsesUserSnapshots(lock.get()) && transition == InitTransition::SELINUX_DETACH) {
        snapuserd_argv->emplace_back("-user_snapshot");
        if (UpdateUsesIouring(lock.get())) {
            snapuserd_argv->emplace_back("-io_uring");
        }
    }
    size_t num_cows = 0;
    size_t ok_cows = 0;
    for (const auto& snapshot : snapshots) {
        std::string user_cow_name = GetDmUserCowName(snapshot, GetSnapshotDriver(lock.get()));
        if (dm_.GetState(user_cow_name) == DmDeviceState::INVALID) {
            continue;
        }
        DeviceMapper::TargetInfo target;
        if (!GetSingleTarget(user_cow_name, TableQuery::Table, &target)) {
            continue;
        }
        auto target_type = DeviceMapper::GetTargetType(target.spec);
        if (target_type != "user") {
            LOG(ERROR) << "Unexpected target type for " << user_cow_name << ": " << target_type;
            continue;
        }
        num_cows++;
        SnapshotStatus snapshot_status;
        if (!ReadSnapshotStatus(lock.get(), snapshot, &snapshot_status)) {
            LOG(ERROR) << "Unable to read snapshot status: " << snapshot;
            continue;
        }
        auto misc_name = user_cow_name;
        std::string source_device_name;
        if (snapshot_status.old_partition_size() > 0) {
            source_device_name = GetSourceDeviceName(snapshot);
        } else {
            source_device_name = GetBaseDeviceName(snapshot);
        }
        std::string source_device;
        if (!dm_.GetDmDevicePathByName(source_device_name, &source_device)) {
            LOG(ERROR) << "Could not get device path for " << GetSourceDeviceName(snapshot);
            continue;
        }
        std::string base_path_merge;
        if (!dm_.GetDmDevicePathByName(GetBaseDeviceName(snapshot), &base_path_merge)) {
            LOG(ERROR) << "Could not get device path for " << GetSourceDeviceName(snapshot);
            continue;
        }
        std::string cow_image_name = GetMappedCowDeviceName(snapshot, snapshot_status);
        std::string cow_image_device;
        if (!dm_.GetDmDevicePathByName(cow_image_name, &cow_image_device)) {
            LOG(ERROR) << "Could not get device path for " << cow_image_name;
            continue;
        }
        if (transition == InitTransition::SELINUX_DETACH) {
            if (!UpdateUsesUserSnapshots(lock.get())) {
                auto message = misc_name + "," + cow_image_device + "," + source_device;
                snapuserd_argv->emplace_back(std::move(message));
            } else {
                auto message = misc_name + "," + cow_image_device + "," + source_device + "," +
                               base_path_merge;
                snapuserd_argv->emplace_back(std::move(message));
            }
            ok_cows++;
            continue;
        }
        DmTable table;
        table.Emplace<DmTargetUser>(0, target.spec.length, misc_name);
        if (!dm_.LoadTableAndActivate(user_cow_name, table)) {
            LOG(ERROR) << "Unable to swap tables for " << misc_name;
            continue;
        }
        std::string control_device = "/dev/dm-user/" + misc_name;
        if (!WaitForDevice(control_device, 10s)) {
            LOG(ERROR) << "dm-user control device no found:  " << misc_name;
            continue;
        }
        uint64_t base_sectors;
        if (!UpdateUsesUserSnapshots(lock.get())) {
            base_sectors =
                    snapuserd_client_->InitDmUserCow(misc_name, cow_image_device, source_device);
        } else {
            base_sectors = snapuserd_client_->InitDmUserCow(misc_name, cow_image_device,
                                                            source_device, base_path_merge);
        }
        if (base_sectors == 0) {
            LOG(FATAL) << "Failed to retrieve base_sectors from Snapuserd";
            return false;
        }
        CHECK(base_sectors <= target.spec.length);
        if (!snapuserd_client_->AttachDmUser(misc_name)) {
            LOG(FATAL) << "Could not initialize snapuserd for " << user_cow_name;
            return false;
        }
        ok_cows++;
    }
    if (ok_cows != num_cows) {
        LOG(ERROR) << "Could not transition all snapuserd consumers.";
        return false;
    }
    return true;
}
std::unique_ptr<LpMetadata> SnapshotManager::ReadCurrentMetadata() {
    const auto& opener = device_->GetPartitionOpener();
    uint32_t slot = SlotNumberForSlotSuffix(device_->GetSlotSuffix());
    auto super_device = device_->GetSuperDevice(slot);
    auto metadata = android::fs_mgr::ReadMetadata(opener, super_device, slot);
    if (!metadata) {
        LOG(ERROR) << "Could not read dynamic partition metadata for device: " << super_device;
        return nullptr;
    }
    return metadata;
}
SnapshotManager::MetadataPartitionState SnapshotManager::GetMetadataPartitionState(
        const LpMetadata& metadata, const std::string& name) {
    auto partition = android::fs_mgr::FindPartition(metadata, name);
    if (!partition) return MetadataPartitionState::None;
    if (partition->attributes & LP_PARTITION_ATTR_UPDATED) {
        return MetadataPartitionState::Updated;
    }
    return MetadataPartitionState::Flashed;
}
bool SnapshotManager::AreAllSnapshotsCancelled(LockedFile* lock) {
    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock, &snapshots)) {
        LOG(WARNING) << "Failed to list snapshots to determine whether device has been flashed "
                     << "after applying an update. Assuming no snapshots.";
        return true;
    }
    std::map<std::string, bool> flashing_status;
    if (!GetSnapshotFlashingStatus(lock, snapshots, &flashing_status)) {
        LOG(WARNING) << "Failed to determine whether partitions have been flashed. Not"
                     << "removing update states.";
        return false;
    }
    bool all_snapshots_cancelled = std::all_of(flashing_status.begin(), flashing_status.end(),
                                               [](const auto& pair) { return pair.second; });
    if (all_snapshots_cancelled) {
        LOG(WARNING) << "All partitions are re-flashed after update, removing all update states.";
    }
    return all_snapshots_cancelled;
}
bool SnapshotManager::GetSnapshotFlashingStatus(LockedFile* lock,
                                                const std::vector<std::string>& snapshots,
                                                std::map<std::string, bool>* out) {
    CHECK(lock);
    auto source_slot_suffix = ReadUpdateSourceSlotSuffix();
    if (source_slot_suffix.empty()) {
        return false;
    }
    uint32_t source_slot = SlotNumberForSlotSuffix(source_slot_suffix);
    uint32_t target_slot = (source_slot == 0) ? 1 : 0;
    const auto& opener = device_->GetPartitionOpener();
    auto super_device = device_->GetSuperDevice(target_slot);
    auto metadata = android::fs_mgr::ReadMetadata(opener, super_device, target_slot);
    if (!metadata) {
        return false;
    }
    for (const auto& snapshot_name : snapshots) {
        if (GetMetadataPartitionState(*metadata, snapshot_name) ==
            MetadataPartitionState::Updated) {
            out->emplace(snapshot_name, false);
        } else {
            LOG(WARNING) << "Detected re-flashing of partition " << snapshot_name << ".";
            out->emplace(snapshot_name, true);
        }
    }
    return true;
}
void SnapshotManager::RemoveInvalidSnapshots(LockedFile* lock) {
    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock, &snapshots, device_->GetSlotSuffix()) || snapshots.empty()) {
        return;
    }
    for (const auto& name : snapshots) {
        if (dm_.GetState(name) == DmDeviceState::ACTIVE && !IsSnapshotDevice(name)) {
            if (!DeleteSnapshot(lock, name)) {
                LOG(ERROR) << "Failed to delete invalid snapshot: " << name;
            } else {
                LOG(INFO) << "Invalid snapshot: " << name << " deleted";
            }
        }
    }
}
bool SnapshotManager::RemoveAllSnapshots(LockedFile* lock) {
    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock, &snapshots)) {
        LOG(ERROR) << "Could not list snapshots";
        return false;
    }
    std::map<std::string, bool> flashing_status;
    if (!GetSnapshotFlashingStatus(lock, snapshots, &flashing_status)) {
        LOG(WARNING) << "Failed to get flashing status";
    }
    auto current_slot = GetCurrentSlot();
    bool ok = true;
    bool has_mapped_cow_images = false;
    for (const auto& name : snapshots) {
        bool should_unmap = current_slot != Slot::Target;
        bool should_delete = ShouldDeleteSnapshot(flashing_status, current_slot, name);
        if (should_unmap && android::base::EndsWith(name, device_->GetSlotSuffix())) {
            if (dm_.GetState(name) == DmDeviceState::INVALID || !IsSnapshotDevice(name)) {
                LOG(ERROR) << "Detected snapshot " << name << " on " << current_slot << " slot"
                           << " for source partition; removing without unmap.";
                should_unmap = false;
            }
        }
        bool partition_ok = true;
        if (should_unmap && !UnmapPartitionWithSnapshot(lock, name)) {
            partition_ok = false;
        }
        if (partition_ok && should_delete && !DeleteSnapshot(lock, name)) {
            partition_ok = false;
        }
        if (!partition_ok) {
            auto cow_image_device = GetCowImageDeviceName(name);
            has_mapped_cow_images |=
                    (EnsureImageManager() && images_->IsImageMapped(cow_image_device));
            ok = false;
        }
    }
    if (ok || !has_mapped_cow_images) {
        if (!EnsureImageManager() || !images_->RemoveAllImages()) {
            LOG(ERROR) << "Could not remove all snapshot artifacts";
            return false;
        }
    }
    return ok;
}
bool SnapshotManager::ShouldDeleteSnapshot(const std::map<std::string, bool>& flashing_status,
                                           Slot current_slot, const std::string& name) {
    if (current_slot != Slot::Target) {
        return true;
    }
    auto it = flashing_status.find(name);
    if (it == flashing_status.end()) {
        LOG(WARNING) << "Can't determine flashing status for " << name;
        return true;
    }
    if (it->second) {
        return true;
    }
    return !IsSnapshotDevice(name);
}
UpdateState SnapshotManager::GetUpdateState(double* progress) {
    auto state_file = GetStateFilePath();
    if (access(state_file.c_str(), F_OK) != 0 && errno == ENOENT) {
        return UpdateState::None;
    }
    auto lock = LockShared();
    if (!lock) {
        return UpdateState::None;
    }
    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock.get());
    auto state = update_status.state();
    if (progress == nullptr) {
        return state;
    }
    if (state == UpdateState::MergeCompleted) {
        *progress = 100.0;
        return state;
    }
    *progress = 0.0;
    if (state != UpdateState::Merging) {
        return state;
    }
    if (!UpdateUsesUserSnapshots(lock.get())) {
        std::vector<std::string> snapshots;
        if (!ListSnapshots(lock.get(), &snapshots)) {
            LOG(ERROR) << "Could not list snapshots";
            return state;
        }
        DmTargetSnapshot::Status fake_snapshots_status = {};
        for (const auto& snapshot : snapshots) {
            DmTargetSnapshot::Status current_status;
            if (!IsSnapshotDevice(snapshot)) continue;
            if (!QuerySnapshotStatus(snapshot, nullptr, &current_status)) continue;
            fake_snapshots_status.sectors_allocated += current_status.sectors_allocated;
            fake_snapshots_status.total_sectors += current_status.total_sectors;
            fake_snapshots_status.metadata_sectors += current_status.metadata_sectors;
        }
        *progress = DmTargetSnapshot::MergePercent(fake_snapshots_status,
                                                   update_status.sectors_allocated());
    } else {
        if (EnsureSnapuserdConnected()) {
            *progress = snapuserd_client_->GetMergePercent();
        }
    }
    return state;
}
bool SnapshotManager::UpdateUsesCompression() {
    auto lock = LockShared();
    if (!lock) return false;
    return UpdateUsesCompression(lock.get());
}
bool SnapshotManager::UpdateUsesCompression(LockedFile* lock) {
    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock);
    return update_status.using_snapuserd();
}
bool SnapshotManager::UpdateUsesIouring(LockedFile* lock) {
    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock);
    return update_status.io_uring_enabled();
}
bool SnapshotManager::UpdateUsesUserSnapshots() {
    if (is_snapshot_userspace_.has_value()) {
        return is_snapshot_userspace_.value();
    }
    auto lock = LockShared();
    if (!lock) return false;
    return UpdateUsesUserSnapshots(lock.get());
}
bool SnapshotManager::UpdateUsesUserSnapshots(LockedFile* lock) {
    if (is_snapshot_userspace_.has_value()) {
        return is_snapshot_userspace_.value();
    }
    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock);
    is_snapshot_userspace_ = update_status.userspace_snapshots();
    return is_snapshot_userspace_.value();
}
bool SnapshotManager::ListSnapshots(LockedFile* lock, std::vector<std::string>* snapshots,
                                    const std::string& suffix) {
    CHECK(lock);
    auto dir_path = metadata_dir_ + "/snapshots"s;
    std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(dir_path.c_str()), closedir);
    if (!dir) {
        PLOG(ERROR) << "opendir failed: " << dir_path;
        return false;
    }
    struct dirent* dp;
    while ((dp = readdir(dir.get())) != nullptr) {
        if (dp->d_type != DT_REG) continue;
        std::string name(dp->d_name);
        if (!suffix.empty() && !android::base::EndsWith(name, suffix)) {
            continue;
        }
        if (name == "system_a" || name == "system_b" || name == "product_a" ||
            name == "product_b") {
            snapshots->insert(snapshots->begin(), std::move(name));
        } else {
            snapshots->emplace_back(std::move(name));
        }
    }
    return true;
}
bool SnapshotManager::IsSnapshotManagerNeeded() {
    return access(kBootIndicatorPath, F_OK) == 0;
}
std::string SnapshotManager::GetGlobalRollbackIndicatorPath() {
    return kRollbackIndicatorPath;
}
bool SnapshotManager::NeedSnapshotsInFirstStageMount() {
    auto slot = GetCurrentSlot();
    if (slot != Slot::Target) {
        if (slot == Slot::Source) {
            auto path = GetRollbackIndicatorPath();
            if (!android::base::WriteStringToFile("1", path)) {
                PLOG(ERROR) << "Unable to write rollback indicator: " << path;
            } else {
                LOG(INFO) << "Rollback detected, writing rollback indicator to " << path;
            }
        }
        LOG(INFO) << "Not booting from new slot. Will not mount snapshots.";
        return false;
    }
    auto lock = LockShared();
    if (!lock) {
        LOG(FATAL) << "Could not read update state to determine snapshot status";
        return false;
    }
    switch (ReadUpdateState(lock.get())) {
        case UpdateState::Unverified:
        case UpdateState::Merging:
        case UpdateState::MergeFailed:
            return true;
        default:
            return false;
    }
}
bool SnapshotManager::CreateLogicalAndSnapshotPartitions(
        const std::string& super_device, const std::chrono::milliseconds& timeout_ms) {
    LOG(INFO) << "Creating logical partitions with snapshots as needed";
    auto lock = LockExclusive();
    if (!lock) return false;
    uint32_t slot = SlotNumberForSlotSuffix(device_->GetSlotSuffix());
    return MapAllPartitions(lock.get(), super_device, slot, timeout_ms);
}
bool SnapshotManager::MapAllPartitions(LockedFile* lock, const std::string& super_device,
                                       uint32_t slot, const std::chrono::milliseconds& timeout_ms) {
    const auto& opener = device_->GetPartitionOpener();
    auto metadata = android::fs_mgr::ReadMetadata(opener, super_device, slot);
    if (!metadata) {
        LOG(ERROR) << "Could not read dynamic partition metadata for device: " << super_device;
        return false;
    }
    if (!EnsureImageManager()) {
        return false;
    }
    for (const auto& partition : metadata->partitions) {
        if (GetPartitionGroupName(metadata->groups[partition.group_index]) == kCowGroupName) {
            LOG(INFO) << "Skip mapping partition " << GetPartitionName(partition) << " in group "
                      << kCowGroupName;
            continue;
        }
        CreateLogicalPartitionParams params = {
                .block_device = super_device,
                .metadata = metadata.get(),
                .partition = &partition,
                .timeout_ms = timeout_ms,
                .partition_opener = &opener,
        };
        if (!MapPartitionWithSnapshot(lock, std::move(params), SnapshotContext::Mount, nullptr)) {
            return false;
        }
    }
    LOG(INFO) << "Created logical partitions with snapshot.";
    return true;
}
static std::chrono::milliseconds GetRemainingTime(
        const std::chrono::milliseconds& timeout,
        const std::chrono::time_point<std::chrono::steady_clock>& begin) {
    if (timeout.count() == 0) return std::chrono::milliseconds(0);
    auto passed_time = std::chrono::steady_clock::now() - begin;
    auto remaining_time = timeout - duration_cast<std::chrono::milliseconds>(passed_time);
    if (remaining_time.count() <= 0) {
        LOG(ERROR) << "MapPartitionWithSnapshot has reached timeout " << timeout.count() << "ms ("
                   << remaining_time.count() << "ms remaining)";
        return std::chrono::milliseconds::min();
    }
    return remaining_time;
}
bool SnapshotManager::MapPartitionWithSnapshot(LockedFile* lock,
                                               CreateLogicalPartitionParams params,
                                               SnapshotContext context, SnapshotPaths* paths) {
    auto begin = std::chrono::steady_clock::now();
    CHECK(lock);
    if (params.GetPartitionName() != params.GetDeviceName()) {
        LOG(ERROR) << "Mapping snapshot with a different name is unsupported: partition_name = "
                   << params.GetPartitionName() << ", device_name = " << params.GetDeviceName();
        return false;
    }
    CreateLogicalPartitionParams::OwnedData params_owned_data;
    if (!params.InitDefaults(&params_owned_data)) {
        return false;
    }
    if (!params.partition->num_extents) {
        LOG(INFO) << "Skipping zero-length logical partition: " << params.GetPartitionName();
        return true;
    }
    std::optional<SnapshotStatus> live_snapshot_status;
    do {
        if (!(params.partition->attributes & LP_PARTITION_ATTR_UPDATED)) {
            LOG(INFO) << "Detected re-flashing of partition, will skip snapshot: "
                      << params.GetPartitionName();
            break;
        }
        auto file_path = GetSnapshotStatusFilePath(params.GetPartitionName());
        if (access(file_path.c_str(), F_OK) != 0) {
            if (errno != ENOENT) {
                PLOG(INFO) << "Can't map snapshot for " << params.GetPartitionName()
                           << ": Can't access " << file_path;
                return false;
            }
            break;
        }
        live_snapshot_status = std::make_optional<SnapshotStatus>();
        if (!ReadSnapshotStatus(lock, params.GetPartitionName(), &*live_snapshot_status)) {
            return false;
        }
        if (live_snapshot_status->state() == SnapshotState::MERGE_COMPLETED) {
            live_snapshot_status.reset();
        }
        if (live_snapshot_status->state() == SnapshotState::NONE ||
            live_snapshot_status->cow_partition_size() + live_snapshot_status->cow_file_size() ==
                    0) {
            LOG(WARNING) << "Snapshot status for " << params.GetPartitionName()
                         << " is invalid, ignoring: state = "
                         << SnapshotState_Name(live_snapshot_status->state())
                         << ", cow_partition_size = " << live_snapshot_status->cow_partition_size()
                         << ", cow_file_size = " << live_snapshot_status->cow_file_size();
            live_snapshot_status.reset();
        }
    } while (0);
    if (live_snapshot_status.has_value()) {
        params.force_writable = true;
        params.device_name = GetBaseDeviceName(params.GetPartitionName());
    }
    AutoDeviceList created_devices;
    std::string base_path;
    if (!CreateLogicalPartition(params, &base_path)) {
        LOG(ERROR) << "Could not create logical partition " << params.GetPartitionName()
                   << " as device " << params.GetDeviceName();
        return false;
    }
    created_devices.EmplaceBack<AutoUnmapDevice>(&dm_, params.GetDeviceName());
    if (paths) {
        paths->target_device = base_path;
    }
    auto remaining_time = GetRemainingTime(params.timeout_ms, begin);
    if (remaining_time.count() < 0) {
        return false;
    }
    if (!WaitForDevice(base_path, remaining_time)) {
        return false;
    }
    if (!live_snapshot_status.has_value()) {
        created_devices.Release();
        return true;
    }
    std::string base_device;
    if (!dm_.GetDeviceString(params.GetDeviceName(), &base_device)) {
        LOG(ERROR) << "Could not determine major/minor for: " << params.GetDeviceName();
        return false;
    }
    remaining_time = GetRemainingTime(params.timeout_ms, begin);
    if (remaining_time.count() < 0) return false;
    std::string cow_name;
    CreateLogicalPartitionParams cow_params = params;
    cow_params.timeout_ms = remaining_time;
    if (!MapCowDevices(lock, cow_params, *live_snapshot_status, &created_devices, &cow_name)) {
        return false;
    }
    std::string cow_device;
    if (!GetMappedImageDeviceStringOrPath(cow_name, &cow_device)) {
        LOG(ERROR) << "Could not determine major/minor for: " << cow_name;
        return false;
    }
    if (paths) {
        paths->cow_device_name = cow_name;
    }
    remaining_time = GetRemainingTime(params.timeout_ms, begin);
    if (remaining_time.count() < 0) return false;
    if (context == SnapshotContext::Update && live_snapshot_status->using_snapuserd()) {
        created_devices.Release();
        return true;
    }
    if (live_snapshot_status->using_snapuserd()) {
        std::string source_device_path;
        if (live_snapshot_status->old_partition_size() > 0) {
            if (!MapSourceDevice(lock, params.GetPartitionName(), remaining_time,
                                 &source_device_path)) {
                LOG(ERROR) << "Could not map source device for: " << cow_name;
                return false;
            }
            auto source_device = GetSourceDeviceName(params.GetPartitionName());
            created_devices.EmplaceBack<AutoUnmapDevice>(&dm_, source_device);
        } else {
            source_device_path = base_path;
        }
        if (!WaitForDevice(source_device_path, remaining_time)) {
            return false;
        }
        std::string cow_path;
        if (!GetMappedImageDevicePath(cow_name, &cow_path)) {
            LOG(ERROR) << "Could not determine path for: " << cow_name;
            return false;
        }
        if (!WaitForDevice(cow_path, remaining_time)) {
            return false;
        }
        auto name = GetDmUserCowName(params.GetPartitionName(), GetSnapshotDriver(lock));
        std::string new_cow_device;
        if (!MapDmUserCow(lock, name, cow_path, source_device_path, base_path, remaining_time,
                          &new_cow_device)) {
            LOG(ERROR) << "Could not map dm-user device for partition "
                       << params.GetPartitionName();
            return false;
        }
        created_devices.EmplaceBack<AutoUnmapDevice>(&dm_, name);
        remaining_time = GetRemainingTime(params.timeout_ms, begin);
        if (remaining_time.count() < 0) return false;
        cow_device = new_cow_device;
    }
    if (!UpdateUsesUserSnapshots(lock)) {
        std::string path;
        if (!MapSnapshot(lock, params.GetPartitionName(), base_device, cow_device, remaining_time,
                         &path)) {
            LOG(ERROR) << "Could not map snapshot for partition: " << params.GetPartitionName();
            return false;
        }
        if (paths) {
            paths->snapshot_device = path;
        }
        LOG(INFO) << "Mapped " << params.GetPartitionName() << " as snapshot device at " << path;
    } else {
        LOG(INFO) << "Mapped " << params.GetPartitionName() << " as snapshot device at "
                  << cow_device;
    }
    created_devices.Release();
    return true;
}
bool SnapshotManager::UnmapPartitionWithSnapshot(LockedFile* lock,
                                                 const std::string& target_partition_name) {
    CHECK(lock);
    if (!UnmapSnapshot(lock, target_partition_name)) {
        return false;
    }
    if (!UnmapCowDevices(lock, target_partition_name)) {
        return false;
    }
    auto base_name = GetBaseDeviceName(target_partition_name);
    if (!DeleteDeviceIfExists(base_name)) {
        LOG(ERROR) << "Cannot delete base device: " << base_name;
        return false;
    }
    auto source_name = GetSourceDeviceName(target_partition_name);
    if (!DeleteDeviceIfExists(source_name)) {
        LOG(ERROR) << "Cannot delete source device: " << source_name;
        return false;
    }
    LOG(INFO) << "Successfully unmapped snapshot " << target_partition_name;
    return true;
}
bool SnapshotManager::MapCowDevices(LockedFile* lock, const CreateLogicalPartitionParams& params,
                                    const SnapshotStatus& snapshot_status,
                                    AutoDeviceList* created_devices, std::string* cow_name) {
    CHECK(lock);
    CHECK(snapshot_status.cow_partition_size() + snapshot_status.cow_file_size() > 0);
    auto begin = std::chrono::steady_clock::now();
    std::string partition_name = params.GetPartitionName();
    std::string cow_image_name = GetCowImageDeviceName(partition_name);
    *cow_name = GetCowName(partition_name);
    if (snapshot_status.cow_file_size() > 0) {
        if (!EnsureImageManager()) return false;
        auto remaining_time = GetRemainingTime(params.timeout_ms, begin);
        if (remaining_time.count() < 0) return false;
        if (!MapCowImage(partition_name, remaining_time).has_value()) {
            LOG(ERROR) << "Could not map cow image for partition: " << partition_name;
            return false;
        }
        created_devices->EmplaceBack<AutoUnmapImage>(images_.get(), cow_image_name);
        if (snapshot_status.cow_partition_size() == 0) {
            *cow_name = std::move(cow_image_name);
            LOG(INFO) << "Mapped COW image for " << partition_name << " at " << *cow_name;
            return true;
        }
    }
    auto remaining_time = GetRemainingTime(params.timeout_ms, begin);
    if (remaining_time.count() < 0) return false;
    CHECK(snapshot_status.cow_partition_size() > 0);
    CreateLogicalPartitionParams cow_partition_params = params;
    cow_partition_params.partition = nullptr;
    cow_partition_params.partition_name = *cow_name;
    cow_partition_params.device_name.clear();
    DmTable table;
    if (!CreateDmTable(cow_partition_params, &table)) {
        return false;
    }
    if (snapshot_status.cow_file_size() > 0) {
        std::string cow_image_device;
        if (!GetMappedImageDeviceStringOrPath(cow_image_name, &cow_image_device)) {
            LOG(ERROR) << "Cannot determine major/minor for: " << cow_image_name;
            return false;
        }
        auto cow_partition_sectors = snapshot_status.cow_partition_size() / kSectorSize;
        auto cow_image_sectors = snapshot_status.cow_file_size() / kSectorSize;
        table.Emplace<DmTargetLinear>(cow_partition_sectors, cow_image_sectors, cow_image_device,
                                      0);
    }
    std::string cow_path;
    if (!dm_.CreateDevice(*cow_name, table, &cow_path, remaining_time)) {
        LOG(ERROR) << "Could not create COW device: " << *cow_name;
        return false;
    }
    created_devices->EmplaceBack<AutoUnmapDevice>(&dm_, *cow_name);
    LOG(INFO) << "Mapped COW device for " << params.GetPartitionName() << " at " << cow_path;
    return true;
}
bool SnapshotManager::UnmapCowDevices(LockedFile* lock, const std::string& name) {
    CHECK(lock);
    if (!EnsureImageManager()) return false;
    if (UpdateUsesCompression(lock) && !UpdateUsesUserSnapshots(lock)) {
        auto dm_user_name = GetDmUserCowName(name, GetSnapshotDriver(lock));
        if (!UnmapDmUserDevice(dm_user_name)) {
            return false;
        }
    }
    if (!DeleteDeviceIfExists(GetCowName(name), 4000ms)) {
        LOG(ERROR) << "Cannot unmap: " << GetCowName(name);
        return false;
    }
    std::string cow_image_name = GetCowImageDeviceName(name);
    if (!images_->UnmapImageIfExists(cow_image_name)) {
        LOG(ERROR) << "Cannot unmap image " << cow_image_name;
        return false;
    }
    return true;
}
bool SnapshotManager::UnmapDmUserDevice(const std::string& dm_user_name) {
    if (dm_.GetState(dm_user_name) == DmDeviceState::INVALID) {
        return true;
    }
    if (!DeleteDeviceIfExists(dm_user_name)) {
        LOG(ERROR) << "Cannot unmap " << dm_user_name;
        return false;
    }
    if (EnsureSnapuserdConnected()) {
        if (!snapuserd_client_->WaitForDeviceDelete(dm_user_name)) {
            LOG(ERROR) << "Failed to wait for " << dm_user_name << " control device to delete";
            return false;
        }
    }
    auto control_device = "/dev/dm-user/" + dm_user_name;
    if (!android::fs_mgr::WaitForFileDeleted(control_device, 10s)) {
        LOG(ERROR) << "Timed out waiting for " << control_device << " to unlink";
        return false;
    }
    return true;
}
bool SnapshotManager::UnmapUserspaceSnapshotDevice(LockedFile* lock,
                                                   const std::string& snapshot_name) {
    auto dm_user_name = GetDmUserCowName(snapshot_name, GetSnapshotDriver(lock));
    if (dm_.GetState(dm_user_name) == DmDeviceState::INVALID) {
        return true;
    }
    CHECK(lock);
    SnapshotStatus snapshot_status;
    if (!ReadSnapshotStatus(lock, snapshot_name, &snapshot_status)) {
        return false;
    }
    if (snapshot_status.state() != SnapshotState::MERGE_COMPLETED) {
        if (!DeleteDeviceIfExists(dm_user_name)) {
            LOG(ERROR) << "Cannot unmap " << dm_user_name;
            return false;
        }
    }
    if (EnsureSnapuserdConnected()) {
        if (!snapuserd_client_->WaitForDeviceDelete(dm_user_name)) {
            LOG(ERROR) << "Failed to wait for " << dm_user_name << " control device to delete";
            return false;
        }
    }
    auto control_device = "/dev/dm-user/" + dm_user_name;
    if (!android::fs_mgr::WaitForFileDeleted(control_device, 10s)) {
        LOG(ERROR) << "Timed out waiting for " << control_device << " to unlink";
        return false;
    }
    return true;
}
bool SnapshotManager::MapAllSnapshots(const std::chrono::milliseconds& timeout_ms) {
    auto lock = LockExclusive();
    if (!lock) return false;
    auto state = ReadUpdateState(lock.get());
    if (state == UpdateState::Unverified) {
        if (GetCurrentSlot() == Slot::Target) {
            LOG(ERROR) << "Cannot call MapAllSnapshots when booting from the target slot.";
            return false;
        }
    } else if (state != UpdateState::Initiated) {
        LOG(ERROR) << "Cannot call MapAllSnapshots from update state: " << state;
        return false;
    }
    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock.get(), &snapshots)) {
        return false;
    }
    const auto& opener = device_->GetPartitionOpener();
    auto slot_suffix = device_->GetOtherSlotSuffix();
    auto slot_number = SlotNumberForSlotSuffix(slot_suffix);
    auto super_device = device_->GetSuperDevice(slot_number);
    auto metadata = android::fs_mgr::ReadMetadata(opener, super_device, slot_number);
    if (!metadata) {
        LOG(ERROR) << "MapAllSnapshots could not read dynamic partition metadata for device: "
                   << super_device;
        return false;
    }
    for (const auto& snapshot : snapshots) {
        if (!UnmapPartitionWithSnapshot(lock.get(), snapshot)) {
            LOG(ERROR) << "MapAllSnapshots could not unmap snapshot: " << snapshot;
            return false;
        }
        CreateLogicalPartitionParams params = {
                .block_device = super_device,
                .metadata = metadata.get(),
                .partition_name = snapshot,
                .timeout_ms = timeout_ms,
                .partition_opener = &opener,
        };
        if (!MapPartitionWithSnapshot(lock.get(), std::move(params), SnapshotContext::Mount,
                                      nullptr)) {
            LOG(ERROR) << "MapAllSnapshots failed to map: " << snapshot;
            return false;
        }
    }
    LOG(INFO) << "MapAllSnapshots succeeded.";
    return true;
}
bool SnapshotManager::UnmapAllSnapshots() {
    auto lock = LockExclusive();
    if (!lock) return false;
    return UnmapAllSnapshots(lock.get());
}
bool SnapshotManager::UnmapAllSnapshots(LockedFile* lock) {
    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock, &snapshots)) {
        return false;
    }
    for (const auto& snapshot : snapshots) {
        if (!UnmapPartitionWithSnapshot(lock, snapshot)) {
            LOG(ERROR) << "Failed to unmap snapshot: " << snapshot;
            return false;
        }
    }
    if (snapuserd_client_) {
        LOG(INFO) << "Shutdown snapuserd daemon";
        snapuserd_client_->DetachSnapuserd();
        snapuserd_client_ = nullptr;
    }
    return true;
}
auto SnapshotManager::OpenFile(const std::string& file, int lock_flags)
        -> std::unique_ptr<LockedFile> {
    unique_fd fd(open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd < 0) {
        PLOG(ERROR) << "Open failed: " << file;
        return nullptr;
    }
    if (lock_flags != 0 && TEMP_FAILURE_RETRY(flock(fd, lock_flags)) < 0) {
        PLOG(ERROR) << "Acquire flock failed: " << file;
        return nullptr;
    }
    int lock_mode = lock_flags & (LOCK_EX | LOCK_SH);
    return std::make_unique<LockedFile>(file, std::move(fd), lock_mode);
}
SnapshotManager::LockedFile::~LockedFile() {
    if (TEMP_FAILURE_RETRY(flock(fd_, LOCK_UN)) < 0) {
        PLOG(ERROR) << "Failed to unlock file: " << path_;
    }
}
std::string SnapshotManager::GetStateFilePath() const {
    return metadata_dir_ + "/state"s;
}
std::string SnapshotManager::GetMergeStateFilePath() const {
    return metadata_dir_ + "/merge_state"s;
}
std::string SnapshotManager::GetLockPath() const {
    return metadata_dir_;
}
std::unique_ptr<SnapshotManager::LockedFile> SnapshotManager::OpenLock(int lock_flags) {
    auto lock_file = GetLockPath();
    return OpenFile(lock_file, lock_flags);
}
std::unique_ptr<SnapshotManager::LockedFile> SnapshotManager::LockShared() {
    return OpenLock(LOCK_SH);
}
std::unique_ptr<SnapshotManager::LockedFile> SnapshotManager::LockExclusive() {
    return OpenLock(LOCK_EX);
}
static UpdateState UpdateStateFromString(const std::string& contents) {
    if (contents.empty() || contents == "none") {
        return UpdateState::None;
    } else if (contents == "initiated") {
        return UpdateState::Initiated;
    } else if (contents == "unverified") {
        return UpdateState::Unverified;
    } else if (contents == "merging") {
        return UpdateState::Merging;
    } else if (contents == "merge-completed") {
        return UpdateState::MergeCompleted;
    } else if (contents == "merge-needs-reboot") {
        return UpdateState::MergeNeedsReboot;
    } else if (contents == "merge-failed") {
        return UpdateState::MergeFailed;
    } else if (contents == "cancelled") {
        return UpdateState::Cancelled;
    } else {
        LOG(ERROR) << "Unknown merge state in update state file: \"" << contents << "\"";
        return UpdateState::None;
    }
}
std::ostream& operator<<(std::ostream& os, UpdateState state) {
    switch (state) {
        case UpdateState::None:
            return os << "none";
        case UpdateState::Initiated:
            return os << "initiated";
        case UpdateState::Unverified:
            return os << "unverified";
        case UpdateState::Merging:
            return os << "merging";
        case UpdateState::MergeCompleted:
            return os << "merge-completed";
        case UpdateState::MergeNeedsReboot:
            return os << "merge-needs-reboot";
        case UpdateState::MergeFailed:
            return os << "merge-failed";
        case UpdateState::Cancelled:
            return os << "cancelled";
        default:
            LOG(ERROR) << "Unknown update state: " << static_cast<uint32_t>(state);
            return os;
    }
}
std::ostream& operator<<(std::ostream& os, MergePhase phase) {
    switch (phase) {
        case MergePhase::NO_MERGE:
            return os << "none";
        case MergePhase::FIRST_PHASE:
            return os << "first";
        case MergePhase::SECOND_PHASE:
            return os << "second";
        default:
            LOG(ERROR) << "Unknown merge phase: " << static_cast<uint32_t>(phase);
            return os << "unknown(" << static_cast<uint32_t>(phase) << ")";
    }
}
UpdateState SnapshotManager::ReadUpdateState(LockedFile* lock) {
    SnapshotUpdateStatus status = ReadSnapshotUpdateStatus(lock);
    return status.state();
}
SnapshotUpdateStatus SnapshotManager::ReadSnapshotUpdateStatus(LockedFile* lock) {
    CHECK(lock);
    SnapshotUpdateStatus status = {};
    std::string contents;
    if (!android::base::ReadFileToString(GetStateFilePath(), &contents)) {
        PLOG(ERROR) << "Read state file failed";
        status.set_state(UpdateState::None);
        return status;
    }
    if (!status.ParseFromString(contents)) {
        LOG(WARNING) << "Unable to parse state file as SnapshotUpdateStatus, using the old format";
        status.set_state(UpdateStateFromString(contents));
    }
    return status;
}
bool SnapshotManager::WriteUpdateState(LockedFile* lock, UpdateState state,
                                       MergeFailureCode failure_code) {
    SnapshotUpdateStatus status;
    status.set_state(state);
    switch (state) {
        case UpdateState::MergeFailed:
            status.set_merge_failure_code(failure_code);
            break;
        case UpdateState::Initiated:
            status.set_source_build_fingerprint(
                    android::base::GetProperty("ro.build.fingerprint", ""));
            break;
        default:
            break;
    }
    if (!(state == UpdateState::Initiated || state == UpdateState::None)) {
        SnapshotUpdateStatus old_status = ReadSnapshotUpdateStatus(lock);
        status.set_using_snapuserd(old_status.using_snapuserd());
        status.set_source_build_fingerprint(old_status.source_build_fingerprint());
        status.set_merge_phase(old_status.merge_phase());
        status.set_userspace_snapshots(old_status.userspace_snapshots());
        status.set_io_uring_enabled(old_status.io_uring_enabled());
    }
    return WriteSnapshotUpdateStatus(lock, status);
}
bool SnapshotManager::WriteSnapshotUpdateStatus(LockedFile* lock,
                                                const SnapshotUpdateStatus& status) {
    CHECK(lock);
    CHECK(lock->lock_mode() == LOCK_EX);
    std::string contents;
    if (!status.SerializeToString(&contents)) {
        LOG(ERROR) << "Unable to serialize SnapshotUpdateStatus.";
        return false;
    }
#ifdef LIBSNAPSHOT_USE_HAL
    auto merge_status = MergeStatus::UNKNOWN;
    switch (status.state()) {
        case UpdateState::None:
        case UpdateState::MergeNeedsReboot:
        case UpdateState::MergeCompleted:
        case UpdateState::Initiated:
            merge_status = MergeStatus::NONE;
            break;
        case UpdateState::Unverified:
            merge_status = MergeStatus::SNAPSHOTTED;
            break;
        case UpdateState::Merging:
        case UpdateState::MergeFailed:
            merge_status = MergeStatus::MERGING;
            break;
        default:
            LOG(ERROR) << "Unexpected update status: " << status.state();
            break;
    }
    bool set_before_write =
            merge_status == MergeStatus::SNAPSHOTTED || merge_status == MergeStatus::MERGING;
    if (set_before_write && !device_->SetBootControlMergeStatus(merge_status)) {
        return false;
    }
#endif
    if (!WriteStringToFileAtomic(contents, GetStateFilePath())) {
        PLOG(ERROR) << "Could not write to state file";
        return false;
    }
#ifdef LIBSNAPSHOT_USE_HAL
    if (!set_before_write && !device_->SetBootControlMergeStatus(merge_status)) {
        return false;
    }
#endif
    return true;
}
std::string SnapshotManager::GetSnapshotStatusFilePath(const std::string& name) {
    auto file = metadata_dir_ + "/snapshots/"s + name;
    return file;
}
bool SnapshotManager::ReadSnapshotStatus(LockedFile* lock, const std::string& name,
                                         SnapshotStatus* status) {
    CHECK(lock);
    auto path = GetSnapshotStatusFilePath(name);
    unique_fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd < 0) {
        PLOG(ERROR) << "Open failed: " << path;
        return false;
    }
    if (!status->ParseFromFileDescriptor(fd.get())) {
        PLOG(ERROR) << "Unable to parse " << path << " as SnapshotStatus";
        return false;
    }
    if (status->name() != name) {
        LOG(WARNING) << "Found snapshot status named " << status->name() << " in " << path;
        status->set_name(name);
    }
    return true;
}
bool SnapshotManager::WriteSnapshotStatus(LockedFile* lock, const SnapshotStatus& status) {
    CHECK(lock);
    CHECK(lock->lock_mode() == LOCK_EX);
    CHECK(!status.name().empty());
    auto path = GetSnapshotStatusFilePath(status.name());
    std::string content;
    if (!status.SerializeToString(&content)) {
        LOG(ERROR) << "Unable to serialize SnapshotStatus for " << status.name();
        return false;
    }
    if (!WriteStringToFileAtomic(content, path)) {
        PLOG(ERROR) << "Unable to write SnapshotStatus to " << path;
        return false;
    }
    return true;
}
bool SnapshotManager::EnsureImageManager() {
    if (images_) return true;
    images_ = device_->OpenImageManager();
    if (!images_) {
        LOG(ERROR) << "Could not open ImageManager";
        return false;
    }
    return true;
}
bool SnapshotManager::EnsureSnapuserdConnected() {
    if (snapuserd_client_) {
        return true;
    }
    if (!use_first_stage_snapuserd_ && !EnsureSnapuserdStarted()) {
        return false;
    }
    snapuserd_client_ = SnapuserdClient::Connect(kSnapuserdSocket, 10s);
    if (!snapuserd_client_) {
        LOG(ERROR) << "Unable to connect to snapuserd";
        return false;
    }
    return true;
}
void SnapshotManager::UnmapAndDeleteCowPartition(MetadataBuilder* current_metadata) {
    std::vector<std::string> to_delete;
    for (auto* existing_cow_partition : current_metadata->ListPartitionsInGroup(kCowGroupName)) {
        if (!DeleteDeviceIfExists(existing_cow_partition->name())) {
            LOG(WARNING) << existing_cow_partition->name()
                         << " cannot be unmapped and its space cannot be reclaimed";
            continue;
        }
        to_delete.push_back(existing_cow_partition->name());
    }
    for (const auto& name : to_delete) {
        current_metadata->RemovePartition(name);
    }
}
static Return AddRequiredSpace(Return orig,
                               const std::map<std::string, SnapshotStatus>& all_snapshot_status) {
    if (orig.error_code() != Return::ErrorCode::NO_SPACE) {
        return orig;
    }
    uint64_t sum = 0;
    for (auto&& [name, status] : all_snapshot_status) {
        sum += status.cow_file_size();
    }
    LOG(INFO) << "Calculated needed COW space: " << sum << " bytes";
    return Return::NoSpace(sum);
}
Return SnapshotManager::CreateUpdateSnapshots(const DeltaArchiveManifest& manifest) {
    auto lock = LockExclusive();
    if (!lock) return Return::Error();
    auto update_state = ReadUpdateState(lock.get());
    if (update_state != UpdateState::Initiated) {
        LOG(ERROR) << "Cannot create update snapshots in state " << update_state;
        return Return::Error();
    }
    if (device_->IsOverlayfsSetup()) {
        LOG(ERROR) << "Cannot create update snapshots with overlayfs setup. Run `adb enable-verity`"
                   << ", reboot, then try again.";
        return Return::Error();
    }
    const auto& opener = device_->GetPartitionOpener();
    auto current_suffix = device_->GetSlotSuffix();
    uint32_t current_slot = SlotNumberForSlotSuffix(current_suffix);
    auto target_suffix = device_->GetOtherSlotSuffix();
    uint32_t target_slot = SlotNumberForSlotSuffix(target_suffix);
    auto current_super = device_->GetSuperDevice(current_slot);
    auto current_metadata = MetadataBuilder::New(opener, current_super, current_slot);
    if (current_metadata == nullptr) {
        LOG(ERROR) << "Cannot create metadata builder.";
        return Return::Error();
    }
    auto target_metadata =
            MetadataBuilder::NewForUpdate(opener, current_super, current_slot, target_slot);
    if (target_metadata == nullptr) {
        LOG(ERROR) << "Cannot create target metadata builder.";
        return Return::Error();
    }
    for (const auto& group_name : current_metadata->ListGroups()) {
        if (android::base::EndsWith(group_name, target_suffix)) {
            current_metadata->RemoveGroupAndPartitions(group_name);
        }
    }
    SnapshotMetadataUpdater metadata_updater(target_metadata.get(), target_slot, manifest);
    if (!metadata_updater.Update()) {
        LOG(ERROR) << "Cannot calculate new metadata.";
        return Return::Error();
    }
    UnmapAndDeleteCowPartition(current_metadata.get());
    CHECK(current_metadata->GetBlockDevicePartitionName(0) == LP_METADATA_DEFAULT_PARTITION_NAME &&
          target_metadata->GetBlockDevicePartitionName(0) == LP_METADATA_DEFAULT_PARTITION_NAME);
    std::map<std::string, SnapshotStatus> all_snapshot_status;
    AutoDeviceList created_devices;
    const auto& dap_metadata = manifest.dynamic_partition_metadata();
    CowOptions options;
    CowWriter writer(options);
    bool cow_format_support = true;
    if (dap_metadata.cow_version() < writer.GetCowVersion()) {
        cow_format_support = false;
    }
    LOG(INFO) << " dap_metadata.cow_version(): " << dap_metadata.cow_version()
              << " writer.GetCowVersion(): " << writer.GetCowVersion();
<<<<<<< HEAD
    bool userspace_snapshots = CanUseUserspaceSnapshots();
    bool legacy_compression = GetLegacyCompressionEnabledProperty();
    std::string vabc_disable_reason;
    if (!dap_metadata.vabc_enabled()) {
        vabc_disable_reason = "not enabled metadata";
    } else if (device_->IsRecovery()) {
        vabc_disable_reason = "recovery";
    } else if (!cow_format_support) {
        vabc_disable_reason = "cow format not supported";
    } else if (!KernelSupportsCompressedSnapshots()) {
        vabc_disable_reason = "kernel missing userspace block device support";
    }
    if (!vabc_disable_reason.empty()) {
        if (userspace_snapshots) {
            LOG(INFO) << "Userspace snapshots disabled: " << vabc_disable_reason;
        }
        if (legacy_compression) {
            LOG(INFO) << "Compression disabled: " << vabc_disable_reason;
        }
        userspace_snapshots = false;
        legacy_compression = false;
    }
    const bool using_snapuserd = userspace_snapshots || legacy_compression;
    if (!using_snapuserd) {
        LOG(INFO) << "Using legacy Virtual A/B (dm-snapshot)";
    }
||||||| 5658f3465
    bool use_compression = IsCompressionEnabled() && dap_metadata.vabc_enabled() &&
                           !device_->IsRecovery() && cow_format_support;
=======
    bool use_compression = IsCompressionEnabled() && dap_metadata.vabc_enabled() &&
                           !device_->IsRecovery() && cow_format_support &&
                           KernelSupportsCompressedSnapshots();
>>>>>>> 3f96053b
    std::string compression_algorithm;
    if (using_snapuserd) {
        compression_algorithm = dap_metadata.vabc_compression_param();
        if (compression_algorithm.empty()) {
            compression_algorithm = "gz";
        }
    }
    PartitionCowCreator cow_creator{
            .target_metadata = target_metadata.get(),
            .target_suffix = target_suffix,
            .target_partition = nullptr,
            .current_metadata = current_metadata.get(),
            .current_suffix = current_suffix,
            .update = nullptr,
            .extra_extents = {},
            .using_snapuserd = using_snapuserd,
            .compression_algorithm = compression_algorithm,
    };
    if (dap_metadata.vabc_feature_set().has_threaded()) {
        cow_creator.enable_threading = dap_metadata.vabc_feature_set().threaded();
    }
    if (dap_metadata.vabc_feature_set().has_batch_writes()) {
        cow_creator.batched_writes = dap_metadata.vabc_feature_set().batch_writes();
    }
    auto ret = CreateUpdateSnapshotsInternal(lock.get(), manifest, &cow_creator, &created_devices,
                                             &all_snapshot_status);
    if (!ret.is_ok()) {
        LOG(ERROR) << "CreateUpdateSnapshotsInternal failed: " << ret.string();
        return ret;
    }
    auto exported_target_metadata = target_metadata->Export();
    if (exported_target_metadata == nullptr) {
        LOG(ERROR) << "Cannot export target metadata";
        return Return::Error();
    }
    ret = InitializeUpdateSnapshots(lock.get(), target_metadata.get(),
                                    exported_target_metadata.get(), target_suffix,
                                    all_snapshot_status);
    if (!ret.is_ok()) return ret;
    if (!UpdatePartitionTable(opener, device_->GetSuperDevice(target_slot),
                              *exported_target_metadata, target_slot)) {
        LOG(ERROR) << "Cannot write target metadata";
        return Return::Error();
    }
    if (using_snapuserd) {
        auto metadata = current_metadata->Export();
        if (!metadata) {
            LOG(ERROR) << "Could not export current metadata";
            return Return::Error();
        }
        auto path = GetOldPartitionMetadataPath();
        if (!android::fs_mgr::WriteToImageFile(path, *metadata.get())) {
            LOG(ERROR) << "Cannot write old metadata to " << path;
            return Return::Error();
        }
    }
    SnapshotUpdateStatus status = ReadSnapshotUpdateStatus(lock.get());
    status.set_state(update_state);
    status.set_using_snapuserd(using_snapuserd);
    if (userspace_snapshots) {
        status.set_userspace_snapshots(true);
        LOG(INFO) << "Virtual A/B using userspace snapshots";
        if (GetIouringEnabledProperty()) {
            status.set_io_uring_enabled(true);
            LOG(INFO) << "io_uring for snapshots enabled";
        }
    } else if (legacy_compression) {
        LOG(INFO) << "Virtual A/B using legacy snapuserd";
    } else {
        LOG(INFO) << "Virtual A/B using dm-snapshot";
    }
    is_snapshot_userspace_.emplace(userspace_snapshots);
    if (!device()->IsTestDevice() && using_snapuserd) {
        std::unique_ptr<SnapuserdClient> snapuserd_client = std::move(snapuserd_client_);
        if (!snapuserd_client) {
            snapuserd_client = SnapuserdClient::Connect(kSnapuserdSocket, 5s);
        }
        if (snapuserd_client) {
            snapuserd_client->DetachSnapuserd();
            snapuserd_client = nullptr;
        }
    }
    if (!WriteSnapshotUpdateStatus(lock.get(), status)) {
        LOG(ERROR) << "Unable to write new update state";
        return Return::Error();
    }
    created_devices.Release();
    LOG(INFO) << "Successfully created all snapshots for target slot " << target_suffix;
    return Return::Ok();
}
Return SnapshotManager::CreateUpdateSnapshotsInternal(
        LockedFile* lock, const DeltaArchiveManifest& manifest, PartitionCowCreator* cow_creator,
        AutoDeviceList* created_devices,
        std::map<std::string, SnapshotStatus>* all_snapshot_status) {
    CHECK(lock);
    auto* target_metadata = cow_creator->target_metadata;
    const auto& target_suffix = cow_creator->target_suffix;
    if (!target_metadata->AddGroup(kCowGroupName, 0)) {
        LOG(ERROR) << "Cannot add group " << kCowGroupName;
        return Return::Error();
    }
    std::map<std::string, const PartitionUpdate*> partition_map;
    std::map<std::string, std::vector<Extent>> extra_extents_map;
    for (const auto& partition_update : manifest.partitions()) {
        auto suffixed_name = partition_update.partition_name() + target_suffix;
        auto&& [it, inserted] = partition_map.emplace(suffixed_name, &partition_update);
        if (!inserted) {
            LOG(ERROR) << "Duplicated partition " << partition_update.partition_name()
                       << " in update manifest.";
            return Return::Error();
        }
        auto& extra_extents = extra_extents_map[suffixed_name];
        if (partition_update.has_hash_tree_extent()) {
            extra_extents.push_back(partition_update.hash_tree_extent());
        }
        if (partition_update.has_fec_extent()) {
            extra_extents.push_back(partition_update.fec_extent());
        }
    }
    for (auto* target_partition : ListPartitionsWithSuffix(target_metadata, target_suffix)) {
        cow_creator->target_partition = target_partition;
        cow_creator->update = nullptr;
        auto iter = partition_map.find(target_partition->name());
        if (iter != partition_map.end()) {
            cow_creator->update = iter->second;
        } else {
            LOG(INFO) << target_partition->name()
                      << " isn't included in the payload, skipping the cow creation.";
            continue;
        }
        cow_creator->extra_extents.clear();
        auto extra_extents_it = extra_extents_map.find(target_partition->name());
        if (extra_extents_it != extra_extents_map.end()) {
            cow_creator->extra_extents = std::move(extra_extents_it->second);
        }
        auto cow_creator_ret = cow_creator->Run();
        if (!cow_creator_ret.has_value()) {
            LOG(ERROR) << "PartitionCowCreator returned no value for " << target_partition->name();
            return Return::Error();
        }
        LOG(INFO) << "For partition " << target_partition->name()
                  << ", device size = " << cow_creator_ret->snapshot_status.device_size()
                  << ", snapshot size = " << cow_creator_ret->snapshot_status.snapshot_size()
                  << ", cow partition size = "
                  << cow_creator_ret->snapshot_status.cow_partition_size()
                  << ", cow file size = " << cow_creator_ret->snapshot_status.cow_file_size();
        if (!DeleteSnapshot(lock, target_partition->name())) {
            LOG(ERROR) << "Cannot delete existing snapshot before creating a new one for partition "
                       << target_partition->name();
            return Return::Error();
        }
        bool needs_snapshot = cow_creator_ret->snapshot_status.snapshot_size() > 0;
        bool needs_cow = (cow_creator_ret->snapshot_status.cow_partition_size() +
                          cow_creator_ret->snapshot_status.cow_file_size()) > 0;
        CHECK(needs_snapshot == needs_cow);
        if (!needs_snapshot) {
            LOG(INFO) << "Skip creating snapshot for partition " << target_partition->name()
                      << "because nothing needs to be snapshotted.";
            continue;
        }
        auto name = target_partition->name();
        auto old_partition_name =
                name.substr(0, name.size() - target_suffix.size()) + cow_creator->current_suffix;
        auto old_partition = cow_creator->current_metadata->FindPartition(old_partition_name);
        if (old_partition) {
            cow_creator_ret->snapshot_status.set_old_partition_size(old_partition->size());
        }
        if (!CreateSnapshot(lock, cow_creator, &cow_creator_ret->snapshot_status)) {
            return Return::Error();
        }
        created_devices->EmplaceBack<AutoDeleteSnapshot>(this, lock, target_partition->name());
        if (cow_creator_ret->snapshot_status.cow_partition_size() > 0) {
            CHECK(cow_creator_ret->snapshot_status.cow_partition_size() % kSectorSize == 0)
                    << "cow_partition_size == "
                    << cow_creator_ret->snapshot_status.cow_partition_size()
                    << " is not a multiple of sector size " << kSectorSize;
            auto cow_partition = target_metadata->AddPartition(GetCowName(target_partition->name()),
                                                               kCowGroupName, 0 );
            if (cow_partition == nullptr) {
                return Return::Error();
            }
            if (!target_metadata->ResizePartition(
                        cow_partition, cow_creator_ret->snapshot_status.cow_partition_size(),
                        cow_creator_ret->cow_partition_usable_regions)) {
                LOG(ERROR) << "Cannot create COW partition on metadata with size "
                           << cow_creator_ret->snapshot_status.cow_partition_size();
                return Return::Error();
            }
        }
        all_snapshot_status->emplace(target_partition->name(),
                                     std::move(cow_creator_ret->snapshot_status));
        LOG(INFO) << "Successfully created snapshot partition for " << target_partition->name();
    }
    LOG(INFO) << "Allocating CoW images.";
    for (auto&& [name, snapshot_status] : *all_snapshot_status) {
        if (snapshot_status.cow_file_size() > 0) {
            auto ret = CreateCowImage(lock, name);
            if (!ret.is_ok()) {
                LOG(ERROR) << "CreateCowImage failed: " << ret.string();
                return AddRequiredSpace(ret, *all_snapshot_status);
            }
        }
        LOG(INFO) << "Successfully created snapshot for " << name;
    }
    return Return::Ok();
}
Return SnapshotManager::InitializeUpdateSnapshots(
        LockedFile* lock, MetadataBuilder* target_metadata,
        const LpMetadata* exported_target_metadata, const std::string& target_suffix,
        const std::map<std::string, SnapshotStatus>& all_snapshot_status) {
    CHECK(lock);
    CreateLogicalPartitionParams cow_params{
            .block_device = LP_METADATA_DEFAULT_PARTITION_NAME,
            .metadata = exported_target_metadata,
            .timeout_ms = std::chrono::milliseconds::max(),
            .partition_opener = &device_->GetPartitionOpener(),
    };
    for (auto* target_partition : ListPartitionsWithSuffix(target_metadata, target_suffix)) {
        AutoDeviceList created_devices_for_cow;
        if (!UnmapPartitionWithSnapshot(lock, target_partition->name())) {
            LOG(ERROR) << "Cannot unmap existing COW devices before re-mapping them for zero-fill: "
                       << target_partition->name();
            return Return::Error();
        }
        auto it = all_snapshot_status.find(target_partition->name());
        if (it == all_snapshot_status.end()) continue;
        cow_params.partition_name = target_partition->name();
        std::string cow_name;
        if (!MapCowDevices(lock, cow_params, it->second, &created_devices_for_cow, &cow_name)) {
            return Return::Error();
        }
        std::string cow_path;
        if (!images_->GetMappedImageDevice(cow_name, &cow_path)) {
            LOG(ERROR) << "Cannot determine path for " << cow_name;
            return Return::Error();
        }
        if (it->second.using_snapuserd()) {
            unique_fd fd(open(cow_path.c_str(), O_RDWR | O_CLOEXEC));
            if (fd < 0) {
                PLOG(ERROR) << "open " << cow_path << " failed for snapshot "
                            << cow_params.partition_name;
                return Return::Error();
            }
            CowOptions options;
            if (device()->IsTestDevice()) {
                options.scratch_space = false;
            }
            options.compression = it->second.compression_algorithm();
            CowWriter writer(options);
            if (!writer.Initialize(fd) || !writer.Finalize()) {
                LOG(ERROR) << "Could not initialize COW device for " << target_partition->name();
                return Return::Error();
            }
        } else {
            auto ret = InitializeKernelCow(cow_path);
            if (!ret.is_ok()) {
                LOG(ERROR) << "Can't zero-fill COW device for " << target_partition->name() << ": "
                           << cow_path;
                return AddRequiredSpace(ret, all_snapshot_status);
            }
        }
    };
    return Return::Ok();
}
bool SnapshotManager::MapUpdateSnapshot(const CreateLogicalPartitionParams& params,
                                        std::string* snapshot_path) {
    auto lock = LockShared();
    if (!lock) return false;
    if (!UnmapPartitionWithSnapshot(lock.get(), params.GetPartitionName())) {
        LOG(ERROR) << "Cannot unmap existing snapshot before re-mapping it: "
                   << params.GetPartitionName();
        return false;
    }
    SnapshotStatus status;
    if (!ReadSnapshotStatus(lock.get(), params.GetPartitionName(), &status)) {
        return false;
    }
    if (status.using_snapuserd()) {
        LOG(ERROR) << "Cannot use MapUpdateSnapshot with snapuserd";
        return false;
    }
    SnapshotPaths paths;
    if (!MapPartitionWithSnapshot(lock.get(), params, SnapshotContext::Update, &paths)) {
        return false;
    }
    if (!paths.snapshot_device.empty()) {
        *snapshot_path = paths.snapshot_device;
    } else {
        *snapshot_path = paths.target_device;
    }
    DCHECK(!snapshot_path->empty());
    return true;
}
std::unique_ptr<ISnapshotWriter> SnapshotManager::OpenSnapshotWriter(
        const android::fs_mgr::CreateLogicalPartitionParams& params,
        const std::optional<std::string>& source_device) {
#if defined(LIBSNAPSHOT_NO_COW_WRITE)
    (void)params;
    (void)source_device;
    LOG(ERROR) << "Snapshots cannot be written in first-stage init or recovery";
    return nullptr;
#else
    auto lock = LockShared();
    if (!lock) return nullptr;
    if (!UnmapPartitionWithSnapshot(lock.get(), params.GetPartitionName())) {
        LOG(ERROR) << "Cannot unmap existing snapshot before re-mapping it: "
                   << params.GetPartitionName();
        return nullptr;
    }
    SnapshotPaths paths;
    if (!MapPartitionWithSnapshot(lock.get(), params, SnapshotContext::Update, &paths)) {
        return nullptr;
    }
    SnapshotStatus status;
    if (!paths.cow_device_name.empty()) {
        if (!ReadSnapshotStatus(lock.get(), params.GetPartitionName(), &status)) {
            return nullptr;
        }
    } else {
        LOG(ERROR) << "No snapshot available for partition " << params.GetPartitionName();
        return nullptr;
    }
    if (status.using_snapuserd()) {
        return OpenCompressedSnapshotWriter(lock.get(), source_device, params.GetPartitionName(),
                                            status, paths);
    }
    return OpenKernelSnapshotWriter(lock.get(), source_device, params.GetPartitionName(), status,
                                    paths);
#endif
}
bool SnapshotManager::UnmapUpdateSnapshot(const std::string& target_partition_name) {
    auto lock = LockShared();
    if (!lock) return false;
    return UnmapPartitionWithSnapshot(lock.get(), target_partition_name);
}
bool SnapshotManager::UnmapAllPartitionsInRecovery() {
    auto lock = LockExclusive();
    if (!lock) return false;
    const auto& opener = device_->GetPartitionOpener();
    uint32_t slot = SlotNumberForSlotSuffix(device_->GetSlotSuffix());
    auto super_device = device_->GetSuperDevice(slot);
    auto metadata = android::fs_mgr::ReadMetadata(opener, super_device, slot);
    if (!metadata) {
        LOG(ERROR) << "Could not read dynamic partition metadata for device: " << super_device;
        return false;
    }
    bool ok = true;
    for (const auto& partition : metadata->partitions) {
        auto partition_name = GetPartitionName(partition);
        ok &= UnmapPartitionWithSnapshot(lock.get(), partition_name);
    }
    return ok;
}
std::ostream& operator<<(std::ostream& os, SnapshotManager::Slot slot) {
    switch (slot) {
        case SnapshotManager::Slot::Unknown:
            return os << "unknown";
        case SnapshotManager::Slot::Source:
            return os << "source";
        case SnapshotManager::Slot::Target:
            return os << "target";
    }
}
bool SnapshotManager::Dump(std::ostream& os) {
    auto file = OpenLock(0 );
    if (!file) return false;
    std::stringstream ss;
    auto update_status = ReadSnapshotUpdateStatus(file.get());
    ss << "Update state: " << update_status.state() << std::endl;
    ss << "Using snapuserd: " << update_status.using_snapuserd() << std::endl;
    ss << "Using userspace snapshots: " << update_status.userspace_snapshots() << std::endl;
    ss << "Using io_uring: " << update_status.io_uring_enabled() << std::endl;
    ss << "Using XOR compression: " << GetXorCompressionEnabledProperty() << std::endl;
    ss << "Current slot: " << device_->GetSlotSuffix() << std::endl;
    ss << "Boot indicator: booting from " << GetCurrentSlot() << " slot" << std::endl;
    ss << "Rollback indicator: "
       << (access(GetRollbackIndicatorPath().c_str(), F_OK) == 0 ? "exists" : strerror(errno))
       << std::endl;
    ss << "Forward merge indicator: "
       << (access(GetForwardMergeIndicatorPath().c_str(), F_OK) == 0 ? "exists" : strerror(errno))
       << std::endl;
    ss << "Source build fingerprint: " << update_status.source_build_fingerprint() << std::endl;
    if (update_status.state() == UpdateState::Merging) {
        ss << "Merge completion: ";
        if (!EnsureSnapuserdConnected()) {
            ss << "N/A";
        } else {
            ss << snapuserd_client_->GetMergePercent() << "%";
        }
        ss << std::endl;
        ss << "Merge phase: " << update_status.merge_phase() << std::endl;
    }
    bool ok = true;
    std::vector<std::string> snapshots;
    if (!ListSnapshots(file.get(), &snapshots)) {
        LOG(ERROR) << "Could not list snapshots";
        snapshots.clear();
        ok = false;
    }
    for (const auto& name : snapshots) {
        ss << "Snapshot: " << name << std::endl;
        SnapshotStatus status;
        if (!ReadSnapshotStatus(file.get(), name, &status)) {
            ok = false;
            continue;
        }
        ss << "    state: " << SnapshotState_Name(status.state()) << std::endl;
        ss << "    device size (bytes): " << status.device_size() << std::endl;
        ss << "    snapshot size (bytes): " << status.snapshot_size() << std::endl;
        ss << "    cow partition size (bytes): " << status.cow_partition_size() << std::endl;
        ss << "    cow file size (bytes): " << status.cow_file_size() << std::endl;
        ss << "    allocated sectors: " << status.sectors_allocated() << std::endl;
        ss << "    metadata sectors: " << status.metadata_sectors() << std::endl;
        ss << "    compression: " << status.compression_algorithm() << std::endl;
        ss << "    merge phase: " << DecideMergePhase(status) << std::endl;
    }
    os << ss.rdbuf();
    return ok;
}
std::unique_ptr<AutoDevice> SnapshotManager::EnsureMetadataMounted() {
    if (!device_->IsRecovery()) {
        LOG(INFO) << "EnsureMetadataMounted does nothing in Android mode.";
        return std::unique_ptr<AutoUnmountDevice>(new AutoUnmountDevice());
    }
    auto ret = AutoUnmountDevice::New(device_->GetMetadataDir());
    if (ret == nullptr) return nullptr;
    if (!LockShared()) {
        LOG(WARNING) << "/metadata is mounted, but errors occur when acquiring a shared lock. "
                        "Subsequent calls to SnapshotManager will fail. Unmounting /metadata now.";
        return nullptr;
    }
    return ret;
}
bool SnapshotManager::HandleImminentDataWipe(const std::function<void()>& callback) {
    if (!device_->IsRecovery()) {
        LOG(ERROR) << "Data wipes are only allowed in recovery.";
        return false;
    }
    auto mount = EnsureMetadataMounted();
    if (!mount || !mount->HasDevice()) {
        return true;
    }
    auto state_file = GetStateFilePath();
    if (access(state_file.c_str(), F_OK) != 0 && errno == ENOENT) {
        return true;
    }
    auto slot_number = SlotNumberForSlotSuffix(device_->GetSlotSuffix());
    auto super_path = device_->GetSuperDevice(slot_number);
    if (!CreateLogicalAndSnapshotPartitions(super_path, 20s)) {
        LOG(ERROR) << "Unable to map partitions to complete merge.";
        return false;
    }
    auto process_callback = [&]() -> bool {
        if (callback) {
            callback();
        }
        return true;
    };
    in_factory_data_reset_ = true;
    UpdateState state =
            ProcessUpdateStateOnDataWipe(true , process_callback);
    in_factory_data_reset_ = false;
    if (state == UpdateState::MergeFailed) {
        return false;
    }
    if (!UnmapAllPartitionsInRecovery()) {
        LOG(ERROR) << "Unable to unmap all partitions; fastboot may fail to flash.";
    }
    if (state != UpdateState::None) {
        auto lock = LockExclusive();
        if (!lock) return false;
        WriteUpdateState(lock.get(), UpdateState::None);
    }
    return true;
}
bool SnapshotManager::FinishMergeInRecovery() {
    if (!device_->IsRecovery()) {
        LOG(ERROR) << "Data wipes are only allowed in recovery.";
        return false;
    }
    auto mount = EnsureMetadataMounted();
    if (!mount || !mount->HasDevice()) {
        return false;
    }
    auto slot_number = SlotNumberForSlotSuffix(device_->GetSlotSuffix());
    auto super_path = device_->GetSuperDevice(slot_number);
    if (!CreateLogicalAndSnapshotPartitions(super_path, 20s)) {
        LOG(ERROR) << "Unable to map partitions to complete merge.";
        return false;
    }
    UpdateState state = ProcessUpdateState();
    if (state != UpdateState::MergeCompleted) {
        LOG(ERROR) << "Merge returned unexpected status: " << state;
        return false;
    }
    if (!UnmapAllPartitionsInRecovery()) {
        LOG(ERROR) << "Unable to unmap all partitions; fastboot may fail to flash.";
    }
    return true;
}
UpdateState SnapshotManager::ProcessUpdateStateOnDataWipe(bool allow_forward_merge,
                                                          const std::function<bool()>& callback) {
    auto slot_number = SlotNumberForSlotSuffix(device_->GetSlotSuffix());
    UpdateState state = ProcessUpdateState(callback);
    LOG(INFO) << "Update state in recovery: " << state;
    switch (state) {
        case UpdateState::MergeFailed:
            LOG(ERROR) << "Unrecoverable merge failure detected.";
            return state;
        case UpdateState::Unverified: {
            auto slot = GetCurrentSlot();
            if (slot == Slot::Target) {
                if (allow_forward_merge &&
                    access(GetForwardMergeIndicatorPath().c_str(), F_OK) == 0) {
                    LOG(INFO) << "Forward merge allowed, initiating merge now.";
                    if (!InitiateMerge()) {
                        LOG(ERROR) << "Failed to initiate merge on data wipe.";
                        return UpdateState::MergeFailed;
                    }
                    return ProcessUpdateStateOnDataWipe(false , callback);
                }
                LOG(ERROR) << "Reverting to old slot since update will be deleted.";
                device_->SetSlotAsUnbootable(slot_number);
            } else {
                LOG(INFO) << "Booting from " << slot << " slot, no action is taken.";
            }
            break;
        }
        case UpdateState::MergeNeedsReboot:
            LOG(ERROR) << "Unexpected merge-needs-reboot state in recovery.";
            break;
        default:
            break;
    }
    return state;
}
bool SnapshotManager::EnsureNoOverflowSnapshot(LockedFile* lock) {
    CHECK(lock);
    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock, &snapshots)) {
        LOG(ERROR) << "Could not list snapshots.";
        return false;
    }
    for (const auto& snapshot : snapshots) {
        SnapshotStatus status;
        if (!ReadSnapshotStatus(lock, snapshot, &status)) {
            return false;
        }
        if (status.using_snapuserd()) {
            continue;
        }
        std::vector<DeviceMapper::TargetInfo> targets;
        if (!dm_.GetTableStatus(snapshot, &targets)) {
            LOG(ERROR) << "Could not read snapshot device table: " << snapshot;
            return false;
        }
        if (targets.size() != 1) {
            LOG(ERROR) << "Unexpected device-mapper table for snapshot: " << snapshot
                       << ", size = " << targets.size();
            return false;
        }
        if (targets[0].IsOverflowSnapshot()) {
            LOG(ERROR) << "Detected overflow in snapshot " << snapshot
                       << ", CoW device size computation is wrong!";
            return false;
        }
    }
    return true;
}
CreateResult SnapshotManager::RecoveryCreateSnapshotDevices() {
    if (!device_->IsRecovery()) {
        LOG(ERROR) << __func__ << " is only allowed in recovery.";
        return CreateResult::NOT_CREATED;
    }
    auto mount = EnsureMetadataMounted();
    if (!mount || !mount->HasDevice()) {
        LOG(ERROR) << "Couldn't mount Metadata.";
        return CreateResult::NOT_CREATED;
    }
    return RecoveryCreateSnapshotDevices(mount);
}
CreateResult SnapshotManager::RecoveryCreateSnapshotDevices(
        const std::unique_ptr<AutoDevice>& metadata_device) {
    if (!device_->IsRecovery()) {
        LOG(ERROR) << __func__ << " is only allowed in recovery.";
        return CreateResult::NOT_CREATED;
    }
    if (metadata_device == nullptr || !metadata_device->HasDevice()) {
        LOG(ERROR) << "Metadata not mounted.";
        return CreateResult::NOT_CREATED;
    }
    auto state_file = GetStateFilePath();
    if (access(state_file.c_str(), F_OK) != 0 && errno == ENOENT) {
        LOG(ERROR) << "Couldn't access state file.";
        return CreateResult::NOT_CREATED;
    }
    if (!NeedSnapshotsInFirstStageMount()) {
        return CreateResult::NOT_CREATED;
    }
    auto slot_suffix = device_->GetOtherSlotSuffix();
    auto slot_number = SlotNumberForSlotSuffix(slot_suffix);
    auto super_path = device_->GetSuperDevice(slot_number);
    if (!CreateLogicalAndSnapshotPartitions(super_path, 20s)) {
        LOG(ERROR) << "Unable to map partitions.";
        return CreateResult::ERROR;
    }
    return CreateResult::CREATED;
}
bool SnapshotManager::UpdateForwardMergeIndicator(bool wipe) {
    auto path = GetForwardMergeIndicatorPath();
    if (!wipe) {
        LOG(INFO) << "Wipe is not scheduled. Deleting forward merge indicator.";
        return RemoveFileIfExists(path);
    }
    LOG(INFO) << "Wipe will be scheduled. Allowing forward merge of snapshots.";
    if (!android::base::WriteStringToFile("1", path)) {
        PLOG(ERROR) << "Unable to write forward merge indicator: " << path;
        return false;
    }
    return true;
}
ISnapshotMergeStats* SnapshotManager::GetSnapshotMergeStatsInstance() {
    return SnapshotMergeStats::GetInstance(*this);
}
bool SnapshotManager::GetMappedImageDevicePath(const std::string& device_name,
                                               std::string* device_path) {
    if (dm_.GetState(device_name) != DmDeviceState::INVALID) {
        return dm_.GetDmDevicePathByName(device_name, device_path);
    }
    return images_->GetMappedImageDevice(device_name, device_path);
}
bool SnapshotManager::GetMappedImageDeviceStringOrPath(const std::string& device_name,
                                                       std::string* device_string_or_mapped_path) {
    if (dm_.GetState(device_name) != DmDeviceState::INVALID) {
        return dm_.GetDeviceString(device_name, device_string_or_mapped_path);
    }
    if (!images_->GetMappedImageDevice(device_name, device_string_or_mapped_path)) {
        return false;
    }
    LOG(WARNING) << "Calling GetMappedImageDevice with local image manager; device "
                 << (device_string_or_mapped_path ? *device_string_or_mapped_path : "(nullptr)")
                 << "may not be available in first stage init! ";
    return true;
}
bool SnapshotManager::WaitForDevice(const std::string& device,
                                    std::chrono::milliseconds timeout_ms) {
    if (!android::base::StartsWith(device, "/")) {
        return true;
    }
    if (uevent_regen_callback_) {
        if (!uevent_regen_callback_(device)) {
            LOG(ERROR) << "Failed to find device after regenerating uevents: " << device;
            return false;
        }
        return true;
    }
    if (!android::base::StartsWith(device, "/dev/dm-user/")) {
        return true;
    }
    if (timeout_ms.count() == 0) {
        LOG(ERROR) << "No timeout was specified to wait for device: " << device;
        return false;
    }
    if (!android::fs_mgr::WaitForFile(device, timeout_ms)) {
        LOG(ERROR) << "Timed out waiting for device to appear: " << device;
        return false;
    }
    return true;
}
bool SnapshotManager::IsSnapuserdRequired() {
    auto lock = LockExclusive();
    if (!lock) return false;
    auto status = ReadSnapshotUpdateStatus(lock.get());
    return status.state() != UpdateState::None && status.using_snapuserd();
}
bool SnapshotManager::PrepareSnapuserdArgsForSelinux(std::vector<std::string>* snapuserd_argv) {
    return PerformInitTransition(InitTransition::SELINUX_DETACH, snapuserd_argv);
}
bool SnapshotManager::IsUserspaceSnapshotUpdateInProgress() {
    auto slot = GetCurrentSlot();
    if (slot == Slot::Target) {
        if (IsSnapuserdRequired()) {
            return true;
        }
    }
    return false;
}
bool SnapshotManager::IsUserspaceSnapshotUpdateInProgress() {
    auto slot = GetCurrentSlot();
    if (slot == Slot::Target) {
        if (IsSnapuserdRequired()) {
            return true;
        }
    }
    return false;
}
bool SnapshotManager::IsUserspaceSnapshotUpdateInProgress() {
    auto slot = GetCurrentSlot();
    if (slot == Slot::Target) {
        if (IsSnapuserdRequired()) {
            return true;
        }
    }
    return false;
}
bool SnapshotManager::IsUserspaceSnapshotUpdateInProgress() {
    auto slot = GetCurrentSlot();
    if (slot == Slot::Target) {
        if (IsSnapuserdRequired()) {
            return true;
        }
    }
    return false;
}
bool SnapshotManager::IsUserspaceSnapshotUpdateInProgress() {
    auto slot = GetCurrentSlot();
    if (slot == Slot::Target) {
        if (IsSnapuserdRequired()) {
            return true;
        }
    }
    return false;
}
bool SnapshotManager::IsUserspaceSnapshotUpdateInProgress() {
    auto slot = GetCurrentSlot();
    if (slot == Slot::Target) {
        if (IsSnapuserdRequired()) {
            return true;
        }
    }
    return false;
}
bool SnapshotManager::IsUserspaceSnapshotUpdateInProgress() {
    auto slot = GetCurrentSlot();
    if (slot == Slot::Target) {
        if (IsSnapuserdRequired()) {
            return true;
        }
    }
    return false;
}
}
}
