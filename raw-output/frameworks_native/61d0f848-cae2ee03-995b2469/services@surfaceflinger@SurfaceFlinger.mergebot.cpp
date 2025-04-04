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
//#define LOG_NDEBUG 0

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include "SurfaceFlinger.h"
#include <android-base/properties.h>
#include <android/configuration.h>
#include <android/hardware/configstore/1.0/ISurfaceFlingerConfigs.h>
#include <android/hardware/configstore/1.1/ISurfaceFlingerConfigs.h>
#include <android/hardware/configstore/1.1/types.h>
#include <android/hardware/power/1.0/IPower.h>
#include <android/native_window.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/PermissionCache.h>
#include <compositionengine/CompositionEngine.h>
#include <compositionengine/CompositionRefreshArgs.h>
#include <compositionengine/Display.h>
#include <compositionengine/DisplayColorProfile.h>
#include <compositionengine/Layer.h>
#include <compositionengine/DisplayCreationArgs.h>
#include <compositionengine/LayerFECompositionState.h>
#include <compositionengine/impl/LayerCompositionState.h>
#include <compositionengine/impl/OutputLayerCompositionState.h>
#include <compositionengine/OutputLayer.h>
#include <compositionengine/RenderSurface.h>
#include <compositionengine/impl/OutputCompositionState.h>
#include <configstore/Utils.h>
#include <cutils/compiler.h>
#include <cutils/properties.h>
#include <dlfcn.h>
#include <dvr/vr_flinger.h>
#include <errno.h>
#include <gui/BufferQueue.h>
#include <gui/DebugEGLImageTracker.h>
#include <gui/GuiConfig.h>
#include <gui/IDisplayEventConnection.h>
#include <gui/IProducerListener.h>
#include <gui/LayerDebugInfo.h>
#include <gui/LayerMetadata.h>
#include <gui/LayerState.h>
#include <gui/Surface.h>
#include <input/IInputFlinger.h>
#include <layerproto/LayerProtoParser.h>
#include <log/log.h>
#include <private/android_filesystem_config.h>
#include <private/gui/SyncFeatures.h>
#include <renderengine/RenderEngine.h>
#include <statslog.h>
#include <sys/types.h>
#include <ui/ColorSpace.h>
#include <ui/DebugUtils.h>
#include <ui/DisplayConfig.h>
#include <ui/DisplayInfo.h>
#include <ui/DisplayStatInfo.h>
#include <ui/DisplayState.h>
#include <ui/GraphicBufferAllocator.h>
#include <ui/PixelFormat.h>
#include <ui/UiConfig.h>
#include <utils/StopWatch.h>
#include <utils/String16.h>
#include <utils/String8.h>
#include <utils/Timers.h>
#include <utils/Trace.h>
#include <utils/misc.h>
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include "ColorLayer.h"
#include <optional>
#include <unordered_map>
#include "BufferLayer.h"
#include "BufferQueueLayer.h"
#include "BufferStateLayer.h"
#include "Client.h"
#include "Colorizer.h"
#include "ContainerLayer.h"
#include "DisplayDevice.h"
#include "DisplayHardware/ComposerHal.h"
#include "DisplayHardware/DisplayIdentification.h"
#include "DisplayHardware/FramebufferSurface.h"
#include "DisplayHardware/HWComposer.h"
#include "DisplayHardware/VirtualDisplaySurface.h"
#include "EffectLayer.h"
#include "Effects/Daltonizer.h"
#include "FrameTracer/FrameTracer.h"
#include "Layer.h"
#include "LayerVector.h"
#include "MonitoredProducer.h"
#include "Scheduler/InjectVSyncSource.h"
#include "NativeWindowSurface.h"
#include "Promise.h"
#include "RefreshRateOverlay.h"
#include "RegionSamplingThread.h"
#include "Scheduler/DispSync.h"
#include "Scheduler/DispSyncSource.h"
#include "Scheduler/EventControlThread.h"
#include "Scheduler/EventThread.h"
#include "Scheduler/LayerHistory.h"
#include "Scheduler/MessageQueue.h"
#include "Scheduler/PhaseOffsets.h"
#include "Scheduler/Scheduler.h"
#include "StartPropertySetThread.h"
#include "SurfaceFlingerProperties.h"
#include "SurfaceInterceptor.h"
#include "TimeStats/TimeStats.h"
#include "android-base/parseint.h"
#include "android-base/stringprintf.h"
#define MAIN_THREAD ACQUIRE(mStateLock) RELEASE(mStateLock)
#define ON_MAIN_THREAD(expr)                                       \
    [&] {                                                          \
        LOG_FATAL_IF(std::this_thread::get_id() != mMainThreadId); \
        UnnecessaryLock lock(mStateLock);                          \
        return (expr);                                             \
    }()
#undef NO_THREAD_SAFETY_ANALYSIS
#define NO_THREAD_SAFETY_ANALYSIS \
    _Pragma("GCC error \"Prefer MAIN_THREAD macros or {Conditional,Timed,Unnecessary}Lock.\"")

namespace android {

using namespace std::string_literals;

using namespace android::hardware::configstore;
using namespace android::hardware::configstore::V1_0;
using namespace android::sysprop;

using android::hardware::power::V1_0::PowerHint;
using base::StringAppendF;
using ui::ColorMode;
using ui::Dataspace;
using ui::DisplayPrimaries;
using ui::Hdr;
using ui::RenderIntent;

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

explicit UnnecessaryLock(Mutex& mutex) ACQUIRE(mutex) {
    explicit UnnecessaryLock(Mutex& mutex) ACQUIRE(mutex) {}
    ~UnnecessaryLock() RELEASE() {}
}// TODO(b/141333600): Consolidate with HWC2::Display::Config::Builder::getDefaultDensity.
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

class FrameRateFlexibilityToken : public BBinder {
public:
    mCallback(callback){}
    ~FrameRateFlexibilityToken(){ mCallback(); }
    
private:
    std::function<void()> mCallback;
};

using ConditionalLock = ConditionalLockGuard<Mutex>;

} // namespace anonymous

// ---------------------------------------------------------------------------

const String16 sHardwareTest("android.permission.HARDWARE_TEST");
const String16 sAccessSurfaceFlinger("android.permission.ACCESS_SURFACE_FLINGER");
const String16 sReadFramebuffer("android.permission.READ_FRAME_BUFFER");
const String16 sDump("android.permission.DUMP");

const char* KERNEL_IDLE_TIMER_PROP = "graphics.display.kernel_idle_timer.enabled";

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
std::string getHwcServiceName() {
    char value[PROPERTY_VALUE_MAX] = {};
    property_get("debug.sf.hwc_service_name", value, "default");
    ALOGI("Using HWComposer service: '%s'", value);
    return std::string(value);
}

bool useTrebleTestingOverride() {
    char value[PROPERTY_VALUE_MAX] = {};
    property_get("debug.sf.treble_testing_override", value, "false");
    ALOGI("Treble testing override: '%s'", value);
    return std::string(value) == "true";
}

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

SurfaceFlingerBE::SurfaceFlingerBE() : mHwcServiceName(getHwcServiceName()) {}

SurfaceFlinger::SurfaceFlinger(Factory& factory, SkipInitializationTag)
      : mFactory(factory),
        mInterceptor(mFactory.createSurfaceInterceptor(this)),
        mTimeStats(std::make_shared<impl::TimeStats>()),
        mFrameTracer(std::make_unique<FrameTracer>()),
        mEventQueue(mFactory.createMessageQueue()),
        mCompositionEngine(mFactory.createCompositionEngine()),
        mInternalDisplayDensity(getDensityFromProperty("ro.sf.lcd_density", true)),
        mEmulatedDisplayDensity(getDensityFromProperty("qemu.sf.lcd_density", false)) {}

SurfaceFlinger::SurfaceFlinger(Factory& factory) : SurfaceFlinger(factory, SkipInitialization) {
    ALOGI("SurfaceFlinger is starting");

    hasSyncFramework {
    ALOGI("SurfaceFlinger is starting");

    hasSyncFramework = running_without_sync_framework(true);

    dispSyncPresentTimeOffset = present_time_offset_from_vsync_ns(0);

    useHwcForRgbToYuv = force_hwc_copy_for_virtual_displays(false);

    maxVirtualDisplaySize = max_virtual_display_dimension(0);

    // Vr flinger is only enabled on Daydream ready devices.
    useVrFlinger = use_vr_flinger(false);

    maxFrameBufferAcquiredBuffers = max_frame_buffer_acquired_buffers(2);

    maxGraphicsWidth = std::max(max_graphics_width(0), 0);
    maxGraphicsHeight = std::max(max_graphics_height(0), 0);

    hasWideColorDisplay = has_wide_color_display(false);

    useColorManagement = use_color_management(false);

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

    useContextPriority = use_context_priority(true);

    using Values = SurfaceFlingerProperties::primary_display_orientation_values;
    switch (primary_display_orientation(Values::ORIENTATION_0)) {
        case Values::ORIENTATION_0:
            break;
        case Values::ORIENTATION_90:
            internalDisplayOrientation = ui::ROTATION_90;
            break;
        case Values::ORIENTATION_180:
            internalDisplayOrientation = ui::ROTATION_180;
            break;
        case Values::ORIENTATION_270:
            internalDisplayOrientation = ui::ROTATION_270;
            break;
    }
    ALOGV("Internal Display Orientation: %s", toCString(internalDisplayOrientation));

    mInternalDisplayPrimaries = sysprop::getDisplayNativePrimaries();

    // debugging stuff...
    char value[PROPERTY_VALUE_MAX];

    property_get("ro.bq.gpu_to_cpu_unsupported", value, "0");
    mGpuToCpuSupported = !atoi(value);

    property_get("ro.build.type", value, "user");
    mIsUserBuild = strcmp(value, "user") == 0;

    property_get("debug.sf.showupdates", value, "0");
    mDebugRegion = atoi(value);

    ALOGI_IF(mDebugRegion, "showupdates enabled");

    // DDMS debugging deprecated (b/120782499)
    property_get("debug.sf.ddms", value, "0");
    int debugDdms = atoi(value);
    ALOGI_IF(debugDdms, "DDMS debugging not supported");

    property_get("debug.sf.disable_backpressure", value, "0");
    mPropagateBackpressure = !atoi(value);
    ALOGI_IF(!mPropagateBackpressure, "Disabling backpressure propagation");

    property_get("debug.sf.enable_gl_backpressure", value, "0");
    mPropagateBackpressureClientComposition = atoi(value);
    ALOGI_IF(mPropagateBackpressureClientComposition,
             "Enabling backpressure propagation for Client Composition");

    property_get("debug.sf.enable_hwc_vds", value, "0");
    mUseHwcVirtualDisplays = atoi(value);
    ALOGI_IF(mUseHwcVirtualDisplays, "Enabling HWC virtual displays");

    property_get("ro.sf.disable_triple_buffer", value, "0");
    mLayerTripleBufferingDisabled = atoi(value);
    ALOGI_IF(mLayerTripleBufferingDisabled, "Disabling Triple Buffering");

    property_get("ro.surface_flinger.supports_background_blur", value, "0");
    bool supportsBlurs = atoi(value);
    mSupportsBlur = supportsBlurs;
    ALOGI_IF(!mSupportsBlur, "Disabling blur effects, they are not supported.");
    property_get("ro.sf.blurs_are_expensive", value, "0");
    mBlursAreExpensive = atoi(value);

    const size_t defaultListSize = ISurfaceComposer::MAX_LAYERS;
    auto listSize = property_get_int32("debug.sf.max_igbp_list_size", int32_t(defaultListSize));
    mMaxGraphicBufferProducerListSize = (listSize > 0) ? size_t(listSize) : defaultListSize;

    property_get("debug.sf.luma_sampling", value, "1");
    mLumaSampling = atoi(value);

    property_get("debug.sf.disable_client_composition_cache", value, "0");
    mDisableClientCompositionCache = atoi(value);

    // We should be reading 'persist.sys.sf.color_saturation' here
    // but since /data may be encrypted, we need to wait until after vold
    // comes online to attempt to read the property. The property is
    // instead read after the boot animation

    if (useTrebleTestingOverride()) {
        // Without the override SurfaceFlinger cannot connect to HIDL
        // services that are not listed in the manifests.  Considered
        // deriving the setting from the set service name, but it
        // would be brittle if the name that's not 'default' is used
        // for production purposes later on.
        setenv("TREBLE_TESTING_OVERRIDE", "true", true);
    }

    useFrameRateApi = use_frame_rate_api(true);

    mKernelIdleTimerEnabled = mSupportKernelIdleTimer = sysprop::support_kernel_idle_timer(false);
    base::SetProperty(KERNEL_IDLE_TIMER_PROP, mKernelIdleTimerEnabled ? "true" : "false");
}

void SurfaceFlinger::onFirstRef() {
    mEventQueue->init(this);
}

SurfaceFlinger::~SurfaceFlinger() 

void SurfaceFlinger::binderDied(const wp<IBinder>&) {
    // the window manager died on us. prepare its eulogy.
    mBootFinished = false;

    // restore initial conditions (default device unblank, etc)
    initializeDisplays();

    // restart the boot-animation
    startBootAnim();
}

void SurfaceFlinger::run() {
    while (true) {
        mEventQueue->waitMessage();
    }
}

template <typename F, typename T>
inline std::future<T> SurfaceFlinger::schedule(F&& f) {
    auto [task, future] = makeTask(std::move(f));
    mEventQueue->postMessage(std::move(task));
    return std::move(future);
}

sp<ISurfaceComposerClient> SurfaceFlinger::createConnection() {
    const sp<Client> client = new Client(this);
    return client->initCheck() == NO_ERROR ? client : nullptr;
}

sp<IBinder> SurfaceFlinger::createDisplay(const String8& displayName, bool secure) {
    class DisplayToken : public BBinder {
        sp<SurfaceFlinger> flinger;
        virtual ~DisplayToken() {
             // no more references, this display must be terminated
             Mutex::Autolock _l(flinger->mStateLock);
             flinger->mCurrentState.displays.removeItem(this);
             flinger->setTransactionFlags(eDisplayTransactionNeeded);
         }
     public:
        explicit DisplayToken(const sp<SurfaceFlinger>& flinger)
            : flinger(flinger) {
        }
    };

    sp<BBinder> token = new DisplayToken(this);

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
        ALOGE("%s: Invalid display token %p", __FUNCTION__, displayToken.get());
        return;
    }

    const DisplayDeviceState& state = mCurrentState.displays.valueAt(index);
    if (state.physical) {
        ALOGE("%s: Invalid operation on physical display", __FUNCTION__);
        return;
    }
    mInterceptor->saveDisplayDeletion(state.sequenceId);
    mCurrentState.displays.removeItemsAt(index);
    setTransactionFlags(eDisplayTransactionNeeded);
}

std::vector<PhysicalDisplayId> SurfaceFlinger::getPhysicalDisplayIds() const {
    Mutex::Autolock lock(mStateLock);

    const auto internalDisplayId = getInternalDisplayIdLocked();
    if (!internalDisplayId) {
        return {};
    }

    std::vector<PhysicalDisplayId> displayIds;
    displayIds.reserve(mPhysicalDisplayTokens.size());
    displayIds.push_back(internalDisplayId->value);

    for (const auto& [id, token] : mPhysicalDisplayTokens) {
        if (id != *internalDisplayId) {
            displayIds.push_back(id.value);
        }
    }

    return displayIds;
}

sp<IBinder> SurfaceFlinger::getPhysicalDisplayToken(PhysicalDisplayId displayId) const {
    Mutex::Autolock lock(mStateLock);
    return getPhysicalDisplayTokenLocked(DisplayId{displayId});
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

void SurfaceFlinger::bootFinished()
{
    if (mBootFinished == true) {
        ALOGE("Extra call to bootFinished");
        return;
    }
    mBootFinished = true;
    if (mStartPropertySetThread->join() != NO_ERROR) {
        ALOGE("Join StartPropertySetThread failed!");
    }
    const nsecs_t now = systemTime();
    const nsecs_t duration = now - mBootTime;
    ALOGI("Boot is finished (%ld ms)", long(ns2ms(duration)) );

    mFrameTracer->initialize();
    mTimeStats->onBootFinished();

    // wait patiently for the window manager death
    const String16 name("window");
    mWindowManager = defaultServiceManager()->getService(name);
    if (mWindowManager != 0) {
        mWindowManager->linkToDeath(static_cast<IBinder::DeathRecipient*>(this));
    }

    if (mVrFlinger) {
      mVrFlinger->OnBootFinished();
    }

    // stop boot animation
    // formerly we would just kill the process, but we now ask it to exit so it
    // can choose where to stop the animation.
    property_set("service.bootanim.exit", "1");

    const int LOGTAG_SF_STOP_BOOTANIM = 60110;
    LOG_EVENT_LONG(LOGTAG_SF_STOP_BOOTANIM,
                   ns2ms(systemTime(SYSTEM_TIME_MONOTONIC)));

<<<<<<< HEAD
    static_cast<void>(schedule([this] {
||||||| 995b24692c
    postMessageAsync(new LambdaMessage([this]() NO_THREAD_SAFETY_ANALYSIS {
=======
    sp<IBinder> input(defaultServiceManager()->getService(String16("inputflinger")));

    postMessageAsync(new LambdaMessage([=]() NO_THREAD_SAFETY_ANALYSIS {
        if (input == nullptr) {
            ALOGE("Failed to link to input service");
        } else {
            mInputFlinger = interface_cast<IInputFlinger>(input);
        }

>>>>>>> cae2ee03
        readPersistentProperties();
        mPowerAdvisor.onBootFinished();
        mBootStage = BootStage::FINISHED;

        if (property_get_bool("sf.debug.show_refresh_rate_overlay", false)) {
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
    return schedule([this] {
               uint32_t name = 0;
               getRenderEngine().genTextures(1, &name);
               return name;
           })
            .get();
}

void SurfaceFlinger::deleteTextureAsync(uint32_t texture) {
    std::lock_guard lock(mTexturePoolMutex);
    // We don't change the pool size, so the fix-up logic in postComposition will decide whether
    // to actually delete this or not based on mTexturePoolSize
    mTexturePool.push_back(texture);
    ATRACE_INT("TexturePoolSize", mTexturePool.size());
}

// Do not call property_set on main thread which will be blocked by init
// Use StartPropertySetThread instead.
void SurfaceFlinger::init() {
    ALOGI(  "SurfaceFlinger's main thread ready to run. "
            "Initializing graphics H/W...");
    Mutex::Autolock _l(mStateLock);

    // Get a RenderEngine for the given display / config (can't fail)
    // TODO(b/77156734): We need to stop casting and use HAL types when possible.
    // Sending maxFrameBufferAcquiredBuffers as the cache size is tightly tuned to single-display.
    mCompositionEngine->setRenderEngine(renderengine::RenderEngine::create(
            renderengine::RenderEngineCreationArgs::Builder()
                .setPixelFormat(static_cast<int32_t>(defaultCompositionPixelFormat))
                .setImageCacheSize(maxFrameBufferAcquiredBuffers)
                .setUseColorManagerment(useColorManagement)
                .setEnableProtectedContext(enable_protected_contents(false))
                .setPrecacheToneMapperShaderOnly(false)
                .setSupportsBackgroundBlur(mSupportsBlur)
                .setContextPriority(useContextPriority
                        ? renderengine::RenderEngine::ContextPriority::HIGH
                        : renderengine::RenderEngine::ContextPriority::MEDIUM)
                .build()));
    mCompositionEngine->setTimeStats(mTimeStats);

    LOG_ALWAYS_FATAL_IF(mVrFlingerRequestsDisplay,
            "Starting with vr flinger active is not currently supported.");
    mCompositionEngine->setHwComposer(getFactory().createHWComposer(getBE().mHwcServiceName));
    mCompositionEngine->getHwComposer().setConfiguration(this, getBE().mComposerSequenceId);
    // Process any initial hotplug and resulting display changes.
    processDisplayHotplugEventsLocked();
    const auto display = getDefaultDisplayDeviceLocked();
    LOG_ALWAYS_FATAL_IF(!display, "Missing internal display after registering composer callback.");
    LOG_ALWAYS_FATAL_IF(!getHwComposer().isConnected(*display->getId()),
                        "Internal display is disconnected.");

    if (useVrFlinger) {
        auto vrFlingerRequestDisplayCallback = [this](bool requestDisplay) {
            // This callback is called from the vr flinger dispatch thread. We
            // need to call signalTransaction(), which requires holding
            // mStateLock when we're not on the main thread. Acquiring
            // mStateLock from the vr flinger dispatch thread might trigger a
            // deadlock in surface flinger (see b/66916578), so post a message
            // to be handled on the main thread instead.
            static_cast<void>(schedule([=] {
                ALOGI("VR request display mode: requestDisplay=%d", requestDisplay);
                mVrFlingerRequestsDisplay = requestDisplay;
                signalTransaction();
            }));
        };
        mVrFlinger = dvr::VrFlinger::Create(getHwComposer().getComposer(),
                                            getHwComposer()
                                                    .fromPhysicalDisplayId(*display->getId())
                                                    .value_or(0),
                                            vrFlingerRequestDisplayCallback);
        if (!mVrFlinger) {
            ALOGE("Failed to start vrflinger");
        }
    }

    // initialize our drawing state
    mDrawingState = mCurrentState;

    // set initial conditions (e.g. unblank default device)
    initializeDisplays();

    char primeShaderCache[PROPERTY_VALUE_MAX];
    property_get("service.sf.prime_shader_cache", primeShaderCache, "1");
    if (atoi(primeShaderCache)) {
        getRenderEngine().primeCache();
    }

    // Inform native graphics APIs whether the present timestamp is supported:

    const bool presentFenceReliable =
            !getHwComposer().hasCapability(hal::Capability::PRESENT_FENCE_IS_NOT_RELIABLE);
    mStartPropertySetThread = getFactory().createStartPropertySetThread(presentFenceReliable);

    if (mStartPropertySetThread->Start() != NO_ERROR) {
        ALOGE("Run StartPropertySetThread failed!");
    }

    ALOGV("Done initializing");
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

    property_get("persist.sys.sf.disable_blurs", value, "0");
    bool disableBlurs = atoi(value);
    mDisableBlurs = disableBlurs;
    ALOGI_IF(disableBlurs, "Disabling blur effects, user preference.");
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

size_t SurfaceFlinger::getMaxTextureSize() const {
    return getRenderEngine().getMaxTextureSize();
}

size_t SurfaceFlinger::getMaxViewportDims() const {
    return getRenderEngine().getMaxViewportDims();
}

// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
bool SurfaceFlinger::authenticateSurfaceTexture(
        const sp<IGraphicBufferProducer>& bufferProducer) const {
    Mutex::Autolock _l(mStateLock);
    return authenticateSurfaceTextureLocked(bufferProducer);
}

bool SurfaceFlinger::authenticateSurfaceTextureLocked(
        const sp<IGraphicBufferProducer>& bufferProducer) const {
    sp<IBinder> surfaceTextureBinder(IInterface::asBinder(bufferProducer));
    return mGraphicBufferProducerList.count(surfaceTextureBinder.get()) > 0;
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

    const Rect viewport = display->getViewport();
    state->viewport = viewport.isValid() ? viewport.getSize() : display->getSize();

    return NO_ERROR;
}

status_t SurfaceFlinger::getDisplayInfo(const sp<IBinder>& displayToken, DisplayInfo* info) {
    if (!displayToken || !info) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);

    const auto display = getDisplayDeviceLocked(displayToken);
    if (!display) {
        return NAME_NOT_FOUND;
    }

    if (const auto connectionType = display->getConnectionType())
        info->connectionType = *connectionType;
    else {
        return INVALID_OPERATION;
    }

    if (mEmulatedDisplayDensity) {
        info->density = mEmulatedDisplayDensity;
    } else {
        info->density = info->connectionType == DisplayConnectionType::Internal
                ? mInternalDisplayDensity
                : FALLBACK_DENSITY;
    }
    info->density /= ACONFIGURATION_DENSITY_MEDIUM;

    info->secure = display->isSecure();
    info->deviceProductInfo = getDeviceProductInfoLocked(*display);

    return NO_ERROR;
}

status_t SurfaceFlinger::getDisplayConfigs(const sp<IBinder>& displayToken,
                                           Vector<DisplayConfig>* configs) {
    if (!displayToken || !configs) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);

    const auto displayId = getPhysicalDisplayIdLocked(displayToken);
    if (!displayId) {
        return NAME_NOT_FOUND;
    }

    const bool isInternal = (displayId == getInternalDisplayIdLocked());

    configs->clear();

    for (const auto& hwConfig : getHwComposer().getConfigs(*displayId)) {
        DisplayConfig config;

        auto width = hwConfig->getWidth();
        auto height = hwConfig->getHeight();

        auto xDpi = hwConfig->getDpiX();
        auto yDpi = hwConfig->getDpiY();

        if (isInternal &&
            (internalDisplayOrientation == ui::ROTATION_90 ||
             internalDisplayOrientation == ui::ROTATION_270)) {
            std::swap(width, height);
            std::swap(xDpi, yDpi);
        }

        config.resolution = ui::Size(width, height);

        if (mEmulatedDisplayDensity) {
            config.xDpi = mEmulatedDisplayDensity;
            config.yDpi = mEmulatedDisplayDensity;
        } else {
            config.xDpi = xDpi;
            config.yDpi = yDpi;
        }

        const nsecs_t period = hwConfig->getVsyncPeriod();
        config.refreshRate = 1e9f / period;

        const auto offsets = mPhaseConfiguration->getOffsetsForRefreshRate(config.refreshRate);
        config.appVsyncOffset = offsets.late.app;
        config.sfVsyncOffset = offsets.late.sf;
        config.configGroup = hwConfig->getConfigGroup();

        // This is how far in advance a buffer must be queued for
        // presentation at a given time.  If you want a buffer to appear
        // on the screen at time N, you must submit the buffer before
        // (N - presentationDeadline).
        //
        // Normally it's one full refresh period (to give SF a chance to
        // latch the buffer), but this can be reduced by configuring a
        // DispSync offset.  Any additional delays introduced by the hardware
        // composer or panel must be accounted for here.
        //
        // We add an additional 1ms to allow for processing time and
        // differences between the ideal and actual refresh rate.
        config.presentationDeadline = period - config.sfVsyncOffset + 1000000;

        configs->push_back(config);
    }

    return NO_ERROR;
}

status_t SurfaceFlinger::getDisplayStats(const sp<IBinder>&, DisplayStatInfo* stats) {
    if (!stats) {
        return BAD_VALUE;
    }

    mScheduler->getDisplayStatInfo(stats);
    return NO_ERROR;
}

int SurfaceFlinger::getActiveConfig(const sp<IBinder>& displayToken) {
    int activeConfig;
    bool isPrimary;

    {
        Mutex::Autolock lock(mStateLock);

        if (const auto display = getDisplayDeviceLocked(displayToken)) {
            activeConfig = display->getActiveConfig().value();
            isPrimary = display->isPrimary();
        } else {
            ALOGE("%s: Invalid display token %p", __FUNCTION__, displayToken.get());
            return NAME_NOT_FOUND;
        }
    }

    if (isPrimary) {
        if (const auto config = getDesiredActiveConfig()) {
            return config->configId.value();
        }
    }

    return activeConfig;
}

void SurfaceFlinger::setDesiredActiveConfig(const ActiveConfigInfo& info) {
    ATRACE_CALL();
    auto& refreshRate = mRefreshRateConfigs->getRefreshRateFromConfigId(info.configId);
    ALOGV("setDesiredActiveConfig(%s)", refreshRate.getName().c_str());

    std::lock_guard<std::mutex> lock(mActiveConfigLock);
    if (mDesiredActiveConfigChanged) {
        // If a config change is pending, just cache the latest request in
        // mDesiredActiveConfig
        const Scheduler::ConfigEvent prevConfig = mDesiredActiveConfig.event;
        mDesiredActiveConfig = info;
        mDesiredActiveConfig.event = mDesiredActiveConfig.event | prevConfig;
    } else {
        // Check is we are already at the desired config
        const auto display = getDefaultDisplayDeviceLocked();
        if (!display || display->getActiveConfig() == refreshRate.getConfigId()) {
            return;
        }

        // Initiate a config change.
        mDesiredActiveConfigChanged = true;
        mDesiredActiveConfig = info;

        // This will trigger HWC refresh without resetting the idle timer.
        repaintEverythingForHWC();
        // Start receiving vsync samples now, so that we can detect a period
        // switch.
        mScheduler->resyncToHardwareVsync(true, refreshRate.getVsyncPeriod());
        // As we called to set period, we will call to onRefreshRateChangeCompleted once
        // DispSync model is locked.
        mVSyncModulator->onRefreshRateChangeInitiated();

        mPhaseConfiguration->setRefreshRateFps(refreshRate.getFps());
        mVSyncModulator->setPhaseOffsets(mPhaseConfiguration->getCurrentOffsets());
        mScheduler->setConfigChangePending(true);
    }

    if (mRefreshRateOverlay) {
        mRefreshRateOverlay->changeRefreshRate(refreshRate);
    }
}

status_t SurfaceFlinger::setActiveConfig(const sp<IBinder>& displayToken, int mode) {
    ATRACE_CALL();

    if (!displayToken) {
        return BAD_VALUE;
    }

    auto future = schedule([=]() -> status_t {
        const auto display = ON_MAIN_THREAD(getDisplayDeviceLocked(displayToken));
        if (!display) {
            ALOGE("Attempt to set allowed display configs for invalid display token %p",
                  displayToken.get());
            return NAME_NOT_FOUND;
        } else if (display->isVirtual()) {
            ALOGW("Attempt to set allowed display configs for virtual display");
            return INVALID_OPERATION;
        } else {
            const HwcConfigIndexType config(mode);
            const float fps = mRefreshRateConfigs->getRefreshRateFromConfigId(config).getFps();
            const scheduler::RefreshRateConfigs::Policy policy{config, {fps, fps}};
            constexpr bool kOverridePolicy = false;

            return setDesiredDisplayConfigSpecsInternal(display, policy, kOverridePolicy);
        }
    });

    return future.get();
}

void SurfaceFlinger::setActiveConfigInternal() {
    ATRACE_CALL();

    const auto display = getDefaultDisplayDeviceLocked();
    if (!display) {
        return;
    }

    auto& oldRefreshRate =
            mRefreshRateConfigs->getRefreshRateFromConfigId(display->getActiveConfig());

    std::lock_guard<std::mutex> lock(mActiveConfigLock);
    mRefreshRateConfigs->setCurrentConfigId(mUpcomingActiveConfig.configId);
    mRefreshRateStats->setConfigMode(mUpcomingActiveConfig.configId);
    display->setActiveConfig(mUpcomingActiveConfig.configId);

    auto& refreshRate =
            mRefreshRateConfigs->getRefreshRateFromConfigId(mUpcomingActiveConfig.configId);
    if (refreshRate.getVsyncPeriod() != oldRefreshRate.getVsyncPeriod()) {
        mTimeStats->incrementRefreshRateSwitches();
    }
    mPhaseConfiguration->setRefreshRateFps(refreshRate.getFps());
    mVSyncModulator->setPhaseOffsets(mPhaseConfiguration->getCurrentOffsets());
    ATRACE_INT("ActiveConfigFPS", refreshRate.getFps());

    if (mUpcomingActiveConfig.event != Scheduler::ConfigEvent::None) {
        const nsecs_t vsyncPeriod =
                mRefreshRateConfigs->getRefreshRateFromConfigId(mUpcomingActiveConfig.configId)
                        .getVsyncPeriod();
        mScheduler->onPrimaryDisplayConfigChanged(mAppConnectionHandle, display->getId()->value,
                                                  mUpcomingActiveConfig.configId, vsyncPeriod);
    }
}

void SurfaceFlinger::desiredActiveConfigChangeDone() {
    std::lock_guard<std::mutex> lock(mActiveConfigLock);
    mDesiredActiveConfig.event = Scheduler::ConfigEvent::None;
    mDesiredActiveConfigChanged = false;

    const auto& refreshRate =
            mRefreshRateConfigs->getRefreshRateFromConfigId(mDesiredActiveConfig.configId);
    mScheduler->resyncToHardwareVsync(true, refreshRate.getVsyncPeriod());
    mPhaseConfiguration->setRefreshRateFps(refreshRate.getFps());
    mVSyncModulator->setPhaseOffsets(mPhaseConfiguration->getCurrentOffsets());
    mScheduler->setConfigChangePending(false);
}

void SurfaceFlinger::performSetActiveConfig() {
    ATRACE_CALL();
    ALOGV("performSetActiveConfig");
    // Store the local variable to release the lock.
    const auto desiredActiveConfig = getDesiredActiveConfig();
    if (!desiredActiveConfig) {
        // No desired active config pending to be applied
        return;
    }

    auto& refreshRate =
            mRefreshRateConfigs->getRefreshRateFromConfigId(desiredActiveConfig->configId);
    ALOGV("performSetActiveConfig changing active config to %d(%s)",
          refreshRate.getConfigId().value(), refreshRate.getName().c_str());
    const auto display = getDefaultDisplayDeviceLocked();
    if (!display || display->getActiveConfig() == desiredActiveConfig->configId) {
        // display is not valid or we are already in the requested mode
        // on both cases there is nothing left to do
        desiredActiveConfigChangeDone();
        return;
    }

    // Desired active config was set, it is different than the config currently in use, however
    // allowed configs might have change by the time we process the refresh.
    // Make sure the desired config is still allowed
    if (!isDisplayConfigAllowed(desiredActiveConfig->configId)) {
        desiredActiveConfigChangeDone();
        return;
    }

    mUpcomingActiveConfig = *desiredActiveConfig;
    const auto displayId = display->getId();
    LOG_ALWAYS_FATAL_IF(!displayId);

    ATRACE_INT("ActiveConfigFPS_HWC", refreshRate.getFps());

    // TODO(b/142753666) use constrains
    hal::VsyncPeriodChangeConstraints constraints;
    constraints.desiredTimeNanos = systemTime();
    constraints.seamlessRequired = false;

    hal::VsyncPeriodChangeTimeline outTimeline;
    auto status =
            getHwComposer().setActiveConfigWithConstraints(*displayId,
                                                           mUpcomingActiveConfig.configId.value(),
                                                           constraints, &outTimeline);
    if (status != NO_ERROR) {
        // setActiveConfigWithConstraints may fail if a hotplug event is just about
        // to be sent. We just log the error in this case.
        ALOGW("setActiveConfigWithConstraints failed: %d", status);
        return;
    }

    mScheduler->onNewVsyncPeriodChangeTimeline(outTimeline);
    // Scheduler will submit an empty frame to HWC if needed.
    mSetActiveConfigPending = true;
}

status_t SurfaceFlinger::getDisplayColorModes(const sp<IBinder>& displayToken,
                                              Vector<ColorMode>* outColorModes) {
    if (!displayToken || !outColorModes) {
        return BAD_VALUE;
    }

    std::vector<ColorMode> modes;
    bool isInternalDisplay = false;
    {
        ConditionalLock lock(mStateLock, std::this_thread::get_id() != mMainThreadId);

        const auto displayId = getPhysicalDisplayIdLocked(displayToken);
        if (!displayId) {
            return NAME_NOT_FOUND;
        }

        modes = getHwComposer().getColorModes(*displayId);
        isInternalDisplay = displayId == getInternalDisplayIdLocked();
    }
    outColorModes->clear();

    // If it's built-in display and the configuration claims it's not wide color capable,
    // filter out all wide color modes. The typical reason why this happens is that the
    // hardware is not good enough to support GPU composition of wide color, and thus the
    // OEMs choose to disable this capability.
    if (isInternalDisplay && !hasWideColorDisplay) {
        std::remove_copy_if(modes.cbegin(), modes.cend(), std::back_inserter(*outColorModes),
                            isWideColorMode);
    } else {
        std::copy(modes.cbegin(), modes.cend(), std::back_inserter(*outColorModes));
    }

    return NO_ERROR;
}

status_t SurfaceFlinger::getDisplayNativePrimaries(const sp<IBinder>& displayToken,
                                                   ui::DisplayPrimaries &primaries) {
    if (!displayToken) {
        return BAD_VALUE;
    }

    // Currently we only support this API for a single internal display.
    if (getInternalDisplayToken() != displayToken) {
        return NAME_NOT_FOUND;
    }

    memcpy(&primaries, &mInternalDisplayPrimaries, sizeof(ui::DisplayPrimaries));
    return NO_ERROR;
}

ColorMode SurfaceFlinger::getActiveColorMode(const sp<IBinder>& displayToken) {
    Mutex::Autolock lock(mStateLock);

    if (const auto display = getDisplayDeviceLocked(displayToken)) {
        return display->getCompositionDisplay()->getState().colorMode;
    }
    return static_cast<ColorMode>(BAD_VALUE);
}

status_t SurfaceFlinger::setActiveColorMode(const sp<IBinder>& displayToken, ColorMode mode) {
    schedule([=]() MAIN_THREAD {
        Vector<ColorMode> modes;
        getDisplayColorModes(displayToken, &modes);
        bool exists = std::find(std::begin(modes), std::end(modes), mode) != std::end(modes);
        if (mode < ColorMode::NATIVE || !exists) {
            ALOGE("Attempt to set invalid active color mode %s (%d) for display token %p",
                  decodeColorMode(mode).c_str(), mode, displayToken.get());
            return;
        }
        const auto display = getDisplayDeviceLocked(displayToken);
        if (!display) {
            ALOGE("Attempt to set active color mode %s (%d) for invalid display token %p",
                  decodeColorMode(mode).c_str(), mode, displayToken.get());
        } else if (display->isVirtual()) {
            ALOGW("Attempt to set active color mode %s (%d) for virtual display",
                  decodeColorMode(mode).c_str(), mode);
        } else {
            display->getCompositionDisplay()->setColorProfile(
                    compositionengine::Output::ColorProfile{mode, Dataspace::UNKNOWN,
                                                            RenderIntent::COLORIMETRIC,
                                                            Dataspace::UNKNOWN});
        }
    }).wait();

    return NO_ERROR;
}

status_t SurfaceFlinger::getAutoLowLatencyModeSupport(const sp<IBinder>& displayToken, bool* outSupport) const {
    if (!displayToken) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);

    const auto displayId = getPhysicalDisplayIdLocked(displayToken);
    if (!displayId) {
        return NAME_NOT_FOUND;
    }
    *outSupport =
            getHwComposer().hasDisplayCapability(*displayId,
                                                 hal::DisplayCapability::AUTO_LOW_LATENCY_MODE);
    return NO_ERROR;
}

void SurfaceFlinger::setAutoLowLatencyMode(const sp<IBinder>& displayToken, bool on) {
    static_cast<void>(schedule([=]() MAIN_THREAD {
        if (const auto displayId = getPhysicalDisplayIdLocked(displayToken)) {
            getHwComposer().setAutoLowLatencyMode(*displayId, on);
        } else {
            ALOGE("%s: Invalid display token %p", __FUNCTION__, displayToken.get());
        }
    }));
}

status_t SurfaceFlinger::getGameContentTypeSupport(const sp<IBinder>& displayToken, bool* outSupport) const {
    if (!displayToken) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);

    const auto displayId = getPhysicalDisplayIdLocked(displayToken);
    if (!displayId) {
        return NAME_NOT_FOUND;
    }

    std::vector<hal::ContentType> types;
    getHwComposer().getSupportedContentTypes(*displayId, &types);

    *outSupport = std::any_of(types.begin(), types.end(),
                              [](auto type) { return type == hal::ContentType::GAME; });
    return NO_ERROR;
}

void SurfaceFlinger::setGameContentType(const sp<IBinder>& displayToken, bool on) {
    static_cast<void>(schedule([=]() MAIN_THREAD {
        if (const auto displayId = getPhysicalDisplayIdLocked(displayToken)) {
            const auto type = on ? hal::ContentType::GAME : hal::ContentType::NONE;
            getHwComposer().setContentType(*displayId, type);
        } else {
            ALOGE("%s: Invalid display token %p", __FUNCTION__, displayToken.get());
        }
    }));
}

status_t SurfaceFlinger::clearAnimationFrameStats() {
    Mutex::Autolock _l(mStateLock);
    mAnimFrameTracker.clearStats();
    return NO_ERROR;
}

status_t SurfaceFlinger::getAnimationFrameStats(FrameStats* outStats) const {
    Mutex::Autolock _l(mStateLock);
    mAnimFrameTracker.getStats(outStats);
    return NO_ERROR;
}

status_t SurfaceFlinger::getHdrCapabilities(const sp<IBinder>& displayToken,
                                            HdrCapabilities* outCapabilities) const {
    Mutex::Autolock lock(mStateLock);

    const auto display = getDisplayDeviceLocked(displayToken);
    if (!display) {
        ALOGE("%s: Invalid display token %p", __FUNCTION__, displayToken.get());
        return NAME_NOT_FOUND;
    }

    // At this point the DisplayDevice should already be set up,
    // meaning the luminance information is already queried from
    // hardware composer and stored properly.
    const HdrCapabilities& capabilities = display->getHdrCapabilities();
    *outCapabilities = HdrCapabilities(capabilities.getSupportedHdrTypes(),
                                       capabilities.getDesiredMaxLuminance(),
                                       capabilities.getDesiredMaxAverageLuminance(),
                                       capabilities.getDesiredMinLuminance());

    return NO_ERROR;
}

std::optional<DeviceProductInfo> SurfaceFlinger::getDeviceProductInfoLocked(const DisplayDevice& display) const {
    // TODO(b/149075047): Populate DeviceProductInfo on hotplug and store it in DisplayDevice to
    // avoid repetitive HAL IPC and EDID parsing.
    const auto displayId = display.getId();
    LOG_FATAL_IF(!displayId);

    const auto hwcDisplayId = getHwComposer().fromPhysicalDisplayId(*displayId);
    LOG_FATAL_IF(!hwcDisplayId);

    uint8_t port;
    DisplayIdentificationData data;
    if (!getHwComposer().getDisplayIdentificationData(*hwcDisplayId, &port, &data)) {
        ALOGV("%s: No identification data.", __FUNCTION__);
        return {};
    }

    const auto info = parseDisplayIdentificationData(port, data);
    if (!info) {
        return {};
    }
    return info->deviceProductInfo;
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

status_t SurfaceFlinger::setDisplayContentSamplingEnabled(const sp<IBinder>& displayToken, bool enable, uint8_t componentMask, uint64_t maxFrames) {
    return schedule([=]() MAIN_THREAD -> status_t {
               if (const auto displayId = getPhysicalDisplayIdLocked(displayToken)) {
                   return getHwComposer().setDisplayContentSamplingEnabled(*displayId, enable,
                                                                           componentMask,
                                                                           maxFrames);
               } else {
                   ALOGE("%s: Invalid display token %p", __FUNCTION__, displayToken.get());
                   return NAME_NOT_FOUND;
               }
           })
            .get();
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
    schedule([=] {
        Mutex::Autolock lock(mStateLock);

        if (const auto handle = mScheduler->enableVSyncInjection(enable)) {
            mEventQueue->setEventConnection(
                    mScheduler->getEventConnection(enable ? handle : mSfConnectionHandle));
        }
    }).wait();

    return NO_ERROR;
}

status_t SurfaceFlinger::injectVSync(nsecs_t when) {
    Mutex::Autolock lock(mStateLock);
    return mScheduler->injectVSync(when, calculateExpectedPresentTime(when)) ? NO_ERROR : BAD_VALUE;
}

status_t SurfaceFlinger::getLayerDebugInfo(std::vector<LayerDebugInfo>* outLayers) {
    outLayers->clear();
    schedule([=] {
        const auto display = ON_MAIN_THREAD(getDefaultDisplayDeviceLocked());
        mDrawingState.traverseInZOrder([&](Layer* layer) {
            outLayers->push_back(layer->getLayerDebugInfo(display.get()));
        });
    }).wait();
    return NO_ERROR;
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

    /*
     * Perform our own transaction if needed
     */
    
void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

// ----------------------------------------------------------------------------

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

// ----------------------------------------------------------------------------

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

// ---------------------------------------------------------------------------

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

// ---------------------------------------------------------------------------

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
}

void SurfaceFlinger::toggleKernelIdleTimer() {
    using KernelIdleTimerAction = scheduler::RefreshRateConfigs::KernelIdleTimerAction;

    // If the support for kernel idle timer is disabled in SF code, don't do anything.
    if (!mSupportKernelIdleTimer) {
        return;
    }
    const KernelIdleTimerAction action = mRefreshRateConfigs->getIdleTimerAction();

    switch (action) {
        case KernelIdleTimerAction::TurnOff:
            if (mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 0);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "false");
                mKernelIdleTimerEnabled = false;
            }
            break;
        case KernelIdleTimerAction::TurnOn:
            if (!mKernelIdleTimerEnabled) {
                ATRACE_INT("KernelIdleTimer", 1);
                base::SetProperty(KERNEL_IDLE_TIMER_PROP, "true");
                mKernelIdleTimerEnabled = true;
            }
            break;
        case KernelIdleTimerAction::NoChange:
            break;
    }
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

status_t SurfaceFlinger::captureScreen(const sp<IBinder>& displayToken,
                                       sp<GraphicBuffer>* outBuffer, bool& outCapturedSecureLayers,
                                       Dataspace reqDataspace, ui::PixelFormat reqPixelFormat,
                                       const Rect& sourceCrop, uint32_t reqWidth,
                                       uint32_t reqHeight, bool useIdentityTransform,
                                       ui::Rotation rotation, bool captureSecureLayers) {
    ATRACE_CALL();

    if (!displayToken) return BAD_VALUE;

    auto renderAreaRotation = ui::Transform::toRotationFlags(rotation);
    if (renderAreaRotation == ui::Transform::ROT_INVALID) {
        ALOGE("%s: Invalid rotation: %s", __FUNCTION__, toCString(rotation));
        renderAreaRotation = ui::Transform::ROT_0;
    }

    sp<DisplayDevice> display;
    {
        Mutex::Autolock lock(mStateLock);

        display = getDisplayDeviceLocked(displayToken);
        if (!display) return NAME_NOT_FOUND;

        // set the requested width/height to the logical display viewport size
        // by default
        if (reqWidth == 0 || reqHeight == 0) {
            reqWidth = uint32_t(display->getViewport().width());
            reqHeight = uint32_t(display->getViewport().height());
        }
    }

    DisplayRenderArea renderArea(display, sourceCrop, reqWidth, reqHeight, reqDataspace,
                                 renderAreaRotation, captureSecureLayers);
    auto traverseLayers = std::bind(&SurfaceFlinger::traverseLayersInDisplay, this, display,
                                    std::placeholders::_1);
    return captureScreenCommon(renderArea, traverseLayers, outBuffer, reqPixelFormat,
                               useIdentityTransform, outCapturedSecureLayers);
}

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

sp<DisplayDevice> SurfaceFlinger::getDisplayByIdOrLayerStack(uint64_t displayOrLayerStack) {
    const sp<IBinder> displayToken = getPhysicalDisplayTokenLocked(DisplayId{displayOrLayerStack});
    if (displayToken) {
        return getDisplayDeviceLocked(displayToken);
    }
    // Couldn't find display by displayId. Try to get display by layerStack since virtual displays
    // may not have a displayId.
    return getDisplayByLayerStack(displayOrLayerStack);
}

sp<DisplayDevice> SurfaceFlinger::getDisplayByLayerStack(uint64_t layerStack) {
    for (const auto& [token, display] : mDisplays) {
        if (display->getLayerStack() == layerStack) {
            return display;
        }
    }
    return nullptr;
}

status_t SurfaceFlinger::captureScreen(uint64_t displayOrLayerStack, Dataspace* outDataspace,
                                       sp<GraphicBuffer>* outBuffer) {
    sp<DisplayDevice> display;
    uint32_t width;
    uint32_t height;
    ui::Transform::RotationFlags captureOrientation;
    {
        Mutex::Autolock lock(mStateLock);
        display = getDisplayByIdOrLayerStack(displayOrLayerStack);
        if (!display) {
            return NAME_NOT_FOUND;
        }

        width = uint32_t(display->getViewport().width());
        height = uint32_t(display->getViewport().height());

        const auto orientation = display->getOrientation();
        captureOrientation = ui::Transform::toRotationFlags(orientation);

        switch (captureOrientation) {
            case ui::Transform::ROT_90:
                captureOrientation = ui::Transform::ROT_270;
                break;

            case ui::Transform::ROT_270:
                captureOrientation = ui::Transform::ROT_90;
                break;

            case ui::Transform::ROT_INVALID:
                ALOGE("%s: Invalid orientation: %s", __FUNCTION__, toCString(orientation));
                captureOrientation = ui::Transform::ROT_0;
                break;

            default:
                break;
        }
        *outDataspace =
                pickDataspaceFromColorMode(display->getCompositionDisplay()->getState().colorMode);
    }

    DisplayRenderArea renderArea(display, Rect(), width, height, *outDataspace, captureOrientation,
                                 false /* captureSecureLayers */);

    auto traverseLayers = std::bind(&SurfaceFlinger::traverseLayersInDisplay, this, display,
                                    std::placeholders::_1);
    bool ignored = false;
    return captureScreenCommon(renderArea, traverseLayers, outBuffer, ui::PixelFormat::RGBA_8888,
                               false /* useIdentityTransform */,
                               ignored /* outCapturedSecureLayers */);
}

status_t SurfaceFlinger::captureLayers(
        const sp<IBinder>& layerHandleBinder, sp<GraphicBuffer>* outBuffer,
        const Dataspace reqDataspace, const ui::PixelFormat reqPixelFormat, const Rect& sourceCrop,
        const std::unordered_set<sp<IBinder>, ISurfaceComposer::SpHash<IBinder>>& excludeHandles,
        float frameScale, bool childrenOnly) {
    ATRACE_CALL();

    class LayerRenderArea : public RenderArea {
    public:
        LayerRenderArea(SurfaceFlinger* flinger, const sp<Layer>& layer, const Rect crop,
                        int32_t reqWidth, int32_t reqHeight, Dataspace reqDataSpace,
                        bool childrenOnly, const Rect& displayViewport)
              : RenderArea(reqWidth, reqHeight, CaptureFill::CLEAR, reqDataSpace, displayViewport),
                mLayer(layer),
                mCrop(crop),
                mNeedsFiltering(false),
                mFlinger(flinger),
                mChildrenOnly(childrenOnly) {}
        const ui::Transform& getTransform() const override { return mTransform; }
        Rect getBounds() const override { return mLayer->getBufferSize(mLayer->getDrawingState()); }
        int getHeight() const override {
            return mLayer->getBufferSize(mLayer->getDrawingState()).getHeight();
        }
        int getWidth() const override {
            return mLayer->getBufferSize(mLayer->getDrawingState()).getWidth();
        }
        bool isSecure() const override { return false; }
        bool needsFiltering() const override { return mNeedsFiltering; }
        sp<const DisplayDevice> getDisplayDevice() const override { return nullptr; }
        Rect getSourceCrop() const override {
            if (mCrop.isEmpty()) {
                return getBounds();
            } else {
                return mCrop;
            }
        }
        class ReparentForDrawing {
        public:
            const sp<Layer>& oldParent;
            const sp<Layer>& newParent;

            ReparentForDrawing(const sp<Layer>& oldParent, const sp<Layer>& newParent,
                               const Rect& drawingBounds)
                  : oldParent(oldParent), newParent(newParent) {
                // Compute and cache the bounds for the new parent layer.
                newParent->computeBounds(drawingBounds.toFloatRect(), ui::Transform(),
                                         0.f /* shadowRadius */);
                oldParent->setChildrenDrawingParent(newParent);
            }
            ~ReparentForDrawing() { oldParent->setChildrenDrawingParent(oldParent); }
        };

        void render(std::function<void()> drawLayers) override {
            const Rect sourceCrop = getSourceCrop();
            // no need to check rotation because there is none
            mNeedsFiltering = sourceCrop.width() != getReqWidth() ||
                sourceCrop.height() != getReqHeight();

            if (!mChildrenOnly) {
                mTransform = mLayer->getTransform().inverse();
                drawLayers();
            } else {
                uint32_t w = static_cast<uint32_t>(getWidth());
                uint32_t h = static_cast<uint32_t>(getHeight());
                // In the "childrenOnly" case we reparent the children to a screenshot
                // layer which has no properties set and which does not draw.
                sp<ContainerLayer> screenshotParentLayer =
                        mFlinger->getFactory().createContainerLayer({mFlinger, nullptr,
                                                                     "Screenshot Parent"s, w, h, 0,
                                                                     LayerMetadata()});

                ReparentForDrawing reparent(mLayer, screenshotParentLayer, sourceCrop);
                drawLayers();
            }
        }

    private:
        const sp<Layer> mLayer;
        const Rect mCrop;

        ui::Transform mTransform;
        bool mNeedsFiltering;

        SurfaceFlinger* mFlinger;
        const bool mChildrenOnly;
    };

    int reqWidth = 0;
    int reqHeight = 0;
    sp<Layer> parent;
    Rect crop(sourceCrop);
    std::unordered_set<sp<Layer>, ISurfaceComposer::SpHash<Layer>> excludeLayers;
    Rect displayViewport;
    {
        Mutex::Autolock lock(mStateLock);

        parent = fromHandleLocked(layerHandleBinder).promote();
        if (parent == nullptr || parent->isRemovedFromCurrentState()) {
            ALOGE("captureLayers called with an invalid or removed parent");
            return NAME_NOT_FOUND;
        }

        const int uid = IPCThreadState::self()->getCallingUid();
        const bool forSystem = uid == AID_GRAPHICS || uid == AID_SYSTEM;
        if (!forSystem && parent->getCurrentState().flags & layer_state_t::eLayerSecure) {
            ALOGW("Attempting to capture secure layer: PERMISSION_DENIED");
            return PERMISSION_DENIED;
        }

        Rect parentSourceBounds = parent->getCroppedBufferSize(parent->getCurrentState());
        if (sourceCrop.width() <= 0) {
            crop.left = 0;
            crop.right = parentSourceBounds.getWidth();
        }

        if (sourceCrop.height() <= 0) {
            crop.top = 0;
            crop.bottom = parentSourceBounds.getHeight();
        }

        if (crop.isEmpty() || frameScale <= 0.0f) {
            // Error out if the layer has no source bounds (i.e. they are boundless) and a source
            // crop was not specified, or an invalid frame scale was provided.
            return BAD_VALUE;
        }
        reqWidth = crop.width() * frameScale;
        reqHeight = crop.height() * frameScale;

        for (const auto& handle : excludeHandles) {
            sp<Layer> excludeLayer = fromHandleLocked(handle).promote();
            if (excludeLayer != nullptr) {
                excludeLayers.emplace(excludeLayer);
            } else {
                ALOGW("Invalid layer handle passed as excludeLayer to captureLayers");
                return NAME_NOT_FOUND;
            }
        }

        const auto display = getDisplayByLayerStack(parent->getLayerStack());
        if (!display) {
            return NAME_NOT_FOUND;
        }

        displayViewport = display->getViewport();
    } // mStateLock

    // really small crop or frameScale
    if (reqWidth <= 0) {
        reqWidth = 1;
    }
    if (reqHeight <= 0) {
        reqHeight = 1;
    }

    LayerRenderArea renderArea(this, parent, crop, reqWidth, reqHeight, reqDataspace, childrenOnly,
                               displayViewport);
    auto traverseLayers = [parent, childrenOnly,
                           &excludeLayers](const LayerVector::Visitor& visitor) {
        parent->traverseChildrenInZOrder(LayerVector::StateSet::Drawing, [&](Layer* layer) {
            if (!layer->isVisible()) {
                return;
            } else if (childrenOnly && layer == parent.get()) {
                return;
            }

            sp<Layer> p = layer;
            while (p != nullptr) {
                if (excludeLayers.count(p) != 0) {
                    return;
                }
                p = p->getParent();
            }

            visitor(layer);
        });
    };

    bool outCapturedSecureLayers = false;
    return captureScreenCommon(renderArea, traverseLayers, outBuffer, reqPixelFormat, false,
                               outCapturedSecureLayers);
}

status_t SurfaceFlinger::captureScreenCommon(RenderArea& renderArea,
                                             TraverseLayersFunction traverseLayers,
                                             sp<GraphicBuffer>* outBuffer,
                                             const ui::PixelFormat reqPixelFormat,
                                             bool useIdentityTransform,
                                             bool& outCapturedSecureLayers) {
    ATRACE_CALL();

    // TODO(b/116112787) Make buffer usage a parameter.
    const uint32_t usage = GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN |
            GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE;
    *outBuffer =
            getFactory().createGraphicBuffer(renderArea.getReqWidth(), renderArea.getReqHeight(),
                                             static_cast<android_pixel_format>(reqPixelFormat), 1,
                                             usage, "screenshot");

    return captureScreenCommon(renderArea, traverseLayers, *outBuffer, useIdentityTransform,
                               false /* regionSampling */, outCapturedSecureLayers);
}

status_t SurfaceFlinger::captureScreenCommon(RenderArea& renderArea,
                                             TraverseLayersFunction traverseLayers,
                                             const sp<GraphicBuffer>& buffer,
                                             bool useIdentityTransform, bool regionSampling,
                                             bool& outCapturedSecureLayers) {
    const int uid = IPCThreadState::self()->getCallingUid();
    const bool forSystem = uid == AID_GRAPHICS || uid == AID_SYSTEM;

    status_t result;
    int syncFd;

    do {
        std::tie(result, syncFd) =
                schedule([&] {
                    if (mRefreshPending) {
                        ATRACE_NAME("Skipping screenshot for now");
                        return std::make_pair(EAGAIN, -1);
                    }

                    status_t result = NO_ERROR;
                    int fd = -1;

                    Mutex::Autolock lock(mStateLock);
                    renderArea.render([&] {
                        result = captureScreenImplLocked(renderArea, traverseLayers, buffer.get(),
                                                         useIdentityTransform, forSystem, &fd,
                                                         regionSampling, outCapturedSecureLayers);
                    });

                    return std::make_pair(result, fd);
                }).get();
    } while (result == EAGAIN);

    if (result == NO_ERROR) {
        sync_wait(syncFd, -1);
        close(syncFd);
    }

    return result;
}

void SurfaceFlinger::renderScreenImplLocked(const RenderArea& renderArea,
                                            TraverseLayersFunction traverseLayers,
                                            ANativeWindowBuffer* buffer, bool useIdentityTransform,
                                            bool regionSampling, int* outSyncFd) {
    ATRACE_CALL();

    const auto reqWidth = renderArea.getReqWidth();
    const auto reqHeight = renderArea.getReqHeight();
    const auto sourceCrop = renderArea.getSourceCrop();
    const auto transform = renderArea.getTransform();
    const auto rotation = renderArea.getRotationFlags();
    const auto& displayViewport = renderArea.getDisplayViewport();

    renderengine::DisplaySettings clientCompositionDisplay;
    std::vector<compositionengine::LayerFE::LayerSettings> clientCompositionLayers;

    // assume that bounds are never offset, and that they are the same as the
    // buffer bounds.
    clientCompositionDisplay.physicalDisplay = Rect(reqWidth, reqHeight);
    clientCompositionDisplay.clip = sourceCrop;
    clientCompositionDisplay.orientation = rotation;

    clientCompositionDisplay.outputDataspace = renderArea.getReqDataSpace();
    clientCompositionDisplay.maxLuminance = DisplayDevice::sDefaultMaxLumiance;

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
    Region clearRegion = Region::INVALID_REGION;
    traverseLayers([&](Layer* layer) {
        const bool supportProtectedContent = false;
        Region clip(renderArea.getBounds());
        compositionengine::LayerFE::ClientCompositionTargetSettings targetSettings{
                clip,
                useIdentityTransform,
                layer->needsFilteringForScreenshots(display.get(), transform) ||
                        renderArea.needsFiltering(),
                renderArea.isSecure(),
                supportProtectedContent,
                clearRegion,
                displayViewport,
                clientCompositionDisplay.outputDataspace,
                true,  /* realContentIsVisible */
                false, /* clearContent */
        };
        std::vector<compositionengine::LayerFE::LayerSettings> results =
                layer->prepareClientCompositionList(targetSettings);
        if (results.size() > 0) {
            for (auto& settings : results) {
                settings.geometry.positionTransform =
                        transform.asMatrix4() * settings.geometry.positionTransform;
                // There's no need to process blurs when we're executing region sampling,
                // we're just trying to understand what we're drawing, and doing so without
                // blurs is already a pretty good approximation.
                if (regionSampling) {
                    settings.backgroundBlurRadius = 0;
                }
            }
            clientCompositionLayers.insert(clientCompositionLayers.end(),
                                           std::make_move_iterator(results.begin()),
                                           std::make_move_iterator(results.end()));
            renderedLayers.push_back(layer);
        }
    });

    std::vector<const renderengine::LayerSettings*> clientCompositionLayerPointers(
            clientCompositionLayers.size());
    std::transform(clientCompositionLayers.begin(), clientCompositionLayers.end(),
                   clientCompositionLayerPointers.begin(),
                   std::pointer_traits<renderengine::LayerSettings*>::pointer_to);

    clientCompositionDisplay.clearRegion = clearRegion;
    // Use an empty fence for the buffer fence, since we just created the buffer so
    // there is no need for synchronization with the GPU.
    base::unique_fd bufferFence;
    base::unique_fd drawFence;
    getRenderEngine().useProtectedContext(false);
    getRenderEngine().drawLayers(clientCompositionDisplay, clientCompositionLayerPointers, buffer,
                                 /*useFramebufferCache=*/false, std::move(bufferFence), &drawFence);

    *outSyncFd = drawFence.release();

    if (*outSyncFd >= 0) {
        sp<Fence> releaseFence = new Fence(dup(*outSyncFd));
        for (auto* layer : renderedLayers) {
            layer->onLayerDisplayed(releaseFence);
        }
    }
}

status_t SurfaceFlinger::captureScreenImplLocked(const RenderArea& renderArea, TraverseLayersFunction traverseLayers, ANativeWindowBuffer* buffer, bool useIdentityTransform, bool forSystem, int* outSyncFd, bool regionSampling, bool& outCapturedSecureLayers) {
    ATRACE_CALL();

    traverseLayers([&](Layer* layer) {
        outCapturedSecureLayers =
                outCapturedSecureLayers || (layer->isVisible() && layer->isSecure());
    });

    // We allow the system server to take screenshots of secure layers for
    // use in situations like the Screen-rotation animation and place
    // the impetus on WindowManager to not persist them.
    if (outCapturedSecureLayers && !forSystem) {
        ALOGW("FB is protected: PERMISSION_DENIED");
        return PERMISSION_DENIED;
    }
    renderScreenImplLocked(renderArea, traverseLayers, buffer, useIdentityTransform, regionSampling,
                           outSyncFd);
    return NO_ERROR;
}

void SurfaceFlinger::setInputWindowsFinished() {
    Mutex::Autolock _l(mStateLock);

    mPendingSyncInputWindows = false;

    mTransactionCV.broadcast();
}

// ---------------------------------------------------------------------------
void SurfaceFlinger::State::traverse(const LayerVector::Visitor& visitor) const {
    layersSortedByZ.traverse(visitor);
}

// ---------------------------------------------------------------------------

void SurfaceFlinger::State::traverseInZOrder(const LayerVector::Visitor& visitor) const {
    layersSortedByZ.traverseInZOrder(stateSet, visitor);
}

void SurfaceFlinger::State::traverseInReverseZOrder(const LayerVector::Visitor& visitor) const {
    layersSortedByZ.traverseInReverseZOrder(stateSet, visitor);
}

void SurfaceFlinger::traverseLayersInDisplay(const sp<const DisplayDevice>& display,
                                             const LayerVector::Visitor& visitor) {
    // We loop through the first level of layers without traversing,
    // as we need to determine which layers belong to the requested display.
    for (const auto& layer : mDrawingState.layersSortedByZ) {
        if (!layer->belongsToDisplay(display->getLayerStack(), false)) {
            continue;
        }
        // relative layers are traversed in Layer::traverseInZOrder
        layer->traverseInZOrder(LayerVector::StateSet::Drawing, [&](Layer* layer) {
            if (!layer->belongsToDisplay(display->getLayerStack(), false)) {
                return;
            }
            if (!layer->isVisible()) {
                return;
            }
            visitor(layer);
        });
    }
}

status_t SurfaceFlinger::setDesiredDisplayConfigSpecsInternal(const sp<DisplayDevice>& display, const std::optional<scheduler::RefreshRateConfigs::Policy>& policy, bool overridePolicy) {
    Mutex::Autolock lock(mStateLock);

    LOG_ALWAYS_FATAL_IF(!display->isPrimary() && overridePolicy,
                        "Can only set override policy on the primary display");
    LOG_ALWAYS_FATAL_IF(!policy && !overridePolicy, "Can only clear the override policy");

    if (!display->isPrimary()) {
        // TODO(b/144711714): For non-primary displays we should be able to set an active config
        // as well. For now, just call directly to setActiveConfigWithConstraints but ideally
        // it should go thru setDesiredActiveConfig, similar to primary display.
        ALOGV("setAllowedDisplayConfigsInternal for non-primary display");
        const auto displayId = display->getId();
        LOG_ALWAYS_FATAL_IF(!displayId);

        hal::VsyncPeriodChangeConstraints constraints;
        constraints.desiredTimeNanos = systemTime();
        constraints.seamlessRequired = false;

        hal::VsyncPeriodChangeTimeline timeline = {0, 0, 0};
        if (getHwComposer().setActiveConfigWithConstraints(*displayId,
                                                           policy->defaultConfig.value(),
                                                           constraints, &timeline) < 0) {
            return BAD_VALUE;
        }
        if (timeline.refreshRequired) {
            repaintEverythingForHWC();
        }

        display->setActiveConfig(policy->defaultConfig);
        const nsecs_t vsyncPeriod = getHwComposer()
                                            .getConfigs(*displayId)[policy->defaultConfig.value()]
                                            ->getVsyncPeriod();
        mScheduler->onNonPrimaryDisplayConfigChanged(mAppConnectionHandle, display->getId()->value,
                                                     policy->defaultConfig, vsyncPeriod);
        return NO_ERROR;
    }

    if (mDebugDisplayConfigSetByBackdoor) {
        // ignore this request as config is overridden by backdoor
        return NO_ERROR;
    }

    status_t setPolicyResult = overridePolicy
            ? mRefreshRateConfigs->setOverridePolicy(policy)
            : mRefreshRateConfigs->setDisplayManagerPolicy(*policy);
    if (setPolicyResult < 0) {
        return BAD_VALUE;
    }
    if (setPolicyResult == scheduler::RefreshRateConfigs::CURRENT_POLICY_UNCHANGED) {
        return NO_ERROR;
    }
    scheduler::RefreshRateConfigs::Policy currentPolicy = mRefreshRateConfigs->getCurrentPolicy();

    ALOGV("Setting desired display config specs: defaultConfig: %d primaryRange: [%.0f %.0f]"
          " expandedRange: [%.0f %.0f]",
          currentPolicy.defaultConfig.value(), currentPolicy.primaryRange.min,
          currentPolicy.primaryRange.max, currentPolicy.appRequestRange.min,
          currentPolicy.appRequestRange.max);

    // TODO(b/140204874): Leave the event in until we do proper testing with all apps that might
    // be depending in this callback.
    const nsecs_t vsyncPeriod =
            mRefreshRateConfigs->getRefreshRateFromConfigId(display->getActiveConfig())
                    .getVsyncPeriod();
    mScheduler->onPrimaryDisplayConfigChanged(mAppConnectionHandle, display->getId()->value,
                                              display->getActiveConfig(), vsyncPeriod);
    toggleKernelIdleTimer();

    auto configId = mScheduler->getPreferredConfigId();
    auto& preferredRefreshRate = configId
            ? mRefreshRateConfigs->getRefreshRateFromConfigId(*configId)
            // NOTE: Choose the default config ID, if Scheduler doesn't have one in mind.
            : mRefreshRateConfigs->getRefreshRateFromConfigId(currentPolicy.defaultConfig);
    ALOGV("trying to switch to Scheduler preferred config %d (%s)",
          preferredRefreshRate.getConfigId().value(), preferredRefreshRate.getName().c_str());

    if (isDisplayConfigAllowed(preferredRefreshRate.getConfigId())) {
        ALOGV("switching to Scheduler preferred config %d",
              preferredRefreshRate.getConfigId().value());
        setDesiredActiveConfig(
                {preferredRefreshRate.getConfigId(), Scheduler::ConfigEvent::Changed});
    } else {
        LOG_ALWAYS_FATAL("Desired config not allowed: %d",
                         preferredRefreshRate.getConfigId().value());
    }

    return NO_ERROR;
}

status_t SurfaceFlinger::setDesiredDisplayConfigSpecs(const sp<IBinder>& displayToken, int32_t defaultConfig, float primaryRefreshRateMin, float primaryRefreshRateMax, float appRequestRefreshRateMin, float appRequestRefreshRateMax) {
    ATRACE_CALL();

    if (!displayToken) {
        return BAD_VALUE;
    }

    auto future = schedule([=]() -> status_t {
        const auto display = ON_MAIN_THREAD(getDisplayDeviceLocked(displayToken));
        if (!display) {
            ALOGE("Attempt to set desired display configs for invalid display token %p",
                  displayToken.get());
            return NAME_NOT_FOUND;
        } else if (display->isVirtual()) {
            ALOGW("Attempt to set desired display configs for virtual display");
            return INVALID_OPERATION;
        } else {
            using Policy = scheduler::RefreshRateConfigs::Policy;
            const Policy policy{HwcConfigIndexType(defaultConfig),
                                {primaryRefreshRateMin, primaryRefreshRateMax},
                                {appRequestRefreshRateMin, appRequestRefreshRateMax}};
            constexpr bool kOverridePolicy = false;

            return setDesiredDisplayConfigSpecsInternal(display, policy, kOverridePolicy);
        }
    });

    return future.get();
}

status_t SurfaceFlinger::getDesiredDisplayConfigSpecs(const sp<IBinder>& displayToken, int32_t* outDefaultConfig, float* outPrimaryRefreshRateMin, float* outPrimaryRefreshRateMax, float* outAppRequestRefreshRateMin, float* outAppRequestRefreshRateMax) {
    ATRACE_CALL();

    if (!displayToken || !outDefaultConfig || !outPrimaryRefreshRateMin ||
        !outPrimaryRefreshRateMax || !outAppRequestRefreshRateMin || !outAppRequestRefreshRateMax) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);
    const auto display = getDisplayDeviceLocked(displayToken);
    if (!display) {
        return NAME_NOT_FOUND;
    }

    if (display->isPrimary()) {
        scheduler::RefreshRateConfigs::Policy policy =
                mRefreshRateConfigs->getDisplayManagerPolicy();
        *outDefaultConfig = policy.defaultConfig.value();
        *outPrimaryRefreshRateMin = policy.primaryRange.min;
        *outPrimaryRefreshRateMax = policy.primaryRange.max;
        *outAppRequestRefreshRateMin = policy.appRequestRange.min;
        *outAppRequestRefreshRateMax = policy.appRequestRange.max;
        return NO_ERROR;
    } else if (display->isVirtual()) {
        return INVALID_OPERATION;
    } else {
        const auto displayId = display->getId();
        LOG_FATAL_IF(!displayId);

        *outDefaultConfig = getHwComposer().getActiveConfigIndex(*displayId);
        auto vsyncPeriod = getHwComposer().getActiveConfig(*displayId)->getVsyncPeriod();
        *outPrimaryRefreshRateMin = 1e9f / vsyncPeriod;
        *outPrimaryRefreshRateMax = 1e9f / vsyncPeriod;
        *outAppRequestRefreshRateMin = 1e9f / vsyncPeriod;
        *outAppRequestRefreshRateMax = 1e9f / vsyncPeriod;
        return NO_ERROR;
    }
}

void SurfaceFlinger::SetInputWindowsListener::onSetInputWindowsFinished() {
    mFlinger->setInputWindowsFinished();
}

wp<Layer> SurfaceFlinger::fromHandle(const sp<IBinder>& handle) {
    Mutex::Autolock _l(mStateLock);
    return fromHandleLocked(handle);
}

wp<Layer> SurfaceFlinger::fromHandleLocked(const sp<IBinder>& handle) {
    BBinder* b = nullptr;
    if (handle) {
        b = handle->localBinder();
    }
    if (b == nullptr) {
        return nullptr;
    }
    auto it = mLayersByLocalBinderToken.find(b);
    if (it != mLayersByLocalBinderToken.end()) {
        return it->second;
    }
    return nullptr;
}

void SurfaceFlinger::onLayerFirstRef(Layer* layer) {
    mNumLayers++;
    mScheduler->registerLayer(layer);
}

void SurfaceFlinger::onLayerDestroyed(Layer* layer) {
    mNumLayers--;
    removeFromOffscreenLayers(layer);
}

// WARNING: ONLY CALL THIS FROM LAYER DTOR
// Here we add children in the current state to offscreen layers and remove the
// layer itself from the offscreen layer list.  Since
// this is the dtor, it is safe to access the current state.  This keeps us
// from dangling children layers such that they are not reachable from the
// Drawing state nor the offscreen layer list
// See b/141111965
void SurfaceFlinger::removeFromOffscreenLayers(Layer* layer) {
    for (auto& child : layer->getCurrentChildren()) {
        mOffscreenLayers.emplace(child.get());
    }
    mOffscreenLayers.erase(layer);
}

void SurfaceFlinger::bufferErased(const client_cache_t& clientCacheId) {
    getRenderEngine().unbindExternalTextureBuffer(clientCacheId.id);
}

status_t SurfaceFlinger::setGlobalShadowSettings(const half4& ambientColor, const half4& spotColor, float lightPosY, float lightPosZ, float lightRadius) {
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
            // Note: METADATA_OWNER_UID and METADATA_WINDOW_TYPE are officially
            // supported, and exposed via the
            // IVrComposerClient::VrCommand::SET_LAYER_INFO command.
            {"org.chromium.arc.V1_0.TaskId", METADATA_TASK_ID},
            {"org.chromium.arc.V1_0.CursorInfo", METADATA_MOUSE_CURSOR},
    };
    return genericLayerMetadataKeyMap;
}

status_t SurfaceFlinger::setFrameRate(const sp<IGraphicBufferProducer>& surface, float frameRate, int8_t compatibility) {
    if (!ValidateFrameRate(frameRate, compatibility, "SurfaceFlinger::setFrameRate")) {
        return BAD_VALUE;
    }

    static_cast<void>(schedule([=] {
        Mutex::Autolock lock(mStateLock);
        if (authenticateSurfaceTextureLocked(surface)) {
            sp<Layer> layer = (static_cast<MonitoredProducer*>(surface.get()))->getLayer();
            if (layer->setFrameRate(
                        Layer::FrameRate(frameRate,
                                         Layer::FrameRate::convertCompatibility(compatibility)))) {
                setTransactionFlags(eTraversalNeeded);
            }
        } else {
            ALOGE("Attempt to set frame rate on an unrecognized IGraphicBufferProducer");
            return BAD_VALUE;
        }
        return NO_ERROR;
    }));

    return NO_ERROR;
}

status_t SurfaceFlinger::acquireFrameRateFlexibilityToken(sp<IBinder>* outToken) {
    if (!outToken) {
        return BAD_VALUE;
    }

    auto future = schedule([this] {
        status_t result = NO_ERROR;
        sp<IBinder> token;

        if (mFrameRateFlexibilityTokenCount == 0) {
            const auto display = ON_MAIN_THREAD(getDefaultDisplayDeviceLocked());

            // This is a little racy, but not in a way that hurts anything. As we grab the
            // defaultConfig from the display manager policy, we could be setting a new display
            // manager policy, leaving us using a stale defaultConfig. The defaultConfig doesn't
            // matter for the override policy though, since we set allowGroupSwitching to true, so
            // it's not a problem.
            scheduler::RefreshRateConfigs::Policy overridePolicy;
            overridePolicy.defaultConfig =
                    mRefreshRateConfigs->getDisplayManagerPolicy().defaultConfig;
            overridePolicy.allowGroupSwitching = true;
            constexpr bool kOverridePolicy = true;
            result = setDesiredDisplayConfigSpecsInternal(display, overridePolicy, kOverridePolicy);
        }

        if (result == NO_ERROR) {
            mFrameRateFlexibilityTokenCount++;
            // Handing out a reference to the SurfaceFlinger object, as we're doing in the line
            // below, is something to consider carefully. The lifetime of the
            // FrameRateFlexibilityToken isn't tied to SurfaceFlinger object lifetime, so if this
            // SurfaceFlinger object were to be destroyed while the token still exists, the token
            // destructor would be accessing a stale SurfaceFlinger reference, and crash. This is ok
            // in this case, for two reasons:
            //   1. Once SurfaceFlinger::run() is called by main_surfaceflinger.cpp, the only way
            //   the program exits is via a crash. So we won't have a situation where the
            //   SurfaceFlinger object is dead but the process is still up.
            //   2. The frame rate flexibility token is acquired/released only by CTS tests, so even
            //   if condition 1 were changed, the problem would only show up when running CTS tests,
            //   not on end user devices, so we could spot it and fix it without serious impact.
            token = new FrameRateFlexibilityToken(
                    [this]() { onFrameRateFlexibilityTokenReleased(); });
            ALOGD("Frame rate flexibility token acquired. count=%d",
                  mFrameRateFlexibilityTokenCount);
        }

        return std::make_pair(result, token);
    });

    status_t result;
    std::tie(result, *outToken) = future.get();
    return result;
}

void SurfaceFlinger::onFrameRateFlexibilityTokenReleased() {
    static_cast<void>(schedule([this] {
        LOG_ALWAYS_FATAL_IF(mFrameRateFlexibilityTokenCount == 0,
                            "Failed tracking frame rate flexibility tokens");
        mFrameRateFlexibilityTokenCount--;
        ALOGD("Frame rate flexibility token released. count=%d", mFrameRateFlexibilityTokenCount);
        if (mFrameRateFlexibilityTokenCount == 0) {
            const auto display = ON_MAIN_THREAD(getDefaultDisplayDeviceLocked());
            constexpr bool kOverridePolicy = true;
            status_t result = setDesiredDisplayConfigSpecsInternal(display, {}, kOverridePolicy);
            LOG_ALWAYS_FATAL_IF(result < 0, "Failed releasing frame rate flexibility token");
        }
    }));
}

void SurfaceFlinger::enableRefreshRateOverlay(bool enable) {
    static_cast<void>(schedule([=] {
        std::unique_ptr<RefreshRateOverlay> overlay;
        if (enable) {
            overlay = std::make_unique<RefreshRateOverlay>(*this);
        }

        {
            Mutex::Autolock lock(mStateLock);

            // Destroy the layer of the current overlay, if any, outside the lock.
            mRefreshRateOverlay.swap(overlay);
            if (!mRefreshRateOverlay) return;

            if (const auto display = getDefaultDisplayDeviceLocked()) {
                mRefreshRateOverlay->setViewport(display->getSize());
            }

            mRefreshRateOverlay->changeRefreshRate(mRefreshRateConfigs->getCurrentRefreshRate());
        }
    }));
}

  // namespace android