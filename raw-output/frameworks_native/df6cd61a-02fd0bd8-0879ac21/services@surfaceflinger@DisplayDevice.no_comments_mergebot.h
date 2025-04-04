       
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <android-base/thread_annotations.h>
#include <android/native_window.h>
#include <binder/IBinder.h>
#include <gui/LayerState.h>
#include <math/mat4.h>
#include <renderengine/RenderEngine.h>
#include <system/window.h>
#include <ui/DisplayId.h>
#include <ui/DisplayIdentification.h>
#include <ui/DisplayState.h>
#include <ui/GraphicTypes.h>
#include <ui/HdrCapabilities.h>
#include <ui/Region.h>
#include <ui/StaticDisplayInfo.h>
#include <ui/Transform.h>
#include <utils/Errors.h>
#include <utils/Mutex.h>
#include <utils/RefBase.h>
#include <utils/Timers.h>
#include "Display/DisplayModeRequest.h"
#include "DisplayHardware/DisplayMode.h"
#include "DisplayHardware/Hal.h"
#include "DisplayHardware/PowerAdvisor.h"
#include "Scheduler/RefreshRateConfigs.h"
#include "FrontEnd/DisplayInfo.h"
#include "Scheduler/RefreshRateSelector.h"
#include "ThreadContext.h"
#include "TracedOrdinal.h"
#include "Utils/Dumper.h"
namespace android {
class Fence;
class HWComposer;
class IGraphicBufferProducer;
class Layer;
class RefreshRateOverlay;
class SurfaceFlinger;
struct CompositionInfo;
struct DisplayDeviceCreationArgs;
namespace compositionengine {
class Display;
class DisplaySurface;
}
namespace display {
class DisplaySnapshot;
}
class DisplayDevice : public RefBase {
public:
    constexpr static float sDefaultMinLumiance = 0.0;
    constexpr static float sDefaultMaxLumiance = 500.0;
    enum { eReceivesInput = 0x01 };
    explicit DisplayDevice(DisplayDeviceCreationArgs& args);
    virtual ~DisplayDevice();
    std::shared_ptr<compositionengine::Display> getCompositionDisplay() const {
        return mCompositionDisplay;
    }
    bool isVirtual() const { return VirtualDisplayId::tryCast(getId()).has_value(); }
    bool isPrimary() const { return mIsPrimary; }
    bool isSecure() const;
    int getWidth() const;
    int getHeight() const;
    ui::Size getSize() const { return {getWidth(), getHeight()}; }
    void setLayerFilter(ui::LayerFilter);
    void setDisplaySize(int width, int height);
    void setProjection(ui::Rotation orientation, Rect viewport, Rect frame);
void stageBrightness(float brightness)private:
                                       GUARDED_BY(kMainThreadContext);
public:
    scheduler::RefreshRateSelector& refreshRateSelector() const { return *mRefreshRateSelector; }
    std::shared_ptr<scheduler::RefreshRateSelector> holdRefreshRateSelector() const {
        return mRefreshRateSelector;
    }
void enableRefreshRateOverlay(bool enable, bool showSpinner, bool showRenderRate,
                                  bool showInMiddle)void persistBrightness(bool needsComposite)private:
                                       GUARDED_BY(kMainThreadContext);
public:
bool isBrightnessStale() constprivate:
                                       GUARDED_BY(kMainThreadContext);
public:
    void setFlags(uint32_t flags);
    ui::Rotation getPhysicalOrientation() const { return mPhysicalOrientation; }
    ui::Rotation getOrientation() const { return mOrientation; }
    static ui::Transform::RotationFlags getPrimaryDisplayRotationFlags();
std::optional<float> getStagedBrightness() constprivate:
                                       GUARDED_BY(kMainThreadContext);
public:
    ui::Transform::RotationFlags getTransformHint() const;
    const ui::Transform& getTransform() const;
    const Rect& getLayerStackSpaceRect() const;
    const Rect& getOrientedDisplaySpaceRect() const;
    bool needsFiltering() const;
    ui::LayerStack getLayerStack() const;
    bool receivesInput() const { return mFlags & eReceivesInput; }
    DisplayId getId() const;
    PhysicalDisplayId getPhysicalId() const {
        const auto id = PhysicalDisplayId::tryCast(getId());
        LOG_FATAL_IF(!id);
        return *id;
    }
    VirtualDisplayId getVirtualId() const {
        const auto id = VirtualDisplayId::tryCast(getId());
        LOG_FATAL_IF(!id);
        return *id;
    }
    const wp<IBinder>& getDisplayToken() const { return mDisplayToken; }
    int32_t getSequenceId() const { return mSequenceId; }
    const Region& getUndefinedRegion() const;
    int32_t getSupportedPerFrameMetadata() const;
    bool hasWideColorGamut() const;
    bool hasHDR10PlusSupport() const;
    bool hasHDR10Support() const;
    bool hasHLGSupport() const;
    bool hasDolbyVisionSupport() const;
    void overrideHdrTypes(const std::vector<ui::Hdr>& hdrTypes);
    HdrCapabilities getHdrCapabilities() const;
    bool hasRenderIntent(ui::RenderIntent intent) const;
    const Rect getBounds() const;
    const Rect bounds() const { return getBounds(); }
    void setDisplayName(const std::string& displayName);
    const std::string& getDisplayName() const { return mDisplayName; }
    surfaceflinger::frontend::DisplayInfo getFrontEndInfo() const;
    std::optional<hardware::graphics::composer::hal::PowerMode> getPowerMode() const;
    void setPowerMode(hardware::graphics::composer::hal::PowerMode mode);
    bool isPoweredOn() const;
    void enableLayerCaching(bool enable);
    ui::Dataspace getCompositionDataSpace() const;
    struct ActiveModeInfo {
        using Event = scheduler::DisplayModeEvent;
        ActiveModeInfo() = default;
        ActiveModeInfo(scheduler::FrameRateMode mode, Event event)
              : modeOpt(std::move(mode)), event(event) {}
        explicit ActiveModeInfo(display::DisplayModeRequest&& request)
              : ActiveModeInfo(std::move(request.mode),
                               request.emitEvent ? Event::Changed : Event::None) {}
        ftl::Optional<scheduler::FrameRateMode> modeOpt;
        Event event = Event::None;
        bool operator!=(const ActiveModeInfo& other) const {
            return modeOpt != other.modeOpt || event != other.event;
        }
    };
    enum class DesiredActiveModeAction {
        None,
        InitiateDisplayModeSwitch,
        InitiateRenderRateSwitch
    };
DesiredActiveModeAction setDesiredActiveMode(const ActiveModeInfo&, bool force = false)private:
                                       GUARDED_BY(kMainThreadContext);
public:
std::optional<ActiveModeInfo> getDesiredActiveMode() constprivate:
                                       GUARDED_BY(kMainThreadContext);
public:
void clearDesiredActiveModeState()private:
                                       GUARDED_BY(kMainThreadContext);
public:
ActiveModeInfo getUpcomingActiveMode() const REQUIRES(kMainThreadContext) {
                                                 return mRefreshRateSelector->getActiveMode();
                                                 }
private:
                                       GUARDED_BY(kMainThreadContext);
                                       GUARDED_BY(kMainThreadContext);
public:
    status_t initiateModeChange(const ActiveModeInfo&,
                                const hal::VsyncPeriodChangeConstraints& constraints,
                                hal::VsyncPeriodChangeTimeline* outTimeline)
private:
                                       GUARDED_BY(kMainThreadContext);
public:
    bool isRefreshRateOverlayEnabled() const { return mRefreshRateOverlay != nullptr; }
    bool onKernelTimerChanged(std::optional<DisplayModeId>, bool timerExpired);
    void animateRefreshRateOverlay();
    nsecs_t getVsyncPeriodFromHWC() const;
    void disconnect();
    void dump(utils::Dumper&) const;
private:
    const sp<SurfaceFlinger> mFlinger;
    HWComposer& mHwComposer;
    const wp<IBinder> mDisplayToken;
    const int32_t mSequenceId;
    const std::shared_ptr<compositionengine::Display> mCompositionDisplay;
    std::string mDisplayName;
    std::string mActiveModeFPSTrace;
    std::string mActiveModeFPSHwcTrace;
    std::string mRenderFrameRateFPSTrace;
    const ui::Rotation mPhysicalOrientation;
    ui::Rotation mOrientation = ui::ROTATION_0;
    static ui::Transform::RotationFlags sPrimaryDisplayRotationFlags;
    std::optional<hardware::graphics::composer::hal::PowerMode> mPowerMode;
    std::optional<float> mStagedBrightness;
    std::optional<float> mBrightness;
    const bool mIsPrimary;
    uint32_t mFlags = 0;
    std::vector<ui::Hdr> mOverrideHdrTypes;
    std::shared_ptr<scheduler::RefreshRateSelector> mRefreshRateSelector;
    std::unique_ptr<RefreshRateOverlay> mRefreshRateOverlay;
    mutable std::mutex mActiveModeLock;
ActiveModeInfo mDesiredActiveMode GUARDED_BY(kMainThreadContext);
    TracedOrdinal<bool> mDesiredActiveModeChanged
                                       GUARDED_BY(kMainThreadContext);
ActiveModeInfo mUpcomingActiveMode GUARDED_BY(kMainThreadContext);
};
struct DisplayDeviceState {
    struct Physical {
        PhysicalDisplayId id;
        hardware::graphics::composer::hal::HWDisplayId hwcDisplayId;
        DisplayModePtr activeMode;
        bool operator==(const Physical& other) const {
            return id == other.id && hwcDisplayId == other.hwcDisplayId;
        }
    };
    bool isVirtual() const { return !physical; }
    int32_t sequenceId = sNextSequenceId++;
    std::optional<Physical> physical;
    sp<IGraphicBufferProducer> surface;
    ui::LayerStack layerStack;
    uint32_t flags = 0;
    Rect layerStackSpaceRect;
    Rect orientedDisplaySpaceRect;
    ui::Rotation orientation = ui::ROTATION_0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string displayName;
    bool isSecure = false;
private:
    static std::atomic<int32_t> sNextSequenceId;
};
struct DisplayDeviceCreationArgs {
    DisplayDeviceCreationArgs(const sp<SurfaceFlinger>&, HWComposer& hwComposer,
                              const wp<IBinder>& displayToken,
                              std::shared_ptr<compositionengine::Display>);
    const sp<SurfaceFlinger> flinger;
    HWComposer& hwComposer;
    const wp<IBinder> displayToken;
    const std::shared_ptr<compositionengine::Display> compositionDisplay;
    std::shared_ptr<scheduler::RefreshRateSelector> refreshRateSelector;
    int32_t sequenceId{0};
    bool isSecure{false};
    sp<ANativeWindow> nativeWindow;
    sp<compositionengine::DisplaySurface> displaySurface;
    ui::Rotation physicalOrientation{ui::ROTATION_0};
    bool hasWideColorGamut{false};
    HdrCapabilities hdrCapabilities;
    int32_t supportedPerFrameMetadata{0};
    std::unordered_map<ui::ColorMode, std::vector<ui::RenderIntent>> hwcColorModes;
    std::optional<hardware::graphics::composer::hal::PowerMode> initialPowerMode;
    bool isPrimary{false};
    DisplayModeId activeModeId;
};
}
