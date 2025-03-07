       
#include <functional>
#include <iostream>
#include <string>
#include <android-base/macros.h>
#include <fstab/fstab.h>
#include <libdm/dm.h>
#include <libfiemap/image_manager.h>
#include <liblp/builder.h>
#include <libsnapshot/snapshot.h>
#include <update_engine/update_metadata.pb.h>
#include <libsnapshot/auto_device.h>
#include <libsnapshot/snapshot.h>
namespace android {
namespace snapshot {
static constexpr uint32_t kSnapshotChunkSize = 8;
struct AutoDeviceList {
    ~AutoDeviceList();
    template <typename T, typename... Args>
    void EmplaceBack(Args&&... args) {
        devices_.emplace_back(std::make_unique<T>(std::forward<Args>(args)...));
    }
    void Release();
  private:
    std::vector<std::unique_ptr<AutoDevice>> devices_;
};
struct AutoUnmapDevice : AutoDevice {
    AutoUnmapDevice(android::dm::IDeviceMapper* dm, const std::string& name)
        : AutoDevice(name), dm_(dm) {}
    ~AutoUnmapDevice();
  private:
    DISALLOW_COPY_AND_ASSIGN(AutoUnmapDevice);
    android::dm::IDeviceMapper* dm_ = nullptr;
};
struct AutoUnmapImage : AutoDevice {
    AutoUnmapImage(android::fiemap::IImageManager* images, const std::string& name)
        : AutoDevice(name), images_(images) {}
    ~AutoUnmapImage();
  private:
    DISALLOW_COPY_AND_ASSIGN(AutoUnmapImage);
    android::fiemap::IImageManager* images_ = nullptr;
};
struct AutoDeleteSnapshot : AutoDevice {
    AutoDeleteSnapshot(SnapshotManager* manager, SnapshotManager::LockedFile* lock,
                       const std::string& name)
        : AutoDevice(name), manager_(manager), lock_(lock) {}
    ~AutoDeleteSnapshot();
  private:
    DISALLOW_COPY_AND_ASSIGN(AutoDeleteSnapshot);
    SnapshotManager* manager_ = nullptr;
    SnapshotManager::LockedFile* lock_ = nullptr;
};
struct AutoUnmountDevice : AutoDevice {
    AutoUnmountDevice() : AutoDevice("") {}
    static std::unique_ptr<AutoUnmountDevice> New(const std::string& path);
    ~AutoUnmountDevice();
  private:
    AutoUnmountDevice(const std::string& path, android::fs_mgr::Fstab&& fstab)
        : AutoDevice(path), fstab_(std::move(fstab)) {}
    android::fs_mgr::Fstab fstab_;
};
std::vector<android::fs_mgr::Partition*> ListPartitionsWithSuffix(
        android::fs_mgr::MetadataBuilder* builder, const std::string& suffix);
Return InitializeKernelCow(const std::string& device);
bool WriteStringToFileAtomic(const std::string& content, const std::string& path);
bool FsyncDirectory(const char* dirname);
struct Now {};
std::ostream& operator<<(std::ostream& os, const Now&);
void AppendExtent(google::protobuf::RepeatedPtrField<chromeos_update_engine::Extent>* extents,
                  uint64_t start_block, uint64_t num_blocks);
bool KernelSupportsCompressedSnapshots();
bool GetLegacyCompressionEnabledProperty();
bool GetUserspaceSnapshotsEnabledProperty();
bool GetIouringEnabledProperty();
bool GetXorCompressionEnabledProperty();
bool CanUseUserspaceSnapshots();
bool IsDmSnapshotTestingEnabled();
std::string GetOtherPartitionName(const std::string& name);
}
}
