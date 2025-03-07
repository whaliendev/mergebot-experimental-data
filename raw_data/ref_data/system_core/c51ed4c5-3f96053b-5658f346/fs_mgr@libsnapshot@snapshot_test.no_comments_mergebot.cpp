#include <libsnapshot/cow_format.h>
#include <libsnapshot/snapshot.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <chrono>
#include <deque>
#include <future>
#include <iostream>
#include <aidl/android/hardware/boot/MergeStatus.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <fs_mgr/file_wait.h>
#include <fs_mgr/roots.h>
#include <fs_mgr_dm_linear.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <libdm/dm.h>
#include <libfiemap/image_manager.h>
#include <liblp/builder.h>
#include <openssl/sha.h>
#include <storage_literals/storage_literals.h>
#include <android/snapshot/snapshot.pb.h>
#include <libsnapshot/test_helpers.h>
#include "partition_cow_creator.h"
#include "utility.h"
#include <libsnapshot/mock_device_info.h>
#include <libsnapshot/mock_snapshot.h>
#if defined(LIBSNAPSHOT_TEST_VAB_LEGACY)
#define DEFAULT_MODE "vab-legacy"
#elif defined(LIBSNAPSHOT_TEST_VABC_LEGACY)
#define DEFAULT_MODE "vabc-legacy"
#else
#define DEFAULT_MODE ""
#endif
namespace android {
namespace snapshot {
using android::base::unique_fd;
using android::dm::DeviceMapper;
using android::dm::DmDeviceState;
using android::dm::IDeviceMapper;
using android::fiemap::FiemapStatus;
using android::fiemap::IImageManager;
using android::fs_mgr::BlockDeviceInfo;
using android::fs_mgr::CreateLogicalPartitionParams;
using android::fs_mgr::DestroyLogicalPartition;
using android::fs_mgr::EnsurePathMounted;
using android::fs_mgr::EnsurePathUnmounted;
using android::fs_mgr::Extent;
using android::fs_mgr::Fstab;
using android::fs_mgr::GetPartitionGroupName;
using android::fs_mgr::GetPartitionName;
using android::fs_mgr::Interval;
using android::fs_mgr::MetadataBuilder;
using android::fs_mgr::SlotSuffixForSlotNumber;
using chromeos_update_engine::DeltaArchiveManifest;
using chromeos_update_engine::DynamicPartitionGroup;
using chromeos_update_engine::PartitionUpdate;
using namespace ::testing;
using namespace android::storage_literals;
using namespace std::chrono_literals;
using namespace std::string_literals;
std::unique_ptr<SnapshotManager> sm;
TestDeviceInfo* test_device = nullptr;
std::string fake_super;
void MountMetadata();
class SnapshotTest : public ::testing::Test {
 public:
  SnapshotTest() : dm_(DeviceMapper::Instance()) {}
  void Cleanup() {
    InitializeState();
    CleanupTestArtifacts();
  }
 protected:
  void SetUp() override {
    const testing::TestInfo* const test_info =
        testing::UnitTest::GetInstance()->current_test_info();
    test_name_ = test_info->test_suite_name() + "/"s + test_info->name();
    LOG(INFO) << "Starting test: " << test_name_;
    SKIP_IF_NON_VIRTUAL_AB();
    SetupProperties();
    if (!DeviceSupportsMode()) {
      GTEST_SKIP() << "Mode not supported on this device";
    }
    InitializeState();
    CleanupTestArtifacts();
    FormatFakeSuper();
    MountMetadata();
    ASSERT_TRUE(sm->BeginUpdate());
  }
  void SetupProperties() {
    std::unordered_map<std::string, std::string> properties;
    ASSERT_TRUE(android::base::SetProperty("snapuserd.test.dm.snapshots", "0"))
        << "Failed to disable property: virtual_ab.userspace.snapshots.enabled";
    ASSERT_TRUE(android::base::SetProperty(
        "snapuserd.test.io_uring.force_disable", "0"))
        << "Failed to set property: snapuserd.test.io_uring.disabled";
    if (FLAGS_force_mode == "vabc-legacy") {
      ASSERT_TRUE(
          android::base::SetProperty("snapuserd.test.dm.snapshots", "1"))
          << "Failed to disable property: "
             "virtual_ab.userspace.snapshots.enabled";
      properties["ro.virtual_ab.compression.enabled"] = "true";
      properties["ro.virtual_ab.userspace.snapshots.enabled"] = "false";
    } else if (FLAGS_force_mode == "vab-legacy") {
      properties["ro.virtual_ab.compression.enabled"] = "false";
      properties["ro.virtual_ab.userspace.snapshots.enabled"] = "false";
    }
    if (FLAGS_force_iouring_disable == "iouring_disabled") {
      ASSERT_TRUE(android::base::SetProperty(
          "snapuserd.test.io_uring.force_disable", "1"))
          << "Failed to set property: snapuserd.test.io_uring.disabled";
      properties["ro.virtual_ab.io_uring.enabled"] = "false";
    }
    auto fetcher = std::make_unique<SnapshotTestPropertyFetcher>(
        "_a", std::move(properties));
    IPropertyFetcher::OverrideForTesting(std::move(fetcher));
    if (GetLegacyCompressionEnabledProperty() || CanUseUserspaceSnapshots()) {
      if (FLAGS_force_mode.empty()) {
        snapuserd_required_ = KernelSupportsCompressedSnapshots();
      } else {
        snapuserd_required_ = true;
      }
    }
  }
  void TearDown() override {
    RETURN_IF_NON_VIRTUAL_AB();
    LOG(INFO) << "Tearing down SnapshotTest test: " << test_name_;
    lock_ = nullptr;
    CleanupTestArtifacts();
    SnapshotTestPropertyFetcher::TearDown();
    LOG(INFO) << "Teardown complete for test: " << test_name_;
  }
  bool DeviceSupportsMode() {
    if (FLAGS_force_mode.empty()) {
      return true;
    }
    if (snapuserd_required_ && !KernelSupportsCompressedSnapshots()) {
      return false;
    }
    return true;
  }
  bool ShouldSkipLegacyMerging() {
    if (!GetLegacyCompressionEnabledProperty() || !snapuserd_required_) {
      return false;
    }
    int api_level = android::base::GetIntProperty("ro.board.api_level", -1);
    if (api_level == -1) {
      api_level =
          android::base::GetIntProperty("ro.product.first_api_level", -1);
    }
    return api_level != __ANDROID_API_S__;
  }
  void InitializeState() {
    ASSERT_TRUE(sm->EnsureImageManager());
    image_manager_ = sm->image_manager();
    test_device->set_slot_suffix("_a");
    sm->set_use_first_stage_snapuserd(false);
  }
  void CleanupTestArtifacts() {
    lock_ = nullptr;
    if (!image_manager_) {
      return;
    }
    std::vector<std::string> snapshots = {"test-snapshot", "test_partition_a",
                                          "test_partition_b"};
    for (const auto& snapshot : snapshots) {
      CleanupSnapshotArtifacts(snapshot);
    }
    std::vector<std::string> partitions = {
        "base-device",
        "test_partition_b",
        "test_partition_b-base",
        "test_partition_b-cow",
    };
    for (const auto& partition : partitions) {
      DeleteDevice(partition);
    }
    if (sm->GetUpdateState() != UpdateState::None) {
      auto state_file = sm->GetStateFilePath();
      unlink(state_file.c_str());
    }
  }
  void CleanupSnapshotArtifacts(const std::string& snapshot) {
    bool is_dm_user = false;
    DeviceMapper::TargetInfo target;
    if (sm->IsSnapshotDevice(snapshot, &target)) {
      is_dm_user = (DeviceMapper::GetTargetType(target.spec) == "user");
    }
    if (is_dm_user) {
      ASSERT_TRUE(sm->EnsureSnapuserdConnected());
      ASSERT_TRUE(AcquireLock());
      auto local_lock = std::move(lock_);
      ASSERT_TRUE(sm->UnmapUserspaceSnapshotDevice(local_lock.get(), snapshot));
    }
    ASSERT_TRUE(DeleteSnapshotDevice(snapshot));
    DeleteBackingImage(image_manager_, snapshot + "-cow-img");
    auto status_file = sm->GetSnapshotStatusFilePath(snapshot);
    android::base::RemoveFileIfExists(status_file);
  }
  bool AcquireLock() {
    lock_ = sm->LockExclusive();
    return !!lock_;
  }
  virtual void TestBody() override {}
  void FormatFakeSuper() {
    BlockDeviceInfo super_device("super", kSuperSize, 0, 0, 4096);
    std::vector<BlockDeviceInfo> devices = {super_device};
    auto builder = MetadataBuilder::New(devices, "super", 65536, 2);
    ASSERT_NE(builder, nullptr);
    auto metadata = builder->Export();
    ASSERT_NE(metadata, nullptr);
    TestPartitionOpener opener(fake_super);
    ASSERT_TRUE(FlashPartitionTable(opener, fake_super, *metadata.get()));
  }
  bool CreatePartition(const std::string& name, uint64_t size,
                       std::string* path = nullptr,
                       const std::optional<std::string> group = {}) {
    TestPartitionOpener opener(fake_super);
    auto builder = MetadataBuilder::New(opener, "super", 0);
    if (!builder) return false;
    std::string partition_group = std::string(android::fs_mgr::kDefaultGroup);
    if (group) {
      partition_group = *group;
    }
    return CreatePartition(builder.get(), name, size, path, partition_group);
  }
  bool CreatePartition(MetadataBuilder* builder, const std::string& name,
                       uint64_t size, std::string* path,
                       const std::string& group) {
    auto partition = builder->AddPartition(name, group, 0);
    if (!partition) return false;
    if (!builder->ResizePartition(partition, size)) {
      return false;
    }
    auto metadata = builder->Export();
    if (!metadata) return false;
    TestPartitionOpener opener(fake_super);
    if (!UpdatePartitionTable(opener, "super", *metadata.get(), 0)) {
      return false;
    }
    if (!path) return true;
    CreateLogicalPartitionParams params = {
        .block_device = fake_super,
        .metadata = metadata.get(),
        .partition_name = name,
        .force_writable = true,
        .timeout_ms = 10s,
    };
    return CreateLogicalPartition(params, path);
  }
  AssertionResult MapUpdateSnapshot(const std::string& name,
                                    std::unique_ptr<ISnapshotWriter>* writer) {
    TestPartitionOpener opener(fake_super);
    CreateLogicalPartitionParams params{
        .block_device = fake_super,
        .metadata_slot = 1,
        .partition_name = name,
        .timeout_ms = 10s,
        .partition_opener = &opener,
    };
    auto old_partition = "/dev/block/mapper/" + GetOtherPartitionName(name);
    auto result = sm->OpenSnapshotWriter(params, {old_partition});
    if (!result) {
      return AssertionFailure() << "Cannot open snapshot for writing: " << name;
    }
    if (!result->Initialize()) {
      return AssertionFailure()
             << "Cannot initialize snapshot for writing: " << name;
    }
    if (writer) {
      *writer = std::move(result);
    }
    return AssertionSuccess();
  }
  AssertionResult MapUpdateSnapshot(const std::string& name,
                                    std::string* path) {
    TestPartitionOpener opener(fake_super);
    CreateLogicalPartitionParams params{
        .block_device = fake_super,
        .metadata_slot = 1,
        .partition_name = name,
        .timeout_ms = 10s,
        .partition_opener = &opener,
    };
    auto result = sm->MapUpdateSnapshot(params, path);
    if (!result) {
      return AssertionFailure() << "Cannot open snapshot for writing: " << name;
    }
    return AssertionSuccess();
  }
  AssertionResult DeleteSnapshotDevice(const std::string& snapshot) {
    AssertionResult res = AssertionSuccess();
    if (!(res = DeleteDevice(snapshot))) return res;
    if (!sm->UnmapDmUserDevice(snapshot + "-user-cow")) {
      return AssertionFailure()
             << "Cannot delete dm-user device for " << snapshot;
    }
    if (!(res = DeleteDevice(snapshot + "-inner"))) return res;
    if (!(res = DeleteDevice(snapshot + "-cow"))) return res;
    if (!image_manager_->UnmapImageIfExists(snapshot + "-cow-img")) {
      return AssertionFailure()
             << "Cannot unmap image " << snapshot << "-cow-img";
    }
    if (!(res = DeleteDevice(snapshot + "-base"))) return res;
    if (!(res = DeleteDevice(snapshot + "-src"))) return res;
    return AssertionSuccess();
  }
  AssertionResult DeleteDevice(const std::string& device) {
    if (!sm->DeleteDeviceIfExists(device, 1s)) {
      return AssertionFailure() << "Can't delete " << device;
    }
    return AssertionSuccess();
  }
  AssertionResult CreateCowImage(const std::string& name) {
    if (!sm->CreateCowImage(lock_.get(), name)) {
      return AssertionFailure() << "Cannot create COW image " << name;
    }
    std::string cow_device;
    auto map_res = MapCowImage(name, 10s, &cow_device);
    if (!map_res) {
      return map_res;
    }
    if (!InitializeKernelCow(cow_device)) {
      return AssertionFailure() << "Cannot zero fill " << cow_device;
    }
    if (!sm->UnmapCowImage(name)) {
      return AssertionFailure()
             << "Cannot unmap " << name << " after zero filling it";
    }
    return AssertionSuccess();
  }
  AssertionResult MapCowImage(const std::string& name,
                              const std::chrono::milliseconds& timeout_ms,
                              std::string* path) {
    auto cow_image_path = sm->MapCowImage(name, timeout_ms);
    if (!cow_image_path.has_value()) {
      return AssertionFailure() << "Cannot map cow image " << name;
    }
    *path = *cow_image_path;
    return AssertionSuccess();
  }
  AssertionResult PrepareOneSnapshot(
      uint64_t device_size,
      std::unique_ptr<ISnapshotWriter>* writer = nullptr) {
    lock_ = nullptr;
    DeltaArchiveManifest manifest;
    auto dynamic_partition_metadata =
        manifest.mutable_dynamic_partition_metadata();
    dynamic_partition_metadata->set_vabc_enabled(snapuserd_required_);
    dynamic_partition_metadata->set_cow_version(
        android::snapshot::kCowVersionMajor);
    if (snapuserd_required_) {
      dynamic_partition_metadata->set_vabc_compression_param(
          FLAGS_compression_method);
    }
    auto group = dynamic_partition_metadata->add_groups();
    group->set_name("group");
    group->set_size(device_size * 2);
    group->add_partition_names("test_partition");
    auto pu = manifest.add_partitions();
    pu->set_partition_name("test_partition");
    pu->set_estimate_cow_size(device_size);
    SetSize(pu, device_size);
    auto extent = pu->add_operations()->add_dst_extents();
    extent->set_start_block(0);
    if (device_size) {
      extent->set_num_blocks(device_size / manifest.block_size());
    }
    TestPartitionOpener opener(fake_super);
    auto builder = MetadataBuilder::New(opener, "super", 0);
    if (!builder) {
      return AssertionFailure() << "Failed to open MetadataBuilder";
    }
    builder->AddGroup("group_a", 16_GiB);
    builder->AddGroup("group_b", 16_GiB);
    if (!CreatePartition(builder.get(), "test_partition_a", device_size,
                         nullptr, "group_a")) {
      return AssertionFailure() << "Failed create test_partition_a";
    }
    if (!sm->CreateUpdateSnapshots(manifest)) {
      return AssertionFailure() << "Failed to create update snapshots";
    }
    if (writer) {
      auto res = MapUpdateSnapshot("test_partition_b", writer);
      if (!res) {
        return res;
      }
    } else if (!snapuserd_required_) {
      std::string ignore;
      if (!MapUpdateSnapshot("test_partition_b", &ignore)) {
        return AssertionFailure() << "Failed to map test_partition_b";
      }
    }
    if (!AcquireLock()) {
      return AssertionFailure() << "Failed to acquire lock";
    }
    return AssertionSuccess();
  }
  AssertionResult SimulateReboot() {
    lock_ = nullptr;
    if (!sm->FinishedSnapshotWrites(false)) {
      return AssertionFailure() << "Failed to finish snapshot writes";
    }
    if (!sm->UnmapUpdateSnapshot("test_partition_b")) {
      return AssertionFailure() << "Failed to unmap COW for test_partition_b";
    }
    if (!dm_.DeleteDeviceIfExists("test_partition_b")) {
      return AssertionFailure() << "Failed to delete test_partition_b";
    }
    if (!dm_.DeleteDeviceIfExists("test_partition_b-base")) {
      return AssertionFailure() << "Failed to destroy test_partition_b-base";
    }
    return AssertionSuccess();
  }
  std::unique_ptr<SnapshotManager> NewManagerForFirstStageMount(
      const std::string& slot_suffix = "_a") {
    auto info = new TestDeviceInfo(fake_super, slot_suffix);
    return NewManagerForFirstStageMount(info);
  }
  std::unique_ptr<SnapshotManager> NewManagerForFirstStageMount(
      TestDeviceInfo* info) {
    info->set_first_stage_init(true);
    auto init = SnapshotManager::NewForFirstStageMount(info);
    if (!init) {
      return nullptr;
    }
    init->SetUeventRegenCallback([](const std::string& device) -> bool {
      return android::fs_mgr::WaitForFile(device, snapshot_timeout_);
    });
    return init;
  }
  static constexpr std::chrono::milliseconds snapshot_timeout_ = 5s;
  DeviceMapper& dm_;
  std::unique_ptr<SnapshotManager::LockedFile> lock_;
  android::fiemap::IImageManager* image_manager_ = nullptr;
  std::string fake_super_;
  bool snapuserd_required_ = false;
  std::string test_name_;
};
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
std::pair<uint64_t, uint64_t> GetBigFileLimit() {
  struct statvfs fs;
  if (statvfs("/data", &fs) < 0) {
    PLOG(ERROR) << "statfs failed";
    return {0, 0};
  }
  auto fs_limit = static_cast<uint64_t>(fs.f_blocks) * (fs.f_bsize - 1);
  auto fs_free = static_cast<uint64_t>(fs.f_bfree) * fs.f_bsize;
  LOG(INFO) << "Big file limit: " << fs_limit << ", free space: " << fs_free;
  return {fs_limit, fs_free};
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
enum class Request { UNKNOWN, LOCK_SHARED, LOCK_EXCLUSIVE, UNLOCK, EXIT };
std::ostream& operator<<(std::ostream& os, Request request) {
  switch (request) {
    case Request::LOCK_SHARED:
      return os << "Shared";
    case Request::LOCK_EXCLUSIVE:
      return os << "Exclusive";
    case Request::UNLOCK:
      return os << "Unlock";
    case Request::EXIT:
      return os << "Exit";
    case Request::UNKNOWN:
      [[fallthrough]];
    default:
      return os << "Unknown";
  }
}
class LockTestConsumer {
 public:
  AssertionResult MakeRequest(Request new_request) {
    {
      std::unique_lock<std::mutex> ulock(mutex_);
      requests_.push_back(new_request);
    }
    cv_.notify_all();
    return AssertionSuccess() << "Request " << new_request << " successful";
  }
  template <typename R, typename P>
  AssertionResult WaitFulfill(std::chrono::duration<R, P> timeout) {
    std::unique_lock<std::mutex> ulock(mutex_);
    if (cv_.wait_for(ulock, timeout, [this] { return requests_.empty(); })) {
      return AssertionSuccess() << "All requests_ fulfilled.";
    }
    return AssertionFailure()
           << "Timeout waiting for fulfilling " << requests_.size()
           << " request(s), first one is "
           << (requests_.empty() ? Request::UNKNOWN : requests_.front());
  }
  void StartHandleRequestsInBackground() {
    future_ =
        std::async(std::launch::async, &LockTestConsumer::HandleRequests, this);
  }
 private:
  void HandleRequests() {
    static constexpr auto consumer_timeout = 3s;
    auto next_request = Request::UNKNOWN;
    do {
      {
        std::unique_lock<std::mutex> ulock(mutex_);
        if (cv_.wait_for(ulock, consumer_timeout,
                         [this] { return !requests_.empty(); })) {
          next_request = requests_.front();
        } else {
          next_request = Request::EXIT;
        }
      }
      switch (next_request) {
        case Request::LOCK_SHARED: {
          lock_ = sm->LockShared();
        } break;
        case Request::LOCK_EXCLUSIVE: {
          lock_ = sm->LockExclusive();
        } break;
        case Request::EXIT:
          [[fallthrough]];
        case Request::UNLOCK: {
          lock_.reset();
        } break;
        case Request::UNKNOWN:
          [[fallthrough]];
        default:
          break;
      }
      {
        std::unique_lock<std::mutex> ulock(mutex_);
        if (next_request == Request::EXIT) {
          requests_.clear();
        } else {
          requests_.pop_front();
        }
      }
      cv_.notify_all();
    } while (next_request != Request::EXIT);
  }
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Request> requests_;
  std::unique_ptr<SnapshotManager::LockedFile> lock_;
  std::future<void> future_;
};
class LockTest : public ::testing::Test {
 public:
  void SetUp() {
    SKIP_IF_NON_VIRTUAL_AB();
    first_consumer.StartHandleRequestsInBackground();
    second_consumer.StartHandleRequestsInBackground();
  }
  void TearDown() {
    RETURN_IF_NON_VIRTUAL_AB();
    EXPECT_TRUE(first_consumer.MakeRequest(Request::EXIT));
    EXPECT_TRUE(second_consumer.MakeRequest(Request::EXIT));
  }
  static constexpr auto request_timeout = 500ms;
  LockTestConsumer first_consumer;
  LockTestConsumer second_consumer;
};
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
using LockTestParam = std::pair<Request, Request>;
class LockTestP : public LockTest,
                  public ::testing::WithParamInterface<LockTestParam> {};
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
class SnapshotUpdateTest : public SnapshotTest {
 public:
  void SetUp() override {
    SKIP_IF_NON_VIRTUAL_AB();
    SnapshotTest::SetUp();
    if (!image_manager_) {
      return;
    }
    Cleanup();
    test_device->set_slot_suffix("_a");
    opener_ = std::make_unique<TestPartitionOpener>(fake_super);
    auto dynamic_partition_metadata =
        manifest_.mutable_dynamic_partition_metadata();
    dynamic_partition_metadata->set_vabc_enabled(snapuserd_required_);
    dynamic_partition_metadata->set_cow_version(
        android::snapshot::kCowVersionMajor);
    if (snapuserd_required_) {
      dynamic_partition_metadata->set_vabc_compression_param(
          FLAGS_compression_method);
    }
    group_ = dynamic_partition_metadata->add_groups();
    group_->set_name("group");
    group_->set_size(kGroupSize);
    group_->add_partition_names("sys");
    group_->add_partition_names("vnd");
    group_->add_partition_names("prd");
    sys_ = manifest_.add_partitions();
    sys_->set_partition_name("sys");
    sys_->set_estimate_cow_size(2_MiB);
    SetSize(sys_, 3_MiB);
    vnd_ = manifest_.add_partitions();
    vnd_->set_partition_name("vnd");
    vnd_->set_estimate_cow_size(2_MiB);
    SetSize(vnd_, 3_MiB);
    prd_ = manifest_.add_partitions();
    prd_->set_partition_name("prd");
    prd_->set_estimate_cow_size(2_MiB);
    SetSize(prd_, 3_MiB);
    src_ = MetadataBuilder::New(*opener_, "super", 0);
    ASSERT_NE(src_, nullptr);
    ASSERT_TRUE(FillFakeMetadata(src_.get(), manifest_, "_a"));
    ASSERT_TRUE(src_->AddGroup("group_b", kGroupSize));
    auto partition = src_->AddPartition("sys_b", "group_b", 0);
    ASSERT_NE(nullptr, partition);
    ASSERT_TRUE(src_->ResizePartition(partition, 1_MiB));
    auto metadata = src_->Export();
    ASSERT_NE(nullptr, metadata);
    ASSERT_TRUE(UpdatePartitionTable(*opener_, "super", *metadata.get(), 0));
    std::string path;
    for (const auto& name : {"sys_a", "vnd_a", "prd_a"}) {
      ASSERT_TRUE(CreateLogicalPartition(
          CreateLogicalPartitionParams{
              .block_device = fake_super,
              .metadata_slot = 0,
              .partition_name = name,
              .timeout_ms = 1s,
              .partition_opener = opener_.get(),
          },
          &path));
      ASSERT_TRUE(WriteRandomData(path));
      auto hash = GetHash(path);
      ASSERT_TRUE(hash.has_value());
      hashes_[name] = *hash;
    }
    for (const auto& name : {"sys_b", "vnd_b", "prd_b"}) {
      ASSERT_TRUE(sm->UnmapUpdateSnapshot(name));
    }
  }
  void TearDown() override {
    RETURN_IF_NON_VIRTUAL_AB();
    LOG(INFO) << "Tearing down SnapshotUpdateTest test: " << test_name_;
    Cleanup();
    SnapshotTest::TearDown();
  }
  void Cleanup() {
    if (!image_manager_) {
      InitializeState();
    }
    MountMetadata();
    for (const auto& suffix : {"_a", "_b"}) {
      test_device->set_slot_suffix(suffix);
      if (sm->ProcessUpdateState() == UpdateState::MergeFailed) {
        ASSERT_TRUE(AcquireLock());
        ASSERT_TRUE(sm->WriteUpdateState(lock_.get(), UpdateState::None));
        lock_ = {};
      }
      EXPECT_TRUE(sm->CancelUpdate()) << suffix;
    }
    EXPECT_TRUE(UnmapAll());
  }
  AssertionResult IsPartitionUnchanged(const std::string& name) {
    std::string path;
    if (!dm_.GetDmDevicePathByName(name, &path)) {
      return AssertionFailure()
             << "Path of " << name << " cannot be determined";
    }
    auto hash = GetHash(path);
    if (!hash.has_value()) {
      return AssertionFailure()
             << "Cannot read partition " << name << ": " << path;
    }
    auto it = hashes_.find(name);
    if (it == hashes_.end()) {
      return AssertionFailure()
             << "No existing hash for " << name << ". Bad test code?";
    }
    if (it->second != *hash) {
      return AssertionFailure() << "Content of " << name << " has changed";
    }
    return AssertionSuccess();
  }
  std::optional<uint64_t> GetSnapshotSize(const std::string& name) {
    if (!AcquireLock()) {
      return std::nullopt;
    }
    auto local_lock = std::move(lock_);
    SnapshotStatus status;
    if (!sm->ReadSnapshotStatus(local_lock.get(), name, &status)) {
      return std::nullopt;
    }
    return status.snapshot_size();
  }
  AssertionResult UnmapAll() {
    for (const auto& name : {"sys", "vnd", "prd", "dlkm"}) {
      if (!dm_.DeleteDeviceIfExists(name + "_a"s)) {
        return AssertionFailure() << "Cannot unmap " << name << "_a";
      }
      if (!DeleteSnapshotDevice(name + "_b"s)) {
        return AssertionFailure() << "Cannot delete snapshot " << name << "_b";
      }
    }
    return AssertionSuccess();
  }
  AssertionResult MapOneUpdateSnapshot(const std::string& name) {
    if (snapuserd_required_) {
      std::unique_ptr<ISnapshotWriter> writer;
      return MapUpdateSnapshot(name, &writer);
    } else {
      std::string path;
      return MapUpdateSnapshot(name, &path);
    }
  }
  AssertionResult WriteSnapshots() {
    for (const auto& partition : {sys_, vnd_, prd_}) {
      auto res = WriteSnapshotAndHash(partition);
      if (!res) {
        return res;
      }
    }
    return AssertionSuccess();
  }
  AssertionResult WriteSnapshotAndHash(PartitionUpdate* partition) {
    std::string name = partition->partition_name() + "_b";
    if (snapuserd_required_) {
      std::unique_ptr<ISnapshotWriter> writer;
      auto res = MapUpdateSnapshot(name, &writer);
      if (!res) {
        return res;
      }
      if (!WriteRandomSnapshotData(writer.get(), &hashes_[name])) {
        return AssertionFailure()
               << "Unable to write random data to snapshot " << name;
      }
      if (!writer->Finalize()) {
        return AssertionFailure() << "Unable to finalize COW for " << name;
      }
    } else {
      std::string path;
      auto res = MapUpdateSnapshot(name, &path);
      if (!res) {
        return res;
      }
      if (!WriteRandomData(path, std::nullopt, &hashes_[name])) {
        return AssertionFailure()
               << "Unable to write random data to snapshot " << name;
      }
    }
    sync();
    return AssertionSuccess() << "Written random data to snapshot " << name
                              << ", hash: " << hashes_[name];
  }
  bool WriteRandomSnapshotData(ICowWriter* writer, std::string* hash) {
    unique_fd rand(open("/dev/urandom", O_RDONLY));
    if (rand < 0) {
      PLOG(ERROR) << "open /dev/urandom";
      return false;
    }
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    if (!writer->options().max_blocks) {
      LOG(ERROR) << "CowWriter must specify maximum number of blocks";
      return false;
    }
    const auto num_blocks = writer->options().max_blocks.value();
    const auto block_size = writer->options().block_size;
    std::string block(block_size, '\0');
    for (uint64_t i = 0; i < num_blocks; i++) {
      if (!ReadFully(rand, block.data(), block.size())) {
        PLOG(ERROR) << "read /dev/urandom";
        return false;
      }
      if (!writer->AddRawBlocks(i, block.data(), block.size())) {
        LOG(ERROR) << "Failed to add raw block " << i;
        return false;
      }
      SHA256_Update(&ctx, block.data(), block.size());
    }
    uint8_t out[32];
    SHA256_Final(out, &ctx);
    *hash = ToHexString(out, sizeof(out));
    return true;
  }
  AssertionResult ShiftAllSnapshotBlocks(const std::string& name,
                                         uint64_t old_size) {
    std::unique_ptr<ISnapshotWriter> writer;
    if (auto res = MapUpdateSnapshot(name, &writer); !res) {
      return res;
    }
    if (!writer->options().max_blocks || !*writer->options().max_blocks) {
      return AssertionFailure()
             << "No max blocks set for " << name << " writer";
    }
    uint64_t src_block = (old_size / writer->options().block_size) - 1;
    uint64_t dst_block = 0;
    uint64_t max_blocks = *writer->options().max_blocks;
    while (dst_block < max_blocks && dst_block < src_block) {
      if (!writer->AddCopy(dst_block, src_block)) {
        return AssertionFailure()
               << "Unable to add copy for " << name << " for blocks "
               << src_block << ", " << dst_block;
      }
      dst_block++;
      src_block--;
    }
    if (!writer->Finalize()) {
      return AssertionFailure() << "Unable to finalize writer for " << name;
    }
    auto hash = HashSnapshot(writer.get());
    if (hash.empty()) {
      return AssertionFailure()
             << "Unable to hash snapshot writer for " << name;
    }
    hashes_[name] = hash;
    return AssertionSuccess();
  }
  AssertionResult MapUpdateSnapshots(const std::vector<std::string>& names = {
                                         "sys_b", "vnd_b", "prd_b"}) {
    for (const auto& name : names) {
      auto res = MapOneUpdateSnapshot(name);
      if (!res) {
        return res;
      }
    }
    return AssertionSuccess();
  }
  void AddOperation(PartitionUpdate* partition_update,
                    uint64_t size_bytes = 0) {
    auto e = partition_update->add_operations()->add_dst_extents();
    e->set_start_block(0);
    if (size_bytes == 0) {
      size_bytes = GetSize(partition_update);
    }
    e->set_num_blocks(size_bytes / manifest_.block_size());
  }
  void AddOperationForPartitions(
      std::vector<PartitionUpdate*> partitions = {}) {
    if (partitions.empty()) {
      partitions = {sys_, vnd_, prd_};
    }
    for (auto* partition : partitions) {
      AddOperation(partition);
    }
  }
  std::unique_ptr<TestPartitionOpener> opener_;
  DeltaArchiveManifest manifest_;
  std::unique_ptr<MetadataBuilder> src_;
  std::map<std::string, std::string> hashes_;
  PartitionUpdate* sys_ = nullptr;
  PartitionUpdate* vnd_ = nullptr;
  PartitionUpdate* prd_ = nullptr;
  DynamicPartitionGroup* group_ = nullptr;
};
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
static std::vector<Interval> ToIntervals(
    const std::vector<std::unique_ptr<Extent>>& extents) {
  std::vector<Interval> ret;
  std::transform(extents.begin(), extents.end(), std::back_inserter(ret),
                 [](const auto& extent) {
                   return extent->AsLinearExtent()->AsInterval();
                 });
  return ret;
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
class MetadataMountedTest : public ::testing::Test {
 public:
  virtual void TestBody() override {}
  void SetUp() override {
    SKIP_IF_NON_VIRTUAL_AB();
    metadata_dir_ = test_device->GetMetadataDir();
    ASSERT_TRUE(ReadDefaultFstab(&fstab_));
  }
  void TearDown() override {
    RETURN_IF_NON_VIRTUAL_AB();
    SetUp();
    test_device->set_recovery(false);
    EXPECT_TRUE(android::fs_mgr::EnsurePathMounted(&fstab_, metadata_dir_));
  }
  AssertionResult IsMetadataMounted() {
    Fstab mounted_fstab;
    if (!ReadFstabFromFile("/proc/mounts", &mounted_fstab)) {
      ADD_FAILURE() << "Failed to scan mounted volumes";
      return AssertionFailure() << "Failed to scan mounted volumes";
    }
    auto entry = GetEntryForPath(&fstab_, metadata_dir_);
    if (entry == nullptr) {
      return AssertionFailure()
             << "No mount point found in fstab for path " << metadata_dir_;
    }
    auto mv = GetEntryForMountPoint(&mounted_fstab, entry->mount_point);
    if (mv == nullptr) {
      return AssertionFailure() << metadata_dir_ << " is not mounted";
    }
    return AssertionSuccess() << metadata_dir_ << " is mounted";
  }
  std::string metadata_dir_;
  Fstab fstab_;
};
void MountMetadata() { MetadataMountedTest().TearDown(); }
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
class AutoKill final {
 public:
  explicit AutoKill(pid_t pid) : pid_(pid) {}
  ~AutoKill() {
    if (pid_ > 0) kill(pid_, SIGKILL);
  }
  bool valid() const { return pid_ > 0; }
 private:
  pid_t pid_;
};
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
class FlashAfterUpdateTest
    : public SnapshotUpdateTest,
      public WithParamInterface<std::tuple<uint32_t, bool>> {
 public:
  AssertionResult InitiateMerge(const std::string& slot_suffix) {
    auto sm = SnapshotManager::New(new TestDeviceInfo(fake_super, slot_suffix));
    if (!sm->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_)) {
      return AssertionFailure() << "Cannot CreateLogicalAndSnapshotPartitions";
    }
    if (!sm->InitiateMerge()) {
      return AssertionFailure() << "Cannot initiate merge";
    }
    return AssertionSuccess();
  }
};
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
class ImageManagerTest : public SnapshotTest {
 protected:
  void SetUp() override {
    SKIP_IF_NON_VIRTUAL_AB();
    SnapshotTest::SetUp();
  }
  void TearDown() override {
    RETURN_IF_NON_VIRTUAL_AB();
    CleanUp();
    SnapshotTest::TearDown();
  }
  void CleanUp() {
    if (!image_manager_) {
      return;
    }
    EXPECT_TRUE(!image_manager_->BackingImageExists(kImageName) ||
                image_manager_->DeleteBackingImage(kImageName));
  }
  static constexpr const char* kImageName = "my_image";
};
TEST_P(ImageManagerTest, CreateImageNoSpace) {
  uint64_t to_allocate = userdata_->free_space() + userdata_->bsize();
  auto res = image_manager_->CreateBackingImage(
      kImageName, to_allocate, IImageManager::CREATE_IMAGE_DEFAULT);
  ASSERT_FALSE(res) << "Should not be able to create image with size = "
                    << to_allocate << " bytes because only "
                    << userdata_->free_space() << " bytes are free";
  ASSERT_EQ(FiemapStatus::ErrorCode::NO_SPACE, res.error_code())
      << res.string();
}
bool Mkdir(const std::string& path) {
  if (mkdir(path.c_str(), 0700) && errno != EEXIST) {
    std::cerr << "Could not mkdir " << path << ": " << strerror(errno)
              << std::endl;
    return false;
  }
  return true;
}
class SnapshotTestEnvironment : public ::testing::Environment {
 public:
  () = delete;
  {}() = delete;
  {}
  void SetUp() override;
  void TearDown() override;
 private:
  bool CreateFakeSuper();
  std::unique_ptr<IImageManager> super_images_;
};
bool SnapshotTestEnvironment::CreateFakeSuper() {
  static constexpr int kImageFlags = IImageManager::CREATE_IMAGE_DEFAULT |
                                     IImageManager::CREATE_IMAGE_ZERO_FILL;
  if (!super_images_->CreateBackingImage("fake-super", kSuperSize,
                                         kImageFlags)) {
    LOG(ERROR) << "Could not create fake super partition";
    return false;
  }
  if (!super_images_->MapImageDevice("fake-super", 10s, &fake_super)) {
    LOG(ERROR) << "Could not map fake super partition";
    return false;
  }
  test_device->set_fake_super(fake_super);
  return true;
}
void SnapshotTestEnvironment::SetUp() {
  RETURN_IF_NON_VIRTUAL_AB_MSG(
      "Virtual A/B is not enabled, skipping global setup.\n");
  std::vector<std::string> paths = {
            "/data/gsi/ota/test",
            "/data/gsi/ota/test/super",
            "/metadata/gsi/ota/test",
            "/metadata/gsi/ota/test/super",
            "/metadata/ota/test",
            "/metadata/ota/test/snapshots",
  };
  for (const auto& path : paths) {
    ASSERT_TRUE(Mkdir(path));
  }
  test_device = new TestDeviceInfo();
  sm = SnapshotManager::New(test_device);
  ASSERT_NE(nullptr, sm) << "Could not create snapshot manager";
  super_images_ = IImageManager::Open("ota/test/super", 10s);
  ASSERT_NE(nullptr, super_images_) << "Could not create image manager";
  bool recreate_fake_super;
  if (super_images_->BackingImageExists("fake-super")) {
    if (super_images_->IsImageMapped("fake-super")) {
      ASSERT_TRUE(
          super_images_->GetMappedImageDevice("fake-super", &fake_super));
    } else {
      ASSERT_TRUE(
          super_images_->MapImageDevice("fake-super", 10s, &fake_super));
    }
    test_device->set_fake_super(fake_super);
    recreate_fake_super = true;
  } else {
    ASSERT_TRUE(CreateFakeSuper());
    recreate_fake_super = false;
  }
  MetadataMountedTest().TearDown();
  SnapshotUpdateTest().Cleanup();
  SnapshotTest().Cleanup();
  if (recreate_fake_super) {
    DeleteBackingImage(super_images_.get(), "fake-super");
    ASSERT_TRUE(CreateFakeSuper());
  }
}
void SnapshotTestEnvironment::TearDown() {
  RETURN_IF_NON_VIRTUAL_AB();
  if (super_images_ != nullptr) {
    DeleteBackingImage(super_images_.get(), "fake-super");
  }
}
void KillSnapuserd() {
  auto status = android::base::GetProperty("init.svc.snapuserd", "stopped");
  if (status == "stopped") {
    return;
  }
  auto snapuserd_client = SnapuserdClient::Connect(kSnapuserdSocket, 5s);
  if (!snapuserd_client) {
    return;
  }
  snapuserd_client->DetachSnapuserd();
}
}
}
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::AddGlobalTestEnvironment(
      new ::android::snapshot::SnapshotTestEnvironment());
  gflags::ParseCommandLineFlags(&argc, &argv, false);
  android::base::SetProperty("ctl.stop", "snapuserd");
  std::unordered_set<std::string> modes = {"", "vab-legacy", "vabc-legacy"};
  if (modes.count(FLAGS_force_mode) == 0) {
    std::cerr << "Unexpected force_config argument\n";
    return 1;
  }
  android::snapshot::KillSnapuserd();
  int ret = RUN_ALL_TESTS();
  android::base::SetProperty("snapuserd.test.dm.snapshots", "0");
  android::base::SetProperty("snapuserd.test.io_uring.force_disable", "0");
  android::snapshot::KillSnapuserd();
  return ret;
}
