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
//#define LOG_NDEBUG 0

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <sys/types.h>
#include <errno.h>
#include <dlfcn.h>
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <cutils/properties.h>
#include <log/log.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/PermissionCache.h>
#include <compositionengine/CompositionEngine.h>
#include <compositionengine/CompositionRefreshArgs.h>
#include <compositionengine/Display.h>
#include <compositionengine/DisplayColorProfile.h>
#include <compositionengine/Layer.h>
#include <compositionengine/OutputLayer.h>
#include <compositionengine/RenderSurface.h>
#include <compositionengine/impl/LayerCompositionState.h>
#include <compositionengine/impl/OutputCompositionState.h>
#include <compositionengine/impl/OutputLayerCompositionState.h>
#include <dvr/vr_flinger.h>
#include <gui/BufferQueue.h>
#include <gui/DebugEGLImageTracker.h>
#include <gui/GuiConfig.h>
#include <gui/IDisplayEventConnection.h>
#include <gui/IProducerListener.h>
#include <gui/LayerDebugInfo.h>
#include <gui/Surface.h>
#include <input/IInputFlinger.h>
#include <renderengine/RenderEngine.h>
#include <ui/ColorSpace.h>
#include <ui/DebugUtils.h>
#include <ui/DisplayInfo.h>
#include <ui/DisplayStatInfo.h>
#include <ui/GraphicBufferAllocator.h>
#include <ui/PixelFormat.h>
#include <ui/UiConfig.h>
#include <utils/StopWatch.h>
#include <utils/String16.h>
#include <utils/String8.h>
#include <utils/Timers.h>
#include <utils/Trace.h>
#include <utils/misc.h>
#include <private/android_filesystem_config.h>
#include <private/gui/SyncFeatures.h>
#include "BufferLayer.h"
#include "BufferQueueLayer.h"
#include "BufferStateLayer.h"
#include "Client.h"
#include "ColorLayer.h"
#include "Colorizer.h"
#include "ContainerLayer.h"
#include "DisplayDevice.h"
#include "Layer.h"
#include "LayerVector.h"
#include "MonitoredProducer.h"
#include "NativeWindowSurface.h"
#include "RefreshRateOverlay.h"
#include "StartPropertySetThread.h"
#include "SurfaceFlinger.h"
#include "SurfaceInterceptor.h"
#include "DisplayHardware/ComposerHal.h"
#include "DisplayHardware/DisplayIdentification.h"
#include "DisplayHardware/FramebufferSurface.h"
#include "DisplayHardware/HWComposer.h"
#include "DisplayHardware/VirtualDisplaySurface.h"
#include "Effects/Daltonizer.h"
#include "FrameTracer/FrameTracer.h"
#include "RegionSamplingThread.h"
#include "Scheduler/DispSync.h"
#include "Scheduler/DispSyncSource.h"
#include "Scheduler/EventControlThread.h"
#include "Scheduler/EventThread.h"
#include "Scheduler/InjectVSyncSource.h"
#include "Scheduler/MessageQueue.h"
#include "Scheduler/PhaseOffsets.h"
#include "Scheduler/Scheduler.h"
#include "TimeStats/TimeStats.h"
#include <cutils/compiler.h>
#include "android-base/stringprintf.h"
#include <android/hardware/configstore/1.0/ISurfaceFlingerConfigs.h>
#include <android/hardware/configstore/1.1/ISurfaceFlingerConfigs.h>
#include <android/hardware/configstore/1.1/types.h>
#include <android/hardware/power/1.0/IPower.h>
#include <configstore/Utils.h>
#include <layerproto/LayerProtoParser.h>
#include "SurfaceFlingerProperties.h"

namespace android {

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

ui::Transform::orientation_flags fromSurfaceComposerRotation(ISurfaceComposer::Rotation rotation) {
    switch (rotation) {
        case ISurfaceComposer::eRotateNone:
            return ui::Transform::ROT_0;
        case ISurfaceComposer::eRotate90:
            return ui::Transform::ROT_90;
        case ISurfaceComposer::eRotate180:
            return ui::Transform::ROT_180;
        case ISurfaceComposer::eRotate270:
            return ui::Transform::ROT_270;
    }
    ALOGE("Invalid rotation passed to captureScreen(): %d\n", rotation);
    return ui::Transform::ROT_0;
}

class ConditionalLock {
public:
    ConditionalLock(Mutex& mutex, bool lock) : mMutex(mutex), mLocked(lock) {
        if (lock) {
            mMutex.lock();
        }
    }
    ~ConditionalLock() { if (mLocked) mMutex.unlock(); }
private:
    Mutex& mMutex;
    bool mLocked;
};

// Currently we only support V0_SRGB and DISPLAY_P3 as composition preference.
bool validateCompositionDataspace(Dataspace dataspace) {
    return dataspace == Dataspace::V0_SRGB || dataspace == Dataspace::DISPLAY_P3;
}

} // namespace anonymous

// ---------------------------------------------------------------------------

const String16 sHardwareTest("android.permission.HARDWARE_TEST");
const String16 sAccessSurfaceFlinger("android.permission.ACCESS_SURFACE_FLINGER");
const String16 sReadFramebuffer("android.permission.READ_FRAME_BUFFER");
const String16 sDump("android.permission.DUMP");

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
        mTimeStats(mFactory.createTimeStats()),
        mFrameTracer(std::make_unique<FrameTracer>()),
        mEventQueue(mFactory.createMessageQueue()),
        mCompositionEngine(mFactory.createCompositionEngine()),
        mPhaseOffsets(mFactory.createPhaseOffsets()) {}

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

    auto tmpPrimaryDisplayOrientation = primary_display_orientation(
            SurfaceFlingerProperties::primary_display_orientation_values::ORIENTATION_0);
    switch (tmpPrimaryDisplayOrientation) {
        case SurfaceFlingerProperties::primary_display_orientation_values::ORIENTATION_90:
            SurfaceFlinger::primaryDisplayOrientation = DisplayState::eOrientation90;
            break;
        case SurfaceFlingerProperties::primary_display_orientation_values::ORIENTATION_180:
            SurfaceFlinger::primaryDisplayOrientation = DisplayState::eOrientation180;
            break;
        case SurfaceFlingerProperties::primary_display_orientation_values::ORIENTATION_270:
            SurfaceFlinger::primaryDisplayOrientation = DisplayState::eOrientation270;
            break;
        default:
            SurfaceFlinger::primaryDisplayOrientation = DisplayState::eOrientationDefault;
            break;
    }
    ALOGV("Primary Display Orientation is set to %2d.", SurfaceFlinger::primaryDisplayOrientation);

    mInternalDisplayPrimaries = sysprop::getDisplayNativePrimaries();

    // debugging stuff...
    char value[PROPERTY_VALUE_MAX];

    property_get("ro.bq.gpu_to_cpu_unsupported", value, "0");
    mGpuToCpuSupported = !atoi(value);

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

    const size_t defaultListSize = MAX_LAYERS;
    auto listSize = property_get_int32("debug.sf.max_igbp_list_size", int32_t(defaultListSize));
    mMaxGraphicBufferProducerListSize = (listSize > 0) ? size_t(listSize) : defaultListSize;

    mUseSmart90ForVideo = use_smart_90_for_video(false);
    property_get("debug.sf.use_smart_90_for_video", value, "0");

    int int_value = atoi(value);
    if (int_value) {
        mUseSmart90ForVideo = true;
    }

    property_get("debug.sf.luma_sampling", value, "1");
    mLumaSampling = atoi(value);

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
}

void SurfaceFlinger::onFirstRef()
{
    mEventQueue->init(this);
}

SurfaceFlinger::~SurfaceFlinger() 

void SurfaceFlinger::binderDied(const wp<IBinder>& /* who */)
{
    // the window manager died on us. prepare its eulogy.

    // restore initial conditions (default device unblank, etc)
    initializeDisplays();

    // restart the boot-animation
    startBootAnim();
}

static sp<ISurfaceComposerClient> initClient(const sp<Client>& client) {
    status_t err = client->initCheck();
    if (err == NO_ERROR) {
        return client;
    }
    return nullptr;
}

sp<ISurfaceComposerClient> SurfaceFlinger::createConnection() {
    return initClient(new Client(this));
}

sp<IBinder> SurfaceFlinger::createDisplay(const String8& displayName,
        bool secure)
{
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
    Mutex::Autolock _l(mStateLock);

    ssize_t index = mCurrentState.displays.indexOfKey(displayToken);
    if (index < 0) {
        ALOGE("destroyDisplay: Invalid display token %p", displayToken.get());
        return;
    }

    const DisplayDeviceState& state = mCurrentState.displays.valueAt(index);
    if (!state.isVirtual()) {
        ALOGE("destroyDisplay called for non-virtual display");
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
    if (mStartPropertySetThread->join() != NO_ERROR) {
        ALOGE("Join StartPropertySetThread failed!");
    }
    const nsecs_t now = systemTime();
    const nsecs_t duration = now - mBootTime;
    ALOGI("Boot is finished (%ld ms)", long(ns2ms(duration)) );

    mFrameTracer->initialize();

    // wait patiently for the window manager death
    const String16 name("window");
    mWindowManager = defaultServiceManager()->getService(name);
    if (mWindowManager != 0) {
        mWindowManager->linkToDeath(static_cast<IBinder::DeathRecipient*>(this));
    }
    sp<IBinder> input(defaultServiceManager()->getService(
            String16("inputflinger")));
    if (input == nullptr) {
        ALOGE("Failed to link to input service");
    } else {
        mInputFlinger = interface_cast<IInputFlinger>(input);
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

    postMessageAsync(new LambdaMessage([this]() NO_THREAD_SAFETY_ANALYSIS {
        readPersistentProperties();
        mBootStage = BootStage::FINISHED;

        // set the refresh rate according to the policy
        const auto& performanceRefreshRate =
                mRefreshRateConfigs.getRefreshRate(RefreshRateType::PERFORMANCE);

        if (performanceRefreshRate && isDisplayConfigAllowed(performanceRefreshRate->configId)) {
            setRefreshRateTo(RefreshRateType::PERFORMANCE, Scheduler::ConfigEvent::None);
        } else {
            setRefreshRateTo(RefreshRateType::DEFAULT, Scheduler::ConfigEvent::None);
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
    uint32_t name = 0;
    postMessageSync(new LambdaMessage([&]() { getRenderEngine().genTextures(1, &name); }));
    return name;
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

    ALOGI("Phase offset: %" PRId64 " ns", mPhaseOffsets->getCurrentAppOffset());

    Mutex::Autolock _l(mStateLock);
    // start the EventThread
    mScheduler =
            getFactory().createScheduler([this](bool enabled) { setPrimaryVsyncEnabled(enabled); },
                                         mRefreshRateConfigs);
    auto resyncCallback =
            mScheduler->makeResyncCallback(std::bind(&SurfaceFlinger::getVsyncPeriod, this));

    mAppConnectionHandle =
            mScheduler->createConnection("app", mPhaseOffsets->getCurrentAppOffset(),
                                         mPhaseOffsets->getOffsetThresholdForNextVsync(),
                                         resyncCallback,
                                         impl::EventThread::InterceptVSyncsCallback());
    mSfConnectionHandle =
            mScheduler->createConnection("sf", mPhaseOffsets->getCurrentSfOffset(),
                                         mPhaseOffsets->getOffsetThresholdForNextVsync(),
                                         resyncCallback, [this](nsecs_t timestamp) {
                                             mInterceptor->saveVSyncEvent(timestamp);
                                         });

    mEventQueue->setEventConnection(mScheduler->getEventConnection(mSfConnectionHandle));

    mVSyncModulator.emplace(*mScheduler, mAppConnectionHandle, mSfConnectionHandle,
                            mPhaseOffsets->getCurrentOffsets());

    mRegionSamplingThread =
            new RegionSamplingThread(*this, *mScheduler,
                                     RegionSamplingThread::EnvironmentTimingTunables());

    // Get a RenderEngine for the given display / config (can't fail)
    int32_t renderEngineFeature = 0;
    renderEngineFeature |= (useColorManagement ?
                            renderengine::RenderEngine::USE_COLOR_MANAGEMENT : 0);
    renderEngineFeature |= (useContextPriority ?
                            renderengine::RenderEngine::USE_HIGH_PRIORITY_CONTEXT : 0);
    renderEngineFeature |=
            (enable_protected_contents(false) ? renderengine::RenderEngine::ENABLE_PROTECTED_CONTEXT
                                              : 0);

    // TODO(b/77156734): We need to stop casting and use HAL types when possible.
    // Sending maxFrameBufferAcquiredBuffers as the cache size is tightly tuned to single-display.
    mCompositionEngine->setRenderEngine(
            renderengine::RenderEngine::create(static_cast<int32_t>(defaultCompositionPixelFormat),
                                               renderEngineFeature, maxFrameBufferAcquiredBuffers));

    LOG_ALWAYS_FATAL_IF(mVrFlingerRequestsDisplay,
            "Starting with vr flinger active is not currently supported.");
    mCompositionEngine->setHwComposer(getFactory().createHWComposer(getBE().mHwcServiceName));
    mCompositionEngine->getHwComposer().registerCallback(this, getBE().mComposerSequenceId);
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
            postMessageAsync(new LambdaMessage([=] {
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
            !getHwComposer().hasCapability(HWC2::Capability::PresentFenceIsNotReliable);
    mStartPropertySetThread = getFactory().createStartPropertySetThread(presentFenceReliable);

    if (mStartPropertySetThread->Start() != NO_ERROR) {
        ALOGE("Run StartPropertySetThread failed!");
    }

    mScheduler->setChangeRefreshRateCallback(
            [this](RefreshRateType type, Scheduler::ConfigEvent event) {
                Mutex::Autolock lock(mStateLock);
                setRefreshRateTo(type, event);
            });
    mScheduler->setGetCurrentRefreshRateTypeCallback([this] {
        Mutex::Autolock lock(mStateLock);
        const auto display = getDefaultDisplayDeviceLocked();
        if (!display) {
            // If we don't have a default display the fallback to the default
            // refresh rate type
            return RefreshRateType::DEFAULT;
        }

        const int configId = display->getActiveConfig();
        for (const auto& [type, refresh] : mRefreshRateConfigs.getRefreshRates()) {
            if (refresh && refresh->configId == configId) {
                return type;
            }
        }
        // This should never happen, but just gracefully fallback to default.
        return RefreshRateType::DEFAULT;
    });
    mScheduler->setGetVsyncPeriodCallback([this] {
        Mutex::Autolock lock(mStateLock);
        return getVsyncPeriod();
    });

    mRefreshRateConfigs.populate(getHwComposer().getConfigs(*display->getId()));
    mRefreshRateStats.setConfigMode(getHwComposer().getActiveConfigIndex(*display->getId()));

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

status_t SurfaceFlinger::getDisplayConfigs(const sp<IBinder>& displayToken,
                                           Vector<DisplayInfo>* configs) {
    if (!displayToken || !configs) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);

    const auto displayId = getPhysicalDisplayIdLocked(displayToken);
    if (!displayId) {
        return NAME_NOT_FOUND;
    }

    // TODO: Not sure if display density should handled by SF any longer
    class Density {
        static float getDensityFromProperty(char const* propName) {
            char property[PROPERTY_VALUE_MAX];
            float density = 0.0f;
            if (property_get(propName, property, nullptr) > 0) {
                density = strtof(property, nullptr);
            }
            return density;
        }
    public:
        static float getEmuDensity() {
            return getDensityFromProperty("qemu.sf.lcd_density"); }
        static float getBuildDensity()  {
            return getDensityFromProperty("ro.sf.lcd_density"); }
    };

    configs->clear();

    for (const auto& hwConfig : getHwComposer().getConfigs(*displayId)) {
        DisplayInfo info = DisplayInfo();

        float xdpi = hwConfig->getDpiX();
        float ydpi = hwConfig->getDpiY();

        info.w = hwConfig->getWidth();
        info.h = hwConfig->getHeight();
        // Default display viewport to display width and height
        info.viewportW = info.w;
        info.viewportH = info.h;

        if (displayId == getInternalDisplayIdLocked()) {
            // The density of the device is provided by a build property
            float density = Density::getBuildDensity() / 160.0f;
            if (density == 0) {
                // the build doesn't provide a density -- this is wrong!
                // use xdpi instead
                ALOGE("ro.sf.lcd_density must be defined as a build property");
                density = xdpi / 160.0f;
            }
            if (Density::getEmuDensity()) {
                // if "qemu.sf.lcd_density" is specified, it overrides everything
                xdpi = ydpi = density = Density::getEmuDensity();
                density /= 160.0f;
            }
            info.density = density;

            // TODO: this needs to go away (currently needed only by webkit)
            const auto display = getDefaultDisplayDeviceLocked();
            info.orientation = display ? display->getOrientation() : 0;

            // This is for screenrecord
            const Rect viewport = display->getViewport();
            if (viewport.isValid()) {
                info.viewportW = uint32_t(viewport.getWidth());
                info.viewportH = uint32_t(viewport.getHeight());
            }
        } else {
            // TODO: where should this value come from?
            static const int TV_DENSITY = 213;
            info.density = TV_DENSITY / 160.0f;
            info.orientation = 0;
        }

        info.xdpi = xdpi;
        info.ydpi = ydpi;
        info.fps = 1e9 / hwConfig->getVsyncPeriod();
        const auto refreshRateType = mRefreshRateConfigs.getRefreshRateType(hwConfig->getId());
        const auto offset = mPhaseOffsets->getOffsetsForRefreshRate(refreshRateType);
        info.appVsyncOffset = offset.late.app;

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
        info.presentationDeadline = hwConfig->getVsyncPeriod() - offset.late.sf + 1000000;

        // All non-virtual displays are currently considered secure.
        info.secure = true;

        if (displayId == getInternalDisplayIdLocked() &&
            primaryDisplayOrientation & DisplayState::eOrientationSwapMask) {
            std::swap(info.w, info.h);
        }

        configs->push_back(info);
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
    const auto display = getDisplayDevice(displayToken);
    if (!display) {
        ALOGE("getActiveConfig: Invalid display token %p", displayToken.get());
        return BAD_VALUE;
    }

    return display->getActiveConfig();
}

void SurfaceFlinger::setDesiredActiveConfig(const ActiveConfigInfo& info) {
    ATRACE_CALL();

    // Don't check against the current mode yet. Worst case we set the desired
    // config twice. However event generation config might have changed so we need to update it
    // accordingly
    std::lock_guard<std::mutex> lock(mActiveConfigLock);
    const Scheduler::ConfigEvent prevConfig = mDesiredActiveConfig.event;
    mDesiredActiveConfig = info;
    mDesiredActiveConfig.event = mDesiredActiveConfig.event | prevConfig;

    if (!mDesiredActiveConfigChanged) {
        // This will trigger HWC refresh without resetting the idle timer.
        repaintEverythingForHWC();
        // Start receiving vsync samples now, so that we can detect a period
        // switch.
        mScheduler->resyncToHardwareVsync(true, getVsyncPeriod());
        // As we called to set period, we will call to onRefreshRateChangeCompleted once
        // DispSync model is locked.
        mVSyncModulator->onRefreshRateChangeInitiated();
        mPhaseOffsets->setRefreshRateType(info.type);
        mVSyncModulator->setPhaseOffsets(mPhaseOffsets->getCurrentOffsets());
    }
    mDesiredActiveConfigChanged = true;

    if (mRefreshRateOverlay) {
        mRefreshRateOverlay->changeRefreshRate(mDesiredActiveConfig.type);
    }
}

status_t SurfaceFlinger::setActiveConfig(const sp<IBinder>& displayToken, int mode) {
    ATRACE_CALL();

    std::vector<int32_t> allowedConfig;
    allowedConfig.push_back(mode);

    return setAllowedDisplayConfigs(displayToken, allowedConfig);
}

void SurfaceFlinger::setActiveConfigInternal() {
    ATRACE_CALL();

    const auto display = getDefaultDisplayDeviceLocked();
    if (!display) {
        return;
    }

    std::lock_guard<std::mutex> lock(mActiveConfigLock);
    mRefreshRateStats.setConfigMode(mUpcomingActiveConfig.configId);

    display->setActiveConfig(mUpcomingActiveConfig.configId);

    mPhaseOffsets->setRefreshRateType(mUpcomingActiveConfig.type);
    mVSyncModulator->setPhaseOffsets(mPhaseOffsets->getCurrentOffsets());
    ATRACE_INT("ActiveConfigMode", mUpcomingActiveConfig.configId);

    if (mUpcomingActiveConfig.event != Scheduler::ConfigEvent::None) {
        mScheduler->onConfigChanged(mAppConnectionHandle, display->getId()->value,
                                    mUpcomingActiveConfig.configId);
    }
}

void SurfaceFlinger::desiredActiveConfigChangeDone() {
    std::lock_guard<std::mutex> lock(mActiveConfigLock);
    mDesiredActiveConfig.event = Scheduler::ConfigEvent::None;
    mDesiredActiveConfigChanged = false;

    mScheduler->resyncToHardwareVsync(true, getVsyncPeriod());
    mPhaseOffsets->setRefreshRateType(mUpcomingActiveConfig.type);
    mVSyncModulator->setPhaseOffsets(mPhaseOffsets->getCurrentOffsets());
}

bool SurfaceFlinger::performSetActiveConfig() {
    ATRACE_CALL();
    if (mCheckPendingFence) {
        if (previousFrameMissed()) {
            // fence has not signaled yet. wait for the next invalidate
            mEventQueue->invalidate();
            return true;
        }

        // We received the present fence from the HWC, so we assume it successfully updated
        // the config, hence we update SF.
        mCheckPendingFence = false;
        setActiveConfigInternal();
    }

    // Store the local variable to release the lock.
    ActiveConfigInfo desiredActiveConfig;
    {
        std::lock_guard<std::mutex> lock(mActiveConfigLock);
        if (!mDesiredActiveConfigChanged) {
            return false;
        }
        desiredActiveConfig = mDesiredActiveConfig;
    }

    const auto display = getDefaultDisplayDeviceLocked();
    if (!display || display->getActiveConfig() == desiredActiveConfig.configId) {
        // display is not valid or we are already in the requested mode
        // on both cases there is nothing left to do
        desiredActiveConfigChangeDone();
        return false;
    }

    // Desired active config was set, it is different than the config currently in use, however
    // allowed configs might have change by the time we process the refresh.
    // Make sure the desired config is still allowed
    if (!isDisplayConfigAllowed(desiredActiveConfig.configId)) {
        desiredActiveConfigChangeDone();
        return false;
    }

    mUpcomingActiveConfig = desiredActiveConfig;
    const auto displayId = display->getId();
    LOG_ALWAYS_FATAL_IF(!displayId);

    ATRACE_INT("ActiveConfigModeHWC", mUpcomingActiveConfig.configId);
    getHwComposer().setActiveConfig(*displayId, mUpcomingActiveConfig.configId);

    // we need to submit an empty frame to HWC to start the process
    mCheckPendingFence = true;
    mEventQueue->invalidate();
    return false;
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
        return BAD_VALUE;
    }

    memcpy(&primaries, &mInternalDisplayPrimaries, sizeof(ui::DisplayPrimaries));
    return NO_ERROR;
}

ColorMode SurfaceFlinger::getActiveColorMode(const sp<IBinder>& displayToken) {
    if (const auto display = getDisplayDevice(displayToken)) {
        return display->getCompositionDisplay()->getState().colorMode;
    }
    return static_cast<ColorMode>(BAD_VALUE);
}

status_t SurfaceFlinger::setActiveColorMode(const sp<IBinder>& displayToken, ColorMode mode) {
    postMessageSync(new LambdaMessage([&] {
        Vector<ColorMode> modes;
        getDisplayColorModes(displayToken, &modes);
        bool exists = std::find(std::begin(modes), std::end(modes), mode) != std::end(modes);
        if (mode < ColorMode::NATIVE || !exists) {
            ALOGE("Attempt to set invalid active color mode %s (%d) for display token %p",
                  decodeColorMode(mode).c_str(), mode, displayToken.get());
            return;
        }
        const auto display = getDisplayDevice(displayToken);
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
    }));

    return NO_ERROR;
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
    Mutex::Autolock _l(mStateLock);

    const auto display = getDisplayDeviceLocked(displayToken);
    if (!display) {
        ALOGE("getHdrCapabilities: Invalid display token %p", displayToken.get());
        return BAD_VALUE;
    }

    // At this point the DisplayDeivce should already be set up,
    // meaning the luminance information is already queried from
    // hardware composer and stored properly.
    const HdrCapabilities& capabilities = display->getHdrCapabilities();
    *outCapabilities = HdrCapabilities(capabilities.getSupportedHdrTypes(),
                                       capabilities.getDesiredMaxLuminance(),
                                       capabilities.getDesiredMaxAverageLuminance(),
                                       capabilities.getDesiredMinLuminance());

    return NO_ERROR;
}

status_t SurfaceFlinger::getDisplayedContentSamplingAttributes(const sp<IBinder>& displayToken,
                                                               ui::PixelFormat* outFormat,
                                                               ui::Dataspace* outDataspace,
                                                               uint8_t* outComponentMask) const {
    if (!outFormat || !outDataspace || !outComponentMask) {
        return BAD_VALUE;
    }
    const auto display = getDisplayDevice(displayToken);
    if (!display || !display->getId()) {
        ALOGE("getDisplayedContentSamplingAttributes: Bad display token: %p", display.get());
        return BAD_VALUE;
    }
    return getHwComposer().getDisplayedContentSamplingAttributes(*display->getId(), outFormat,
                                                                 outDataspace, outComponentMask);
}

status_t SurfaceFlinger::setDisplayContentSamplingEnabled(const sp<IBinder>& displayToken,
                                                          bool enable, uint8_t componentMask,
                                                          uint64_t maxFrames) const {
    const auto display = getDisplayDevice(displayToken);
    if (!display || !display->getId()) {
        ALOGE("setDisplayContentSamplingEnabled: Bad display token: %p", display.get());
        return BAD_VALUE;
    }

    return getHwComposer().setDisplayContentSamplingEnabled(*display->getId(), enable,
                                                            componentMask, maxFrames);
}

status_t SurfaceFlinger::getDisplayedContentSample(const sp<IBinder>& displayToken,
                                                   uint64_t maxFrames, uint64_t timestamp,
                                                   DisplayedFrameStats* outStats) const {
    const auto display = getDisplayDevice(displayToken);
    if (!display || !display->getId()) {
        ALOGE("getDisplayContentSample: Bad display token: %p", displayToken.get());
        return BAD_VALUE;
    }

    return getHwComposer().getDisplayedContentSample(*display->getId(), maxFrames, timestamp,
                                                     outStats);
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
    Mutex::Autolock _l(mStateLock);
    const auto display = getDisplayDeviceLocked(displayToken);
    if (!display) {
        return BAD_VALUE;
    }

    // Use hasWideColorDisplay to override built-in display.
    const auto displayId = display->getId();
    if (displayId && displayId == getInternalDisplayIdLocked()) {
        *outIsWideColorDisplay = hasWideColorDisplay;
        return NO_ERROR;
    }
    *outIsWideColorDisplay = display->hasWideColorGamut();
    return NO_ERROR;
}

status_t SurfaceFlinger::enableVSyncInjections(bool enable) {
    postMessageSync(new LambdaMessage([&] {
        Mutex::Autolock _l(mStateLock);

        if (mInjectVSyncs == enable) {
            return;
        }

        auto resyncCallback =
                mScheduler->makeResyncCallback(std::bind(&SurfaceFlinger::getVsyncPeriod, this));

        // TODO(b/128863962): Part of the Injector should be refactored, so that it
        // can be passed to Scheduler.
        if (enable) {
            ALOGV("VSync Injections enabled");
            if (mVSyncInjector.get() == nullptr) {
                mVSyncInjector = std::make_unique<InjectVSyncSource>();
                mInjectorEventThread = std::make_unique<
                        impl::EventThread>(mVSyncInjector.get(),
                                           impl::EventThread::InterceptVSyncsCallback(),
                                           "injEventThread");
            }
            mEventQueue->setEventThread(mInjectorEventThread.get(), std::move(resyncCallback));
        } else {
            ALOGV("VSync Injections disabled");
            mEventQueue->setEventThread(mScheduler->getEventThread(mSfConnectionHandle),
                                        std::move(resyncCallback));
        }

        mInjectVSyncs = enable;
    }));

    return NO_ERROR;
}

status_t SurfaceFlinger::injectVSync(nsecs_t when) {
    Mutex::Autolock _l(mStateLock);

    if (!mInjectVSyncs) {
        ALOGE("VSync Injections not enabled");
        return BAD_VALUE;
    }
    if (mInjectVSyncs && mInjectorEventThread.get() != nullptr) {
        ALOGV("Injecting VSync inside SurfaceFlinger");
        mVSyncInjector->onInjectSyncEvent(when);
    }
    return NO_ERROR;
}

status_t SurfaceFlinger::getLayerDebugInfo(std::vector<LayerDebugInfo>* outLayers) const
        NO_THREAD_SAFETY_ANALYSIS {
    // Try to acquire a lock for 1s, fail gracefully
    const status_t err = mStateLock.timedLock(s2ns(1));
    const bool locked = (err == NO_ERROR);
    if (!locked) {
        ALOGE("LayerDebugInfo: SurfaceFlinger unresponsive (%s [%d]) - exit", strerror(-err), err);
        return TIMED_OUT;
    }

    outLayers->clear();
    mCurrentState.traverseInZOrder([&](Layer* layer) {
        outLayers->push_back(layer->getLayerDebugInfo());
    });

    mStateLock.unlock();
    return NO_ERROR;
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

// ----------------------------------------------------------------------------

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

// ----------------------------------------------------------------------------

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

// ---------------------------------------------------------------------------

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

// ---------------------------------------------------------------------------

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
    }
}

void Scheduler::changeRefreshRate(RefreshRateType refreshRateType, ConfigEvent configEvent) {
    std::lock_guard<std::mutex> lock(mCallbackLock);
    if (mChangeRefreshRateCallback) {
        mChangeRefreshRateCallback(refreshRateType, configEvent);
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
                                       const Dataspace reqDataspace,
                                       const ui::PixelFormat reqPixelFormat, Rect sourceCrop,
                                       uint32_t reqWidth, uint32_t reqHeight,
                                       bool useIdentityTransform,
                                       ISurfaceComposer::Rotation rotation,
                                       bool captureSecureLayers) {
    ATRACE_CALL();

    if (!displayToken) return BAD_VALUE;

    auto renderAreaRotation = fromSurfaceComposerRotation(rotation);

    sp<DisplayDevice> display;
    {
        Mutex::Autolock _l(mStateLock);

        display = getDisplayDeviceLocked(displayToken);
        if (!display) return BAD_VALUE;

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

const sp<DisplayDevice> SurfaceFlinger::getDisplayByIdOrLayerStack(uint64_t displayOrLayerStack) {
    const sp<IBinder> displayToken = getPhysicalDisplayTokenLocked(DisplayId{displayOrLayerStack});
    if (displayToken) {
        return getDisplayDeviceLocked(displayToken);
    }
    // Couldn't find display by displayId. Try to get display by layerStack since virtual displays
    // may not have a displayId.
    for (const auto& [token, display] : mDisplays) {
        if (display->getLayerStack() == displayOrLayerStack) {
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
    ui::Transform::orientation_flags captureOrientation;
    {
        Mutex::Autolock _l(mStateLock);
        display = getDisplayByIdOrLayerStack(displayOrLayerStack);
        if (!display) {
            return BAD_VALUE;
        }

        width = uint32_t(display->getViewport().width());
        height = uint32_t(display->getViewport().height());

        captureOrientation = fromSurfaceComposerRotation(
                static_cast<ISurfaceComposer::Rotation>(display->getOrientation()));
        if (captureOrientation == ui::Transform::orientation_flags::ROT_90) {
            captureOrientation = ui::Transform::orientation_flags::ROT_270;
        } else if (captureOrientation == ui::Transform::orientation_flags::ROT_270) {
            captureOrientation = ui::Transform::orientation_flags::ROT_90;
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
                        bool childrenOnly)
              : RenderArea(reqWidth, reqHeight, CaptureFill::CLEAR, reqDataSpace),
                mLayer(layer),
                mCrop(crop),
                mNeedsFiltering(false),
                mFlinger(flinger),
                mChildrenOnly(childrenOnly) {}
        const ui::Transform& getTransform() const override { return mTransform; }
        Rect getBounds() const override {
            const Layer::State& layerState(mLayer->getDrawingState());
            return mLayer->getBufferSize(layerState);
        }
        int getHeight() const override {
            return mLayer->getBufferSize(mLayer->getDrawingState()).getHeight();
        }
        int getWidth() const override {
            return mLayer->getBufferSize(mLayer->getDrawingState()).getWidth();
        }
        bool isSecure() const override { return false; }
        bool needsFiltering() const override { return mNeedsFiltering; }
        const sp<const DisplayDevice> getDisplayDevice() const override { return nullptr; }
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
                newParent->computeBounds(drawingBounds.toFloatRect(), ui::Transform());
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
                Rect bounds = getBounds();
                // In the "childrenOnly" case we reparent the children to a screenshot
                // layer which has no properties set and which does not draw.
                sp<ContainerLayer> screenshotParentLayer =
                        mFlinger->getFactory().createContainerLayer(
                                LayerCreationArgs(mFlinger, nullptr, String8("Screenshot Parent"),
                                                  bounds.getWidth(), bounds.getHeight(), 0,
                                                  LayerMetadata()));

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

    {
        Mutex::Autolock _l(mStateLock);

        parent = fromHandle(layerHandleBinder);
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

        if (sourceCrop.width() <= 0) {
            crop.left = 0;
            crop.right = parent->getBufferSize(parent->getCurrentState()).getWidth();
        }

        if (sourceCrop.height() <= 0) {
            crop.top = 0;
            crop.bottom = parent->getBufferSize(parent->getCurrentState()).getHeight();
        }
        reqWidth = crop.width() * frameScale;
        reqHeight = crop.height() * frameScale;

        for (const auto& handle : excludeHandles) {
            sp<Layer> excludeLayer = fromHandle(handle);
            if (excludeLayer != nullptr) {
                excludeLayers.emplace(excludeLayer);
            } else {
                ALOGW("Invalid layer handle passed as excludeLayer to captureLayers");
                return NAME_NOT_FOUND;
            }
        }
    } // mStateLock

    // really small crop or frameScale
    if (reqWidth <= 0) {
        reqWidth = 1;
    }
    if (reqHeight <= 0) {
        reqHeight = 1;
    }

    LayerRenderArea renderArea(this, parent, crop, reqWidth, reqHeight, reqDataspace, childrenOnly);
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
                               outCapturedSecureLayers);
}

status_t SurfaceFlinger::captureScreenCommon(RenderArea& renderArea,
                                             TraverseLayersFunction traverseLayers,
                                             const sp<GraphicBuffer>& buffer,
                                             bool useIdentityTransform,
                                             bool& outCapturedSecureLayers) {
    // This mutex protects syncFd and captureResult for communication of the return values from the
    // main thread back to this Binder thread
    std::mutex captureMutex;
    std::condition_variable captureCondition;
    std::unique_lock<std::mutex> captureLock(captureMutex);
    int syncFd = -1;
    std::optional<status_t> captureResult;

    const int uid = IPCThreadState::self()->getCallingUid();
    const bool forSystem = uid == AID_GRAPHICS || uid == AID_SYSTEM;

    sp<LambdaMessage> message = new LambdaMessage([&] {
        // If there is a refresh pending, bug out early and tell the binder thread to try again
        // after the refresh.
        if (mRefreshPending) {
            ATRACE_NAME("Skipping screenshot for now");
            std::unique_lock<std::mutex> captureLock(captureMutex);
            captureResult = std::make_optional<status_t>(EAGAIN);
            captureCondition.notify_one();
            return;
        }

        status_t result = NO_ERROR;
        int fd = -1;
        {
            Mutex::Autolock _l(mStateLock);
            renderArea.render([&] {
                result = captureScreenImplLocked(renderArea, traverseLayers, buffer.get(),
                                                 useIdentityTransform, forSystem, &fd,
                                                 outCapturedSecureLayers);
            });
        }

        {
            std::unique_lock<std::mutex> captureLock(captureMutex);
            syncFd = fd;
            captureResult = std::make_optional<status_t>(result);
            captureCondition.notify_one();
        }
    });

    status_t result = postMessageAsync(message);
    if (result == NO_ERROR) {
        captureCondition.wait(captureLock, [&] { return captureResult; });
        while (*captureResult == EAGAIN) {
            captureResult.reset();
            result = postMessageAsync(message);
            if (result != NO_ERROR) {
                return result;
            }
            captureCondition.wait(captureLock, [&] { return captureResult; });
        }
        result = *captureResult;
    }

    if (result == NO_ERROR) {
        sync_wait(syncFd, -1);
        close(syncFd);
    }

    return result;
}

void SurfaceFlinger::renderScreenImplLocked(const RenderArea& renderArea,
                                            TraverseLayersFunction traverseLayers,
                                            ANativeWindowBuffer* buffer, bool useIdentityTransform,
                                            int* outSyncFd) {
    ATRACE_CALL();

    const auto reqWidth = renderArea.getReqWidth();
    const auto reqHeight = renderArea.getReqHeight();
    const auto rotation = renderArea.getRotationFlags();
    const auto transform = renderArea.getTransform();
    const auto sourceCrop = renderArea.getSourceCrop();

    renderengine::DisplaySettings clientCompositionDisplay;
    std::vector<renderengine::LayerSettings> clientCompositionLayers;

    // assume that bounds are never offset, and that they are the same as the
    // buffer bounds.
    clientCompositionDisplay.physicalDisplay = Rect(reqWidth, reqHeight);
    clientCompositionDisplay.clip = sourceCrop;
    clientCompositionDisplay.globalTransform = transform.asMatrix4();

    // Now take into account the rotation flag. We append a transform that
    // rotates the layer stack about the origin, then translate by buffer
    // boundaries to be in the right quadrant.
    mat4 rotMatrix;
    int displacementX = 0;
    int displacementY = 0;
    float rot90InRadians = 2.0f * static_cast<float>(M_PI) / 4.0f;
    switch (rotation) {
        case ui::Transform::ROT_90:
            rotMatrix = mat4::rotate(rot90InRadians, vec3(0, 0, 1));
            displacementX = renderArea.getBounds().getHeight();
            break;
        case ui::Transform::ROT_180:
            rotMatrix = mat4::rotate(rot90InRadians * 2.0f, vec3(0, 0, 1));
            displacementY = renderArea.getBounds().getWidth();
            displacementX = renderArea.getBounds().getHeight();
            break;
        case ui::Transform::ROT_270:
            rotMatrix = mat4::rotate(rot90InRadians * 3.0f, vec3(0, 0, 1));
            displacementY = renderArea.getBounds().getWidth();
            break;
        default:
            break;
    }

    // We need to transform the clipping window into the right spot.
    // First, rotate the clipping rectangle by the rotation hint to get the
    // right orientation
    const vec4 clipTL = vec4(sourceCrop.left, sourceCrop.top, 0, 1);
    const vec4 clipBR = vec4(sourceCrop.right, sourceCrop.bottom, 0, 1);
    const vec4 rotClipTL = rotMatrix * clipTL;
    const vec4 rotClipBR = rotMatrix * clipBR;
    const int newClipLeft = std::min(rotClipTL[0], rotClipBR[0]);
    const int newClipTop = std::min(rotClipTL[1], rotClipBR[1]);
    const int newClipRight = std::max(rotClipTL[0], rotClipBR[0]);
    const int newClipBottom = std::max(rotClipTL[1], rotClipBR[1]);

    // Now reposition the clipping rectangle with the displacement vector
    // computed above.
    const mat4 displacementMat = mat4::translate(vec4(displacementX, displacementY, 0, 1));
    clientCompositionDisplay.clip =
            Rect(newClipLeft + displacementX, newClipTop + displacementY,
                 newClipRight + displacementX, newClipBottom + displacementY);

    mat4 clipTransform = displacementMat * rotMatrix;
    clientCompositionDisplay.globalTransform =
            clipTransform * clientCompositionDisplay.globalTransform;

    clientCompositionDisplay.outputDataspace = renderArea.getReqDataSpace();
    clientCompositionDisplay.maxLuminance = DisplayDevice::sDefaultMaxLumiance;

    const float alpha = RenderArea::getCaptureFillValue(renderArea.getCaptureFill());

    renderengine::LayerSettings fillLayer;
    fillLayer.source.buffer.buffer = nullptr;
    fillLayer.source.solidColor = half3(0.0, 0.0, 0.0);
    fillLayer.geometry.boundaries = FloatRect(0.0, 0.0, 1.0, 1.0);
    fillLayer.alpha = half(alpha);
    clientCompositionLayers.push_back(fillLayer);

    Region clearRegion = Region::INVALID_REGION;
    traverseLayers([&](Layer* layer) {
        const bool supportProtectedContent = false;
        Region clip(renderArea.getBounds());
        compositionengine::LayerFE::ClientCompositionTargetSettings targetSettings{
                clip,
                useIdentityTransform,
                layer->needsFiltering(renderArea.getDisplayDevice()) || renderArea.needsFiltering(),
                renderArea.isSecure(),
                supportProtectedContent,
                clearRegion,
        };
        auto result = layer->prepareClientComposition(targetSettings);
        if (result) {
            clientCompositionLayers.push_back(*result);
        }
    });

    clientCompositionDisplay.clearRegion = clearRegion;
    // Use an empty fence for the buffer fence, since we just created the buffer so
    // there is no need for synchronization with the GPU.
    base::unique_fd bufferFence;
    base::unique_fd drawFence;
    getRenderEngine().useProtectedContext(false);
    getRenderEngine().drawLayers(clientCompositionDisplay, clientCompositionLayers, buffer,
                                 /*useFramebufferCache=*/false, std::move(bufferFence), &drawFence);

    *outSyncFd = drawFence.release();
}

status_t SurfaceFlinger::captureScreenImplLocked(const RenderArea& renderArea,
                                                 TraverseLayersFunction traverseLayers,
                                                 ANativeWindowBuffer* buffer,
                                                 bool useIdentityTransform, bool forSystem,
                                                 int* outSyncFd, bool& outCapturedSecureLayers) {
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
    renderScreenImplLocked(renderArea, traverseLayers, buffer, useIdentityTransform, outSyncFd);
    return NO_ERROR;
}

void SurfaceFlinger::setInputWindowsFinished() {
    Mutex::Autolock _l(mStateLock);

    mPendingSyncInputWindows = false;
    mTransactionCV.broadcast();
}

// ---------------------------------------------------------------------------

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

void SurfaceFlinger::setAllowedDisplayConfigsInternal(const sp<DisplayDevice>& display,
                                                      const std::vector<int32_t>& allowedConfigs) {
    if (!display->isPrimary()) {
        return;
    }

    const auto allowedDisplayConfigs = DisplayConfigs(allowedConfigs.begin(),
                                                      allowedConfigs.end());
    if (allowedDisplayConfigs == mAllowedDisplayConfigs) {
        return;
    }

    ALOGV("Updating allowed configs");
    mAllowedDisplayConfigs = std::move(allowedDisplayConfigs);

    // TODO(b/140204874): This hack triggers a notification that something has changed, so
    // that listeners that care about a change in allowed configs can get the notification.
    // Giving current ActiveConfig so that most other listeners would just drop the event
    mScheduler->onConfigChanged(mAppConnectionHandle, display->getId()->value,
                                display->getActiveConfig());

    // Set the highest allowed config by iterating backwards on available refresh rates
    const auto& refreshRates = mRefreshRateConfigs.getRefreshRates();
    for (auto iter = refreshRates.crbegin(); iter != refreshRates.crend(); ++iter) {
        if (iter->second && isDisplayConfigAllowed(iter->second->configId)) {
            ALOGV("switching to config %d", iter->second->configId);
            setDesiredActiveConfig(
                    {iter->first, iter->second->configId, Scheduler::ConfigEvent::Changed});
            break;
        }
    }
}

status_t SurfaceFlinger::setAllowedDisplayConfigs(const sp<IBinder>& displayToken,
                                                  const std::vector<int32_t>& allowedConfigs) {
    ATRACE_CALL();

    if (!displayToken || allowedConfigs.empty()) {
        return BAD_VALUE;
    }

    if (mDebugDisplayConfigSetByBackdoor) {
        // ignore this request as config is overridden by backdoor
        return NO_ERROR;
    }

    postMessageSync(new LambdaMessage([&]() NO_THREAD_SAFETY_ANALYSIS {
        const auto display = getDisplayDeviceLocked(displayToken);
        if (!display) {
            ALOGE("Attempt to set allowed display configs for invalid display token %p",
                  displayToken.get());
        } else if (display->isVirtual()) {
            ALOGW("Attempt to set allowed display configs for virtual display");
        } else {
            setAllowedDisplayConfigsInternal(display, allowedConfigs);
        }
    }));

    return NO_ERROR;
}

status_t SurfaceFlinger::getAllowedDisplayConfigs(const sp<IBinder>& displayToken,
                                                  std::vector<int32_t>* outAllowedConfigs) {
    ATRACE_CALL();

    if (!displayToken || !outAllowedConfigs) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mStateLock);

    const auto display = getDisplayDeviceLocked(displayToken);
    if (!display) {
        return NAME_NOT_FOUND;
    }

    if (display->isPrimary()) {
        outAllowedConfigs->assign(mAllowedDisplayConfigs.begin(), mAllowedDisplayConfigs.end());
    }

    return NO_ERROR;
}

void SurfaceFlinger::SetInputWindowsListener::onSetInputWindowsFinished() {
    mFlinger->setInputWindowsFinished();
}

sp<Layer> SurfaceFlinger::fromHandle(const sp<IBinder>& handle) {
    BBinder *b = handle->localBinder();
    if (b == nullptr) {
        return nullptr;
    }
    auto it = mLayersByLocalBinderToken.find(b);
    if (it != mLayersByLocalBinderToken.end()) {
        return it->second.promote();
    }
    return nullptr;
}

void SurfaceFlinger::bufferErased(const client_cache_t& clientCacheId) {
    getRenderEngine().unbindExternalTextureBuffer(clientCacheId.id);
}

  // namespace android