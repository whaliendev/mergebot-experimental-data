#include <libsnapshot/snapshot.h>
#include <dirent.h>
#include <math.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/unistd.h>
#include <optional>
#include <thread>
#include <unordered_set>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <ext4_utils/ext4_utils.h>
#include <fs_mgr.h>
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
#include "utility.h"
namespace android {
namespace snapshot {
using android::base::unique_fd;
using android::dm::DeviceMapper;
using android::dm::DmDeviceState;
using android::dm::DmTable;
using android::dm::DmTargetLinear;
using android::dm::DmTargetSnapshot;
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
using android::hardware::boot::V1_1::MergeStatus;
using chromeos_update_engine::DeltaArchiveManifest;
using chromeos_update_engine::Extent;
using chromeos_update_engine::InstallOperation;
using std::chrono::duration_cast;
using namespace std::chrono_literals;
using namespace std::string_literals;
static constexpr char kBootIndicatorPath[] = "/metadata/ota/snapshot-boot";
static constexpr char kRollbackIndicatorPath[] = "/metadata/ota/rollback-indicator";
static constexpr auto kUpdateStateCheckInterval = 2s;
SnapshotManager::~SnapshotManager() {}
std::unique_ptr<SnapshotManager> SnapshotManager::New(IDeviceInfo* info) {
    if (!info) {
        info = new DeviceInfo();
    }
    return std::unique_ptr<SnapshotManager>(new SnapshotManager(info));
}
std::unique_ptr<SnapshotManager> SnapshotManager::NewForFirstStageMount(IDeviceInfo* info) {
    auto sm = New(info);
    if (!sm || !sm->ForceLocalImageManager()) {
        return nullptr;
    }
    return sm;
}
SnapshotManager::SnapshotManager(IDeviceInfo* device) : device_(device) {
    gsid_dir_ = device_->GetGsidDir();
    metadata_dir_ = device_->GetMetadataDir();
}
static std::string GetCowName(const std::string& snapshot_name) {
    return snapshot_name + "-cow";
}
static std::string GetCowImageDeviceName(const std::string& snapshot_name) {
    return snapshot_name + "-cow-img";
}
static std::string GetBaseDeviceName(const std::string& partition_name) {
    return partition_name + "-base";
}
static std::string GetSnapshotExtraDeviceName(const std::string& snapshot_name) {
    return snapshot_name + "-inner";
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
    if (state == UpdateState::None) return true;
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
bool SnapshotManager::CreateSnapshot(LockedFile* lock, SnapshotStatus* status) {
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
    uint64_t snapshot_sectors = status.snapshot_size() / kSectorSize;
    uint64_t linear_sectors = (status.device_size() - status.snapshot_size()) / kSectorSize;
    auto& dm = DeviceMapper::Instance();
    SnapshotStorageMode mode;
    switch (ReadUpdateState(lock)) {
        case UpdateState::MergeCompleted:
        case UpdateState::MergeNeedsReboot:
            LOG(ERROR) << "Should not create a snapshot device for " << name
                       << " after global merging has completed.";
            return false;
        case UpdateState::Merging:
        case UpdateState::MergeFailed:
            mode = SnapshotStorageMode::Merge;
            break;
        default:
            mode = SnapshotStorageMode::Persistent;
            break;
    }
    auto snap_name = (linear_sectors > 0) ? GetSnapshotExtraDeviceName(name) : name;
    DmTable table;
    table.Emplace<DmTargetSnapshot>(0, snapshot_sectors, base_device, cow_device, mode,
                                    kSnapshotChunkSize);
    if (!dm.CreateDevice(snap_name, table, dev_path, timeout_ms)) {
        LOG(ERROR) << "Could not create snapshot device: " << snap_name;
        return false;
    }
    if (linear_sectors) {
        std::string snap_dev;
        if (!dm.GetDeviceString(snap_name, &snap_dev)) {
            LOG(ERROR) << "Cannot determine major/minor for: " << snap_name;
            return false;
        }
        DmTable table;
        table.Emplace<DmTargetLinear>(0, snapshot_sectors, snap_dev, 0);
        table.Emplace<DmTargetLinear>(snapshot_sectors, linear_sectors, base_device,
                                      snapshot_sectors);
        if (!dm.CreateDevice(name, table, dev_path, timeout_ms)) {
            LOG(ERROR) << "Could not create outer snapshot device: " << name;
            dm.DeleteDevice(snap_name);
            return false;
        }
    }
    return true;
}
std::optional<std::string> SnapshotManager::MapCowImage(
        const std::string& name, const std::chrono::milliseconds& timeout_ms) {
    if (!EnsureImageManager()) return std::nullopt;
    auto cow_image_name = GetCowImageDeviceName(name);
    bool ok;
    std::string cow_dev;
    if (has_local_image_manager_) {
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
bool SnapshotManager::UnmapSnapshot(LockedFile* lock, const std::string& name) {
    CHECK(lock);
    auto& dm = DeviceMapper::Instance();
    if (!dm.DeleteDeviceIfExists(name)) {
        LOG(ERROR) << "Could not delete snapshot device: " << name;
        return false;
    }
    auto snapshot_extra_device = GetSnapshotExtraDeviceName(name);
    if (!dm.DeleteDeviceIfExists(snapshot_extra_device)) {
        LOG(ERROR) << "Could not delete snapshot inner device: " << snapshot_extra_device;
        return false;
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
    auto& dm = DeviceMapper::Instance();
    for (const auto& snapshot : snapshots) {
        if (dm.GetState(snapshot) == DmDeviceState::INVALID) {
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
    DmTargetSnapshot::Status initial_target_values = {};
    for (const auto& snapshot : snapshots) {
        DmTargetSnapshot::Status current_status;
        if (!QuerySnapshotStatus(snapshot, nullptr, &current_status)) {
            return false;
        }
        initial_target_values.sectors_allocated += current_status.sectors_allocated;
        initial_target_values.total_sectors += current_status.total_sectors;
        initial_target_values.metadata_sectors += current_status.metadata_sectors;
    }
    SnapshotUpdateStatus initial_status;
    initial_status.set_state(UpdateState::Merging);
    initial_status.set_sectors_allocated(initial_target_values.sectors_allocated);
    initial_status.set_total_sectors(initial_target_values.total_sectors);
    initial_status.set_metadata_sectors(initial_target_values.metadata_sectors);
    if (!WriteSnapshotUpdateStatus(lock.get(), initial_status)) {
        return false;
    }
    bool rewrote_all = true;
    for (const auto& snapshot : snapshots) {
        if (!SwitchSnapshotToMerge(lock.get(), snapshot)) {
            LOG(ERROR) << "Failed to switch snapshot to a merge target: " << snapshot;
            rewrote_all = false;
        }
    }
    if (!rewrote_all) {
        WriteUpdateState(lock.get(), UpdateState::MergeFailed);
    }
    return true;
}
bool SnapshotManager::SwitchSnapshotToMerge(LockedFile* lock, const std::string& name) {
    SnapshotStatus status;
    if (!ReadSnapshotStatus(lock, name, &status)) {
        return false;
    }
    if (status.state() != SnapshotState::CREATED) {
        LOG(WARNING) << "Snapshot " << name
                     << " has unexpected state: " << SnapshotState_Name(status.state());
    }
    auto dm_name = GetSnapshotDeviceName(name, status);
    if (!RewriteSnapshotDeviceTable(dm_name)) {
        return false;
    }
    status.set_state(SnapshotState::MERGING);
    DmTargetSnapshot::Status dm_status;
    if (!QuerySnapshotStatus(dm_name, nullptr, &dm_status)) {
        LOG(ERROR) << "Could not query merge status for snapshot: " << dm_name;
    }
    status.set_sectors_allocated(dm_status.sectors_allocated);
    status.set_metadata_sectors(dm_status.metadata_sectors);
    if (!WriteSnapshotStatus(lock, status)) {
        LOG(ERROR) << "Could not update status file for snapshot: " << name;
    }
    return true;
}
bool SnapshotManager::RewriteSnapshotDeviceTable(const std::string& dm_name) {
    auto& dm = DeviceMapper::Instance();
    std::vector<DeviceMapper::TargetInfo> old_targets;
    if (!dm.GetTableInfo(dm_name, &old_targets)) {
        LOG(ERROR) << "Could not read snapshot device table: " << dm_name;
        return false;
    }
    if (old_targets.size() != 1 || DeviceMapper::GetTargetType(old_targets[0].spec) != "snapshot") {
        LOG(ERROR) << "Unexpected device-mapper table for snapshot: " << dm_name;
        return false;
    }
    std::string base_device, cow_device;
    if (!DmTargetSnapshot::GetDevicesFromParams(old_targets[0].data, &base_device, &cow_device)) {
        LOG(ERROR) << "Could not derive underlying devices for snapshot: " << dm_name;
        return false;
    }
    DmTable table;
    table.Emplace<DmTargetSnapshot>(0, old_targets[0].spec.length, base_device, cow_device,
                                    SnapshotStorageMode::Merge, kSnapshotChunkSize);
    if (!dm.LoadTableAndActivate(dm_name, table)) {
        LOG(ERROR) << "Could not swap device-mapper tables on snapshot device " << dm_name;
        return false;
    }
    LOG(INFO) << "Successfully switched snapshot device to a merge target: " << dm_name;
    return true;
}
enum class TableQuery {
Table,Status,};
static bool GetSingleTarget(const std::string& dm_name, TableQuery query,
                            DeviceMapper::TargetInfo* target) {
    auto& dm = DeviceMapper::Instance();
    if (dm.GetState(dm_name) == DmDeviceState::INVALID) {
        return false;
    }
    std::vector<DeviceMapper::TargetInfo> targets;
    bool result;
    if (query == TableQuery::Status) {
        result = dm.GetTableStatus(dm_name, &targets);
    } else {
        result = dm.GetTableInfo(dm_name, &targets);
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
    if (type != "snapshot" && type != "snapshot-merge") {
        return false;
    }
    if (target) {
        *target = std::move(snap_target);
    }
    return true;
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
    return true;
}
UpdateState SnapshotManager::ProcessUpdateState(const std::function<bool()>& callback,
                                                const std::function<bool()>& before_cancel) {
    while (true) {
        UpdateState state = CheckMergeState(before_cancel);
        if (state == UpdateState::MergeFailed) {
            AcknowledgeMergeFailure();
        }
        if (state != UpdateState::Merging) {
            return state;
        }
        if (callback && !callback()) {
            return state;
        }
        std::this_thread::sleep_for(kUpdateStateCheckInterval);
    }
}
UpdateState SnapshotManager::CheckMergeState(const std::function<bool()>& before_cancel) {
    auto lock = LockExclusive();
    if (!lock) {
        return UpdateState::MergeFailed;
    }
    UpdateState state = CheckMergeState(lock.get(), before_cancel);
    if (state == UpdateState::MergeCompleted) {
        AcknowledgeMergeSuccess(lock.get());
    } else if (state == UpdateState::Cancelled) {
        if (!RemoveAllUpdateState(lock.get(), before_cancel)) {
            return ReadSnapshotUpdateStatus(lock.get()).state();
        }
    }
    return state;
}
UpdateState SnapshotManager::CheckMergeState(LockedFile* lock,
                                             const std::function<bool()>& before_cancel) {
    UpdateState state = ReadUpdateState(lock);
    switch (state) {
        case UpdateState::None:
        case UpdateState::MergeCompleted:
            return state;
        case UpdateState::Merging:
        case UpdateState::MergeNeedsReboot:
        case UpdateState::MergeFailed:
            break;
        case UpdateState::Unverified:
            if (HandleCancelledUpdate(lock, before_cancel)) {
                return UpdateState::Cancelled;
            }
            return state;
        default:
            return state;
    }
    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock, &snapshots)) {
        return UpdateState::MergeFailed;
    }
    bool cancelled = false;
    bool failed = false;
    bool merging = false;
    bool needs_reboot = false;
    for (const auto& snapshot : snapshots) {
        UpdateState snapshot_state = CheckTargetMergeState(lock, snapshot);
        switch (snapshot_state) {
            case UpdateState::MergeFailed:
                failed = true;
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
            default:
                LOG(ERROR) << "Unknown merge status for \"" << snapshot << "\": "
                           << "\"" << snapshot_state << "\"";
                failed = true;
                break;
        }
    }
    if (merging) {
        return UpdateState::Merging;
    }
    if (failed) {
        return UpdateState::MergeFailed;
    }
    if (needs_reboot) {
        WriteUpdateState(lock, UpdateState::MergeNeedsReboot);
        return UpdateState::MergeNeedsReboot;
    }
    if (cancelled) {
        return UpdateState::Cancelled;
    }
    return UpdateState::MergeCompleted;
}
UpdateState SnapshotManager::CheckTargetMergeState(LockedFile* lock, const std::string& name) {
    SnapshotStatus snapshot_status;
    if (!ReadSnapshotStatus(lock, name, &snapshot_status)) {
        return UpdateState::MergeFailed;
    }
    std::string dm_name = GetSnapshotDeviceName(name, snapshot_status);
    std::unique_ptr<LpMetadata> current_metadata;
    if (!IsSnapshotDevice(dm_name)) {
        if (!current_metadata) {
            current_metadata = ReadCurrentMetadata();
        }
        if (!current_metadata ||
            GetMetadataPartitionState(*current_metadata, name) != MetadataPartitionState::Updated) {
            DeleteSnapshot(lock, name);
            return UpdateState::Cancelled;
        }
        if (snapshot_status.state() == SnapshotState::MERGE_COMPLETED) {
            OnSnapshotMergeComplete(lock, name, snapshot_status);
            return UpdateState::MergeCompleted;
        }
        LOG(ERROR) << "Expected snapshot or snapshot-merge for device: " << dm_name;
        return UpdateState::MergeFailed;
    }
    DCHECK((current_metadata = ReadCurrentMetadata()) &&
           GetMetadataPartitionState(*current_metadata, name) == MetadataPartitionState::Updated);
    std::string target_type;
    DmTargetSnapshot::Status status;
    if (!QuerySnapshotStatus(dm_name, &target_type, &status)) {
        return UpdateState::MergeFailed;
    }
    if (target_type != "snapshot-merge") {
        LOG(ERROR) << "Snapshot " << name << " has incorrect target type: " << target_type;
        return UpdateState::MergeFailed;
    }
    if (status.sectors_allocated != status.metadata_sectors) {
        if (snapshot_status.state() == SnapshotState::MERGE_COMPLETED) {
            LOG(ERROR) << "Snapshot " << name << " is merging after being marked merge-complete.";
            return UpdateState::MergeFailed;
        }
        return UpdateState::Merging;
    }
    snapshot_status.set_state(SnapshotState::MERGE_COMPLETED);
    if (!WriteSnapshotStatus(lock, snapshot_status)) {
        return UpdateState::MergeFailed;
    }
    if (!OnSnapshotMergeComplete(lock, name, snapshot_status)) {
        return UpdateState::MergeNeedsReboot;
    }
    return UpdateState::MergeCompleted;
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
void SnapshotManager::AcknowledgeMergeSuccess(LockedFile* lock) {
    RemoveAllUpdateState(lock);
}
void SnapshotManager::AcknowledgeMergeFailure() {
    LOG(ERROR) << "Merge could not be completed and will be marked as failed.";
    auto lock = LockExclusive();
    if (!lock) return;
    UpdateState state = ReadUpdateState(lock.get());
    if (state != UpdateState::Merging && state != UpdateState::MergeNeedsReboot) {
        return;
    }
    WriteUpdateState(lock.get(), UpdateState::MergeFailed);
}
bool SnapshotManager::OnSnapshotMergeComplete(LockedFile* lock, const std::string& name,
                                              const SnapshotStatus& status) {
    auto dm_name = GetSnapshotDeviceName(name, status);
    if (IsSnapshotDevice(dm_name)) {
        std::string target_type;
        DmTargetSnapshot::Status dm_status;
        if (!QuerySnapshotStatus(dm_name, &target_type, &dm_status)) {
            return false;
        }
        if (target_type != "snapshot-merge") {
            LOG(ERROR) << "Unexpected target type " << target_type
                       << " for snapshot device: " << dm_name;
            return false;
        }
        if (dm_status.sectors_allocated != dm_status.metadata_sectors) {
            LOG(ERROR) << "Merge is unexpectedly incomplete for device " << dm_name;
            return false;
        }
        if (!CollapseSnapshotDevice(name, status)) {
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
bool SnapshotManager::CollapseSnapshotDevice(const std::string& name,
                                             const SnapshotStatus& status) {
    auto& dm = DeviceMapper::Instance();
    auto dm_name = GetSnapshotDeviceName(name, status);
    DeviceMapper::TargetInfo target;
    if (!GetSingleTarget(dm_name, TableQuery::Table, &target)) {
        return false;
    }
    if (DeviceMapper::GetTargetType(target.spec) != "snapshot-merge") {
        LOG(ERROR) << "Snapshot device has invalid target type: " << dm_name;
        return false;
    }
    std::string base_device, cow_device;
    if (!DmTargetSnapshot::GetDevicesFromParams(target.data, &base_device, &cow_device)) {
        LOG(ERROR) << "Could not parse snapshot device " << dm_name
                   << " parameters: " << target.data;
        return false;
    }
    uint64_t snapshot_sectors = status.snapshot_size() / kSectorSize;
    if (snapshot_sectors * kSectorSize != status.snapshot_size()) {
        LOG(ERROR) << "Snapshot " << name
                   << " size is not sector aligned: " << status.snapshot_size();
        return false;
    }
    if (dm_name != name) {
        std::vector<DeviceMapper::TargetInfo> outer_table;
        if (!dm.GetTableInfo(name, &outer_table)) {
            LOG(ERROR) << "Could not validate outer snapshot table: " << name;
            return false;
        }
        if (outer_table.size() != 2) {
            LOG(ERROR) << "Expected 2 dm-linear targets for table " << name
                       << ", got: " << outer_table.size();
            return false;
        }
        for (const auto& target : outer_table) {
            auto target_type = DeviceMapper::GetTargetType(target.spec);
            if (target_type != "linear") {
                LOG(ERROR) << "Outer snapshot table may only contain linear targets, but " << name
                           << " has target: " << target_type;
                return false;
            }
        }
        if (outer_table[0].spec.length != snapshot_sectors) {
            LOG(ERROR) << "dm-snapshot " << name << " should have " << snapshot_sectors
                       << " sectors, got: " << outer_table[0].spec.length;
            return false;
        }
        uint64_t expected_device_sectors = status.device_size() / kSectorSize;
        uint64_t actual_device_sectors = outer_table[0].spec.length + outer_table[1].spec.length;
        if (expected_device_sectors != actual_device_sectors) {
            LOG(ERROR) << "Outer device " << name << " should have " << expected_device_sectors
                       << " sectors, got: " << actual_device_sectors;
            return false;
        }
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
    if (!dm.LoadTableAndActivate(name, table)) {
        return false;
    }
    if (dm_name != name && !dm.DeleteDeviceIfExists(dm_name)) {
        LOG(ERROR) << "Unable to delete snapshot device " << dm_name << ", COW cannot be "
                   << "reclaimed until after reboot.";
        return false;
    }
    auto base_name = GetBaseDeviceName(name);
    if (!dm.DeleteDeviceIfExists(base_name)) {
        LOG(ERROR) << "Unable to delete base device for snapshot: " << base_name;
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
        bool should_delete = ShouldDeleteSnapshot(lock, flashing_status, current_slot, name);
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
bool SnapshotManager::ShouldDeleteSnapshot(LockedFile* lock,
                                           const std::map<std::string, bool>& flashing_status,
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
    SnapshotStatus status;
    if (!ReadSnapshotStatus(lock, name, &status)) {
        LOG(WARNING) << "Unable to read snapshot status for " << name
                     << ", guessing snapshot device name";
        auto extra_name = GetSnapshotExtraDeviceName(name);
        return !IsSnapshotDevice(name) && !IsSnapshotDevice(extra_name);
    }
    auto dm_name = GetSnapshotDeviceName(name, status);
    return !IsSnapshotDevice(dm_name);
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
    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock.get(), &snapshots)) {
        LOG(ERROR) << "Could not list snapshots";
        return state;
    }
    DmTargetSnapshot::Status fake_snapshots_status = {};
    for (const auto& snapshot : snapshots) {
        DmTargetSnapshot::Status current_status;
        if (!QuerySnapshotStatus(snapshot, nullptr, &current_status)) continue;
        fake_snapshots_status.sectors_allocated += current_status.sectors_allocated;
        fake_snapshots_status.total_sectors += current_status.total_sectors;
        fake_snapshots_status.metadata_sectors += current_status.metadata_sectors;
    }
    *progress = DmTargetSnapshot::MergePercent(fake_snapshots_status,
                                               update_status.sectors_allocated());
    return state;
}
bool SnapshotManager::ListSnapshots(LockedFile* lock, std::vector<std::string>* snapshots) {
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
        snapshots->emplace_back(dp->d_name);
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
    const auto& opener = device_->GetPartitionOpener();
    uint32_t slot = SlotNumberForSlotSuffix(device_->GetSlotSuffix());
    auto metadata = android::fs_mgr::ReadMetadata(opener, super_device, slot);
    if (!metadata) {
        LOG(ERROR) << "Could not read dynamic partition metadata for device: " << super_device;
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
                .partition_opener = &opener,
                .timeout_ms = timeout_ms,
        };
        std::string ignore_path;
        if (!MapPartitionWithSnapshot(lock.get(), std::move(params), &ignore_path)) {
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
                                               std::string* path) {
    auto begin = std::chrono::steady_clock::now();
    CHECK(lock);
    path->clear();
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
    auto& dm = DeviceMapper::Instance();
    std::string base_path;
    if (!CreateLogicalPartition(params, &base_path)) {
        LOG(ERROR) << "Could not create logical partition " << params.GetPartitionName()
                   << " as device " << params.GetDeviceName();
        return false;
    }
    created_devices.EmplaceBack<AutoUnmapDevice>(&dm, params.GetDeviceName());
    if (!live_snapshot_status.has_value()) {
        *path = base_path;
        created_devices.Release();
        return true;
    }
    std::string base_device;
    if (!dm.GetDeviceString(params.GetDeviceName(), &base_device)) {
        LOG(ERROR) << "Could not determine major/minor for: " << params.GetDeviceName();
        return false;
    }
    auto remaining_time = GetRemainingTime(params.timeout_ms, begin);
    if (remaining_time.count() < 0) return false;
    std::string cow_name;
    CreateLogicalPartitionParams cow_params = params;
    cow_params.timeout_ms = remaining_time;
    if (!MapCowDevices(lock, cow_params, *live_snapshot_status, &created_devices, &cow_name)) {
        return false;
    }
    std::string cow_device;
    if (!dm.GetDeviceString(cow_name, &cow_device)) {
        LOG(ERROR) << "Could not determine major/minor for: " << cow_name;
        return false;
    }
    remaining_time = GetRemainingTime(params.timeout_ms, begin);
    if (remaining_time.count() < 0) return false;
    if (!MapSnapshot(lock, params.GetPartitionName(), base_device, cow_device, remaining_time,
                     path)) {
        LOG(ERROR) << "Could not map snapshot for partition: " << params.GetPartitionName();
        return false;
    }
    created_devices.Release();
    LOG(INFO) << "Mapped " << params.GetPartitionName() << " as snapshot device at " << *path;
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
    auto& dm = DeviceMapper::Instance();
    std::string base_name = GetBaseDeviceName(target_partition_name);
    if (!dm.DeleteDeviceIfExists(base_name)) {
        LOG(ERROR) << "Cannot delete base device: " << base_name;
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
    auto& dm = DeviceMapper::Instance();
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
        if (!dm.GetDeviceString(cow_image_name, &cow_image_device)) {
            LOG(ERROR) << "Cannot determine major/minor for: " << cow_image_name;
            return false;
        }
        auto cow_partition_sectors = snapshot_status.cow_partition_size() / kSectorSize;
        auto cow_image_sectors = snapshot_status.cow_file_size() / kSectorSize;
        table.Emplace<DmTargetLinear>(cow_partition_sectors, cow_image_sectors, cow_image_device,
                                      0);
    }
    std::string cow_path;
    if (!dm.CreateDevice(*cow_name, table, &cow_path, remaining_time)) {
        LOG(ERROR) << "Could not create COW device: " << *cow_name;
        return false;
    }
    created_devices->EmplaceBack<AutoUnmapDevice>(&dm, *cow_name);
    LOG(INFO) << "Mapped COW device for " << params.GetPartitionName() << " at " << cow_path;
    return true;
}
bool SnapshotManager::UnmapCowDevices(LockedFile* lock, const std::string& name) {
    CHECK(lock);
    if (!EnsureImageManager()) return false;
    auto& dm = DeviceMapper::Instance();
    auto cow_name = GetCowName(name);
    if (!dm.DeleteDeviceIfExists(cow_name)) {
        LOG(ERROR) << "Cannot unmap " << cow_name;
        return false;
    }
    std::string cow_image_name = GetCowImageDeviceName(name);
    if (!images_->UnmapImageIfExists(cow_image_name)) {
        LOG(ERROR) << "Cannot unmap image " << cow_image_name;
        return false;
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
    if (lock_flags != 0 && flock(fd, lock_flags) < 0) {
        PLOG(ERROR) << "Acquire flock failed: " << file;
        return nullptr;
    }
    int lock_mode = lock_flags & (LOCK_EX | LOCK_SH);
    return std::make_unique<LockedFile>(file, std::move(fd), lock_mode);
}
SnapshotManager::LockedFile::~LockedFile() {
    if (flock(fd_, LOCK_UN) < 0) {
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
bool SnapshotManager::WriteUpdateState(LockedFile* lock, UpdateState state) {
    SnapshotUpdateStatus status = {};
    status.set_state(state);
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
std::string SnapshotManager::GetSnapshotDeviceName(const std::string& snapshot_name,
                                                   const SnapshotStatus& status) {
    if (status.device_size() != status.snapshot_size()) {
        return GetSnapshotExtraDeviceName(snapshot_name);
    }
    return snapshot_name;
}
bool SnapshotManager::EnsureImageManager() {
    if (images_) return true;
    images_ = android::fiemap::IImageManager::Open(gsid_dir_, 15000ms);
    if (!images_) {
        LOG(ERROR) << "Could not open ImageManager";
        return false;
    }
    return true;
}
bool SnapshotManager::ForceLocalImageManager() {
    images_ = android::fiemap::ImageManager::Open(gsid_dir_);
    if (!images_) {
        LOG(ERROR) << "Could not open ImageManager";
        return false;
    }
    has_local_image_manager_ = true;
    return true;
}
static void UnmapAndDeleteCowPartition(MetadataBuilder* current_metadata) {
    auto& dm = DeviceMapper::Instance();
    std::vector<std::string> to_delete;
    for (auto* existing_cow_partition : current_metadata->ListPartitionsInGroup(kCowGroupName)) {
        if (!dm.DeleteDeviceIfExists(existing_cow_partition->name())) {
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
    return Return::NoSpace(sum);
}
Return SnapshotManager::CreateUpdateSnapshots(const DeltaArchiveManifest& manifest) {
    auto lock = LockExclusive();
    if (!lock) return Return::Error();
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
    PartitionCowCreator cow_creator{
            .target_metadata = target_metadata.get(),
            .target_suffix = target_suffix,
            .target_partition = nullptr,
            .current_metadata = current_metadata.get(),
            .current_suffix = current_suffix,
            .operations = nullptr,
            .extra_extents = {},
    };
    auto ret = CreateUpdateSnapshotsInternal(lock.get(), manifest, &cow_creator, &created_devices,
                                             &all_snapshot_status);
    if (!ret.is_ok()) return ret;
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
    std::map<std::string, const RepeatedPtrField<InstallOperation>*> install_operation_map;
    std::map<std::string, std::vector<Extent>> extra_extents_map;
    for (const auto& partition_update : manifest.partitions()) {
        auto suffixed_name = partition_update.partition_name() + target_suffix;
        auto&& [it, inserted] =
                install_operation_map.emplace(suffixed_name, &partition_update.operations());
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
        cow_creator->operations = nullptr;
        auto operations_it = install_operation_map.find(target_partition->name());
        if (operations_it != install_operation_map.end()) {
            cow_creator->operations = operations_it->second;
        }
        cow_creator->extra_extents.clear();
        auto extra_extents_it = extra_extents_map.find(target_partition->name());
        if (extra_extents_it != extra_extents_map.end()) {
            cow_creator->extra_extents = std::move(extra_extents_it->second);
        }
        auto cow_creator_ret = cow_creator->Run();
        if (!cow_creator_ret.has_value()) {
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
        if (!CreateSnapshot(lock, &cow_creator_ret->snapshot_status)) {
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
            if (!ret.is_ok()) return AddRequiredSpace(ret, *all_snapshot_status);
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
    auto& dm = DeviceMapper::Instance();
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
        if (!dm.GetDmDevicePathByName(cow_name, &cow_path)) {
            LOG(ERROR) << "Cannot determine path for " << cow_name;
            return Return::Error();
        }
        auto ret = InitializeCow(cow_path);
        if (!ret.is_ok()) {
            LOG(ERROR) << "Can't zero-fill COW device for " << target_partition->name() << ": "
                       << cow_path;
            return AddRequiredSpace(ret, all_snapshot_status);
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
    return MapPartitionWithSnapshot(lock.get(), params, snapshot_path);
}
bool SnapshotManager::UnmapUpdateSnapshot(const std::string& target_partition_name) {
    auto lock = LockShared();
    if (!lock) return false;
    return UnmapPartitionWithSnapshot(lock.get(), target_partition_name);
}
bool SnapshotManager::UnmapAllPartitions() {
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
    ss << "Update state: " << ReadUpdateState(file.get()) << std::endl;
    ss << "Current slot: " << device_->GetSlotSuffix() << std::endl;
    ss << "Boot indicator: booting from " << GetCurrentSlot() << " slot" << std::endl;
    ss << "Rollback indicator: "
       << (access(GetRollbackIndicatorPath().c_str(), F_OK) == 0 ? "exists" : strerror(errno))
       << std::endl;
    ss << "Forward merge indicator: "
       << (access(GetForwardMergeIndicatorPath().c_str(), F_OK) == 0 ? "exists" : strerror(errno))
       << std::endl;
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
    }
    os << ss.rdbuf();
    return ok;
}
std::unique_ptr<AutoDevice> SnapshotManager::EnsureMetadataMounted() {
    if (!device_->IsRecovery()) {
        LOG(INFO) << "EnsureMetadataMounted does nothing in Android mode.";
        return std::unique_ptr<AutoUnmountDevice>(new AutoUnmountDevice());
    }
    return AutoUnmountDevice::New(device_->GetMetadataDir());
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
    if (!CreateLogicalAndSnapshotPartitions(super_path)) {
        LOG(ERROR) << "Unable to map partitions to complete merge.";
        return false;
    }
    auto process_callback = [&]() -> bool {
        if (callback) {
            callback();
        }
        return true;
    };
    if (!ProcessUpdateStateOnDataWipe(true , process_callback)) {
        return false;
    }
    if (!UnmapAllPartitions()) {
        LOG(ERROR) << "Unable to unmap all partitions; fastboot may fail to flash.";
    }
    return true;
}
bool SnapshotManager::ProcessUpdateStateOnDataWipe(bool allow_forward_merge, const std::function<bool()>& callback) {
    auto slot_number = SlotNumberForSlotSuffix(device_->GetSlotSuffix());
    UpdateState state = ProcessUpdateState(callback);
    LOG(INFO) << "Update state in recovery: " << state;
    switch (state) {
        case UpdateState::MergeFailed:
            LOG(ERROR) << "Unrecoverable merge failure detected.";
            return false;
        case UpdateState::Unverified: {
            auto slot = GetCurrentSlot();
            if (slot == Slot::Target) {
                if (allow_forward_merge &&
                    access(GetForwardMergeIndicatorPath().c_str(), F_OK) == 0) {
                    LOG(INFO) << "Forward merge allowed, initiating merge now.";
                    return InitiateMerge() &&
                           ProcessUpdateStateOnDataWipe(false , callback);
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
    return true;
}
bool SnapshotManager::EnsureNoOverflowSnapshot(LockedFile* lock) {
    CHECK(lock);
    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock, &snapshots)) {
        LOG(ERROR) << "Could not list snapshots.";
        return false;
    }
    auto& dm = DeviceMapper::Instance();
    for (const auto& snapshot : snapshots) {
        std::vector<DeviceMapper::TargetInfo> targets;
        if (!dm.GetTableStatus(snapshot, &targets)) {
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
    if (!CreateLogicalAndSnapshotPartitions(super_path)) {
        LOG(ERROR) << "Unable to map partitions.";
        return CreateResult::ERROR;
    }
    return CreateResult::CREATED;
}
bool SnapshotManager::UpdateForwardMergeIndicator(bool wipe) {
    if (!wipe) {
        return RemoveFileIfExists(path);
    }
    LOG(INFO) << "Wipe will be scheduled. Allowing forward merge of snapshots.";
    if (!android::base::WriteStringToFile("1", path)) {
        PLOG(ERROR) << "Unable to write forward merge indicator: " << path;
        return false;
    }
    return true;
}
}
}
