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
    virtual coldboot_action_t ColdbootCallback(uevent* uevent);
    virtual bool GetRequiredDevices() = 0;
    virtual bool SetUpDmVerity(fstab_rec* fstab_rec) = 0;
    bool need_dm_verity_;
    std::unique_ptr<fstab, decltype(&fs_mgr_free_fstab)> device_tree_fstab_;
    std::vector<fstab_rec*> mount_fstab_recs_;
    std::set<std::string> required_devices_partition_names_;
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
    coldboot_action_t ColdbootCallback(uevent* uevent) override;
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
    for (auto mount_point : {"/system", "/vendor", "/odm"}) {
        fstab_rec* fstab_rec =
            fs_mgr_get_entry_for_mount_point(device_tree_fstab_.get(), mount_point);
        if (fstab_rec != nullptr) {
            mount_fstab_recs_.push_back(fstab_rec);
        }
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
void FirstStageMount::InitRequiredDevices() {
    if (required_devices_partition_names_.empty()) {
        return;
    }
    if (need_dm_verity_) {
        const std::string dm_path = "/devices/virtual/misc/device-mapper";
        device_init(("/sys" + dm_path).c_str(), [&dm_path](uevent* uevent) -> coldboot_action_t {
            if (uevent->path && uevent->path == dm_path) return COLDBOOT_STOP;
            return COLDBOOT_CONTINUE;
        });
    }
    device_init(nullptr,
                [this](uevent* uevent) -> coldboot_action_t { return ColdbootCallback(uevent); });
    device_close();
}
coldboot_action_t FirstStageMount::ColdbootCallback(uevent* uevent) {
    }
if (!uevent->partition_name.empty()) {
            }
        }
    }
    return COLDBOOT_CONTINUE;
}
void FirstStageMount::InitVerityDevice(const std::string& verity_device) {
    const std::string device_name(basename(verity_device.c_str()));
    const std::string syspath = "/sys/block/" + device_name;
    device_init(syspath.c_str(), [&](uevent* uevent) -> coldboot_action_t {
if (uevent->device_name == device_name) { LOG(VERBOSE) << "Creating dm-verity device : " << verity_device;
            LOG(VERBOSE) << "Creating dm-verity device : " << verity_device;
            return COLDBOOT_STOP;
        }
        return COLDBOOT_CONTINUE;
    });
    device_close();
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
coldboot_action_t FirstStageMountVBootV2::ColdbootCallback(uevent* uevent) {
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
