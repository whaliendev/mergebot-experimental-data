#include "artd.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>
#include "aidl/com/android/server/art/ArtConstants.h"
#include "aidl/com/android/server/art/BnArtd.h"
#include "android-base/collections.h"
#include "android-base/errors.h"
#include "android-base/file.h"
#include "android-base/logging.h"
#include "android-base/parseint.h"
#include "android-base/result-gmock.h"
#include "android-base/result.h"
#include "android-base/scopeguard.h"
#include "android-base/strings.h"
#include "android/binder_auto_utils.h"
#include "android/binder_status.h"
#include "base/array_ref.h"
#include "base/common_art_test.h"
#include "base/macros.h"
#include "exec_utils.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "oat_file.h"
#include "path_utils.h"
#include "profman/profman_result.h"
#include "testing.h"
#include "tools/system_properties.h"
#include "ziparchive/zip_writer.h"
namespace art {
namespace artd {
namespace {
using ::aidl::com::android::server::art::ArtConstants;
using ::aidl::com::android::server::art::ArtdDexoptResult;
using ::aidl::com::android::server::art::ArtifactsPath;
using ::aidl::com::android::server::art::CopyAndRewriteProfileResult;
using ::aidl::com::android::server::art::DexMetadataPath;
using ::aidl::com::android::server::art::DexoptOptions;
using ::aidl::com::android::server::art::FileVisibility;
using ::aidl::com::android::server::art::FsPermission;
using ::aidl::com::android::server::art::IArtdCancellationSignal;
using ::aidl::com::android::server::art::OutputArtifacts;
using ::aidl::com::android::server::art::OutputProfile;
using ::aidl::com::android::server::art::PriorityClass;
using ::aidl::com::android::server::art::ProfilePath;
using ::aidl::com::android::server::art::VdexPath;
using ::android::base::Append;
using ::android::base::Error;
using ::android::base::make_scope_guard;
using ::android::base::ParseInt;
using ::android::base::ReadFdToString;
using ::android::base::ReadFileToString;
using ::android::base::Result;
using ::android::base::ScopeGuard;
using ::android::base::Split;
using ::android::base::WriteStringToFd;
using ::android::base::WriteStringToFile;
using ::android::base::testing::HasValue;
using ::testing::_;
using ::testing::AllOf;
using ::testing::AnyNumber;
using ::testing::AnyOf;
using ::testing::Contains;
using ::testing::ContainsRegex;
using ::testing::DoAll;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Matcher;
using ::testing::MockFunction;
using ::testing::Not;
using ::testing::Property;
using ::testing::ResultOf;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::UnorderedElementsAreArray;
using ::testing::WithArg;
using PrimaryCurProfilePath = ProfilePath::PrimaryCurProfilePath;
using PrimaryRefProfilePath = ProfilePath::PrimaryRefProfilePath;
using TmpProfilePath = ProfilePath::TmpProfilePath;
using std::literals::operator""s;
ScopeGuard<std::function<void()>> ScopedSetLogger(android::base::LogFunction&& logger) {
  android::base::LogFunction old_logger = android::base::SetLogger(std::move(logger));
  return make_scope_guard([old_logger = std::move(old_logger)]() mutable {
    android::base::SetLogger(std::move(old_logger));
  });
}
void CheckContent(const std::string& path, const std::string& expected_content) {
  std::string actual_content;
  ASSERT_TRUE(ReadFileToString(path, &actual_content));
  EXPECT_EQ(actual_content, expected_content);
}
void CheckOtherReadable(const std::string& path, bool expected_value) {
  EXPECT_EQ((std::filesystem::status(path).permissions() & std::filesystem::perms::others_read) !=
                std::filesystem::perms::none,
            expected_value);
}
Result<std::vector<std::string>> GetFlagValues(ArrayRef<const std::string> args,
                                               std::string_view flag) {
  std::vector<std::string> values;
  for (const std::string& arg : args) {
    std::string_view value(arg);
    if (android::base::ConsumePrefix(&value, flag)) {
      values.emplace_back(value);
    }
  }
  if (values.empty()) {
    return Errorf("Flag '{}' not found", flag);
  }
  return values;
}
Result<std::string> GetFlagValue(ArrayRef<const std::string> args, std::string_view flag) {
  std::vector<std::string> flag_values = OR_RETURN(GetFlagValues(args, flag));
  if (flag_values.size() > 1) {
    return Errorf("Duplicate flag '{}'", flag);
  }
  return flag_values[0];
}
void WriteToFdFlagImpl(const std::vector<std::string>& args,
                       std::string_view flag,
                       std::string_view content,
                       bool assume_empty) {
  std::string value = OR_FAIL(GetFlagValue(ArrayRef<const std::string>(args), flag));
  ASSERT_NE(value, "");
  int fd;
  ASSERT_TRUE(ParseInt(value, &fd));
  if (assume_empty) {
    ASSERT_EQ(lseek(fd, 0, SEEK_CUR), 0);
  } else {
    ASSERT_EQ(ftruncate(fd, 0), 0);
    ASSERT_EQ(lseek(fd, 0, SEEK_SET), 0);
  }
  ASSERT_TRUE(WriteStringToFd(content, fd));
}
ACTION_P(ClearAndWriteToFdFlag, flag, content) {
  WriteToFdFlagImpl(arg0, flag, content, false);
}
ACTION_P(ClearAndWriteToFdFlag, flag, content) {
  WriteToFdFlagImpl(arg0, flag, content, false);
}
template <typename... Args>
auto HasKeepFdsFor(Args&&... args) {
  std::vector<std::string_view> fd_flags;
  Append(fd_flags, std::forward<Args>(args)...);
  return HasKeepFdsForImpl(fd_flags);
}
template <typename... Args>
auto HasKeepFdsFor(Args&&... args) {
  std::vector<std::string_view> fd_flags;
  Append(fd_flags, std::forward<Args>(args)...);
  return HasKeepFdsForImpl(fd_flags);
}
class MockSystemProperties : public tools::SystemProperties {
 public:
  MOCK_METHOD(std::string, GetProperty, (const std::string& key), (const, override));
};
class MockExecUtils : public ExecUtils {
 public:
  ExecResult ExecAndReturnResult(const std::vector<std::string>& arg_vector,
                                 int,
                                 const ExecCallbacks& callbacks,
                                 ProcessStat* stat,
                                 std::string*) const override {
    Result<int> code = DoExecAndReturnCode(arg_vector, callbacks, stat);
    if (code.ok()) {
      return {.status = ExecResult::kExited, .exit_code = code.value()};
    }
    return {.status = ExecResult::kUnknown};
  }
  MOCK_METHOD(Result<int>,
              DoExecAndReturnCode,
              (const std::vector<std::string>& arg_vector,
               const ExecCallbacks& callbacks,
               ProcessStat* stat),
              (const));
};
class ArtdTest : public CommonArtTest {
 protected:
  void SetUp() override {
    CommonArtTest::SetUp();
    auto mock_props = std::make_unique<MockSystemProperties>();
    mock_props_ = mock_props.get();
    EXPECT_CALL(*mock_props_, GetProperty).Times(AnyNumber()).WillRepeatedly(Return(""));
    auto mock_exec_utils = std::make_unique<MockExecUtils>();
    mock_exec_utils_ = mock_exec_utils.get();
    artd_ = ndk::SharedRefBase::make<Artd>(std::move(mock_props),
                                           std::move(mock_exec_utils),
                                           mock_kill_.AsStdFunction(),
                                           mock_fstat_.AsStdFunction());
    scratch_dir_ = std::make_unique<ScratchDir>();
    scratch_path_ = scratch_dir_->GetPath();
    scratch_path_.resize(scratch_path_.length() - 1);
    TestOnlySetListRootDir(scratch_path_);
    ON_CALL(mock_fstat_, Call).WillByDefault(fstat);
    art_root_ = scratch_path_ + "/com.android.art";
    std::filesystem::create_directories(art_root_);
    setenv("ANDROID_ART_ROOT", art_root_.c_str(), 1);
    android_data_ = scratch_path_ + "/data";
    std::filesystem::create_directories(android_data_);
    setenv("ANDROID_DATA", android_data_.c_str(), 1);
    android_expand_ = scratch_path_ + "/mnt/expand";
    std::filesystem::create_directories(android_expand_);
    setenv("ANDROID_EXPAND", android_expand_.c_str(), 1);
    dex_file_ = scratch_path_ + "/a/b.apk";
    isa_ = "arm64";
    artifacts_path_ = ArtifactsPath{
        .dexPath = dex_file_,
        .isa = isa_,
        .isInDalvikCache = false,
    };
    struct stat st;
    ASSERT_EQ(stat(scratch_path_.c_str(), &st), 0);
    output_artifacts_ = OutputArtifacts{
        .artifactsPath = artifacts_path_,
        .permissionSettings =
            OutputArtifacts::PermissionSettings{
                .dirFsPermission =
                    FsPermission{
                        .uid = static_cast<int32_t>(st.st_uid),
                        .gid = static_cast<int32_t>(st.st_gid),
                        .isOtherReadable = true,
                        .isOtherExecutable = true,
                    },
                .fileFsPermission =
                    FsPermission{
                        .uid = static_cast<int32_t>(st.st_uid),
                        .gid = static_cast<int32_t>(st.st_gid),
                        .isOtherReadable = true,
                    },
            },
    };
    clc_1_ = GetTestDexFileName("Main");
    clc_2_ = GetTestDexFileName("Nested");
    class_loader_context_ = ART_FORMAT("PCL[{}:{}]", clc_1_, clc_2_);
    compiler_filter_ = "speed";
    tmp_profile_path_ =
        TmpProfilePath{.finalPath = PrimaryRefProfilePath{.packageName = "com.android.foo",
                                                          .profileName = "primary"},
                       .id = "12345"};
    profile_path_ = tmp_profile_path_;
    vdex_path_ = artifacts_path_;
    dm_path_ = DexMetadataPath{.dexPath = dex_file_};
    std::filesystem::create_directories(
        std::filesystem::path(OR_FATAL(BuildFinalProfilePath(tmp_profile_path_))).parent_path());
  }
  void TearDown() override {
    scratch_dir_.reset();
    CommonArtTest::TearDown();
  }
  void RunDexopt(Matcher<ndk::ScopedAStatus> status_matcher,
                 Matcher<ArtdDexoptResult> aidl_return_matcher = Field(&ArtdDexoptResult::cancelled,
                                                                       false),
                 std::shared_ptr<IArtdCancellationSignal> cancellation_signal = nullptr) {
    InitFilesBeforeDexopt();
    if (cancellation_signal == nullptr) {
      ASSERT_TRUE(artd_->createCancellationSignal(&cancellation_signal).isOk());
    }
    ArtdDexoptResult aidl_return;
    ndk::ScopedAStatus status = artd_->dexopt(output_artifacts_,
                                              dex_file_,
                                              isa_,
                                              class_loader_context_,
                                              compiler_filter_,
                                              profile_path_,
                                              vdex_path_,
                                              dm_path_,
                                              priority_class_,
                                              dexopt_options_,
                                              cancellation_signal,
                                              &aidl_return);
    ASSERT_THAT(status, std::move(status_matcher)) << status.getMessage();
    if (status.isOk()) {
      ASSERT_THAT(aidl_return, std::move(aidl_return_matcher));
    }
  }
  void RunDexopt(Matcher<ndk::ScopedAStatus> status_matcher,
                 Matcher<ArtdDexoptResult> aidl_return_matcher = Field(&ArtdDexoptResult::cancelled,
                                                                       false),
                 std::shared_ptr<IArtdCancellationSignal> cancellation_signal = nullptr) {
    InitFilesBeforeDexopt();
    if (cancellation_signal == nullptr) {
      ASSERT_TRUE(artd_->createCancellationSignal(&cancellation_signal).isOk());
    }
    ArtdDexoptResult aidl_return;
    ndk::ScopedAStatus status = artd_->dexopt(output_artifacts_,
                                              dex_file_,
                                              isa_,
                                              class_loader_context_,
                                              compiler_filter_,
                                              profile_path_,
                                              vdex_path_,
                                              dm_path_,
                                              priority_class_,
                                              dexopt_options_,
                                              cancellation_signal,
                                              &aidl_return);
    ASSERT_THAT(status, std::move(status_matcher)) << status.getMessage();
    if (status.isOk()) {
      ASSERT_THAT(aidl_return, std::move(aidl_return_matcher));
    }
  }
  OutputProfile >> RunCopyAndRewriteProfile() {
    OutputProfile dst{.profilePath = tmp_profile_path_,
                      .fsPermission = FsPermission{.uid = -1, .gid = -1}};
    dst.profilePath.id = "";
    dst.profilePath.tmpPath = "";
    CopyAndRewriteProfileResult result;
    ndk::ScopedAStatus status =
        artd_->copyAndRewriteProfile(tmp_profile_path_, &dst, dex_file_, &result);
    if constexpr (kExpectOk) {
      if (!status.isOk()) {
        return Error() << status.getMessage();
      }
      return std::make_pair(std::move(result), std::move(dst));
    } else {
      return std::make_pair(std::move(status), std::move(dst));
    }
  }
  template <bool kExpectOk = true>
  RunCopyAndRewriteProfileResult<kExpectOk> RunCopyAndRewriteEmbeddedProfile() {
    OutputProfile dst{.profilePath = tmp_profile_path_,
                      .fsPermission = FsPermission{.uid = -1, .gid = -1}};
    dst.profilePath.id = "";
    dst.profilePath.tmpPath = "";
    CopyAndRewriteProfileResult result;
    ndk::ScopedAStatus status = artd_->copyAndRewriteEmbeddedProfile(&dst, dex_file_, &result);
    if constexpr (kExpectOk) {
      if (!status.isOk()) {
        return Error() << status.getMessage();
      }
      return std::make_pair(std::move(result), std::move(dst));
    } else {
      return std::make_pair(std::move(status), std::move(dst));
    }
  }
  void CreateFile(const std::string& filename, const std::string& content = "") {
    std::filesystem::path path(filename);
    std::filesystem::create_directories(path.parent_path());
    ASSERT_TRUE(WriteStringToFile(content, filename));
  }
  void CreateZipWithSingleEntry(const std::string& filename,
                                const std::string& entry_name,
                                const std::string& content = "") {
    std::unique_ptr<File> file(OS::CreateEmptyFileWriteOnly(filename.c_str()));
    ASSERT_NE(file, nullptr);
    file->MarkUnchecked();
    ZipWriter writer(fdopen(file->Fd(), "wb"));
    ASSERT_EQ(writer.StartEntry(entry_name, 0), 0);
    ASSERT_EQ(writer.WriteBytes(content.c_str(), content.size()), 0);
    ASSERT_EQ(writer.FinishEntry(), 0);
    ASSERT_EQ(writer.Finish(), 0);
  }
  std::shared_ptr<Artd> artd_;
  std::unique_ptr<ScratchDir> scratch_dir_;
  std::string scratch_path_;
  std::string art_root_;
  std::string android_data_;
  std::string android_expand_;
  MockFunction<android::base::LogFunction> mock_logger_;
  ScopedUnsetEnvironmentVariable art_root_env_ = ScopedUnsetEnvironmentVariable("ANDROID_ART_ROOT");
  ScopedUnsetEnvironmentVariable android_data_env_ = ScopedUnsetEnvironmentVariable("ANDROID_DATA");
  ScopedUnsetEnvironmentVariable android_expand_env_ =
      ScopedUnsetEnvironmentVariable("ANDROID_EXPAND");
  MockSystemProperties* mock_props_;
  MockExecUtils* mock_exec_utils_;
  MockFunction<int(pid_t, int)> mock_kill_;
  MockFunction<int(int, struct stat*)> mock_fstat_;
  std::string dex_file_;
  std::string isa_;
  ArtifactsPath artifacts_path_;
  OutputArtifacts output_artifacts_;
  std::string clc_1_;
  std::string clc_2_;
  std::optional<std::string> class_loader_context_;
  std::string compiler_filter_;
  std::optional<VdexPath> vdex_path_;
  std::optional<DexMetadataPath> dm_path_;
  PriorityClass priority_class_ = PriorityClass::BACKGROUND;
  DexoptOptions dexopt_options_;
  std::optional<ProfilePath> profile_path_;
  TmpProfilePath tmp_profile_path_;
  bool dex_file_other_readable_ = true;
  bool profile_other_readable_ = true;
 private:
  void InitFilesBeforeDexopt() {
    CreateFile(dex_file_);
    std::filesystem::permissions(dex_file_,
                                 std::filesystem::perms::others_read,
                                 dex_file_other_readable_ ? std::filesystem::perm_options::add :
                                                            std::filesystem::perm_options::remove);
    if (vdex_path_.has_value()) {
      CreateFile(OR_FATAL(BuildVdexPath(vdex_path_.value())), "old_vdex");
    }
    if (dm_path_.has_value()) {
      CreateFile(OR_FATAL(BuildDexMetadataPath(dm_path_.value())));
    }
    if (profile_path_.has_value()) {
      std::string path = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));
      CreateFile(path);
      std::filesystem::permissions(path,
                                   std::filesystem::perms::others_read,
                                   profile_other_readable_ ? std::filesystem::perm_options::add :
                                                             std::filesystem::perm_options::remove);
    }
    std::string oat_path = OR_FATAL(BuildOatPath(artifacts_path_));
    CreateFile(oat_path, "old_oat");
    CreateFile(OatPathToVdexPath(oat_path), "old_vdex");
    CreateFile(OatPathToArtPath(oat_path), "old_art");
  }
};
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
static void SetDefaultResourceControlProps(MockSystemProperties* mock_props) {
  EXPECT_CALL(*mock_props, GetProperty("dalvik.vm.dex2oat-cpu-set")).WillRepeatedly(Return("0,2"));
  EXPECT_CALL(*mock_props, GetProperty("dalvik.vm.dex2oat-threads")).WillRepeatedly(Return("4"));
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
static void SetAllResourceControlProps(MockSystemProperties* mock_props) {
  EXPECT_CALL(*mock_props, GetProperty("dalvik.vm.dex2oat-cpu-set")).WillRepeatedly(Return("0,2"));
  EXPECT_CALL(*mock_props, GetProperty("dalvik.vm.dex2oat-threads")).WillRepeatedly(Return("4"));
  EXPECT_CALL(*mock_props, GetProperty("dalvik.vm.boot-dex2oat-cpu-set"))
      .WillRepeatedly(Return("0,1,2,3"));
  EXPECT_CALL(*mock_props, GetProperty("dalvik.vm.boot-dex2oat-threads"))
      .WillRepeatedly(Return("8"));
  EXPECT_CALL(*mock_props, GetProperty("dalvik.vm.restore-dex2oat-cpu-set"))
      .WillRepeatedly(Return("0,2,3"));
  EXPECT_CALL(*mock_props, GetProperty("dalvik.vm.restore-dex2oat-threads"))
      .WillRepeatedly(Return("6"));
  EXPECT_CALL(*mock_props, GetProperty("dalvik.vm.background-dex2oat-cpu-set"))
      .WillRepeatedly(Return("0"));
  EXPECT_CALL(*mock_props, GetProperty("dalvik.vm.background-dex2oat-threads"))
      .WillRepeatedly(Return("2"));
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
class ArtdGetVisibilityTest : public ArtdTest {
 protected:
  template <typename PathType>
  void TestGetVisibilityOtherReadable(Method<PathType> method,
                                      const PathType& input,
                                      const std::string& path) {
    CreateFile(path);
    std::filesystem::permissions(
        path, std::filesystem::perms::others_read, std::filesystem::perm_options::add);
    FileVisibility result;
    ASSERT_TRUE(((*artd_).*method)(input, &result).isOk());
    EXPECT_EQ(result, FileVisibility::OTHER_READABLE);
  }
  template <typename PathType>
  void TestGetVisibilityNotOtherReadable(Method<PathType> method,
                                         const PathType& input,
                                         const std::string& path) {
    CreateFile(path);
    std::filesystem::permissions(
        path, std::filesystem::perms::others_read, std::filesystem::perm_options::remove);
    FileVisibility result;
    ASSERT_TRUE(((*artd_).*method)(input, &result).isOk());
    EXPECT_EQ(result, FileVisibility::NOT_OTHER_READABLE);
  }
  template <typename PathType>
  void TestGetVisibilityNotFound(Method<PathType> method, const PathType& input) {
    FileVisibility result;
    ASSERT_TRUE(((*artd_).*method)(input, &result).isOk());
    EXPECT_EQ(result, FileVisibility::NOT_FOUND);
  }
  template <typename PathType>
  void TestGetVisibilityPermissionDenied(Method<PathType> method,
                                         const PathType& input,
                                         const std::string& path) {
    CreateFile(path);
    auto scoped_inaccessible = ScopedInaccessible(std::filesystem::path(path).parent_path());
    auto scoped_unroot = ScopedUnroot();
    FileVisibility result;
    ndk::ScopedAStatus status = ((*artd_).*method)(input, &result);
    EXPECT_FALSE(status.isOk());
    EXPECT_EQ(status.getExceptionCode(), EX_SERVICE_SPECIFIC);
    EXPECT_THAT(status.getMessage(), HasSubstr("Failed to get status of"));
  }
};
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;
  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };
  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };
  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/*/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");
  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts({.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"},
                                   &aidl_return)
          .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"},
                      &aidl_return)
                  .isOk());
  ASSERT_TRUE(artd_
                  ->deleteRuntimeArtifacts(
                      {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                      &aidl_return)
                  .isOk());
  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }
  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}
}
}
}
