       
#include <sys/types.h>
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
}
namespace renderengine {
class RenderEngine;
}
enum {
    eTransactionNeeded = 0x01,
    eTraversalNeeded = 0x02,
    eDisplayTransactionNeeded = 0x04,
    eTransformHintUpdateNeeded = 0x08,
    eTransactionFlushNeeded = 0x10,
    eTransactionMask = 0x1f,
};
enum class LatchUnsignaledConfig {
    Disabled,
    AutoSingleLayer,
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
    SurfaceFlinger(surfaceflinger::Factory&, SkipInitializationTag) ANDROID_API;
    explicit SurfaceFlinger(surfaceflinger::Factory&) ANDROID_API;
    static status_t setSchedFifo(bool enabled) ANDROID_API;
    static status_t setSchedAttr(bool enabled);
    static char const* getServiceName() ANDROID_API { return "SurfaceFlinger"; }
    static bool hasSyncFramework;
    static int64_t dispSyncPresentTimeOffset;
    static bool useHwcForRgbToYuv;
    static int64_t maxFrameBufferAcquiredBuffers;
    static uint32_t maxGraphicsWidth;
    static uint32_t maxGraphicsHeight;
    static bool hasWideColorDisplay;
    static const constexpr bool useColorManagement = true;
    static bool useContextPriority;
    static ui::Dataspace defaultCompositionDataspace;
    static ui::PixelFormat defaultCompositionPixelFormat;
    static ui::Dataspace wideColorGamutCompositionDataspace;
    static ui::PixelFormat wideColorGamutCompositionPixelFormat;
    static constexpr SkipInitializationTag SkipInitialization;
    static LatchUnsignaledConfig enableLatchUnsignaledConfig;
    void init() ANDROID_API;
    void run() ANDROID_API;
    enum class FrameHint { kNone, kActive };
    void scheduleCommit(FrameHint);
    void scheduleComposite(FrameHint);
    void scheduleRepaint();
    void scheduleSample();
    surfaceflinger::Factory& getFactory() { return mFactory; }
    compositionengine::CompositionEngine& getCompositionEngine() const;
    uint32_t getNewTexture();
    void deleteTextureAsync(uint32_t texture);
    renderengine::RenderEngine& getRenderEngine() const;
    void onLayerFirstRef(Layer*);
    void onLayerDestroyed(Layer*);
    void onLayerUpdate();
    void removeHierarchyFromOffscreenLayers(Layer* layer);
    void removeFromOffscreenLayers(Layer* layer);
    std::atomic<uint32_t> mNumClones;
    TransactionCallbackInvoker& getTransactionCallbackInvoker() {
        return mTransactionCallbackInvoker;
    }
    wp<Layer> fromHandle(const sp<IBinder>& handle) const;
    bool mDisableClientCompositionCache = false;
    void disableExpensiveRendering();
    FloatRect getMaxDisplayBounds();
    bool mPredictCompositionStrategy = false;
    bool mTreat170mAsSrgb = false;
protected:
    virtual ~SurfaceFlinger();
    virtual void processDisplayAdded(const wp<IBinder>& displayToken, const DisplayDeviceState&)
            REQUIRES(mStateLock);
    virtual std::shared_ptr<renderengine::ExternalTexture> getExternalTextureFromBufferData(
            const BufferData& bufferData, const char* layerName) const;
    template <typename Predicate>
    bool hasDisplay(Predicate p) const REQUIRES(mStateLock) {
        return static_cast<bool>(findDisplay(p));
    }
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
        DefaultKeyedVector< wp<IBinder>, DisplayDeviceState> displays;
        bool colorMatrixChanged = true;
        mat4 colorMatrix;
        renderengine::ShadowSettings globalShadowSettings;
        void traverse(const LayerVector::Visitor& visitor) const;
        void traverseInZOrder(const LayerVector::Visitor& visitor) const;
        void traverseInReverseZOrder(const LayerVector::Visitor& visitor) const;
    };
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
    template <typename F, std::enable_if_t<!std::is_member_function_pointer_v<F>>* = nullptr>
    static Dumper dumper(F&& dump) {
        using namespace std::placeholders;
        return std::bind(std::forward<F>(dump), _3);
    }
    template <typename F, std::enable_if_t<std::is_member_function_pointer_v<F>>* = nullptr>
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
    static const int MAX_ALLOWED_DISPLAY_FRAMES = 2048;
    static const size_t MAX_LAYERS = 4096;
    status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override;
    status_t dump(int fd, const Vector<String16>& args) override { return priorityDump(fd, args); }
    bool callingThreadHasUnscopedSurfaceFlingerAccess(bool usePermissionCache = true)
            EXCLUDES(mStateLock);
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
    status_t getDisplayState(const sp<IBinder>& displayToken, ui::DisplayState*)
            EXCLUDES(mStateLock);
    status_t getStaticDisplayInfo(const sp<IBinder>& displayToken, ui::StaticDisplayInfo*)
            EXCLUDES(mStateLock);
    status_t getDynamicDisplayInfo(const sp<IBinder>& displayToken, ui::DynamicDisplayInfo*)
            EXCLUDES(mStateLock);
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
    void binderDied(const wp<IBinder>& who) override;
    void onComposerHalVsync(hal::HWDisplayId, int64_t timestamp,
                            std::optional<hal::VsyncPeriodNanos>) override;
    void onComposerHalHotplug(hal::HWDisplayId, hal::Connection) override;
    void onComposerHalRefresh(hal::HWDisplayId) override;
    void onComposerHalVsyncPeriodTimingChanged(hal::HWDisplayId,
                                               const hal::VsyncPeriodChangeTimeline&) override;
    void onComposerHalSeamlessPossible(hal::HWDisplayId) override;
    void onComposerHalVsyncIdle(hal::HWDisplayId) override;
    bool commit(TimePoint frameTime, VsyncId, TimePoint expectedVsyncTime) override;
    void composite(TimePoint frameTime, VsyncId) override;
    void sample() override;
    void setVsyncEnabled(bool) override;
    void requestDisplayMode(DisplayModePtr, DisplayModeEvent) override;
    void kernelTimerChanged(bool expired) override;
    void triggerOnFrameRateOverridesChanged() override;
    void toggleKernelIdleTimer() REQUIRES(mStateLock);
    std::pair<std::optional<KernelIdleTimerController>, std::chrono::milliseconds>
            getKernelIdleTimerProperties(DisplayId) REQUIRES(mStateLock);
    void updateKernelIdleTimer(std::chrono::milliseconds timeoutMs, KernelIdleTimerController,
                               PhysicalDisplayId) REQUIRES(mStateLock);
    bool mKernelIdleTimerEnabled = false;
    bool mRefreshRateOverlaySpinner = false;
    void onInitializeDisplays() REQUIRES(mStateLock);
    void setDesiredActiveMode(const ActiveModeInfo& info) REQUIRES(mStateLock);
    status_t setActiveModeFromBackdoor(const sp<IBinder>& displayToken, int id);
    void updateInternalStateWithChangedMode() REQUIRES(mStateLock);
    void setActiveModeInHwcIfNeeded() REQUIRES(mStateLock);
    void clearDesiredActiveModeState(const sp<DisplayDevice>&) REQUIRES(mStateLock);
    void desiredActiveModeChangeDone(const sp<DisplayDevice>&) REQUIRES(mStateLock);
    void setPowerModeInternal(const sp<DisplayDevice>& display, hal::PowerMode mode)
            REQUIRES(mStateLock);
    bool hasVisibleHdrLayer(const sp<DisplayDevice>& display) REQUIRES(mStateLock);
    status_t setDesiredDisplayModeSpecsInternal(
            const sp<DisplayDevice>& display,
            const std::optional<scheduler::RefreshRateConfigs::Policy>& policy, bool overridePolicy)
            EXCLUDES(mStateLock);
    void commitTransactions() EXCLUDES(mStateLock);
    void commitTransactionsLocked(uint32_t transactionFlags) REQUIRES(mStateLock);
    void doCommitTransactions() REQUIRES(mStateLock);
    bool latchBuffers();
    void updateLayerGeometry();
    void updateInputFlinger();
    void persistDisplayBrightness(bool needsComposite) REQUIRES(kMainThreadContext);
    void buildWindowInfos(std::vector<gui::WindowInfo>& outWindowInfos,
                          std::vector<gui::DisplayInfo>& outDisplayInfos);
    void commitInputWindowCommands() REQUIRES(mStateLock);
    void updateCursorAsync();
    void initScheduler(const sp<DisplayDevice>& display) REQUIRES(mStateLock);
    void updatePhaseConfiguration(const Fps&) REQUIRES(mStateLock);
    void setVsyncConfig(const VsyncModulator::VsyncConfig&, nsecs_t vsyncPeriod);
    bool applyTransactionState(const FrameTimelineInfo& info, Vector<ComposerState>& state,
                               const Vector<DisplayState>& displays, uint32_t flags,
                               const InputWindowCommands& inputWindowCommands,
                               const int64_t desiredPresentTime, bool isAutoTimestamp,
                               const client_cache_t& uncacheBuffer, const int64_t postTime,
                               uint32_t permissions, bool hasListenerCallbacks,
                               const std::vector<ListenerCallbacks>& listenerCallbacks,
                               int originPid, int originUid, uint64_t transactionId)
            REQUIRES(mStateLock);
    bool flushTransactionQueues(VsyncId) REQUIRES(kMainThreadContext);
    bool transactionFlushNeeded();
    int flushPendingTransactionQueues(
            std::vector<TransactionState>& transactions,
            std::unordered_map<sp<IBinder>, uint64_t, SpHash<IBinder>>& bufferLayersReadyToPresent,
            bool tryApplyUnsignaled) REQUIRES(mStateLock) REQUIRES(kMainThreadContext);
    uint32_t setClientStateLocked(const FrameTimelineInfo&, ComposerState&,
                                  int64_t desiredPresentTime, bool isAutoTimestamp,
                                  int64_t postTime, uint32_t permissions) REQUIRES(mStateLock);
    uint32_t getTransactionFlags() const;
    void setTransactionFlags(uint32_t mask, TransactionSchedule = TransactionSchedule::Late,
                             const sp<IBinder>& applyToken = nullptr,
                             FrameHint = FrameHint::kActive);
    uint32_t clearTransactionFlags(uint32_t mask);
    void commitOffscreenLayers();
    enum class TransactionReadiness {
        NotReady,
        NotReadyBarrier,
        Ready,
        ReadyUnsignaled,
    };
    TransactionReadiness transactionIsReadyToBeApplied(
            TransactionState&, const FrameTimelineInfo&, bool isAutoTimestamp,
            TimePoint desiredPresentTime, uid_t originUid, const Vector<ComposerState>&,
            const std::unordered_map<sp<IBinder>, uint64_t, SpHash<IBinder>>&
                    bufferLayersReadyToPresent,
            size_t totalTXapplied, bool tryApplyUnsignaled) const REQUIRES(mStateLock)
            REQUIRES(kMainThreadContext);
    static LatchUnsignaledConfig getLatchUnsignaledConfig();
    bool shouldLatchUnsignaled(const sp<Layer>& layer, const layer_state_t&, size_t numStates,
                               size_t totalTXapplied) const;
    bool applyTransactions(std::vector<TransactionState>& transactions, VsyncId)
            REQUIRES(mStateLock);
    uint32_t setDisplayStateLocked(const DisplayState& s) REQUIRES(mStateLock);
    uint32_t addInputWindowCommands(const InputWindowCommands& inputWindowCommands)
            REQUIRES(mStateLock);
    bool frameIsEarly(TimePoint expectedPresentTime, VsyncId) const;
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
    void onHandleDestroyed(BBinder* handle, sp<Layer>& layer);
    void markLayerPendingRemovalLocked(const sp<Layer>& layer);
    status_t addClientLayer(const sp<Client>& client, const sp<IBinder>& handle,
                            const sp<Layer>& lbc, const wp<Layer>& parentLayer, bool addToRoot,
                            uint32_t* outTransformHint);
    void computeLayerBounds();
    void startBootAnim();
    ftl::SharedFuture<FenceResult> captureScreenCommon(RenderAreaFuture, TraverseLayersFunction,
                                                       ui::Size bufferSize, ui::PixelFormat,
                                                       bool allowProtected, bool grayscale,
                                                       const sp<IScreenCaptureListener>&);
    ftl::SharedFuture<FenceResult> captureScreenCommon(
            RenderAreaFuture, TraverseLayersFunction,
            const std::shared_ptr<renderengine::ExternalTexture>&, bool regionSampling,
            bool grayscale, const sp<IScreenCaptureListener>&);
    ftl::SharedFuture<FenceResult> renderScreenImpl(
            const RenderArea&, TraverseLayersFunction,
            const std::shared_ptr<renderengine::ExternalTexture>&, bool canCaptureBlackoutContent,
            bool regionSampling, bool grayscale, ScreenCaptureResults&) EXCLUDES(mStateLock);
    void traverseLayersInLayerStack(ui::LayerStack, const int32_t uid, const LayerVector::Visitor&);
    void readPersistentProperties();
    uint32_t getMaxAcquiredBufferCountForCurrentRefreshRate(uid_t uid) const;
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
        return findDisplay([id](const auto& display) { return display.getId() == id; });
    }
    sp<const DisplayDevice> getDefaultDisplayDeviceLocked() const REQUIRES(mStateLock) {
        return const_cast<SurfaceFlinger*>(this)->getDefaultDisplayDeviceLocked();
    }
    sp<DisplayDevice> getDefaultDisplayDeviceLocked() REQUIRES(mStateLock) {
        if (const auto display = getDisplayDeviceLocked(mActiveDisplayToken)) {
            return display;
        }
        mActiveDisplayToken.clear();
        return getDisplayDeviceLocked(getPrimaryDisplayTokenLocked());
    }
    sp<const DisplayDevice> getDefaultDisplayDevice() const EXCLUDES(mStateLock) {
        Mutex::Autolock lock(mStateLock);
        return getDefaultDisplayDeviceLocked();
    }
    template <typename Predicate>
    sp<DisplayDevice> findDisplay(Predicate p) const REQUIRES(mStateLock) {
        const auto it = std::find_if(mDisplays.begin(), mDisplays.end(),
                                     [&](const auto& pair) { return p(*pair.second); });
        return it == mDisplays.end() ? nullptr : it->second;
    }
    std::vector<PhysicalDisplayId> getPhysicalDisplayIdsLocked() const REQUIRES(mStateLock);
    void invalidateLayerStack(const sp<const Layer>& layer, const Region& dirty);
    bool isDisplayActiveLocked(const sp<const DisplayDevice>& display) const REQUIRES(mStateLock) {
        return display->getDisplayToken() == mActiveDisplayToken;
    }
    HWComposer& getHwComposer() const;
    void postComposition() REQUIRES(kMainThreadContext);
    std::pair<DisplayModes, DisplayModePtr> loadDisplayModes(PhysicalDisplayId) const
            REQUIRES(mStateLock);
    sp<DisplayDevice> setupNewDisplayDeviceInternal(
            const wp<IBinder>& displayToken,
            std::shared_ptr<compositionengine::Display> compositionDisplay,
            const DisplayDeviceState& state,
            const sp<compositionengine::DisplaySurface>& displaySurface,
            const sp<IGraphicBufferProducer>& producer) REQUIRES(mStateLock);
    void processDisplayChangesLocked() REQUIRES(mStateLock);
    void processDisplayRemoved(const wp<IBinder>& displayToken) REQUIRES(mStateLock);
    void processDisplayChanged(const wp<IBinder>& displayToken,
                               const DisplayDeviceState& currentState,
                               const DisplayDeviceState& drawingState) REQUIRES(mStateLock);
    void processDisplayHotplugEventsLocked() REQUIRES(mStateLock);
    void dispatchDisplayHotplugEvent(PhysicalDisplayId displayId, bool connected);
    nsecs_t getVsyncPeriodFromHWC() const REQUIRES(mStateLock);
    void setHWCVsyncEnabled(PhysicalDisplayId id, hal::Vsync enabled) {
        mLastHWCVsyncState = enabled;
        getHwComposer().setVsyncEnabled(id, enabled);
    }
    using FenceTimePtr = std::shared_ptr<FenceTime>;
    const FenceTimePtr& getPreviousPresentFence(TimePoint frameTime, Period)
            REQUIRES(kMainThreadContext);
    static bool isFencePending(const FenceTimePtr&, int graceTimeMs);
    TimePoint calculateExpectedPresentTime(TimePoint frameTime) const;
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
    sp<IBinder> getPrimaryDisplayTokenLocked() const REQUIRES(mStateLock) {
        return getPhysicalDisplayTokenLocked(getPrimaryDisplayIdLocked());
    }
    PhysicalDisplayId getPrimaryDisplayIdLocked() const REQUIRES(mStateLock) {
        return getHwComposer().getPrimaryDisplayId();
    }
    void enableHalVirtualDisplays(bool);
    VirtualDisplayId acquireVirtualDisplay(ui::Size, ui::PixelFormat) REQUIRES(mStateLock);
    void releaseVirtualDisplay(VirtualDisplayId);
    void onActiveDisplayChangedLocked(const sp<DisplayDevice>& activeDisplay) REQUIRES(mStateLock);
    void onActiveDisplaySizeChanged(const sp<DisplayDevice>& activeDisplay);
    void dumpAllLocked(const DumpArgs& args, std::string& result) const REQUIRES(mStateLock);
    void dumpHwcLayersMinidumpLocked(std::string& result) const REQUIRES(mStateLock);
    void appendSfConfigString(std::string& result) const;
    void listLayersLocked(std::string& result) const;
    void dumpStatsLocked(const DumpArgs& args, std::string& result) const REQUIRES(mStateLock);
    void clearStatsLocked(const DumpArgs& args, std::string& result);
    void dumpTimeStats(const DumpArgs& args, bool asProto, std::string& result) const;
    void dumpFrameTimeline(const DumpArgs& args, std::string& result) const;
    void logFrameStats(TimePoint now) REQUIRES(kMainThreadContext);
    void dumpVSync(std::string& result) const REQUIRES(mStateLock);
    void dumpCompositionDisplays(std::string& result) const REQUIRES(mStateLock);
    void dumpDisplays(std::string& result) const REQUIRES(mStateLock);
    void dumpDisplayIdentificationData(std::string& result) const REQUIRES(mStateLock);
    void dumpRawDisplayIdentificationData(const DumpArgs&, std::string& result) const;
    void dumpWideColorInfo(std::string& result) const REQUIRES(mStateLock);
    LayersProto dumpDrawingStateProto(uint32_t traceFlags) const;
    void dumpOffscreenLayersProto(LayersProto& layersProto,
                                  uint32_t traceFlags = LayerTracing::TRACE_ALL) const;
    void dumpDisplayProto(LayersTraceProto& layersTraceProto) const;
    void dumpHwc(std::string& result) const;
    LayersProto dumpProtoFromMainThread(uint32_t traceFlags = LayerTracing::TRACE_ALL)
            EXCLUDES(mStateLock);
    void dumpOffscreenLayers(std::string& result) EXCLUDES(mStateLock);
    void dumpPlannerInfo(const DumpArgs& args, std::string& result) const REQUIRES(mStateLock);
    status_t doDump(int fd, const DumpArgs& args, bool asProto);
    status_t dumpCritical(int fd, const DumpArgs&, bool asProto);
    status_t dumpAll(int fd, const DumpArgs& args, bool asProto) override {
        return doDump(fd, args, asProto);
    }
    static mat4 calculateColorMatrix(float saturation);
    void updateColorMatrixLocked();
    status_t CheckTransactCodeCredentials(uint32_t code);
    void queueTransaction(TransactionState& state);
    void waitForSynchronousTransaction(const CountDownLatch& transactionCommittedSignal);
    void signalSynchronousTransactions(const uint32_t flag);
    const std::unordered_map<std::string, uint32_t>& getGenericLayerMetadataKeyMap() const;
    std::vector<ui::ColorMode> getDisplayColorModes(const DisplayDevice&) REQUIRES(mStateLock);
    static int calculateMaxAcquiredBufferCount(Fps refreshRate,
                                               std::chrono::nanoseconds presentLatency);
    int getMaxAcquiredBufferCountForRefreshRate(Fps refreshRate) const;
    void updateInternalDisplayVsyncLocked(const sp<DisplayDevice>& activeDisplay)
            REQUIRES(mStateLock);
    bool isHdrLayer(Layer* layer) const;
    ui::Rotation getPhysicalDisplayOrientation(DisplayId, bool isPrimary) const
            REQUIRES(mStateLock);
    sp<StartPropertySetThread> mStartPropertySetThread;
    surfaceflinger::Factory& mFactory;
    pid_t mPid;
    std::future<void> mRenderEnginePrimeCacheFuture;
    mutable Mutex mStateLock;
    State mCurrentState{LayerVector::StateSet::Current};
    std::atomic<int32_t> mTransactionFlags = 0;
    std::vector<std::shared_ptr<CountDownLatch>> mTransactionCommittedSignals;
    std::atomic<uint32_t> mUniqueTransactionId = 1;
    SortedVector<sp<Layer>> mLayersPendingRemoval;
    Daltonizer mDaltonizer;
    float mGlobalSaturationFactor = 1.0f;
    mat4 mClientColorMatrix;
    size_t mMaxGraphicBufferProducerListSize = MAX_LAYERS;
    size_t mGraphicBufferProducerListSizeLogThreshold =
            static_cast<size_t>(0.95 * static_cast<double>(MAX_LAYERS));
    bool mLayersRemoved = false;
    bool mLayersAdded = false;
    std::atomic_bool mMustComposite = false;
    std::atomic_bool mGeometryDirty = false;
    const nsecs_t mBootTime = systemTime();
    bool mGpuToCpuSupported = false;
    bool mIsUserBuild = true;
    State mDrawingState{LayerVector::StateSet::Drawing};
    bool mVisibleRegionsDirty = false;
    bool mVisibleRegionsWereDirtyThisFrame = false;
    bool mAddingHDRLayerInfoListener = false;
    bool mIgnoreHdrCameraLayers = false;
    bool mUpdateInputInfo = false;
    bool mSomeChildrenChanged;
    bool mSomeDataspaceChanged = false;
    bool mForceTransactionDisplayChange = false;
    std::unordered_set<sp<Layer>, SpHash<Layer>> mLayersWithQueuedFrames;
    std::vector<sp<Layer>> mLayersPendingRefresh;
    bool mHadClientComposition = false;
    bool mHadDeviceComposition = false;
    bool mReusedClientComposition = false;
    BootStage mBootStage = BootStage::BOOTLOADER;
    std::vector<HotplugEvent> mPendingHotplugEvents GUARDED_BY(mStateLock);
    ftl::SmallMap<wp<IBinder>, const sp<DisplayDevice>, 5> mDisplays GUARDED_BY(mStateLock);
    ftl::SmallMap<PhysicalDisplayId, const sp<IBinder>, 3> mPhysicalDisplayTokens
            GUARDED_BY(mStateLock);
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
    bool mSupportsBlur = false;
    bool mBlursAreExpensive = false;
    std::atomic<uint32_t> mFrameMissedCount = 0;
    std::atomic<uint32_t> mHwcFrameMissedCount = 0;
    std::atomic<uint32_t> mGpuFrameMissedCount = 0;
    TransactionCallbackInvoker mTransactionCallbackInvoker;
    std::mutex mTexturePoolMutex;
    uint32_t mTexturePoolSize = 0;
    std::vector<uint32_t> mTexturePool;
    std::unordered_map<sp<IBinder>, std::queue<TransactionState>, IListenerHash>
            mPendingTransactionQueues;
    LocklessQueue<TransactionState> mLocklessTransactionQueue;
    std::atomic<size_t> mPendingTransactionCount = 0;
    std::atomic<size_t> mNumLayers = 0;
    sp<IBinder> mWindowManager;
    std::atomic<bool> mBootFinished = false;
    std::thread::id mMainThreadId = std::this_thread::get_id();
    DisplayColorSetting mDisplayColorSetting = DisplayColorSetting::kEnhanced;
    ui::ColorMode mForceColorMode = ui::ColorMode::NATIVE;
    ui::Dataspace mDefaultCompositionDataspace;
    ui::Dataspace mWideColorGamutCompositionDataspace;
    ui::Dataspace mColorSpaceAgnosticDataspace;
    float mDimmingRatio = -1.f;
    std::unique_ptr<compositionengine::CompositionEngine> mCompositionEngine;
    size_t mMaxRenderTargetSize{1};
    const std::string mHwcServiceName;
    bool hasMockHwc() const { return mHwcServiceName == "mock"; }
    std::unique_ptr<scheduler::Scheduler> mScheduler;
    scheduler::ConnectionHandle mAppConnectionHandle;
    scheduler::ConnectionHandle mSfConnectionHandle;
    std::unique_ptr<scheduler::VsyncConfiguration> mVsyncConfiguration;
    sp<VsyncModulator> mVsyncModulator;
    std::unique_ptr<scheduler::RefreshRateStats> mRefreshRateStats;
    scheduler::PresentLatencyTracker mPresentLatencyTracker GUARDED_BY(kMainThreadContext);
    struct FenceWithFenceTime {
        sp<Fence> fence = Fence::NO_FENCE;
        FenceTimePtr fenceTime = FenceTime::NO_FENCE;
    };
    std::array<FenceWithFenceTime, 2> mPreviousPresentFences;
    TimePoint mScheduledPresentTime GUARDED_BY(kMainThreadContext);
    TimePoint mExpectedPresentTime GUARDED_BY(kMainThreadContext);
    hal::Vsync mHWCVsyncPendingState = hal::Vsync::DISABLE;
    hal::Vsync mLastHWCVsyncState = hal::Vsync::DISABLE;
    bool mSetActiveModePending = false;
    bool mLumaSampling = true;
    sp<RegionSamplingThread> mRegionSamplingThread;
    sp<FpsReporter> mFpsReporter;
    sp<TunnelModeEnabledReporter> mTunnelModeEnabledReporter;
    ui::DisplayPrimaries mInternalDisplayPrimaries;
    const float mInternalDisplayDensity;
    const float mEmulatedDisplayDensity;
    sp<os::IInputFlinger> mInputFlinger;
    InputWindowCommands mInputWindowCommands;
    std::unique_ptr<Hwc2::PowerAdvisor> mPowerAdvisor;
    void enableRefreshRateOverlay(bool enable) REQUIRES(mStateLock);
    bool mDebugDisplayModeSetByBackdoor = false;
    std::unordered_set<Layer*> mOffscreenLayers;
    BufferCountTracker mBufferCountTracker;
    std::unordered_map<DisplayId, sp<HdrLayerInfoReporter>> mHdrLayerInfoListeners
            GUARDED_BY(mStateLock);
    mutable std::mutex mCreatedLayersLock;
    struct LayerCreatedState {
        LayerCreatedState(const wp<Layer>& layer, const wp<Layer> parent, bool addToRoot)
              : layer(layer), initialParent(parent), addToRoot(addToRoot) {}
        wp<Layer> layer;
        wp<Layer> initialParent;
        bool addToRoot;
    };
    std::vector<LayerCreatedState> mCreatedLayers GUARDED_BY(mCreatedLayersLock);
    bool commitCreatedLayers(VsyncId);
    void handleLayerCreatedLocked(const LayerCreatedState&, VsyncId) REQUIRES(mStateLock);
    mutable std::mutex mMirrorDisplayLock;
    struct MirrorDisplayState {
        MirrorDisplayState(ui::LayerStack layerStack, sp<IBinder>& rootHandle,
                           const sp<Client>& client)
              : layerStack(layerStack), rootHandle(rootHandle), client(client) {}
        ui::LayerStack layerStack;
        sp<IBinder> rootHandle;
        const sp<Client> client;
    };
    std::vector<MirrorDisplayState> mMirrorDisplays GUARDED_BY(mMirrorDisplayLock);
    bool commitMirrorDisplays(VsyncId);
    std::atomic<ui::Transform::RotationFlags> mActiveDisplayTransformHint;
    bool isRefreshRateOverlayEnabled() const REQUIRES(mStateLock) {
        return hasDisplay(
                [](const auto& display) { return display.isRefreshRateOverlayEnabled(); });
    }
    wp<IBinder> mActiveDisplayToken GUARDED_BY(mStateLock);
    const sp<WindowInfosListenerInvoker> mWindowInfosListenerInvoker;
    FlagManager mFlagManager;
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
private:
    sp<SurfaceFlinger> mFlinger;
};
}
