#include "metrics.h"
#include "base/macros.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "metrics_test.h"
#pragma clang diagnostic push
#pragma clang diagnostic error "-Wconversion"
namespace art {
namespace metrics {
using test::CounterValue;
using test::GetBuckets;
using test::TestBackendBase;
class MetricsTest : public testing::Test {};
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
TEST(CompilerReason, Name) {
  ASSERT_EQ(CompilationReasonName(CompilationReason::kUnknown), "unknown");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kFirstBoot), "first-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBootAfterOTA), "boot-after-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPostBoot), "post-boot");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstall), "install");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallFast), "install-fast");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulk), "install-bulk");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondary),
            "install-bulk-secondary");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkDowngraded),
            "install-bulk-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallBulkSecondaryDowngraded),
            "install-bulk-secondary-downgraded");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kBgDexopt), "bg-dexopt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kABOTA), "ab-ota");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInactive), "inactive");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kShared), "shared");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kInstallWithDexMetadata),
            "install-with-dex-metadata");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kPrebuilt), "prebuilt");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kCmdLine), "cmdline");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kError), "error");
  ASSERT_EQ(CompilationReasonName(CompilationReason::kVdex), "vdex");
}
}
}
