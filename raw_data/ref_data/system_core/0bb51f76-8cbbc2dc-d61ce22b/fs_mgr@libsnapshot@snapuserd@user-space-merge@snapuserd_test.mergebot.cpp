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

#include <android-base/strings.h>
#include <gflags/gflags.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/memfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <chrono>
#include <iostream>
#include <memory>
#include <string_view>
#include <android-base/file.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <fs_mgr/file_wait.h>
#include <gtest/gtest.h>
#include <libdm/dm.h>
#include <snapuserd/snapuserd_client.h>
#include <libdm/loop_control.h>
#include <libsnapshot/cow_writer.h>
#include <snapuserd/dm_user_block_server.h>
#include <storage_literals/storage_literals.h>
#include "handler_manager.h"
#include "merge_worker.h"
#include "read_worker.h"
#include "snapuserd_core.h"
#include "testing/dm_user_harness.h"
#include "testing/host_harness.h"
#include "testing/temp_device.h"
#include "utility.h"

namespace android {
namespace snapshot {

using namespace android::storage_literals;
using android::base::unique_fd;
using LoopDevice = android::dm::LoopDevice;
using namespace std::chrono_literals;
using namespace android::dm;
using namespace std;

using testing::AssertionFailure;
using testing::AssertionResult;
using testing::AssertionSuccess;

class SnapuserdTestBase : public ::testing::Test {
 protected:
  void SetUp() override;
  void TearDown() override;
  void CreateBaseDevice();
  void CreateCowDevice();
  void SetDeviceControlName();
  std::unique_ptr<ICowWriter> CreateCowDeviceInternal();

  std::unique_ptr<ITestHarness> harness_;
  size_t size_ = 10_MiB;
  int total_base_size_ = 0;
  std::string system_device_ctrl_name_;
  std::string system_device_name_;

  unique_ptr<IBackingDevice> base_dev_;
  unique_fd base_fd_;

  std::unique_ptr<TemporaryFile> cow_system_;

  std::unique_ptr<uint8_t[]> orig_buffer_;
};

void SnapuserdTestBase::SetUp() {
#if __ANDROID__
  harness_ = std::make_unique<DmUserTestHarness>();
#else
  harness_ = std::make_unique<HostTestHarness>();
#endif
}

void SnapuserdTestBase::TearDown() {}

static constexpr char kSnapuserdSocketTest[] = "snapuserdTest";

class SnapuserdTest : public SnapuserdTestBase {
 protected:
  void SetUp() override;
  void TearDown() override;

 public:
  void SetupDefault();
  void SetupOrderedOps();
  void SetupOrderedOpsInverted();
  void SetupCopyOverlap_1();
  void SetupCopyOverlap_2();
  bool Merge();
  void ValidateMerge();
  void ReadSnapshotDeviceAndValidate();
  void Shutdown();
  void MergeInterrupt();
  void MergeInterruptFixed(int duration);
  void MergeInterruptRandomly(int max_duration);
  bool StartMerge();
  void CheckMergeCompletion();

  static const uint64_t kSectorSize = 512;

 protected:
  void SnapuserdTest::SetUp() {
    ASSERT_NO_FATAL_FAILURE(SnapuserdTestBase::SetUp());
    handlers_ = std::make_unique<SnapshotHandlerManager>();
  }
  void SnapuserdTest::TearDown() {
    SnapuserdTestBase::TearDown();
    Shutdown();
  }

 private:
  void SetupImpl();

  void SimulateDaemonRestart();

  void CreateCowDeviceOrderedOps();
  void CreateCowDeviceOrderedOpsInverted();
  void CreateCowDeviceWithCopyOverlap_1();
  void CreateCowDeviceWithCopyOverlap_2();
  void SetupDaemon();
  void InitCowDevice();
  void InitDaemon();

 protected:
  void CreateUserDevice();

 private:
  void StartSnapuserdDaemon();

  unique_ptr<IUserDevice> dmuser_dev_;

  std::unique_ptr<SnapuserdClient> client_;
  std::unique_ptr<uint8_t[]> merged_buffer_;
  int cow_num_sectors_;
};

void SnapuserdTest::Shutdown() {
  if (dmuser_dev_) {
    ASSERT_TRUE(dmuser_dev_->Destroy());
  }

  auto misc_device = "/dev/dm-user/" + system_device_ctrl_name_;
<<<<<<< HEAD
  ASSERT_TRUE(handlers_->DeleteHandler(system_device_ctrl_name_));
|||||||
  ASSERT_TRUE(handlers_.DeleteHandler(system_device_ctrl_name_));
=======
  ASSERT_TRUE(client_->WaitForDeviceDelete(system_device_ctrl_name_));
>>>>>>> 8cbbc2dc926e131322853222f434d949aed12adc
  ASSERT_TRUE(android::fs_mgr::WaitForFileDeleted(misc_device, 10s));
<<<<<<< HEAD
  handlers_->TerminateMergeThreads();
  handlers_->JoinAllThreads();
  handlers_ = std::make_unique<SnapshotHandlerManager>();
|||||||
  handlers_.TerminateMergeThreads();
=======
  ASSERT_TRUE(client_->DetachSnapuserd());
>>>>>>> 8cbbc2dc926e131322853222f434d949aed12adc
}

void SnapuserdTest::SetupDefault() { ASSERT_NO_FATAL_FAILURE(SetupImpl()); }

void SnapuserdTest::SetupOrderedOps() {
  ASSERT_NO_FATAL_FAILURE(CreateBaseDevice());
  ASSERT_NO_FATAL_FAILURE(CreateCowDeviceOrderedOps());
  ASSERT_NO_FATAL_FAILURE(SetupDaemon());
}

void SnapuserdTest::SetupOrderedOpsInverted() {
  ASSERT_NO_FATAL_FAILURE(CreateBaseDevice());
  ASSERT_NO_FATAL_FAILURE(CreateCowDeviceOrderedOpsInverted());
  ASSERT_NO_FATAL_FAILURE(SetupDaemon());
}

void SnapuserdTest::SetupCopyOverlap_1() {
  ASSERT_NO_FATAL_FAILURE(CreateBaseDevice());
  ASSERT_NO_FATAL_FAILURE(CreateCowDeviceWithCopyOverlap_1());
  ASSERT_NO_FATAL_FAILURE(SetupDaemon());
}

void SnapuserdTest::SetupCopyOverlap_2() {
  ASSERT_NO_FATAL_FAILURE(CreateBaseDevice());
  ASSERT_NO_FATAL_FAILURE(CreateCowDeviceWithCopyOverlap_2());
  ASSERT_NO_FATAL_FAILURE(SetupDaemon());
}

void SnapuserdTest::SetupDaemon() {
  SetDeviceControlName();

<<<<<<< HEAD
  ASSERT_NO_FATAL_FAILURE(CreateUserDevice());
  ASSERT_NO_FATAL_FAILURE(InitCowDevice());
  ASSERT_NO_FATAL_FAILURE(InitDaemon());
|||||||
  CreateDmUserDevice();
  InitCowDevice();
  InitDaemon();

  setup_ok_ = true;

  return setup_ok_;
=======
  StartSnapuserdDaemon();

  CreateDmUserDevice();
  InitCowDevice();
  InitDaemon();

  setup_ok_ = true;

  return setup_ok_;
>>>>>>> 8cbbc2dc926e131322853222f434d949aed12adc
}

void SnapuserdTest::StartSnapuserdDaemon() {
  pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    std::string arg0 = "/system/bin/snapuserd";
    std::string arg1 = "-socket="s + kSnapuserdSocketTest;
    char* const argv[] = {arg0.data(), arg1.data(), nullptr};
    ASSERT_GE(execv(arg0.c_str(), argv), 0);
  } else {
    client_ = SnapuserdClient::Connect(kSnapuserdSocketTest, 10s);
    ASSERT_NE(client_, nullptr);
  }
}

void SnapuserdTestBase::CreateBaseDevice() {
  total_base_size_ = (size_ * 5);

  base_dev_ = harness_->CreateBackingDevice(total_base_size_);
  ASSERT_NE(base_dev_, nullptr);

  base_fd_.reset(open(base_dev_->GetPath().c_str(), O_RDWR | O_CLOEXEC));
  ASSERT_GE(base_fd_, 0);

  unique_fd rnd_fd(open("/dev/random", O_RDONLY));
  ASSERT_GE(rnd_fd, 0);

  std::unique_ptr<uint8_t[]> random_buffer = std::make_unique<uint8_t[]>(1_MiB);

  for (size_t j = 0; j < ((total_base_size_) / 1_MiB); j++) {
    ASSERT_EQ(ReadFullyAtOffset(rnd_fd, (char*)random_buffer.get(), 1_MiB, 0),
              true);
    ASSERT_EQ(android::base::WriteFully(base_fd_, random_buffer.get(), 1_MiB),
              true);
  }

  ASSERT_EQ(lseek(base_fd_, 0, SEEK_SET), 0);
}

void SnapuserdTest::ReadSnapshotDeviceAndValidate() {
  unique_fd fd(open(dmuser_dev_->GetPath().c_str(), O_RDONLY));
  ASSERT_GE(fd, 0);
  std::unique_ptr<uint8_t[]> snapuserd_buffer =
      std::make_unique<uint8_t[]>(size_);

  // COPY
  loff_t offset = 0;
  ASSERT_EQ(ReadFullyAtOffset(fd, snapuserd_buffer.get(), size_, offset), true);
  ASSERT_EQ(memcmp(snapuserd_buffer.get(), orig_buffer_.get(), size_), 0);

  // REPLACE
  offset += size_;
  ASSERT_EQ(ReadFullyAtOffset(fd, snapuserd_buffer.get(), size_, offset), true);
  ASSERT_EQ(
      memcmp(snapuserd_buffer.get(), (char*)orig_buffer_.get() + size_, size_),
      0);

  // ZERO
  offset += size_;
  ASSERT_EQ(ReadFullyAtOffset(fd, snapuserd_buffer.get(), size_, offset), true);
  ASSERT_EQ(memcmp(snapuserd_buffer.get(),
                   (char*)orig_buffer_.get() + (size_ * 2), size_),
            0);

  // REPLACE
  offset += size_;
  ASSERT_EQ(ReadFullyAtOffset(fd, snapuserd_buffer.get(), size_, offset), true);
  ASSERT_EQ(memcmp(snapuserd_buffer.get(),
                   (char*)orig_buffer_.get() + (size_ * 3), size_),
            0);

  // XOR
  offset += size_;
  ASSERT_EQ(ReadFullyAtOffset(fd, snapuserd_buffer.get(), size_, offset), true);
  ASSERT_EQ(memcmp(snapuserd_buffer.get(),
                   (char*)orig_buffer_.get() + (size_ * 4), size_),
            0);
}

std::unique_ptr<ICowWriter> SnapuserdTestBase::CreateCowDeviceInternal() {
  cow_system_ = std::make_unique<TemporaryFile>();

  CowOptions options;
  options.compression = "gz";

  unique_fd fd(cow_system_->fd);
  cow_system_->fd = -1;

  return CreateCowWriter(kDefaultCowVersion, options, std::move(fd));
}

void SnapuserdTest::CreateCowDeviceWithCopyOverlap_2() {
  auto writer = CreateCowDeviceInternal();
  ASSERT_NE(writer, nullptr);

  size_t num_blocks = size_ / writer->GetBlockSize();
  size_t x = num_blocks;
  size_t blk_src_copy = 0;

  // Create overlapping copy operations
  while (1) {
    ASSERT_TRUE(writer->AddCopy(blk_src_copy, blk_src_copy + 1));
    x -= 1;
    if (x == 1) {
      break;
    }
    blk_src_copy += 1;
  }

  // Flush operations
  ASSERT_TRUE(writer->Finalize());

  // Construct the buffer required for validation
  orig_buffer_ = std::make_unique<uint8_t[]>(total_base_size_);

  // Read the entire base device
  ASSERT_EQ(android::base::ReadFullyAtOffset(base_fd_, orig_buffer_.get(),
                                             total_base_size_, 0),
            true);

  // Merged operations required for validation
  int block_size = 4096;
  x = num_blocks;
  loff_t src_offset = block_size;
  loff_t dest_offset = 0;

  while (1) {
    memmove((char*)orig_buffer_.get() + dest_offset,
            (char*)orig_buffer_.get() + src_offset, block_size);
    x -= 1;
    if (x == 1) {
      break;
    }
    src_offset += block_size;
    dest_offset += block_size;
  }
}

void SnapuserdTest::CreateCowDeviceWithCopyOverlap_1() {
  auto writer = CreateCowDeviceInternal();
  ASSERT_NE(writer, nullptr);

  size_t num_blocks = size_ / writer->GetBlockSize();
  size_t x = num_blocks;
  size_t blk_src_copy = num_blocks - 1;

  // Create overlapping copy operations
  while (1) {
    ASSERT_TRUE(writer->AddCopy(blk_src_copy + 1, blk_src_copy));
    x -= 1;
    if (x == 0) {
      ASSERT_EQ(blk_src_copy, 0);
      break;
    }
    blk_src_copy -= 1;
  }

  // Flush operations
  ASSERT_TRUE(writer->Finalize());

  // Construct the buffer required for validation
  orig_buffer_ = std::make_unique<uint8_t[]>(total_base_size_);

  // Read the entire base device
  ASSERT_EQ(android::base::ReadFullyAtOffset(base_fd_, orig_buffer_.get(),
                                             total_base_size_, 0),
            true);

  // Merged operations
  ASSERT_EQ(android::base::ReadFullyAtOffset(base_fd_, orig_buffer_.get(),
                                             writer->GetBlockSize(), 0),
            true);
  ASSERT_EQ(android::base::ReadFullyAtOffset(
                base_fd_, (char*)orig_buffer_.get() + writer->GetBlockSize(),
                size_, 0),
            true);
}

void SnapuserdTest::CreateCowDeviceOrderedOpsInverted() {
  unique_fd rnd_fd;
  loff_t offset = 0;

  auto writer = CreateCowDeviceInternal();
  ASSERT_NE(writer, nullptr);

  rnd_fd.reset(open("/dev/random", O_RDONLY));
  ASSERT_TRUE(rnd_fd > 0);

  std::unique_ptr<uint8_t[]> random_buffer_1_ =
      std::make_unique<uint8_t[]>(size_);

  // Fill random data
  for (size_t j = 0; j < (size_ / 1_MiB); j++) {
    ASSERT_EQ(ReadFullyAtOffset(rnd_fd, (char*)random_buffer_1_.get() + offset,
                                1_MiB, 0),
              true);

    offset += 1_MiB;
  }

  size_t num_blocks = size_ / writer->GetBlockSize();
  size_t blk_end_copy = num_blocks * 3;
  size_t source_blk = num_blocks - 1;
  size_t blk_src_copy = blk_end_copy - 1;
  uint16_t xor_offset = 5;

  size_t x = num_blocks;
  while (1) {
    ASSERT_TRUE(writer->AddCopy(source_blk, blk_src_copy));
    x -= 1;
    if (x == 0) {
      break;
    }
    source_blk -= 1;
    blk_src_copy -= 1;
  }

  for (size_t i = num_blocks; i > 0; i--) {
    ASSERT_TRUE(writer->AddXorBlocks(
        num_blocks + i - 1,
        &random_buffer_1_.get()[writer->GetBlockSize() * (i - 1)],
        writer->GetBlockSize(), 2 * num_blocks + i - 1, xor_offset));
  }
  // Flush operations
  ASSERT_TRUE(writer->Finalize());
  // Construct the buffer required for validation
  orig_buffer_ = std::make_unique<uint8_t[]>(total_base_size_);
  // Read the entire base device
  ASSERT_EQ(android::base::ReadFullyAtOffset(base_fd_, orig_buffer_.get(),
                                             total_base_size_, 0),
            true);
  // Merged Buffer
  memmove(orig_buffer_.get(), (char*)orig_buffer_.get() + 2 * size_, size_);
  memmove(orig_buffer_.get() + size_,
          (char*)orig_buffer_.get() + 2 * size_ + xor_offset, size_);
  for (int i = 0; i < size_; i++) {
    orig_buffer_.get()[size_ + i] ^= random_buffer_1_.get()[i];
  }
}

void SnapuserdTest::CreateCowDeviceOrderedOps() {
  unique_fd rnd_fd;
  loff_t offset = 0;

  auto writer = CreateCowDeviceInternal();
  ASSERT_NE(writer, nullptr);

  rnd_fd.reset(open("/dev/random", O_RDONLY));
  ASSERT_TRUE(rnd_fd > 0);

  std::unique_ptr<uint8_t[]> random_buffer_1_ =
      std::make_unique<uint8_t[]>(size_);

  // Fill random data
  for (size_t j = 0; j < (size_ / 1_MiB); j++) {
    ASSERT_EQ(ReadFullyAtOffset(rnd_fd, (char*)random_buffer_1_.get() + offset,
                                1_MiB, 0),
              true);

    offset += 1_MiB;
  }
  memset(random_buffer_1_.get(), 0, size_);

  size_t num_blocks = size_ / writer->GetBlockSize();
  size_t x = num_blocks;
  size_t source_blk = 0;
  size_t blk_src_copy = 2 * num_blocks;
  uint16_t xor_offset = 5;

  while (1) {
    ASSERT_TRUE(writer->AddCopy(source_blk, blk_src_copy));

    x -= 1;
    if (x == 0) {
      break;
    }
    source_blk += 1;
    blk_src_copy += 1;
  }

  ASSERT_TRUE(writer->AddXorBlocks(num_blocks, random_buffer_1_.get(), size_,
                                   2 * num_blocks, xor_offset));
  // Flush operations
  ASSERT_TRUE(writer->Finalize());
  // Construct the buffer required for validation
  orig_buffer_ = std::make_unique<uint8_t[]>(total_base_size_);
  // Read the entire base device
  ASSERT_EQ(android::base::ReadFullyAtOffset(base_fd_, orig_buffer_.get(),
                                             total_base_size_, 0),
            true);
  // Merged Buffer
  memmove(orig_buffer_.get(), (char*)orig_buffer_.get() + 2 * size_, size_);
  memmove(orig_buffer_.get() + size_,
          (char*)orig_buffer_.get() + 2 * size_ + xor_offset, size_);
  for (int i = 0; i < size_; i++) {
    orig_buffer_.get()[size_ + i] ^= random_buffer_1_.get()[i];
  }
}

void SnapuserdTest::InitCowDevice() {
<<<<<<< HEAD
  bool use_iouring = true;
  if (FLAGS_force_config == "iouring_disabled") {
    use_iouring = false;
  }

  auto factory = harness_->GetBlockServerFactory();
  auto opener = factory->CreateOpener(system_device_ctrl_name_);
  auto handler = handlers_->AddHandler(
      system_device_ctrl_name_, cow_system_->path, base_dev_->GetPath(),
      base_dev_->GetPath(), opener, 1, use_iouring, false);
  ASSERT_NE(handler, nullptr);
  ASSERT_NE(handler->snapuserd(), nullptr);
#ifdef __ANDROID__
  ASSERT_NE(handler->snapuserd()->GetNumSectors(), 0);
#endif
  ||||||| bool use_iouring = true;
  if (FLAGS_force_config == "iouring_disabled") {
    use_iouring = false;
  }

  auto handler = handlers_.AddHandler(
      system_device_ctrl_name_, cow_system_->path, base_loop_->device(),
      base_loop_->device(), 1, use_iouring, false);
  ASSERT_NE(handler, nullptr);
  ASSERT_NE(handler->snapuserd(), nullptr);
  ASSERT_NE(handler->snapuserd()->GetNumSectors(), 0);
=======
  uint64_t num_sectors =
      client_->InitDmUserCow(system_device_ctrl_name_, cow_system_->path,
                             base_loop_->device(), base_loop_->device());
  ASSERT_NE(num_sectors, 0);
>>>>>>> 8cbbc2dc926e131322853222f434d949aed12adc
}

void SnapuserdTest::CreateUserDevice() {
  auto dev_sz = base_dev_->GetSize();
  ASSERT_NE(dev_sz, 0);

  cow_num_sectors_ = dev_sz >> 9;

  dmuser_dev_ = harness_->CreateUserDevice(
      system_device_name_, system_device_ctrl_name_, cow_num_sectors_);
  ASSERT_NE(dmuser_dev_, nullptr);
}

void SnapuserdTestBase::SetDeviceControlName() {
  system_device_name_.clear();
  system_device_ctrl_name_.clear();

  std::string str(cow_system_->path);
  std::size_t found = str.find_last_of("/\\");
  ASSERT_NE(found, std::string::npos);
  system_device_name_ = str.substr(found + 1);

  system_device_ctrl_name_ = system_device_name_ + "-ctrl";
}

void SnapuserdTest::InitDaemon() {
<<<<<<< HEAD
  ASSERT_TRUE(handlers_->StartHandler(system_device_ctrl_name_));
|||||||
  ASSERT_TRUE(handlers_.StartHandler(system_device_ctrl_name_));
=======
  bool ok = client_->AttachDmUser(system_device_ctrl_name_);
  ASSERT_TRUE(ok);
>>>>>>> 8cbbc2dc926e131322853222f434d949aed12adc
}

void SnapuserdTest::CheckMergeCompletion() {
  while (true) {
<<<<<<< HEAD
    double percentage = handlers_->GetMergePercentage();
|||||||
    double percentage = handlers_.GetMergePercentage();
=======
    double percentage = client_->GetMergePercent();
>>>>>>> 8cbbc2dc926e131322853222f434d949aed12adc
    if ((int)percentage == 100) {
      break;
    }

    std::this_thread::sleep_for(1s);
  }
}

void SnapuserdTest::SetupImpl() {
  ASSERT_NO_FATAL_FAILURE(CreateBaseDevice());
  ASSERT_NO_FATAL_FAILURE(CreateCowDevice());

  SetDeviceControlName();

<<<<<<< HEAD
  ASSERT_NO_FATAL_FAILURE(CreateUserDevice());
  ASSERT_NO_FATAL_FAILURE(InitCowDevice());
  ASSERT_NO_FATAL_FAILURE(InitDaemon());
|||||||
  CreateDmUserDevice();
  InitCowDevice();
  InitDaemon();

  setup_ok_ = true;
=======
  StartSnapuserdDaemon();

  CreateDmUserDevice();
  InitCowDevice();
  InitDaemon();

  setup_ok_ = true;
>>>>>>> 8cbbc2dc926e131322853222f434d949aed12adc
}

bool SnapuserdTest::Merge() {
  if (!StartMerge()) {
    return false;
  }
  CheckMergeCompletion();
  return true;
}

bool SnapuserdTest::StartMerge() {
<<<<<<< HEAD
  return handlers_->InitiateMerge(system_device_ctrl_name_);
|||||||
  ASSERT_TRUE(handlers_.InitiateMerge(system_device_ctrl_name_));
=======
  bool ok = client_->InitiateMerge(system_device_ctrl_name_);
  ASSERT_TRUE(ok);
>>>>>>> 8cbbc2dc926e131322853222f434d949aed12adc
}

void SnapuserdTest::ValidateMerge() {
  merged_buffer_ = std::make_unique<uint8_t[]>(total_base_size_);
  ASSERT_EQ(android::base::ReadFullyAtOffset(base_fd_, merged_buffer_.get(),
                                             total_base_size_, 0),
            true);
  ASSERT_EQ(memcmp(merged_buffer_.get(), orig_buffer_.get(), total_base_size_),
            0);
}

void SnapuserdTest::SimulateDaemonRestart() {
  ASSERT_NO_FATAL_FAILURE(Shutdown());
  std::this_thread::sleep_for(500ms);
  SetDeviceControlName();
<<<<<<< HEAD
  ASSERT_NO_FATAL_FAILURE(CreateUserDevice());
  ASSERT_NO_FATAL_FAILURE(InitCowDevice());
  ASSERT_NO_FATAL_FAILURE(InitDaemon());
|||||||
  CreateDmUserDevice();
  InitCowDevice();
  InitDaemon();
=======
  StartSnapuserdDaemon();
  CreateDmUserDevice();
  InitCowDevice();
  InitDaemon();
>>>>>>> 8cbbc2dc926e131322853222f434d949aed12adc
}

void SnapuserdTest::MergeInterruptRandomly(int max_duration) {
  std::srand(std::time(nullptr));
  ASSERT_TRUE(StartMerge());

  for (int i = 0; i < 20; i++) {
    int duration = std::rand() % max_duration;
    std::this_thread::sleep_for(std::chrono::milliseconds(duration));
    ASSERT_NO_FATAL_FAILURE(SimulateDaemonRestart());
    ASSERT_TRUE(StartMerge());
  }

  ASSERT_NO_FATAL_FAILURE(SimulateDaemonRestart());
  ASSERT_TRUE(Merge());
}

void SnapuserdTest::MergeInterruptFixed(int duration) {
  ASSERT_TRUE(StartMerge());

  for (int i = 0; i < 25; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(duration));
    ASSERT_NO_FATAL_FAILURE(SimulateDaemonRestart());
    ASSERT_TRUE(StartMerge());
  }

  ASSERT_NO_FATAL_FAILURE(SimulateDaemonRestart());
  ASSERT_TRUE(Merge());
}

void SnapuserdTest::MergeInterrupt() {
  // Interrupt merge at various intervals
  ASSERT_TRUE(StartMerge());
  std::this_thread::sleep_for(250ms);
  ASSERT_NO_FATAL_FAILURE(SimulateDaemonRestart());

  ASSERT_TRUE(StartMerge());
  std::this_thread::sleep_for(250ms);
  ASSERT_NO_FATAL_FAILURE(SimulateDaemonRestart());

  ASSERT_TRUE(StartMerge());
  std::this_thread::sleep_for(150ms);
  ASSERT_NO_FATAL_FAILURE(SimulateDaemonRestart());

  ASSERT_TRUE(StartMerge());
  std::this_thread::sleep_for(100ms);
  ASSERT_NO_FATAL_FAILURE(SimulateDaemonRestart());

  ASSERT_TRUE(StartMerge());
  std::this_thread::sleep_for(800ms);
  ASSERT_NO_FATAL_FAILURE(SimulateDaemonRestart());

  ASSERT_TRUE(StartMerge());
  std::this_thread::sleep_for(600ms);
  ASSERT_NO_FATAL_FAILURE(SimulateDaemonRestart());

  ASSERT_TRUE(Merge());
}

TEST_F(HandlerTest, ReadUnalignedSize) {
  std::unique_ptr<uint8_t[]> snapuserd_buffer =
      std::make_unique<uint8_t[]>(SECTOR_SIZE);

  ASSERT_TRUE(ReadSectors(0, SECTOR_SIZE, snapuserd_buffer.get()));
  ASSERT_EQ(memcmp(snapuserd_buffer.get(), orig_buffer_.get(), SECTOR_SIZE), 0);
}

class HandlerTest : public SnapuserdTestBase {
 protected:
  void SetUp() override;
  void TearDown() override;

  AssertionResult ReadSectors(sector_t sector, uint64_t size, void* buffer);

  TestBlockServerFactory factory_;
  std::shared_ptr<TestBlockServerOpener> opener_;
  std::shared_ptr<SnapshotHandler> handler_;
  std::unique_ptr<ReadWorker> read_worker_;
  TestBlockServer* block_server_;
  std::future<bool> handler_thread_;
};

void HandlerTest::SetUp() {
  ASSERT_NO_FATAL_FAILURE(SnapuserdTestBase::SetUp());
  ASSERT_NO_FATAL_FAILURE(CreateBaseDevice());
  ASSERT_NO_FATAL_FAILURE(CreateCowDevice());
  ASSERT_NO_FATAL_FAILURE(SetDeviceControlName());

  opener_ = factory_.CreateTestOpener(system_device_ctrl_name_);
  ASSERT_NE(opener_, nullptr);

  handler_ = std::make_shared<SnapshotHandler>(
      system_device_ctrl_name_, cow_system_->path, base_dev_->GetPath(),
      base_dev_->GetPath(), opener_, 1, false, false);
  ASSERT_TRUE(handler_->InitCowDevice());
  ASSERT_TRUE(handler_->InitializeWorkers());

  read_worker_ = std::make_unique<ReadWorker>(
      cow_system_->path, base_dev_->GetPath(), system_device_ctrl_name_,
      base_dev_->GetPath(), handler_->GetSharedPtr(), opener_);
  ASSERT_TRUE(read_worker_->Init());
  block_server_ = static_cast<TestBlockServer*>(read_worker_->block_server());

  handler_thread_ =
      std::async(std::launch::async, &SnapshotHandler::Start, handler_.get());
}

void HandlerTest::TearDown() {
  ASSERT_TRUE(factory_.DeleteQueue(system_device_ctrl_name_));
  ASSERT_TRUE(handler_thread_.get());
  SnapuserdTestBase::TearDown();
}

AssertionResult HandlerTest::ReadSectors(sector_t sector, uint64_t size,
                                         void* buffer) {
  if (!read_worker_->RequestSectors(sector, size)) {
    return AssertionFailure() << "request sectors failed";
  }

  std::string result = std::move(block_server_->sent_io());
  if (result.size() != size) {
    return AssertionFailure() << "size mismatch in result, got "
                              << result.size() << ", expected " << size;
  }

  memcpy(buffer, result.data(), size);
  return AssertionSuccess();
}

TEST_F(HandlerTest, ReadUnalignedSize) {
  std::unique_ptr<uint8_t[]> snapuserd_buffer =
      std::make_unique<uint8_t[]>(SECTOR_SIZE);

  ASSERT_TRUE(ReadSectors(0, SECTOR_SIZE, snapuserd_buffer.get()));
  ASSERT_EQ(memcmp(snapuserd_buffer.get(), orig_buffer_.get(), SECTOR_SIZE), 0);
}

TEST_F(HandlerTest, ReadUnalignedSize) {
  std::unique_ptr<uint8_t[]> snapuserd_buffer =
      std::make_unique<uint8_t[]>(SECTOR_SIZE);

  ASSERT_TRUE(ReadSectors(0, SECTOR_SIZE, snapuserd_buffer.get()));
  ASSERT_EQ(memcmp(snapuserd_buffer.get(), orig_buffer_.get(), SECTOR_SIZE), 0);
}

TEST_F(HandlerTest, ReadUnalignedSize) {
  std::unique_ptr<uint8_t[]> snapuserd_buffer =
      std::make_unique<uint8_t[]>(SECTOR_SIZE);

  ASSERT_TRUE(ReadSectors(0, SECTOR_SIZE, snapuserd_buffer.get()));
  ASSERT_EQ(memcmp(snapuserd_buffer.get(), orig_buffer_.get(), SECTOR_SIZE), 0);
}

TEST_F(HandlerTest, ReadUnalignedSize) {
  std::unique_ptr<uint8_t[]> snapuserd_buffer =
      std::make_unique<uint8_t[]>(SECTOR_SIZE);

  ASSERT_TRUE(ReadSectors(0, SECTOR_SIZE, snapuserd_buffer.get()));
  ASSERT_EQ(memcmp(snapuserd_buffer.get(), orig_buffer_.get(), SECTOR_SIZE), 0);
}

TEST_F(HandlerTest, ReadUnalignedSize) {
  std::unique_ptr<uint8_t[]> snapuserd_buffer =
      std::make_unique<uint8_t[]>(SECTOR_SIZE);

  ASSERT_TRUE(ReadSectors(0, SECTOR_SIZE, snapuserd_buffer.get()));
  ASSERT_EQ(memcmp(snapuserd_buffer.get(), orig_buffer_.get(), SECTOR_SIZE), 0);
}

TEST_F(HandlerTest, ReadUnalignedSize) {
  std::unique_ptr<uint8_t[]> snapuserd_buffer =
      std::make_unique<uint8_t[]>(SECTOR_SIZE);

  ASSERT_TRUE(ReadSectors(0, SECTOR_SIZE, snapuserd_buffer.get()));
  ASSERT_EQ(memcmp(snapuserd_buffer.get(), orig_buffer_.get(), SECTOR_SIZE), 0);
}

TEST_F(HandlerTest, ReadUnalignedSize) {
  std::unique_ptr<uint8_t[]> snapuserd_buffer =
      std::make_unique<uint8_t[]>(SECTOR_SIZE);

  ASSERT_TRUE(ReadSectors(0, SECTOR_SIZE, snapuserd_buffer.get()));
  ASSERT_EQ(memcmp(snapuserd_buffer.get(), orig_buffer_.get(), SECTOR_SIZE), 0);
}

TEST_F(HandlerTest, ReadUnalignedSize) {
  std::unique_ptr<uint8_t[]> snapuserd_buffer =
      std::make_unique<uint8_t[]>(SECTOR_SIZE);

  ASSERT_TRUE(ReadSectors(0, SECTOR_SIZE, snapuserd_buffer.get()));
  ASSERT_EQ(memcmp(snapuserd_buffer.get(), orig_buffer_.get(), SECTOR_SIZE), 0);
}

TEST_F(HandlerTest, ReadUnalignedSize) {
  std::unique_ptr<uint8_t[]> snapuserd_buffer =
      std::make_unique<uint8_t[]>(SECTOR_SIZE);

  ASSERT_TRUE(ReadSectors(0, SECTOR_SIZE, snapuserd_buffer.get()));
  ASSERT_EQ(memcmp(snapuserd_buffer.get(), orig_buffer_.get(), SECTOR_SIZE), 0);
}

TEST_F(HandlerTest, ReadUnalignedSize) {
  std::unique_ptr<uint8_t[]> snapuserd_buffer =
      std::make_unique<uint8_t[]>(SECTOR_SIZE);

  ASSERT_TRUE(ReadSectors(0, SECTOR_SIZE, snapuserd_buffer.get()));
  ASSERT_EQ(memcmp(snapuserd_buffer.get(), orig_buffer_.get(), SECTOR_SIZE), 0);
}

void SnapuserdTestBase::CreateCowDevice() {
  unique_fd rnd_fd;
  loff_t offset = 0;

  auto writer = CreateCowDeviceInternal();
  ASSERT_NE(writer, nullptr);

  rnd_fd.reset(open("/dev/random", O_RDONLY));
  ASSERT_TRUE(rnd_fd > 0);

  std::unique_ptr<uint8_t[]> random_buffer_1_ =
      std::make_unique<uint8_t[]>(size_);

  // Fill random data
  for (size_t j = 0; j < (size_ / 1_MiB); j++) {
    ASSERT_EQ(ReadFullyAtOffset(rnd_fd, (char*)random_buffer_1_.get() + offset,
                                1_MiB, 0),
              true);

    offset += 1_MiB;
  }

  size_t num_blocks = size_ / writer->GetBlockSize();
  size_t blk_end_copy = num_blocks * 2;
  size_t source_blk = num_blocks - 1;
  size_t blk_src_copy = blk_end_copy - 1;

  uint32_t sequence[num_blocks * 2];
  // Sequence for Copy ops
  for (int i = 0; i < num_blocks; i++) {
    sequence[i] = num_blocks - 1 - i;
  }
  // Sequence for Xor ops
  for (int i = 0; i < num_blocks; i++) {
    sequence[num_blocks + i] = 5 * num_blocks - 1 - i;
  }
  ASSERT_TRUE(writer->AddSequenceData(2 * num_blocks, sequence));

  size_t x = num_blocks;
  while (1) {
    ASSERT_TRUE(writer->AddCopy(source_blk, blk_src_copy));
    x -= 1;
    if (x == 0) {
      break;
    }
    source_blk -= 1;
    blk_src_copy -= 1;
  }

  source_blk = num_blocks;
  blk_src_copy = blk_end_copy;

  ASSERT_TRUE(writer->AddRawBlocks(source_blk, random_buffer_1_.get(), size_));

  size_t blk_zero_copy_start = source_blk + num_blocks;
  size_t blk_zero_copy_end = blk_zero_copy_start + num_blocks;

  ASSERT_TRUE(writer->AddZeroBlocks(blk_zero_copy_start, num_blocks));

  size_t blk_random2_replace_start = blk_zero_copy_end;

  ASSERT_TRUE(writer->AddRawBlocks(blk_random2_replace_start,
                                   random_buffer_1_.get(), size_));

  size_t blk_xor_start = blk_random2_replace_start + num_blocks;
  size_t xor_offset = BLOCK_SZ / 2;
  ASSERT_TRUE(writer->AddXorBlocks(blk_xor_start, random_buffer_1_.get(), size_,
                                   num_blocks, xor_offset));

  // Flush operations
  ASSERT_TRUE(writer->Finalize());
  // Construct the buffer required for validation
  orig_buffer_ = std::make_unique<uint8_t[]>(total_base_size_);
  std::string zero_buffer(size_, 0);
  ASSERT_EQ(android::base::ReadFullyAtOffset(base_fd_, orig_buffer_.get(),
                                             size_, size_),
            true);
  memcpy((char*)orig_buffer_.get() + size_, random_buffer_1_.get(), size_);
  memcpy((char*)orig_buffer_.get() + (size_ * 2), (void*)zero_buffer.c_str(),
         size_);
  memcpy((char*)orig_buffer_.get() + (size_ * 3), random_buffer_1_.get(),
         size_);
  ASSERT_EQ(
      android::base::ReadFullyAtOffset(base_fd_, &orig_buffer_.get()[size_ * 4],
                                       size_, size_ + xor_offset),
      true);
  for (int i = 0; i < size_; i++) {
    orig_buffer_.get()[(size_ * 4) + i] =
        (uint8_t)(orig_buffer_.get()[(size_ * 4) + i] ^
                  random_buffer_1_.get()[i]);
  }
}

}  // namespace snapshot

}  // namespace android

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);

  gflags::ParseCommandLineFlags(&argc, &argv, false);

  android::base::SetProperty("ctl.stop", "snapuserd");

  if (FLAGS_force_config == "iouring_disabled") {
    if (!android::base::SetProperty("snapuserd.test.io_uring.force_disable",
                                    "1")) {
      return testing::AssertionFailure()
             << "Failed to disable property: snapuserd.test.io_uring.disabled";
    }
  }

  int ret = RUN_ALL_TESTS();

  if (FLAGS_force_config == "iouring_disabled") {
    android::base::SetProperty("snapuserd.test.io_uring.force_disable", "0");
  }

  return ret;
}