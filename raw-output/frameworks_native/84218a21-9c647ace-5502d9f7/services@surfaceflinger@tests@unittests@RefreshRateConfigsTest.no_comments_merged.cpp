#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextra"
#undef LOG_TAG
#define LOG_TAG "SchedulerUnittests"
#include <ftl/enum.h>
#include <gmock/gmock.h>
#include <log/log.h>
#include <ui/Size.h>
#include "DisplayHardware/HWC2.h"
#include "FpsOps.h"
#include "Scheduler/RefreshRateConfigs.h"
using namespace std::chrono_literals;
namespace android::scheduler {
namespace hal = android::hardware::graphics::composer::hal;
using RefreshRate = RefreshRateConfigs::RefreshRate;
using LayerVoteType = RefreshRateConfigs::LayerVoteType;
using LayerRequirement = RefreshRateConfigs::LayerRequirement;
class RefreshRateConfigsTest : public testing::Test {
protected:
    using GetBestRefreshRateInvocation = RefreshRateConfigs::GetBestRefreshRateInvocation;
    RefreshRateConfigsTest();
    ~RefreshRateConfigsTest();
    RefreshRate createRefreshRate(DisplayModePtr displayMode) {
        return {displayMode, RefreshRate::ConstructorTag(0)};
    }
    Fps findClosestKnownFrameRate(const RefreshRateConfigs& refreshRateConfigs, Fps frameRate) {
        return refreshRateConfigs.findClosestKnownFrameRate(frameRate);
    }
    std::vector<Fps> getKnownFrameRate(const RefreshRateConfigs& refreshRateConfigs) {
        return refreshRateConfigs.mKnownFrameRates;
    }
    RefreshRate getMinRefreshRateByPolicy(const RefreshRateConfigs& refreshRateConfigs) {
        std::lock_guard lock(refreshRateConfigs.mLock);
        return refreshRateConfigs.getMinRefreshRateByPolicyLocked();
    }
    RefreshRate getMinSupportedRefreshRate(const RefreshRateConfigs& refreshRateConfigs) {
        std::lock_guard lock(refreshRateConfigs.mLock);
        return *refreshRateConfigs.mMinSupportedRefreshRate;
    }
    RefreshRate getMaxSupportedRefreshRate(const RefreshRateConfigs& refreshRateConfigs) {
        std::lock_guard lock(refreshRateConfigs.mLock);
        return *refreshRateConfigs.mMaxSupportedRefreshRate;
    }
    void setLastBestRefreshRateInvocation(RefreshRateConfigs& refreshRateConfigs,
                                          const GetBestRefreshRateInvocation& invocation) {
        std::lock_guard lock(refreshRateConfigs.mLock);
        refreshRateConfigs.lastBestRefreshRateInvocation.emplace(
                GetBestRefreshRateInvocation(invocation));
    }
    std::optional<GetBestRefreshRateInvocation> getLastBestRefreshRateInvocation(
            const RefreshRateConfigs& refreshRateConfigs) {
        std::lock_guard lock(refreshRateConfigs.mLock);
        return refreshRateConfigs.lastBestRefreshRateInvocation;
    }
    static inline const DisplayModeId HWC_CONFIG_ID_60 = DisplayModeId(0);
    static inline const DisplayModeId HWC_CONFIG_ID_90 = DisplayModeId(1);
    static inline const DisplayModeId HWC_CONFIG_ID_72 = DisplayModeId(2);
    static inline const DisplayModeId HWC_CONFIG_ID_120 = DisplayModeId(3);
    static inline const DisplayModeId HWC_CONFIG_ID_30 = DisplayModeId(4);
    static inline const DisplayModeId HWC_CONFIG_ID_25 = DisplayModeId(5);
    static inline const DisplayModeId HWC_CONFIG_ID_50 = DisplayModeId(6);
    static inline const DisplayModeId HWC_CONFIG_ID_24 = DisplayModeId(7);
    static inline const DisplayModeId HWC_CONFIG_ID_24_FRAC = DisplayModeId(8);
    static inline const DisplayModeId HWC_CONFIG_ID_30_FRAC = DisplayModeId(9);
    static inline const DisplayModeId HWC_CONFIG_ID_60_FRAC = DisplayModeId(10);
<<<<<<< HEAD
    DisplayModePtr mConfig60 = createDisplayMode(HWC_CONFIG_ID_60, 0, (60_Hz).getPeriodNsecs());
    DisplayModePtr mConfig60Frac =
            createDisplayMode(HWC_CONFIG_ID_60_FRAC, 0, (59.94_Hz).getPeriodNsecs());
    DisplayModePtr mConfig90 = createDisplayMode(HWC_CONFIG_ID_90, 0, (90_Hz).getPeriodNsecs());
||||||| 5502d9f7a2
    DisplayModePtr mConfig60 = createDisplayMode(HWC_CONFIG_ID_60, 0, Fps(60.0f).getPeriodNsecs());
    DisplayModePtr mConfig90 = createDisplayMode(HWC_CONFIG_ID_90, 0, Fps(90.0f).getPeriodNsecs());
=======
    DisplayModePtr mConfig60 = createDisplayMode(HWC_CONFIG_ID_60, 0, Fps(60.0f).getPeriodNsecs());
    DisplayModePtr mConfig60Frac =
            createDisplayMode(HWC_CONFIG_ID_60_FRAC, 0, Fps(59.94f).getPeriodNsecs());
    DisplayModePtr mConfig90 = createDisplayMode(HWC_CONFIG_ID_90, 0, Fps(90.0f).getPeriodNsecs());
>>>>>>> 9c647ace
    DisplayModePtr mConfig90DifferentGroup =
            createDisplayMode(HWC_CONFIG_ID_90, 1, (90_Hz).getPeriodNsecs());
    DisplayModePtr mConfig90DifferentResolution =
            createDisplayMode(HWC_CONFIG_ID_90, 0, (90_Hz).getPeriodNsecs(), ui::Size(111, 222));
    DisplayModePtr mConfig72 = createDisplayMode(HWC_CONFIG_ID_72, 0, (72_Hz).getPeriodNsecs());
    DisplayModePtr mConfig72DifferentGroup =
            createDisplayMode(HWC_CONFIG_ID_72, 1, (72_Hz).getPeriodNsecs());
    DisplayModePtr mConfig120 = createDisplayMode(HWC_CONFIG_ID_120, 0, (120_Hz).getPeriodNsecs());
    DisplayModePtr mConfig120DifferentGroup =
            createDisplayMode(HWC_CONFIG_ID_120, 1, (120_Hz).getPeriodNsecs());
    DisplayModePtr mConfig30 = createDisplayMode(HWC_CONFIG_ID_30, 0, (30_Hz).getPeriodNsecs());
    DisplayModePtr mConfig30DifferentGroup =
<<<<<<< HEAD
            createDisplayMode(HWC_CONFIG_ID_30, 1, (30_Hz).getPeriodNsecs());
    DisplayModePtr mConfig30Frac =
            createDisplayMode(HWC_CONFIG_ID_30_FRAC, 0, (29.97_Hz).getPeriodNsecs());
    DisplayModePtr mConfig25 = createDisplayMode(HWC_CONFIG_ID_25, 0, (25_Hz).getPeriodNsecs());
||||||| 5502d9f7a2
            createDisplayMode(HWC_CONFIG_ID_30, 1, Fps(30.0f).getPeriodNsecs());
=======
            createDisplayMode(HWC_CONFIG_ID_30, 1, Fps(30.0f).getPeriodNsecs());
    DisplayModePtr mConfig30Frac =
            createDisplayMode(HWC_CONFIG_ID_30_FRAC, 0, Fps(29.97f).getPeriodNsecs());
    DisplayModePtr mConfig25 = createDisplayMode(HWC_CONFIG_ID_25, 0, Fps(25.0f).getPeriodNsecs());
>>>>>>> 9c647ace
    DisplayModePtr mConfig25DifferentGroup =
<<<<<<< HEAD
            createDisplayMode(HWC_CONFIG_ID_25, 1, (25_Hz).getPeriodNsecs());
    DisplayModePtr mConfig50 = createDisplayMode(HWC_CONFIG_ID_50, 0, (50_Hz).getPeriodNsecs());
    DisplayModePtr mConfig24 = createDisplayMode(HWC_CONFIG_ID_24, 0, (24_Hz).getPeriodNsecs());
    DisplayModePtr mConfig24Frac =
            createDisplayMode(HWC_CONFIG_ID_24_FRAC, 0, (23.976_Hz).getPeriodNsecs());
||||||| 5502d9f7a2
            createDisplayMode(HWC_CONFIG_ID_25, 1, Fps(25.0f).getPeriodNsecs());
    DisplayModePtr mConfig50 = createDisplayMode(HWC_CONFIG_ID_50, 0, Fps(50.0f).getPeriodNsecs());
=======
            createDisplayMode(HWC_CONFIG_ID_25, 1, Fps(25.0f).getPeriodNsecs());
    DisplayModePtr mConfig50 = createDisplayMode(HWC_CONFIG_ID_50, 0, Fps(50.0f).getPeriodNsecs());
    DisplayModePtr mConfig24 = createDisplayMode(HWC_CONFIG_ID_24, 0, Fps(24.0f).getPeriodNsecs());
    DisplayModePtr mConfig24Frac =
            createDisplayMode(HWC_CONFIG_ID_24_FRAC, 0, Fps(23.976f).getPeriodNsecs());
>>>>>>> 9c647ace
    DisplayModes m60OnlyConfigDevice = {mConfig60};
    DisplayModes m60_90Device = {mConfig60, mConfig90};
    DisplayModes m60_90DeviceWithDifferentGroups = {mConfig60, mConfig90DifferentGroup};
    DisplayModes m60_90DeviceWithDifferentResolutions = {mConfig60, mConfig90DifferentResolution};
    DisplayModes m60_72_90Device = {mConfig60, mConfig90, mConfig72};
    DisplayModes m60_90_72_120Device = {mConfig60, mConfig90, mConfig72, mConfig120};
    DisplayModes m30_60_72_90_120Device = {mConfig60, mConfig90, mConfig72, mConfig120, mConfig30};
    DisplayModes m30_60Device = {mConfig60, mConfig90DifferentGroup, mConfig72DifferentGroup,
                                 mConfig120DifferentGroup, mConfig30};
    DisplayModes m30_60_72_90Device = {mConfig60, mConfig90, mConfig72, mConfig120DifferentGroup,
                                       mConfig30};
    DisplayModes m30_60_90Device = {mConfig60, mConfig90, mConfig72DifferentGroup,
                                    mConfig120DifferentGroup, mConfig30};
    DisplayModes m25_30_50_60Device = {mConfig60,
                                       mConfig90,
                                       mConfig72DifferentGroup,
                                       mConfig120DifferentGroup,
                                       mConfig30DifferentGroup,
                                       mConfig25DifferentGroup,
                                       mConfig50};
    DisplayModes m60_120Device = {mConfig60, mConfig120};
    DisplayModes m24_25_30_50_60WithFracDevice = {mConfig24, mConfig24Frac, mConfig25,
                                                  mConfig30, mConfig30Frac, mConfig50,
                                                  mConfig60, mConfig60Frac};
    RefreshRate mExpected60Config = {mConfig60, RefreshRate::ConstructorTag(0)};
    RefreshRate mExpectedAlmost60Config = {createDisplayMode(HWC_CONFIG_ID_60, 0, 16666665),
                                           RefreshRate::ConstructorTag(0)};
    RefreshRate mExpected90Config = {mConfig90, RefreshRate::ConstructorTag(0)};
    RefreshRate mExpected90DifferentGroupConfig = {mConfig90DifferentGroup,
                                                   RefreshRate::ConstructorTag(0)};
    RefreshRate mExpected90DifferentResolutionConfig = {mConfig90DifferentResolution,
                                                        RefreshRate::ConstructorTag(0)};
    RefreshRate mExpected72Config = {mConfig72, RefreshRate::ConstructorTag(0)};
    RefreshRate mExpected30Config = {mConfig30, RefreshRate::ConstructorTag(0)};
    RefreshRate mExpected120Config = {mConfig120, RefreshRate::ConstructorTag(0)};
    DisplayModePtr createDisplayMode(DisplayModeId modeId, int32_t group, int64_t vsyncPeriod,
                                     ui::Size resolution = ui::Size());
};
using Builder = DisplayMode::Builder;
RefreshRateConfigsTest::RefreshRateConfigsTest() {
    const ::testing::TestInfo* const test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
    ALOGD("**** Setting up for %s.%s\n", test_info->test_case_name(), test_info->name());
}
RefreshRateConfigsTest::~RefreshRateConfigsTest() {
    const ::testing::TestInfo* const test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
    ALOGD("**** Tearing down after %s.%s\n", test_info->test_case_name(), test_info->name());
}
DisplayModePtr RefreshRateConfigsTest::createDisplayMode(DisplayModeId modeId, int32_t group,
                                                         int64_t vsyncPeriod, ui::Size resolution) {
    return DisplayMode::Builder(hal::HWConfigId(modeId.value()))
            .setId(modeId)
            .setPhysicalDisplayId(PhysicalDisplayId::fromPort(0))
            .setVsyncPeriod(int32_t(vsyncPeriod))
            .setGroup(group)
            .setHeight(resolution.height)
            .setWidth(resolution.width)
            .build();
}
namespace {
TEST_F(RefreshRateConfigsTest, oneDeviceConfig_SwitchingSupported) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60OnlyConfigDevice,
                                                                     HWC_CONFIG_ID_60);
}
TEST_F(RefreshRateConfigsTest, invalidPolicy) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60OnlyConfigDevice,
                                                                     HWC_CONFIG_ID_60);
    ASSERT_LT(refreshRateConfigs->setDisplayManagerPolicy({DisplayModeId(10), {60_Hz, 60_Hz}}), 0);
    ASSERT_LT(refreshRateConfigs->setDisplayManagerPolicy({HWC_CONFIG_ID_60, {20_Hz, 40_Hz}}), 0);
}
TEST_F(RefreshRateConfigsTest, twoDeviceConfigs_storesFullRefreshRateMap) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90Device,
                                                                     HWC_CONFIG_ID_60);
    const auto& minRate = getMinSupportedRefreshRate(*refreshRateConfigs);
    const auto& performanceRate = getMaxSupportedRefreshRate(*refreshRateConfigs);
    ASSERT_EQ(mExpected60Config, minRate);
    ASSERT_EQ(mExpected90Config, performanceRate);
    const auto& minRateByPolicy = getMinRefreshRateByPolicy(*refreshRateConfigs);
    const auto& performanceRateByPolicy = refreshRateConfigs->getMaxRefreshRateByPolicy();
    ASSERT_EQ(minRateByPolicy, minRate);
    ASSERT_EQ(performanceRateByPolicy, performanceRate);
}
TEST_F(RefreshRateConfigsTest, twoDeviceConfigs_storesFullRefreshRateMap_differentGroups) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90DeviceWithDifferentGroups,
                                                                     HWC_CONFIG_ID_60);
    const auto& minRate = getMinRefreshRateByPolicy(*refreshRateConfigs);
    const auto& performanceRate = getMaxSupportedRefreshRate(*refreshRateConfigs);
    const auto& minRate60 = getMinRefreshRateByPolicy(*refreshRateConfigs);
    const auto& performanceRate60 = refreshRateConfigs->getMaxRefreshRateByPolicy();
    ASSERT_EQ(mExpected60Config, minRate);
    ASSERT_EQ(mExpected60Config, minRate60);
    ASSERT_EQ(mExpected60Config, performanceRate60);
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy({HWC_CONFIG_ID_90, {60_Hz, 90_Hz}}), 0);
    refreshRateConfigs->setCurrentModeId(HWC_CONFIG_ID_90);
    const auto& minRate90 = getMinRefreshRateByPolicy(*refreshRateConfigs);
    const auto& performanceRate90 = refreshRateConfigs->getMaxRefreshRateByPolicy();
    ASSERT_EQ(mExpected90DifferentGroupConfig, performanceRate);
    ASSERT_EQ(mExpected90DifferentGroupConfig, minRate90);
    ASSERT_EQ(mExpected90DifferentGroupConfig, performanceRate90);
}
TEST_F(RefreshRateConfigsTest, twoDeviceConfigs_storesFullRefreshRateMap_differentResolutions) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90DeviceWithDifferentResolutions,
                                                                     HWC_CONFIG_ID_60);
    const auto& minRate = getMinRefreshRateByPolicy(*refreshRateConfigs);
    const auto& performanceRate = getMaxSupportedRefreshRate(*refreshRateConfigs);
    const auto& minRate60 = getMinRefreshRateByPolicy(*refreshRateConfigs);
    const auto& performanceRate60 = refreshRateConfigs->getMaxRefreshRateByPolicy();
    ASSERT_EQ(mExpected60Config, minRate);
    ASSERT_EQ(mExpected60Config, minRate60);
    ASSERT_EQ(mExpected60Config, performanceRate60);
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy({HWC_CONFIG_ID_90, {60_Hz, 90_Hz}}), 0);
    refreshRateConfigs->setCurrentModeId(HWC_CONFIG_ID_90);
    const auto& minRate90 = getMinRefreshRateByPolicy(*refreshRateConfigs);
    const auto& performanceRate90 = refreshRateConfigs->getMaxRefreshRateByPolicy();
    ASSERT_EQ(mExpected90DifferentResolutionConfig, performanceRate);
    ASSERT_EQ(mExpected90DifferentResolutionConfig, minRate90);
    ASSERT_EQ(mExpected90DifferentResolutionConfig, performanceRate90);
}
TEST_F(RefreshRateConfigsTest, twoDeviceConfigs_policyChange) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90Device,
                                                                     HWC_CONFIG_ID_60);
    auto minRate = getMinRefreshRateByPolicy(*refreshRateConfigs);
    auto performanceRate = refreshRateConfigs->getMaxRefreshRateByPolicy();
    ASSERT_EQ(mExpected60Config, minRate);
    ASSERT_EQ(mExpected90Config, performanceRate);
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy({HWC_CONFIG_ID_60, {60_Hz, 60_Hz}}), 0);
    auto minRate60 = getMinRefreshRateByPolicy(*refreshRateConfigs);
    auto performanceRate60 = refreshRateConfigs->getMaxRefreshRateByPolicy();
    ASSERT_EQ(mExpected60Config, minRate60);
    ASSERT_EQ(mExpected60Config, performanceRate60);
}
TEST_F(RefreshRateConfigsTest, twoDeviceConfigs_getCurrentRefreshRate) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90Device,
                                                                     HWC_CONFIG_ID_60);
    {
        auto current = refreshRateConfigs->getCurrentRefreshRate();
        EXPECT_EQ(current.getModeId(), HWC_CONFIG_ID_60);
    }
    refreshRateConfigs->setCurrentModeId(HWC_CONFIG_ID_90);
    {
        auto current = refreshRateConfigs->getCurrentRefreshRate();
        EXPECT_EQ(current.getModeId(), HWC_CONFIG_ID_90);
    }
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy({HWC_CONFIG_ID_90, {90_Hz, 90_Hz}}), 0);
    {
        auto current = refreshRateConfigs->getCurrentRefreshRate();
        EXPECT_EQ(current.getModeId(), HWC_CONFIG_ID_90);
    }
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_noLayers) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_72_90Device,
                                                 HWC_CONFIG_ID_72);
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate({}, {}));
    ASSERT_EQ(refreshRateConfigs->setDisplayManagerPolicy({HWC_CONFIG_ID_60, {60_Hz, 60_Hz}}),
              NO_ERROR);
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate({}, {}));
    refreshRateConfigs = std::make_unique<RefreshRateConfigs>(m60_90DeviceWithDifferentGroups,
                                                                                  HWC_CONFIG_ID_60);
    ASSERT_EQ(refreshRateConfigs->setDisplayManagerPolicy(
                      {HWC_CONFIG_ID_90, true, {0_Hz, 90_Hz}}),
              NO_ERROR);
    EXPECT_EQ(mExpected90DifferentGroupConfig, refreshRateConfigs->getBestRefreshRate({}, {}));
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_60_90) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90Device,
                                                                     HWC_CONFIG_ID_60);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    auto& lr = layers[0];
    lr.vote = LayerVoteType::Min;
    lr.name = "Min";
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.vote = LayerVoteType::Max;
    lr.name = "Max";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 90_Hz;
    lr.vote = LayerVoteType::Heuristic;
    lr.name = "90Hz Heuristic";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 60_Hz;
    lr.name = "60Hz Heuristic";
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 45_Hz;
    lr.name = "45Hz Heuristic";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 30_Hz;
    lr.name = "30Hz Heuristic";
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 24_Hz;
    lr.name = "24Hz Heuristic";
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.name = "";
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy({HWC_CONFIG_ID_60, {60_Hz, 60_Hz}}), 0);
    lr.vote = LayerVoteType::Min;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.vote = LayerVoteType::Max;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 90_Hz;
    lr.vote = LayerVoteType::Heuristic;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 60_Hz;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 45_Hz;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 30_Hz;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 24_Hz;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy({HWC_CONFIG_ID_90, {90_Hz, 90_Hz}}), 0);
    lr.vote = LayerVoteType::Min;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.vote = LayerVoteType::Max;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 90_Hz;
    lr.vote = LayerVoteType::Heuristic;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 60_Hz;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 45_Hz;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 30_Hz;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 24_Hz;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy({HWC_CONFIG_ID_60, {0_Hz, 120_Hz}}), 0);
    lr.vote = LayerVoteType::Min;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.vote = LayerVoteType::Max;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 90_Hz;
    lr.vote = LayerVoteType::Heuristic;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 60_Hz;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 45_Hz;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 30_Hz;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 24_Hz;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_multipleThreshold_60_90) {
    RefreshRateConfigs::Config config = {.frameRateMultipleThreshold = 90};
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90Device,
                                                                     HWC_CONFIG_ID_60, config);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    auto& lr = layers[0];
    lr.vote = LayerVoteType::Min;
    lr.name = "Min";
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.vote = LayerVoteType::Max;
    lr.name = "Max";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 90_Hz;
    lr.vote = LayerVoteType::Heuristic;
    lr.name = "90Hz Heuristic";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 60_Hz;
    lr.name = "60Hz Heuristic";
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 45_Hz;
    lr.name = "45Hz Heuristic";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 30_Hz;
    lr.name = "30Hz Heuristic";
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 24_Hz;
    lr.name = "24Hz Heuristic";
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_60_72_90) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_72_90Device,
                                                                     HWC_CONFIG_ID_60);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    auto& lr = layers[0];
    lr.vote = LayerVoteType::Min;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.vote = LayerVoteType::Max;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 90_Hz;
    lr.vote = LayerVoteType::Heuristic;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 60_Hz;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 45_Hz;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 30_Hz;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 24_Hz;
    EXPECT_EQ(mExpected72Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_30_60_72_90_120) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m30_60_72_90_120Device,
                                                                     HWC_CONFIG_ID_60);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}, {.weight = 1.f}};
    auto& lr1 = layers[0];
    auto& lr2 = layers[1];
    lr1.desiredRefreshRate = 24_Hz;
    lr1.vote = LayerVoteType::Heuristic;
    lr2.desiredRefreshRate = 60_Hz;
    lr2.vote = LayerVoteType::Heuristic;
    EXPECT_EQ(mExpected120Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.desiredRefreshRate = 24_Hz;
    lr1.vote = LayerVoteType::Heuristic;
    lr2.desiredRefreshRate = 48_Hz;
    lr2.vote = LayerVoteType::Heuristic;
    EXPECT_EQ(mExpected72Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.desiredRefreshRate = 24_Hz;
    lr1.vote = LayerVoteType::Heuristic;
    lr2.desiredRefreshRate = 48_Hz;
    lr2.vote = LayerVoteType::Heuristic;
    EXPECT_EQ(mExpected72Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_30_60_90_120_DifferentTypes) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m30_60_72_90_120Device,
                                                                     HWC_CONFIG_ID_60);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}, {.weight = 1.f}};
    auto& lr1 = layers[0];
    auto& lr2 = layers[1];
    lr1.desiredRefreshRate = 24_Hz;
    lr1.vote = LayerVoteType::ExplicitDefault;
    lr1.name = "24Hz ExplicitDefault";
    lr2.desiredRefreshRate = 60_Hz;
    lr2.vote = LayerVoteType::Heuristic;
    lr2.name = "60Hz Heuristic";
    EXPECT_EQ(mExpected120Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.desiredRefreshRate = 24_Hz;
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.name = "24Hz ExplicitExactOrMultiple";
    lr2.desiredRefreshRate = 60_Hz;
    lr2.vote = LayerVoteType::Heuristic;
    lr2.name = "60Hz Heuristic";
    EXPECT_EQ(mExpected120Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.desiredRefreshRate = 24_Hz;
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.name = "24Hz ExplicitExactOrMultiple";
    lr2.desiredRefreshRate = 60_Hz;
    lr2.vote = LayerVoteType::ExplicitDefault;
    lr2.name = "60Hz ExplicitDefault";
    EXPECT_EQ(mExpected120Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.desiredRefreshRate = 24_Hz;
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.name = "24Hz ExplicitExactOrMultiple";
    lr2.desiredRefreshRate = 90_Hz;
    lr2.vote = LayerVoteType::Heuristic;
    lr2.name = "90Hz Heuristic";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.desiredRefreshRate = 24_Hz;
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.name = "24Hz ExplicitExactOrMultiple";
    lr2.desiredRefreshRate = 90_Hz;
    lr2.vote = LayerVoteType::ExplicitDefault;
    lr2.name = "90Hz Heuristic";
    EXPECT_EQ(mExpected72Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.desiredRefreshRate = 24_Hz;
    lr1.vote = LayerVoteType::ExplicitDefault;
    lr1.name = "24Hz ExplicitDefault";
    lr2.desiredRefreshRate = 90_Hz;
    lr2.vote = LayerVoteType::Heuristic;
    lr2.name = "90Hz Heuristic";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.desiredRefreshRate = 24_Hz;
    lr1.vote = LayerVoteType::Heuristic;
    lr1.name = "24Hz Heuristic";
    lr2.desiredRefreshRate = 90_Hz;
    lr2.vote = LayerVoteType::ExplicitDefault;
    lr2.name = "90Hz ExplicitDefault";
    EXPECT_EQ(mExpected72Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.desiredRefreshRate = 24_Hz;
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.name = "24Hz ExplicitExactOrMultiple";
    lr2.desiredRefreshRate = 90_Hz;
    lr2.vote = LayerVoteType::ExplicitDefault;
    lr2.name = "90Hz ExplicitDefault";
    EXPECT_EQ(mExpected72Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.desiredRefreshRate = 24_Hz;
    lr1.vote = LayerVoteType::ExplicitDefault;
    lr1.name = "24Hz ExplicitDefault";
    lr2.desiredRefreshRate = 90_Hz;
    lr2.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr2.name = "90Hz ExplicitExactOrMultiple";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_30_60_90_120_DifferentTypes_multipleThreshold) {
    RefreshRateConfigs::Config config = {.frameRateMultipleThreshold = 120};
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m30_60_72_90_120Device,
                                                                     HWC_CONFIG_ID_60, config);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}, {.weight = 1.f}};
    auto& lr1 = layers[0];
    auto& lr2 = layers[1];
    lr1.desiredRefreshRate = 24_Hz;
    lr1.vote = LayerVoteType::ExplicitDefault;
    lr1.name = "24Hz ExplicitDefault";
    lr2.desiredRefreshRate = 60_Hz;
    lr2.vote = LayerVoteType::Heuristic;
    lr2.name = "60Hz Heuristic";
    EXPECT_EQ(mExpected120Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.desiredRefreshRate = 24_Hz;
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.name = "24Hz ExplicitExactOrMultiple";
    lr2.desiredRefreshRate = 60_Hz;
    lr2.vote = LayerVoteType::Heuristic;
    lr2.name = "60Hz Heuristic";
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.desiredRefreshRate = 24_Hz;
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.name = "24Hz ExplicitExactOrMultiple";
    lr2.desiredRefreshRate = 60_Hz;
    lr2.vote = LayerVoteType::ExplicitDefault;
    lr2.name = "60Hz ExplicitDefault";
    EXPECT_EQ(mExpected72Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.desiredRefreshRate = 24_Hz;
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.name = "24Hz ExplicitExactOrMultiple";
    lr2.desiredRefreshRate = 90_Hz;
    lr2.vote = LayerVoteType::Heuristic;
    lr2.name = "90Hz Heuristic";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.desiredRefreshRate = 24_Hz;
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.name = "24Hz ExplicitExactOrMultiple";
    lr2.desiredRefreshRate = 90_Hz;
    lr2.vote = LayerVoteType::ExplicitDefault;
    lr2.name = "90Hz Heuristic";
    EXPECT_EQ(mExpected72Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.desiredRefreshRate = 24_Hz;
    lr1.vote = LayerVoteType::ExplicitDefault;
    lr1.name = "24Hz ExplicitDefault";
    lr2.desiredRefreshRate = 90_Hz;
    lr2.vote = LayerVoteType::Heuristic;
    lr2.name = "90Hz Heuristic";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.desiredRefreshRate = 24_Hz;
    lr1.vote = LayerVoteType::Heuristic;
    lr1.name = "24Hz Heuristic";
    lr2.desiredRefreshRate = 90_Hz;
    lr2.vote = LayerVoteType::ExplicitDefault;
    lr2.name = "90Hz ExplicitDefault";
    EXPECT_EQ(mExpected72Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.desiredRefreshRate = 24_Hz;
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.name = "24Hz ExplicitExactOrMultiple";
    lr2.desiredRefreshRate = 90_Hz;
    lr2.vote = LayerVoteType::ExplicitDefault;
    lr2.name = "90Hz ExplicitDefault";
    EXPECT_EQ(mExpected72Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.desiredRefreshRate = 24_Hz;
    lr1.vote = LayerVoteType::ExplicitDefault;
    lr1.name = "24Hz ExplicitDefault";
    lr2.desiredRefreshRate = 90_Hz;
    lr2.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr2.name = "90Hz ExplicitExactOrMultiple";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_30_60) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m30_60Device,
                                                                     HWC_CONFIG_ID_60);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    auto& lr = layers[0];
    lr.vote = LayerVoteType::Min;
    EXPECT_EQ(mExpected30Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.vote = LayerVoteType::Max;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 90_Hz;
    lr.vote = LayerVoteType::Heuristic;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 60_Hz;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 45_Hz;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 30_Hz;
    EXPECT_EQ(mExpected30Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 24_Hz;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_30_60_72_90) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m30_60_72_90Device,
                                                                     HWC_CONFIG_ID_60);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    auto& lr = layers[0];
    lr.vote = LayerVoteType::Min;
    lr.name = "Min";
    EXPECT_EQ(mExpected30Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.vote = LayerVoteType::Max;
    lr.name = "Max";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 90_Hz;
    lr.vote = LayerVoteType::Heuristic;
    lr.name = "90Hz Heuristic";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.desiredRefreshRate = 60_Hz;
    lr.name = "60Hz Heuristic";
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {.touch = true}));
    lr.desiredRefreshRate = 45_Hz;
    lr.name = "45Hz Heuristic";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {.touch = true}));
    lr.desiredRefreshRate = 30_Hz;
    lr.name = "30Hz Heuristic";
    EXPECT_EQ(mExpected30Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {.touch = true}));
    lr.desiredRefreshRate = 24_Hz;
    lr.name = "24Hz Heuristic";
    EXPECT_EQ(mExpected72Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {.touch = true}));
    lr.desiredRefreshRate = 24_Hz;
    lr.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr.name = "24Hz ExplicitExactOrMultiple";
    EXPECT_EQ(mExpected72Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {.touch = true}));
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_PriorityTest) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m30_60_90Device,
                                                                     HWC_CONFIG_ID_60);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}, {.weight = 1.f}};
    auto& lr1 = layers[0];
    auto& lr2 = layers[1];
    lr1.vote = LayerVoteType::Min;
    lr2.vote = LayerVoteType::Max;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.vote = LayerVoteType::Min;
    lr2.vote = LayerVoteType::Heuristic;
    lr2.desiredRefreshRate = 24_Hz;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.vote = LayerVoteType::Min;
    lr2.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr2.desiredRefreshRate = 24_Hz;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.vote = LayerVoteType::Max;
    lr2.vote = LayerVoteType::Heuristic;
    lr2.desiredRefreshRate = 60_Hz;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.vote = LayerVoteType::Max;
    lr2.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr2.desiredRefreshRate = 60_Hz;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.vote = LayerVoteType::Heuristic;
    lr1.desiredRefreshRate = 15_Hz;
    lr2.vote = LayerVoteType::Heuristic;
    lr2.desiredRefreshRate = 45_Hz;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.vote = LayerVoteType::Heuristic;
    lr1.desiredRefreshRate = 30_Hz;
    lr2.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr2.desiredRefreshRate = 45_Hz;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_24FpsVideo) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90Device,
                                                                     HWC_CONFIG_ID_60);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    auto& lr = layers[0];
    lr.vote = LayerVoteType::ExplicitExactOrMultiple;
    for (float fps = 23.0f; fps < 25.0f; fps += 0.1f) {
        lr.desiredRefreshRate = Fps::fromValue(fps);
        const auto refreshRate = refreshRateConfigs->getBestRefreshRate(layers, {});
        EXPECT_EQ(mExpected60Config, refreshRate)
                << lr.desiredRefreshRate << " chooses " << refreshRate.getName();
    }
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_24FpsVideo_multipleThreshold_60_120) {
    RefreshRateConfigs::Config config = {.frameRateMultipleThreshold = 120};
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_120Device,
                                                                     HWC_CONFIG_ID_60, config);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    auto& lr = layers[0];
    lr.vote = LayerVoteType::ExplicitExactOrMultiple;
    for (float fps = 23.0f; fps < 25.0f; fps += 0.1f) {
        lr.desiredRefreshRate = Fps::fromValue(fps);
        const auto refreshRate = refreshRateConfigs->getBestRefreshRate(layers, {});
        EXPECT_EQ(mExpected60Config, refreshRate)
                << lr.desiredRefreshRate << " chooses " << refreshRate.getName();
    }
}
TEST_F(RefreshRateConfigsTest, twoDeviceConfigs_getBestRefreshRate_Explicit) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90Device,
                                                                     HWC_CONFIG_ID_60);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}, {.weight = 1.f}};
    auto& lr1 = layers[0];
    auto& lr2 = layers[1];
    lr1.vote = LayerVoteType::Heuristic;
    lr1.desiredRefreshRate = 60_Hz;
    lr2.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr2.desiredRefreshRate = 90_Hz;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.vote = LayerVoteType::ExplicitDefault;
    lr1.desiredRefreshRate = 90_Hz;
    lr2.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr2.desiredRefreshRate = 60_Hz;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.vote = LayerVoteType::Heuristic;
    lr1.desiredRefreshRate = 90_Hz;
    lr2.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr2.desiredRefreshRate = 60_Hz;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
}
TEST_F(RefreshRateConfigsTest, testInPolicy) {
    ASSERT_TRUE(mExpectedAlmost60Config.inPolicy(60.000004_Hz, 60.000004_Hz));
    ASSERT_TRUE(mExpectedAlmost60Config.inPolicy(59_Hz, 60.1_Hz));
    ASSERT_FALSE(mExpectedAlmost60Config.inPolicy(75_Hz, 90_Hz));
    ASSERT_FALSE(mExpectedAlmost60Config.inPolicy(60.0011_Hz, 90_Hz));
    ASSERT_FALSE(mExpectedAlmost60Config.inPolicy(50_Hz, 59.998_Hz));
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_75HzContent) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90Device,
                                                                     HWC_CONFIG_ID_60);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    auto& lr = layers[0];
    lr.vote = LayerVoteType::ExplicitExactOrMultiple;
    for (float fps = 75.0f; fps < 100.0f; fps += 0.1f) {
        lr.desiredRefreshRate = Fps::fromValue(fps);
        const auto refreshRate = refreshRateConfigs->getBestRefreshRate(layers, {});
        EXPECT_EQ(mExpected90Config, refreshRate)
                << lr.desiredRefreshRate << " chooses " << refreshRate.getName();
    }
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_Multiples) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90Device,
                                                                     HWC_CONFIG_ID_60);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}, {.weight = 1.f}};
    auto& lr1 = layers[0];
    auto& lr2 = layers[1];
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.desiredRefreshRate = 60_Hz;
    lr1.name = "60Hz ExplicitExactOrMultiple";
    lr2.vote = LayerVoteType::Heuristic;
    lr2.desiredRefreshRate = 90_Hz;
    lr2.name = "90Hz Heuristic";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.desiredRefreshRate = 60_Hz;
    lr1.name = "60Hz ExplicitExactOrMultiple";
    lr2.vote = LayerVoteType::ExplicitDefault;
    lr2.desiredRefreshRate = 90_Hz;
    lr2.name = "90Hz ExplicitDefault";
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.desiredRefreshRate = 60_Hz;
    lr1.name = "60Hz ExplicitExactOrMultiple";
    lr2.vote = LayerVoteType::Max;
    lr2.name = "Max";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.desiredRefreshRate = 30_Hz;
    lr1.name = "30Hz ExplicitExactOrMultiple";
    lr2.vote = LayerVoteType::Heuristic;
    lr2.desiredRefreshRate = 90_Hz;
    lr2.name = "90Hz Heuristic";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.desiredRefreshRate = 30_Hz;
    lr1.name = "30Hz ExplicitExactOrMultiple";
    lr2.vote = LayerVoteType::Max;
    lr2.name = "Max";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
}
TEST_F(RefreshRateConfigsTest, scrollWhileWatching60fps_60_90) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90Device,
                                                                     HWC_CONFIG_ID_60);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}, {.weight = 1.f}};
    auto& lr1 = layers[0];
    auto& lr2 = layers[1];
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.desiredRefreshRate = 60_Hz;
    lr1.name = "60Hz ExplicitExactOrMultiple";
    lr2.vote = LayerVoteType::NoVote;
    lr2.name = "NoVote";
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.desiredRefreshRate = 60_Hz;
    lr1.name = "60Hz ExplicitExactOrMultiple";
    lr2.vote = LayerVoteType::NoVote;
    lr2.name = "NoVote";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {.touch = true}));
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.desiredRefreshRate = 60_Hz;
    lr1.name = "60Hz ExplicitExactOrMultiple";
    lr2.vote = LayerVoteType::Max;
    lr2.name = "Max";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {.touch = true}));
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.desiredRefreshRate = 60_Hz;
    lr1.name = "60Hz ExplicitExactOrMultiple";
    lr2.vote = LayerVoteType::Max;
    lr2.name = "Max";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.desiredRefreshRate = 60_Hz;
    lr1.name = "60Hz ExplicitExactOrMultiple";
    lr2.vote = LayerVoteType::Heuristic;
    lr2.desiredRefreshRate = 90_Hz;
    lr2.name = "90Hz Heuristic";
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
}
TEST_F(RefreshRateConfigsTest, touchConsidered) {
    RefreshRateConfigs::GlobalSignals consideredSignals;
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90Device,
                                                                     HWC_CONFIG_ID_60);
    refreshRateConfigs->getBestRefreshRate({}, {}, &consideredSignals);
    EXPECT_EQ(false, consideredSignals.touch);
    refreshRateConfigs->getBestRefreshRate({}, {.touch = true}, &consideredSignals);
    EXPECT_EQ(true, consideredSignals.touch);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}, {.weight = 1.f}};
    auto& lr1 = layers[0];
    auto& lr2 = layers[1];
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.desiredRefreshRate = 60_Hz;
    lr1.name = "60Hz ExplicitExactOrMultiple";
    lr2.vote = LayerVoteType::Heuristic;
    lr2.desiredRefreshRate = 60_Hz;
    lr2.name = "60Hz Heuristic";
    refreshRateConfigs->getBestRefreshRate(layers, {.touch = true}, &consideredSignals);
    EXPECT_EQ(true, consideredSignals.touch);
    lr1.vote = LayerVoteType::ExplicitDefault;
    lr1.desiredRefreshRate = 60_Hz;
    lr1.name = "60Hz ExplicitExactOrMultiple";
    lr2.vote = LayerVoteType::Heuristic;
    lr2.desiredRefreshRate = 60_Hz;
    lr2.name = "60Hz Heuristic";
    refreshRateConfigs->getBestRefreshRate(layers, {.touch = true}, &consideredSignals);
    EXPECT_EQ(false, consideredSignals.touch);
    lr1.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr1.desiredRefreshRate = 60_Hz;
    lr1.name = "60Hz ExplicitExactOrMultiple";
    lr2.vote = LayerVoteType::Heuristic;
    lr2.desiredRefreshRate = 60_Hz;
    lr2.name = "60Hz Heuristic";
    refreshRateConfigs->getBestRefreshRate(layers, {.touch = true}, &consideredSignals);
    EXPECT_EQ(true, consideredSignals.touch);
    lr1.vote = LayerVoteType::ExplicitDefault;
    lr1.desiredRefreshRate = 60_Hz;
    lr1.name = "60Hz ExplicitExactOrMultiple";
    lr2.vote = LayerVoteType::Heuristic;
    lr2.desiredRefreshRate = 60_Hz;
    lr2.name = "60Hz Heuristic";
    refreshRateConfigs->getBestRefreshRate(layers, {.touch = true}, &consideredSignals);
    EXPECT_EQ(false, consideredSignals.touch);
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_ExplicitDefault) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90_72_120Device,
                                                 HWC_CONFIG_ID_60);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    auto& lr = layers[0];
    const std::initializer_list<std::pair<Fps, Fps>> testCases = {
            {130_Hz, 120_Hz}, {120_Hz, 120_Hz}, {119_Hz, 120_Hz}, {110_Hz, 120_Hz},
            {100_Hz, 90_Hz}, {90_Hz, 90_Hz}, {89_Hz, 90_Hz},
            {80_Hz, 72_Hz}, {73_Hz, 72_Hz}, {72_Hz, 72_Hz}, {71_Hz, 72_Hz}, {70_Hz, 72_Hz},
            {65_Hz, 60_Hz}, {60_Hz, 60_Hz}, {59_Hz, 60_Hz}, {58_Hz, 60_Hz},
            {55_Hz, 90_Hz}, {50_Hz, 90_Hz}, {45_Hz, 90_Hz},
            {42_Hz, 120_Hz}, {40_Hz, 120_Hz}, {39_Hz, 120_Hz},
            {37_Hz, 72_Hz}, {36_Hz, 72_Hz}, {35_Hz, 72_Hz},
            {30_Hz, 60_Hz},
    };
    for (auto [desired, expected] : testCases) {
        lr.vote = LayerVoteType::ExplicitDefault;
        lr.desiredRefreshRate = desired;
        std::stringstream ss;
        ss << "ExplicitDefault " << desired;
        lr.name = ss.str();
<<<<<<< HEAD
        const auto refreshRate = refreshRateConfigs->getBestRefreshRate(layers, {});
        EXPECT_EQ(refreshRate.getFps(), expected);
    }
}
TEST_F(RefreshRateConfigsTest,
       getBestRefreshRate_ExplicitExactOrMultiple_WithFractionalRefreshRates) {
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    auto& lr = layers[0];
    {
        android::DisplayModes modes = {mConfig24, mConfig25, mConfig30,
                                       mConfig30Frac, mConfig60, mConfig60Frac};
        auto refreshRateConfigs =
                std::make_unique<RefreshRateConfigs>(modes, HWC_CONFIG_ID_60);
        lr.vote = LayerVoteType::ExplicitExactOrMultiple;
        lr.desiredRefreshRate = 23.976_Hz;
        lr.name = "ExplicitExactOrMultiple 23.976 Hz";
        EXPECT_EQ(HWC_CONFIG_ID_24, refreshRateConfigs->getBestRefreshRate(layers, {}).getModeId());
    }
    {
        android::DisplayModes modes = {mConfig24Frac, mConfig25, mConfig30,
                                       mConfig30Frac, mConfig60, mConfig60Frac};
        auto refreshRateConfigs =
                std::make_unique<RefreshRateConfigs>(modes, HWC_CONFIG_ID_60);
        lr.desiredRefreshRate = 24_Hz;
        lr.name = "ExplicitExactOrMultiple 24 Hz";
        EXPECT_EQ(HWC_CONFIG_ID_24_FRAC,
                  refreshRateConfigs->getBestRefreshRate(layers, {}).getModeId());
    }
    {
        android::DisplayModes modes = {mConfig24, mConfig24Frac, mConfig25,
                                       mConfig30, mConfig60, mConfig60Frac};
        auto refreshRateConfigs =
                std::make_unique<RefreshRateConfigs>(modes, HWC_CONFIG_ID_60);
        lr.desiredRefreshRate = 29.97_Hz;
        lr.name = "ExplicitExactOrMultiple 29.97 Hz";
        EXPECT_EQ(HWC_CONFIG_ID_60_FRAC,
                  refreshRateConfigs->getBestRefreshRate(layers, {}).getModeId());
    }
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_ExplicitExact_WithFractionalRefreshRates) {
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    auto& lr = layers[0];
    {
        auto refreshRateConfigs =
                std::make_unique<RefreshRateConfigs>(m24_25_30_50_60WithFracDevice,
                                                                         HWC_CONFIG_ID_60);
        for (auto desired : {23.976_Hz, 24_Hz, 25_Hz, 29.97_Hz, 30_Hz, 50_Hz, 59.94_Hz, 60_Hz}) {
            lr.vote = LayerVoteType::ExplicitExact;
            lr.desiredRefreshRate = desired;
            std::stringstream ss;
            ss << "ExplicitExact " << desired;
            lr.name = ss.str();
            auto selectedRefreshRate = refreshRateConfigs->getBestRefreshRate(layers, {});
            EXPECT_EQ(selectedRefreshRate.getFps(), lr.desiredRefreshRate);
        }
    }
    {
        android::DisplayModes modes = {mConfig24, mConfig25, mConfig30,
                                       mConfig30Frac, mConfig60, mConfig60Frac};
        auto refreshRateConfigs =
                std::make_unique<RefreshRateConfigs>(modes, HWC_CONFIG_ID_60);
        lr.vote = LayerVoteType::ExplicitExact;
        lr.desiredRefreshRate = 23.976_Hz;
        lr.name = "ExplicitExact 23.976 Hz";
        EXPECT_EQ(HWC_CONFIG_ID_24, refreshRateConfigs->getBestRefreshRate(layers, {}).getModeId());
    }
    {
        android::DisplayModes modes = {mConfig24Frac, mConfig25, mConfig30,
                                       mConfig30Frac, mConfig60, mConfig60Frac};
        auto refreshRateConfigs =
                std::make_unique<RefreshRateConfigs>(modes, HWC_CONFIG_ID_60);
        lr.desiredRefreshRate = 24_Hz;
        lr.name = "ExplicitExact 24 Hz";
        EXPECT_EQ(HWC_CONFIG_ID_24_FRAC,
                  refreshRateConfigs->getBestRefreshRate(layers, {}).getModeId());
||||||| 5502d9f7a2
        const auto& refreshRate =
                refreshRateConfigs->getBestRefreshRate(layers, {.touch = false, .idle = false});
        EXPECT_TRUE(refreshRate.getFps().equalsWithMargin(Fps(test.second)))
                << "Expecting " << test.first << "fps => " << test.second << "Hz";
=======
        const auto& refreshRate =
                refreshRateConfigs->getBestRefreshRate(layers, {.touch = false, .idle = false});
        EXPECT_TRUE(refreshRate.getFps().equalsWithMargin(Fps(test.second)))
                << "Expecting " << test.first << "fps => " << test.second << "Hz"
                << " but it was " << refreshRate.getFps();
    }
}
TEST_F(RefreshRateConfigsTest,
       getBestRefreshRate_ExplicitExactOrMultiple_WithFractionalRefreshRates) {
    auto layers = std::vector<LayerRequirement>{LayerRequirement{.weight = 1.0f}};
    auto& lr = layers[0];
    {
        android::DisplayModes modes = {mConfig24, mConfig25, mConfig30,
                                       mConfig30Frac, mConfig60, mConfig60Frac};
        auto refreshRateConfigs =
                std::make_unique<RefreshRateConfigs>(modes, HWC_CONFIG_ID_60);
        lr.vote = LayerVoteType::ExplicitExactOrMultiple;
        lr.desiredRefreshRate = Fps(23.976f);
        lr.name = "ExplicitExactOrMultiple 23.976 fps";
        EXPECT_EQ(HWC_CONFIG_ID_24,
                  refreshRateConfigs->getBestRefreshRate(layers, {.touch = false, .idle = false})
                          .getModeId());
    }
    {
        android::DisplayModes modes = {mConfig24Frac, mConfig25, mConfig30,
                                       mConfig30Frac, mConfig60, mConfig60Frac};
        auto refreshRateConfigs =
                std::make_unique<RefreshRateConfigs>(modes, HWC_CONFIG_ID_60);
        lr.desiredRefreshRate = Fps(24.f);
        lr.name = "ExplicitExactOrMultiple 24 fps";
        EXPECT_EQ(HWC_CONFIG_ID_24_FRAC,
                  refreshRateConfigs->getBestRefreshRate(layers, {.touch = false, .idle = false})
                          .getModeId());
    }
    {
        android::DisplayModes modes = {mConfig24, mConfig24Frac, mConfig25,
                                       mConfig30, mConfig60, mConfig60Frac};
        auto refreshRateConfigs =
                std::make_unique<RefreshRateConfigs>(modes, HWC_CONFIG_ID_60);
        lr.desiredRefreshRate = Fps(29.97f);
        lr.name = "ExplicitExactOrMultiple 29.97f fps";
        EXPECT_EQ(HWC_CONFIG_ID_60_FRAC,
                  refreshRateConfigs->getBestRefreshRate(layers, {.touch = false, .idle = false})
                          .getModeId());
    }
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_ExplicitExact_WithFractionalRefreshRates) {
    auto layers = std::vector<LayerRequirement>{LayerRequirement{.weight = 1.0f}};
    auto& lr = layers[0];
    {
        auto refreshRateConfigs =
                std::make_unique<RefreshRateConfigs>(m24_25_30_50_60WithFracDevice,
                                                                         HWC_CONFIG_ID_60);
        for (auto desiredRefreshRate : {23.976f, 24.f, 25.f, 29.97f, 30.f, 50.f, 59.94f, 60.f}) {
            lr.vote = LayerVoteType::ExplicitExact;
            lr.desiredRefreshRate = Fps(desiredRefreshRate);
            std::stringstream ss;
            ss << "ExplicitExact " << desiredRefreshRate << " fps";
            lr.name = ss.str();
            auto selecteRefreshRate =
                    refreshRateConfigs->getBestRefreshRate(layers, {.touch = false, .idle = false});
            EXPECT_TRUE(selecteRefreshRate.getFps().equalsWithMargin(lr.desiredRefreshRate))
                    << "Expecting " << lr.desiredRefreshRate << " but it was "
                    << selecteRefreshRate.getFps();
        }
    }
    {
        android::DisplayModes modes = {mConfig24, mConfig25, mConfig30,
                                       mConfig30Frac, mConfig60, mConfig60Frac};
        auto refreshRateConfigs =
                std::make_unique<RefreshRateConfigs>(modes, HWC_CONFIG_ID_60);
        lr.vote = LayerVoteType::ExplicitExact;
        lr.desiredRefreshRate = Fps(23.976f);
        lr.name = "ExplicitExact 23.976 fps";
        EXPECT_EQ(HWC_CONFIG_ID_24,
                  refreshRateConfigs->getBestRefreshRate(layers, {.touch = false, .idle = false})
                          .getModeId());
    }
    {
        android::DisplayModes modes = {mConfig24Frac, mConfig25, mConfig30,
                                       mConfig30Frac, mConfig60, mConfig60Frac};
        auto refreshRateConfigs =
                std::make_unique<RefreshRateConfigs>(modes, HWC_CONFIG_ID_60);
        lr.desiredRefreshRate = Fps(24.f);
        lr.name = "ExplicitExact 24 fps";
        EXPECT_EQ(HWC_CONFIG_ID_24_FRAC,
                  refreshRateConfigs->getBestRefreshRate(layers, {.touch = false, .idle = false})
                          .getModeId());
>>>>>>> 9c647ace
    }
}
TEST_F(RefreshRateConfigsTest,
       getBestRefreshRate_withDisplayManagerRequestingSingleRate_ignoresTouchFlag) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90Device,
                                                                     HWC_CONFIG_ID_90);
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy(
                      {HWC_CONFIG_ID_90, {90_Hz, 90_Hz}, {60_Hz, 90_Hz}}),
              0);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    auto& lr = layers[0];
    RefreshRateConfigs::GlobalSignals consideredSignals;
    lr.vote = LayerVoteType::ExplicitDefault;
    lr.desiredRefreshRate = 60_Hz;
    lr.name = "60Hz ExplicitDefault";
    lr.focused = true;
    EXPECT_EQ(mExpected60Config,
              refreshRateConfigs->getBestRefreshRate(layers, {.touch = true, .idle = true},
                                                     &consideredSignals));
    EXPECT_EQ(false, consideredSignals.touch);
}
TEST_F(RefreshRateConfigsTest,
       getBestRefreshRate_withDisplayManagerRequestingSingleRate_ignoresIdleFlag) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90Device,
                                                                     HWC_CONFIG_ID_60);
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy(
                      {HWC_CONFIG_ID_60, {60_Hz, 60_Hz}, {60_Hz, 90_Hz}}),
              0);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    auto& lr = layers[0];
    lr.vote = LayerVoteType::ExplicitDefault;
    lr.desiredRefreshRate = 90_Hz;
    lr.name = "90Hz ExplicitDefault";
    lr.focused = true;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {.idle = true}));
}
TEST_F(RefreshRateConfigsTest,
       getBestRefreshRate_withDisplayManagerRequestingSingleRate_onlySwitchesRatesForExplicitFocusedLayers) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90Device,
                                                                     HWC_CONFIG_ID_90);
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy(
                      {HWC_CONFIG_ID_90, {90_Hz, 90_Hz}, {60_Hz, 90_Hz}}),
              0);
    RefreshRateConfigs::GlobalSignals consideredSignals;
    EXPECT_EQ(mExpected90Config,
              refreshRateConfigs->getBestRefreshRate({}, {}, &consideredSignals));
    EXPECT_EQ(false, consideredSignals.touch);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    auto& lr = layers[0];
    lr.vote = LayerVoteType::ExplicitExactOrMultiple;
    lr.desiredRefreshRate = 60_Hz;
    lr.name = "60Hz ExplicitExactOrMultiple";
    lr.focused = false;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.focused = true;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.vote = LayerVoteType::ExplicitDefault;
    lr.desiredRefreshRate = 60_Hz;
    lr.name = "60Hz ExplicitDefault";
    lr.focused = false;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.focused = true;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.vote = LayerVoteType::Heuristic;
    lr.desiredRefreshRate = 60_Hz;
    lr.name = "60Hz Heuristic";
    lr.focused = false;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.focused = true;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.vote = LayerVoteType::Max;
    lr.desiredRefreshRate = 60_Hz;
    lr.name = "60Hz Max";
    lr.focused = false;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.focused = true;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.vote = LayerVoteType::Min;
    lr.desiredRefreshRate = 60_Hz;
    lr.name = "60Hz Min";
    lr.focused = false;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    lr.focused = true;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
}
TEST_F(RefreshRateConfigsTest, groupSwitchingNotAllowed) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90DeviceWithDifferentGroups,
                                                                     HWC_CONFIG_ID_60);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    auto& layer = layers[0];
    layer.vote = LayerVoteType::ExplicitDefault;
    layer.desiredRefreshRate = 90_Hz;
    layer.seamlessness = Seamlessness::SeamedAndSeamless;
    layer.name = "90Hz ExplicitDefault";
    layer.focused = true;
    ASSERT_EQ(HWC_CONFIG_ID_60, refreshRateConfigs->getBestRefreshRate(layers, {}).getModeId());
}
TEST_F(RefreshRateConfigsTest, groupSwitchingWithOneLayer) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90DeviceWithDifferentGroups,
                                                                     HWC_CONFIG_ID_60);
    RefreshRateConfigs::Policy policy;
    policy.defaultMode = refreshRateConfigs->getCurrentPolicy().defaultMode;
    policy.allowGroupSwitching = true;
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy(policy), 0);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    auto& layer = layers[0];
    layer.vote = LayerVoteType::ExplicitDefault;
    layer.desiredRefreshRate = 90_Hz;
    layer.seamlessness = Seamlessness::SeamedAndSeamless;
    layer.name = "90Hz ExplicitDefault";
    layer.focused = true;
    ASSERT_EQ(HWC_CONFIG_ID_90, refreshRateConfigs->getBestRefreshRate(layers, {}).getModeId());
}
TEST_F(RefreshRateConfigsTest, groupSwitchingWithOneLayerOnlySeamless) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90DeviceWithDifferentGroups,
                                                                     HWC_CONFIG_ID_60);
    RefreshRateConfigs::Policy policy;
    policy.defaultMode = refreshRateConfigs->getCurrentPolicy().defaultMode;
    policy.allowGroupSwitching = true;
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy(policy), 0);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    auto& layer = layers[0];
    layer.vote = LayerVoteType::ExplicitDefault;
    layer.desiredRefreshRate = 90_Hz;
    layer.seamlessness = Seamlessness::OnlySeamless;
    layer.name = "90Hz ExplicitDefault";
    layer.focused = true;
    ASSERT_EQ(HWC_CONFIG_ID_60, refreshRateConfigs->getBestRefreshRate(layers, {}).getModeId());
}
TEST_F(RefreshRateConfigsTest, groupSwitchingWithOneLayerOnlySeamlessDefaultFps) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90DeviceWithDifferentGroups,
                                                                     HWC_CONFIG_ID_60);
    RefreshRateConfigs::Policy policy;
    policy.defaultMode = refreshRateConfigs->getCurrentPolicy().defaultMode;
    policy.allowGroupSwitching = true;
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy(policy), 0);
    refreshRateConfigs->setCurrentModeId(HWC_CONFIG_ID_90);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    auto& layer = layers[0];
    layer.vote = LayerVoteType::ExplicitDefault;
    layer.desiredRefreshRate = 60_Hz;
    layer.seamlessness = Seamlessness::OnlySeamless;
    layer.name = "60Hz ExplicitDefault";
    layer.focused = true;
    ASSERT_EQ(HWC_CONFIG_ID_90, refreshRateConfigs->getBestRefreshRate(layers, {}).getModeId());
}
TEST_F(RefreshRateConfigsTest, groupSwitchingWithOneLayerDefaultSeamlessness) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90DeviceWithDifferentGroups,
                                                                     HWC_CONFIG_ID_60);
    RefreshRateConfigs::Policy policy;
    policy.defaultMode = refreshRateConfigs->getCurrentPolicy().defaultMode;
    policy.allowGroupSwitching = true;
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy(policy), 0);
    refreshRateConfigs->setCurrentModeId(HWC_CONFIG_ID_90);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    auto& layer = layers[0];
    layer.vote = LayerVoteType::ExplicitDefault;
    layer.desiredRefreshRate = 60_Hz;
    layer.seamlessness = Seamlessness::Default;
    layer.name = "60Hz ExplicitDefault";
    layer.focused = true;
    ASSERT_EQ(HWC_CONFIG_ID_60, refreshRateConfigs->getBestRefreshRate(layers, {}).getModeId());
}
TEST_F(RefreshRateConfigsTest, groupSwitchingWithTwoLayersOnlySeamlessAndSeamed) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90DeviceWithDifferentGroups,
                                                                     HWC_CONFIG_ID_60);
    RefreshRateConfigs::Policy policy;
    policy.defaultMode = refreshRateConfigs->getCurrentPolicy().defaultMode;
    policy.allowGroupSwitching = true;
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy(policy), 0);
    refreshRateConfigs->setCurrentModeId(HWC_CONFIG_ID_90);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    layers[0].vote = LayerVoteType::ExplicitDefault;
    layers[0].desiredRefreshRate = 60_Hz;
    layers[0].seamlessness = Seamlessness::OnlySeamless;
    layers[0].name = "60Hz ExplicitDefault";
    layers[0].focused = true;
    layers.push_back(LayerRequirement{.weight = 0.5f});
    layers[1].vote = LayerVoteType::ExplicitDefault;
    layers[1].seamlessness = Seamlessness::SeamedAndSeamless;
    layers[1].desiredRefreshRate = 90_Hz;
    layers[1].name = "90Hz ExplicitDefault";
    layers[1].focused = false;
    ASSERT_EQ(HWC_CONFIG_ID_90, refreshRateConfigs->getBestRefreshRate(layers, {}).getModeId());
}
TEST_F(RefreshRateConfigsTest, groupSwitchingWithTwoLayersDefaultFocusedAndSeamed) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90DeviceWithDifferentGroups,
                                                                     HWC_CONFIG_ID_60);
    RefreshRateConfigs::Policy policy;
    policy.defaultMode = refreshRateConfigs->getCurrentPolicy().defaultMode;
    policy.allowGroupSwitching = true;
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy(policy), 0);
    refreshRateConfigs->setCurrentModeId(HWC_CONFIG_ID_90);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    layers[0].seamlessness = Seamlessness::Default;
    layers[0].desiredRefreshRate = 60_Hz;
    layers[0].focused = true;
    layers[0].vote = LayerVoteType::ExplicitDefault;
    layers[0].name = "60Hz ExplicitDefault";
    layers.push_back(LayerRequirement{.weight = 0.1f});
    layers[1].seamlessness = Seamlessness::SeamedAndSeamless;
    layers[1].desiredRefreshRate = 90_Hz;
    layers[1].focused = true;
    layers[1].vote = LayerVoteType::ExplicitDefault;
    layers[1].name = "90Hz ExplicitDefault";
    ASSERT_EQ(HWC_CONFIG_ID_90, refreshRateConfigs->getBestRefreshRate(layers, {}).getModeId());
}
TEST_F(RefreshRateConfigsTest, groupSwitchingWithTwoLayersDefaultNotFocusedAndSeamed) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90DeviceWithDifferentGroups,
                                                                     HWC_CONFIG_ID_60);
    RefreshRateConfigs::Policy policy;
    policy.defaultMode = refreshRateConfigs->getCurrentPolicy().defaultMode;
    policy.allowGroupSwitching = true;
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy(policy), 0);
    refreshRateConfigs->setCurrentModeId(HWC_CONFIG_ID_90);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    layers[0].seamlessness = Seamlessness::Default;
    layers[0].desiredRefreshRate = 60_Hz;
    layers[0].focused = true;
    layers[0].vote = LayerVoteType::ExplicitDefault;
    layers[0].name = "60Hz ExplicitDefault";
    layers.push_back(LayerRequirement{.weight = 0.7f});
    layers[1].seamlessness = Seamlessness::SeamedAndSeamless;
    layers[1].desiredRefreshRate = 90_Hz;
    layers[1].focused = false;
    layers[1].vote = LayerVoteType::ExplicitDefault;
    layers[1].name = "90Hz ExplicitDefault";
    ASSERT_EQ(HWC_CONFIG_ID_60, refreshRateConfigs->getBestRefreshRate(layers, {}).getModeId());
}
TEST_F(RefreshRateConfigsTest, nonSeamlessVotePrefersSeamlessSwitches) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m30_60Device,
                                                                     HWC_CONFIG_ID_60);
    RefreshRateConfigs::Policy policy;
    policy.defaultMode = refreshRateConfigs->getCurrentPolicy().defaultMode;
    policy.allowGroupSwitching = true;
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy(policy), 0);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    auto& layer = layers[0];
    layer.vote = LayerVoteType::ExplicitExactOrMultiple;
    layer.desiredRefreshRate = 60_Hz;
    layer.seamlessness = Seamlessness::SeamedAndSeamless;
    layer.name = "60Hz ExplicitExactOrMultiple";
    layer.focused = true;
    ASSERT_EQ(HWC_CONFIG_ID_60, refreshRateConfigs->getBestRefreshRate(layers, {}).getModeId());
    refreshRateConfigs->setCurrentModeId(HWC_CONFIG_ID_120);
    ASSERT_EQ(HWC_CONFIG_ID_120, refreshRateConfigs->getBestRefreshRate(layers, {}).getModeId());
}
TEST_F(RefreshRateConfigsTest, nonSeamlessExactAndSeamlessMultipleLayers) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m25_30_50_60Device,
                                                                     HWC_CONFIG_ID_60);
    RefreshRateConfigs::Policy policy;
    policy.defaultMode = refreshRateConfigs->getCurrentPolicy().defaultMode;
    policy.allowGroupSwitching = true;
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy(policy), 0);
    std::vector<LayerRequirement> layers = {{.name = "60Hz ExplicitDefault",
                                             .vote = LayerVoteType::ExplicitDefault,
                                             .desiredRefreshRate = 60_Hz,
                                             .seamlessness = Seamlessness::SeamedAndSeamless,
                                             .weight = 0.5f,
                                             .focused = false},
                                            {.name = "25Hz ExplicitExactOrMultiple",
                                             .vote = LayerVoteType::ExplicitExactOrMultiple,
                                             .desiredRefreshRate = 25_Hz,
                                             .seamlessness = Seamlessness::OnlySeamless,
                                             .weight = 1.f,
                                             .focused = true}};
    ASSERT_EQ(HWC_CONFIG_ID_50, refreshRateConfigs->getBestRefreshRate(layers, {}).getModeId());
    auto& seamedLayer = layers[0];
    seamedLayer.desiredRefreshRate = 30_Hz;
    seamedLayer.name = "30Hz ExplicitDefault";
    refreshRateConfigs->setCurrentModeId(HWC_CONFIG_ID_30);
    ASSERT_EQ(HWC_CONFIG_ID_25, refreshRateConfigs->getBestRefreshRate(layers, {}).getModeId());
}
TEST_F(RefreshRateConfigsTest, minLayersDontTrigerSeamedSwitch) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90DeviceWithDifferentGroups,
                                                                     HWC_CONFIG_ID_90);
    RefreshRateConfigs::Policy policy;
    policy.defaultMode = refreshRateConfigs->getCurrentPolicy().defaultMode;
    policy.allowGroupSwitching = true;
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy(policy), 0);
    std::vector<LayerRequirement> layers = {
            {.name = "Min", .vote = LayerVoteType::Min, .weight = 1.f, .focused = true}};
    ASSERT_EQ(HWC_CONFIG_ID_90, refreshRateConfigs->getBestRefreshRate(layers, {}).getModeId());
}
TEST_F(RefreshRateConfigsTest, primaryVsAppRequestPolicy) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m30_60_90Device,
                                                                     HWC_CONFIG_ID_60);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    layers[0].name = "Test layer";
    struct Args {
        bool touch = false;
        bool focused = true;
    };
    auto getFrameRate = [&](LayerVoteType voteType, Fps fps, Args args = {}) -> DisplayModeId {
        layers[0].vote = voteType;
        layers[0].desiredRefreshRate = fps;
        layers[0].focused = args.focused;
        return refreshRateConfigs->getBestRefreshRate(layers, {.touch = args.touch}).getModeId();
    };
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy(
                      {HWC_CONFIG_ID_60, {30_Hz, 60_Hz}, {30_Hz, 90_Hz}}),
              0);
    EXPECT_EQ(HWC_CONFIG_ID_60, refreshRateConfigs->getBestRefreshRate({}, {}).getModeId());
    EXPECT_EQ(HWC_CONFIG_ID_60, getFrameRate(LayerVoteType::NoVote, 90_Hz));
    EXPECT_EQ(HWC_CONFIG_ID_30, getFrameRate(LayerVoteType::Min, 90_Hz));
    EXPECT_EQ(HWC_CONFIG_ID_60, getFrameRate(LayerVoteType::Max, 90_Hz));
    EXPECT_EQ(HWC_CONFIG_ID_60, getFrameRate(LayerVoteType::Heuristic, 90_Hz));
    EXPECT_EQ(HWC_CONFIG_ID_90, getFrameRate(LayerVoteType::ExplicitDefault, 90_Hz));
    EXPECT_EQ(HWC_CONFIG_ID_60, getFrameRate(LayerVoteType::ExplicitExactOrMultiple, 90_Hz));
    EXPECT_EQ(HWC_CONFIG_ID_60,
              getFrameRate(LayerVoteType::ExplicitDefault, 90_Hz, {.focused = false}));
    EXPECT_EQ(HWC_CONFIG_ID_60,
              getFrameRate(LayerVoteType::ExplicitExactOrMultiple, 90_Hz, {.focused = false}));
    EXPECT_EQ(HWC_CONFIG_ID_60, getFrameRate(LayerVoteType::Max, 90_Hz, {.touch = true}));
    EXPECT_EQ(HWC_CONFIG_ID_90,
              getFrameRate(LayerVoteType::ExplicitDefault, 90_Hz, {.touch = true}));
    EXPECT_EQ(HWC_CONFIG_ID_60,
              getFrameRate(LayerVoteType::ExplicitExactOrMultiple, 90_Hz, {.touch = true}));
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy(
                      {HWC_CONFIG_ID_60, {60_Hz, 60_Hz}, {60_Hz, 60_Hz}}),
              0);
    EXPECT_EQ(HWC_CONFIG_ID_60, getFrameRate(LayerVoteType::NoVote, 90_Hz));
    EXPECT_EQ(HWC_CONFIG_ID_60, getFrameRate(LayerVoteType::Min, 90_Hz));
    EXPECT_EQ(HWC_CONFIG_ID_60, getFrameRate(LayerVoteType::Max, 90_Hz));
    EXPECT_EQ(HWC_CONFIG_ID_60, getFrameRate(LayerVoteType::Heuristic, 90_Hz));
    EXPECT_EQ(HWC_CONFIG_ID_60, getFrameRate(LayerVoteType::ExplicitDefault, 90_Hz));
    EXPECT_EQ(HWC_CONFIG_ID_60, getFrameRate(LayerVoteType::ExplicitExactOrMultiple, 90_Hz));
}
TEST_F(RefreshRateConfigsTest, idle) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90Device,
                                                                     HWC_CONFIG_ID_60);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    layers[0].name = "Test layer";
    const auto getIdleFrameRate = [&](LayerVoteType voteType, bool touchActive) -> DisplayModeId {
        layers[0].vote = voteType;
        layers[0].desiredRefreshRate = 90_Hz;
        RefreshRateConfigs::GlobalSignals consideredSignals;
        const auto configId =
                refreshRateConfigs
                        ->getBestRefreshRate(layers, {.touch = touchActive, .idle = true},
                                             &consideredSignals)
                        .getModeId();
        EXPECT_EQ(!touchActive, consideredSignals.idle);
        return configId;
    };
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy(
                      {HWC_CONFIG_ID_60, {60_Hz, 90_Hz}, {60_Hz, 90_Hz}}),
              0);
    EXPECT_EQ(HWC_CONFIG_ID_90, getIdleFrameRate(LayerVoteType::NoVote, true));
    EXPECT_EQ(HWC_CONFIG_ID_90, getIdleFrameRate(LayerVoteType::Min, true));
    EXPECT_EQ(HWC_CONFIG_ID_90, getIdleFrameRate(LayerVoteType::Max, true));
    EXPECT_EQ(HWC_CONFIG_ID_90, getIdleFrameRate(LayerVoteType::Heuristic, true));
    EXPECT_EQ(HWC_CONFIG_ID_90,
              getIdleFrameRate(LayerVoteType::ExplicitDefault, true));
    EXPECT_EQ(HWC_CONFIG_ID_90,
              getIdleFrameRate(LayerVoteType::ExplicitExactOrMultiple, true));
    EXPECT_EQ(HWC_CONFIG_ID_90,
              refreshRateConfigs->getBestRefreshRate({}, {.touch = true, .idle = true})
                      .getModeId());
    refreshRateConfigs->setCurrentModeId(HWC_CONFIG_ID_90);
    EXPECT_EQ(HWC_CONFIG_ID_60, getIdleFrameRate(LayerVoteType::NoVote, false));
    EXPECT_EQ(HWC_CONFIG_ID_60, getIdleFrameRate(LayerVoteType::Min, false));
    EXPECT_EQ(HWC_CONFIG_ID_60, getIdleFrameRate(LayerVoteType::Max, false));
    EXPECT_EQ(HWC_CONFIG_ID_60, getIdleFrameRate(LayerVoteType::Heuristic, false));
    EXPECT_EQ(HWC_CONFIG_ID_60,
              getIdleFrameRate(LayerVoteType::ExplicitDefault, false));
    EXPECT_EQ(HWC_CONFIG_ID_60,
              getIdleFrameRate(LayerVoteType::ExplicitExactOrMultiple, false));
    EXPECT_EQ(HWC_CONFIG_ID_60,
              refreshRateConfigs->getBestRefreshRate({}, {.idle = true}).getModeId());
}
TEST_F(RefreshRateConfigsTest, findClosestKnownFrameRate) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90Device,
                                                                     HWC_CONFIG_ID_60);
    for (float fps = 1.0f; fps <= 120.0f; fps += 0.1f) {
        const auto knownFrameRate =
                findClosestKnownFrameRate(*refreshRateConfigs, Fps::fromValue(fps));
        Fps expectedFrameRate;
        if (fps < 26.91f) {
            expectedFrameRate = 24_Hz;
        } else if (fps < 37.51f) {
            expectedFrameRate = 30_Hz;
        } else if (fps < 52.51f) {
            expectedFrameRate = 45_Hz;
        } else if (fps < 66.01f) {
            expectedFrameRate = 60_Hz;
        } else if (fps < 81.01f) {
            expectedFrameRate = 72_Hz;
        } else {
            expectedFrameRate = 90_Hz;
        }
        EXPECT_EQ(expectedFrameRate, knownFrameRate);
    }
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_KnownFrameRate) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90Device,
                                                                     HWC_CONFIG_ID_60);
    struct Expectation {
        Fps fps;
        const RefreshRate& refreshRate;
    };
    const std::initializer_list<Expectation> knownFrameRatesExpectations = {
            {24_Hz, mExpected60Config}, {30_Hz, mExpected60Config}, {45_Hz, mExpected90Config},
            {60_Hz, mExpected60Config}, {72_Hz, mExpected90Config}, {90_Hz, mExpected90Config},
    };
    const auto knownFrameRateList = getKnownFrameRate(*refreshRateConfigs);
    const bool equal = std::equal(knownFrameRateList.begin(), knownFrameRateList.end(),
                                  knownFrameRatesExpectations.begin(),
                                  [](Fps fps, const Expectation& expected) {
                                      return isApproxEqual(fps, expected.fps);
                                  });
    EXPECT_TRUE(equal);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    auto& layer = layers[0];
    layer.vote = LayerVoteType::Heuristic;
    for (const auto& [fps, refreshRate] : knownFrameRatesExpectations) {
        layer.desiredRefreshRate = fps;
        EXPECT_EQ(refreshRate, refreshRateConfigs->getBestRefreshRate(layers, {}));
    }
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_ExplicitExact) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m30_60_72_90_120Device,
                                                                     HWC_CONFIG_ID_60);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}, {.weight = 0.5f}};
    auto& explicitExactLayer = layers[0];
    auto& explicitExactOrMultipleLayer = layers[1];
    explicitExactOrMultipleLayer.vote = LayerVoteType::ExplicitExactOrMultiple;
    explicitExactOrMultipleLayer.name = "ExplicitExactOrMultiple";
    explicitExactOrMultipleLayer.desiredRefreshRate = 60_Hz;
    explicitExactLayer.vote = LayerVoteType::ExplicitExact;
    explicitExactLayer.name = "ExplicitExact";
    explicitExactLayer.desiredRefreshRate = 30_Hz;
    EXPECT_EQ(mExpected30Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    EXPECT_EQ(mExpected30Config, refreshRateConfigs->getBestRefreshRate(layers, {.touch = true}));
    explicitExactOrMultipleLayer.desiredRefreshRate = 120_Hz;
    explicitExactLayer.desiredRefreshRate = 60_Hz;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    explicitExactLayer.desiredRefreshRate = 72_Hz;
    EXPECT_EQ(mExpected72Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    explicitExactLayer.desiredRefreshRate = 90_Hz;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    explicitExactLayer.desiredRefreshRate = 120_Hz;
    EXPECT_EQ(mExpected120Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_ExplicitExactEnableFrameRateOverride) {
    RefreshRateConfigs::Config config = {.enableFrameRateOverride = true};
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m30_60_72_90_120Device,
                                                                     HWC_CONFIG_ID_60, config);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}, {.weight = 0.5f}};
    auto& explicitExactLayer = layers[0];
    auto& explicitExactOrMultipleLayer = layers[1];
    explicitExactOrMultipleLayer.vote = LayerVoteType::ExplicitExactOrMultiple;
    explicitExactOrMultipleLayer.name = "ExplicitExactOrMultiple";
    explicitExactOrMultipleLayer.desiredRefreshRate = 60_Hz;
    explicitExactLayer.vote = LayerVoteType::ExplicitExact;
    explicitExactLayer.name = "ExplicitExact";
    explicitExactLayer.desiredRefreshRate = 30_Hz;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    EXPECT_EQ(mExpected120Config, refreshRateConfigs->getBestRefreshRate(layers, {.touch = true}));
    explicitExactOrMultipleLayer.desiredRefreshRate = 120_Hz;
    explicitExactLayer.desiredRefreshRate = 60_Hz;
    EXPECT_EQ(mExpected120Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    explicitExactLayer.desiredRefreshRate = 72_Hz;
    EXPECT_EQ(mExpected72Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    explicitExactLayer.desiredRefreshRate = 90_Hz;
    EXPECT_EQ(mExpected90Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    explicitExactLayer.desiredRefreshRate = 120_Hz;
    EXPECT_EQ(mExpected120Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_ReadsCached) {
    using GlobalSignals = RefreshRateConfigs::GlobalSignals;
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m30_60_72_90_120Device,
                                                                     HWC_CONFIG_ID_60);
    setLastBestRefreshRateInvocation(*refreshRateConfigs,
                                     GetBestRefreshRateInvocation{.globalSignals = {.touch = true,
                                                                                    .idle = true},
                                                                  .outSignalsConsidered =
                                                                          {.touch = true},
                                                                  .resultingBestRefreshRate =
                                                                          createRefreshRate(
                                                                                  mConfig90)});
    EXPECT_EQ(createRefreshRate(mConfig90),
              refreshRateConfigs->getBestRefreshRate({}, {.touch = true, .idle = true}));
    const GlobalSignals cachedSignalsConsidered{.touch = true};
    setLastBestRefreshRateInvocation(*refreshRateConfigs,
                                     GetBestRefreshRateInvocation{.globalSignals = {.touch = true,
                                                                                    .idle = true},
                                                                  .outSignalsConsidered =
                                                                          cachedSignalsConsidered,
                                                                  .resultingBestRefreshRate =
                                                                          createRefreshRate(
                                                                                  mConfig30)});
    GlobalSignals signalsConsidered;
    EXPECT_EQ(createRefreshRate(mConfig30),
              refreshRateConfigs->getBestRefreshRate({}, {.touch = true, .idle = true},
                                                     &signalsConsidered));
    EXPECT_EQ(cachedSignalsConsidered, signalsConsidered);
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_WritesCache) {
    using GlobalSignals = RefreshRateConfigs::GlobalSignals;
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m30_60_72_90_120Device,
                                                                     HWC_CONFIG_ID_60);
    ASSERT_FALSE(getLastBestRefreshRateInvocation(*refreshRateConfigs).has_value());
    GlobalSignals globalSignals{.touch = true, .idle = true};
    std::vector<LayerRequirement> layers = {{.weight = 1.f}, {.weight = 0.5f}};
    const auto lastResult =
            refreshRateConfigs->getBestRefreshRate(layers, globalSignals,
                                                                              nullptr);
    const auto lastInvocation = getLastBestRefreshRateInvocation(*refreshRateConfigs);
    ASSERT_TRUE(lastInvocation.has_value());
    ASSERT_EQ(layers, lastInvocation->layerRequirements);
    ASSERT_EQ(globalSignals, lastInvocation->globalSignals);
    ASSERT_EQ(lastResult, lastInvocation->resultingBestRefreshRate);
    GlobalSignals detaultSignals;
    ASSERT_FALSE(detaultSignals == lastInvocation->outSignalsConsidered);
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_ExplicitExactTouchBoost) {
    RefreshRateConfigs::Config config = {.enableFrameRateOverride = true};
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_120Device,
                                                                     HWC_CONFIG_ID_60, config);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}, {.weight = 0.5f}};
    auto& explicitExactLayer = layers[0];
    auto& explicitExactOrMultipleLayer = layers[1];
    explicitExactOrMultipleLayer.vote = LayerVoteType::ExplicitExactOrMultiple;
    explicitExactOrMultipleLayer.name = "ExplicitExactOrMultiple";
    explicitExactOrMultipleLayer.desiredRefreshRate = 60_Hz;
    explicitExactLayer.vote = LayerVoteType::ExplicitExact;
    explicitExactLayer.name = "ExplicitExact";
    explicitExactLayer.desiredRefreshRate = 30_Hz;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    EXPECT_EQ(mExpected120Config, refreshRateConfigs->getBestRefreshRate(layers, {.touch = true}));
    explicitExactOrMultipleLayer.vote = LayerVoteType::NoVote;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {.touch = true}));
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_FractionalRefreshRates_ExactAndDefault) {
    RefreshRateConfigs::Config config = {.enableFrameRateOverride = true};
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m24_25_30_50_60WithFracDevice,
                                                                     HWC_CONFIG_ID_60, config);
    std::vector<LayerRequirement> layers = {{.weight = 0.5f}, {.weight = 0.5f}};
    auto& explicitDefaultLayer = layers[0];
    auto& explicitExactOrMultipleLayer = layers[1];
    explicitExactOrMultipleLayer.vote = LayerVoteType::ExplicitExactOrMultiple;
    explicitExactOrMultipleLayer.name = "ExplicitExactOrMultiple";
    explicitExactOrMultipleLayer.desiredRefreshRate = 60_Hz;
    explicitDefaultLayer.vote = LayerVoteType::ExplicitDefault;
    explicitDefaultLayer.name = "ExplicitDefault";
    explicitDefaultLayer.desiredRefreshRate = 59.94_Hz;
    EXPECT_EQ(mExpected60Config, refreshRateConfigs->getBestRefreshRate(layers, {}));
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_deviceWithCloseRefreshRates) {
    constexpr int kMinRefreshRate = 10;
    constexpr int kMaxRefreshRate = 240;
    DisplayModes displayModes;
    for (int fps = kMinRefreshRate; fps < kMaxRefreshRate; fps++) {
        constexpr int32_t kGroup = 0;
        const auto refreshRate = Fps::fromValue(static_cast<float>(fps));
        displayModes.push_back(
                createDisplayMode(DisplayModeId(fps), kGroup, refreshRate.getPeriodNsecs()));
    }
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(displayModes,
                                                                     displayModes[0]->getId());
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    const auto testRefreshRate = [&](Fps fps, LayerVoteType vote) {
        layers[0].desiredRefreshRate = fps;
        layers[0].vote = vote;
        EXPECT_EQ(fps.getIntValue(),
                  refreshRateConfigs->getBestRefreshRate(layers, {}).getFps().getIntValue())
                << "Failed for " << ftl::enum_string(vote);
    };
    for (int fps = kMinRefreshRate; fps < kMaxRefreshRate; fps++) {
        const auto refreshRate = Fps::fromValue(static_cast<float>(fps));
        testRefreshRate(refreshRate, LayerVoteType::Heuristic);
        testRefreshRate(refreshRate, LayerVoteType::ExplicitDefault);
        testRefreshRate(refreshRate, LayerVoteType::ExplicitExactOrMultiple);
        testRefreshRate(refreshRate, LayerVoteType::ExplicitExact);
    }
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_conflictingVotes) {
    const DisplayModes displayModes = {
            createDisplayMode(DisplayModeId(0), 0, (43_Hz).getPeriodNsecs()),
            createDisplayMode(DisplayModeId(1), 0, (53_Hz).getPeriodNsecs()),
            createDisplayMode(DisplayModeId(2), 0, (55_Hz).getPeriodNsecs()),
            createDisplayMode(DisplayModeId(3), 0, (60_Hz).getPeriodNsecs()),
    };
    const RefreshRateConfigs::GlobalSignals globalSignals = {.touch = false, .idle = false};
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(displayModes,
                                                                     displayModes[0]->getId());
    const auto layers = std::vector<LayerRequirement>{
            LayerRequirement{
                    .vote = LayerVoteType::ExplicitDefault,
                    .desiredRefreshRate = 43_Hz,
                    .seamlessness = Seamlessness::SeamedAndSeamless,
                    .weight = 0.41f,
            },
            LayerRequirement{
                    .vote = LayerVoteType::ExplicitExactOrMultiple,
                    .desiredRefreshRate = 53_Hz,
                    .seamlessness = Seamlessness::SeamedAndSeamless,
                    .weight = 0.41f,
            },
    };
    EXPECT_EQ(53_Hz, refreshRateConfigs->getBestRefreshRate(layers, globalSignals).getFps());
}
TEST_F(RefreshRateConfigsTest, getBestRefreshRate_FractionalRefreshRates_ExactAndDefault) {
    RefreshRateConfigs::Config config = {.enableFrameRateOverride = true};
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m24_25_30_50_60WithFracDevice,
                                                                     HWC_CONFIG_ID_60, config);
    auto layers = std::vector<LayerRequirement>{LayerRequirement{.weight = 0.5f},
                                                LayerRequirement{.weight = 0.5f}};
    auto& explicitDefaultLayer = layers[0];
    auto& explicitExactOrMultipleLayer = layers[1];
    explicitExactOrMultipleLayer.vote = LayerVoteType::ExplicitExactOrMultiple;
    explicitExactOrMultipleLayer.name = "ExplicitExactOrMultiple";
    explicitExactOrMultipleLayer.desiredRefreshRate = Fps(60);
    explicitDefaultLayer.vote = LayerVoteType::ExplicitDefault;
    explicitDefaultLayer.name = "ExplicitDefault";
    explicitDefaultLayer.desiredRefreshRate = Fps(59.94f);
    EXPECT_EQ(mExpected60Config,
              refreshRateConfigs->getBestRefreshRate(layers, {.touch = false, .idle = false}));
}
TEST_F(RefreshRateConfigsTest, testComparisonOperator) {
    EXPECT_TRUE(mExpected60Config < mExpected90Config);
    EXPECT_FALSE(mExpected60Config < mExpected60Config);
    EXPECT_FALSE(mExpected90Config < mExpected90Config);
}
TEST_F(RefreshRateConfigsTest, testKernelIdleTimerAction) {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_90Device,
                                                                     HWC_CONFIG_ID_90);
    EXPECT_EQ(KernelIdleTimerAction::TurnOn, refreshRateConfigs->getIdleTimerAction());
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy({HWC_CONFIG_ID_60, {60_Hz, 90_Hz}}), 0);
    EXPECT_EQ(KernelIdleTimerAction::TurnOn, refreshRateConfigs->getIdleTimerAction());
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy({HWC_CONFIG_ID_60, {60_Hz, 60_Hz}}), 0);
    EXPECT_EQ(KernelIdleTimerAction::TurnOff, refreshRateConfigs->getIdleTimerAction());
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy({HWC_CONFIG_ID_90, {90_Hz, 90_Hz}}), 0);
    EXPECT_EQ(KernelIdleTimerAction::TurnOff, refreshRateConfigs->getIdleTimerAction());
}
TEST_F(RefreshRateConfigsTest, testKernelIdleTimerActionFor120Hz) {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m60_120Device,
                                                                     HWC_CONFIG_ID_120);
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy({HWC_CONFIG_ID_60, {0_Hz, 60_Hz}}), 0);
    EXPECT_EQ(KernelIdleTimerAction::TurnOn, refreshRateConfigs->getIdleTimerAction());
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy({HWC_CONFIG_ID_60, {60_Hz, 60_Hz}}), 0);
    EXPECT_EQ(KernelIdleTimerAction::TurnOff, refreshRateConfigs->getIdleTimerAction());
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy({HWC_CONFIG_ID_60, {60_Hz, 120_Hz}}), 0);
    EXPECT_EQ(KernelIdleTimerAction::TurnOn, refreshRateConfigs->getIdleTimerAction());
    ASSERT_GE(refreshRateConfigs->setDisplayManagerPolicy({HWC_CONFIG_ID_120, {120_Hz, 120_Hz}}),
              0);
    EXPECT_EQ(KernelIdleTimerAction::TurnOff, refreshRateConfigs->getIdleTimerAction());
}
TEST_F(RefreshRateConfigsTest, getFrameRateDivider) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m30_60_72_90_120Device,
                                                                     HWC_CONFIG_ID_30);
    const auto frameRate = 30_Hz;
    Fps displayRefreshRate = refreshRateConfigs->getCurrentRefreshRate().getFps();
    EXPECT_EQ(1, RefreshRateConfigs::getFrameRateDivider(displayRefreshRate, frameRate));
    refreshRateConfigs->setCurrentModeId(HWC_CONFIG_ID_60);
    displayRefreshRate = refreshRateConfigs->getCurrentRefreshRate().getFps();
    EXPECT_EQ(2, RefreshRateConfigs::getFrameRateDivider(displayRefreshRate, frameRate));
    refreshRateConfigs->setCurrentModeId(HWC_CONFIG_ID_72);
    displayRefreshRate = refreshRateConfigs->getCurrentRefreshRate().getFps();
    EXPECT_EQ(0, RefreshRateConfigs::getFrameRateDivider(displayRefreshRate, frameRate));
    refreshRateConfigs->setCurrentModeId(HWC_CONFIG_ID_90);
    displayRefreshRate = refreshRateConfigs->getCurrentRefreshRate().getFps();
    EXPECT_EQ(3, RefreshRateConfigs::getFrameRateDivider(displayRefreshRate, frameRate));
    refreshRateConfigs->setCurrentModeId(HWC_CONFIG_ID_120);
    displayRefreshRate = refreshRateConfigs->getCurrentRefreshRate().getFps();
    EXPECT_EQ(4, RefreshRateConfigs::getFrameRateDivider(displayRefreshRate, frameRate));
    refreshRateConfigs->setCurrentModeId(HWC_CONFIG_ID_90);
    displayRefreshRate = refreshRateConfigs->getCurrentRefreshRate().getFps();
    EXPECT_EQ(4, RefreshRateConfigs::getFrameRateDivider(displayRefreshRate, 22.5_Hz));
    EXPECT_EQ(0, RefreshRateConfigs::getFrameRateDivider(24_Hz, 25_Hz));
    EXPECT_EQ(0, RefreshRateConfigs::getFrameRateDivider(24_Hz, 23.976_Hz));
    EXPECT_EQ(0, RefreshRateConfigs::getFrameRateDivider(30_Hz, 29.97_Hz));
    EXPECT_EQ(0, RefreshRateConfigs::getFrameRateDivider(60_Hz, 59.94_Hz));
}
TEST_F(RefreshRateConfigsTest, isFractionalPairOrMultiple) {
    EXPECT_TRUE(RefreshRateConfigs::isFractionalPairOrMultiple(23.976_Hz, 24_Hz));
    EXPECT_TRUE(RefreshRateConfigs::isFractionalPairOrMultiple(24_Hz, 23.976_Hz));
    EXPECT_TRUE(RefreshRateConfigs::isFractionalPairOrMultiple(29.97_Hz, 30_Hz));
    EXPECT_TRUE(RefreshRateConfigs::isFractionalPairOrMultiple(30_Hz, 29.97_Hz));
    EXPECT_TRUE(RefreshRateConfigs::isFractionalPairOrMultiple(59.94_Hz, 60_Hz));
    EXPECT_TRUE(RefreshRateConfigs::isFractionalPairOrMultiple(60_Hz, 59.94_Hz));
    EXPECT_TRUE(RefreshRateConfigs::isFractionalPairOrMultiple(29.97_Hz, 60_Hz));
    EXPECT_TRUE(RefreshRateConfigs::isFractionalPairOrMultiple(60_Hz, 29.97_Hz));
    EXPECT_TRUE(RefreshRateConfigs::isFractionalPairOrMultiple(59.94_Hz, 30_Hz));
    EXPECT_TRUE(RefreshRateConfigs::isFractionalPairOrMultiple(30_Hz, 59.94_Hz));
    const auto refreshRates = {23.976_Hz, 24_Hz, 25_Hz, 29.97_Hz, 30_Hz, 50_Hz, 59.94_Hz, 60_Hz};
    for (auto refreshRate : refreshRates) {
        EXPECT_FALSE(RefreshRateConfigs::isFractionalPairOrMultiple(refreshRate, refreshRate));
    }
    EXPECT_FALSE(RefreshRateConfigs::isFractionalPairOrMultiple(24_Hz, 25_Hz));
    EXPECT_FALSE(RefreshRateConfigs::isFractionalPairOrMultiple(23.978_Hz, 25_Hz));
    EXPECT_FALSE(RefreshRateConfigs::isFractionalPairOrMultiple(29.97_Hz, 59.94_Hz));
}
TEST_F(RefreshRateConfigsTest, isFractionalPairOrMultiple) {
    EXPECT_TRUE(RefreshRateConfigs::isFractionalPairOrMultiple(Fps(23.976f), Fps(24.f)));
    EXPECT_TRUE(RefreshRateConfigs::isFractionalPairOrMultiple(Fps(24.f), Fps(23.976f)));
    EXPECT_TRUE(RefreshRateConfigs::isFractionalPairOrMultiple(Fps(29.97f), Fps(30.f)));
    EXPECT_TRUE(RefreshRateConfigs::isFractionalPairOrMultiple(Fps(30.f), Fps(29.97f)));
    EXPECT_TRUE(RefreshRateConfigs::isFractionalPairOrMultiple(Fps(59.94f), Fps(60.f)));
    EXPECT_TRUE(RefreshRateConfigs::isFractionalPairOrMultiple(Fps(60.f), Fps(59.94f)));
    EXPECT_TRUE(RefreshRateConfigs::isFractionalPairOrMultiple(Fps(29.97f), Fps(60.f)));
    EXPECT_TRUE(RefreshRateConfigs::isFractionalPairOrMultiple(Fps(60.f), Fps(29.97f)));
    EXPECT_TRUE(RefreshRateConfigs::isFractionalPairOrMultiple(Fps(59.94f), Fps(30.f)));
    EXPECT_TRUE(RefreshRateConfigs::isFractionalPairOrMultiple(Fps(30.f), Fps(59.94f)));
    const std::vector<float> refreshRates = {23.976f, 24.f, 25.f, 29.97f, 30.f, 50.f, 59.94f, 60.f};
    for (auto refreshRate : refreshRates) {
        EXPECT_FALSE(
                RefreshRateConfigs::isFractionalPairOrMultiple(Fps(refreshRate), Fps(refreshRate)));
    }
    EXPECT_FALSE(RefreshRateConfigs::isFractionalPairOrMultiple(Fps(24.f), Fps(25.f)));
    EXPECT_FALSE(RefreshRateConfigs::isFractionalPairOrMultiple(Fps(23.978f), Fps(25.f)));
    EXPECT_FALSE(RefreshRateConfigs::isFractionalPairOrMultiple(Fps(29.97f), Fps(59.94f)));
}
TEST_F(RefreshRateConfigsTest, getFrameRateOverrides_noLayers) {
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m30_60_72_90_120Device,
                                                 HWC_CONFIG_ID_120);
    ASSERT_TRUE(refreshRateConfigs->getFrameRateOverrides({}, 120_Hz, {}).empty());
}
TEST_F(RefreshRateConfigsTest, getFrameRateOverrides_60on120) {
    RefreshRateConfigs::Config config = {.enableFrameRateOverride = true};
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m30_60_72_90_120Device,
                                                 HWC_CONFIG_ID_120, config);
    std::vector<LayerRequirement> layers = {{.weight = 1.f}};
    layers[0].name = "Test layer";
    layers[0].ownerUid = 1234;
    layers[0].desiredRefreshRate = 60_Hz;
    layers[0].vote = LayerVoteType::ExplicitDefault;
    auto frameRateOverrides = refreshRateConfigs->getFrameRateOverrides(layers, 120_Hz, {});
    ASSERT_EQ(1, frameRateOverrides.size());
    ASSERT_EQ(1, frameRateOverrides.count(1234));
    ASSERT_EQ(60_Hz, frameRateOverrides.at(1234));
    layers[0].vote = LayerVoteType::ExplicitExactOrMultiple;
    frameRateOverrides = refreshRateConfigs->getFrameRateOverrides(layers, 120_Hz, {});
    ASSERT_EQ(1, frameRateOverrides.size());
    ASSERT_EQ(1, frameRateOverrides.count(1234));
    ASSERT_EQ(60_Hz, frameRateOverrides.at(1234));
    layers[0].vote = LayerVoteType::NoVote;
    frameRateOverrides = refreshRateConfigs->getFrameRateOverrides(layers, 120_Hz, {});
    ASSERT_TRUE(frameRateOverrides.empty());
    layers[0].vote = LayerVoteType::Min;
    frameRateOverrides = refreshRateConfigs->getFrameRateOverrides(layers, 120_Hz, {});
    ASSERT_TRUE(frameRateOverrides.empty());
    layers[0].vote = LayerVoteType::Max;
    frameRateOverrides = refreshRateConfigs->getFrameRateOverrides(layers, 120_Hz, {});
    ASSERT_TRUE(frameRateOverrides.empty());
    layers[0].vote = LayerVoteType::Heuristic;
    frameRateOverrides = refreshRateConfigs->getFrameRateOverrides(layers, 120_Hz, {});
    ASSERT_TRUE(frameRateOverrides.empty());
}
TEST_F(RefreshRateConfigsTest, getFrameRateOverrides_twoUids) {
    RefreshRateConfigs::Config config = {.enableFrameRateOverride = true};
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m30_60_72_90_120Device,
                                                 HWC_CONFIG_ID_120, config);
    std::vector<LayerRequirement> layers = {{.ownerUid = 1234, .weight = 1.f},
                                            {.ownerUid = 5678, .weight = 1.f}};
    layers[0].name = "Test layer 1234";
    layers[0].desiredRefreshRate = 60_Hz;
    layers[0].vote = LayerVoteType::ExplicitDefault;
    layers[1].name = "Test layer 5678";
    layers[1].desiredRefreshRate = 30_Hz;
    layers[1].vote = LayerVoteType::ExplicitDefault;
    auto frameRateOverrides = refreshRateConfigs->getFrameRateOverrides(layers, 120_Hz, {});
    ASSERT_EQ(2, frameRateOverrides.size());
    ASSERT_EQ(1, frameRateOverrides.count(1234));
    ASSERT_EQ(60_Hz, frameRateOverrides.at(1234));
    ASSERT_EQ(1, frameRateOverrides.count(5678));
    ASSERT_EQ(30_Hz, frameRateOverrides.at(5678));
    layers[1].vote = LayerVoteType::Heuristic;
    frameRateOverrides = refreshRateConfigs->getFrameRateOverrides(layers, 120_Hz, {});
    ASSERT_EQ(1, frameRateOverrides.size());
    ASSERT_EQ(1, frameRateOverrides.count(1234));
    ASSERT_EQ(60_Hz, frameRateOverrides.at(1234));
    layers[1].ownerUid = 1234;
    frameRateOverrides = refreshRateConfigs->getFrameRateOverrides(layers, 120_Hz, {});
    ASSERT_TRUE(frameRateOverrides.empty());
}
TEST_F(RefreshRateConfigsTest, getFrameRateOverrides_touch) {
    RefreshRateConfigs::Config config = {.enableFrameRateOverride = true};
    auto refreshRateConfigs =
            std::make_unique<RefreshRateConfigs>(m30_60_72_90_120Device,
                                                 HWC_CONFIG_ID_120, config);
    std::vector<LayerRequirement> layers = {{.ownerUid = 1234, .weight = 1.f}};
    layers[0].name = "Test layer";
    layers[0].desiredRefreshRate = 60_Hz;
    layers[0].vote = LayerVoteType::ExplicitDefault;
    auto frameRateOverrides = refreshRateConfigs->getFrameRateOverrides(layers, 120_Hz, {});
    ASSERT_EQ(1, frameRateOverrides.size());
    ASSERT_EQ(1, frameRateOverrides.count(1234));
    ASSERT_EQ(60_Hz, frameRateOverrides.at(1234));
    frameRateOverrides = refreshRateConfigs->getFrameRateOverrides(layers, 120_Hz, {.touch = true});
    ASSERT_EQ(1, frameRateOverrides.size());
    ASSERT_EQ(1, frameRateOverrides.count(1234));
    ASSERT_EQ(60_Hz, frameRateOverrides.at(1234));
    layers[0].vote = LayerVoteType::ExplicitExact;
    frameRateOverrides = refreshRateConfigs->getFrameRateOverrides(layers, 120_Hz, {});
    ASSERT_EQ(1, frameRateOverrides.size());
    ASSERT_EQ(1, frameRateOverrides.count(1234));
    ASSERT_EQ(60_Hz, frameRateOverrides.at(1234));
    frameRateOverrides = refreshRateConfigs->getFrameRateOverrides(layers, 120_Hz, {.touch = true});
    ASSERT_EQ(1, frameRateOverrides.size());
    ASSERT_EQ(1, frameRateOverrides.count(1234));
    ASSERT_EQ(60_Hz, frameRateOverrides.at(1234));
    layers[0].vote = LayerVoteType::ExplicitExactOrMultiple;
    frameRateOverrides = refreshRateConfigs->getFrameRateOverrides(layers, 120_Hz, {});
    ASSERT_EQ(1, frameRateOverrides.size());
    ASSERT_EQ(1, frameRateOverrides.count(1234));
    ASSERT_EQ(60_Hz, frameRateOverrides.at(1234));
    frameRateOverrides = refreshRateConfigs->getFrameRateOverrides(layers, 120_Hz, {.touch = true});
    ASSERT_TRUE(frameRateOverrides.empty());
}
}
}
#pragma clang diagnostic pop
