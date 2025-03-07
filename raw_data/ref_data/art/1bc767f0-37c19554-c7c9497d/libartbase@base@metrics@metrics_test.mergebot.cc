/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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
}  // namespace metrics

}  // namespace art