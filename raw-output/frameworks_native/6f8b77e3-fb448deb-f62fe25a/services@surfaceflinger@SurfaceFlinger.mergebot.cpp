/*
 * Copyright (C) 2007 The Android Open Source Project
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
// TODO(b/129481165): remove the #pragma below and fix conversion issues

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wextra"
//#define LOG_NDEBUG 0

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include "SurfaceFlinger.h"
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android/configuration.h>
#include <android/gui/IDisplayEventConnection.h>
#include <android/gui/StaticDisplayInfo.h>
#include <android/hardware/configstore/1.0/ISurfaceFlingerConfigs.h>
#include <android/hardware/configstore/1.1/ISurfaceFlingerConfigs.h>
#include <android/hardware/configstore/1.1/types.h>
#include <android/hardware/power/Boost.h>
#include <android/native_window.h>
#include <android/os/IInputFlinger.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/PermissionCache.h>
#include <compositionengine/CompositionEngine.h>
#include <compositionengine/CompositionRefreshArgs.h>
#include <compositionengine/Display.h>
#include <compositionengine/DisplayColorProfile.h>
#include <compositionengine/DisplayCreationArgs.h>
#include <compositionengine/LayerFECompositionState.h>
#include <compositionengine/OutputLayer.h>
#include <compositionengine/RenderSurface.h>
#include <compositionengine/impl/OutputCompositionState.h>
#include <compositionengine/impl/OutputLayerCompositionState.h>
#include <configstore/Utils.h>
#include <cutils/compiler.h>
#include <cutils/properties.h>
#include <ftl/algorithm.h>
#include <ftl/fake_guard.h>
#include <ftl/future.h>
#include <ftl/small_map.h>
#include <ftl/unit.h>
#include <gui/AidlStatusUtil.h>
#include <gui/BufferQueue.h>
#include <gui/DebugEGLImageTracker.h>
#include <gui/IProducerListener.h>
#include <gui/LayerDebugInfo.h>
#include <gui/LayerMetadata.h>
#include <gui/LayerState.h>
#include <gui/Surface.h>
#include <gui/TraceUtils.h>
#include <hidl/ServiceManagement.h>
#include <layerproto/LayerProtoParser.h>
#include <log/log.h>
#include <private/android_filesystem_config.h>
#include <private/gui/SyncFeatures.h>
#include <processgroup/processgroup.h>
#include <renderengine/RenderEngine.h>
#include <renderengine/impl/ExternalTexture.h>
#include <sys/types.h>
#include <ui/ColorSpace.h>
#include <ui/DataspaceUtils.h>
#include <ui/DebugUtils.h>
#include <ui/DisplayId.h>
#include <ui/DisplayMode.h>
#include <ui/DisplayStatInfo.h>
#include <ui/DisplayState.h>
#include <ui/DynamicDisplayInfo.h>
#include <ui/GraphicBufferAllocator.h>
#include <ui/PixelFormat.h>
#include <ui/StaticDisplayInfo.h>
#include <utils/StopWatch.h>
#include <utils/String16.h>
#include <utils/String8.h>
#include <utils/Timers.h>
#include <utils/misc.h>
#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include "BufferLayer.h"
#include "BufferQueueLayer.h"
#include "ContainerLayer.h"
#include "BufferStateLayer.h"
#include <ui/DisplayIdentification.h>
#include "BackgroundExecutor.h"
#include "Client.h"
#include "EffectLayer.h"
#include "Colorizer.h"
#include "Display/DisplayMap.h"
#include "DisplayDevice.h"
#include "DisplayHardware/ComposerHal.h"
#include "DisplayHardware/FramebufferSurface.h"
#include "DisplayHardware/HWComposer.h"
#include "DisplayHardware/Hal.h"
#include "DisplayHardware/PowerAdvisor.h"
#include "MonitoredProducer.h"
#include "DisplayHardware/VirtualDisplaySurface.h"
#include "DisplayRenderArea.h"
#include "Effects/Daltonizer.h"
#include "FlagManager.h"
#include "FpsReporter.h"
#include "FrameTimeline/FrameTimeline.h"
#include "FrameTracer/FrameTracer.h"
#include "HdrLayerInfoReporter.h"
#include "Layer.h"
#include "LayerProtoHelper.h"
#include "LayerRenderArea.h"
#include "LayerVector.h"
#include "MutexUtils.h"
#include "NativeWindowSurface.h"
#include "RefreshRateOverlay.h"
#include "RegionSamplingThread.h"
#include "Scheduler/DispSyncSource.h"
#include "Scheduler/EventThread.h"
#include "Scheduler/LayerHistory.h"
#include "Scheduler/Scheduler.h"
#include "Scheduler/VsyncConfiguration.h"
#include "Scheduler/VsyncController.h"
#include "StartPropertySetThread.h"
#include "SurfaceFlingerProperties.h"
#include "SurfaceInterceptor.h"
#include "TimeStats/TimeStats.h"
#include "TunnelModeEnabledReporter.h"
#include "WindowInfosListenerInvoker.h"
#include <aidl/android/hardware/graphics/common/DisplayDecorationSupport.h>
#include <aidl/android/hardware/graphics/composer3/DisplayCapability.h>
#include <aidl/android/hardware/graphics/composer3/RenderIntent.h>
#undef NO_THREAD_SAFETY_ANALYSIS
#define NO_THREAD_SAFETY_ANALYSIS \
    _Pragma("GCC error \"Prefer <ftl/fake_guard.h> or MutexUtils.h helpers.\"")
// To enable layer borders in the system, change the below flag to true.

#undef DOES_CONTAIN_BORDER
#define DOES_CONTAIN_BORDER false

namespace android {

using namespace std::chrono_literals;
using namespace std::string_literals;

using namespace hardware::configstore;
using namespace hardware::configstore::V1_0;
using namespace sysprop;

using aidl::android::hardware::graphics::common::DisplayDecorationSupport;
using aidl::android::hardware::graphics::composer3::Capability;
using aidl::android::hardware::graphics::composer3::DisplayCapability;
using CompositionStrategyPredictionState = android::compositionengine::impl::
        OutputCompositionState::CompositionStrategyPredictionState;

using base::StringAppendF;
using display::PhysicalDisplay;
using display::PhysicalDisplays;
using gui::DisplayInfo;
using gui::GameMode;
using gui::IDisplayEventConnection;
using gui::IWindowInfosListener;
using gui::LayerMetadata;
using gui::WindowInfo;
using gui::aidl_utils::binderStatusFromStatusT;
using ui::ColorMode;
using ui::Dataspace;
using ui::DisplayPrimaries;
using ui::RenderIntent;

using KernelIdleTimerController = scheduler::RefreshRateConfigs::KernelIdleTimerController;

namespace {

bool isWideColorMode(const ColorMode colorMode) {
    switch (colorMode) {
        case ColorMode::DISPLAY_P3:
        case ColorMode::ADOBE_RGB:
        case ColorMode::DCI_P3:
        case ColorMode::BT2020:
        case ColorMode::DISPLAY_BT2020:
        case ColorMode::BT2100_PQ:
        case ColorMode::BT2100_HLG:
            return true;
        case ColorMode::NATIVE:
        case ColorMode::STANDARD_BT601_625:
        case ColorMode::STANDARD_BT601_625_UNADJUSTED:
        case ColorMode::STANDARD_BT601_525:
        case ColorMode::STANDARD_BT601_525_UNADJUSTED:
        case ColorMode::STANDARD_BT709:
        case ColorMode::SRGB:
            return false;
    }
    return false;
}

// TODO(b/141333600): Consolidate with DisplayMode::Builder::getDefaultDensity.
constexpr float FALLBACK_DENSITY = ACONFIGURATION_DENSITY_TV;

float getDensityFromProperty(const char* property, bool required) {
    char value[PROPERTY_VALUE_MAX];
    const float density = property_get(property, value, nullptr) > 0 ? std::atof(value) : 0.f;
    if (!density && required) {
        ALOGE("%s must be defined as a build property", property);
        return FALLBACK_DENSITY;
    }
    return density;
}

// Currently we only support V0_SRGB and DISPLAY_P3 as composition preference.
bool validateCompositionDataspace(Dataspace dataspace) {
    return dataspace == Dataspace::V0_SRGB || dataspace == Dataspace::DISPLAY_P3;
}

std::chrono::milliseconds getIdleTimerTimeout(DisplayId displayId) {
    const auto displayIdleTimerMsKey = [displayId] {
        std::stringstream ss;
        ss << "debug.sf.set_idle_timer_ms_" << displayId.value;
        return ss.str();
    }();

    const int32_t displayIdleTimerMs = base::GetIntProperty(displayIdleTimerMsKey, 0);
    if (displayIdleTimerMs > 0) {
        return std::chrono::milliseconds(displayIdleTimerMs);
    }

    const int32_t setIdleTimerMs = base::GetIntProperty("debug.sf.set_idle_timer_ms", 0);
    const int32_t millis = setIdleTimerMs ? setIdleTimerMs : sysprop::set_idle_timer_ms(0);
    return std::chrono::milliseconds(millis);
}

bool getKernelIdleTimerSyspropConfig(DisplayId displayId) {
    const auto displaySupportKernelIdleTimerKey = [displayId] {
        std::stringstream ss;
        ss << "debug.sf.support_kernel_idle_timer_" << displayId.value;
        return ss.str();
    }();

    const auto displaySupportKernelIdleTimer =
            base::GetBoolProperty(displaySupportKernelIdleTimerKey, false);
    return displaySupportKernelIdleTimer || sysprop::support_kernel_idle_timer(false);
}

} // namespace anonymous

// ---------------------------------------------------------------------------

const String16 sHardwareTest("android.permission.HARDWARE_TEST");
const String16 sAccessSurfaceFlinger("android.permission.ACCESS_SURFACE_FLINGER");
const String16 sRotateSurfaceFlinger("android.permission.ROTATE_SURFACE_FLINGER");
const String16 sReadFramebuffer("android.permission.READ_FRAME_BUFFER");
const String16 sControlDisplayBrightness("android.permission.CONTROL_DISPLAY_BRIGHTNESS");
const String16 sDump("android.permission.DUMP");
const String16 sCaptureBlackoutContent("android.permission.CAPTURE_BLACKOUT_CONTENT");
const String16 sInternalSystemWindow("android.permission.INTERNAL_SYSTEM_WINDOW");

const char* KERNEL_IDLE_TIMER_PROP = "graphics.display.kernel_idle_timer.enabled";

static const int MAX_TRACING_MEMORY = 1024 * 1024 * 1024;                                                          // 1GB
                                                          // ---------------------------------------------------------------------------
status_t SurfaceFlinger::getSupportedFrameTimestamps(
        std::vector<FrameEvent>* outSupported) const {
    *outSupported = {
        FrameEvent::REQUESTED_PRESENT,
        FrameEvent::ACQUIRE,
        FrameEvent::LATCH,
        FrameEvent::FIRST_REFRESH_START,
        FrameEvent::LAST_REFRESH_START,
        FrameEvent::GPU_COMPOSITION_DONE,
        FrameEvent::DEQUEUE_READY,
        FrameEvent::RELEASE,
    };

                            FTL_FAKE_GUARD(kMainThreadContext) = default;{
                            ALOGI(  "SurfaceFlinger's main thread ready to run. "
                            "Initializing graphics H/W...");
                            Mutex::Autolock lock(mStateLock);
                            
                            // Get a RenderEngine for the given display / config (can't fail)
                            // TODO(b/77156734): We need to stop casting and use HAL types when possible.
                            // Sending maxFrameBufferAcquiredBuffers as the cache size is tightly tuned to single-display.
                            auto builder = renderengine::RenderEngineCreationArgs::Builder()
                            .setPixelFormat(static_cast<int32_t>(defaultCompositionPixelFormat))
                            .setImageCacheSize(maxFrameBufferAcquiredBuffers)
                            .setUseColorManagerment(useColorManagement)
                            .setEnableProtectedContext(enable_protected_contents(false))
                            .setPrecacheToneMapperShaderOnly(false)
                            .setSupportsBackgroundBlur(mSupportsBlur)
                            .setContextPriority(
                                   useContextPriority
                                           ? renderengine::RenderEngine::ContextPriority::REALTIME
                                           : renderengine::RenderEngine::ContextPriority::MEDIUM);
                            if (auto type = chooseRenderEngineTypeViaSysProp()) {
                            builder.setRenderEngineType(type.value());
                            }
                            mCompositionEngine->setRenderEngine(renderengine::RenderEngine::create(builder.build()));
                            mMaxRenderTargetSize =
                            std::min(getRenderEngine().getMaxTextureSize(), getRenderEngine().getMaxViewportDims());
                            
                            // Set SF main policy after initializing RenderEngine which has its own policy.
                            if (!SetTaskProfiles(0, {"SFMainPolicy"})) {
                            ALOGW("Failed to set main task profile");
                            }
                            
                            mCompositionEngine->setTimeStats(mTimeStats);
                            mCompositionEngine->setHwComposer(getFactory().createHWComposer(mHwcServiceName));
                            mCompositionEngine->getHwComposer().setCallback(*this);
                            ClientCache::getInstance().setRenderEngine(&getRenderEngine());
                            
                            enableLatchUnsignaledConfig = getLatchUnsignaledConfig();
                            
                            if (base::GetBoolProperty("debug.sf.enable_hwc_vds"s, false)) {
                            enableHalVirtualDisplays(true);
                            }
                            
                            // Process hotplug for displays connected at boot.
                            LOG_ALWAYS_FATAL_IF(!configureLocked(),
                            "Initial display configuration failed: HWC did not hotplug");
                            
                            // Commit primary display.
                            sp<const DisplayDevice> display;
                            if (const auto indexOpt = mCurrentState.getDisplayIndex(getPrimaryDisplayIdLocked())) {
                            const auto& displays = mCurrentState.displays;
                            
                            const auto& token = displays.keyAt(*indexOpt);
                            const auto& state = displays.valueAt(*indexOpt);
                            
                            processDisplayAdded(token, state);
                            mDrawingState.displays.add(token, state);
                            
                            display = getDefaultDisplayDeviceLocked();
                            }
                            
                            LOG_ALWAYS_FATAL_IF(!display, "Failed to configure the primary display");
                            LOG_ALWAYS_FATAL_IF(!getHwComposer().isConnected(display->getPhysicalId()),
                            "Primary display is disconnected");
                            
                            // TODO(b/241285876): The Scheduler needlessly depends on creating the CompositionEngine part of
                            // the DisplayDevice, hence the above commit of the primary display. Remove that special case by
                            // initializing the Scheduler after configureLocked, once decoupled from DisplayDevice.
                            initScheduler(display);
                            dispatchDisplayHotplugEvent(display->getPhysicalId(), true);
                            
                            // Commit secondary display(s).
                            processDisplayChangesLocked();
                            
                            // initialize our drawing state
                            mDrawingState = mCurrentState;
                            
                            // set initial conditions (e.g. unblank default device)
                            initializeDisplays();
                            
                            mPowerAdvisor->init();
                            
                            char primeShaderCache[PROPERTY_VALUE_MAX];
                            property_get("service.sf.prime_shader_cache", primeShaderCache, "1");
                            if (atoi(primeShaderCache)) {
                            if (setSchedFifo(false) != NO_ERROR) {
                            ALOGW("Can't set SCHED_OTHER for primeCache");
                            }
                            
                            mRenderEnginePrimeCacheFuture = getRenderEngine().primeCache();
                            
                            if (setSchedFifo(true) != NO_ERROR) {
                            ALOGW("Can't set SCHED_OTHER for primeCache");
                            }
                            }
                            
                            onActiveDisplaySizeChanged(display);
                            
                            // Inform native graphics APIs whether the present timestamp is supported:
                            
                            const bool presentFenceReliable =
                            !getHwComposer().hasCapability(Capability::PRESENT_FENCE_IS_NOT_RELIABLE);
                            mStartPropertySetThread = getFactory().createStartPropertySetThread(presentFenceReliable);
                            
                            if (mStartPropertySetThread->Start() != NO_ERROR) {
                            ALOGE("Run StartPropertySetThread failed!");
                            }
                            
                            ALOGV("Done initializing");
                            }
                            
status_t SurfaceFlinger::getSupportedFrameTimestamps(
        std::vector<FrameEvent>* outSupported) const {
    *outSupported = {
        FrameEvent::REQUESTED_PRESENT,
        FrameEvent::ACQUIRE,
        FrameEvent::LATCH,
        FrameEvent::FIRST_REFRESH_START,
        FrameEvent::LAST_REFRESH_START,
        FrameEvent::GPU_COMPOSITION_DONE,
        FrameEvent::DEQUEUE_READY,
        FrameEvent::RELEASE,
    };

status_t SurfaceFlinger::getSupportedFrameTimestamps(
        std::vector<FrameEvent>* outSupported) const {
    *outSupported = {
        FrameEvent::REQUESTED_PRESENT,
        FrameEvent::ACQUIRE,
        FrameEvent::LATCH,
        FrameEvent::FIRST_REFRESH_START,
        FrameEvent::LAST_REFRESH_START,
        FrameEvent::GPU_COMPOSITION_DONE,
        FrameEvent::DEQUEUE_READY,
        FrameEvent::RELEASE,
    };

status_t SurfaceFlinger::getSupportedFrameTimestamps(
        std::vector<FrameEvent>* outSupported) const {
    *outSupported = {
        FrameEvent::REQUESTED_PRESENT,
        FrameEvent::ACQUIRE,
        FrameEvent::LATCH,
        FrameEvent::FIRST_REFRESH_START,
        FrameEvent::LAST_REFRESH_START,
        FrameEvent::GPU_COMPOSITION_DONE,
        FrameEvent::DEQUEUE_READY,
        FrameEvent::RELEASE,
    };

status_t SurfaceFlinger::getSupportedFrameTimestamps(
        std::vector<FrameEvent>* outSupported) const {
    *outSupported = {
        FrameEvent::REQUESTED_PRESENT,
        FrameEvent::ACQUIRE,
        FrameEvent::LATCH,
        FrameEvent::FIRST_REFRESH_START,
        FrameEvent::LAST_REFRESH_START,
        FrameEvent::GPU_COMPOSITION_DONE,
        FrameEvent::DEQUEUE_READY,
        FrameEvent::RELEASE,
    };

status_t SurfaceFlinger::getSupportedFrameTimestamps(
        std::vector<FrameEvent>* outSupported) const {
    *outSupported = {
        FrameEvent::REQUESTED_PRESENT,
        FrameEvent::ACQUIRE,
        FrameEvent::LATCH,
        FrameEvent::FIRST_REFRESH_START,
        FrameEvent::LAST_REFRESH_START,
        FrameEvent::GPU_COMPOSITION_DONE,
        FrameEvent::DEQUEUE_READY,
        FrameEvent::RELEASE,
    };

status_t SurfaceFlinger::getSupportedFrameTimestamps(
        std::vector<FrameEvent>* outSupported) const {
    *outSupported = {
        FrameEvent::REQUESTED_PRESENT,
        FrameEvent::ACQUIRE,
        FrameEvent::LATCH,
        FrameEvent::FIRST_REFRESH_START,
        FrameEvent::LAST_REFRESH_START,
        FrameEvent::GPU_COMPOSITION_DONE,
        FrameEvent::DEQUEUE_READY,
        FrameEvent::RELEASE,
    };

status_t SurfaceFlinger::getSupportedFrameTimestamps(
        std::vector<FrameEvent>* outSupported) const {
    *outSupported = {
        FrameEvent::REQUESTED_PRESENT,
        FrameEvent::ACQUIRE,
        FrameEvent::LATCH,
        FrameEvent::FIRST_REFRESH_START,
        FrameEvent::LAST_REFRESH_START,
        FrameEvent::GPU_COMPOSITION_DONE,
        FrameEvent::DEQUEUE_READY,
        FrameEvent::RELEASE,
    };

status_t SurfaceFlinger::getSupportedFrameTimestamps(
        std::vector<FrameEvent>* outSupported) const {
    *outSupported = {
        FrameEvent::REQUESTED_PRESENT,
        FrameEvent::ACQUIRE,
        FrameEvent::LATCH,
        FrameEvent::FIRST_REFRESH_START,
        FrameEvent::LAST_REFRESH_START,
        FrameEvent::GPU_COMPOSITION_DONE,
        FrameEvent::DEQUEUE_READY,
        FrameEvent::RELEASE,
    };

status_t SurfaceFlinger::getSupportedFrameTimestamps(
        std::vector<FrameEvent>* outSupported) const {
    *outSupported = {
        FrameEvent::REQUESTED_PRESENT,
        FrameEvent::ACQUIRE,
        FrameEvent::LATCH,
        FrameEvent::FIRST_REFRESH_START,
        FrameEvent::LAST_REFRESH_START,
        FrameEvent::GPU_COMPOSITION_DONE,
        FrameEvent::DEQUEUE_READY,
        FrameEvent::RELEASE,
    };

status_t SurfaceFlinger::getSupportedFrameTimestamps(
        std::vector<FrameEvent>* outSupported) const {
    *outSupported = {
        FrameEvent::REQUESTED_PRESENT,
        FrameEvent::ACQUIRE,
        FrameEvent::LATCH,
        FrameEvent::FIRST_REFRESH_START,
        FrameEvent::LAST_REFRESH_START,
        FrameEvent::GPU_COMPOSITION_DONE,
        FrameEvent::DEQUEUE_READY,
        FrameEvent::RELEASE,
    };

status_t SurfaceFlinger::getSupportedFrameTimestamps(
        std::vector<FrameEvent>* outSupported) const {
    *outSupported = {
        FrameEvent::REQUESTED_PRESENT,
        FrameEvent::ACQUIRE,
        FrameEvent::LATCH,
        FrameEvent::FIRST_REFRESH_START,
        FrameEvent::LAST_REFRESH_START,
        FrameEvent::GPU_COMPOSITION_DONE,
        FrameEvent::DEQUEUE_READY,
        FrameEvent::RELEASE,
    };

status_t SurfaceFlinger::getSupportedFrameTimestamps(
        std::vector<FrameEvent>* outSupported) const {
    *outSupported = {
        FrameEvent::REQUESTED_PRESENT,
        FrameEvent::ACQUIRE,
        FrameEvent::LATCH,
        FrameEvent::FIRST_REFRESH_START,
        FrameEvent::LAST_REFRESH_START,
        FrameEvent::GPU_COMPOSITION_DONE,
        FrameEvent::DEQUEUE_READY,
        FrameEvent::RELEASE,
    };

std::string decodeDisplayColorSetting(DisplayColorSetting displayColorSetting) {
    switch(displayColorSetting) {
        case DisplayColorSetting::kManaged:
            return std::string("Managed");
        case DisplayColorSetting::kUnmanaged:
            return std::string("Unmanaged");
        case DisplayColorSetting::kEnhanced:
            return std::string("Enhanced");
        default:
            return std::string("Unknown ") +
                std::to_string(static_cast<int>(displayColorSetting));
    }
}

bool callingThreadHasRotateSurfaceFlingerAccess() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    return uid == AID_GRAPHICS || uid == AID_SYSTEM ||
            PermissionCache::checkPermission(sRotateSurfaceFlinger, pid, uid);
}

bool callingThreadHasInternalSystemWindowAccess() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    return uid == AID_GRAPHICS || uid == AID_SYSTEM ||
        PermissionCache::checkPermission(sInternalSystemWindow, pid, uid);
}

SurfaceFlinger::SurfaceFlinger(Factory& factory, SkipInitializationTag)
      : mFactory(factory),
        mPid(getpid()),
        mInterceptor(mFactory.createSurfaceInterceptor()),
        mTimeStats(std::make_shared<impl::TimeStats>()),
        mFrameTracer(mFactory.createFrameTracer()),
        mFrameTimeline(mFactory.createFrameTimeline(mTimeStats, mPid)),
        mCompositionEngine(mFactory.createCompositionEngine()),
        mHwcServiceName(base::GetProperty("debug.sf.hwc_service_name"s, "default"s)),
        mTunnelModeEnabledReporter(sp<TunnelModeEnabledReporter>::make()),
        mInternalDisplayDensity(getDensityFromProperty("ro.sf.lcd_density", true)),
        mEmulatedDisplayDensity(getDensityFromProperty("qemu.sf.lcd_density", false)),
        mPowerAdvisor(std::make_unique<Hwc2::impl::PowerAdvisor>(*this)),
        mWindowInfosListenerInvoker(sp<WindowInfosListenerInvoker>::make()) {
    ALOGI("Using HWComposer service: %s", mHwcServiceName.c_str());
}

SurfaceFlinger::SurfaceFlinger(Factory& factory) : SurfaceFlinger(factory, SkipInitialization) {
    ALOGI("SurfaceFlinger is starting");

    hasSyncFramework {
    ALOGI("SurfaceFlinger is starting");

    hasSyncFramework = running_without_sync_framework(true);

    dispSyncPresentTimeOffset = present_time_offset_from_vsync_ns(0);

    useHwcForRgbToYuv = force_hwc_copy_for_virtual_displays(false);

    maxFrameBufferAcquiredBuffers = max_frame_buffer_acquired_buffers(2);

    maxGraphicsWidth = std::max(max_graphics_width(0), 0);
    maxGraphicsHeight = std::max(max_graphics_height(0), 0);

    hasWideColorDisplay = has_wide_color_display(false);
    mDefaultCompositionDataspace =
            static_cast<ui::Dataspace>(default_composition_dataspace(Dataspace::V0_SRGB));
    mWideColorGamutCompositionDataspace = static_cast<ui::Dataspace>(wcg_composition_dataspace(
            hasWideColorDisplay ? Dataspace::DISPLAY_P3 : Dataspace::V0_SRGB));
    defaultCompositionDataspace = mDefaultCompositionDataspace;
    wideColorGamutCompositionDataspace = mWideColorGamutCompositionDataspace;
    defaultCompositionPixelFormat = static_cast<ui::PixelFormat>(
            default_composition_pixel_format(ui::PixelFormat::RGBA_8888));
    wideColorGamutCompositionPixelFormat =
            static_cast<ui::PixelFormat>(wcg_composition_pixel_format(ui::PixelFormat::RGBA_8888));

    mColorSpaceAgnosticDataspace =
            static_cast<ui::Dataspace>(color_space_agnostic_dataspace(Dataspace::UNKNOWN));

    mLayerCachingEnabled = [] {
        const bool enable =
                android::sysprop::SurfaceFlingerProperties::enable_layer_caching().value_or(false);
        return base::GetBoolProperty(std::string("debug.sf.enable_layer_caching"), enable);
    }();

    useContextPriority = use_context_priority(true);

    mInternalDisplayPrimaries = sysprop::getDisplayNativePrimaries();

    // debugging stuff...
    char value[PROPERTY_VALUE_MAX];

    property_get("ro.bq.gpu_to_cpu_unsupported", value, "0");
    mGpuToCpuSupported = !atoi(value);

    property_get("ro.build.type", value, "user");
    mIsUserBuild = strcmp(value, "user") == 0;

    mDebugFlashDelay = base::GetUintProperty("debug.sf.showupdates"s, 0u);

    // DDMS debugging deprecated (b/120782499)
    property_get("debug.sf.ddms", value, "0");
    int debugDdms = atoi(value);
    ALOGI_IF(debugDdms, "DDMS debugging not supported");

    property_get("debug.sf.enable_gl_backpressure", value, "0");
    mPropagateBackpressureClientComposition = atoi(value);
    ALOGI_IF(mPropagateBackpressureClientComposition,
             "Enabling backpressure propagation for Client Composition");

    property_get("ro.surface_flinger.supports_background_blur", value, "0");
    bool supportsBlurs = atoi(value);
    mSupportsBlur = supportsBlurs;
    ALOGI_IF(!mSupportsBlur, "Disabling blur effects, they are not supported.");
    property_get("ro.sf.blurs_are_expensive", value, "0");
    mBlursAreExpensive = atoi(value);

    const size_t defaultListSize = MAX_LAYERS;
    auto listSize = property_get_int32("debug.sf.max_igbp_list_size", int32_t(defaultListSize));
    mMaxGraphicBufferProducerListSize = (listSize > 0) ? size_t(listSize) : defaultListSize;
    mGraphicBufferProducerListSizeLogThreshold =
            std::max(static_cast<int>(0.95 *
                                      static_cast<double>(mMaxGraphicBufferProducerListSize)),
                     1);

    property_get("debug.sf.luma_sampling", value, "1");
    mLumaSampling = atoi(value);

    property_get("debug.sf.disable_client_composition_cache", value, "0");
    mDisableClientCompositionCache = atoi(value);

    property_get("debug.sf.predict_hwc_composition_strategy", value, "1");
    mPredictCompositionStrategy = atoi(value);

    property_get("debug.sf.treat_170m_as_sRGB", value, "0");
    mTreat170mAsSrgb = atoi(value);

    // We should be reading 'persist.sys.sf.color_saturation' here
    // but since /data may be encrypted, we need to wait until after vold
    // comes online to attempt to read the property. The property is
    // instead read after the boot animation

    if (base::GetBoolProperty("debug.sf.treble_testing_override"s, false)) {
        // Without the override SurfaceFlinger cannot connect to HIDL
        // services that are not listed in the manifests.  Considered
        // deriving the setting from the set service name, but it
        // would be brittle if the name that's not 'default' is used
        // for production purposes later on.
        ALOGI("Enabling Treble testing override");
        android::hardware::details::setTrebleTestingOverride(true);
    }

    mRefreshRateOverlaySpinner = property_get_bool("sf.debug.show_refresh_rate_overlay_spinner", 0);

    if (!mIsUserBuild && base::GetBoolProperty("debug.sf.enable_transaction_tracing"s, true)) {
        mTransactionTracing.emplace();
    }

    mIgnoreHdrCameraLayers = ignore_hdr_camera_layers(false);

    // Power hint session mode, representing which hint(s) to send: early, late, or both)
    mPowerHintSessionMode =
            {.late = base::GetBoolProperty("debug.sf.send_late_power_session_hint"s, true),
             .early = base::GetBoolProperty("debug.sf.send_early_power_session_hint"s, false)};
}

LatchUnsignaledConfig SurfaceFlinger::getLatchUnsignaledConfig() {
    if (base::GetBoolProperty("debug.sf.auto_latch_unsignaled"s, true)) {
        return LatchUnsignaledConfig::AutoSingleLayer;
    }

    if (base::GetBoolProperty("debug.sf.latch_unsignaled"s, false)) {
        return LatchUnsignaledConfig::Always;
    }

    return LatchUnsignaledConfig::Disabled;
}

SurfaceFlinger::~SurfaceFlinger() 

void SurfaceFlinger::binderDied(const wp<IBinder>&) {
    // the window manager died on us. prepare its eulogy.
    mBootFinished = false;

    // Sever the link to inputflinger since it's gone as well.
    static_cast<void>(mScheduler->schedule(
            [this] { mInputFlinger.clear(); }));

    // restore initial conditions (default device unblank, etc)
    initializeDisplays();

    // restart the boot-animation
    startBootAnim();
}

void SurfaceFlinger::run() {
    mScheduler->run();
}

sp<IBinder> SurfaceFlinger::createDisplay(const String8& displayName, bool secure) {
    // onTransact already checks for some permissions, but adding an additional check here.
    // This is to ensure that only system and graphics can request to create a secure
    // display. Secure displays can show secure content so we add an additional restriction on it.
    const int uid = IPCThreadState::self()->getCallingUid();
    if (secure && uid != AID_GRAPHICS && uid != AID_SYSTEM) {
        ALOGE("Only privileged processes can create a secure display");
        return nullptr;
    }

    class DisplayToken : public BBinder {
        sp<SurfaceFlinger> flinger;
        virtual ~DisplayToken() {
             // no more references, this display must be terminated
             Mutex::Autolock _l(flinger->mStateLock);
             flinger->mCurrentState.displays.removeItem(wp<IBinder>::fromExisting(this));
             flinger->setTransactionFlags(eDisplayTransactionNeeded);
         }
     public:
        explicit DisplayToken(const sp<SurfaceFlinger>& flinger)
            : flinger(flinger) {
        }
    };

    sp<BBinder> token = sp<DisplayToken>::make(sp<SurfaceFlinger>::fromExisting(this));

    Mutex::Autolock _l(mStateLock);
    // Display ID is assigned when virtual display is allocated by HWC.
    DisplayDeviceState state;
    state.isSecure = secure;
    state.displayName = displayName;
    mCurrentState.displays.add(token, state);
    mInterceptor->saveDisplayCreation(state);
    return token;
}

void SurfaceFlinger::destroyDisplay(const sp<IBinder>& displayToken) {
    Mutex::Autolock lock(mStateLock);

    const ssize_t index = mCurrentState.displays.indexOfKey(displayToken);
    if (index < 0) {
        ALOGE("%s: Invalid display token %p", __func__, displayToken.get());
        return;
    }

    const DisplayDeviceState& state = mCurrentState.displays.valueAt(index);
    if (state.physical) {
        ALOGE("%s: Invalid operation on physical display", __func__);
        return;
    }
    mInterceptor->saveDisplayDeletion(state.sequenceId);
    mCurrentState.displays.removeItemsAt(index);
    setTransactionFlags(eDisplayTransactionNeeded);
}

void SurfaceFlinger::enableHalVirtualDisplays(bool enable) {
    auto& generator = mVirtualDisplayIdGenerators.hal;
    if (!generator && enable) {
        ALOGI("Enabling HAL virtual displays");
        generator.emplace(getHwComposer().getMaxVirtualDisplayCount());
    } else if (generator && !enable) {
        ALOGW_IF(generator->inUse(), "Disabling HAL virtual displays while in use");
        generator.reset();
    }
}

VirtualDisplayId SurfaceFlinger::acquireVirtualDisplay(ui::Size resolution,
                                                       ui::PixelFormat format) {
    if (auto& generator = mVirtualDisplayIdGenerators.hal) {
        if (const auto id = generator->generateId()) {
            if (getHwComposer().allocateVirtualDisplay(*id, resolution, &format)) {
                return *id;
            }

            generator->releaseId(*id);
        } else {
            ALOGW("%s: Exhausted HAL virtual displays", __func__);
        }

        ALOGW("%s: Falling back to GPU virtual display", __func__);
    }

    const auto id = mVirtualDisplayIdGenerators.gpu.generateId();
    LOG_ALWAYS_FATAL_IF(!id, "Failed to generate ID for GPU virtual display");
    return *id;
}

void SurfaceFlinger::releaseVirtualDisplay(VirtualDisplayId displayId) {
    if (const auto id = HalVirtualDisplayId::tryCast(displayId)) {
        if (auto& generator = mVirtualDisplayIdGenerators.hal) {
            generator->releaseId(*id);
        }
        return;
    }

    const auto id = GpuVirtualDisplayId::tryCast(displayId);
    LOG_ALWAYS_FATAL_IF(!id);
    mVirtualDisplayIdGenerators.gpu.releaseId(*id);
}

std::vector<PhysicalDisplayId> SurfaceFlinger::getPhysicalDisplayIdsLocked() const {
    std::vector<PhysicalDisplayId> displayIds;
    displayIds.reserve(mPhysicalDisplays.size());

    const auto defaultDisplayId = getDefaultDisplayDeviceLocked()->getPhysicalId();
    displayIds.push_back(defaultDisplayId);

    for (const auto& [id, display] : mPhysicalDisplays) {
        if (id != defaultDisplayId) {
            displayIds.push_back(id);
        }
    }

    return displayIds;
}

std::optional<PhysicalDisplayId> SurfaceFlinger::getPhysicalDisplayIdLocked(const sp<display::DisplayToken>& displayToken) const {
    return ftl::find_if(mPhysicalDisplays, PhysicalDisplay::hasToken(displayToken))
            .transform(&ftl::to_key<PhysicalDisplays>);
}

sp<IBinder> SurfaceFlinger::getPhysicalDisplayToken(PhysicalDisplayId displayId) const {
    Mutex::Autolock lock(mStateLock);
    return getPhysicalDisplayTokenLocked(displayId);
}

status_t SurfaceFlinger::getColorManagement(bool* outGetColorManagement) const {
    if (!outGetColorManagement) {
        return BAD_VALUE;
    }
    *outGetColorManagement = useColorManagement;
    return NO_ERROR;
}

HWComposer& SurfaceFlinger::getHwComposer() const {
    return mCompositionEngine->getHwComposer();
}

renderengine::RenderEngine& SurfaceFlinger::getRenderEngine() const {
    return mCompositionEngine->getRenderEngine();
}

compositionengine::CompositionEngine& SurfaceFlinger::getCompositionEngine() const {
    return *mCompositionEngine.get();
}

void SurfaceFlinger::bootFinished() {
    if (mBootFinished == true) {
        ALOGE("Extra call to bootFinished");
        return;
    }
    mBootFinished = true;
    if (mStartPropertySetThread->join() != NO_ERROR) {
        ALOGE("Join StartPropertySetThread failed!");
    }

    if (mRenderEnginePrimeCacheFuture.valid()) {
        mRenderEnginePrimeCacheFuture.get();
    }
    const nsecs_t now = systemTime();
    const nsecs_t duration = now - mBootTime;
    ALOGI("Boot is finished (%ld ms)", long(ns2ms(duration)) );

    mFrameTracer->initialize();
    mFrameTimeline->onBootFinished();
    getRenderEngine().setEnableTracing(mFlagManager.use_skia_tracing());

    // wait patiently for the window manager death
    const String16 name("window");
    mWindowManager = defaultServiceManager()->getService(name);
    if (mWindowManager != 0) {
        mWindowManager->linkToDeath(sp<IBinder::DeathRecipient>::fromExisting(this));
    }

    // stop boot animation
    // formerly we would just kill the process, but we now ask it to exit so it
    // can choose where to stop the animation.
    property_set("service.bootanim.exit", "1");

    const int LOGTAG_SF_STOP_BOOTANIM = 60110;
    LOG_EVENT_LONG(LOGTAG_SF_STOP_BOOTANIM,
                   ns2ms(systemTime(SYSTEM_TIME_MONOTONIC)));

    sp<IBinder> input(defaultServiceManager()->getService(String16("inputflinger")));

    static_cast<void>(mScheduler->schedule([=]() FTL_FAKE_GUARD(kMainThreadContext) {
        if (input == nullptr) {
            ALOGE("Failed to link to input service");
        } else {
            mInputFlinger = interface_cast<os::IInputFlinger>(input);
        }

        readPersistentProperties();
        mPowerAdvisor->onBootFinished();
        const bool powerHintEnabled = mFlagManager.use_adpf_cpu_hint();
        mPowerAdvisor->enablePowerHint(powerHintEnabled);
        const bool powerHintUsed = mPowerAdvisor->usePowerHintSession();
        ALOGD("Power hint is %s",
              powerHintUsed ? "supported" : (powerHintEnabled ? "unsupported" : "disabled"));
        if (powerHintUsed) {
            std::optional<pid_t> renderEngineTid = getRenderEngine().getRenderEngineTid();
            std::vector<int32_t> tidList;
            tidList.emplace_back(gettid());
            if (renderEngineTid.has_value()) {
                tidList.emplace_back(*renderEngineTid);
            }
            if (!mPowerAdvisor->startPowerHintSession(tidList)) {
                ALOGW("Cannot start power hint session");
            }
        }

        mBootStage = BootStage::FINISHED;

        if (base::GetBoolProperty("sf.debug.show_refresh_rate_overlay"s, false)) {
            ftl::FakeGuard guard(mStateLock);
            enableRefreshRateOverlay(true);
        }
    }));
}

uint32_t SurfaceFlinger::getNewTexture() {
    {
        std::lock_guard lock(mTexturePoolMutex);
        if (!mTexturePool.empty()) {
            uint32_t name = mTexturePool.back();
            mTexturePool.pop_back();
            ATRACE_INT("TexturePoolSize", mTexturePool.size());
            return name;
        }

        // The pool was too small, so increase it for the future
        ++mTexturePoolSize;
    }

    // The pool was empty, so we need to get a new texture name directly using a
    // blocking call to the main thread
    auto genTextures = [this] {
               uint32_t name = 0;
               getRenderEngine().genTextures(1, &name);
               return name;
    };
    if (std::this_thread::get_id() == mMainThreadId) {
        return genTextures();
    } else {
        return mScheduler->schedule(genTextures).get();
    }
}

void SurfaceFlinger::deleteTextureAsync(uint32_t texture) {
    std::lock_guard lock(mTexturePoolMutex);
    // We don't change the pool size, so the fix-up logic in postComposition will decide whether
    // to actually delete this or not based on mTexturePoolSize
    mTexturePool.push_back(texture);
    ATRACE_INT("TexturePoolSize", mTexturePool.size());
}

static std::optional<renderengine::RenderEngine::RenderEngineType>
chooseRenderEngineTypeViaSysProp() {
    char prop[PROPERTY_VALUE_MAX];
    property_get(PROPERTY_DEBUG_RENDERENGINE_BACKEND, prop, "");

    if (strcmp(prop, "gles") == 0) {
        return renderengine::RenderEngine::RenderEngineType::GLES;
    } else if (strcmp(prop, "threaded") == 0) {
        return renderengine::RenderEngine::RenderEngineType::THREADED;
    } else if (strcmp(prop, "skiagl") == 0) {
        return renderengine::RenderEngine::RenderEngineType::SKIA_GL;
    } else if (strcmp(prop, "skiaglthreaded") == 0) {
        return renderengine::RenderEngine::RenderEngineType::SKIA_GL_THREADED;
    } else {
        ALOGE("Unrecognized RenderEngineType %s; ignoring!", prop);
        return {};
    }
}

void SurfaceFlinger::readPersistentProperties() {
    Mutex::Autolock _l(mStateLock);

    char value[PROPERTY_VALUE_MAX];

    property_get("persist.sys.sf.color_saturation", value, "1.0");
    mGlobalSaturationFactor = atof(value);
    updateColorMatrixLocked();
    ALOGV("Saturation is set to %.2f", mGlobalSaturationFactor);

    property_get("persist.sys.sf.native_mode", value, "0");
    mDisplayColorSetting = static_cast<DisplayColorSetting>(atoi(value));

    property_get("persist.sys.sf.color_mode", value, "0");
    mForceColorMode = static_cast<ColorMode>(atoi(value));
}

void SurfaceFlinger::startBootAnim() {
    // Start boot animation service by setting a property mailbox
    // if property setting thread is already running, Start() will be just a NOP
    mStartPropertySetThread->Start();
    // Wait until property was set
    if (mStartPropertySetThread->join() != NO_ERROR) {
        ALOGE("Join StartPropertySetThread failed!");
    }
}

// ----------------------------------------------------------------------------

status_t SurfaceFlinger::getSupportedFrameTimestamps(
        std::vector<FrameEvent>* outSupported) const {
    *outSupported = {
        FrameEvent::REQUESTED_PRESENT,
        FrameEvent::ACQUIRE,
        FrameEvent::LATCH,
        FrameEvent::FIRST_REFRESH_START,
        FrameEvent::LAST_REFRESH_START,
        FrameEvent::GPU_COMPOSITION_DONE,
        FrameEvent::DEQUEUE_READY,
        FrameEvent::RELEASE,
    };

private:
    const int mApi;
} 

status_t SurfaceFlinger::getDisplayState(const sp<IBinder>& displayToken, ui::DisplayState* state) {
    if (!displayToken || !state) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);

    const auto display = getDisplayDeviceLocked(displayToken);
    if (!display) {
        return NAME_NOT_FOUND;
    }

    state->layerStack = display->getLayerStack();
    state->orientation = display->getOrientation();

    const Rect layerStackRect = display->getLayerStackSpaceRect();
    state->layerStackSpaceRect =
            layerStackRect.isValid() ? layerStackRect.getSize() : display->getSize();

    return NO_ERROR;
}

status_t SurfaceFlinger::getStaticDisplayInfo(const sp<IBinder>& displayToken,
                                              ui::StaticDisplayInfo* info) {
    if (!displayToken || !info) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);

    const auto displayOpt = ftl::find_if(mPhysicalDisplays, PhysicalDisplay::hasToken(displayToken))
                                    .transform(&ftl::to_mapped_ref<PhysicalDisplays>)
                                    .and_then(getDisplayDeviceAndSnapshot());

    if (!displayOpt) {
        return NAME_NOT_FOUND;
    }

    const auto& [display, snapshotRef] = *displayOpt;
    const auto& snapshot = snapshotRef.get();

    info->connectionType = snapshot.connectionType();
    info->deviceProductInfo = snapshot.deviceProductInfo();

    if (mEmulatedDisplayDensity) {
        info->density = mEmulatedDisplayDensity;
    } else {
        info->density = info->connectionType == ui::DisplayConnectionType::Internal
                ? mInternalDisplayDensity
                : FALLBACK_DENSITY;
    }
    info->density /= ACONFIGURATION_DENSITY_MEDIUM;

    info->secure = display->isSecure();
    info->installOrientation = display->getPhysicalOrientation();

    return NO_ERROR;
}

status_t SurfaceFlinger::getDynamicDisplayInfo(const sp<IBinder>& displayToken,
                                               ui::DynamicDisplayInfo* info) {
    if (!displayToken || !info) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);

    const auto displayOpt = ftl::find_if(mPhysicalDisplays, PhysicalDisplay::hasToken(displayToken))
                                    .transform(&ftl::to_mapped_ref<PhysicalDisplays>)
                                    .and_then(getDisplayDeviceAndSnapshot());
    if (!displayOpt) {
        return NAME_NOT_FOUND;
    }

    const auto& [display, snapshotRef] = *displayOpt;
    const auto& snapshot = snapshotRef.get();

    const auto& displayModes = snapshot.displayModes();

    info->supportedDisplayModes.clear();
    info->supportedDisplayModes.reserve(displayModes.size());

    for (const auto& [id, mode] : displayModes) {
        ui::DisplayMode outMode;
        outMode.id = static_cast<int32_t>(id.value());

        auto [width, height] = mode->getResolution();
        auto [xDpi, yDpi] = mode->getDpi();

        if (const auto physicalOrientation = display->getPhysicalOrientation();
            physicalOrientation == ui::ROTATION_90 || physicalOrientation == ui::ROTATION_270) {
            std::swap(width, height);
            std::swap(xDpi, yDpi);
        }

        outMode.resolution = ui::Size(width, height);

        outMode.xDpi = xDpi;
        outMode.yDpi = yDpi;

        const nsecs_t period = mode->getVsyncPeriod();
        outMode.refreshRate = Fps::fromPeriodNsecs(period).getValue();

        const auto vsyncConfigSet =
                mVsyncConfiguration->getConfigsForRefreshRate(Fps::fromValue(outMode.refreshRate));
        outMode.appVsyncOffset = vsyncConfigSet.late.appOffset;
        outMode.sfVsyncOffset = vsyncConfigSet.late.sfOffset;
        outMode.group = mode->getGroup();

        // This is how far in advance a buffer must be queued for
        // presentation at a given time.  If you want a buffer to appear
        // on the screen at time N, you must submit the buffer before
        // (N - presentationDeadline).
        //
        // Normally it's one full refresh period (to give SF a chance to
        // latch the buffer), but this can be reduced by configuring a
        // VsyncController offset.  Any additional delays introduced by the hardware
        // composer or panel must be accounted for here.
        //
        // We add an additional 1ms to allow for processing time and
        // differences between the ideal and actual refresh rate.
        outMode.presentationDeadline = period - outMode.sfVsyncOffset + 1000000;

        info->supportedDisplayModes.push_back(outMode);
    }

    const PhysicalDisplayId displayId = snapshot.displayId();

    info->activeDisplayModeId = display->refreshRateConfigs().getActiveModePtr()->getId().value();
    info->activeColorMode = display->getCompositionDisplay()->getState().colorMode;
    info->supportedColorModes = getDisplayColorModes(displayId);
    info->hdrCapabilities = display->getHdrCapabilities();

    info->autoLowLatencyModeSupported =
            getHwComposer().hasDisplayCapability(displayId,
                                                 DisplayCapability::AUTO_LOW_LATENCY_MODE);
    info->gameContentTypeSupported =
            getHwComposer().supportsContentType(displayId, hal::ContentType::GAME);

    info->preferredBootDisplayMode = static_cast<ui::DisplayModeId>(-1);

    if (getHwComposer().hasCapability(Capability::BOOT_DISPLAY_CONFIG)) {
        if (const auto hwcId = getHwComposer().getPreferredBootDisplayMode(displayId)) {
            if (const auto modeId = snapshot.translateModeId(*hwcId)) {
                info->preferredBootDisplayMode = modeId->value();
            }
        }
    }

    return NO_ERROR;
}

status_t SurfaceFlinger::getDisplayStats(const sp<IBinder>&, DisplayStatInfo* outStats) {
    if (!outStats) {
        return BAD_VALUE;
    }

    const auto& schedule = mScheduler->getVsyncSchedule();
    outStats->vsyncTime = schedule.vsyncDeadlineAfter(TimePoint::now()).ns();
    outStats->vsyncPeriod = schedule.period().ns();
    return NO_ERROR;
}

void SurfaceFlinger::setDesiredActiveMode(const ActiveModeInfo& info) {
    ATRACE_CALL();

    if (!info.mode) {
        ALOGW("requested display mode is null");
        return;
    }
    auto display = getDisplayDeviceLocked(info.mode->getPhysicalDisplayId());
    if (!display) {
        ALOGW("%s: display is no longer valid", __func__);
        return;
    }

    if (display->setDesiredActiveMode(info)) {
        scheduleComposite(FrameHint::kNone);

        // Start receiving vsync samples now, so that we can detect a period
        // switch.
        mScheduler->resyncToHardwareVsync(true, info.mode->getFps());
        // As we called to set period, we will call to onRefreshRateChangeCompleted once
        // VsyncController model is locked.
        modulateVsync(&VsyncModulator::onRefreshRateChangeInitiated);

        updatePhaseConfiguration(info.mode->getFps());
        mScheduler->setModeChangePending(true);
    }
}

status_t SurfaceFlinger::setActiveModeFromBackdoor(const sp<display::DisplayToken>& displayToken,
                                                   DisplayModeId modeId) {
    ATRACE_CALL();

    if (!displayToken) {
        return BAD_VALUE;
    }

    const char* const whence = __func__;
    auto future = mScheduler->schedule([=]() -> status_t {
        const auto displayOpt =
                FTL_FAKE_GUARD(mStateLock,
                               ftl::find_if(mPhysicalDisplays,
                                            PhysicalDisplay::hasToken(displayToken))
                                       .transform(&ftl::to_mapped_ref<PhysicalDisplays>)
                                       .and_then(getDisplayDeviceAndSnapshot()));
        if (!displayOpt) {
            ALOGE("%s: Invalid physical display token %p", whence, displayToken.get());
            return NAME_NOT_FOUND;
        }

        const auto& [display, snapshotRef] = *displayOpt;
        const auto& snapshot = snapshotRef.get();

        const auto fpsOpt = snapshot.displayModes().get(modeId).transform(
                [](const DisplayModePtr& mode) { return mode->getFps(); });

        if (!fpsOpt) {
            ALOGE("%s: Invalid mode %d for display %s", whence, modeId.value(),
                  to_string(snapshot.displayId()).c_str());
            return BAD_VALUE;
        }

        const Fps fps = *fpsOpt;
        // Keep the old switching type.
        const bool allowGroupSwitching =
                display->refreshRateConfigs().getCurrentPolicy().allowGroupSwitching;
        const scheduler::RefreshRateConfigs::Policy policy{modeId, allowGroupSwitching, {fps, fps}};
        constexpr bool kOverridePolicy = false;

        return setDesiredDisplayModeSpecsInternal(display, policy, kOverridePolicy);
    });

    return future.get();
}

void SurfaceFlinger::updateInternalStateWithChangedMode() {
    ATRACE_CALL();

    const auto display = getDefaultDisplayDeviceLocked();
    if (!display) {
        return;
    }

    const auto upcomingModeInfo = display->getUpcomingActiveMode();
    if (!upcomingModeInfo.mode) {
        // There is no pending mode change. This can happen if the active
        // display changed and the mode change happened on a different display.
        return;
    }

    if (display->getActiveMode().getResolution() != upcomingModeInfo.mode->getResolution()) {
        auto& state = mCurrentState.displays.editValueFor(display->getDisplayToken());
        // We need to generate new sequenceId in order to recreate the display (and this
        // way the framebuffer).
        state.sequenceId = DisplayDeviceState{}.sequenceId;
        state.physical->activeMode = upcomingModeInfo.mode;
        processDisplayChangesLocked();

        // processDisplayChangesLocked will update all necessary components so we're done here.
        return;
    }

    mPhysicalDisplays.get(display->getPhysicalId())
            .transform(&PhysicalDisplay::snapshotRef)
            .transform(ftl::unit_fn([&](const display::DisplaySnapshot& snapshot) {
                FTL_FAKE_GUARD(kMainThreadContext,
                               display->setActiveMode(upcomingModeInfo.mode->getId(), snapshot));
            }));

    const Fps refreshRate = upcomingModeInfo.mode->getFps();
    mRefreshRateStats->setRefreshRate(refreshRate);
    updatePhaseConfiguration(refreshRate);

    if (upcomingModeInfo.event != DisplayModeEvent::None) {
        mScheduler->onPrimaryDisplayModeChanged(mAppConnectionHandle, upcomingModeInfo.mode);
    }
}

void SurfaceFlinger::clearDesiredActiveModeState(const sp<DisplayDevice>& display) {
    display->clearDesiredActiveModeState();
    if (isDisplayActiveLocked(display)) {
        mScheduler->setModeChangePending(false);
    }
}

void SurfaceFlinger::desiredActiveModeChangeDone(const sp<DisplayDevice>& display) {
    const auto refreshRate = display->getDesiredActiveMode()->mode->getFps();
    clearDesiredActiveModeState(display);
    mScheduler->resyncToHardwareVsync(true, refreshRate);
    updatePhaseConfiguration(refreshRate);
}

void SurfaceFlinger::setActiveModeInHwcIfNeeded() {
    ATRACE_CALL();

    std::optional<PhysicalDisplayId> displayToUpdateImmediately;

    for (const auto& [id, physical] : mPhysicalDisplays) {
        const auto& snapshot = physical.snapshot();

        if (snapshot.connectionType() != ui::DisplayConnectionType::Internal) {
            continue;
        }

        const auto display = getDisplayDeviceLocked(id);
        if (!display) continue;

        // Store the local variable to release the lock.
        const auto desiredActiveMode = display->getDesiredActiveMode();
        if (!desiredActiveMode) {
            // No desired active mode pending to be applied
            continue;
        }

        if (!isDisplayActiveLocked(display)) {
            // display is no longer the active display, so abort the mode change
            clearDesiredActiveModeState(display);
            continue;
        }

        const auto desiredModeId = desiredActiveMode->mode->getId();
        const auto refreshRateOpt =
                snapshot.displayModes()
                        .get(desiredModeId)
                        .transform([](const DisplayModePtr& mode) { return mode->getFps(); });

        if (!refreshRateOpt) {
            ALOGW("Desired display mode is no longer supported. Mode ID = %d",
                  desiredModeId.value());
            clearDesiredActiveModeState(display);
            continue;
        }

        ALOGV("%s changing active mode to %d(%s) for display %s", __func__, desiredModeId.value(),
              to_string(*refreshRateOpt).c_str(), to_string(display->getId()).c_str());

        if (display->getActiveMode().getId() == desiredModeId) {
            // we are already in the requested mode, there is nothing left to do
            desiredActiveModeChangeDone(display);
            continue;
        }

        // Desired active mode was set, it is different than the mode currently in use, however
        // allowed modes might have changed by the time we process the refresh.
        // Make sure the desired mode is still allowed
        const auto displayModeAllowed = display->refreshRateConfigs().isModeAllowed(desiredModeId);
        if (!displayModeAllowed) {
            clearDesiredActiveModeState(display);
            continue;
        }

        // TODO(b/142753666) use constrains
        hal::VsyncPeriodChangeConstraints constraints;
        constraints.desiredTimeNanos = systemTime();
        constraints.seamlessRequired = false;
        hal::VsyncPeriodChangeTimeline outTimeline;

        const auto status =
                display->initiateModeChange(*desiredActiveMode, constraints, &outTimeline);

        if (status != NO_ERROR) {
            // initiateModeChange may fail if a hotplug event is just about
            // to be sent. We just log the error in this case.
            ALOGW("initiateModeChange failed: %d", status);
            continue;
        }
        mScheduler->onNewVsyncPeriodChangeTimeline(outTimeline);

        if (outTimeline.refreshRequired) {
            scheduleComposite(FrameHint::kNone);
            mSetActiveModePending = true;
        } else {
            // Updating the internal state should be done outside the loop,
            // because it can recreate a DisplayDevice and modify mDisplays
            // which will invalidate the iterator.
            displayToUpdateImmediately = display->getPhysicalId();
        }
    }

    if (displayToUpdateImmediately) {
        updateInternalStateWithChangedMode();

        const auto display = getDisplayDeviceLocked(*displayToUpdateImmediately);
        const auto desiredActiveMode = display->getDesiredActiveMode();
        if (desiredActiveMode &&
            display->getActiveMode().getId() == desiredActiveMode->mode->getId()) {
            desiredActiveModeChangeDone(display);
        }
    }
}

void SurfaceFlinger::disableExpensiveRendering() {
    const char* const whence = __func__;
    auto future = mScheduler->schedule([=]() FTL_FAKE_GUARD(mStateLock) {
        ATRACE_NAME(whence);
        if (mPowerAdvisor->isUsingExpensiveRendering()) {
            for (const auto& [_, display] : mDisplays) {
                constexpr bool kDisable = false;
                mPowerAdvisor->setExpensiveRenderingExpected(display->getId(), kDisable);
            }
        }
    });

    future.wait();
}

std::vector<ColorMode> SurfaceFlinger::getDisplayColorModes(PhysicalDisplayId displayId) {
    auto modes = getHwComposer().getColorModes(displayId);

    const bool isInternalDisplay = mPhysicalDisplays.get(displayId)
                                           .transform(&PhysicalDisplay::isInternal)
                                           .value_or(false);

    // If the display is internal and the configuration claims it's not wide color capable,
    // filter out all wide color modes. The typical reason why this happens is that the
    // hardware is not good enough to support GPU composition of wide color, and thus the
    // OEMs choose to disable this capability.
    if (isInternalDisplay && !hasWideColorDisplay) {
        const auto newEnd = std::remove_if(modes.begin(), modes.end(), isWideColorMode);
        modes.erase(newEnd, modes.end());
    }

    return modes;
}

status_t SurfaceFlinger::getDisplayNativePrimaries(const sp<IBinder>& displayToken,
                                                   ui::DisplayPrimaries& primaries) {
    if (!displayToken) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);

    const auto display = ftl::find_if(mPhysicalDisplays, PhysicalDisplay::hasToken(displayToken))
                                 .transform(&ftl::to_mapped_ref<PhysicalDisplays>);
    if (!display) {
        return NAME_NOT_FOUND;
    }

    if (!display.transform(&PhysicalDisplay::isInternal).value()) {
        return INVALID_OPERATION;
    }

    // TODO(b/229846990): For now, assume that all internal displays have the same primaries.
    primaries = mInternalDisplayPrimaries;
    return NO_ERROR;
}

status_t SurfaceFlinger::setActiveColorMode(const sp<IBinder>& displayToken, ColorMode mode) {
    if (!displayToken) {
        return BAD_VALUE;
    }

    auto future = mScheduler->schedule([=]() FTL_FAKE_GUARD(mStateLock) -> status_t {
        const auto display = getDisplayDeviceLocked(displayToken);
        if (!display) {
            ALOGE("Attempt to set active color mode %s (%d) for invalid display token %p",
                  decodeColorMode(mode).c_str(), mode, displayToken.get());
            return NAME_NOT_FOUND;
        }

        if (display->isVirtual()) {
            ALOGW("Attempt to set active color mode %s (%d) for virtual display",
                  decodeColorMode(mode).c_str(), mode);
            return INVALID_OPERATION;
        }

        const auto modes = getDisplayColorModes(display->getPhysicalId());
        const bool exists = std::find(modes.begin(), modes.end(), mode) != modes.end();

        if (mode < ColorMode::NATIVE || !exists) {
            ALOGE("Attempt to set invalid active color mode %s (%d) for display token %p",
                  decodeColorMode(mode).c_str(), mode, displayToken.get());
            return BAD_VALUE;
        }

        display->getCompositionDisplay()->setColorProfile(
                {mode, Dataspace::UNKNOWN, RenderIntent::COLORIMETRIC, Dataspace::UNKNOWN});

        return NO_ERROR;
    });

    // TODO(b/195698395): Propagate error.
    future.wait();
    return NO_ERROR;
}

status_t SurfaceFlinger::getBootDisplayModeSupport(bool* outSupport) const {
    auto future = mScheduler->schedule(
            [this] { return getHwComposer().hasCapability(Capability::BOOT_DISPLAY_CONFIG); });

    *outSupport = future.get();
    return NO_ERROR;
}

status_t SurfaceFlinger::setBootDisplayMode(const sp<display::DisplayToken>& displayToken,
                                            DisplayModeId modeId) {
    const char* const whence = __func__;
    auto future = mScheduler->schedule([=]() FTL_FAKE_GUARD(mStateLock) -> status_t {
        const auto snapshotOpt =
                ftl::find_if(mPhysicalDisplays, PhysicalDisplay::hasToken(displayToken))
                        .transform(&ftl::to_mapped_ref<PhysicalDisplays>)
                        .transform(&PhysicalDisplay::snapshotRef);

        if (!snapshotOpt) {
            ALOGE("%s: Invalid physical display token %p", whence, displayToken.get());
            return NAME_NOT_FOUND;
        }

        const auto& snapshot = snapshotOpt->get();
        const auto hwcIdOpt = snapshot.displayModes().get(modeId).transform(
                [](const DisplayModePtr& mode) { return mode->getHwcId(); });

        if (!hwcIdOpt) {
            ALOGE("%s: Invalid mode %d for display %s", whence, modeId.value(),
                  to_string(snapshot.displayId()).c_str());
            return BAD_VALUE;
        }

        return getHwComposer().setBootDisplayMode(snapshot.displayId(), *hwcIdOpt);
    });
    return future.get();
}

status_t SurfaceFlinger::clearBootDisplayMode(const sp<IBinder>& displayToken) {
    const char* const whence = __func__;
    auto future = mScheduler->schedule([=]() FTL_FAKE_GUARD(mStateLock) -> status_t {
        if (const auto displayId = getPhysicalDisplayIdLocked(displayToken)) {
            return getHwComposer().clearBootDisplayMode(*displayId);
        } else {
            ALOGE("%s: Invalid display token %p", whence, displayToken.get());
            return BAD_VALUE;
        }
    });
    return future.get();
}

void SurfaceFlinger::setAutoLowLatencyMode(const sp<IBinder>& displayToken, bool on) {
    const char* const whence = __func__;
    static_cast<void>(mScheduler->schedule([=]() FTL_FAKE_GUARD(mStateLock) {
        if (const auto displayId = getPhysicalDisplayIdLocked(displayToken)) {
            getHwComposer().setAutoLowLatencyMode(*displayId, on);
        } else {
            ALOGE("%s: Invalid display token %p", whence, displayToken.get());
        }
    }));
}

void SurfaceFlinger::setGameContentType(const sp<IBinder>& displayToken, bool on) {
    const char* const whence = __func__;
    static_cast<void>(mScheduler->schedule([=]() FTL_FAKE_GUARD(mStateLock) {
        if (const auto displayId = getPhysicalDisplayIdLocked(displayToken)) {
            const auto type = on ? hal::ContentType::GAME : hal::ContentType::NONE;
            getHwComposer().setContentType(*displayId, type);
        } else {
            ALOGE("%s: Invalid display token %p", whence, displayToken.get());
        }
    }));
}

status_t SurfaceFlinger::overrideHdrTypes(const sp<IBinder>& displayToken,
                                          const std::vector<ui::Hdr>& hdrTypes) {
    Mutex::Autolock lock(mStateLock);

    auto display = getDisplayDeviceLocked(displayToken);
    if (!display) {
        ALOGE("%s: Invalid display token %p", __func__, displayToken.get());
        return NAME_NOT_FOUND;
    }

    display->overrideHdrTypes(hdrTypes);
    dispatchDisplayHotplugEvent(display->getPhysicalId(), true /* connected */);
    return NO_ERROR;
}

status_t SurfaceFlinger::onPullAtom(const int32_t atomId, std::string* pulledData, bool* success) {
    *success = mTimeStats->onPullAtom(atomId, pulledData);
    return NO_ERROR;
}

status_t SurfaceFlinger::getDisplayedContentSamplingAttributes(const sp<IBinder>& displayToken,
                                                               ui::PixelFormat* outFormat,
                                                               ui::Dataspace* outDataspace,
                                                               uint8_t* outComponentMask) const {
    if (!outFormat || !outDataspace || !outComponentMask) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);

    const auto displayId = getPhysicalDisplayIdLocked(displayToken);
    if (!displayId) {
        return NAME_NOT_FOUND;
    }

    return getHwComposer().getDisplayedContentSamplingAttributes(*displayId, outFormat,
                                                                 outDataspace, outComponentMask);
}

status_t SurfaceFlinger::setDisplayContentSamplingEnabled(const sp<IBinder>& displayToken,
                                                          bool enable, uint8_t componentMask,
                                                          uint64_t maxFrames) {
    const char* const whence = __func__;
    auto future = mScheduler->schedule([=]() FTL_FAKE_GUARD(mStateLock) -> status_t {
        if (const auto displayId = getPhysicalDisplayIdLocked(displayToken)) {
            return getHwComposer().setDisplayContentSamplingEnabled(*displayId, enable,
                                                                    componentMask, maxFrames);
        } else {
            ALOGE("%s: Invalid display token %p", whence, displayToken.get());
            return NAME_NOT_FOUND;
        }
    });

    return future.get();
}

status_t SurfaceFlinger::getDisplayedContentSample(const sp<IBinder>& displayToken,
                                                   uint64_t maxFrames, uint64_t timestamp,
                                                   DisplayedFrameStats* outStats) const {
    Mutex::Autolock lock(mStateLock);

    const auto displayId = getPhysicalDisplayIdLocked(displayToken);
    if (!displayId) {
        return NAME_NOT_FOUND;
    }

    return getHwComposer().getDisplayedContentSample(*displayId, maxFrames, timestamp, outStats);
}

status_t SurfaceFlinger::getProtectedContentSupport(bool* outSupported) const {
    if (!outSupported) {
        return BAD_VALUE;
    }
    *outSupported = getRenderEngine().supportsProtectedContent();
    return NO_ERROR;
}

status_t SurfaceFlinger::isWideColorDisplay(const sp<IBinder>& displayToken,
                                            bool* outIsWideColorDisplay) const {
    if (!displayToken || !outIsWideColorDisplay) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);
    const auto display = getDisplayDeviceLocked(displayToken);
    if (!display) {
        return NAME_NOT_FOUND;
    }

    *outIsWideColorDisplay =
            display->isPrimary() ? hasWideColorDisplay : display->hasWideColorGamut();
    return NO_ERROR;
}

status_t SurfaceFlinger::enableVSyncInjections(bool enable) {
    auto future = mScheduler->schedule([=] {
        Mutex::Autolock lock(mStateLock);

        if (const auto handle = mScheduler->enableVSyncInjection(enable)) {
            mScheduler->setInjector(enable ? mScheduler->getEventConnection(handle) : nullptr);
        }
    });

    future.wait();
    return NO_ERROR;
}

status_t SurfaceFlinger::injectVSync(nsecs_t when) {
    Mutex::Autolock lock(mStateLock);
    const nsecs_t expectedPresentTime = calculateExpectedPresentTime(TimePoint::fromNs(when)).ns();
    const nsecs_t deadlineTimestamp = expectedPresentTime;
    return mScheduler->injectVSync(when, expectedPresentTime, deadlineTimestamp) ? NO_ERROR
                                                                                 : BAD_VALUE;
}

status_t SurfaceFlinger::getLayerDebugInfo(std::vector<gui::LayerDebugInfo>* outLayers) {
    outLayers->clear();
    auto future = mScheduler->schedule([=] {
        const auto display = FTL_FAKE_GUARD(mStateLock, getDefaultDisplayDeviceLocked());
        mDrawingState.traverseInZOrder([&](Layer* layer) {
            outLayers->push_back(layer->getLayerDebugInfo(display.get()));
        });
    });

    future.wait();
    return NO_ERROR;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

// ----------------------------------------------------------------------------

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

    const int mApi;
    ~WindowDisconnector() {
        native_window_api_disconnect(mWindow, mApi);
    }
    
    const int mApi;
    ~WindowDisconnector() {
        native_window_api_disconnect(mWindow, mApi);
    }
    
status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

// ---------------------------------------------------------------------------

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

// A simple RAII class to disconnect from an ANativeWindow* when it goes out of scope
class WindowDisconnector {
public:
    ~WindowDisconnector() {
        native_window_api_disconnect(mWindow, mApi);
    }
    
    ~WindowDisconnector() {
        native_window_api_disconnect(mWindow, mApi);
    }
    
private:
    const int mApi;
    const int mApi;
};

static Dataspace pickDataspaceFromColorMode(const ColorMode colorMode) {
    switch (colorMode) {
        case ColorMode::DISPLAY_P3:
        case ColorMode::BT2100_PQ:
        case ColorMode::BT2100_HLG:
        case ColorMode::DISPLAY_BT2020:
            return Dataspace::DISPLAY_P3;
        default:
            return Dataspace::V0_SRGB;
    }
}

static bool hasCaptureBlackoutContentPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    return uid == AID_GRAPHICS || uid == AID_SYSTEM ||
            PermissionCache::checkPermission(sCaptureBlackoutContent, pid, uid);
}

static status_t validateScreenshotPermissions(const CaptureArgs& captureArgs) {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if (uid == AID_GRAPHICS || PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        return OK;
    }

    // If the caller doesn't have the correct permissions but is only attempting to screenshot
    // itself, we allow it to continue.
    if (captureArgs.uid == uid) {
        return OK;
    }

    ALOGE("Permission Denial: can't take screenshot pid=%d, uid=%d", pid, uid);
    return PERMISSION_DENIED;
}

status_t SurfaceFlinger::setSchedFifo(bool enabled) {
    static constexpr int kFifoPriority = 2;
    static constexpr int kOtherPriority = 0;

    struct sched_param param = {0};
    int sched_policy;
    if (enabled) {
        sched_policy = SCHED_FIFO;
        param.sched_priority = kFifoPriority;
    } else {
        sched_policy = SCHED_OTHER;
        param.sched_priority = kOtherPriority;
    }

    if (sched_setscheduler(0, sched_policy, &param) != 0) {
        return -errno;
    }

    return NO_ERROR;
}

status_t SurfaceFlinger::setSchedAttr(bool enabled) {
    static const unsigned int kUclampMin =
            base::GetUintProperty<unsigned int>("ro.surface_flinger.uclamp.min", 0U);

    if (!kUclampMin) {
        // uclamp.min set to 0 (default), skip setting
        return NO_ERROR;
    }

    // Currently, there is no wrapper in bionic: b/183240349.
    struct sched_attr {
        uint32_t size;
        uint32_t sched_policy;
        uint64_t sched_flags;
        int32_t sched_nice;
        uint32_t sched_priority;
        uint64_t sched_runtime;
        uint64_t sched_deadline;
        uint64_t sched_period;
        uint32_t sched_util_min;
        uint32_t sched_util_max;
    };

    sched_attr attr = {};
    attr.size = sizeof(attr);

    attr.sched_flags = (SCHED_FLAG_KEEP_ALL | SCHED_FLAG_UTIL_CLAMP);
    attr.sched_util_min = enabled ? kUclampMin : 0;
    attr.sched_util_max = 1024;

    if (syscall(__NR_sched_setattr, 0, &attr, 0)) {
        return -errno;
    }

    return NO_ERROR;
}

status_t SurfaceFlinger::captureDisplay(const DisplayCaptureArgs& args,
                                        const sp<IScreenCaptureListener>& captureListener) {
    ATRACE_CALL();

    status_t validate = validateScreenshotPermissions(args);
    if (validate != OK) {
        return validate;
    }

    if (!args.displayToken) return BAD_VALUE;

    wp<const DisplayDevice> displayWeak;
    ui::LayerStack layerStack;
    ui::Size reqSize(args.width, args.height);
    ui::Dataspace dataspace;
    {
        Mutex::Autolock lock(mStateLock);
        sp<DisplayDevice> display = getDisplayDeviceLocked(args.displayToken);
        if (!display) return NAME_NOT_FOUND;
        displayWeak = display;
        layerStack = display->getLayerStack();

        // set the requested width/height to the logical display layer stack rect size by default
        if (args.width == 0 || args.height == 0) {
            reqSize = display->getLayerStackSpaceRect().getSize();
        }

        // The dataspace is depended on the color mode of display, that could use non-native mode
        // (ex. displayP3) to enhance the content, but some cases are checking native RGB in bytes,
        // and failed if display is not in native mode. This provide a way to force using native
        // colors when capture.
        dataspace = args.dataspace;
        if (dataspace == ui::Dataspace::UNKNOWN) {
            const ui::ColorMode colorMode = display->getCompositionDisplay()->getState().colorMode;
            dataspace = pickDataspaceFromColorMode(colorMode);
        }
    }

    RenderAreaFuture renderAreaFuture = ftl::defer([=] {
        return DisplayRenderArea::create(displayWeak, args.sourceCrop, reqSize, dataspace,
                                         args.useIdentityTransform, args.captureSecureLayers);
    });

    auto traverseLayers = [this, args, layerStack](const LayerVector::Visitor& visitor) {
        traverseLayersInLayerStack(layerStack, args.uid, visitor);
    };

    auto future = captureScreenCommon(std::move(renderAreaFuture), traverseLayers, reqSize,
                                      args.pixelFormat, args.allowProtected, args.grayscale,
                                      captureListener);
    return fenceStatus(future.get());
}

status_t SurfaceFlinger::captureDisplay(DisplayId displayId,
                                        const sp<IScreenCaptureListener>& captureListener) {
    ui::LayerStack layerStack;
    wp<const DisplayDevice> displayWeak;
    ui::Size size;
    ui::Dataspace dataspace;
    {
        Mutex::Autolock lock(mStateLock);

        const auto display = getDisplayDeviceLocked(displayId);
        if (!display) {
            return NAME_NOT_FOUND;
        }

        displayWeak = display;
        layerStack = display->getLayerStack();
        size = display->getLayerStackSpaceRect().getSize();

        dataspace =
                pickDataspaceFromColorMode(display->getCompositionDisplay()->getState().colorMode);
    }

    RenderAreaFuture renderAreaFuture = ftl::defer([=] {
        return DisplayRenderArea::create(displayWeak, Rect(), size, dataspace,
                                         false /* useIdentityTransform */,
                                         false /* captureSecureLayers */);
    });

    auto traverseLayers = [this, layerStack](const LayerVector::Visitor& visitor) {
        traverseLayersInLayerStack(layerStack, CaptureArgs::UNSET_UID, visitor);
    };

    if (captureListener == nullptr) {
        ALOGE("capture screen must provide a capture listener callback");
        return BAD_VALUE;
    }

    constexpr bool kAllowProtected = false;
    constexpr bool kGrayscale = false;

    auto future = captureScreenCommon(std::move(renderAreaFuture), traverseLayers, size,
                                      ui::PixelFormat::RGBA_8888, kAllowProtected, kGrayscale,
                                      captureListener);
    return fenceStatus(future.get());
}

status_t SurfaceFlinger::captureLayers(const LayerCaptureArgs& args,
                                       const sp<IScreenCaptureListener>& captureListener) {
    ATRACE_CALL();

    status_t validate = validateScreenshotPermissions(args);
    if (validate != OK) {
        return validate;
    }

    ui::Size reqSize;
    sp<Layer> parent;
    Rect crop(args.sourceCrop);
    std::unordered_set<sp<Layer>, SpHash<Layer>> excludeLayers;
    ui::Dataspace dataspace;

    // Call this before holding mStateLock to avoid any deadlocking.
    bool canCaptureBlackoutContent = hasCaptureBlackoutContentPermission();

    {
        Mutex::Autolock lock(mStateLock);

        parent = fromHandle(args.layerHandle).promote();
        if (parent == nullptr) {
            ALOGE("captureLayers called with an invalid or removed parent");
            return NAME_NOT_FOUND;
        }

        if (!canCaptureBlackoutContent &&
            parent->getDrawingState().flags & layer_state_t::eLayerSecure) {
            ALOGW("Attempting to capture secure layer: PERMISSION_DENIED");
            return PERMISSION_DENIED;
        }

        Rect parentSourceBounds = parent->getCroppedBufferSize(parent->getDrawingState());
        if (args.sourceCrop.width() <= 0) {
            crop.left = 0;
            crop.right = parentSourceBounds.getWidth();
        }

        if (args.sourceCrop.height() <= 0) {
            crop.top = 0;
            crop.bottom = parentSourceBounds.getHeight();
        }

        if (crop.isEmpty() || args.frameScaleX <= 0.0f || args.frameScaleY <= 0.0f) {
            // Error out if the layer has no source bounds (i.e. they are boundless) and a source
            // crop was not specified, or an invalid frame scale was provided.
            return BAD_VALUE;
        }
        reqSize = ui::Size(crop.width() * args.frameScaleX, crop.height() * args.frameScaleY);

        for (const auto& handle : args.excludeHandles) {
            sp<Layer> excludeLayer = fromHandle(handle).promote();
            if (excludeLayer != nullptr) {
                excludeLayers.emplace(excludeLayer);
            } else {
                ALOGW("Invalid layer handle passed as excludeLayer to captureLayers");
                return NAME_NOT_FOUND;
            }
        }

        // The dataspace is depended on the color mode of display, that could use non-native mode
        // (ex. displayP3) to enhance the content, but some cases are checking native RGB in bytes,
        // and failed if display is not in native mode. This provide a way to force using native
        // colors when capture.
        dataspace = args.dataspace;
    } // mStateLock

    // really small crop or frameScale
    if (reqSize.width <= 0 || reqSize.height <= 0) {
        ALOGW("Failed to captureLayes: crop or scale too small");
        return BAD_VALUE;
    }

    Rect layerStackSpaceRect(0, 0, reqSize.width, reqSize.height);
    bool childrenOnly = args.childrenOnly;
    RenderAreaFuture renderAreaFuture = ftl::defer([=]() -> std::unique_ptr<RenderArea> {
        return std::make_unique<LayerRenderArea>(*this, parent, crop, reqSize, dataspace,
                                                 childrenOnly, layerStackSpaceRect,
                                                 args.captureSecureLayers);
    });

    auto traverseLayers = [parent, args, excludeLayers](const LayerVector::Visitor& visitor) {
        parent->traverseChildrenInZOrder(LayerVector::StateSet::Drawing, [&](Layer* layer) {
            if (!layer->isVisible()) {
                return;
            } else if (args.childrenOnly && layer == parent.get()) {
                return;
            } else if (args.uid != CaptureArgs::UNSET_UID && args.uid != layer->getOwnerUid()) {
                return;
            }

            auto p = sp<Layer>::fromExisting(layer);
            while (p != nullptr) {
                if (excludeLayers.count(p) != 0) {
                    return;
                }
                p = p->getParent();
            }

            visitor(layer);
        });
    };

    if (captureListener == nullptr) {
        ALOGE("capture screen must provide a capture listener callback");
        return BAD_VALUE;
    }

    auto future = captureScreenCommon(std::move(renderAreaFuture), traverseLayers, reqSize,
                                      args.pixelFormat, args.allowProtected, args.grayscale,
                                      captureListener);
    return fenceStatus(future.get());
}

ftl::SharedFuture<FenceResult> SurfaceFlinger::captureScreenCommon(
        RenderAreaFuture renderAreaFuture, TraverseLayersFunction traverseLayers,
        ui::Size bufferSize, ui::PixelFormat reqPixelFormat, bool allowProtected, bool grayscale,
        const sp<IScreenCaptureListener>& captureListener) {
    ATRACE_CALL();

    if (exceedsMaxRenderTargetSize(bufferSize.getWidth(), bufferSize.getHeight())) {
        ALOGE("Attempted to capture screen with size (%" PRId32 ", %" PRId32
              ") that exceeds render target size limit.",
              bufferSize.getWidth(), bufferSize.getHeight());
        return ftl::yield<FenceResult>(base::unexpected(BAD_VALUE)).share();
    }

    // Loop over all visible layers to see whether there's any protected layer. A protected layer is
    // typically a layer with DRM contents, or have the GRALLOC_USAGE_PROTECTED set on the buffer.
    // A protected layer has no implication on whether it's secure, which is explicitly set by
    // application to avoid being screenshot or drawn via unsecure display.
    const bool supportsProtected = getRenderEngine().supportsProtectedContent();
    bool hasProtectedLayer = false;
    if (allowProtected && supportsProtected) {
        auto future = mScheduler->schedule([=]() {
            bool protectedLayerFound = false;
            traverseLayers([&](Layer* layer) {
                protectedLayerFound =
                        protectedLayerFound || (layer->isVisible() && layer->isProtected());
            });
            return protectedLayerFound;
        });
        hasProtectedLayer = future.get();
    }

    const uint32_t usage = GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_HW_RENDER |
            GRALLOC_USAGE_HW_TEXTURE |
            (hasProtectedLayer && allowProtected && supportsProtected
                     ? GRALLOC_USAGE_PROTECTED
                     : GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN);
    sp<GraphicBuffer> buffer =
            getFactory().createGraphicBuffer(bufferSize.getWidth(), bufferSize.getHeight(),
                                             static_cast<android_pixel_format>(reqPixelFormat),
                                             1 /* layerCount */, usage, "screenshot");

    const status_t bufferStatus = buffer->initCheck();
    if (bufferStatus != OK) {
        // Animations may end up being really janky, but don't crash here.
        // Otherwise an irreponsible process may cause an SF crash by allocating
        // too much.
        ALOGE("%s: Buffer failed to allocate: %d", __func__, bufferStatus);
        return ftl::yield<FenceResult>(base::unexpected(bufferStatus)).share();
    }
    const std::shared_ptr<renderengine::ExternalTexture> texture = std::make_shared<
            renderengine::impl::ExternalTexture>(buffer, getRenderEngine(),
                                                 renderengine::impl::ExternalTexture::Usage::
                                                         WRITEABLE);
    return captureScreenCommon(std::move(renderAreaFuture), traverseLayers, texture,
                               false /* regionSampling */, grayscale, captureListener);
}

ftl::SharedFuture<FenceResult> SurfaceFlinger::captureScreenCommon(
        RenderAreaFuture renderAreaFuture, TraverseLayersFunction traverseLayers,
        const std::shared_ptr<renderengine::ExternalTexture>& buffer, bool regionSampling,
        bool grayscale, const sp<IScreenCaptureListener>& captureListener) {
    ATRACE_CALL();

    bool canCaptureBlackoutContent = hasCaptureBlackoutContentPermission();

    auto future = mScheduler->schedule([=, renderAreaFuture = std::move(renderAreaFuture)]() mutable
                                       -> ftl::SharedFuture<FenceResult> {
        ScreenCaptureResults captureResults;
        std::unique_ptr<RenderArea> renderArea = renderAreaFuture.get();
        if (!renderArea) {
            ALOGW("Skipping screen capture because of invalid render area.");
            captureResults.fenceResult = base::unexpected(NO_MEMORY);
            captureListener->onScreenCaptureCompleted(captureResults);
            return ftl::yield<FenceResult>(base::unexpected(NO_ERROR)).share();
        }

        ftl::SharedFuture<FenceResult> renderFuture;
        renderArea->render([&] {
            renderFuture =
                    renderScreenImpl(*renderArea, traverseLayers, buffer, canCaptureBlackoutContent,
                                     regionSampling, grayscale, captureResults);
        });

        if (captureListener) {
            // Defer blocking on renderFuture back to the Binder thread.
            return ftl::Future(std::move(renderFuture))
                    .then([captureListener, captureResults = std::move(captureResults)](
                                  FenceResult fenceResult) mutable -> FenceResult {
                        captureResults.fenceResult = std::move(fenceResult);
                        captureListener->onScreenCaptureCompleted(captureResults);
                        return base::unexpected(NO_ERROR);
                    })
                    .share();
        }
        return renderFuture;
    });

    // Flatten nested futures.
    auto chain = ftl::Future(std::move(future)).then([](ftl::SharedFuture<FenceResult> future) {
        return future;
    });

    return chain.share();
}

ftl::SharedFuture<FenceResult> SurfaceFlinger::renderScreenImpl(
        const RenderArea& renderArea, TraverseLayersFunction traverseLayers,
        const std::shared_ptr<renderengine::ExternalTexture>& buffer,
        bool canCaptureBlackoutContent, bool regionSampling, bool grayscale,
        ScreenCaptureResults& captureResults) {
    ATRACE_CALL();

    traverseLayers([&](Layer* layer) {
        captureResults.capturedSecureLayers =
                captureResults.capturedSecureLayers || (layer->isVisible() && layer->isSecure());
    });

    const bool useProtected = buffer->getUsage() & GRALLOC_USAGE_PROTECTED;

    // We allow the system server to take screenshots of secure layers for
    // use in situations like the Screen-rotation animation and place
    // the impetus on WindowManager to not persist them.
    if (captureResults.capturedSecureLayers && !canCaptureBlackoutContent) {
        ALOGW("FB is protected: PERMISSION_DENIED");
        return ftl::yield<FenceResult>(base::unexpected(PERMISSION_DENIED)).share();
    }

    captureResults.buffer = buffer->getBuffer();
    auto dataspace = renderArea.getReqDataSpace();
    auto parent = renderArea.getParentLayer();
    auto renderIntent = RenderIntent::TONE_MAP_COLORIMETRIC;
    auto sdrWhitePointNits = DisplayDevice::sDefaultMaxLumiance;
    auto displayBrightnessNits = DisplayDevice::sDefaultMaxLumiance;

    if ((dataspace == ui::Dataspace::UNKNOWN) && (parent != nullptr)) {
        Mutex::Autolock lock(mStateLock);
        auto display = findDisplay([layerStack = parent->getLayerStack()](const auto& display) {
            return display.getLayerStack() == layerStack;
        });
        if (!display) {
            // If the layer is not on a display, use the dataspace for the default display.
            display = getDefaultDisplayDeviceLocked();
        }

        const ui::ColorMode colorMode = display->getCompositionDisplay()->getState().colorMode;
        dataspace = pickDataspaceFromColorMode(colorMode);
        renderIntent = display->getCompositionDisplay()->getState().renderIntent;
        sdrWhitePointNits = display->getCompositionDisplay()->getState().sdrWhitePointNits;
        displayBrightnessNits = display->getCompositionDisplay()->getState().displayBrightnessNits;
    }
    captureResults.capturedDataspace = dataspace;

    const auto reqWidth = renderArea.getReqWidth();
    const auto reqHeight = renderArea.getReqHeight();
    const auto sourceCrop = renderArea.getSourceCrop();
    const auto transform = renderArea.getTransform();
    const auto rotation = renderArea.getRotationFlags();
    const auto& layerStackSpaceRect = renderArea.getLayerStackSpaceRect();

    renderengine::DisplaySettings clientCompositionDisplay;
    std::vector<compositionengine::LayerFE::LayerSettings> clientCompositionLayers;

    // assume that bounds are never offset, and that they are the same as the
    // buffer bounds.
    clientCompositionDisplay.physicalDisplay = Rect(reqWidth, reqHeight);
    clientCompositionDisplay.clip = sourceCrop;
    clientCompositionDisplay.orientation = rotation;

    clientCompositionDisplay.outputDataspace = dataspace;
    clientCompositionDisplay.currentLuminanceNits = displayBrightnessNits;
    clientCompositionDisplay.maxLuminance = DisplayDevice::sDefaultMaxLumiance;
    clientCompositionDisplay.renderIntent =
            static_cast<aidl::android::hardware::graphics::composer3::RenderIntent>(renderIntent);

    const float colorSaturation = grayscale ? 0 : 1;
    clientCompositionDisplay.colorTransform = calculateColorMatrix(colorSaturation);

    const float alpha = RenderArea::getCaptureFillValue(renderArea.getCaptureFill());

    compositionengine::LayerFE::LayerSettings fillLayer;
    fillLayer.source.buffer.buffer = nullptr;
    fillLayer.source.solidColor = half3(0.0, 0.0, 0.0);
    fillLayer.geometry.boundaries =
            FloatRect(sourceCrop.left, sourceCrop.top, sourceCrop.right, sourceCrop.bottom);
    fillLayer.alpha = half(alpha);
    clientCompositionLayers.push_back(fillLayer);

    const auto display = renderArea.getDisplayDevice();
    std::vector<Layer*> renderedLayers;
    bool disableBlurs = false;
    traverseLayers([&](Layer* layer) {
        // Layer::prepareClientComposition uses the layer's snapshot to populate the resulting
        // LayerSettings. Calling Layer::updateSnapshot ensures that LayerSettings are
        // generated with the layer's current buffer and geometry.
        layer->updateSnapshot(true /* updateGeometry */);

        disableBlurs |= layer->getDrawingState().sidebandStream != nullptr;

        Region clip(renderArea.getBounds());
        compositionengine::LayerFE::ClientCompositionTargetSettings targetSettings{
                clip,
                layer->needsFilteringForScreenshots(display.get(), transform) ||
                        renderArea.needsFiltering(),
                renderArea.isSecure(),
                useProtected,
                layerStackSpaceRect,
                clientCompositionDisplay.outputDataspace,
                true,  /* realContentIsVisible */
                false, /* clearContent */
                disableBlurs ? compositionengine::LayerFE::ClientCompositionTargetSettings::
                                       BlurSetting::Disabled
                             : compositionengine::LayerFE::ClientCompositionTargetSettings::
                                       BlurSetting::Enabled,
                isHdrLayer(layer) ? displayBrightnessNits : sdrWhitePointNits,

        };
        std::optional<compositionengine::LayerFE::LayerSettings> settings =
                layer->prepareClientComposition(targetSettings);
        if (!settings) {
            return;
        }

        settings->geometry.positionTransform =
                transform.asMatrix4() * settings->geometry.positionTransform;
        // There's no need to process blurs when we're executing region sampling,
        // we're just trying to understand what we're drawing, and doing so without
        // blurs is already a pretty good approximation.
        if (regionSampling) {
            settings->backgroundBlurRadius = 0;
            settings->blurRegions.clear();
        }
        captureResults.capturedHdrLayers |= isHdrLayer(layer);

        clientCompositionLayers.push_back(std::move(*settings));
        renderedLayers.push_back(layer);
    });

    std::vector<renderengine::LayerSettings> clientRenderEngineLayers;
    clientRenderEngineLayers.reserve(clientCompositionLayers.size());
    std::transform(clientCompositionLayers.begin(), clientCompositionLayers.end(),
                   std::back_inserter(clientRenderEngineLayers),
                   [](compositionengine::LayerFE::LayerSettings& settings)
                           -> renderengine::LayerSettings { return settings; });

    // Use an empty fence for the buffer fence, since we just created the buffer so
    // there is no need for synchronization with the GPU.
    base::unique_fd bufferFence;
    getRenderEngine().useProtectedContext(useProtected);

    constexpr bool kUseFramebufferCache = false;
    const auto future = getRenderEngine()
                                .drawLayers(clientCompositionDisplay, clientRenderEngineLayers,
                                            buffer, kUseFramebufferCache, std::move(bufferFence))
                                .share();

    for (auto* layer : renderedLayers) {
        layer->onLayerDisplayed(future);
    }

    // Always switch back to unprotected context.
    getRenderEngine().useProtectedContext(false);

    return future;
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
void SurfaceFlinger::State::traverse(const LayerVector::Visitor& visitor) const {
    layersSortedByZ.traverse(visitor);
}

void SurfaceFlinger::State::traverseInZOrder(const LayerVector::Visitor& visitor) const {
    layersSortedByZ.traverseInZOrder(stateSet, visitor);
}

void SurfaceFlinger::State::traverseInReverseZOrder(const LayerVector::Visitor& visitor) const {
    layersSortedByZ.traverseInReverseZOrder(stateSet, visitor);
}

void SurfaceFlinger::traverseLayersInLayerStack(ui::LayerStack layerStack, const int32_t uid,
                                                const LayerVector::Visitor& visitor) {
    // We loop through the first level of layers without traversing,
    // as we need to determine which layers belong to the requested display.
    for (const auto& layer : mDrawingState.layersSortedByZ) {
        if (layer->getLayerStack() != layerStack) {
            continue;
        }
        // relative layers are traversed in Layer::traverseInZOrder
        layer->traverseInZOrder(LayerVector::StateSet::Drawing, [&](Layer* layer) {
            if (layer->isInternalDisplayOverlay()) {
                return;
            }
            if (!layer->isVisible()) {
                return;
            }
            if (uid != CaptureArgs::UNSET_UID && layer->getOwnerUid() != uid) {
                return;
            }
            visitor(layer);
        });
    }
}

std::optional<DisplayModePtr> SurfaceFlinger::getPreferredDisplayMode(PhysicalDisplayId displayId, DisplayModeId defaultModeId) const {
    if (const auto schedulerMode = mScheduler->getPreferredDisplayMode();
        schedulerMode && schedulerMode->getPhysicalDisplayId() == displayId) {
        return schedulerMode;
    }

    return mPhysicalDisplays.get(displayId)
            .transform(&PhysicalDisplay::snapshotRef)
            .and_then([&](const display::DisplaySnapshot& snapshot) {
                return snapshot.displayModes().get(defaultModeId);
            });
}

status_t SurfaceFlinger::setDesiredDisplayModeSpecsInternal(
        const sp<DisplayDevice>& display,
        const std::optional<scheduler::RefreshRateConfigs::Policy>& policy, bool overridePolicy) {
    Mutex::Autolock lock(mStateLock);

    if (mDebugDisplayModeSetByBackdoor) {
        // ignore this request as mode is overridden by backdoor
        return NO_ERROR;
    }

    const status_t setPolicyResult = display->setRefreshRatePolicy(policy, overridePolicy);
    if (setPolicyResult < 0) {
        return BAD_VALUE;
    }
    if (setPolicyResult == scheduler::RefreshRateConfigs::CURRENT_POLICY_UNCHANGED) {
        return NO_ERROR;
    }

    const scheduler::RefreshRateConfigs::Policy currentPolicy =
            display->refreshRateConfigs().getCurrentPolicy();

    ALOGV("Setting desired display mode specs: %s", currentPolicy.toString().c_str());

    // TODO(b/140204874): Leave the event in until we do proper testing with all apps that might
    // be depending in this callback.
    const auto activeModePtr = display->refreshRateConfigs().getActiveModePtr();
    if (isDisplayActiveLocked(display)) {
        mScheduler->onPrimaryDisplayModeChanged(mAppConnectionHandle, activeModePtr);
        toggleKernelIdleTimer();
    } else {
        mScheduler->onNonPrimaryDisplayModeChanged(mAppConnectionHandle, activeModePtr);
    }

    auto preferredModeOpt =
            getPreferredDisplayMode(display->getPhysicalId(), currentPolicy.defaultMode);
    if (!preferredModeOpt) {
        ALOGE("%s: Preferred mode is unknown", __func__);
        return NAME_NOT_FOUND;
    }

    auto preferredMode = std::move(*preferredModeOpt);
    const auto preferredModeId = preferredMode->getId();

    ALOGV("Switching to Scheduler preferred mode %d (%s)", preferredModeId.value(),
          to_string(preferredMode->getFps()).c_str());

    if (!display->refreshRateConfigs().isModeAllowed(preferredModeId)) {
        ALOGE("%s: Preferred mode %d is disallowed", __func__, preferredModeId.value());
        return INVALID_OPERATION;
    }

    setDesiredActiveMode({std::move(preferredMode), DisplayModeEvent::Changed});
    return NO_ERROR;
}

status_t SurfaceFlinger::setDesiredDisplayModeSpecs(
        const sp<IBinder>& displayToken, ui::DisplayModeId defaultMode, bool allowGroupSwitching,
        float primaryRefreshRateMin, float primaryRefreshRateMax, float appRequestRefreshRateMin,
        float appRequestRefreshRateMax) {
    ATRACE_CALL();

    if (!displayToken) {
        return BAD_VALUE;
    }

    auto future = mScheduler->schedule([=]() -> status_t {
        const auto display = FTL_FAKE_GUARD(mStateLock, getDisplayDeviceLocked(displayToken));
        if (!display) {
            ALOGE("Attempt to set desired display modes for invalid display token %p",
                  displayToken.get());
            return NAME_NOT_FOUND;
        } else if (display->isVirtual()) {
            ALOGW("Attempt to set desired display modes for virtual display");
            return INVALID_OPERATION;
        } else {
            using Policy = scheduler::RefreshRateConfigs::Policy;
            const Policy policy{DisplayModeId(defaultMode),
                                allowGroupSwitching,
                                {Fps::fromValue(primaryRefreshRateMin),
                                 Fps::fromValue(primaryRefreshRateMax)},
                                {Fps::fromValue(appRequestRefreshRateMin),
                                 Fps::fromValue(appRequestRefreshRateMax)}};
            constexpr bool kOverridePolicy = false;

            return setDesiredDisplayModeSpecsInternal(display, policy, kOverridePolicy);
        }
    });

    return future.get();
}

status_t SurfaceFlinger::getDesiredDisplayModeSpecs(const sp<IBinder>& displayToken,
                                                    ui::DisplayModeId* outDefaultMode,
                                                    bool* outAllowGroupSwitching,
                                                    float* outPrimaryRefreshRateMin,
                                                    float* outPrimaryRefreshRateMax,
                                                    float* outAppRequestRefreshRateMin,
                                                    float* outAppRequestRefreshRateMax) {
    ATRACE_CALL();

    if (!displayToken || !outDefaultMode || !outPrimaryRefreshRateMin ||
        !outPrimaryRefreshRateMax || !outAppRequestRefreshRateMin || !outAppRequestRefreshRateMax) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);
    const auto display = getDisplayDeviceLocked(displayToken);
    if (!display) {
        return NAME_NOT_FOUND;
    }

    if (display->isVirtual()) {
        return INVALID_OPERATION;
    }

    scheduler::RefreshRateConfigs::Policy policy =
            display->refreshRateConfigs().getDisplayManagerPolicy();
    *outDefaultMode = policy.defaultMode.value();
    *outAllowGroupSwitching = policy.allowGroupSwitching;
    *outPrimaryRefreshRateMin = policy.primaryRange.min.getValue();
    *outPrimaryRefreshRateMax = policy.primaryRange.max.getValue();
    *outAppRequestRefreshRateMin = policy.appRequestRange.min.getValue();
    *outAppRequestRefreshRateMax = policy.appRequestRange.max.getValue();
    return NO_ERROR;
}

wp<Layer> SurfaceFlinger::fromHandle(const sp<IBinder>& handle) const {
    return Layer::fromHandle(handle);
}

void SurfaceFlinger::onLayerFirstRef(Layer* layer) {
    mNumLayers++;
    if (!layer->isRemovedFromCurrentState()) {
        mScheduler->registerLayer(layer);
    }
}

void SurfaceFlinger::onLayerDestroyed(Layer* layer) {
    mNumLayers--;
    removeHierarchyFromOffscreenLayers(layer);
    if (!layer->isRemovedFromCurrentState()) {
        mScheduler->deregisterLayer(layer);
    }
    if (mTransactionTracing) {
        mTransactionTracing->onLayerRemoved(layer->getSequence());
    }
}

void SurfaceFlinger::onLayerUpdate() {
    scheduleCommit(FrameHint::kActive);
}

// WARNING: ONLY CALL THIS FROM LAYER DTOR
// Here we add children in the current state to offscreen layers and remove the
// layer itself from the offscreen layer list.  Since
// this is the dtor, it is safe to access the current state.  This keeps us
// from dangling children layers such that they are not reachable from the
// Drawing state nor the offscreen layer list
// See b/141111965
void SurfaceFlinger::removeHierarchyFromOffscreenLayers(Layer* layer) {
    for (auto& child : layer->getCurrentChildren()) {
        mOffscreenLayers.emplace(child.get());
    }
    mOffscreenLayers.erase(layer);
}

void SurfaceFlinger::removeFromOffscreenLayers(Layer* layer) {
    mOffscreenLayers.erase(layer);
}

status_t SurfaceFlinger::setGlobalShadowSettings(const half4& ambientColor, const half4& spotColor,
                                                 float lightPosY, float lightPosZ,
                                                 float lightRadius) {
    Mutex::Autolock _l(mStateLock);
    mCurrentState.globalShadowSettings.ambientColor = vec4(ambientColor);
    mCurrentState.globalShadowSettings.spotColor = vec4(spotColor);
    mCurrentState.globalShadowSettings.lightPos.y = lightPosY;
    mCurrentState.globalShadowSettings.lightPos.z = lightPosZ;
    mCurrentState.globalShadowSettings.lightRadius = lightRadius;

    // these values are overridden when calculating the shadow settings for a layer.
    mCurrentState.globalShadowSettings.lightPos.x = 0.f;
    mCurrentState.globalShadowSettings.length = 0.f;
    return NO_ERROR;
}

const std::unordered_map<std::string, uint32_t>& SurfaceFlinger::getGenericLayerMetadataKeyMap()
        const {
    // TODO(b/149500060): Remove this fixed/static mapping. Please prefer taking
    // on the work to remove the table in that bug rather than adding more to
    // it.
    static const std::unordered_map<std::string, uint32_t> genericLayerMetadataKeyMap{
            {"org.chromium.arc.V1_0.TaskId", gui::METADATA_TASK_ID},
            {"org.chromium.arc.V1_0.CursorInfo", gui::METADATA_MOUSE_CURSOR},
    };
    return genericLayerMetadataKeyMap;
}

status_t SurfaceFlinger::setOverrideFrameRate(uid_t uid, float frameRate) {
    PhysicalDisplayId displayId = [&]() {
        Mutex::Autolock lock(mStateLock);
        return getDefaultDisplayDeviceLocked()->getPhysicalId();
    }();

    mScheduler->setGameModeRefreshRateForUid(FrameRateOverride{static_cast<uid_t>(uid), frameRate});
    mScheduler->onFrameRateOverridesChanged(mAppConnectionHandle, displayId);
    return NO_ERROR;
}

void SurfaceFlinger::enableRefreshRateOverlay(bool enable) {
    for (const auto& [id, display] : mPhysicalDisplays) {
        if (display.snapshot().connectionType() == ui::DisplayConnectionType::Internal) {
            if (const auto device = getDisplayDeviceLocked(id)) {
                device->enableRefreshRateOverlay(enable, mRefreshRateOverlaySpinner);
            }
        }
    }
}

status_t SurfaceFlinger::addTransactionTraceListener(
        const sp<gui::ITransactionTraceListener>& listener) {
    if (!listener) {
        return BAD_VALUE;
    }

    mInterceptor->addTransactionTraceListener(listener);

    return NO_ERROR;
}

int SurfaceFlinger::getGpuContextPriority() {
    return getRenderEngine().getContextPriority();
}

int SurfaceFlinger::calculateMaxAcquiredBufferCount(Fps refreshRate,
                                                    std::chrono::nanoseconds presentLatency) {
    auto pipelineDepth = presentLatency.count() / refreshRate.getPeriodNsecs();
    if (presentLatency.count() % refreshRate.getPeriodNsecs()) {
        pipelineDepth++;
    }
    return std::max(1ll, pipelineDepth - 1);
}

status_t SurfaceFlinger::getMaxAcquiredBufferCount(int* buffers) const {
    Fps maxRefreshRate = 60_Hz;

    if (!getHwComposer().isHeadless()) {
        if (const auto display = getDefaultDisplayDevice()) {
            maxRefreshRate = display->refreshRateConfigs().getSupportedRefreshRateRange().max;
        }
    }

    *buffers = getMaxAcquiredBufferCountForRefreshRate(maxRefreshRate);
    return NO_ERROR;
}

uint32_t SurfaceFlinger::getMaxAcquiredBufferCountForCurrentRefreshRate(uid_t uid) const {
    Fps refreshRate = 60_Hz;

    if (const auto frameRateOverride = mScheduler->getFrameRateOverride(uid)) {
        refreshRate = *frameRateOverride;
    } else if (!getHwComposer().isHeadless()) {
        if (const auto display = FTL_FAKE_GUARD(mStateLock, getDefaultDisplayDeviceLocked())) {
            refreshRate = display->refreshRateConfigs().getActiveModePtr()->getFps();
        }
    }

    return getMaxAcquiredBufferCountForRefreshRate(refreshRate);
}

int SurfaceFlinger::getMaxAcquiredBufferCountForRefreshRate(Fps refreshRate) const {
    const auto vsyncConfig = mVsyncConfiguration->getConfigsForRefreshRate(refreshRate).late;
    const auto presentLatency = vsyncConfig.appWorkDuration + vsyncConfig.sfWorkDuration;
    return calculateMaxAcquiredBufferCount(refreshRate, presentLatency);
}

void SurfaceFlinger::handleLayerCreatedLocked(const LayerCreatedState& state, VsyncId vsyncId) {
    sp<Layer> layer = state.layer.promote();
    if (!layer) {
        ALOGD("Layer was destroyed soon after creation %p", state.layer.unsafe_get());
        return;
    }

    sp<Layer> parent;
    bool addToRoot = state.addToRoot;
    if (state.initialParent != nullptr) {
        parent = state.initialParent.promote();
        if (parent == nullptr) {
            ALOGD("Parent was destroyed soon after creation %p", state.initialParent.unsafe_get());
            addToRoot = false;
        }
    }

    if (parent == nullptr && addToRoot) {
        layer->setIsAtRoot(true);
        mCurrentState.layersSortedByZ.add(layer);
    } else if (parent == nullptr) {
        layer->onRemovedFromCurrentState();
    } else if (parent->isRemovedFromCurrentState()) {
        parent->addChild(layer);
        layer->onRemovedFromCurrentState();
    } else {
        parent->addChild(layer);
    }

    layer->updateTransformHint(mActiveDisplayTransformHint);
    if (mTransactionTracing) {
        mTransactionTracing->onLayerAddedToDrawingState(layer->getSequence(), vsyncId.value);
    }
    mInterceptor->saveSurfaceCreation(layer);
}

void SurfaceFlinger::sample() {
    if (!mLumaSampling || !mRegionSamplingThread) {
        return;
    }

    mRegionSamplingThread->onCompositionComplete(mScheduler->getScheduledFrameTime());
}

void SurfaceFlinger::onActiveDisplaySizeChanged(const sp<const DisplayDevice>& activeDisplay) {
    mScheduler->onActiveDisplayAreaChanged(activeDisplay->getWidth() * activeDisplay->getHeight());
    getRenderEngine().onActiveDisplaySizeChanged(activeDisplay->getSize());
}

void SurfaceFlinger::onActiveDisplayChangedLocked(const sp<DisplayDevice>& activeDisplay) {
    ATRACE_CALL();

    if (const auto display = getDisplayDeviceLocked(mActiveDisplayToken)) {
        display->getCompositionDisplay()->setLayerCachingTexturePoolEnabled(false);
    }

    if (!activeDisplay) {
        ALOGE("%s: activeDisplay is null", __func__);
        return;
    }
    mActiveDisplayToken = activeDisplay->getDisplayToken();
    activeDisplay->getCompositionDisplay()->setLayerCachingTexturePoolEnabled(true);
    updateInternalDisplayVsyncLocked(activeDisplay);
    mScheduler->setModeChangePending(false);
    mScheduler->setRefreshRateConfigs(activeDisplay->holdRefreshRateConfigs());
    onActiveDisplaySizeChanged(activeDisplay);
    mActiveDisplayTransformHint = activeDisplay->getTransformHint();

    // Update the kernel timer for the current active display, since the policy
    // for this display might have changed when it was not the active display.
    toggleKernelIdleTimer();
}

status_t SurfaceFlinger::addWindowInfosListener(
        const sp<IWindowInfosListener>& windowInfosListener) const {
    mWindowInfosListenerInvoker->addWindowInfosListener(windowInfosListener);
    return NO_ERROR;
}

status_t SurfaceFlinger::removeWindowInfosListener(
        const sp<IWindowInfosListener>& windowInfosListener) const {
    mWindowInfosListenerInvoker->removeWindowInfosListener(windowInfosListener);
    return NO_ERROR;
}

std::shared_ptr<renderengine::ExternalTexture> SurfaceFlinger::getExternalTextureFromBufferData(
        const BufferData& bufferData, const char* layerName) const {
    bool cacheIdChanged = bufferData.flags.test(BufferData::BufferDataChange::cachedBufferChanged);
    bool bufferSizeExceedsLimit = false;
    std::shared_ptr<renderengine::ExternalTexture> buffer = nullptr;
    if (cacheIdChanged && bufferData.buffer != nullptr) {
        bufferSizeExceedsLimit = exceedsMaxRenderTargetSize(bufferData.buffer->getWidth(),
                                                            bufferData.buffer->getHeight());
        if (!bufferSizeExceedsLimit) {
            ClientCache::getInstance().add(bufferData.cachedBuffer, bufferData.buffer);
            buffer = ClientCache::getInstance().get(bufferData.cachedBuffer);
        }
    } else if (cacheIdChanged) {
        buffer = ClientCache::getInstance().get(bufferData.cachedBuffer);
    } else if (bufferData.buffer != nullptr) {
        bufferSizeExceedsLimit = exceedsMaxRenderTargetSize(bufferData.buffer->getWidth(),
                                                            bufferData.buffer->getHeight());
        if (!bufferSizeExceedsLimit) {
            buffer = std::make_shared<
                    renderengine::impl::ExternalTexture>(bufferData.buffer, getRenderEngine(),
                                                         renderengine::impl::ExternalTexture::
                                                                 Usage::READABLE);
        }
    }
    ALOGE_IF(bufferSizeExceedsLimit,
             "Attempted to create an ExternalTexture for layer %s that exceeds render target size "
             "limit.",
             layerName);
    return buffer;
}

bool SurfaceFlinger::commitMirrorDisplays(VsyncId vsyncId) {
    std::vector<MirrorDisplayState> mirrorDisplays;
    {
        std::scoped_lock<std::mutex> lock(mMirrorDisplayLock);
        mirrorDisplays = std::move(mMirrorDisplays);
        mMirrorDisplays.clear();
        if (mirrorDisplays.size() == 0) {
            return false;
        }
    }

    sp<IBinder> unused;
    for (const auto& mirrorDisplay : mirrorDisplays) {
        // Set mirror layer's default layer stack to -1 so it doesn't end up rendered on a display
        // accidentally.
        sp<Layer> rootMirrorLayer = Layer::fromHandle(mirrorDisplay.rootHandle).promote();
        rootMirrorLayer->setLayerStack(ui::LayerStack::fromValue(-1));
        for (const auto& layer : mDrawingState.layersSortedByZ) {
            if (layer->getLayerStack() != mirrorDisplay.layerStack ||
                layer->isInternalDisplayOverlay()) {
                continue;
            }

            LayerCreationArgs mirrorArgs(this, mirrorDisplay.client, "MirrorLayerParent",
                                         ISurfaceComposerClient::eNoColorFill,
                                         gui::LayerMetadata());
            sp<Layer> childMirror;
            createEffectLayer(mirrorArgs, &unused, &childMirror);
            childMirror->setClonedChild(layer->createClone());
            if (mTransactionTracing) {
                mTransactionTracing->onLayerAddedToDrawingState(childMirror->getSequence(),
                                                                vsyncId.value);
            }
            childMirror->reparent(mirrorDisplay.rootHandle);
        }
    }
    return true;
}

bool SurfaceFlinger::commitCreatedLayers(VsyncId vsyncId) {
    std::vector<LayerCreatedState> createdLayers;
    {
        std::scoped_lock<std::mutex> lock(mCreatedLayersLock);
        createdLayers = std::move(mCreatedLayers);
        mCreatedLayers.clear();
        if (createdLayers.size() == 0) {
            return false;
        }
    }

    Mutex::Autolock _l(mStateLock);
    for (const auto& createdLayer : createdLayers) {
        handleLayerCreatedLocked(createdLayer, vsyncId);
    }
    createdLayers.clear();
    mLayersAdded = true;
    return true;
}

// gui::ISurfaceComposer
binder::Status SurfaceComposerAIDL::bootFinished() {
    status_t status = checkAccessPermission();
    if (status != OK) {
        return binderStatusFromStatusT(status);
    }
    mFlinger->bootFinished();
    return binder::Status::ok();
}

binder::Status SurfaceComposerAIDL::createDisplayEventConnection(VsyncSource vsyncSource, EventRegistration eventRegistration, sp<IDisplayEventConnection>* outConnection) {
    sp<IDisplayEventConnection> conn =
            mFlinger->createDisplayEventConnection(vsyncSource, eventRegistration);
    if (conn == nullptr) {
        *outConnection = nullptr;
        return binderStatusFromStatusT(BAD_VALUE);
    } else {
        *outConnection = conn;
        return binder::Status::ok();
    }
}

binder::Status SurfaceComposerAIDL::createConnection(sp<gui::ISurfaceComposerClient>* outClient) {
    const sp<Client> client = sp<Client>::make(mFlinger);
    if (client->initCheck() == NO_ERROR) {
        *outClient = client;
        return binder::Status::ok();
    } else {
        *outClient = nullptr;
        return binderStatusFromStatusT(BAD_VALUE);
    }
}

// gui::ISurfaceComposer

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

status_t SurfaceComposerAIDL::checkReadFrameBufferPermission() {
    IPCThreadState* ipc = IPCThreadState::self();
    const int pid = ipc->getCallingPid();
    const int uid = ipc->getCallingUid();
    if ((uid != AID_GRAPHICS) && !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
        ALOGE("Permission Denial: can't read framebuffer pid=%d, uid=%d", pid, uid);
        return PERMISSION_DENIED;
    }
    return OK;
}

  // namespace android