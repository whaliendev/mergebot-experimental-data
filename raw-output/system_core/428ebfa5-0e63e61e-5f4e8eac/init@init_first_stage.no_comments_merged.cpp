#include "init_first_stage.h"
#include <stdlib.h>
#include <unistd.h>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include "devices.h"
#include "fs_mgr.h"
#include "fs_mgr_avb.h"
#include "uevent.h"
#include "uevent_listener.h"
#include "util.h"
class FirstStageMount {
  public:
    FirstStageMount();
    virtual ~FirstStageMount() = default;
    static std::unique_ptr<FirstStageMount> Create();
    bool DoFirstStageMount();
    bool InitDevices();
  protected:
    void InitRequiredDevices();
    void InitVerityDevice(const std::string& verity_device);
    bool MountPartitions();
    virtual ListenerAction UeventCallback(const Uevent& uevent);
    virtual bool GetRequiredDevices() = 0;
    virtual bool SetUpDmVerity(fstab_rec* fstab_rec) = 0;
    bool need_dm_verity_;
    std::unique_ptr<fstab, decltype(&fs_mgr_free_fstab)> device_tree_fstab_;
    std::vector<fstab_rec*> mount_fstab_recs_;
    std::set<std::string> required_devices_partition_names_;
    DeviceHandler device_handler_;
    UeventListener uevent_listener_;
};
class FirstStageMountVBootV1 : public FirstStageMount {
  public:
    FirstStageMountVBootV1() = default;
    ~FirstStageMountVBootV1() override = default;
  protected:
    bool GetRequiredDevices() override;
    bool SetUpDmVerity(fstab_rec* fstab_rec) override;
};
class FirstStageMountVBootV2 : public FirstStageMount {
  public:
    friend void SetInitAvbVersionInRecovery();
    FirstStageMountVBootV2();
    ~FirstStageMountVBootV2() override = default;
  protected:
    ListenerAction UeventCallback(const Uevent& uevent) override;
    bool GetRequiredDevices() override;
    bool SetUpDmVerity(fstab_rec* fstab_rec) override;
    bool InitAvbHandle();
    std::string device_tree_vbmeta_parts_;
    FsManagerAvbUniquePtr avb_handle_;
    ByNameSymlinkMap by_name_symlink_map_;
};
static inline bool IsDtVbmetaCompatible() {
    return is_android_dt_value_expected("vbmeta/compatible", "android,vbmeta");
}
static bool inline IsRecoveryMode() {
    return access("/sbin/recovery", F_OK) == 0;
}
FirstStageMount::FirstStageMount()
    : need_dm_verity_(false), device_tree_fstab_(fs_mgr_read_fstab_dt(), fs_mgr_free_fstab) {
    if (!device_tree_fstab_) {
        LOG(ERROR) << "Failed to read fstab from device tree";
        return;
    }
    for (int i = 0; i < device_tree_fstab_->num_entries; i++) {
        mount_fstab_recs_.push_back(&device_tree_fstab_->recs[i]);
    }
}
std::unique_ptr<FirstStageMount> FirstStageMount::Create() {
    if (IsDtVbmetaCompatible()) {
        return std::make_unique<FirstStageMountVBootV2>();
    } else {
        return std::make_unique<FirstStageMountVBootV1>();
    }
}
bool FirstStageMount::DoFirstStageMount() {
    if (mount_fstab_recs_.empty()) return true;
    if (!InitDevices()) return false;
    if (!MountPartitions()) return false;
    return true;
}
<<<<<<< HEAD
bool FirstStageMount::InitDevices() {
    return GetRequiredDevices() && InitRequiredDevices();
}
||||||| 5f4e8eac8
bool FirstStageMount::InitDevices() { return GetRequiredDevices() && InitRequiredDevices(); }
=======
bool FirstStageMount::InitDevices() {
    if (!GetRequiredDevices()) return false;
    InitRequiredDevices();
    if (!required_devices_partition_names_.empty()) {
        LOG(ERROR) << __FUNCTION__ << "(): partition(s) not found: "
                   << android::base::Join(required_devices_partition_names_, ", ");
        return false;
    } else {
        return true;
    }
}
>>>>>>> 0e63e61e
void FirstStageMount::InitRequiredDevices() {
    if (required_devices_partition_names_.empty()) {
        return;
    }
    if (need_dm_verity_) {
        const std::string dm_path = "/devices/virtual/misc/device-mapper";
<<<<<<< HEAD
        bool found = false;
        auto dm_callback = [this, &dm_path, &found](const Uevent& uevent) {
            if (uevent.path == dm_path) {
                device_handler_.HandleDeviceEvent(uevent);
                found = true;
                return ListenerAction::kStop;
            }
            return ListenerAction::kContinue;
        };
        uevent_listener_.RegenerateUeventsForPath("/sys" + dm_path, dm_callback);
        if (!found) {
            uevent_listener_.Poll(dm_callback, 10s);
        }
        if (!found) {
            LOG(ERROR) << "device-mapper device not found";
            return false;
        }
||||||| 5f4e8eac8
        bool found = false;
        auto dm_callback = [&dm_path, &found](uevent* uevent) -> coldboot_action_t {
            if (uevent->path && uevent->path == dm_path) {
                found = true;
                return COLDBOOT_STOP;
            }
            return COLDBOOT_CONTINUE;
        };
        device_init(("/sys" + dm_path).c_str(), dm_callback);
        if (!found) {
            device_poll(dm_callback, 10s);
        }
        if (!found) {
            LOG(ERROR) << "device-mapper device not found";
            return false;
        }
=======
        device_init(("/sys" + dm_path).c_str(), [&dm_path](uevent* uevent) -> coldboot_action_t {
            if (uevent->path && uevent->path == dm_path) return COLDBOOT_STOP;
            return COLDBOOT_CONTINUE;
        });
>>>>>>> 0e63e61e
    }
<<<<<<< HEAD
    auto uevent_callback = [this](const Uevent& uevent) { return UeventCallback(uevent); };
    uevent_listener_.RegenerateUevents(uevent_callback);
    if (!required_devices_partition_names_.empty()) {
        uevent_listener_.Poll(uevent_callback, 10s);
    }
    if (!required_devices_partition_names_.empty()) {
        LOG(ERROR) << __PRETTY_FUNCTION__ << ": partition(s) not found: "
                   << android::base::Join(required_devices_partition_names_, ", ");
        return false;
    }
||||||| 5f4e8eac8
    auto uevent_callback = [this](uevent* uevent) -> coldboot_action_t {
        return ColdbootCallback(uevent);
    };
    device_init(nullptr, uevent_callback);
    if (!required_devices_partition_names_.empty()) {
        device_poll(uevent_callback, 10s);
    }
    if (!required_devices_partition_names_.empty()) {
        LOG(ERROR) << __PRETTY_FUNCTION__ << ": partition(s) not found: "
                   << android::base::Join(required_devices_partition_names_, ", ");
        return false;
    }
=======
    device_init(nullptr,
                [this](uevent* uevent) -> coldboot_action_t { return ColdbootCallback(uevent); });
>>>>>>> 0e63e61e
<<<<<<< HEAD
    return true;
||||||| 5f4e8eac8
    device_close();
    return true;
=======
    device_close();
>>>>>>> 0e63e61e
}
ListenerAction FirstStageMount::UeventCallback(const Uevent& uevent) {
    if (uevent.subsystem != "block") {
        return ListenerAction::kContinue;
    }
    if (!uevent.partition_name.empty()) {
        auto iter = required_devices_partition_names_.find(uevent.partition_name);
        if (iter != required_devices_partition_names_.end()) {
            LOG(VERBOSE) << __PRETTY_FUNCTION__ << ": found partition: " << *iter;
            required_devices_partition_names_.erase(iter);
            device_handler_.HandleDeviceEvent(uevent);
            if (required_devices_partition_names_.empty()) {
                return ListenerAction::kStop;
            } else {
                return ListenerAction::kContinue;
            }
        }
    }
    return ListenerAction::kContinue;
}
void FirstStageMount::InitVerityDevice(const std::string& verity_device) {
    const std::string device_name(basename(verity_device.c_str()));
    const std::string syspath = "/sys/block/" + device_name;
<<<<<<< HEAD
    auto verity_callback = [&device_name, &verity_device, this, &found](const Uevent& uevent) {
        if (uevent.device_name == device_name) {
||||||| 5f4e8eac8
    auto verity_callback = [&](uevent* uevent) -> coldboot_action_t {
        if (uevent->device_name && uevent->device_name == device_name) {
=======
    device_init(syspath.c_str(), [&](uevent* uevent) -> coldboot_action_t {
        if (uevent->device_name && uevent->device_name == device_name) {
>>>>>>> 0e63e61e
            LOG(VERBOSE) << "Creating dm-verity device : " << verity_device;
<<<<<<< HEAD
            device_handler_.HandleDeviceEvent(uevent);
            found = true;
            return ListenerAction::kStop;
||||||| 5f4e8eac8
            found = true;
            return COLDBOOT_STOP;
=======
            return COLDBOOT_STOP;
>>>>>>> 0e63e61e
        }
<<<<<<< HEAD
        return ListenerAction::kContinue;
    };
    uevent_listener_.RegenerateUeventsForPath(syspath, verity_callback);
    if (!found) {
        uevent_listener_.Poll(verity_callback, 10s);
    }
    if (!found) {
        LOG(ERROR) << "dm-verity device not found";
        return false;
    }
    return true;
||||||| 5f4e8eac8
        return COLDBOOT_CONTINUE;
    };
    device_init(syspath.c_str(), verity_callback);
    if (!found) {
        device_poll(verity_callback, 10s);
    }
    if (!found) {
        LOG(ERROR) << "dm-verity device not found";
        return false;
    }
    device_close();
    return true;
=======
        return COLDBOOT_CONTINUE;
    });
    device_close();
>>>>>>> 0e63e61e
}
bool FirstStageMount::MountPartitions() {
    for (auto fstab_rec : mount_fstab_recs_) {
        if (!SetUpDmVerity(fstab_rec)) {
            PLOG(ERROR) << "Failed to setup verity for '" << fstab_rec->mount_point << "'";
            return false;
        }
        if (fs_mgr_do_mount_one(fstab_rec)) {
            PLOG(ERROR) << "Failed to mount '" << fstab_rec->mount_point << "'";
            return false;
        }
    }
    return true;
}
bool FirstStageMountVBootV1::GetRequiredDevices() {
    std::string verity_loc_device;
    need_dm_verity_ = false;
    for (auto fstab_rec : mount_fstab_recs_) {
        if (fs_mgr_is_verifyatboot(fstab_rec)) {
            LOG(ERROR) << "Partitions can't be verified at boot";
            return false;
        }
        if (fs_mgr_is_verified(fstab_rec)) {
            need_dm_verity_ = true;
        }
        if (fstab_rec->verity_loc) {
            if (verity_loc_device.empty()) {
                verity_loc_device = fstab_rec->verity_loc;
            } else if (verity_loc_device != fstab_rec->verity_loc) {
                LOG(ERROR) << "More than one verity_loc found: " << verity_loc_device << ", "
                           << fstab_rec->verity_loc;
                return false;
            }
        }
    }
    for (auto fstab_rec : mount_fstab_recs_) {
        required_devices_partition_names_.emplace(basename(fstab_rec->blk_device));
    }
    if (!verity_loc_device.empty()) {
        required_devices_partition_names_.emplace(basename(verity_loc_device.c_str()));
    }
    return true;
}
bool FirstStageMountVBootV1::SetUpDmVerity(fstab_rec* fstab_rec) {
    if (fs_mgr_is_verified(fstab_rec)) {
        int ret = fs_mgr_setup_verity(fstab_rec, false );
        if (ret == FS_MGR_SETUP_VERITY_DISABLED) {
            LOG(INFO) << "Verity disabled for '" << fstab_rec->mount_point << "'";
        } else if (ret == FS_MGR_SETUP_VERITY_SUCCESS) {
            InitVerityDevice(fstab_rec->blk_device);
        } else {
            return false;
        }
    }
    return true;
}
FirstStageMountVBootV2::FirstStageMountVBootV2() : avb_handle_(nullptr) {
    if (!read_android_dt_file("vbmeta/parts", &device_tree_vbmeta_parts_)) {
        PLOG(ERROR) << "Failed to read vbmeta/parts from device tree";
        return;
    }
}
bool FirstStageMountVBootV2::GetRequiredDevices() {
    need_dm_verity_ = false;
    for (auto fstab_rec : mount_fstab_recs_) {
        if (fs_mgr_is_avb(fstab_rec)) {
            need_dm_verity_ = true;
        }
        required_devices_partition_names_.emplace(basename(fstab_rec->blk_device));
    }
    if (need_dm_verity_) {
        if (device_tree_vbmeta_parts_.empty()) {
            LOG(ERROR) << "Missing vbmeta parts in device tree";
            return false;
        }
        std::vector<std::string> partitions = android::base::Split(device_tree_vbmeta_parts_, ",");
        std::string ab_suffix = fs_mgr_get_slot_suffix();
        for (const auto& partition : partitions) {
            required_devices_partition_names_.emplace(partition + ab_suffix);
        }
    }
    return true;
}
ListenerAction FirstStageMountVBootV2::UeventCallback(const Uevent& uevent) {
    if (!uevent.partition_name.empty() &&
        required_devices_partition_names_.find(uevent.partition_name) !=
            required_devices_partition_names_.end()) {
        std::vector<std::string> links = device_handler_.GetBlockDeviceSymlinks(uevent);
        if (!links.empty()) {
            auto[it, inserted] = by_name_symlink_map_.emplace(uevent.partition_name, links[0]);
            if (!inserted) {
                LOG(ERROR) << "Partition '" << uevent.partition_name
                           << "' already existed in the by-name symlink map with a value of '"
                           << it->second << "', new value '" << links[0] << "' will be ignored.";
            }
        }
    }
    return FirstStageMount::UeventCallback(uevent);
}
bool FirstStageMountVBootV2::SetUpDmVerity(fstab_rec* fstab_rec) {
    if (fs_mgr_is_avb(fstab_rec)) {
        if (!InitAvbHandle()) return false;
        if (avb_handle_->hashtree_disabled()) {
            LOG(INFO) << "avb hashtree disabled for '" << fstab_rec->mount_point << "'";
        } else if (avb_handle_->SetUpAvb(fstab_rec, false )) {
            InitVerityDevice(fstab_rec->blk_device);
        } else {
            return false;
        }
    }
    return true;
}
bool FirstStageMountVBootV2::InitAvbHandle() {
    if (avb_handle_) return true;
    if (by_name_symlink_map_.empty()) {
        LOG(ERROR) << "by_name_symlink_map_ is empty";
        return false;
    }
    avb_handle_ = FsManagerAvbHandle::Open(std::move(by_name_symlink_map_));
    by_name_symlink_map_.clear();
    if (!avb_handle_) {
        PLOG(ERROR) << "Failed to open FsManagerAvbHandle";
        return false;
    }
    setenv("INIT_AVB_VERSION", avb_handle_->avb_version().c_str(), 1);
    return true;
}
bool DoFirstStageMount() {
    if (IsRecoveryMode()) {
        LOG(INFO) << "First stage mount skipped (recovery mode)";
        return true;
    }
    if (!is_android_dt_value_expected("fstab/compatible", "android,fstab")) {
        LOG(INFO) << "First stage mount skipped (missing/incompatible fstab in device tree)";
        return true;
    }
    std::unique_ptr<FirstStageMount> handle = FirstStageMount::Create();
    if (!handle) {
        LOG(ERROR) << "Failed to create FirstStageMount";
        return false;
    }
    return handle->DoFirstStageMount();
}
void SetInitAvbVersionInRecovery() {
    if (!IsRecoveryMode()) {
        LOG(INFO) << "Skipped setting INIT_AVB_VERSION (not in recovery mode)";
        return;
    }
    if (!IsDtVbmetaCompatible()) {
        LOG(INFO) << "Skipped setting INIT_AVB_VERSION (not vbmeta compatible)";
        return;
    }
    FirstStageMountVBootV2 avb_first_mount;
    if (!avb_first_mount.InitDevices()) {
        LOG(ERROR) << "Failed to init devices for INIT_AVB_VERSION";
        return;
    }
    FsManagerAvbUniquePtr avb_handle =
        FsManagerAvbHandle::Open(std::move(avb_first_mount.by_name_symlink_map_));
    if (!avb_handle) {
        PLOG(ERROR) << "Failed to open FsManagerAvbHandle for INIT_AVB_VERSION";
        return;
    }
    setenv("INIT_AVB_VERSION", avb_handle->avb_version().c_str(), 1);
}
