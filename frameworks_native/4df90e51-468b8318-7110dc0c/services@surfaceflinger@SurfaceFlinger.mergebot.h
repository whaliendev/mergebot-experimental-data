/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <sys/types.h>
/*
 * NOTE: Make sure this file doesn't include  anything from <gl/ > or <gl2/ >
 */

#include <android-base/thread_annotations.h>
#include <android/gui/BnSurfaceComposer.h>
#include <android/gui/DisplayStatInfo.h>
#include <android/gui/DisplayState.h>
#include <android/gui/ISurfaceComposerClient.h>
#include <cutils/atomic.h>
#include <cutils/compiler.h>
#include <ftl/future.h>
#include <ftl/small_map.h>
#include <gui/BufferQueue.h>
#include <gui/CompositorTiming.h>
#include <gui/FrameTimestamps.h>
#include <gui/ISurfaceComposer.h>
#include <gui/ISurfaceComposerClient.h>
#include <gui/ITransactionCompletedListener.h>
#include <gui/LayerDebugInfo.h>
#include <gui/LayerState.h>
#include <layerproto/LayerProtoHeader.h>
#include <math/mat4.h>
#include <renderengine/LayerSettings.h>
#include <serviceutils/PriorityDumper.h>
#include <system/graphics.h>
#include <ui/FenceTime.h>
#include <ui/PixelFormat.h>
#include <ui/Size.h>
#include <utils/Errors.h>
#include <utils/KeyedVector.h>
#include <utils/RefBase.h>
#include <utils/SortedVector.h>
#include <utils/Trace.h>
#include <utils/threads.h>
#include <compositionengine/FenceResult.h>
#include <compositionengine/OutputColorSetting.h>
#include <scheduler/Fps.h>
#include <scheduler/PresentLatencyTracker.h>
#include <scheduler/Time.h>
#include "ClientCache.h"
#include "DisplayDevice.h"
#include "DisplayHardware/HWC2.h"
#include "DisplayHardware/PowerAdvisor.h"
#include "DisplayIdGenerator.h"
#include "Effects/Daltonizer.h"
#include "FlagManager.h"
#include "FrameTracker.h"
#include "LayerVector.h"
#include "LocklessQueue.h"
#include "Scheduler/RefreshRateConfigs.h"
#include "Scheduler/RefreshRateStats.h"
#include "Scheduler/Scheduler.h"
#include "Scheduler/VsyncModulator.h"
#include "SurfaceFlingerFactory.h"
#include "ThreadContext.h"
#include "TracedOrdinal.h"
#include "Tracing/LayerTracing.h"
#include "Tracing/TransactionTracing.h"
#include "TransactionCallbackInvoker.h"
#include "TransactionState.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <aidl/android/hardware/graphics/common/DisplayDecorationSupport.h>
#include "Client.h"
using namespace android::surfaceflinger;

namespace android {

class EventThread;
class FlagManager;
class FpsReporter;
class TunnelModeEnabledReporter;
class HdrLayerInfoReporter;
class HWComposer;
class IGraphicBufferProducer;
class Layer;
class MessageBase;
class RefreshRateOverlay;
class RegionSamplingThread;
class RenderArea;
class TimeStats;
class FrameTracer;
class ScreenCapturer;
class WindowInfosListenerInvoker;

using gui::CaptureArgs;
using gui::DisplayCaptureArgs;
using gui::IRegionSamplingListener;
using gui::LayerCaptureArgs;
using gui::ScreenCaptureResults;

namespace frametimeline {
class FrameTimeline;
}

namespace os {
class IInputFlinger;
}

namespace compositionengine {
class DisplaySurface;
class OutputLayer;

struct CompositionRefreshArgs;
} // namespace compositionengine

namespace renderengine {
class RenderEngine;
} // namespace renderengine

enum {
    eTransactionNeeded = 0x01,
    eTraversalNeeded = 0x02,
    eDisplayTransactionNeeded = 0x04,
    eTransformHintUpdateNeeded = 0x08,
    eTransactionFlushNeeded = 0x10,
    eTransactionMask = 0x1f,
};

// Latch Unsignaled buffer behaviours
enum class LatchUnsignaledConfig {
    // All buffers are latched signaled.
    Disabled, // Latch unsignaled is permitted when a single layer is updated in a frame,
    // and the update includes just a buffer update (i.e. no sync transactions
    // or geometry changes).
    AutoSingleLayer, // All buffers are latched unsignaled. This behaviour is discouraged as it
    // can break sync transactions, stall the display and cause undesired side effects.
    Always,
};

using DisplayColorSetting = compositionengine::OutputColorSetting;

class SurfaceFlinger : public BnSurfaceComposer,
                       public PriorityDumper,
                       private IBinder::DeathRecipient,
                       private HWC2::ComposerCallback,
                       private ICompositor,
                       private scheduler::ISchedulerCallback {
public:
    struct SkipInitializationTag {};

    ANDROID_API;
    ANDROID_API;
    ANDROID_API;

    ANDROID_API;

    ANDROID_API;

    ANDROID_API;

    // set main thread scheduling attributes
    static status_t setSchedAttr(bool enabled);

    static char const* getServiceName() ANDROID_API { return "SurfaceFlinger"; }

    // If fences from sync Framework are supported.
    static bool hasSyncFramework;

    // The offset in nanoseconds to use when VsyncController timestamps present fence
    // signaling time.
    static int64_t dispSyncPresentTimeOffset;

    // Some hardware can do RGB->YUV conversion more efficiently in hardware
    // controlled by HWC than in hardware controlled by the video encoder.
    // This instruct VirtualDisplaySurface to use HWC for such conversion on
    // GL composition.
    static bool useHwcForRgbToYuv;

    // Controls the number of buffers SurfaceFlinger will allocate for use in
    // FramebufferSurface
    static int64_t maxFrameBufferAcquiredBuffers;

    // Controls the maximum width and height in pixels that the graphics pipeline can support for
    // GPU fallback composition. For example, 8k devices with 4k GPUs, or 4k devices with 2k GPUs.
    static uint32_t maxGraphicsWidth;
    static uint32_t maxGraphicsHeight;

    // Indicate if a device has wide color gamut display. This is typically
    // found on devices with wide color gamut (e.g. Display-P3) display.
    static bool hasWideColorDisplay;

    // Indicate if device wants color management on its display.
    static const constexpr bool useColorManagement = true;

    static bool useContextPriority;

    // The data space and pixel format that SurfaceFlinger expects hardware composer
    // to composite efficiently. Meaning under most scenarios, hardware composer
    // will accept layers with the data space and pixel format.
    static ui::Dataspace defaultCompositionDataspace;
    static ui::PixelFormat defaultCompositionPixelFormat;

    // The data space and pixel format that SurfaceFlinger expects hardware composer
    // to composite efficiently for wide color gamut surfaces. Meaning under most scenarios,
    // hardware composer will accept layers with the data space and pixel format.
    static ui::Dataspace wideColorGamutCompositionDataspace;
    static ui::PixelFormat wideColorGamutCompositionPixelFormat;

    static constexpr SkipInitializationTag SkipInitialization;

    static LatchUnsignaledConfig enableLatchUnsignaledConfig;

    // must be called before clients can connect
    void init() ANDROID_API;

    // starts SurfaceFlinger main loop in the current thread
    void run() ANDROID_API;

    // Indicates frame activity, i.e. whether commit and/or composite is taking place.
    enum class FrameHint { kNone, kActive };

    // Schedule commit of transactions on the main thread ahead of the next VSYNC.
    void scheduleCommit(FrameHint);
    // As above, but also force composite regardless if transactions were committed.
    void scheduleComposite(FrameHint);
    // As above, but also force dirty geometry to repaint.
    void scheduleRepaint();
    // Schedule sampling independently from commit or composite.
    void scheduleSample();

    surfaceflinger::Factory& getFactory() { return mFactory; }

    // The CompositionEngine encapsulates all composition related interfaces and actions.
    compositionengine::CompositionEngine& getCompositionEngine() const;

    // Obtains a name from the texture pool, or, if the pool is empty, posts a
    // synchronous message to the main thread to obtain one on the fly
    uint32_t getNewTexture();

    // utility function to delete a texture on the main thread
    void deleteTextureAsync(uint32_t texture);

    renderengine::RenderEngine& getRenderEngine() const;

    void onLayerFirstRef(Layer*);
    void onLayerDestroyed(Layer*);
    void onLayerUpdate();

    void removeHierarchyFromOffscreenLayers(Layer* layer);
    void removeFromOffscreenLayers(Layer* layer);

    // TODO: Remove atomic if move dtor to main thread CL lands
    std::atomic<uint32_t> mNumClones;

    TransactionCallbackInvoker& getTransactionCallbackInvoker() {
        return mTransactionCallbackInvoker;
    }

    // Converts from a binder handle to a Layer
    // Returns nullptr if the handle does not point to an existing layer.
    // Otherwise, returns a weak reference so that callers off the main-thread
    // won't accidentally hold onto the last strong reference.
    wp<Layer> fromHandle(const sp<IBinder>& handle) const;

    // If set, disables reusing client composition buffers. This can be set by
    // debug.sf.disable_client_composition_cache
    bool mDisableClientCompositionCache = false;
    // Disables expensive rendering for all displays
    // This is scheduled on the main thread
    void disableExpensiveRendering();
    FloatRect getMaxDisplayBounds();

    // If set, composition engine tries to predict the composition strategy provided by HWC
    // based on the previous frame. If the strategy can be predicted, gpu composition will
    // run parallel to the hwc validateDisplay call and re-run if the predition is incorrect.
    bool mPredictCompositionStrategy = false;

    // If true, then any layer with a SMPTE 170M transfer function is decoded using the sRGB
    // transfer instead. This is mainly to preserve legacy behavior, where implementations treated
    // SMPTE 170M as sRGB prior to color management being implemented, and now implementations rely
    // on this behavior to increase contrast for some media sources.
    bool mTreat170mAsSrgb = false;

protected:
    // We're reference counted, never destroy SurfaceFlinger directly
    virtual ~SurfaceFlinger();

    REQUIRES(mStateLock);

    REQUIRES(mStateLock);

    virtual std::shared_ptr<renderengine::ExternalTexture> getExternalTextureFromBufferData(
            const BufferData& bufferData, const char* layerName) const;

    REQUIRES(mStateLock) { return static_cast<bool>(findDisplay(p)); }

    bool exceedsMaxRenderTargetSize(uint32_t width, uint32_t height) const {
        return width > mMaxRenderTargetSize || height > mMaxRenderTargetSize;
    }

private:
    friend class BufferLayer;
    friend class BufferStateLayer;
    friend class Client;
    friend class FpsReporter;
    friend class TunnelModeEnabledReporter;
    friend class Layer;
    friend class RefreshRateOverlay;
    friend class RegionSamplingThread;
    friend class LayerRenderArea;
    friend class LayerTracing;

    // For unit tests
    friend class TestableSurfaceFlinger;
    friend class TransactionApplicationTest;
    friend class TunnelModeEnabledReporterTest;

    using VsyncModulator = scheduler::VsyncModulator;
    using TransactionSchedule = scheduler::TransactionSchedule;
    using TraverseLayersFunction = std::function<void(const LayerVector::Visitor&)>;
    using RenderAreaFuture = ftl::Future<std::unique_ptr<RenderArea>>;
    using DumpArgs = Vector<String16>;
    using Dumper = std::function<void(const DumpArgs&, bool asProto, std::string&)>;

    class State {
    public:
        explicit State(LayerVector::StateSet set) : stateSet(set), layersSortedByZ(set) {}
        State& operator=(const State& other) {
            // We explicitly don't copy stateSet so that, e.g., mDrawingState
            // always uses the Drawing StateSet.
            layersSortedByZ = other.layersSortedByZ;
            displays = other.displays;
            colorMatrixChanged = other.colorMatrixChanged;
            if (colorMatrixChanged) {
                colorMatrix = other.colorMatrix;
            }
            globalShadowSettings = other.globalShadowSettings;

            return *this;
        }

        const LayerVector::StateSet stateSet = LayerVector::StateSet::Invalid;
        LayerVector layersSortedByZ;
        DefaultKeyedVector<wp<IBinder>, DisplayDeviceState> displays;

        bool colorMatrixChanged = true;
        mat4 colorMatrix;

        renderengine::ShadowSettings globalShadowSettings;

        void traverse(const LayerVector::Visitor& visitor) const;
        void traverseInZOrder(const LayerVector::Visitor& visitor) const;
        void traverseInReverseZOrder(const LayerVector::Visitor& visitor) const;
    };

    // Keeps track of pending buffers per layer handle in the transaction queue or current/drawing
    // state before the buffers are latched. The layer owns the atomic counters and decrements the
    // count in the main thread when dropping or latching a buffer.
    //
    // The binder threads increment the same counter when a new transaction containing a buffer is
    // added to the transaction queue. The map is updated with the layer handle lifecycle updates.
    // This is done to avoid lock contention with the main thread.
    class BufferCountTracker {
    public:
        void increment(BBinder* layerHandle) {
            std::lock_guard<std::mutex> lock(mLock);
            auto it = mCounterByLayerHandle.find(layerHandle);
            if (it != mCounterByLayerHandle.end()) {
                auto [name, pendingBuffers] = it->second;
                int32_t count = ++(*pendingBuffers);
                ATRACE_INT(name.c_str(), count);
            } else {
                ALOGW("Handle not found! %p", layerHandle);
            }
        }

        void add(BBinder* layerHandle, const std::string& name, std::atomic<int32_t>* counter) {
            std::lock_guard<std::mutex> lock(mLock);
            mCounterByLayerHandle[layerHandle] = std::make_pair(name, counter);
        }

        void remove(BBinder* layerHandle) {
            std::lock_guard<std::mutex> lock(mLock);
            mCounterByLayerHandle.erase(layerHandle);
        }

    private:
        std::mutex mLock;
        std::unordered_map<BBinder*, std::pair<std::string, std::atomic<int32_t>*>>
                mCounterByLayerHandle GUARDED_BY(mLock);
    };

    using ActiveModeInfo = DisplayDevice::ActiveModeInfo;
    using KernelIdleTimerController =
            ::android::scheduler::RefreshRateConfigs::KernelIdleTimerController;

    enum class BootStage {
        BOOTLOADER,
        BOOTANIMATION,
        FINISHED,
    };

    struct HotplugEvent {
        hal::HWDisplayId hwcDisplayId;
        hal::Connection connection = hal::Connection::INVALID;
    };

    static Dumper dumper(F&& dump) {
        using namespace std::placeholders;
        return std::bind(std::forward<F>(dump), _3);
    }

    Dumper dumper(F dump) {
        using namespace std::placeholders;
        return std::bind(dump, this, _3);
    }

    template <typename F>
    Dumper argsDumper(F dump) {
        using namespace std::placeholders;
        return std::bind(dump, this, _1, _3);
    }

    template <typename F>
    Dumper protoDumper(F dump) {
        using namespace std::placeholders;
        return std::bind(dump, this, _1, _2, _3);
    }

    template <typename... Args,
              typename Handler = VsyncModulator::VsyncConfigOpt (VsyncModulator::*)(Args...)>
    void modulateVsync(Handler handler, Args... args) {
        if (const auto config = (*mVsyncModulator.*handler)(args...)) {
            const auto vsyncPeriod = mScheduler->getVsyncPeriodFromRefreshRateConfigs();
            setVsyncConfig(*config, vsyncPeriod);
        }
    }

    // Maximum allowed number of display frames that can be set through backdoor
    static const int MAX_ALLOWED_DISPLAY_FRAMES = 2048;

    static const size_t MAX_LAYERS = 4096;

    // Implements IBinder.
    status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override;
    status_t dump(int fd, const Vector<String16>& args) override { return priorityDump(fd, args); }
    EXCLUDES(mStateLock);

    EXCLUDES(mStateLock);

    // Implements ISurfaceComposer
    sp<IBinder> createDisplay(const String8& displayName, bool secure);
    void destroyDisplay(const sp<IBinder>& displayToken);
    std::vector<PhysicalDisplayId> getPhysicalDisplayIds() const EXCLUDES(mStateLock) {
        Mutex::Autolock lock(mStateLock);
        return getPhysicalDisplayIdsLocked();
    }
    sp<IBinder> getPhysicalDisplayToken(PhysicalDisplayId displayId) const;
    status_t setTransactionState(const FrameTimelineInfo& frameTimelineInfo,
                                 const Vector<ComposerState>& state,
                                 const Vector<DisplayState>& displays, uint32_t flags,
                                 const sp<IBinder>& applyToken,
                                 const InputWindowCommands& inputWindowCommands,
                                 int64_t desiredPresentTime, bool isAutoTimestamp,
                                 const client_cache_t& uncacheBuffer, bool hasListenerCallbacks,
                                 const std::vector<ListenerCallbacks>& listenerCallbacks,
                                 uint64_t transactionId) override;
    void bootFinished();
    virtual status_t getSupportedFrameTimestamps(std::vector<FrameEvent>* outSupported) const;
    sp<IDisplayEventConnection> createDisplayEventConnection(
            gui::ISurfaceComposer::VsyncSource vsyncSource =
                    gui::ISurfaceComposer::VsyncSource::eVsyncSourceApp,
            EventRegistrationFlags eventRegistration = {});

    status_t captureDisplay(const DisplayCaptureArgs&, const sp<IScreenCaptureListener>&);
    status_t captureDisplay(DisplayId, const sp<IScreenCaptureListener>&);
    status_t captureLayers(const LayerCaptureArgs&, const sp<IScreenCaptureListener>&);

    status_t getDisplayStats(const sp<IBinder>& displayToken, DisplayStatInfo* stats);
    EXCLUDES(mStateLock);
    EXCLUDES(mStateLock);
    EXCLUDES(mStateLock) override;
    EXCLUDES(mStateLock) override;
    EXCLUDES(mStateLock) override;
    EXCLUDES(mStateLock) override;
    status_t getDisplayNativePrimaries(const sp<IBinder>& displayToken, ui::DisplayPrimaries&);
    status_t setActiveColorMode(const sp<IBinder>& displayToken, ui::ColorMode colorMode);
    status_t getBootDisplayModeSupport(bool* outSupport) const;
    status_t setBootDisplayMode(const sp<IBinder>& displayToken, ui::DisplayModeId id);
    status_t clearBootDisplayMode(const sp<IBinder>& displayToken);
    void setAutoLowLatencyMode(const sp<IBinder>& displayToken, bool on);
    void setGameContentType(const sp<IBinder>& displayToken, bool on);
    void setPowerMode(const sp<IBinder>& displayToken, int mode);
    status_t overrideHdrTypes(const sp<IBinder>& displayToken,
                              const std::vector<ui::Hdr>& hdrTypes);
    status_t onPullAtom(const int32_t atomId, std::string* pulledData, bool* success);
    status_t enableVSyncInjections(bool enable);
    status_t injectVSync(nsecs_t when);
    status_t getLayerDebugInfo(std::vector<gui::LayerDebugInfo>* outLayers);
    status_t getColorManagement(bool* outGetColorManagement) const;
    status_t getCompositionPreference(ui::Dataspace* outDataspace, ui::PixelFormat* outPixelFormat,
                                      ui::Dataspace* outWideColorGamutDataspace,
                                      ui::PixelFormat* outWideColorGamutPixelFormat) const;
    status_t getDisplayedContentSamplingAttributes(const sp<IBinder>& displayToken,
                                                   ui::PixelFormat* outFormat,
                                                   ui::Dataspace* outDataspace,
                                                   uint8_t* outComponentMask) const;
    status_t setDisplayContentSamplingEnabled(const sp<IBinder>& displayToken, bool enable,
                                              uint8_t componentMask, uint64_t maxFrames);
    status_t getDisplayedContentSample(const sp<IBinder>& displayToken, uint64_t maxFrames,
                                       uint64_t timestamp, DisplayedFrameStats* outStats) const;
    status_t getProtectedContentSupport(bool* outSupported) const;
    status_t isWideColorDisplay(const sp<IBinder>& displayToken, bool* outIsWideColorDisplay) const;
    status_t addRegionSamplingListener(const Rect& samplingArea, const sp<IBinder>& stopLayerHandle,
                                       const sp<IRegionSamplingListener>& listener);
    status_t removeRegionSamplingListener(const sp<IRegionSamplingListener>& listener);
    status_t addFpsListener(int32_t taskId, const sp<gui::IFpsListener>& listener);
    status_t removeFpsListener(const sp<gui::IFpsListener>& listener);
    status_t addTunnelModeEnabledListener(const sp<gui::ITunnelModeEnabledListener>& listener);
    status_t removeTunnelModeEnabledListener(const sp<gui::ITunnelModeEnabledListener>& listener);
    status_t setDesiredDisplayModeSpecs(const sp<IBinder>& displayToken,
                                        ui::DisplayModeId displayModeId, bool allowGroupSwitching,
                                        float primaryRefreshRateMin, float primaryRefreshRateMax,
                                        float appRequestRefreshRateMin,
                                        float appRequestRefreshRateMax);
    status_t getDesiredDisplayModeSpecs(const sp<IBinder>& displayToken,
                                        ui::DisplayModeId* outDefaultMode,
                                        bool* outAllowGroupSwitching,
                                        float* outPrimaryRefreshRateMin,
                                        float* outPrimaryRefreshRateMax,
                                        float* outAppRequestRefreshRateMin,
                                        float* outAppRequestRefreshRateMax);
    status_t getDisplayBrightnessSupport(const sp<IBinder>& displayToken, bool* outSupport) const;
    status_t setDisplayBrightness(const sp<IBinder>& displayToken,
                                  const gui::DisplayBrightness& brightness);
    status_t addHdrLayerInfoListener(const sp<IBinder>& displayToken,
                                     const sp<gui::IHdrLayerInfoListener>& listener);
    status_t removeHdrLayerInfoListener(const sp<IBinder>& displayToken,
                                        const sp<gui::IHdrLayerInfoListener>& listener);
    status_t notifyPowerBoost(int32_t boostId);
    status_t setGlobalShadowSettings(const half4& ambientColor, const half4& spotColor,
                                     float lightPosY, float lightPosZ, float lightRadius);
    status_t getDisplayDecorationSupport(
            const sp<IBinder>& displayToken,
            std::optional<aidl::android::hardware::graphics::common::DisplayDecorationSupport>*
                    outSupport) const;
    status_t setFrameRate(const sp<IGraphicBufferProducer>& surface, float frameRate,
                          int8_t compatibility, int8_t changeFrameRateStrategy);

    status_t setFrameTimelineInfo(const sp<IGraphicBufferProducer>& surface,
                                  const gui::FrameTimelineInfo& frameTimelineInfo);

    status_t setOverrideFrameRate(uid_t uid, float frameRate);

    status_t addTransactionTraceListener(const sp<gui::ITransactionTraceListener>& listener);

    int getGpuContextPriority();

    status_t getMaxAcquiredBufferCount(int* buffers) const;

    status_t addWindowInfosListener(const sp<gui::IWindowInfosListener>& windowInfosListener) const;
    status_t removeWindowInfosListener(
            const sp<gui::IWindowInfosListener>& windowInfosListener) const;

    // Implements IBinder::DeathRecipient.
    void binderDied(const wp<IBinder>& who) override;

    // HWC2::ComposerCallback overrides:
    void onComposerHalVsync(hal::HWDisplayId, int64_t timestamp,
                            std::optional<hal::VsyncPeriodNanos>) override;
    void onComposerHalHotplug(hal::HWDisplayId, hal::Connection) override;
    void onComposerHalRefresh(hal::HWDisplayId) override;
    void onComposerHalVsyncPeriodTimingChanged(hal::HWDisplayId,
                                               const hal::VsyncPeriodChangeTimeline&) override;
    void onComposerHalSeamlessPossible(hal::HWDisplayId) override;
    void onComposerHalVsyncIdle(hal::HWDisplayId) override;

    // ICompositor overrides:
    // Commits transactions for layers and displays. Returns whether any state has been invalidated,
    // i.e. whether a frame should be composited for each display.
    bool commit(TimePoint frameTime, VsyncId, TimePoint expectedVsyncTime) override;

    // Composites a frame for each display. CompositionEngine performs GPU and/or HAL composition
    // via RenderEngine and the Composer HAL, respectively.
    void composite(TimePoint frameTime, VsyncId) override;

    // Samples the composited frame via RegionSamplingThread.
    void sample() override;

    /*
     * ISchedulerCallback
     */
    // Toggles hardware VSYNC by calling into HWC.
    void setVsyncEnabled(bool) override;
    // Sets the desired display mode if allowed by policy.
    void requestDisplayMode(DisplayModePtr, DisplayModeEvent) override;
    // Called when kernel idle timer has expired. Used to update the refresh rate overlay.
    void kernelTimerChanged(bool expired) override;
    // Called when the frame rate override list changed to trigger an event.
    void triggerOnFrameRateOverridesChanged() override;
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    // Keeps track of whether the kernel idle timer is currently enabled, so we don't have to
    // make calls to sys prop each time.
    bool mKernelIdleTimerEnabled = false;
    // Show spinner with refresh rate overlay
    bool mRefreshRateOverlaySpinner = false;

    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    status_t setActiveModeFromBackdoor(const sp<IBinder>& displayToken, int id);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);

    REQUIRES(mStateLock);

    REQUIRES(mStateLock);

    REQUIRES(mStateLock);

    EXCLUDES(mStateLock);

    EXCLUDES(mStateLock);

    EXCLUDES(mStateLock);
    EXCLUDES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);

    REQUIRES(mStateLock);

    // Returns whether a new buffer has been latched.
    bool latchBuffers();

    void updateLayerGeometry();

    void updateInputFlinger();
    REQUIRES(kMainThreadContext);
    REQUIRES(kMainThreadContext);
    void buildWindowInfos(std::vector<gui::WindowInfo>& outWindowInfos,
                          std::vector<gui::DisplayInfo>& outDisplayInfos);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    void updateCursorAsync();

    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    void setVsyncConfig(const VsyncModulator::VsyncConfig&, nsecs_t vsyncPeriod);

    REQUIRES(mStateLock);
    REQUIRES(kMainThreadContext);
    REQUIRES(mStateLock);
    // Returns true if there is at least one transaction that needs to be flushed
    bool transactionFlushNeeded();

    REQUIRES(mStateLock, mQueueLock);

    REQUIRES(mStateLock, mQueueLock);

    REQUIRES(mStateLock);

    REQUIRES(mStateLock);

    uint32_t getTransactionFlags() const;

    // Sets the masked bits, and schedules a commit if needed.
    void setTransactionFlags(uint32_t mask, TransactionSchedule = TransactionSchedule::Late,
                             const sp<IBinder>& applyToken = nullptr,
                             FrameHint = FrameHint::kActive);

    // Clears and returns the masked bits.
    uint32_t clearTransactionFlags(uint32_t mask);

    void commitOffscreenLayers();

    enum class TransactionReadiness {
        NotReady,
        NotReadyBarrier,
        Ready,
        ReadyUnsignaled,
    };
    REQUIRES(kMainThreadContext);

    static LatchUnsignaledConfig getLatchUnsignaledConfig();
    bool shouldLatchUnsignaled(const sp<Layer>& layer, const layer_state_t&, size_t numStates,
                               size_t totalTXapplied) const;
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    bool frameIsEarly(TimePoint expectedPresentTime, VsyncId) const;

    REQUIRES(mStateLock);
    /*
     * Layer management
     */
    status_t createLayer(LayerCreationArgs& args, sp<IBinder>* outHandle,
                         const sp<IBinder>& parentHandle, int32_t* outLayerId,
                         const sp<Layer>& parentLayer = nullptr,
                         uint32_t* outTransformHint = nullptr);

    status_t createBufferStateLayer(LayerCreationArgs& args, sp<IBinder>* outHandle,
                                    sp<Layer>* outLayer);

    status_t createEffectLayer(const LayerCreationArgs& args, sp<IBinder>* outHandle,
                               sp<Layer>* outLayer);

    status_t mirrorLayer(const LayerCreationArgs& args, const sp<IBinder>& mirrorFromHandle,
                         sp<IBinder>* outHandle, int32_t* outLayerId);

    status_t mirrorDisplay(DisplayId displayId, const LayerCreationArgs& args,
                           sp<IBinder>* outHandle, int32_t* outLayerId);

    // called when all clients have released all their references to
    // this layer meaning it is entirely safe to destroy all
    // resources associated to this layer.
    void onHandleDestroyed(BBinder* handle, sp<Layer>& layer);
    void markLayerPendingRemovalLocked(const sp<Layer>& layer);

    // add a layer to SurfaceFlinger
    status_t addClientLayer(const sp<Client>& client, const sp<IBinder>& handle,
                            const sp<Layer>& lbc, const wp<Layer>& parentLayer, bool addToRoot,
                            uint32_t* outTransformHint);

    // Traverse through all the layers and compute and cache its bounds.
    void computeLayerBounds();

    // Boot animation, on/off animations and screen capture
    void startBootAnim();

    ftl::SharedFuture<FenceResult> captureScreenCommon(RenderAreaFuture, TraverseLayersFunction,
                                                       ui::Size bufferSize, ui::PixelFormat,
                                                       bool allowProtected, bool grayscale,
                                                       const sp<IScreenCaptureListener>&);
    ftl::SharedFuture<FenceResult> captureScreenCommon(
            RenderAreaFuture, TraverseLayersFunction,
            const std::shared_ptr<renderengine::ExternalTexture>&, bool regionSampling,
            bool grayscale, const sp<IScreenCaptureListener>&);
    EXCLUDES(mStateLock);

    EXCLUDES(mStateLock);

    // If the uid provided is not UNSET_UID, the traverse will skip any layers that don't have a
    // matching ownerUid
    void traverseLayersInLayerStack(ui::LayerStack, const int32_t uid, const LayerVector::Visitor&);

    void readPersistentProperties();

    uint32_t getMaxAcquiredBufferCountForCurrentRefreshRate(uid_t uid) const;

    /*
     * Display and layer stack management
     */
    // called when starting, or restarting after system_server death
    void initializeDisplays();

    sp<const DisplayDevice> getDisplayDeviceLocked(const wp<IBinder>& displayToken) const
            REQUIRES(mStateLock) {
        return const_cast<SurfaceFlinger*>(this)->getDisplayDeviceLocked(displayToken);
    }

    sp<DisplayDevice> getDisplayDeviceLocked(const wp<IBinder>& displayToken) REQUIRES(mStateLock) {
        const sp<DisplayDevice> nullDisplay;
        return mDisplays.get(displayToken).value_or(std::cref(nullDisplay));
    }

    sp<const DisplayDevice> getDisplayDeviceLocked(PhysicalDisplayId id) const
            REQUIRES(mStateLock) {
        return const_cast<SurfaceFlinger*>(this)->getDisplayDeviceLocked(id);
    }

    sp<DisplayDevice> getDisplayDeviceLocked(PhysicalDisplayId id) REQUIRES(mStateLock) {
        if (const auto token = getPhysicalDisplayTokenLocked(id)) {
            return getDisplayDeviceLocked(token);
        }
        return nullptr;
    }

    sp<const DisplayDevice> getDisplayDeviceLocked(DisplayId id) const REQUIRES(mStateLock) {
        // TODO(b/182939859): Replace tokens with IDs for display lookup.
        return findDisplay([id](const auto& display) { return display.getId() == id; });
    }

    // Returns the primary display or (for foldables) the active display, assuming that the inner
    // and outer displays have mutually exclusive power states.
    sp<const DisplayDevice> getDefaultDisplayDeviceLocked() const REQUIRES(mStateLock) {
        return const_cast<SurfaceFlinger*>(this)->getDefaultDisplayDeviceLocked();
    }

    sp<DisplayDevice> getDefaultDisplayDeviceLocked() REQUIRES(mStateLock) {
        if (const auto display = getDisplayDeviceLocked(mActiveDisplayToken)) {
            return display;
        }
        // The active display is outdated, so fall back to the primary display.
        mActiveDisplayToken.clear();
        return getDisplayDeviceLocked(getPrimaryDisplayTokenLocked());
    }

    sp<const DisplayDevice> getDefaultDisplayDevice() const EXCLUDES(mStateLock) {
        Mutex::Autolock lock(mStateLock);
        return getDefaultDisplayDeviceLocked();
    }

    REQUIRES(mStateLock) {
        const auto it = std::find_if(mDisplays.begin(), mDisplays.end(),
                                     [&](const auto& pair) { return p(*pair.second); });

        return it == mDisplays.end() ? nullptr : it->second;
    }

    REQUIRES(mStateLock);

    REQUIRES(mStateLock);

    // mark a region of a layer stack dirty. this updates the dirty
    // region of all screens presenting this layer stack.
    void invalidateLayerStack(const sp<const Layer>& layer, const Region& dirty);

    bool isDisplayActiveLocked(const sp<const DisplayDevice>& display) const REQUIRES(mStateLock) {
        return display->getDisplayToken() == mActiveDisplayToken;
    }

    /*
     * H/W composer
     */
    // The following thread safety rules apply when accessing HWComposer:
    // 1. When reading display state from HWComposer on the main thread, it's not necessary to
    //    acquire mStateLock.
    // 2. When accessing HWComposer on a thread other than the main thread, we always
    //    need to acquire mStateLock. This is because the main thread could be
    //    in the process of writing display state, e.g. creating or destroying a display.
    HWComposer& getHwComposer() const;

    REQUIRES(kMainThreadContext);
    REQUIRES(mStateLock);

    REQUIRES(mStateLock);

    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);

    REQUIRES(mStateLock);

    void dispatchDisplayHotplugEvent(PhysicalDisplayId displayId, bool connected);

    REQUIRES(mStateLock);

    REQUIRES(mStateLock);

    void setHWCVsyncEnabled(PhysicalDisplayId id, hal::Vsync enabled) {
        mLastHWCVsyncState = enabled;
        getHwComposer().setVsyncEnabled(id, enabled);
    }

    using FenceTimePtr = std::shared_ptr<FenceTime>;

    REQUIRES(kMainThreadContext);

    // Blocks the thread waiting for up to graceTimeMs in case the fence is about to signal.
    static bool isFencePending(const FenceTimePtr&, int graceTimeMs);

    // Calculates the expected present time for this frame. For negative offsets, performs a
    // correction using the predicted vsync for the next frame instead.
    TimePoint calculateExpectedPresentTime(TimePoint frameTime) const;

    struct FenceWithFenceTime {
        sp<Fence> fence = Fence::NO_FENCE;
        FenceTimePtr fenceTime = FenceTime::NO_FENCE;
    };

    /*
     * Display identification
     */
    sp<IBinder> getPhysicalDisplayTokenLocked(PhysicalDisplayId displayId) const
            REQUIRES(mStateLock) {
        const sp<IBinder> nullToken;
        return mPhysicalDisplayTokens.get(displayId).value_or(std::cref(nullToken));
    }

    std::optional<PhysicalDisplayId> getPhysicalDisplayIdLocked(
            const sp<IBinder>& displayToken) const REQUIRES(mStateLock) {
        for (const auto& [id, token] : mPhysicalDisplayTokens) {
            if (token == displayToken) {
                return id;
            }
        }
        return {};
    }

    // Returns the first display connected at boot.
    //
    // TODO(b/229851933): SF conflates the primary display with the first display connected at boot,
    // which typically has DisplayConnectionType::Internal. (Theoretically, it must be an internal
    // display because SF does not support disconnecting it, though in practice HWC may circumvent
    // this limitation.)
    sp<IBinder> getPrimaryDisplayTokenLocked() const REQUIRES(mStateLock) {
        return getPhysicalDisplayTokenLocked(getPrimaryDisplayIdLocked());
    }

    PhysicalDisplayId getPrimaryDisplayIdLocked() const REQUIRES(mStateLock) {
        return getHwComposer().getPrimaryDisplayId();
    }

    // Toggles use of HAL/GPU virtual displays.
    void enableHalVirtualDisplays(bool);

    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    void releaseVirtualDisplay(VirtualDisplayId);

    REQUIRES(mStateLock);

    REQUIRES(mStateLock);

    void onActiveDisplaySizeChanged(const sp<DisplayDevice>& activeDisplay);

    REQUIRES(mStateLock);

    REQUIRES(mStateLock);

    REQUIRES(mStateLock);

    void appendSfConfigString(std::string& result) const;
    void listLayersLocked(std::string& result) const;
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    void clearStatsLocked(const DumpArgs& args, std::string& result);
    void dumpTimeStats(const DumpArgs& args, bool asProto, std::string& result) const;
    void dumpFrameTimeline(const DumpArgs& args, std::string& result) const;
    REQUIRES(kMainThreadContext);

    REQUIRES(kMainThreadContext);

    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    REQUIRES(mStateLock);
    void dumpRawDisplayIdentificationData(const DumpArgs&, std::string& result) const;
    REQUIRES(mStateLock);

    REQUIRES(mStateLock);

    LayersProto dumpDrawingStateProto(uint32_t traceFlags) const;
    void dumpOffscreenLayersProto(LayersProto& layersProto,
                                  uint32_t traceFlags = LayerTracing::TRACE_ALL) const;
    void dumpDisplayProto(LayersTraceProto& layersTraceProto) const;

    // Dumps state from HW Composer
    void dumpHwc(std::string& result) const;
    EXCLUDES(mStateLock);
    EXCLUDES(mStateLock);
    EXCLUDES(mStateLock);
    EXCLUDES(mStateLock);
    REQUIRES(mStateLock);

    REQUIRES(mStateLock);

    status_t doDump(int fd, const DumpArgs& args, bool asProto);

    status_t dumpCritical(int fd, const DumpArgs&, bool asProto);

    status_t dumpAll(int fd, const DumpArgs& args, bool asProto) override {
        return doDump(fd, args, asProto);
    }

    static mat4 calculateColorMatrix(float saturation);

    void updateColorMatrixLocked();

    // Verify that transaction is being called by an approved process:
    // either AID_GRAPHICS or AID_SYSTEM.
    status_t CheckTransactCodeCredentials(uint32_t code);

    EXCLUDES(mQueueLock);
    EXCLUDES(mQueueLock);
    void waitForSynchronousTransaction(const CountDownLatch& transactionCommittedSignal);
    void signalSynchronousTransactions(const uint32_t flag);

    /*
     * Generic Layer Metadata
     */
    const std::unordered_map<std::string, uint32_t>& getGenericLayerMetadataKeyMap() const;

    REQUIRES(mStateLock);

    REQUIRES(mStateLock);

    static int calculateMaxAcquiredBufferCount(Fps refreshRate,
                                               std::chrono::nanoseconds presentLatency);
    int getMaxAcquiredBufferCountForRefreshRate(Fps refreshRate) const;

    REQUIRES(mStateLock);

    REQUIRES(mStateLock);

    bool isHdrLayer(Layer* layer) const;

    REQUIRES(mStateLock);

    REQUIRES(mStateLock);

    sp<StartPropertySetThread> mStartPropertySetThread;
    surfaceflinger::Factory& mFactory;
    pid_t mPid;
    std::future<void> mRenderEnginePrimeCacheFuture;

    // access must be protected by mStateLock
    mutable Mutex mStateLock;
    State mCurrentState{LayerVector::StateSet::Current};
    std::atomic<int32_t> mTransactionFlags = 0;
    std::vector<std::shared_ptr<CountDownLatch>> mTransactionCommittedSignals;
    std::atomic<uint32_t> mUniqueTransactionId = 1;
    SortedVector<sp<Layer>> mLayersPendingRemoval;

    // global color transform states
    Daltonizer mDaltonizer;
    float mGlobalSaturationFactor = 1.0f;
    mat4 mClientColorMatrix;

    size_t mMaxGraphicBufferProducerListSize = MAX_LAYERS;
    // If there are more GraphicBufferProducers tracked by SurfaceFlinger than
    // this threshold, then begin logging.
    size_t mGraphicBufferProducerListSizeLogThreshold =
            static_cast<size_t>(0.95 * static_cast<double>(MAX_LAYERS));

    // protected by mStateLock (but we could use another lock)
    bool mLayersRemoved = false;
    bool mLayersAdded = false;

    std::atomic_bool mMustComposite = false;
    std::atomic_bool mGeometryDirty = false;

    // constant members (no synchronization needed for access)
    const nsecs_t mBootTime = systemTime();
    bool mGpuToCpuSupported = false;
    bool mIsUserBuild = true;

    // Can only accessed from the main thread, these members
    // don't need synchronization
    State mDrawingState{LayerVector::StateSet::Drawing};
    bool mVisibleRegionsDirty = false;

    // VisibleRegions dirty is already cleared by postComp, but we need to track it to prevent
    // extra work in the HDR layer info listener.
    bool mVisibleRegionsWereDirtyThisFrame = false;
    // Used to ensure we omit a callback when HDR layer info listener is newly added but the
    // scene hasn't changed
    bool mAddingHDRLayerInfoListener = false;
    bool mIgnoreHdrCameraLayers = false;

    // Set during transaction application stage to track if the input info or children
    // for a layer has changed.
    // TODO: Also move visibleRegions over to a boolean system.
    bool mUpdateInputInfo = false;
    bool mSomeChildrenChanged;
    bool mSomeDataspaceChanged = false;
    bool mForceTransactionDisplayChange = false;

    // Tracks layers that have pending frames which are candidates for being
    // latched.
    std::unordered_set<sp<Layer>, SpHash<Layer>> mLayersWithQueuedFrames;
    // Tracks layers that need to update a display's dirty region.
    std::vector<sp<Layer>> mLayersPendingRefresh;
    // size should be longest sf-duration / shortest vsync period and round up
    std::array<FenceWithFenceTime, 5>
            mPreviousPresentFences; // currently consider 166hz.
                                    // True if in the previous frame at least one layer was composed
                                    // via the GPU.
    bool mHadClientComposition = false;
    // True if in the previous frame at least one layer was composed via HW Composer.
    // Note that it is possible for a frame to be composed via both client and device
    // composition, for example in the case of overlays.
    bool mHadDeviceComposition = false;
    // True if in the previous frame, the client composition was skipped by reusing the buffer
    // used in a previous composition. This can happed if the client composition requests
    // did not change.
    bool mReusedClientComposition = false;

    BootStage mBootStage = BootStage::BOOTLOADER;

    std::vector<HotplugEvent> mPendingHotplugEventsvoid SurfaceFlinger::composite(nsecs_t frameTime,
                                                                                  int64_t vsyncId)

            bool commitMirrorDisplays(VsyncId);

    void SurfaceFlinger::composite(nsecs_t frameTime, int64_t vsyncId)

            ftl::SmallMap<PhysicalDisplayId, const sp<IBinder>, 3> mPhysicalDisplayTokens
            void SurfaceFlinger::composite(nsecs_t frameTime, int64_t vsyncId)

                    struct {
        DisplayIdGenerator<GpuVirtualDisplayId> gpu;
        std::optional<DisplayIdGenerator<HalVirtualDisplayId>> hal;
    } mVirtualDisplayIdGenerators;

    std::atomic_uint mDebugFlashDelay = 0;
    std::atomic_bool mDebugDisableHWC = false;
    std::atomic_bool mDebugDisableTransformHint = false;
    std::atomic<nsecs_t> mDebugInTransaction = 0;
    std::atomic_bool mForceFullDamage = false;

    bool mLayerCachingEnabled = false;
    bool mPropagateBackpressureClientComposition = false;
    sp<SurfaceInterceptor> mInterceptor;

    LayerTracing mLayerTracing{*this};
    bool mLayerTracingEnabled = false;

    std::optional<TransactionTracing> mTransactionTracing;
    std::atomic<bool> mTracingEnabledChanged = false;

    const std::shared_ptr<TimeStats> mTimeStats;
    const std::unique_ptr<FrameTracer> mFrameTracer;
    const std::unique_ptr<frametimeline::FrameTimeline> mFrameTimeline;

    VsyncId mLastCommittedVsyncId;

    // If blurs should be enabled on this device.
    bool mSupportsBlur = false;
    // If blurs are considered expensive and should require high GPU frequency.
    bool mBlursAreExpensive = false;
    std::atomic<uint32_t> mFrameMissedCount = 0;
    std::atomic<uint32_t> mHwcFrameMissedCount = 0;
    std::atomic<uint32_t> mGpuFrameMissedCount = 0;

    TransactionCallbackInvoker mTransactionCallbackInvoker;

    // We maintain a pool of pre-generated texture names to hand out to avoid
    // layer creation needing to run on the main thread (which it would
    // otherwise need to do to access RenderEngine).
    std::mutex mTexturePoolMutex;
    uint32_t mTexturePoolSize = 0;
    std::vector<uint32_t> mTexturePool;

    std::unordered_map<sp<IBinder>, std::queue<TransactionState>, IListenerHash>
            mPendingTransactionQueues;
    LocklessQueue<TransactionState> mLocklessTransactionQueue;
    std::atomic<size_t> mPendingTransactionCount = 0;
    void SurfaceFlinger::composite(nsecs_t frameTime, int64_t vsyncId)

            void SurfaceFlinger::composite(nsecs_t frameTime, int64_t vsyncId)

                    std::atomic<size_t> mNumLayers = 0;

    // to linkToDeath
    sp<IBinder> mWindowManager;
    // We want to avoid multiple calls to BOOT_FINISHED as they come in on
    // different threads without a lock and could trigger unsynchronized writes to
    // to mWindowManager or mInputFlinger
    std::atomic<bool> mBootFinished = false;

    std::thread::id mMainThreadId = std::this_thread::get_id();

    DisplayColorSetting mDisplayColorSetting = DisplayColorSetting::kEnhanced;

    // Color mode forced by setting persist.sys.sf.color_mode, it must:
    //     1. not be NATIVE color mode, NATIVE color mode means no forced color mode;
    //     2. be one of the supported color modes returned by hardware composer, otherwise
    //        it will not be respected.
    // persist.sys.sf.color_mode will only take effect when persist.sys.sf.native_mode
    // is not set to 1.
    // This property can be used to force SurfaceFlinger to always pick a certain color mode.
    ui::ColorMode mForceColorMode = ui::ColorMode::NATIVE;

    ui::Dataspace mDefaultCompositionDataspace;
    ui::Dataspace mWideColorGamutCompositionDataspace;
    ui::Dataspace mColorSpaceAgnosticDataspace;
    float mDimmingRatio = -1.f;

    std::unique_ptr<compositionengine::CompositionEngine> mCompositionEngine;
    // mMaxRenderTargetSize is only set once in init() so it doesn't need to be protected by
    // any mutex.
    size_t mMaxRenderTargetSize{1};

    const std::string mHwcServiceName;

    bool hasMockHwc() const { return mHwcServiceName == "mock"; }

    /*
     * Scheduler
     */
    std::unique_ptr<scheduler::Scheduler> mScheduler;
    scheduler::ConnectionHandle mAppConnectionHandle;
    scheduler::ConnectionHandle mSfConnectionHandle;

    // Stores phase offsets configured per refresh rate.
    std::unique_ptr<scheduler::VsyncConfiguration> mVsyncConfiguration;

    // Optional to defer construction until PhaseConfiguration is created.
    sp<VsyncModulator> mVsyncModulator;

    std::unique_ptr<scheduler::RefreshRateStats> mRefreshRateStats;

    scheduler::PresentLatencyTracker mPresentLatencyTracker TimePoint mExpectedPresentTime TimePoint
            mScheduledPresentTime hal::Vsync mHWCVsyncPendingState = hal::Vsync::DISABLE;
    hal::Vsync mLastHWCVsyncState = hal::Vsync::DISABLE;

    // below flags are set by main thread only
    bool mSetActiveModePending = false;

    bool mLumaSampling = true;
    sp<RegionSamplingThread> mRegionSamplingThread;
    sp<FpsReporter> mFpsReporter;
    sp<TunnelModeEnabledReporter> mTunnelModeEnabledReporter;
    ui::DisplayPrimaries mInternalDisplayPrimaries;

    const float mInternalDisplayDensity;
    const float mEmulatedDisplayDensity;

    // Should only be accessed by the main thread.
    sp<os::IInputFlinger> mInputFlinger;
    InputWindowCommands mInputWindowCommands;

    std::unique_ptr<Hwc2::PowerAdvisor> mPowerAdvisor;

    REQUIRES(mStateLock);

    REQUIRES(mStateLock);

    // Flag used to set override desired display mode from backdoor
    bool mDebugDisplayModeSetByBackdoor = false;

    // A set of layers that have no parent so they are not drawn on screen.
    // Should only be accessed by the main thread.
    // The Layer pointer is removed from the set when the destructor is called so there shouldn't
    // be any issues with a raw pointer referencing an invalid object.
    std::unordered_set<Layer*> mOffscreenLayers;

    BufferCountTracker mBufferCountTracker;

    std::unordered_map<DisplayId, sp<HdrLayerInfoReporter>> mHdrLayerInfoListeners void
    SurfaceFlinger::composite(nsecs_t frameTime, int64_t vsyncId)

            mutable std::mutex mCreatedLayersLock;
    struct LayerCreatedState {
        LayerCreatedState(const wp<Layer>& layer, const wp<Layer> parent, bool addToRoot)
              : layer(layer), initialParent(parent), addToRoot(addToRoot) {}
        wp<Layer> layer;
        // Indicates the initial parent of the created layer, only used for creating layer in
        // SurfaceFlinger. If nullptr, it may add the created layer into the current root layers.
        wp<Layer> initialParent;
        // Indicates whether the layer getting created should be added at root if there's no parent
        // and has permission ACCESS_SURFACE_FLINGER. If set to false and no parent, the layer will
        // be added offscreen.
        bool addToRoot;
    };

    // A temporay pool that store the created layers and will be added to current state in main
    // thread.
    std::vector<LayerCreatedState> mCreatedLayersvoid SurfaceFlinger::composite(nsecs_t frameTime,
                                                                                int64_t vsyncId)

            bool commitCreatedLayers(VsyncId);
    REQUIRES(mStateLock);

    REQUIRES(mStateLock);

    std::atomic<ui::Transform::RotationFlags> mActiveDisplayTransformHint;

    bool isRefreshRateOverlayEnabled() const REQUIRES(mStateLock) {
        return hasDisplay(
                [](const auto& display) { return display.isRefreshRateOverlayEnabled(); });
    }

    wp<IBinder> mActiveDisplayTokenvoid SurfaceFlinger::composite(nsecs_t frameTime,
                                                                  int64_t vsyncId)

            const sp<WindowInfosListenerInvoker> mWindowInfosListenerInvoker;

    FlagManager mFlagManager;

    // returns the framerate of the layer with the given sequence ID
    float getLayerFramerate(nsecs_t now, int32_t id) const {
        return mScheduler->getLayerFramerate(now, id);
    }

    bool mPowerHintSessionEnabled;

    struct {
        bool late = false;
        bool early = false;
    } mPowerHintSessionMode;

    void startPowerHintSession() const;

    nsecs_t mAnimationTransactionTimeout = s2ns(5);

    friend class SurfaceComposerAIDL;
    mutable std::mutex mMirrorDisplayLock;
    struct MirrorDisplayState {
        MirrorDisplayState(ui::LayerStack layerStack, sp<IBinder>& rootHandle,
                           const sp<Client>& client)
              : layerStack(layerStack), rootHandle(rootHandle), client(client) {}

        ui::LayerStack layerStack;
        sp<IBinder> rootHandle;
        const sp<Client> client;
    };
    std::vector<MirrorDisplayState> mMirrorDisplays
};

class SurfaceComposerAIDL : public gui::BnSurfaceComposer {
public:
    SurfaceComposerAIDL(sp<SurfaceFlinger> sf) : mFlinger(std::move(sf)) {}

    binder::Status bootFinished() override;
    binder::Status createDisplayEventConnection(
            VsyncSource vsyncSource, EventRegistration eventRegistration,
            sp<gui::IDisplayEventConnection>* outConnection) override;
    binder::Status createConnection(sp<gui::ISurfaceComposerClient>* outClient) override;
    binder::Status createDisplay(const std::string& displayName, bool secure,
                                 sp<IBinder>* outDisplay) override;
    binder::Status destroyDisplay(const sp<IBinder>& display) override;
    binder::Status getPhysicalDisplayIds(std::vector<int64_t>* outDisplayIds) override;
    binder::Status getPhysicalDisplayToken(int64_t displayId, sp<IBinder>* outDisplay) override;
    binder::Status setPowerMode(const sp<IBinder>& display, int mode) override;
    binder::Status getSupportedFrameTimestamps(std::vector<FrameEvent>* outSupported) override;
    binder::Status getDisplayStats(const sp<IBinder>& display,
                                   gui::DisplayStatInfo* outStatInfo) override;
    binder::Status getDisplayState(const sp<IBinder>& display,
                                   gui::DisplayState* outState) override;
    binder::Status getStaticDisplayInfo(const sp<IBinder>& display,
                                        gui::StaticDisplayInfo* outInfo) override;
    binder::Status getDynamicDisplayInfo(const sp<IBinder>& display,
                                         gui::DynamicDisplayInfo* outInfo) override;
    binder::Status getDisplayNativePrimaries(const sp<IBinder>& display,
                                             gui::DisplayPrimaries* outPrimaries) override;
    binder::Status setActiveColorMode(const sp<IBinder>& display, int colorMode) override;
    binder::Status setBootDisplayMode(const sp<IBinder>& display, int displayModeId) override;
    binder::Status clearBootDisplayMode(const sp<IBinder>& display) override;
    binder::Status getBootDisplayModeSupport(bool* outMode) override;
    binder::Status setAutoLowLatencyMode(const sp<IBinder>& display, bool on) override;
    binder::Status setGameContentType(const sp<IBinder>& display, bool on) override;
    binder::Status captureDisplay(const DisplayCaptureArgs&,
                                  const sp<IScreenCaptureListener>&) override;
    binder::Status captureDisplayById(int64_t, const sp<IScreenCaptureListener>&) override;
    binder::Status captureLayers(const LayerCaptureArgs&,
                                 const sp<IScreenCaptureListener>&) override;
    // TODO(b/239076119): Remove deprecated AIDL.
    [[deprecated]] binder::Status clearAnimationFrameStats() override {
        return binder::Status::ok();
    }
    [[deprecated]] binder::Status getAnimationFrameStats(gui::FrameStats*) override {
        return binder::Status::ok();
    }

    binder::Status overrideHdrTypes(const sp<IBinder>& display,
                                    const std::vector<int32_t>& hdrTypes) override;
    binder::Status onPullAtom(int32_t atomId, gui::PullAtomData* outPullData) override;
    binder::Status enableVSyncInjections(bool enable) override;
    binder::Status injectVSync(int64_t when) override;
    binder::Status getLayerDebugInfo(std::vector<gui::LayerDebugInfo>* outLayers) override;
    binder::Status getColorManagement(bool* outGetColorManagement) override;
    binder::Status getCompositionPreference(gui::CompositionPreference* outPref) override;
    binder::Status getDisplayedContentSamplingAttributes(
            const sp<IBinder>& display, gui::ContentSamplingAttributes* outAttrs) override;
    binder::Status setDisplayContentSamplingEnabled(const sp<IBinder>& display, bool enable,
                                                    int8_t componentMask,
                                                    int64_t maxFrames) override;
    binder::Status getDisplayedContentSample(const sp<IBinder>& display, int64_t maxFrames,
                                             int64_t timestamp,
                                             gui::DisplayedFrameStats* outStats) override;
    binder::Status getProtectedContentSupport(bool* outSupporte) override;
    binder::Status isWideColorDisplay(const sp<IBinder>& token,
                                      bool* outIsWideColorDisplay) override;
    binder::Status addRegionSamplingListener(
            const gui::ARect& samplingArea, const sp<IBinder>& stopLayerHandle,
            const sp<gui::IRegionSamplingListener>& listener) override;
    binder::Status removeRegionSamplingListener(
            const sp<gui::IRegionSamplingListener>& listener) override;
    binder::Status addFpsListener(int32_t taskId, const sp<gui::IFpsListener>& listener) override;
    binder::Status removeFpsListener(const sp<gui::IFpsListener>& listener) override;
    binder::Status addTunnelModeEnabledListener(
            const sp<gui::ITunnelModeEnabledListener>& listener) override;
    binder::Status removeTunnelModeEnabledListener(
            const sp<gui::ITunnelModeEnabledListener>& listener) override;
    binder::Status setDesiredDisplayModeSpecs(const sp<IBinder>& displayToken, int32_t defaultMode,
                                              bool allowGroupSwitching, float primaryRefreshRateMin,
                                              float primaryRefreshRateMax,
                                              float appRequestRefreshRateMin,
                                              float appRequestRefreshRateMax) override;
    binder::Status getDesiredDisplayModeSpecs(const sp<IBinder>& displayToken,
                                              gui::DisplayModeSpecs* outSpecs) override;
    binder::Status getDisplayBrightnessSupport(const sp<IBinder>& displayToken,
                                               bool* outSupport) override;
    binder::Status setDisplayBrightness(const sp<IBinder>& displayToken,
                                        const gui::DisplayBrightness& brightness) override;
    binder::Status addHdrLayerInfoListener(const sp<IBinder>& displayToken,
                                           const sp<gui::IHdrLayerInfoListener>& listener) override;
    binder::Status removeHdrLayerInfoListener(
            const sp<IBinder>& displayToken,
            const sp<gui::IHdrLayerInfoListener>& listener) override;
    binder::Status notifyPowerBoost(int boostId) override;

    binder::Status setGlobalShadowSettings(const gui::Color& ambientColor,
                                           const gui::Color& spotColor, float lightPosY,
                                           float lightPosZ, float lightRadius) override;
    binder::Status getDisplayDecorationSupport(
            const sp<IBinder>& displayToken,
            std::optional<gui::DisplayDecorationSupport>* outSupport) override;
    binder::Status setOverrideFrameRate(int32_t uid, float frameRate) override;
    binder::Status addTransactionTraceListener(
            const sp<gui::ITransactionTraceListener>& listener) override;
    binder::Status getGpuContextPriority(int32_t* outPriority) override;
    binder::Status getMaxAcquiredBufferCount(int32_t* buffers) override;
    binder::Status addWindowInfosListener(
            const sp<gui::IWindowInfosListener>& windowInfosListener) override;
    binder::Status removeWindowInfosListener(
            const sp<gui::IWindowInfosListener>& windowInfosListener) override;

private:
    static const constexpr bool kUsePermissionCache = true;
    status_t checkAccessPermission(bool usePermissionCache = kUsePermissionCache);
    status_t checkControlDisplayBrightnessPermission();

    status_t checkReadFrameBufferPermission();

    sp<SurfaceFlinger> mFlinger;
};

} // namespace android