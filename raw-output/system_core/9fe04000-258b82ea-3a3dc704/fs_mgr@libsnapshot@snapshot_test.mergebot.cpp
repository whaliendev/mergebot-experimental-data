// Copyright (C) 2018 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <libsnapshot/cow_format.h>
#include <libsnapshot/snapshot.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
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
// Mock classes are not used. Header included to ensure mocked class definition aligns with the
// class itself.

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

// Global states. See test_helpers.h.
std::unique_ptr<SnapshotManager> sm;
TestDeviceInfo* test_device = nullptr;
std::string fake_super;

void MountMetadata();
class SnapshotTest : public ::testing::Test {
public:
    SnapshotTest() : dm_(DeviceMapper::Instance()) {}
    
    // This is exposed for main.
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
        ASSERT_TRUE(android::base::SetProperty("snapuserd.test.io_uring.force_disable", "0"))
                << "Failed to set property: snapuserd.test.io_uring.disabled";
    
        if (FLAGS_force_mode == "vabc-legacy") {
            ASSERT_TRUE(android::base::SetProperty("snapuserd.test.dm.snapshots", "1"))
                    << "Failed to disable property: virtual_ab.userspace.snapshots.enabled";
            properties["ro.virtual_ab.compression.enabled"] = "true";
            properties["ro.virtual_ab.userspace.snapshots.enabled"] = "false";
        } else if (FLAGS_force_mode == "vab-legacy") {
            properties["ro.virtual_ab.compression.enabled"] = "false";
            properties["ro.virtual_ab.userspace.snapshots.enabled"] = "false";
        }
    
        if (FLAGS_force_iouring_disable == "iouring_disabled") {
            ASSERT_TRUE(android::base::SetProperty("snapuserd.test.io_uring.force_disable", "1"))
                    << "Failed to set property: snapuserd.test.io_uring.disabled";
            properties["ro.virtual_ab.io_uring.enabled"] = "false";
        }
    
        auto fetcher = std::make_unique<SnapshotTestPropertyFetcher>("_a", std::move(properties));
        IPropertyFetcher::OverrideForTesting(std::move(fetcher));
    
        if (GetLegacyCompressionEnabledProperty() || CanUseUserspaceSnapshots()) {
            // If we're asked to test the device's actual configuration, then it
            // may be misconfigured, so check for kernel support as libsnapshot does.
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
    
    void InitializeState() {
        ASSERT_TRUE(sm->EnsureImageManager());
        image_manager_ = sm->image_manager();
    
        test_device->set_slot_suffix("_a");
    
        sm->set_use_first_stage_snapuserd(false);
    }
    
    void CleanupTestArtifacts() {
        // Normally cancelling inside a merge is not allowed. Since these
        // are tests, we don't care, destroy everything that might exist.
        // Note we hardcode this list because of an annoying quirk: when
        // completing a merge, the snapshot stops existing, so we can't
        // get an accurate list to remove.
        lock_ = nullptr;
    
        // If there is no image manager, the test was skipped.
        if (!image_manager_) {
            return;
        }
    
        std::vector<std::string> snapshots = {"test-snapshot", "test_partition_a",
                                              "test_partition_b"};
        for (const auto& snapshot : snapshots) {
            CleanupSnapshotArtifacts(snapshot);
        }
    
        // Remove stale partitions in fake super.
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
        // The device-mapper stack may have been collapsed to dm-linear, so it's
        // necessary to check what state it's in before attempting a cleanup.
        // SnapshotManager has no path like this because we'd never remove a
        // merged snapshot (a live partition).
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
    
    // This is so main() can instantiate this to invoke Cleanup.
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
    
    // If |path| is non-null, the partition will be mapped after creation.
    bool CreatePartition(const std::string& name, uint64_t size, std::string* path = nullptr,
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
    
    bool CreatePartition(MetadataBuilder* builder, const std::string& name, uint64_t size,
                         std::string* path, const std::string& group) {
        auto partition = builder->AddPartition(name, group, 0);
        if (!partition) return false;
        if (!builder->ResizePartition(partition, size)) {
            return false;
        }
    
        // Update the source slot.
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
    
    AssertionResult MapUpdateSnapshot(const std::string& name, std::string* path) {
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
    
    AssertionResult MapUpdateSnapshot(const std::string& name, std::string* path) {
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
            return AssertionFailure() << "Cannot delete dm-user device for " << snapshot;
        }
        if (!(res = DeleteDevice(snapshot + "-inner"))) return res;
        if (!(res = DeleteDevice(snapshot + "-cow"))) return res;
        if (!image_manager_->UnmapImageIfExists(snapshot + "-cow-img")) {
            return AssertionFailure() << "Cannot unmap image " << snapshot << "-cow-img";
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
            return AssertionFailure() << "Cannot unmap " << name << " after zero filling it";
        }
        return AssertionSuccess();
    }
    
    AssertionResult MapCowImage(const std::string& name,
                                const std::chrono::milliseconds& timeout_ms, std::string* path) {
        auto cow_image_path = sm->MapCowImage(name, timeout_ms);
        if (!cow_image_path.has_value()) {
            return AssertionFailure() << "Cannot map cow image " << name;
        }
        *path = *cow_image_path;
        return AssertionSuccess();
    }
    
    // Prepare A/B slot for a partition named "test_partition".
    AssertionResult PrepareOneSnapshot(uint64_t device_size,
                                       std::unique_ptr<ISnapshotWriter>* writer = nullptr) {
        lock_ = nullptr;
    
        DeltaArchiveManifest manifest;
    
        auto dynamic_partition_metadata = manifest.mutable_dynamic_partition_metadata();
        dynamic_partition_metadata->set_vabc_enabled(snapuserd_required_);
        dynamic_partition_metadata->set_cow_version(android::snapshot::kCowVersionMajor);
        if (snapuserd_required_) {
            dynamic_partition_metadata->set_vabc_compression_param(FLAGS_compression_method);
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
        if (!CreatePartition(builder.get(), "test_partition_a", device_size, nullptr, "group_a")) {
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
    
    // Simulate a reboot into the new slot.
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
    
    std::unique_ptr<SnapshotManager> NewManagerForFirstStageMount(TestDeviceInfo* info) {
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

TEST_F(SnapshotTest, MergeFailureCode) {
    ASSERT_TRUE(AcquireLock());

    ASSERT_TRUE(sm->WriteUpdateState(lock_.get(), UpdateState::MergeFailed,
                                     MergeFailureCode::ListSnapshots));
    ASSERT_EQ(test_device->merge_status(), MergeStatus::MERGING);

    SnapshotUpdateStatus status = sm->ReadSnapshotUpdateStatus(lock_.get());
    ASSERT_EQ(status.state(), UpdateState::MergeFailed);
    ASSERT_EQ(status.merge_failure_code(), MergeFailureCode::ListSnapshots);
}

TEST_F(SnapshotTest, MergeFailureCode) {
    ASSERT_TRUE(AcquireLock());

    ASSERT_TRUE(sm->WriteUpdateState(lock_.get(), UpdateState::MergeFailed,
                                     MergeFailureCode::ListSnapshots));
    ASSERT_EQ(test_device->merge_status(), MergeStatus::MERGING);

    SnapshotUpdateStatus status = sm->ReadSnapshotUpdateStatus(lock_.get());
    ASSERT_EQ(status.state(), UpdateState::MergeFailed);
    ASSERT_EQ(status.merge_failure_code(), MergeFailureCode::ListSnapshots);
}

TEST_F(SnapshotTest, MergeFailureCode) {
    ASSERT_TRUE(AcquireLock());

    ASSERT_TRUE(sm->WriteUpdateState(lock_.get(), UpdateState::MergeFailed,
                                     MergeFailureCode::ListSnapshots));
    ASSERT_EQ(test_device->merge_status(), MergeStatus::MERGING);

    SnapshotUpdateStatus status = sm->ReadSnapshotUpdateStatus(lock_.get());
    ASSERT_EQ(status.state(), UpdateState::MergeFailed);
    ASSERT_EQ(status.merge_failure_code(), MergeFailureCode::ListSnapshots);
}

TEST_F(SnapshotTest, MergeFailureCode) {
    ASSERT_TRUE(AcquireLock());

    ASSERT_TRUE(sm->WriteUpdateState(lock_.get(), UpdateState::MergeFailed,
                                     MergeFailureCode::ListSnapshots));
    ASSERT_EQ(test_device->merge_status(), MergeStatus::MERGING);

    SnapshotUpdateStatus status = sm->ReadSnapshotUpdateStatus(lock_.get());
    ASSERT_EQ(status.state(), UpdateState::MergeFailed);
    ASSERT_EQ(status.merge_failure_code(), MergeFailureCode::ListSnapshots);
}

TEST_F(SnapshotTest, MergeFailureCode) {
    ASSERT_TRUE(AcquireLock());

    ASSERT_TRUE(sm->WriteUpdateState(lock_.get(), UpdateState::MergeFailed,
                                     MergeFailureCode::ListSnapshots));
    ASSERT_EQ(test_device->merge_status(), MergeStatus::MERGING);

    SnapshotUpdateStatus status = sm->ReadSnapshotUpdateStatus(lock_.get());
    ASSERT_EQ(status.state(), UpdateState::MergeFailed);
    ASSERT_EQ(status.merge_failure_code(), MergeFailureCode::ListSnapshots);
}

TEST_F(SnapshotTest, MergeFailureCode) {
    ASSERT_TRUE(AcquireLock());

    ASSERT_TRUE(sm->WriteUpdateState(lock_.get(), UpdateState::MergeFailed,
                                     MergeFailureCode::ListSnapshots));
    ASSERT_EQ(test_device->merge_status(), MergeStatus::MERGING);

    SnapshotUpdateStatus status = sm->ReadSnapshotUpdateStatus(lock_.get());
    ASSERT_EQ(status.state(), UpdateState::MergeFailed);
    ASSERT_EQ(status.merge_failure_code(), MergeFailureCode::ListSnapshots);
}

TEST_F(SnapshotTest, MergeFailureCode) {
    ASSERT_TRUE(AcquireLock());

    ASSERT_TRUE(sm->WriteUpdateState(lock_.get(), UpdateState::MergeFailed,
                                     MergeFailureCode::ListSnapshots));
    ASSERT_EQ(test_device->merge_status(), MergeStatus::MERGING);

    SnapshotUpdateStatus status = sm->ReadSnapshotUpdateStatus(lock_.get());
    ASSERT_EQ(status.state(), UpdateState::MergeFailed);
    ASSERT_EQ(status.merge_failure_code(), MergeFailureCode::ListSnapshots);
}

TEST_F(SnapshotTest, MergeFailureCode) {
    ASSERT_TRUE(AcquireLock());

    ASSERT_TRUE(sm->WriteUpdateState(lock_.get(), UpdateState::MergeFailed,
                                     MergeFailureCode::ListSnapshots));
    ASSERT_EQ(test_device->merge_status(), MergeStatus::MERGING);

    SnapshotUpdateStatus status = sm->ReadSnapshotUpdateStatus(lock_.get());
    ASSERT_EQ(status.state(), UpdateState::MergeFailed);
    ASSERT_EQ(status.merge_failure_code(), MergeFailureCode::ListSnapshots);
}

TEST_F(SnapshotTest, MergeFailureCode) {
    ASSERT_TRUE(AcquireLock());

    ASSERT_TRUE(sm->WriteUpdateState(lock_.get(), UpdateState::MergeFailed,
                                     MergeFailureCode::ListSnapshots));
    ASSERT_EQ(test_device->merge_status(), MergeStatus::MERGING);

    SnapshotUpdateStatus status = sm->ReadSnapshotUpdateStatus(lock_.get());
    ASSERT_EQ(status.state(), UpdateState::MergeFailed);
    ASSERT_EQ(status.merge_failure_code(), MergeFailureCode::ListSnapshots);
}

TEST_F(SnapshotTest, MergeFailureCode) {
    ASSERT_TRUE(AcquireLock());

    ASSERT_TRUE(sm->WriteUpdateState(lock_.get(), UpdateState::MergeFailed,
                                     MergeFailureCode::ListSnapshots));
    ASSERT_EQ(test_device->merge_status(), MergeStatus::MERGING);

    SnapshotUpdateStatus status = sm->ReadSnapshotUpdateStatus(lock_.get());
    ASSERT_EQ(status.state(), UpdateState::MergeFailed);
    ASSERT_EQ(status.merge_failure_code(), MergeFailureCode::ListSnapshots);
}

TEST_F(SnapshotTest, MergeFailureCode) {
    ASSERT_TRUE(AcquireLock());

    ASSERT_TRUE(sm->WriteUpdateState(lock_.get(), UpdateState::MergeFailed,
                                     MergeFailureCode::ListSnapshots));
    ASSERT_EQ(test_device->merge_status(), MergeStatus::MERGING);

    SnapshotUpdateStatus status = sm->ReadSnapshotUpdateStatus(lock_.get());
    ASSERT_EQ(status.state(), UpdateState::MergeFailed);
    ASSERT_EQ(status.merge_failure_code(), MergeFailureCode::ListSnapshots);
}

enum class Request {             UNKNOWN,                  LOCK_SHARED,                            LOCK_EXCLUSIVE,                                                    UNLOCK,                                                               EXIT};

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
        return AssertionFailure() << "Timeout waiting for fulfilling " << requests_.size()
                                  << " request(s), first one is "
                                  << (requests_.empty() ? Request::UNKNOWN : requests_.front());
    }
    
    void StartHandleRequestsInBackground() {
        future_ = std::async(std::launch::async, &LockTestConsumer::HandleRequests, this);
    }
    
private:
    void HandleRequests() {
        static constexpr auto consumer_timeout = 3s;
    
        auto next_request = Request::UNKNOWN;
        do {
            // Peek next request.
            {
                std::unique_lock<std::mutex> ulock(mutex_);
                if (cv_.wait_for(ulock, consumer_timeout, [this] { return !requests_.empty(); })) {
                    next_request = requests_.front();
                } else {
                    next_request = Request::EXIT;
                }
            }
    
            // Handle next request.
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
    
            // Pop next request. This thread is the only thread that
            // pops from the front of the requests_ deque.
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

TEST_F(LockTest, SharedShared) {
    ASSERT_TRUE(first_consumer.MakeRequest(Request::LOCK_SHARED));
    ASSERT_TRUE(first_consumer.WaitFulfill(request_timeout));
    ASSERT_TRUE(second_consumer.MakeRequest(Request::LOCK_SHARED));
    ASSERT_TRUE(second_consumer.WaitFulfill(request_timeout));
}

using LockTestParam = std::pair<Request, Request>;
class LockTestP : public LockTest, public ::testing::WithParamInterface<LockTestParam> {};

TEST_P(LockTestP, Test) {
    ASSERT_TRUE(first_consumer.MakeRequest(GetParam().first));
    ASSERT_TRUE(first_consumer.WaitFulfill(request_timeout));
    ASSERT_TRUE(second_consumer.MakeRequest(GetParam().second));
    ASSERT_FALSE(second_consumer.WaitFulfill(request_timeout))
            << "Should not be able to " << GetParam().second << " while separate thread "
            << GetParam().first;
    ASSERT_TRUE(first_consumer.MakeRequest(Request::UNLOCK));
    ASSERT_TRUE(second_consumer.WaitFulfill(request_timeout))
            << "Should be able to hold lock that is released by separate thread";
}
class SnapshotUpdateTest : public SnapshotTest {
public:
    void SetUp() override {
        SKIP_IF_NON_VIRTUAL_AB();
    
        SnapshotTest::SetUp();
        if (!image_manager_) {
            // Test was skipped.
            return;
        }
    
        Cleanup();
    
        // Cleanup() changes slot suffix, so initialize it again.
        test_device->set_slot_suffix("_a");
    
        opener_ = std::make_unique<TestPartitionOpener>(fake_super);
    
        auto dynamic_partition_metadata = manifest_.mutable_dynamic_partition_metadata();
        dynamic_partition_metadata->set_vabc_enabled(snapuserd_required_);
        dynamic_partition_metadata->set_cow_version(android::snapshot::kCowVersionMajor);
        if (snapuserd_required_) {
            dynamic_partition_metadata->set_vabc_compression_param(FLAGS_compression_method);
        }
    
        // Create a fake update package metadata.
        // Not using full name "system", "vendor", "product" because these names collide with the
        // mapped partitions on the running device.
        // Each test modifies manifest_ slightly to indicate changes to the partition layout.
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
    
        // Initialize source partition metadata using |manifest_|.
        src_ = MetadataBuilder::New(*opener_, "super", 0);
        ASSERT_NE(src_, nullptr);
        ASSERT_TRUE(FillFakeMetadata(src_.get(), manifest_, "_a"));
        // Add sys_b which is like system_other.
        ASSERT_TRUE(src_->AddGroup("group_b", kGroupSize));
        auto partition = src_->AddPartition("sys_b", "group_b", 0);
        ASSERT_NE(nullptr, partition);
        ASSERT_TRUE(src_->ResizePartition(partition, 1_MiB));
        auto metadata = src_->Export();
        ASSERT_NE(nullptr, metadata);
        ASSERT_TRUE(UpdatePartitionTable(*opener_, "super", *metadata.get(), 0));
    
        // Map source partitions.
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
    
        // OTA client blindly unmaps all partitions that are possibly mapped.
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
    
            // Cheat our way out of merge failed states.
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
            return AssertionFailure() << "Path of " << name << " cannot be determined";
        }
        auto hash = GetHash(path);
        if (!hash.has_value()) {
            return AssertionFailure() << "Cannot read partition " << name << ": " << path;
        }
        auto it = hashes_.find(name);
        if (it == hashes_.end()) {
            return AssertionFailure() << "No existing hash for " << name << ". Bad test code?";
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
                return AssertionFailure() << "Unable to write random data to snapshot " << name;
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
                return AssertionFailure() << "Unable to write random data to snapshot " << name;
            }
        }
    
        // Make sure updates to one device are seen by all devices.
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
    
    // Generate a snapshot that moves all the upper blocks down to the start.
    // It doesn't really matter the order, we just want copies that reference
    // blocks that won't exist if the partition shrinks.
    AssertionResult ShiftAllSnapshotBlocks(const std::string& name, uint64_t old_size) {
        std::unique_ptr<ISnapshotWriter> writer;
        if (auto res = MapUpdateSnapshot(name, &writer); !res) {
            return res;
        }
        if (!writer->options().max_blocks || !*writer->options().max_blocks) {
            return AssertionFailure() << "No max blocks set for " << name << " writer";
        }
    
        uint64_t src_block = (old_size / writer->options().block_size) - 1;
        uint64_t dst_block = 0;
        uint64_t max_blocks = *writer->options().max_blocks;
        while (dst_block < max_blocks && dst_block < src_block) {
            if (!writer->AddCopy(dst_block, src_block)) {
                return AssertionFailure() << "Unable to add copy for " << name << " for blocks "
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
            return AssertionFailure() << "Unable to hash snapshot writer for " << name;
        }
        hashes_[name] = hash;
    
        return AssertionSuccess();
    }
    
    AssertionResult MapUpdateSnapshots(const std::vector<std::string>& names = {"sys_b", "vnd_b",
                                                                                "prd_b"}) {
        for (const auto& name : names) {
            auto res = MapOneUpdateSnapshot(name);
            if (!res) {
                return res;
            }
        }
        return AssertionSuccess();
    }
    
    // Create fake install operations to grow the COW device size.
    void AddOperation(PartitionUpdate* partition_update, uint64_t size_bytes = 0) {
        auto e = partition_update->add_operations()->add_dst_extents();
        e->set_start_block(0);
        if (size_bytes == 0) {
            size_bytes = GetSize(partition_update);
        }
        e->set_num_blocks(size_bytes / manifest_.block_size());
    }
    
    void AddOperationForPartitions(std::vector<PartitionUpdate*> partitions = {}) {
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

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

static std::vector<Interval> ToIntervals(const std::vector<std::unique_ptr<Extent>>& extents) {
    std::vector<Interval> ret;
    std::transform(extents.begin(), extents.end(), std::back_inserter(ret),
                   [](const auto& extent) { return extent->AsLinearExtent()->AsInterval(); });
    return ret;
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

class MetadataMountedTest : public ::testing::Test {
public:
    // This is so main() can instantiate this to invoke Cleanup.
    virtual void TestBody() override {}
    void SetUp() override {
        SKIP_IF_NON_VIRTUAL_AB();
        metadata_dir_ = test_device->GetMetadataDir();
        ASSERT_TRUE(ReadDefaultFstab(&fstab_));
    }
    void TearDown() override {
        RETURN_IF_NON_VIRTUAL_AB();
        SetUp();
        // Remount /metadata
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
            return AssertionFailure() << "No mount point found in fstab for path " << metadata_dir_;
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

void MountMetadata() {
    MetadataMountedTest().TearDown();
}

TEST_F(MetadataMountedTest, Recovery) {
    test_device->set_recovery(true);
    metadata_dir_ = test_device->GetMetadataDir();

    EXPECT_TRUE(android::fs_mgr::EnsurePathUnmounted(&fstab_, metadata_dir_));
    EXPECT_FALSE(IsMetadataMounted());

    auto device = sm->EnsureMetadataMounted();
    EXPECT_NE(nullptr, device);
    EXPECT_TRUE(IsMetadataMounted());

    device.reset();
    EXPECT_FALSE(IsMetadataMounted());
}

TEST_F(MetadataMountedTest, Recovery) {
    test_device->set_recovery(true);
    metadata_dir_ = test_device->GetMetadataDir();

    EXPECT_TRUE(android::fs_mgr::EnsurePathUnmounted(&fstab_, metadata_dir_));
    EXPECT_FALSE(IsMetadataMounted());

    auto device = sm->EnsureMetadataMounted();
    EXPECT_NE(nullptr, device);
    EXPECT_TRUE(IsMetadataMounted());

    device.reset();
    EXPECT_FALSE(IsMetadataMounted());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
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

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

TEST_F(SnapshotUpdateTest, QueryStatusError) {
    // Grow all partitions. Set |prd| large enough that |sys| and |vnd|'s COWs
    // fit in super, but not |prd|.
    constexpr uint64_t partition_size = 3788_KiB;
    SetSize(sys_, partition_size);

    AddOperationForPartitions();

    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));

    if (sm->UpdateUsesUserSnapshots()) {
        GTEST_SKIP() << "Test does not apply to userspace snapshots";
    }

    ASSERT_TRUE(WriteSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    ASSERT_TRUE(UnmapAll());

    class DmStatusFailure final : public DeviceMapperWrapper {
      public:
        bool GetTableStatus(const std::string& name, std::vector<TargetInfo>* table) override {
            if (!DeviceMapperWrapper::GetTableStatus(name, table)) {
                return false;
            }
            if (name == "sys_b" && !table->empty()) {
                auto& info = table->at(0);
                if (DeviceMapper::GetTargetType(info.spec) == "snapshot-merge") {
                    info.data = "Merge failed";
                }
            }
            return true;
        }
    };
    DmStatusFailure wrapper;

    // After reboot, init does first stage mount.
    auto info = new TestDeviceInfo(fake_super, "_b");
    info->set_dm(&wrapper);

    auto init = NewManagerForFirstStageMount(info);
    ASSERT_NE(init, nullptr);

    ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Initiate the merge and wait for it to be completed.
    ASSERT_TRUE(init->InitiateMerge());
    ASSERT_EQ(UpdateState::MergeFailed, init->ProcessUpdateState());

    // Simulate a reboot that tries the merge again, with the non-failing dm.
    ASSERT_TRUE(UnmapAll());
    init = NewManagerForFirstStageMount("_b");
    ASSERT_NE(init, nullptr);
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));
    ASSERT_EQ(UpdateState::MergeCompleted, init->ProcessUpdateState());
}

class FlashAfterUpdateTest : public SnapshotUpdateTest,
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

TEST_P(FlashAfterUpdateTest, FlashSlotAfterUpdate) {
    // Execute the update.
    ASSERT_TRUE(sm->BeginUpdate());
    ASSERT_TRUE(sm->CreateUpdateSnapshots(manifest_));
    ASSERT_TRUE(MapUpdateSnapshots());
    ASSERT_TRUE(sm->FinishedSnapshotWrites(false));

    // Simulate shutting down the device.
    ASSERT_TRUE(UnmapAll());

    bool after_merge = std::get<1>(GetParam());
    if (after_merge) {
        ASSERT_TRUE(InitiateMerge("_b"));
        // Simulate shutting down the device after merge has initiated.
        ASSERT_TRUE(UnmapAll());
    }

    auto flashed_slot = std::get<0>(GetParam());
    auto flashed_slot_suffix = SlotSuffixForSlotNumber(flashed_slot);

    // Simulate flashing |flashed_slot|. This clears the UPDATED flag.
    auto flashed_builder = MetadataBuilder::New(*opener_, "super", flashed_slot);
    ASSERT_NE(flashed_builder, nullptr);
    flashed_builder->RemoveGroupAndPartitions(group_->name() + flashed_slot_suffix);
    flashed_builder->RemoveGroupAndPartitions(kCowGroupName);
    ASSERT_TRUE(FillFakeMetadata(flashed_builder.get(), manifest_, flashed_slot_suffix));

    // Deliberately remove a partition from this build so that
    // InitiateMerge do not switch state to "merging". This is possible in
    // practice because the list of dynamic partitions may change.
    ASSERT_NE(nullptr, flashed_builder->FindPartition("prd" + flashed_slot_suffix));
    flashed_builder->RemovePartition("prd" + flashed_slot_suffix);

    // Note that fastbootd always updates the partition table of both slots.
    auto flashed_metadata = flashed_builder->Export();
    ASSERT_NE(nullptr, flashed_metadata);
    ASSERT_TRUE(UpdatePartitionTable(*opener_, "super", *flashed_metadata, 0));
    ASSERT_TRUE(UpdatePartitionTable(*opener_, "super", *flashed_metadata, 1));

    std::string path;
    for (const auto& name : {"sys", "vnd"}) {
        ASSERT_TRUE(CreateLogicalPartition(
                CreateLogicalPartitionParams{
                        .block_device = fake_super,
                        .metadata_slot = flashed_slot,
                        .partition_name = name + flashed_slot_suffix,
                        .timeout_ms = 1s,
                        .partition_opener = opener_.get(),
                },
                &path));
        ASSERT_TRUE(WriteRandomData(path));
        auto hash = GetHash(path);
        ASSERT_TRUE(hash.has_value());
        hashes_[name + flashed_slot_suffix] = *hash;
    }

    // Simulate shutting down the device after flash.
    ASSERT_TRUE(UnmapAll());

    // Simulate reboot. After reboot, init does first stage mount.
    auto init = NewManagerForFirstStageMount(flashed_slot_suffix);
    ASSERT_NE(init, nullptr);

    if (flashed_slot && after_merge) {
        ASSERT_TRUE(init->NeedSnapshotsInFirstStageMount());
    }
    ASSERT_TRUE(init->CreateLogicalAndSnapshotPartitions("super", snapshot_timeout_));

    // Check that the target partitions have the same content.
    for (const auto& name : {"sys", "vnd"}) {
        ASSERT_TRUE(IsPartitionUnchanged(name + flashed_slot_suffix));
    }

    // There should be no snapshot to merge.
    auto new_sm = SnapshotManager::New(new TestDeviceInfo(fake_super, flashed_slot_suffix));
    if (flashed_slot == 0 && after_merge) {
        ASSERT_EQ(UpdateState::MergeCompleted, new_sm->ProcessUpdateState());
    } else {
        // update_engine calls ProcessUpdateState first -- should see Cancelled.
        ASSERT_EQ(UpdateState::Cancelled, new_sm->ProcessUpdateState());
    }

    // Next OTA calls CancelUpdate no matter what.
    ASSERT_TRUE(new_sm->CancelUpdate());
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

TEST_F(ImageManagerTest, CreateImageNoSpace) {
    bool at_least_one_failure = false;
    for (uint64_t size = 1_MiB; size <= 512_MiB; size *= 2) {
        auto userdata = std::make_unique<LowSpaceUserdata>();
        ASSERT_TRUE(userdata->Init(size));

        uint64_t to_allocate = userdata->free_space() + userdata->bsize();

        auto res = image_manager_->CreateBackingImage(kImageName, to_allocate,
                                                      IImageManager::CREATE_IMAGE_DEFAULT);
        if (!res) {
            at_least_one_failure = true;
        } else {
            ASSERT_EQ(res.error_code(), FiemapStatus::ErrorCode::NO_SPACE) << res.string();
        }

        CleanUp();
    }

    ASSERT_TRUE(at_least_one_failure)
            << "We should have failed to allocate at least one over-sized image";
}

bool Mkdir(const std::string& path) {
    if (mkdir(path.c_str(), 0700) && errno != EEXIST) {
        std::cerr << "Could not mkdir " << path << ": " << strerror(errno) << std::endl;
        return false;
    }
    return true;
}

class SnapshotTestEnvironment : public ::testing::Environment {
public:
    () = delete;{}
    () = delete;{}
    void SetUp() override;
    void TearDown() override;
    
private:
    bool CreateFakeSuper();
    
    std::unique_ptr<IImageManager> super_images_;
};

bool SnapshotTestEnvironment::CreateFakeSuper() {
    // Create and map the fake super partition.
    static constexpr int kImageFlags =
            IImageManager::CREATE_IMAGE_DEFAULT | IImageManager::CREATE_IMAGE_ZERO_FILL;
    if (!super_images_->CreateBackingImage("fake-super", kSuperSize, kImageFlags)) {
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
    // b/163082876: GTEST_SKIP in Environment will make atest report incorrect results. Until
    // that is fixed, don't call GTEST_SKIP here, but instead call GTEST_SKIP in individual test
    // suites.
    RETURN_IF_NON_VIRTUAL_AB_MSG("Virtual A/B is not enabled, skipping global setup.\n");

    std::vector<std::string> paths = {
            // clang-format off
            "/data/gsi/ota/test",
            "/data/gsi/ota/test/super",
            "/metadata/gsi/ota/test",
            "/metadata/gsi/ota/test/super",
            "/metadata/ota/test",
            "/metadata/ota/test/snapshots",
            // clang-format on
    };
    for (const auto& path : paths) {
        ASSERT_TRUE(Mkdir(path));
    }

    // Create this once, otherwise, gsid will start/stop between each test.
    test_device = new TestDeviceInfo();
    sm = SnapshotManager::New(test_device);
    ASSERT_NE(nullptr, sm) << "Could not create snapshot manager";

    // Use a separate image manager for our fake super partition.
    super_images_ = IImageManager::Open("ota/test/super", 10s);
    ASSERT_NE(nullptr, super_images_) << "Could not create image manager";

    // Map the old image if one exists so we can safely unmap everything that
    // depends on it.
    bool recreate_fake_super;
    if (super_images_->BackingImageExists("fake-super")) {
        if (super_images_->IsImageMapped("fake-super")) {
            ASSERT_TRUE(super_images_->GetMappedImageDevice("fake-super", &fake_super));
        } else {
            ASSERT_TRUE(super_images_->MapImageDevice("fake-super", 10s, &fake_super));
        }
        test_device->set_fake_super(fake_super);
        recreate_fake_super = true;
    } else {
        ASSERT_TRUE(CreateFakeSuper());
        recreate_fake_super = false;
    }

    // Clean up previous run.
    MetadataMountedTest().TearDown();
    SnapshotUpdateTest().Cleanup();
    SnapshotTest().Cleanup();

    if (recreate_fake_super) {
        // Clean up any old copy.
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

} // namespace snapshot

} // namespace android

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new ::android::snapshot::SnapshotTestEnvironment());
    gflags::ParseCommandLineFlags(&argc, &argv, false);

    android::base::SetProperty("ctl.stop", "snapuserd");

    std::unordered_set<std::string> modes = {"", "vab-legacy", "vabc-legacy"};
    if (modes.count(FLAGS_force_mode) == 0) {
        std::cerr << "Unexpected force_config argument\n";
        return 1;
    }

    // This is necessary if the configuration we're testing doesn't match the device.
    android::snapshot::KillSnapuserd();

    int ret = RUN_ALL_TESTS();

    android::base::SetProperty("snapuserd.test.dm.snapshots", "0");
    android::base::SetProperty("snapuserd.test.io_uring.force_disable", "0");

    android::snapshot::KillSnapuserd();
    return ret;
}