#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"
#undef LOG_TAG
#define LOG_TAG "Layer"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include "Layer.h"
#include <android-base/stringprintf.h>
#include <android/native_window.h>
#include <binder/IPCThreadState.h>
#include <compositionengine/Display.h>
#include <compositionengine/LayerFECompositionState.h>
#include <compositionengine/OutputLayer.h>
#include <compositionengine/impl/OutputLayerCompositionState.h>
#include <cutils/compiler.h>
#include <cutils/native_handle.h>
#include <cutils/properties.h>
#include <gui/BufferItem.h>
#include <gui/LayerDebugInfo.h>
#include <gui/Surface.h>
#include <math.h>
#include <private/android_filesystem_config.h>
#include <renderengine/RenderEngine.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <ui/DebugUtils.h>
#include <ui/GraphicBuffer.h>
#include <ui/PixelFormat.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/NativeHandle.h>
#include <utils/StopWatch.h>
#include <utils/Trace.h>
#include <algorithm>
#include <mutex>
#include <sstream>
#include "BufferLayer.h"
#include "Colorizer.h"
#include "DisplayDevice.h"
#include "DisplayHardware/HWComposer.h"
#include "EffectLayer.h"
#include "FrameTimeline.h"
#include "FrameTracer/FrameTracer.h"
#include "LayerProtoHelper.h"
#include "LayerRejecter.h"
#include "MonitoredProducer.h"
#include "SurfaceFlinger.h"
#include "TimeStats/TimeStats.h"
#include "input/InputWindow.h"
#define DEBUG_RESIZE 0
namespace android {
namespace {
constexpr int kDumpTableRowLength = 159;
}
using base::StringAppendF;
using namespace android::flag_operators;
using PresentState = frametimeline::SurfaceFrame::PresentState;
std::atomic<int32_t> Layer::sSequence{1};
Layer::Layer(const LayerCreationArgs& args)
      : mFlinger(args.flinger),
        mName(args.name),
        mClientRef(args.client),
        mWindowType(static_cast<InputWindowInfo::Type>(
                args.metadata.getInt32(METADATA_WINDOW_TYPE, 0))) {
    uint32_t layerFlags = 0;
    if (args.flags & ISurfaceComposerClient::eHidden) layerFlags |= layer_state_t::eLayerHidden;
    if (args.flags & ISurfaceComposerClient::eOpaque) layerFlags |= layer_state_t::eLayerOpaque;
    if (args.flags & ISurfaceComposerClient::eSecure) layerFlags |= layer_state_t::eLayerSecure;
    if (args.flags & ISurfaceComposerClient::eSkipScreenshot)
        layerFlags |= layer_state_t::eLayerSkipScreenshot;
    mCurrentState.active_legacy.w = args.w;
    mCurrentState.active_legacy.h = args.h;
    mCurrentState.flags = layerFlags;
    mCurrentState.active_legacy.transform.set(0, 0);
    mCurrentState.crop_legacy.makeInvalid();
    mCurrentState.requestedCrop_legacy = mCurrentState.crop_legacy;
    mCurrentState.z = 0;
    mCurrentState.color.a = 1.0f;
    mCurrentState.layerStack = 0;
    mCurrentState.sequence = 0;
    mCurrentState.requested_legacy = mCurrentState.active_legacy;
    mCurrentState.active.w = UINT32_MAX;
    mCurrentState.active.h = UINT32_MAX;
    mCurrentState.active.transform.set(0, 0);
    mCurrentState.frameNumber = 0;
    mCurrentState.transform = 0;
    mCurrentState.transformToDisplayInverse = false;
    mCurrentState.crop.makeInvalid();
    mCurrentState.acquireFence = new Fence(-1);
    mCurrentState.dataspace = ui::Dataspace::UNKNOWN;
    mCurrentState.hdrMetadata.validTypes = 0;
    mCurrentState.surfaceDamageRegion = Region::INVALID_REGION;
    mCurrentState.cornerRadius = 0.0f;
    mCurrentState.backgroundBlurRadius = 0;
    mCurrentState.api = -1;
    mCurrentState.hasColorTransform = false;
    mCurrentState.colorSpaceAgnostic = false;
    mCurrentState.frameRateSelectionPriority = PRIORITY_UNSET;
    mCurrentState.metadata = args.metadata;
    mCurrentState.shadowRadius = 0.f;
    mCurrentState.treeHasFrameRateVote = false;
    mCurrentState.fixedTransformHint = ui::Transform::ROT_INVALID;
    mCurrentState.frameTimelineVsyncId = ISurfaceComposer::INVALID_VSYNC_ID;
    mCurrentState.postTime = -1;
    if (args.flags & ISurfaceComposerClient::eNoColorFill) {
        mCurrentState.color.r = -1.0_hf;
        mCurrentState.color.g = -1.0_hf;
        mCurrentState.color.b = -1.0_hf;
    }
    mDrawingState = mCurrentState;
    CompositorTiming compositorTiming;
    args.flinger->getCompositorTiming(&compositorTiming);
    mFrameEventHistory.initializeCompositorTiming(compositorTiming);
    mFrameTracker.setDisplayRefreshPeriod(compositorTiming.interval);
    mCallingPid = args.callingPid;
    mCallingUid = args.callingUid;
    if (mCallingUid == AID_GRAPHICS || mCallingUid == AID_SYSTEM) {
        mOwnerUid = args.metadata.getInt32(METADATA_OWNER_UID, mCallingUid);
        mOwnerPid = args.metadata.getInt32(METADATA_OWNER_PID, mCallingPid);
    } else {
        mOwnerUid = mCallingUid;
        mOwnerPid = mCallingPid;
    }
}
void Layer::onFirstRef() {
    mFlinger->onLayerFirstRef(this);
}
Layer::~Layer() {
    sp<Client> c(mClientRef.promote());
    if (c != 0) {
        c->detachLayer(this);
    }
    mFrameTracker.logAndResetStats(mName);
    mFlinger->onLayerDestroyed(this);
}
LayerCreationArgs::LayerCreationArgs(SurfaceFlinger* flinger, sp<Client> client, std::string name,
                                     uint32_t w, uint32_t h, uint32_t flags, LayerMetadata metadata)
      : flinger(flinger),
        client(std::move(client)),
        name(std::move(name)),
        w(w),
        h(h),
        flags(flags),
        metadata(std::move(metadata)) {
    IPCThreadState* ipc = IPCThreadState::self();
    callingPid = ipc->getCallingPid();
    callingUid = ipc->getCallingUid();
}
void Layer::onLayerDisplayed(const sp<Fence>& ) {}
void Layer::removeRemoteSyncPoints() {
    for (auto& point : mRemoteSyncPoints) {
        point->setTransactionApplied();
    }
    mRemoteSyncPoints.clear();
    {
        for (State pendingState : mPendingStates) {
            pendingState.barrierLayer_legacy = nullptr;
        }
    }
}
void Layer::removeRelativeZ(const std::vector<Layer*>& layersInTree) {
    if (mCurrentState.zOrderRelativeOf == nullptr) {
        return;
    }
    sp<Layer> strongRelative = mCurrentState.zOrderRelativeOf.promote();
    if (strongRelative == nullptr) {
        setZOrderRelativeOf(nullptr);
        return;
    }
    if (!std::binary_search(layersInTree.begin(), layersInTree.end(), strongRelative.get())) {
        strongRelative->removeZOrderRelative(this);
        mFlinger->setTransactionFlags(eTraversalNeeded);
        setZOrderRelativeOf(nullptr);
    }
}
void Layer::removeFromCurrentState() {
    mRemovedFromCurrentState = true;
    removeRemoteSyncPoints();
    {
    Mutex::Autolock syncLock(mLocalSyncPointMutex);
    for (auto& point : mLocalSyncPoints) {
        point->setFrameAvailable();
    }
    mLocalSyncPoints.clear();
    }
    mFlinger->markLayerPendingRemovalLocked(this);
}
sp<Layer> Layer::getRootLayer() {
    sp<Layer> parent = getParent();
    if (parent == nullptr) {
        return this;
    }
    return parent->getRootLayer();
}
void Layer::onRemovedFromCurrentState() {
    auto layersInTree = getRootLayer()->getLayersInTree(LayerVector::StateSet::Current);
    std::sort(layersInTree.begin(), layersInTree.end());
    traverse(LayerVector::StateSet::Current, [&](Layer* layer) {
        layer->removeFromCurrentState();
        layer->removeRelativeZ(layersInTree);
    });
}
void Layer::addToCurrentState() {
    mRemovedFromCurrentState = false;
    for (const auto& child : mCurrentChildren) {
        child->addToCurrentState();
    }
}
bool Layer::getPremultipledAlpha() const {
    return mPremultipliedAlpha;
}
sp<IBinder> Layer::getHandle() {
    Mutex::Autolock _l(mLock);
    if (mGetHandleCalled) {
        ALOGE("Get handle called twice" );
        return nullptr;
    }
    mGetHandleCalled = true;
    return new Handle(mFlinger, this);
}
static Rect reduce(const Rect& win, const Region& exclude) {
    if (CC_LIKELY(exclude.isEmpty())) {
        return win;
    }
    if (exclude.isRect()) {
        return win.reduce(exclude.getBounds());
    }
    return Region(win).subtract(exclude).getBounds();
}
static FloatRect reduce(const FloatRect& win, const Region& exclude) {
    if (CC_LIKELY(exclude.isEmpty())) {
        return win;
    }
    return Region(Rect{win}).subtract(exclude).getBounds().toFloatRect();
}
Rect Layer::getScreenBounds(bool reduceTransparentRegion) const {
    if (!reduceTransparentRegion) {
        return Rect{mScreenBounds};
    }
    FloatRect bounds = getBounds();
    ui::Transform t = getTransform();
    bounds = t.transform(bounds);
    return Rect{bounds};
}
FloatRect Layer::getBounds() const {
    const State& s(getDrawingState());
    return getBounds(getActiveTransparentRegion(s));
}
FloatRect Layer::getBounds(const Region& activeTransparentRegion) const {
    return reduce(mBounds, activeTransparentRegion);
}
ui::Transform Layer::getBufferScaleTransform() const {
    if (!isFixedSize() || getBuffer() == nullptr) {
        return {};
    }
    const uint32_t activeWidth = getActiveWidth(getDrawingState());
    const uint32_t activeHeight = getActiveHeight(getDrawingState());
    if (activeWidth >= UINT32_MAX && activeHeight >= UINT32_MAX) {
        return {};
    }
    int bufferWidth = getBuffer()->getWidth();
    int bufferHeight = getBuffer()->getHeight();
    if (getBufferTransform() & NATIVE_WINDOW_TRANSFORM_ROT_90) {
        std::swap(bufferWidth, bufferHeight);
    }
    float sx = activeWidth / static_cast<float>(bufferWidth);
    float sy = activeHeight / static_cast<float>(bufferHeight);
    ui::Transform extraParentScaling;
    extraParentScaling.set(sx, 0, 0, sy);
    return extraParentScaling;
}
ui::Transform Layer::getTransformWithScale(const ui::Transform& bufferScaleTransform) const {
    if (!isFixedSize() || getBuffer() == nullptr) {
        return mEffectiveTransform;
    }
    return mEffectiveTransform * bufferScaleTransform;
}
FloatRect Layer::getBoundsPreScaling(const ui::Transform& bufferScaleTransform) const {
    if (!isFixedSize() || getBuffer() == nullptr) {
        return mBounds;
    }
    return bufferScaleTransform.inverse().transform(mBounds);
}
void Layer::computeBounds(FloatRect parentBounds, ui::Transform parentTransform,
                          float parentShadowRadius) {
    const State& s(getDrawingState());
    mEffectiveTransform = parentTransform * getActiveTransform(s);
    parentBounds = getActiveTransform(s).inverse().transform(parentBounds);
    mSourceBounds = computeSourceBounds(parentBounds);
    FloatRect bounds = mSourceBounds;
    const Rect layerCrop = getCrop(s);
    if (!layerCrop.isEmpty()) {
        bounds = mSourceBounds.intersect(layerCrop.toFloatRect());
    }
    bounds = bounds.intersect(parentBounds);
    mBounds = bounds;
    mScreenBounds = mEffectiveTransform.transform(mBounds);
    if (s.shadowRadius > 0.f) {
        mEffectiveShadowRadius = s.shadowRadius;
    } else {
        mEffectiveShadowRadius = parentShadowRadius;
    }
    const float childShadowRadius = canDrawShadows() ? 0.f : mEffectiveShadowRadius;
    ui::Transform bufferScaleTransform = getBufferScaleTransform();
    for (const sp<Layer>& child : mDrawingChildren) {
        child->computeBounds(getBoundsPreScaling(bufferScaleTransform),
                             getTransformWithScale(bufferScaleTransform), childShadowRadius);
    }
}
Rect Layer::getCroppedBufferSize(const State& s) const {
    Rect size = getBufferSize(s);
    Rect crop = getCrop(s);
    if (!crop.isEmpty() && size.isValid()) {
        size.intersect(crop, &size);
    } else if (!crop.isEmpty()) {
        size = crop;
    }
    return size;
}
void Layer::setupRoundedCornersCropCoordinates(Rect win,
                                               const FloatRect& roundedCornersCrop) const {
    win.left -= roundedCornersCrop.left;
    win.right -= roundedCornersCrop.left;
    win.top -= roundedCornersCrop.top;
    win.bottom -= roundedCornersCrop.top;
}
void Layer::prepareBasicGeometryCompositionState() {
    const auto& drawingState{getDrawingState()};
    const uint32_t layerStack = getLayerStack();
    const auto alpha = static_cast<float>(getAlpha());
    const bool opaque = isOpaque(drawingState);
    const bool usesRoundedCorners = getRoundedCornerState().radius != 0.f;
    auto blendMode = Hwc2::IComposerClient::BlendMode::NONE;
    if (!opaque || alpha != 1.0f) {
        blendMode = mPremultipliedAlpha ? Hwc2::IComposerClient::BlendMode::PREMULTIPLIED
                                        : Hwc2::IComposerClient::BlendMode::COVERAGE;
    }
    auto* compositionState = editCompositionState();
    compositionState->layerStackId =
            (layerStack != ~0u) ? std::make_optional(layerStack) : std::nullopt;
    compositionState->internalOnly = getPrimaryDisplayOnly();
    compositionState->isVisible = isVisible();
    compositionState->isOpaque = opaque && !usesRoundedCorners && alpha == 1.f;
    compositionState->shadowRadius = mEffectiveShadowRadius;
    compositionState->contentDirty = contentDirty;
    contentDirty = false;
    compositionState->geomLayerBounds = mBounds;
    compositionState->geomLayerTransform = getTransform();
    compositionState->geomInverseLayerTransform = compositionState->geomLayerTransform.inverse();
    compositionState->transparentRegionHint = getActiveTransparentRegion(drawingState);
    compositionState->blendMode = static_cast<Hwc2::IComposerClient::BlendMode>(blendMode);
    compositionState->alpha = alpha;
    compositionState->backgroundBlurRadius = drawingState.backgroundBlurRadius;
    compositionState->blurRegions = drawingState.blurRegions;
}
void Layer::prepareGeometryCompositionState() {
    const auto& drawingState{getDrawingState()};
    int type = drawingState.metadata.getInt32(METADATA_WINDOW_TYPE, 0);
    int appId = drawingState.metadata.getInt32(METADATA_OWNER_UID, 0);
    sp<Layer> parent = mDrawingParent.promote();
    if (parent.get()) {
        auto& parentState = parent->getDrawingState();
        const int parentType = parentState.metadata.getInt32(METADATA_WINDOW_TYPE, 0);
        const int parentAppId = parentState.metadata.getInt32(METADATA_OWNER_UID, 0);
        if (parentType > 0 && parentAppId > 0) {
            type = parentType;
            appId = parentAppId;
        }
    }
    auto* compositionState = editCompositionState();
    compositionState->geomBufferSize = getBufferSize(drawingState);
    compositionState->geomContentCrop = getBufferCrop();
    compositionState->geomCrop = getCrop(drawingState);
    compositionState->geomBufferTransform = getBufferTransform();
    compositionState->geomBufferUsesDisplayInverseTransform = getTransformToDisplayInverse();
    compositionState->geomUsesSourceCrop = usesSourceCrop();
    compositionState->isSecure = isSecure();
    compositionState->metadata.clear();
    const auto& supportedMetadata = mFlinger->getHwComposer().getSupportedLayerGenericMetadata();
    for (const auto& [key, mandatory] : supportedMetadata) {
        const auto& genericLayerMetadataCompatibilityMap =
                mFlinger->getGenericLayerMetadataKeyMap();
        auto compatIter = genericLayerMetadataCompatibilityMap.find(key);
        if (compatIter == std::end(genericLayerMetadataCompatibilityMap)) {
            continue;
        }
        const uint32_t id = compatIter->second;
        auto it = drawingState.metadata.mMap.find(id);
        if (it == std::end(drawingState.metadata.mMap)) {
            continue;
        }
        compositionState->metadata
                .emplace(key, compositionengine::GenericLayerMetadataEntry{mandatory, it->second});
    }
}
void Layer::preparePerFrameCompositionState() {
    const auto& drawingState{getDrawingState()};
    auto* compositionState = editCompositionState();
    compositionState->forceClientComposition = false;
    compositionState->isColorspaceAgnostic = isColorSpaceAgnostic();
    compositionState->dataspace = getDataSpace();
    compositionState->colorTransform = getColorTransform();
    compositionState->colorTransformIsIdentity = !hasColorTransform();
    compositionState->surfaceDamage = surfaceDamageRegion;
    compositionState->hasProtectedContent = isProtected();
    const bool usesRoundedCorners = getRoundedCornerState().radius != 0.f;
    compositionState->isOpaque =
            isOpaque(drawingState) && !usesRoundedCorners && getAlpha() == 1.0_hf;
    if (isHdrY410() || usesRoundedCorners || drawShadows() ||
        getDrawingState().blurRegions.size() > 0) {
        compositionState->forceClientComposition = true;
    }
}
void Layer::prepareCursorCompositionState() {
    const State& drawingState{getDrawingState()};
    auto* compositionState = editCompositionState();
    Rect win = getCroppedBufferSize(drawingState);
    Rect bounds = reduce(win, getActiveTransparentRegion(drawingState));
    Rect frame(getTransform().transform(bounds));
    compositionState->cursorFrame = frame;
}
sp<compositionengine::LayerFE> Layer::asLayerFE() const {
    return const_cast<compositionengine::LayerFE*>(
            static_cast<const compositionengine::LayerFE*>(this));
}
sp<compositionengine::LayerFE> Layer::getCompositionEngineLayerFE() const {
    return nullptr;
}
compositionengine::LayerFECompositionState* Layer::editCompositionState() {
    return nullptr;
}
const compositionengine::LayerFECompositionState* Layer::getCompositionState() const {
    return nullptr;
}
bool Layer::onPreComposition(nsecs_t) {
    return false;
}
void Layer::prepareCompositionState(compositionengine::LayerFE::StateSubset subset) {
    using StateSubset = compositionengine::LayerFE::StateSubset;
    switch (subset) {
        case StateSubset::BasicGeometry:
            prepareBasicGeometryCompositionState();
            break;
        case StateSubset::GeometryAndContent:
            prepareBasicGeometryCompositionState();
            prepareGeometryCompositionState();
            preparePerFrameCompositionState();
            break;
        case StateSubset::Content:
            preparePerFrameCompositionState();
            break;
        case StateSubset::Cursor:
            prepareCursorCompositionState();
            break;
    }
}
const char* Layer::getDebugName() const {
    return mName.c_str();
}
std::optional<compositionengine::LayerFE::LayerSettings> Layer::prepareClientComposition(
        compositionengine::LayerFE::ClientCompositionTargetSettings& targetSettings) {
    if (!getCompositionState()) {
        return {};
    }
    FloatRect bounds = getBounds();
    half alpha = getAlpha();
    compositionengine::LayerFE::LayerSettings layerSettings;
    layerSettings.geometry.boundaries = bounds;
    layerSettings.geometry.positionTransform = getTransform().asMatrix4();
    if (hasColorTransform()) {
        layerSettings.colorTransform = getColorTransform();
    }
    const auto roundedCornerState = getRoundedCornerState();
    layerSettings.geometry.roundedCornersRadius = roundedCornerState.radius;
    layerSettings.geometry.roundedCornersCrop = roundedCornerState.cropRect;
    layerSettings.alpha = alpha;
    layerSettings.sourceDataspace = getDataSpace();
    if (!targetSettings.disableBlurs) {
        layerSettings.backgroundBlurRadius = getBackgroundBlurRadius();
        layerSettings.blurRegions = getBlurRegions();
    }
    return layerSettings;
}
std::optional<compositionengine::LayerFE::LayerSettings> Layer::prepareShadowClientComposition(
        const LayerFE::LayerSettings& casterLayerSettings, const Rect& layerStackRect,
        ui::Dataspace outputDataspace) {
    renderengine::ShadowSettings shadow = getShadowSettings(layerStackRect);
    if (shadow.length <= 0.f) {
        return {};
    }
    const float casterAlpha = casterLayerSettings.alpha;
    const bool casterIsOpaque = ((casterLayerSettings.source.buffer.buffer != nullptr) &&
                                 casterLayerSettings.source.buffer.isOpaque);
    compositionengine::LayerFE::LayerSettings shadowLayer = casterLayerSettings;
    shadowLayer.shadow = shadow;
    shadowLayer.geometry.boundaries = mBounds;
    shadowLayer.shadow.casterIsTranslucent = !casterIsOpaque || (casterAlpha < 1.0f);
    shadowLayer.shadow.ambientColor *= casterAlpha;
    shadowLayer.shadow.spotColor *= casterAlpha;
    shadowLayer.sourceDataspace = outputDataspace;
    shadowLayer.source.buffer.buffer = nullptr;
    shadowLayer.source.buffer.fence = nullptr;
    shadowLayer.frameNumber = 0;
    shadowLayer.bufferId = 0;
    if (shadowLayer.shadow.ambientColor.a <= 0.f && shadowLayer.shadow.spotColor.a <= 0.f) {
        return {};
    }
    float casterCornerRadius = shadowLayer.geometry.roundedCornersRadius;
    const FloatRect& cornerRadiusCropRect = shadowLayer.geometry.roundedCornersCrop;
    const FloatRect& casterRect = shadowLayer.geometry.boundaries;
    if (casterCornerRadius > 0.f) {
        float cropRectOffset = std::max(std::abs(cornerRadiusCropRect.top - casterRect.top),
                                        std::abs(cornerRadiusCropRect.left - casterRect.left));
        if (cropRectOffset > casterCornerRadius) {
            casterCornerRadius = 0;
        } else {
            casterCornerRadius -= cropRectOffset;
        }
        shadowLayer.geometry.roundedCornersRadius = casterCornerRadius;
    }
    return shadowLayer;
}
void Layer::prepareClearClientComposition(LayerFE::LayerSettings& layerSettings,
                                          bool blackout) const {
    layerSettings.source.buffer.buffer = nullptr;
    layerSettings.source.solidColor = half3(0.0, 0.0, 0.0);
    layerSettings.disableBlending = true;
    layerSettings.bufferId = 0;
    layerSettings.frameNumber = 0;
    layerSettings.alpha = blackout ? 1.0f : 0.0f;
}
std::vector<compositionengine::LayerFE::LayerSettings> Layer::prepareClientCompositionList(
        compositionengine::LayerFE::ClientCompositionTargetSettings& targetSettings) {
    std::optional<compositionengine::LayerFE::LayerSettings> layerSettings =
            prepareClientComposition(targetSettings);
    if (!layerSettings) {
        return {};
    }
    if (targetSettings.clearContent) {
        prepareClearClientComposition(*layerSettings, false );
        return {*layerSettings};
    }
    std::optional<compositionengine::LayerFE::LayerSettings> shadowSettings =
            prepareShadowClientComposition(*layerSettings, targetSettings.viewport,
                                           targetSettings.dataspace);
    if (!shadowSettings) {
        return {*layerSettings};
    }
    if (targetSettings.realContentIsVisible) {
        return {*shadowSettings, *layerSettings};
    }
    return {*shadowSettings};
}
Hwc2::IComposerClient::Composition Layer::getCompositionType(const DisplayDevice& display) const {
    const auto outputLayer = findOutputLayerForDisplay(&display);
    if (outputLayer == nullptr) {
        return Hwc2::IComposerClient::Composition::INVALID;
    }
    if (outputLayer->getState().hwc) {
        return (*outputLayer->getState().hwc).hwcCompositionType;
    } else {
        return Hwc2::IComposerClient::Composition::CLIENT;
    }
}
bool Layer::addSyncPoint(const std::shared_ptr<SyncPoint>& point) {
    if (point->getFrameNumber() <= mCurrentFrameNumber) {
        return false;
    }
    if (isRemovedFromCurrentState()) {
        return false;
    }
    Mutex::Autolock lock(mLocalSyncPointMutex);
    mLocalSyncPoints.push_back(point);
    return true;
}
bool Layer::isSecure() const {
    const State& s(mDrawingState);
    if (s.flags & layer_state_t::eLayerSecure) {
        return true;
    }
    const auto p = mDrawingParent.promote();
    return (p != nullptr) ? p->isSecure() : false;
}
void Layer::pushPendingState() {
    if (!mCurrentState.modified) {
        return;
    }
    ATRACE_CALL();
    if (mCurrentState.barrierLayer_legacy != nullptr && !isRemovedFromCurrentState()) {
        sp<Layer> barrierLayer = mCurrentState.barrierLayer_legacy.promote();
        if (barrierLayer == nullptr) {
            ALOGE("[%s] Unable to promote barrier Layer.", getDebugName());
            mCurrentState.barrierLayer_legacy = nullptr;
        } else {
            auto syncPoint = std::make_shared<SyncPoint>(mCurrentState.barrierFrameNumber, this,
                                                         barrierLayer);
            if (barrierLayer->addSyncPoint(syncPoint)) {
                std::stringstream ss;
                ss << "Adding sync point " << mCurrentState.barrierFrameNumber;
                ATRACE_NAME(ss.str().c_str());
                mRemoteSyncPoints.push_back(std::move(syncPoint));
            } else {
                mCurrentState.barrierLayer_legacy = nullptr;
            }
        }
        setTransactionFlags(eTransactionNeeded);
        mFlinger->setTransactionFlags(eTraversalNeeded);
    }
    mPendingStates.push_back(mCurrentState);
    ATRACE_INT(mTransactionName.c_str(), mPendingStates.size());
}
void Layer::popPendingState(State* stateToCommit) {
    ATRACE_CALL();
    *stateToCommit = mPendingStates[0];
    mPendingStates.pop_front();
    ATRACE_INT(mTransactionName.c_str(), mPendingStates.size());
}
bool Layer::applyPendingStates(State* stateToCommit) {
    bool stateUpdateAvailable = false;
    while (!mPendingStates.empty()) {
        if (mPendingStates[0].barrierLayer_legacy != nullptr) {
            if (mRemoteSyncPoints.empty()) {
                ALOGV("[%s] No local sync point found", getDebugName());
                popPendingState(stateToCommit);
                stateUpdateAvailable = true;
                continue;
            }
            if (mRemoteSyncPoints.front()->getFrameNumber() !=
                mPendingStates[0].barrierFrameNumber) {
                ALOGE("[%s] Unexpected sync point frame number found", getDebugName());
                mRemoteSyncPoints.front()->setTransactionApplied();
                mRemoteSyncPoints.pop_front();
                continue;
            }
            if (mRemoteSyncPoints.front()->frameIsAvailable()) {
                ATRACE_NAME("frameIsAvailable");
                popPendingState(stateToCommit);
                stateUpdateAvailable = true;
                mRemoteSyncPoints.front()->setTransactionApplied();
                mRemoteSyncPoints.pop_front();
            } else {
                ATRACE_NAME("!frameIsAvailable");
                mRemoteSyncPoints.front()->checkTimeoutAndLog();
                break;
            }
        } else {
            popPendingState(stateToCommit);
            stateUpdateAvailable = true;
        }
    }
    if (!mPendingStates.empty()) {
        setTransactionFlags(eTransactionNeeded);
        mFlinger->setTraversalNeeded();
    }
    if (stateUpdateAvailable) {
        const auto vsyncId =
                stateToCommit->frameTimelineVsyncId == ISurfaceComposer::INVALID_VSYNC_ID
                ? std::nullopt
                : std::make_optional(stateToCommit->frameTimelineVsyncId);
        mSurfaceFrame =
                mFlinger->mFrameTimeline->createSurfaceFrameForToken(getOwnerPid(), getOwnerUid(),
                                                                     mName, mTransactionName,
                                                                     vsyncId);
        mSurfaceFrame->setActualQueueTime(stateToCommit->postTime);
        mSurfaceFrame->setAcquireFenceTime(stateToCommit->postTime);
        onSurfaceFrameCreated(mSurfaceFrame);
    }
    mCurrentState.modified = false;
    return stateUpdateAvailable;
}
uint32_t Layer::doTransactionResize(uint32_t flags, State* stateToCommit) {
    const State& s(getDrawingState());
    const bool sizeChanged = (stateToCommit->requested_legacy.w != s.requested_legacy.w) ||
            (stateToCommit->requested_legacy.h != s.requested_legacy.h);
    if (sizeChanged) {
        ALOGD_IF(DEBUG_RESIZE,
                 "doTransaction: geometry (layer=%p '%s'), tr=%02x, scalingMode=%d\n"
                 "  current={ active   ={ wh={%4u,%4u} crop={%4d,%4d,%4d,%4d} (%4d,%4d) }\n"
                 "            requested={ wh={%4u,%4u} }}\n"
                 "  drawing={ active   ={ wh={%4u,%4u} crop={%4d,%4d,%4d,%4d} (%4d,%4d) }\n"
                 "            requested={ wh={%4u,%4u} }}\n",
                 this, getName().c_str(), getBufferTransform(), getEffectiveScalingMode(),
                 stateToCommit->active_legacy.w, stateToCommit->active_legacy.h,
                 stateToCommit->crop_legacy.left, stateToCommit->crop_legacy.top,
                 stateToCommit->crop_legacy.right, stateToCommit->crop_legacy.bottom,
                 stateToCommit->crop_legacy.getWidth(), stateToCommit->crop_legacy.getHeight(),
                 stateToCommit->requested_legacy.w, stateToCommit->requested_legacy.h,
                 s.active_legacy.w, s.active_legacy.h, s.crop_legacy.left, s.crop_legacy.top,
                 s.crop_legacy.right, s.crop_legacy.bottom, s.crop_legacy.getWidth(),
                 s.crop_legacy.getHeight(), s.requested_legacy.w, s.requested_legacy.h);
    }
    const bool resizePending =
            ((stateToCommit->requested_legacy.w != stateToCommit->active_legacy.w) ||
             (stateToCommit->requested_legacy.h != stateToCommit->active_legacy.h)) &&
            (getBuffer() != nullptr);
    if (!isFixedSize()) {
        if (resizePending && mSidebandStream == nullptr) {
            flags |= eDontUpdateGeometryState;
        }
    }
    if (!(flags & eDontUpdateGeometryState)) {
        State& editCurrentState(getCurrentState());
        editCurrentState.active_legacy = editCurrentState.requested_legacy;
        stateToCommit->active_legacy = stateToCommit->requested_legacy;
    }
    return flags;
}
uint32_t Layer::doTransaction(uint32_t flags) {
    ATRACE_CALL();
    if (mLayerDetached) {
        forceSendCallbacks();
        mCurrentState.callbackHandles = {};
        return flags;
    }
    if (mChildrenChanged) {
        flags |= eVisibleRegion;
        mChildrenChanged = false;
    }
    pushPendingState();
    State c = getCurrentState();
    if (!applyPendingStates(&c)) {
        return flags;
    }
    flags = doTransactionResize(flags, &c);
    const State& s(getDrawingState());
    if (getActiveGeometry(c) != getActiveGeometry(s)) {
        flags |= Layer::eVisibleRegion;
    }
    if (c.sequence != s.sequence) {
        flags |= eVisibleRegion;
        this->contentDirty = true;
        mNeedsFiltering = getActiveTransform(c).needsBilinearFiltering();
    }
    if (mCurrentState.inputInfoChanged) {
        flags |= eInputInfoChanged;
        mCurrentState.inputInfoChanged = false;
    }
    for (auto& handle : s.callbackHandles) {
        c.callbackHandles.push_back(handle);
    }
    commitTransaction(c);
    mPendingStatesSnapshot = mPendingStates;
    mCurrentState.callbackHandles = {};
    return flags;
}
void Layer::commitTransaction(const State& stateToCommit) {
    mDrawingState = stateToCommit;
    mFlinger->mFrameTimeline->addSurfaceFrame(mSurfaceFrame, PresentState::Presented);
}
uint32_t Layer::getTransactionFlags(uint32_t flags) {
    return mTransactionFlags.fetch_and(~flags) & flags;
}
uint32_t Layer::setTransactionFlags(uint32_t flags) {
    return mTransactionFlags.fetch_or(flags);
}
bool Layer::setPosition(float x, float y) {
    if (mCurrentState.requested_legacy.transform.tx() == x &&
        mCurrentState.requested_legacy.transform.ty() == y)
        return false;
    mCurrentState.sequence++;
    mCurrentState.requested_legacy.transform.set(x, y);
    mCurrentState.active_legacy.transform.set(x, y);
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setChildLayer(const sp<Layer>& childLayer, int32_t z) {
    ssize_t idx = mCurrentChildren.indexOf(childLayer);
    if (idx < 0) {
        return false;
    }
    if (childLayer->setLayer(z)) {
        mCurrentChildren.removeAt(idx);
        mCurrentChildren.add(childLayer);
        return true;
    }
    return false;
}
bool Layer::setChildRelativeLayer(const sp<Layer>& childLayer,
        const sp<IBinder>& relativeToHandle, int32_t relativeZ) {
    ssize_t idx = mCurrentChildren.indexOf(childLayer);
    if (idx < 0) {
        return false;
    }
    if (childLayer->setRelativeLayer(relativeToHandle, relativeZ)) {
        mCurrentChildren.removeAt(idx);
        mCurrentChildren.add(childLayer);
        return true;
    }
    return false;
}
bool Layer::setLayer(int32_t z) {
    if (mCurrentState.z == z && !usingRelativeZ(LayerVector::StateSet::Current)) return false;
    mCurrentState.sequence++;
    mCurrentState.z = z;
    mCurrentState.modified = true;
    if (mCurrentState.zOrderRelativeOf != nullptr) {
        sp<Layer> strongRelative = mCurrentState.zOrderRelativeOf.promote();
        if (strongRelative != nullptr) {
            strongRelative->removeZOrderRelative(this);
        }
        setZOrderRelativeOf(nullptr);
    }
    setTransactionFlags(eTransactionNeeded);
    return true;
}
void Layer::removeZOrderRelative(const wp<Layer>& relative) {
    mCurrentState.zOrderRelatives.remove(relative);
    mCurrentState.sequence++;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
}
void Layer::addZOrderRelative(const wp<Layer>& relative) {
    mCurrentState.zOrderRelatives.add(relative);
    mCurrentState.modified = true;
    mCurrentState.sequence++;
    setTransactionFlags(eTransactionNeeded);
}
void Layer::setZOrderRelativeOf(const wp<Layer>& relativeOf) {
    mCurrentState.zOrderRelativeOf = relativeOf;
    mCurrentState.sequence++;
    mCurrentState.modified = true;
    mCurrentState.isRelativeOf = relativeOf != nullptr;
    setTransactionFlags(eTransactionNeeded);
}
bool Layer::setRelativeLayer(const sp<IBinder>& relativeToHandle, int32_t relativeZ) {
    sp<Handle> handle = static_cast<Handle*>(relativeToHandle.get());
    if (handle == nullptr) {
        return false;
    }
    sp<Layer> relative = handle->owner.promote();
    if (relative == nullptr) {
        return false;
    }
    if (mCurrentState.z == relativeZ && usingRelativeZ(LayerVector::StateSet::Current) &&
        mCurrentState.zOrderRelativeOf == relative) {
        return false;
    }
    mCurrentState.sequence++;
    mCurrentState.modified = true;
    mCurrentState.z = relativeZ;
    auto oldZOrderRelativeOf = mCurrentState.zOrderRelativeOf.promote();
    if (oldZOrderRelativeOf != nullptr) {
        oldZOrderRelativeOf->removeZOrderRelative(this);
    }
    setZOrderRelativeOf(relative);
    relative->addZOrderRelative(this);
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setSize(uint32_t w, uint32_t h) {
    if (mCurrentState.requested_legacy.w == w && mCurrentState.requested_legacy.h == h)
        return false;
    mCurrentState.requested_legacy.w = w;
    mCurrentState.requested_legacy.h = h;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    setDefaultBufferSize(mCurrentState.requested_legacy.w, mCurrentState.requested_legacy.h);
    return true;
}
bool Layer::setAlpha(float alpha) {
    if (mCurrentState.color.a == alpha) return false;
    mCurrentState.sequence++;
    mCurrentState.color.a = alpha;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setBackgroundColor(const half3& color, float alpha, ui::Dataspace dataspace) {
    if (!mCurrentState.bgColorLayer && alpha == 0) {
        return false;
    }
    mCurrentState.sequence++;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    if (!mCurrentState.bgColorLayer && alpha != 0) {
        uint32_t flags = ISurfaceComposerClient::eFXSurfaceEffect;
        std::string name = mName + "BackgroundColorLayer";
        mCurrentState.bgColorLayer = mFlinger->getFactory().createEffectLayer(
                LayerCreationArgs(mFlinger.get(), nullptr, std::move(name), 0, 0, flags,
                                  LayerMetadata()));
        addChild(mCurrentState.bgColorLayer);
        mFlinger->mLayersAdded = true;
        if (isRemovedFromCurrentState()) {
            mCurrentState.bgColorLayer->onRemovedFromCurrentState();
        }
        mFlinger->setTransactionFlags(eTransactionNeeded);
    } else if (mCurrentState.bgColorLayer && alpha == 0) {
        mCurrentState.bgColorLayer->reparent(nullptr);
        mCurrentState.bgColorLayer = nullptr;
        return true;
    }
    mCurrentState.bgColorLayer->setColor(color);
    mCurrentState.bgColorLayer->setLayer(std::numeric_limits<int32_t>::min());
    mCurrentState.bgColorLayer->setAlpha(alpha);
    mCurrentState.bgColorLayer->setDataspace(dataspace);
    return true;
}
bool Layer::setCornerRadius(float cornerRadius) {
    if (mCurrentState.cornerRadius == cornerRadius) return false;
    mCurrentState.sequence++;
    mCurrentState.cornerRadius = cornerRadius;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setBackgroundBlurRadius(int backgroundBlurRadius) {
    if (mCurrentState.backgroundBlurRadius == backgroundBlurRadius) return false;
    mCurrentState.sequence++;
    mCurrentState.backgroundBlurRadius = backgroundBlurRadius;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setMatrix(const layer_state_t::matrix22_t& matrix,
        bool allowNonRectPreservingTransforms) {
    ui::Transform t;
    t.set(matrix.dsdx, matrix.dtdy, matrix.dtdx, matrix.dsdy);
    if (!allowNonRectPreservingTransforms && !t.preserveRects()) {
        ALOGW("Attempt to set rotation matrix without permission ACCESS_SURFACE_FLINGER nor "
              "ROTATE_SURFACE_FLINGER ignored");
        return false;
    }
    mCurrentState.sequence++;
    mCurrentState.requested_legacy.transform.set(matrix.dsdx, matrix.dtdy, matrix.dtdx,
                                                 matrix.dsdy);
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setTransparentRegionHint(const Region& transparent) {
    mCurrentState.requestedTransparentRegion_legacy = transparent;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setBlurRegions(const std::vector<BlurRegion>& blurRegions) {
    mCurrentState.sequence++;
    mCurrentState.blurRegions = blurRegions;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setFlags(uint8_t flags, uint8_t mask) {
    const uint32_t newFlags = (mCurrentState.flags & ~mask) | (flags & mask);
    if (mCurrentState.flags == newFlags) return false;
    mCurrentState.sequence++;
    mCurrentState.flags = newFlags;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setCrop_legacy(const Rect& crop) {
    if (mCurrentState.requestedCrop_legacy == crop) return false;
    mCurrentState.sequence++;
    mCurrentState.requestedCrop_legacy = crop;
    mCurrentState.crop_legacy = crop;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setMetadata(const LayerMetadata& data) {
    if (!mCurrentState.metadata.merge(data, true )) return false;
    mCurrentState.sequence++;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setLayerStack(uint32_t layerStack) {
    if (mCurrentState.layerStack == layerStack) return false;
    mCurrentState.sequence++;
    mCurrentState.layerStack = layerStack;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setColorSpaceAgnostic(const bool agnostic) {
    if (mCurrentState.colorSpaceAgnostic == agnostic) {
        return false;
    }
    mCurrentState.sequence++;
    mCurrentState.colorSpaceAgnostic = agnostic;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setFrameRateSelectionPriority(int32_t priority) {
    if (mCurrentState.frameRateSelectionPriority == priority) return false;
    mCurrentState.frameRateSelectionPriority = priority;
    mCurrentState.sequence++;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
int32_t Layer::getFrameRateSelectionPriority() const {
    if (mDrawingState.frameRateSelectionPriority != PRIORITY_UNSET) {
        return mDrawingState.frameRateSelectionPriority;
    }
    sp<Layer> parent = getParent();
    if (parent != nullptr) {
        return parent->getFrameRateSelectionPriority();
    }
    return Layer::PRIORITY_UNSET;
}
bool Layer::isLayerFocusedBasedOnPriority(int32_t priority) {
    return priority == PRIORITY_FOCUSED_WITH_MODE || priority == PRIORITY_FOCUSED_WITHOUT_MODE;
};
uint32_t Layer::getLayerStack() const {
    auto p = mDrawingParent.promote();
    if (p == nullptr) {
        return getDrawingState().layerStack;
    }
    return p->getLayerStack();
}
bool Layer::setShadowRadius(float shadowRadius) {
    if (mCurrentState.shadowRadius == shadowRadius) {
        return false;
    }
    mCurrentState.sequence++;
    mCurrentState.shadowRadius = shadowRadius;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setFixedTransformHint(ui::Transform::RotationFlags fixedTransformHint) {
    if (mCurrentState.fixedTransformHint == fixedTransformHint) {
        return false;
    }
    mCurrentState.sequence++;
    mCurrentState.fixedTransformHint = fixedTransformHint;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
void Layer::updateTreeHasFrameRateVote() {
    const auto traverseTree = [&](const LayerVector::Visitor& visitor) {
        auto parent = getParent();
        while (parent) {
            visitor(parent.get());
            parent = parent->getParent();
        }
        traverse(LayerVector::StateSet::Current, visitor);
    };
    int layersWithVote = 0;
    traverseTree([&layersWithVote](Layer* layer) {
        const auto layerVotedWithDefaultCompatibility =
                layer->mCurrentState.frameRate.rate.isValid() &&
                layer->mCurrentState.frameRate.type == FrameRateCompatibility::Default;
        const auto layerVotedWithNoVote =
                layer->mCurrentState.frameRate.type == FrameRateCompatibility::NoVote;
        if (layerVotedWithDefaultCompatibility || layerVotedWithNoVote) {
            layersWithVote++;
        }
    });
    bool transactionNeeded = false;
    traverseTree([layersWithVote, &transactionNeeded](Layer* layer) {
        if (layer->mCurrentState.treeHasFrameRateVote != layersWithVote > 0) {
            layer->mCurrentState.sequence++;
            layer->mCurrentState.treeHasFrameRateVote = layersWithVote > 0;
            layer->mCurrentState.modified = true;
            layer->setTransactionFlags(eTransactionNeeded);
            transactionNeeded = true;
        }
    });
    if (transactionNeeded) {
        mFlinger->setTransactionFlags(eTraversalNeeded);
    }
}
bool Layer::setFrameRate(FrameRate frameRate) {
    if (!mFlinger->useFrameRateApi) {
        return false;
    }
    if (mCurrentState.frameRate == frameRate) {
        return false;
    }
    mFlinger->mScheduler->recordLayerHistory(this, systemTime(),
                                             LayerHistory::LayerUpdateType::SetFrameRate);
    mCurrentState.sequence++;
    mCurrentState.frameRate = frameRate;
    mCurrentState.modified = true;
    updateTreeHasFrameRateVote();
    setTransactionFlags(eTransactionNeeded);
    return true;
}
void Layer::setFrameTimelineVsyncForTransaction(int64_t frameTimelineVsyncId, nsecs_t postTime) {
    mCurrentState.frameTimelineVsyncId = frameTimelineVsyncId;
    mCurrentState.postTime = postTime;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
}
Layer::FrameRate Layer::getFrameRateForLayerTree() const {
    const auto frameRate = getDrawingState().frameRate;
    if (frameRate.rate.isValid() || frameRate.type == FrameRateCompatibility::NoVote) {
        return frameRate;
    }
    if (getDrawingState().treeHasFrameRateVote) {
        return {Fps(0.0f), FrameRateCompatibility::NoVote};
    }
    return frameRate;
}
void Layer::deferTransactionUntil_legacy(const sp<Layer>& barrierLayer, uint64_t frameNumber) {
    ATRACE_CALL();
    if (mLayerDetached) {
        return;
    }
    mCurrentState.barrierLayer_legacy = barrierLayer;
    mCurrentState.barrierFrameNumber = frameNumber;
    mCurrentState.modified = true;
    pushPendingState();
    mCurrentState.barrierLayer_legacy = nullptr;
    mCurrentState.barrierFrameNumber = 0;
    mCurrentState.modified = false;
}
void Layer::deferTransactionUntil_legacy(const sp<IBinder>& barrierHandle, uint64_t frameNumber) {
    sp<Handle> handle = static_cast<Handle*>(barrierHandle.get());
    deferTransactionUntil_legacy(handle->owner.promote(), frameNumber);
}
bool Layer::isHiddenByPolicy() const {
    const State& s(mDrawingState);
    const auto& parent = mDrawingParent.promote();
    if (parent != nullptr && parent->isHiddenByPolicy()) {
        return true;
    }
    if (usingRelativeZ(LayerVector::StateSet::Drawing)) {
        auto zOrderRelativeOf = mDrawingState.zOrderRelativeOf.promote();
        if (zOrderRelativeOf != nullptr) {
            if (zOrderRelativeOf->isHiddenByPolicy()) {
                return true;
            }
        }
    }
    return s.flags & layer_state_t::eLayerHidden;
}
uint32_t Layer::getEffectiveUsage(uint32_t usage) const {
    if (mProtectedByApp) {
        usage |= GraphicBuffer::USAGE_PROTECTED;
    }
    if (mPotentialCursor) {
        usage |= GraphicBuffer::USAGE_CURSOR;
    }
    usage |= GraphicBuffer::USAGE_HW_COMPOSER;
    return usage;
}
void Layer::updateTransformHint(ui::Transform::RotationFlags transformHint) {
    if (mFlinger->mDebugDisableTransformHint || transformHint & ui::Transform::ROT_INVALID) {
        transformHint = ui::Transform::ROT_0;
    }
    setTransformHint(transformHint);
}
LayerDebugInfo Layer::getLayerDebugInfo(const DisplayDevice* display) const {
    using namespace std::string_literals;
    LayerDebugInfo info;
    const State& ds = getDrawingState();
    info.mName = getName();
    sp<Layer> parent = mDrawingParent.promote();
    info.mParentName = parent ? parent->getName() : "none"s;
    info.mType = getType();
    info.mTransparentRegion = ds.activeTransparentRegion_legacy;
    info.mVisibleRegion = getVisibleRegion(display);
    info.mSurfaceDamageRegion = surfaceDamageRegion;
    info.mLayerStack = getLayerStack();
    info.mX = ds.active_legacy.transform.tx();
    info.mY = ds.active_legacy.transform.ty();
    info.mZ = ds.z;
    info.mWidth = ds.active_legacy.w;
    info.mHeight = ds.active_legacy.h;
    info.mCrop = ds.crop_legacy;
    info.mColor = ds.color;
    info.mFlags = ds.flags;
    info.mPixelFormat = getPixelFormat();
    info.mDataSpace = static_cast<android_dataspace>(getDataSpace());
    info.mMatrix[0][0] = ds.active_legacy.transform[0][0];
    info.mMatrix[0][1] = ds.active_legacy.transform[0][1];
    info.mMatrix[1][0] = ds.active_legacy.transform[1][0];
    info.mMatrix[1][1] = ds.active_legacy.transform[1][1];
    {
        sp<const GraphicBuffer> buffer = getBuffer();
        if (buffer != 0) {
            info.mActiveBufferWidth = buffer->getWidth();
            info.mActiveBufferHeight = buffer->getHeight();
            info.mActiveBufferStride = buffer->getStride();
            info.mActiveBufferFormat = buffer->format;
        } else {
            info.mActiveBufferWidth = 0;
            info.mActiveBufferHeight = 0;
            info.mActiveBufferStride = 0;
            info.mActiveBufferFormat = 0;
        }
    }
    info.mNumQueuedFrames = getQueuedFrameCount();
    info.mRefreshPending = isBufferLatched();
    info.mIsOpaque = isOpaque(ds);
    info.mContentDirty = contentDirty;
    return info;
}
void Layer::miniDumpHeader(std::string& result) {
    result.append(kDumpTableRowLength, '-');
    result.append("\n");
    result.append(" Layer name\n");
    result.append("           Z | ");
    result.append(" Window Type | ");
    result.append(" Comp Type | ");
    result.append(" Transform | ");
    result.append("  Disp Frame (LTRB) | ");
    result.append("         Source Crop (LTRB) | ");
    result.append("    Frame Rate (Explicit) (Seamlessness) [Focused]\n");
    result.append(kDumpTableRowLength, '-');
    result.append("\n");
}
std::string Layer::frameRateCompatibilityString(Layer::FrameRateCompatibility compatibility) {
    switch (compatibility) {
        case FrameRateCompatibility::Default:
            return "Default";
        case FrameRateCompatibility::ExactOrMultiple:
            return "ExactOrMultiple";
        case FrameRateCompatibility::NoVote:
            return "NoVote";
    }
}
void Layer::miniDump(std::string& result, const DisplayDevice& display) const {
    const auto outputLayer = findOutputLayerForDisplay(&display);
    if (!outputLayer) {
        return;
    }
    std::string name;
    if (mName.length() > 77) {
        std::string shortened;
        shortened.append(mName, 0, 36);
        shortened.append("[...]");
        shortened.append(mName, mName.length() - 36);
        name = std::move(shortened);
    } else {
        name = mName;
    }
    StringAppendF(&result, " %s\n", name.c_str());
    const State& layerState(getDrawingState());
    const auto& outputLayerState = outputLayer->getState();
    if (layerState.zOrderRelativeOf != nullptr || mDrawingParent != nullptr) {
        StringAppendF(&result, "  rel %6d | ", layerState.z);
    } else {
        StringAppendF(&result, "  %10d | ", layerState.z);
    }
    StringAppendF(&result, "  %10d | ", mWindowType);
    StringAppendF(&result, "%10s | ", toString(getCompositionType(display)).c_str());
    StringAppendF(&result, "%10s | ", toString(outputLayerState.bufferTransform).c_str());
    const Rect& frame = outputLayerState.displayFrame;
    StringAppendF(&result, "%4d %4d %4d %4d | ", frame.left, frame.top, frame.right, frame.bottom);
    const FloatRect& crop = outputLayerState.sourceCrop;
    StringAppendF(&result, "%6.1f %6.1f %6.1f %6.1f | ", crop.left, crop.top, crop.right,
                  crop.bottom);
    if (layerState.frameRate.rate.isValid() ||
        layerState.frameRate.type != FrameRateCompatibility::Default) {
        StringAppendF(&result, "%s %15s %17s", to_string(layerState.frameRate.rate).c_str(),
                      frameRateCompatibilityString(layerState.frameRate.type).c_str(),
                      toString(layerState.frameRate.seamlessness).c_str());
    } else {
        result.append(41, ' ');
    }
    const auto focused = isLayerFocusedBasedOnPriority(getFrameRateSelectionPriority());
    StringAppendF(&result, "    [%s]\n", focused ? "*" : " ");
    result.append(kDumpTableRowLength, '-');
    result.append("\n");
}
void Layer::dumpFrameStats(std::string& result) const {
    mFrameTracker.dumpStats(result);
}
void Layer::clearFrameStats() {
    mFrameTracker.clearStats();
}
void Layer::logFrameStats() {
    mFrameTracker.logAndResetStats(mName);
}
void Layer::getFrameStats(FrameStats* outStats) const {
    mFrameTracker.getStats(outStats);
}
void Layer::dumpFrameEvents(std::string& result) {
    StringAppendF(&result, "- Layer %s (%s, %p)\n", getName().c_str(), getType(), this);
    Mutex::Autolock lock(mFrameEventHistoryMutex);
    mFrameEventHistory.checkFencesForCompletion();
    mFrameEventHistory.dump(result);
}
void Layer::dumpCallingUidPid(std::string& result) const {
    StringAppendF(&result, "Layer %s (%s) callingPid:%d callingUid:%d ownerUid:%d\n",
                  getName().c_str(), getType(), mCallingPid, mCallingUid, mOwnerUid);
}
void Layer::onDisconnect() {
    Mutex::Autolock lock(mFrameEventHistoryMutex);
    mFrameEventHistory.onDisconnect();
    const int32_t layerId = getSequence();
    mFlinger->mTimeStats->onDestroy(layerId);
    mFlinger->mFrameTracer->onDestroy(layerId);
}
void Layer::addAndGetFrameTimestamps(const NewFrameEventsEntry* newTimestamps,
                                     FrameEventHistoryDelta* outDelta) {
    if (newTimestamps) {
        mFlinger->mTimeStats->setPostTime(getSequence(), newTimestamps->frameNumber,
                                          getName().c_str(), mOwnerUid, newTimestamps->postedTime);
        mFlinger->mTimeStats->setAcquireFence(getSequence(), newTimestamps->frameNumber,
                                              newTimestamps->acquireFence);
    }
    Mutex::Autolock lock(mFrameEventHistoryMutex);
    if (newTimestamps) {
        mAcquireTimeline.updateSignalTimes();
        mAcquireTimeline.push(newTimestamps->acquireFence);
        mFrameEventHistory.addQueue(*newTimestamps);
    }
    if (outDelta) {
        mFrameEventHistory.getAndResetDelta(outDelta);
    }
}
size_t Layer::getChildrenCount() const {
    size_t count = 0;
    for (const sp<Layer>& child : mCurrentChildren) {
        count += 1 + child->getChildrenCount();
    }
    return count;
}
void Layer::addChild(const sp<Layer>& layer) {
    mChildrenChanged = true;
    setTransactionFlags(eTransactionNeeded);
    mCurrentChildren.add(layer);
    layer->setParent(this);
    updateTreeHasFrameRateVote();
}
ssize_t Layer::removeChild(const sp<Layer>& layer) {
    mChildrenChanged = true;
    setTransactionFlags(eTransactionNeeded);
    layer->setParent(nullptr);
    const auto removeResult = mCurrentChildren.remove(layer);
    updateTreeHasFrameRateVote();
    layer->updateTreeHasFrameRateVote();
    return removeResult;
}
void Layer::reparentChildren(const sp<Layer>& newParent) {
    if (attachChildren()) {
        setTransactionFlags(eTransactionNeeded);
    }
    for (const sp<Layer>& child : mCurrentChildren) {
        newParent->addChild(child);
    }
    mCurrentChildren.clear();
    updateTreeHasFrameRateVote();
}
bool Layer::reparentChildren(const sp<IBinder>& newParentHandle) {
    sp<Handle> handle = nullptr;
    sp<Layer> newParent = nullptr;
    if (newParentHandle == nullptr) {
        return false;
    }
    handle = static_cast<Handle*>(newParentHandle.get());
    newParent = handle->owner.promote();
    if (newParent == nullptr) {
        ALOGE("Unable to promote Layer handle");
        return false;
    }
    reparentChildren(newParent);
    return true;
}
void Layer::setChildrenDrawingParent(const sp<Layer>& newParent) {
    for (const sp<Layer>& child : mDrawingChildren) {
        child->mDrawingParent = newParent;
        child->computeBounds(newParent->mBounds,
                             newParent->getTransformWithScale(newParent->getBufferScaleTransform()),
                             newParent->mEffectiveShadowRadius);
    }
}
bool Layer::reparent(const sp<IBinder>& newParentHandle) {
    bool callSetTransactionFlags = false;
    if (mLayerDetached && newParentHandle == nullptr) {
        return false;
    }
    sp<Layer> newParent;
    if (newParentHandle != nullptr) {
        auto handle = static_cast<Handle*>(newParentHandle.get());
        newParent = handle->owner.promote();
        if (newParent == nullptr) {
            ALOGE("Unable to promote Layer handle");
            return false;
        }
        if (newParent == this) {
            ALOGE("Invalid attempt to reparent Layer (%s) to itself", getName().c_str());
            return false;
        }
    }
    sp<Layer> parent = getParent();
    if (parent != nullptr) {
        parent->removeChild(this);
    }
    if (newParentHandle != nullptr) {
        newParent->addChild(this);
        if (!newParent->isRemovedFromCurrentState()) {
            addToCurrentState();
        } else {
            onRemovedFromCurrentState();
        }
        if (mLayerDetached) {
            mLayerDetached = false;
            callSetTransactionFlags = true;
        }
    } else {
        onRemovedFromCurrentState();
    }
    if (attachChildren() || callSetTransactionFlags) {
        setTransactionFlags(eTransactionNeeded);
    }
    return true;
}
bool Layer::detachChildren() {
    for (const sp<Layer>& child : mCurrentChildren) {
        sp<Client> parentClient = mClientRef.promote();
        sp<Client> client(child->mClientRef.promote());
        if (client != nullptr && parentClient != client) {
            child->mLayerDetached = true;
            child->detachChildren();
            child->removeRemoteSyncPoints();
        }
    }
    return true;
}
bool Layer::attachChildren() {
    bool changed = false;
    for (const sp<Layer>& child : mCurrentChildren) {
        sp<Client> parentClient = mClientRef.promote();
        sp<Client> client(child->mClientRef.promote());
        if (client != nullptr && parentClient != client) {
            if (child->mLayerDetached) {
                child->mLayerDetached = false;
                child->attachChildren();
                changed = true;
            }
        }
    }
    return changed;
}
bool Layer::setColorTransform(const mat4& matrix) {
    static const mat4 identityMatrix = mat4();
    if (mCurrentState.colorTransform == matrix) {
        return false;
    }
    ++mCurrentState.sequence;
    mCurrentState.colorTransform = matrix;
    mCurrentState.hasColorTransform = matrix != identityMatrix;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
mat4 Layer::getColorTransform() const {
    mat4 colorTransform = mat4(getDrawingState().colorTransform);
    if (sp<Layer> parent = mDrawingParent.promote(); parent != nullptr) {
        colorTransform = parent->getColorTransform() * colorTransform;
    }
    return colorTransform;
}
bool Layer::hasColorTransform() const {
    bool hasColorTransform = getDrawingState().hasColorTransform;
    if (sp<Layer> parent = mDrawingParent.promote(); parent != nullptr) {
        hasColorTransform = hasColorTransform || parent->hasColorTransform();
    }
    return hasColorTransform;
}
bool Layer::isLegacyDataSpace() const {
    return !(getDataSpace() &
             (ui::Dataspace::STANDARD_MASK | ui::Dataspace::TRANSFER_MASK |
              ui::Dataspace::RANGE_MASK));
}
void Layer::setParent(const sp<Layer>& layer) {
    mCurrentParent = layer;
}
int32_t Layer::getZ(LayerVector::StateSet stateSet) const {
    const bool useDrawing = stateSet == LayerVector::StateSet::Drawing;
    const State& state = useDrawing ? mDrawingState : mCurrentState;
    return state.z;
}
bool Layer::usingRelativeZ(LayerVector::StateSet stateSet) const {
    const bool useDrawing = stateSet == LayerVector::StateSet::Drawing;
    const State& state = useDrawing ? mDrawingState : mCurrentState;
    return state.isRelativeOf;
}
__attribute__((no_sanitize("unsigned-integer-overflow"))) LayerVector Layer::makeTraversalList(
        LayerVector::StateSet stateSet, bool* outSkipRelativeZUsers) {
    LOG_ALWAYS_FATAL_IF(stateSet == LayerVector::StateSet::Invalid,
                        "makeTraversalList received invalid stateSet");
    const bool useDrawing = stateSet == LayerVector::StateSet::Drawing;
    const LayerVector& children = useDrawing ? mDrawingChildren : mCurrentChildren;
    const State& state = useDrawing ? mDrawingState : mCurrentState;
    if (state.zOrderRelatives.size() == 0) {
        *outSkipRelativeZUsers = true;
        return children;
    }
    LayerVector traverse(stateSet);
    for (const wp<Layer>& weakRelative : state.zOrderRelatives) {
        sp<Layer> strongRelative = weakRelative.promote();
        if (strongRelative != nullptr) {
            traverse.add(strongRelative);
        }
    }
    for (const sp<Layer>& child : children) {
        if (child->usingRelativeZ(stateSet)) {
            continue;
        }
        traverse.add(child);
    }
    return traverse;
}
void Layer::traverseInZOrder(LayerVector::StateSet stateSet, const LayerVector::Visitor& visitor) {
    bool skipRelativeZUsers = false;
    const LayerVector list = makeTraversalList(stateSet, &skipRelativeZUsers);
    size_t i = 0;
    for (; i < list.size(); i++) {
        const auto& relative = list[i];
        if (skipRelativeZUsers && relative->usingRelativeZ(stateSet)) {
            continue;
        }
        if (relative->getZ(stateSet) >= 0) {
            break;
        }
        relative->traverseInZOrder(stateSet, visitor);
    }
    visitor(this);
    for (; i < list.size(); i++) {
        const auto& relative = list[i];
        if (skipRelativeZUsers && relative->usingRelativeZ(stateSet)) {
            continue;
        }
        relative->traverseInZOrder(stateSet, visitor);
    }
}
void Layer::traverseInReverseZOrder(LayerVector::StateSet stateSet,
                                    const LayerVector::Visitor& visitor) {
    bool skipRelativeZUsers = false;
    LayerVector list = makeTraversalList(stateSet, &skipRelativeZUsers);
    int32_t i = 0;
    for (i = int32_t(list.size()) - 1; i >= 0; i--) {
        const auto& relative = list[i];
        if (skipRelativeZUsers && relative->usingRelativeZ(stateSet)) {
            continue;
        }
        if (relative->getZ(stateSet) < 0) {
            break;
        }
        relative->traverseInReverseZOrder(stateSet, visitor);
    }
    visitor(this);
    for (; i >= 0; i--) {
        const auto& relative = list[i];
        if (skipRelativeZUsers && relative->usingRelativeZ(stateSet)) {
            continue;
        }
        relative->traverseInReverseZOrder(stateSet, visitor);
    }
}
void Layer::traverse(LayerVector::StateSet state, const LayerVector::Visitor& visitor) {
    visitor(this);
    const LayerVector& children =
            state == LayerVector::StateSet::Drawing ? mDrawingChildren : mCurrentChildren;
    for (const sp<Layer>& child : children) {
        child->traverse(state, visitor);
    }
}
LayerVector Layer::makeChildrenTraversalList(LayerVector::StateSet stateSet,
                                             const std::vector<Layer*>& layersInTree) {
    LOG_ALWAYS_FATAL_IF(stateSet == LayerVector::StateSet::Invalid,
                        "makeTraversalList received invalid stateSet");
    const bool useDrawing = stateSet == LayerVector::StateSet::Drawing;
    const LayerVector& children = useDrawing ? mDrawingChildren : mCurrentChildren;
    const State& state = useDrawing ? mDrawingState : mCurrentState;
    LayerVector traverse(stateSet);
    for (const wp<Layer>& weakRelative : state.zOrderRelatives) {
        sp<Layer> strongRelative = weakRelative.promote();
        if (std::binary_search(layersInTree.begin(), layersInTree.end(), strongRelative.get())) {
            traverse.add(strongRelative);
        }
    }
    for (const sp<Layer>& child : children) {
        const State& childState = useDrawing ? child->mDrawingState : child->mCurrentState;
        if (std::binary_search(layersInTree.begin(), layersInTree.end(),
                               childState.zOrderRelativeOf.promote().get())) {
            continue;
        }
        traverse.add(child);
    }
    return traverse;
}
void Layer::traverseChildrenInZOrderInner(const std::vector<Layer*>& layersInTree,
                                          LayerVector::StateSet stateSet,
                                          const LayerVector::Visitor& visitor) {
    const LayerVector list = makeChildrenTraversalList(stateSet, layersInTree);
    size_t i = 0;
    for (; i < list.size(); i++) {
        const auto& relative = list[i];
        if (relative->getZ(stateSet) >= 0) {
            break;
        }
        relative->traverseChildrenInZOrderInner(layersInTree, stateSet, visitor);
    }
    visitor(this);
    for (; i < list.size(); i++) {
        const auto& relative = list[i];
        relative->traverseChildrenInZOrderInner(layersInTree, stateSet, visitor);
    }
}
std::vector<Layer*> Layer::getLayersInTree(LayerVector::StateSet stateSet) {
    const bool useDrawing = stateSet == LayerVector::StateSet::Drawing;
    const LayerVector& children = useDrawing ? mDrawingChildren : mCurrentChildren;
    std::vector<Layer*> layersInTree = {this};
    for (size_t i = 0; i < children.size(); i++) {
        const auto& child = children[i];
        std::vector<Layer*> childLayers = child->getLayersInTree(stateSet);
        layersInTree.insert(layersInTree.end(), childLayers.cbegin(), childLayers.cend());
    }
    return layersInTree;
}
void Layer::traverseChildrenInZOrder(LayerVector::StateSet stateSet,
                                     const LayerVector::Visitor& visitor) {
    std::vector<Layer*> layersInTree = getLayersInTree(stateSet);
    std::sort(layersInTree.begin(), layersInTree.end());
    traverseChildrenInZOrderInner(layersInTree, stateSet, visitor);
}
ui::Transform Layer::getTransform() const {
    return mEffectiveTransform;
}
half Layer::getAlpha() const {
    const auto& p = mDrawingParent.promote();
    half parentAlpha = (p != nullptr) ? p->getAlpha() : 1.0_hf;
    return parentAlpha * getDrawingState().color.a;
}
ui::Transform::RotationFlags Layer::getFixedTransformHint() const {
    ui::Transform::RotationFlags fixedTransformHint = mCurrentState.fixedTransformHint;
    if (fixedTransformHint != ui::Transform::ROT_INVALID) {
        return fixedTransformHint;
    }
    const auto& p = mCurrentParent.promote();
    if (!p) return fixedTransformHint;
    return p->getFixedTransformHint();
}
half4 Layer::getColor() const {
    const half4 color(getDrawingState().color);
    return half4(color.r, color.g, color.b, getAlpha());
}
int32_t Layer::getBackgroundBlurRadius() const {
    return getDrawingState().backgroundBlurRadius;
}
const std::vector<BlurRegion>& Layer::getBlurRegions() const {
    return getDrawingState().blurRegions;
}
Layer::RoundedCornerState Layer::getRoundedCornerState() const {
    const auto& p = mDrawingParent.promote();
    if (p != nullptr) {
        RoundedCornerState parentState = p->getRoundedCornerState();
        if (parentState.radius > 0) {
            ui::Transform t = getActiveTransform(getDrawingState());
            t = t.inverse();
            parentState.cropRect = t.transform(parentState.cropRect);
            auto scaleX = sqrtf(t[0][0] * t[0][0] + t[0][1] * t[0][1]);
            auto scaleY = sqrtf(t[1][0] * t[1][0] + t[1][1] * t[1][1]);
            parentState.radius *= (scaleX + scaleY) / 2.0f;
            return parentState;
        }
    }
    const float radius = getDrawingState().cornerRadius;
    return radius > 0 && getCrop(getDrawingState()).isValid()
            ? RoundedCornerState(getCrop(getDrawingState()).toFloatRect(), radius)
            : RoundedCornerState();
}
renderengine::ShadowSettings Layer::getShadowSettings(const Rect& layerStackRect) const {
    renderengine::ShadowSettings state = mFlinger->mDrawingState.globalShadowSettings;
    state.lightPos.x = (layerStackRect.width() / 2.f) - mScreenBounds.left;
    state.lightPos.y -= mScreenBounds.top;
    state.length = mEffectiveShadowRadius;
    return state;
}
void Layer::commitChildList() {
    for (size_t i = 0; i < mCurrentChildren.size(); i++) {
        const auto& child = mCurrentChildren[i];
        child->commitChildList();
    }
    mDrawingChildren = mCurrentChildren;
    mDrawingParent = mCurrentParent;
}
static wp<Layer> extractLayerFromBinder(const wp<IBinder>& weakBinderHandle) {
    if (weakBinderHandle == nullptr) {
        return nullptr;
    }
    sp<IBinder> binderHandle = weakBinderHandle.promote();
    if (binderHandle == nullptr) {
        return nullptr;
    }
    sp<Layer::Handle> handle = static_cast<Layer::Handle*>(binderHandle.get());
    if (handle == nullptr) {
        return nullptr;
    }
    return handle->owner;
}
void Layer::setInputInfo(const InputWindowInfo& info) {
    mCurrentState.inputInfo = info;
    mCurrentState.touchableRegionCrop = extractLayerFromBinder(info.touchableRegionCropHandle);
    mCurrentState.modified = true;
    mCurrentState.inputInfoChanged = true;
    setTransactionFlags(eTransactionNeeded);
}
LayerProto* Layer::writeToProto(LayersProto& layersProto, uint32_t traceFlags,
                                const DisplayDevice* display) {
    LayerProto* layerProto = layersProto.add_layers();
    writeToProtoDrawingState(layerProto, traceFlags, display);
    writeToProtoCommonState(layerProto, LayerVector::StateSet::Drawing, traceFlags);
    if (traceFlags & SurfaceTracing::TRACE_COMPOSITION) {
        if (display) {
            const Hwc2::IComposerClient::Composition compositionType = getCompositionType(*display);
            layerProto->set_hwc_composition_type(static_cast<HwcCompositionType>(compositionType));
        }
    }
    for (const sp<Layer>& layer : mDrawingChildren) {
        layer->writeToProto(layersProto, traceFlags, display);
    }
    return layerProto;
}
void Layer::writeToProtoDrawingState(LayerProto* layerInfo, uint32_t traceFlags,
                                     const DisplayDevice* display) {
    const ui::Transform transform = getTransform();
    if (traceFlags & SurfaceTracing::TRACE_CRITICAL) {
        for (const auto& pendingState : mPendingStatesSnapshot) {
            auto barrierLayer = pendingState.barrierLayer_legacy.promote();
            if (barrierLayer != nullptr) {
                BarrierLayerProto* barrierLayerProto = layerInfo->add_barrier_layer();
                barrierLayerProto->set_id(barrierLayer->sequence);
                barrierLayerProto->set_frame_number(pendingState.barrierFrameNumber);
            }
        }
        auto buffer = getBuffer();
        if (buffer != nullptr) {
            LayerProtoHelper::writeToProto(buffer,
                                           [&]() { return layerInfo->mutable_active_buffer(); });
            LayerProtoHelper::writeToProto(ui::Transform(getBufferTransform()),
                                           layerInfo->mutable_buffer_transform());
        }
        layerInfo->set_invalidate(contentDirty);
        layerInfo->set_is_protected(isProtected());
        layerInfo->set_dataspace(dataspaceDetails(static_cast<android_dataspace>(getDataSpace())));
        layerInfo->set_queued_frames(getQueuedFrameCount());
        layerInfo->set_refresh_pending(isBufferLatched());
        layerInfo->set_curr_frame(mCurrentFrameNumber);
        layerInfo->set_effective_scaling_mode(getEffectiveScalingMode());
        layerInfo->set_corner_radius(getRoundedCornerState().radius);
        layerInfo->set_background_blur_radius(getBackgroundBlurRadius());
        LayerProtoHelper::writeToProto(transform, layerInfo->mutable_transform());
        LayerProtoHelper::writePositionToProto(transform.tx(), transform.ty(),
                                               [&]() { return layerInfo->mutable_position(); });
        LayerProtoHelper::writeToProto(mBounds, [&]() { return layerInfo->mutable_bounds(); });
        if (traceFlags & SurfaceTracing::TRACE_COMPOSITION) {
            LayerProtoHelper::writeToProto(getVisibleRegion(display),
                                           [&]() { return layerInfo->mutable_visible_region(); });
        }
        LayerProtoHelper::writeToProto(surfaceDamageRegion,
                                       [&]() { return layerInfo->mutable_damage_region(); });
        if (hasColorTransform()) {
            LayerProtoHelper::writeToProto(getColorTransform(),
                                           layerInfo->mutable_color_transform());
        }
    }
    LayerProtoHelper::writeToProto(mSourceBounds,
                                   [&]() { return layerInfo->mutable_source_bounds(); });
    LayerProtoHelper::writeToProto(mScreenBounds,
                                   [&]() { return layerInfo->mutable_screen_bounds(); });
    LayerProtoHelper::writeToProto(getRoundedCornerState().cropRect,
                                   [&]() { return layerInfo->mutable_corner_radius_crop(); });
    layerInfo->set_shadow_radius(mEffectiveShadowRadius);
}
void Layer::writeToProtoCommonState(LayerProto* layerInfo, LayerVector::StateSet stateSet,
                                    uint32_t traceFlags) {
    const bool useDrawing = stateSet == LayerVector::StateSet::Drawing;
    const LayerVector& children = useDrawing ? mDrawingChildren : mCurrentChildren;
    const State& state = useDrawing ? mDrawingState : mCurrentState;
    ui::Transform requestedTransform = state.active_legacy.transform;
    if (traceFlags & SurfaceTracing::TRACE_CRITICAL) {
        layerInfo->set_id(sequence);
        layerInfo->set_name(getName().c_str());
        layerInfo->set_type(getType());
        for (const auto& child : children) {
            layerInfo->add_children(child->sequence);
        }
        for (const wp<Layer>& weakRelative : state.zOrderRelatives) {
            sp<Layer> strongRelative = weakRelative.promote();
            if (strongRelative != nullptr) {
                layerInfo->add_relatives(strongRelative->sequence);
            }
        }
        LayerProtoHelper::writeToProto(state.activeTransparentRegion_legacy,
                                       [&]() { return layerInfo->mutable_transparent_region(); });
        layerInfo->set_layer_stack(getLayerStack());
        layerInfo->set_z(state.z);
        LayerProtoHelper::writePositionToProto(requestedTransform.tx(), requestedTransform.ty(),
                                               [&]() {
                                                   return layerInfo->mutable_requested_position();
                                               });
        LayerProtoHelper::writeSizeToProto(state.active_legacy.w, state.active_legacy.h,
                                           [&]() { return layerInfo->mutable_size(); });
        LayerProtoHelper::writeToProto(state.crop_legacy,
                                       [&]() { return layerInfo->mutable_crop(); });
        layerInfo->set_is_opaque(isOpaque(state));
        layerInfo->set_pixel_format(decodePixelFormat(getPixelFormat()));
        LayerProtoHelper::writeToProto(getColor(), [&]() { return layerInfo->mutable_color(); });
        LayerProtoHelper::writeToProto(state.color,
                                       [&]() { return layerInfo->mutable_requested_color(); });
        layerInfo->set_flags(state.flags);
        LayerProtoHelper::writeToProto(requestedTransform,
                                       layerInfo->mutable_requested_transform());
        auto parent = useDrawing ? mDrawingParent.promote() : mCurrentParent.promote();
        if (parent != nullptr) {
            layerInfo->set_parent(parent->sequence);
        } else {
            layerInfo->set_parent(-1);
        }
        auto zOrderRelativeOf = state.zOrderRelativeOf.promote();
        if (zOrderRelativeOf != nullptr) {
            layerInfo->set_z_order_relative_of(zOrderRelativeOf->sequence);
        } else {
            layerInfo->set_z_order_relative_of(-1);
        }
        layerInfo->set_is_relative_of(state.isRelativeOf);
        layerInfo->set_owner_uid(mOwnerUid);
    }
    if (traceFlags & SurfaceTracing::TRACE_INPUT) {
        InputWindowInfo info;
        if (useDrawing) {
            info = fillInputInfo();
        } else {
            info = state.inputInfo;
        }
        LayerProtoHelper::writeToProto(info, state.touchableRegionCrop,
                                       [&]() { return layerInfo->mutable_input_window_info(); });
    }
    if (traceFlags & SurfaceTracing::TRACE_EXTRA) {
        auto protoMap = layerInfo->mutable_metadata();
        for (const auto& entry : state.metadata.mMap) {
            (*protoMap)[entry.first] = std::string(entry.second.cbegin(), entry.second.cend());
        }
    }
}
bool Layer::isRemovedFromCurrentState() const {
    return mRemovedFromCurrentState;
}
void Layer::fillInputFrameInfo(InputWindowInfo& info) {
    Rect layerBounds = info.portalToDisplayId == ADISPLAY_ID_NONE
            ? getBufferSize(getDrawingState())
            : info.touchableRegion.getBounds();
    if (!layerBounds.isValid()) {
        layerBounds = getCroppedBufferSize(getDrawingState());
    }
    if (!layerBounds.isValid()) {
        info.frameLeft = 0;
        info.frameTop = 0;
        info.frameRight = 0;
        info.frameBottom = 0;
        info.transform.reset();
        return;
    }
    ui::Transform t = getTransform();
    int32_t xSurfaceInset = info.surfaceInset;
    int32_t ySurfaceInset = info.surfaceInset;
    const float xScale = t.getScaleX();
    const float yScale = t.getScaleY();
    if (xScale != 1.0f || yScale != 1.0f) {
        xSurfaceInset = std::round(xSurfaceInset * xScale);
        ySurfaceInset = std::round(ySurfaceInset * yScale);
    }
    Rect transformedLayerBounds = t.transform(layerBounds);
    xSurfaceInset = (xSurfaceInset >= 0)
            ? std::min(xSurfaceInset, transformedLayerBounds.getWidth() / 2)
            : 0;
    ySurfaceInset = (ySurfaceInset >= 0)
            ? std::min(ySurfaceInset, transformedLayerBounds.getHeight() / 2)
            : 0;
    {
    int32_t tmp;
    if (!__builtin_add_overflow(transformedLayerBounds.left, xSurfaceInset, &tmp))
        transformedLayerBounds.left = tmp;
    if (!__builtin_sub_overflow(transformedLayerBounds.right, xSurfaceInset, &tmp))
        transformedLayerBounds.right = tmp;
    if (!__builtin_add_overflow(transformedLayerBounds.top, ySurfaceInset, &tmp))
        transformedLayerBounds.top = tmp;
    if (!__builtin_sub_overflow(transformedLayerBounds.bottom, ySurfaceInset, &tmp))
        transformedLayerBounds.bottom = tmp;
    }
    ui::Transform inverseTransform = t.inverse();
    Rect nonTransformedBounds = inverseTransform.transform(transformedLayerBounds);
    vec2 translation = t.transform(nonTransformedBounds.left, nonTransformedBounds.top);
    ui::Transform inputTransform(t);
    inputTransform.set(translation.x, translation.y);
    info.transform = inputTransform.inverse();
    Rect screenBounds = Rect{mScreenBounds};
    transformedLayerBounds.intersect(screenBounds, &transformedLayerBounds);
    info.frameLeft = transformedLayerBounds.left;
    info.frameTop = transformedLayerBounds.top;
    info.frameRight = transformedLayerBounds.right;
    info.frameBottom = transformedLayerBounds.bottom;
    info.touchableRegion = inputTransform.transform(info.touchableRegion);
}
InputWindowInfo Layer::fillInputInfo() {
    if (!hasInputInfo()) {
        mDrawingState.inputInfo.name = getName();
        mDrawingState.inputInfo.ownerUid = mOwnerUid;
        mDrawingState.inputInfo.ownerPid = mOwnerPid;
        mDrawingState.inputInfo.inputFeatures = InputWindowInfo::Feature::NO_INPUT_CHANNEL;
        mDrawingState.inputInfo.flags = InputWindowInfo::Flag::NOT_TOUCH_MODAL;
        mDrawingState.inputInfo.displayId = getLayerStack();
    }
    InputWindowInfo info = mDrawingState.inputInfo;
    info.id = sequence;
    if (info.displayId == ADISPLAY_ID_NONE) {
        info.displayId = getLayerStack();
    }
    fillInputFrameInfo(info);
    info.visible = hasInputInfo() ? canReceiveInput() : isVisible();
    info.alpha = getAlpha();
    auto cropLayer = mDrawingState.touchableRegionCrop.promote();
    if (info.replaceTouchableRegionWithCrop) {
        if (cropLayer == nullptr) {
            info.touchableRegion = Region(Rect{mScreenBounds});
        } else {
            info.touchableRegion = Region(Rect{cropLayer->mScreenBounds});
        }
    } else if (cropLayer != nullptr) {
        info.touchableRegion = info.touchableRegion.intersect(Rect{cropLayer->mScreenBounds});
    }
    if (isClone()) {
        sp<Layer> clonedRoot = getClonedRoot();
        if (clonedRoot != nullptr) {
            Rect rect(clonedRoot->mScreenBounds);
            info.touchableRegion = info.touchableRegion.intersect(rect);
        }
    }
    return info;
}
sp<Layer> Layer::getClonedRoot() {
    if (mClonedChild != nullptr) {
        return this;
    }
    if (mDrawingParent == nullptr || mDrawingParent.promote() == nullptr) {
        return nullptr;
    }
    return mDrawingParent.promote()->getClonedRoot();
}
bool Layer::hasInputInfo() const {
    return mDrawingState.inputInfo.token != nullptr;
}
bool Layer::canReceiveInput() const {
    return !isHiddenByPolicy();
}
compositionengine::OutputLayer* Layer::findOutputLayerForDisplay(
        const DisplayDevice* display) const {
    if (!display) return nullptr;
    return display->getCompositionDisplay()->getOutputLayerForLayer(getCompositionEngineLayerFE());
}
Region Layer::getVisibleRegion(const DisplayDevice* display) const {
    const auto outputLayer = findOutputLayerForDisplay(display);
    return outputLayer ? outputLayer->getState().visibleRegion : Region();
}
void Layer::setInitialValuesForClone(const sp<Layer>& clonedFrom) {
    mDrawingState = clonedFrom->mDrawingState;
    mClonedFrom = clonedFrom;
}
void Layer::updateMirrorInfo() {
    if (mClonedChild == nullptr || !mClonedChild->isClonedFromAlive()) {
        mClonedChild = nullptr;
        return;
    }
    std::map<sp<Layer>, sp<Layer>> clonedLayersMap;
    if (!mClonedChild->getClonedFrom()->isRemovedFromCurrentState()) {
        addChildToDrawing(mClonedChild);
    }
    mClonedChild->updateClonedDrawingState(clonedLayersMap);
    mClonedChild->updateClonedChildren(this, clonedLayersMap);
    mClonedChild->updateClonedRelatives(clonedLayersMap);
}
void Layer::updateClonedDrawingState(std::map<sp<Layer>, sp<Layer>>& clonedLayersMap) {
    if (isClonedFromAlive()) {
        sp<Layer> clonedFrom = getClonedFrom();
        mDrawingState = clonedFrom->mDrawingState;
        clonedLayersMap.emplace(clonedFrom, this);
    }
    for (sp<Layer>& child : mDrawingChildren) {
        child->updateClonedDrawingState(clonedLayersMap);
    }
}
void Layer::updateClonedChildren(const sp<Layer>& mirrorRoot,
                                 std::map<sp<Layer>, sp<Layer>>& clonedLayersMap) {
    mDrawingChildren.clear();
    if (!isClonedFromAlive()) {
        return;
    }
    sp<Layer> clonedFrom = getClonedFrom();
    for (sp<Layer>& child : clonedFrom->mDrawingChildren) {
        if (child == mirrorRoot) {
            continue;
        }
        sp<Layer> clonedChild = clonedLayersMap[child];
        if (clonedChild == nullptr) {
            clonedChild = child->createClone();
            clonedLayersMap[child] = clonedChild;
        }
        addChildToDrawing(clonedChild);
        clonedChild->updateClonedChildren(mirrorRoot, clonedLayersMap);
    }
}
void Layer::updateClonedInputInfo(const std::map<sp<Layer>, sp<Layer>>& clonedLayersMap) {
    auto cropLayer = mDrawingState.touchableRegionCrop.promote();
    if (cropLayer != nullptr) {
        if (clonedLayersMap.count(cropLayer) == 0) {
            mDrawingState.touchableRegionCrop = this;
        } else {
            const sp<Layer>& clonedCropLayer = clonedLayersMap.at(cropLayer);
            mDrawingState.touchableRegionCrop = clonedCropLayer;
        }
    }
    mDrawingState.inputInfo.flags &= ~InputWindowInfo::Flag::WATCH_OUTSIDE_TOUCH;
}
void Layer::updateClonedRelatives(const std::map<sp<Layer>, sp<Layer>>& clonedLayersMap) {
    mDrawingState.zOrderRelativeOf = nullptr;
    mDrawingState.zOrderRelatives.clear();
    if (!isClonedFromAlive()) {
        return;
    }
    const sp<Layer>& clonedFrom = getClonedFrom();
    for (wp<Layer>& relativeWeak : clonedFrom->mDrawingState.zOrderRelatives) {
        const sp<Layer>& relative = relativeWeak.promote();
        if (clonedLayersMap.count(relative) > 0) {
            auto& clonedRelative = clonedLayersMap.at(relative);
            mDrawingState.zOrderRelatives.add(clonedRelative);
        }
    }
    const sp<Layer>& relativeOf = clonedFrom->mDrawingState.zOrderRelativeOf.promote();
    if (clonedLayersMap.count(relativeOf) > 0) {
        const sp<Layer>& clonedRelativeOf = clonedLayersMap.at(relativeOf);
        mDrawingState.zOrderRelativeOf = clonedRelativeOf;
    }
    updateClonedInputInfo(clonedLayersMap);
    for (sp<Layer>& child : mDrawingChildren) {
        child->updateClonedRelatives(clonedLayersMap);
    }
}
void Layer::addChildToDrawing(const sp<Layer>& layer) {
    mDrawingChildren.add(layer);
    layer->mDrawingParent = this;
}
Layer::FrameRateCompatibility Layer::FrameRate::convertCompatibility(int8_t compatibility) {
    switch (compatibility) {
        case ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT:
            return FrameRateCompatibility::Default;
        case ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE:
            return FrameRateCompatibility::ExactOrMultiple;
        default:
            LOG_ALWAYS_FATAL("Invalid frame rate compatibility value %d", compatibility);
            return FrameRateCompatibility::Default;
    }
}
bool Layer::getPrimaryDisplayOnly() const {
    const State& s(mDrawingState);
    if (s.flags & layer_state_t::eLayerSkipScreenshot) {
        return true;
    }
    sp<Layer> parent = mDrawingParent.promote();
    return parent == nullptr ? false : parent->getPrimaryDisplayOnly();
}
std::ostream& operator<<(std::ostream& stream, const Layer::FrameRate& rate) {
    return stream << "{rate=" << rate.rate
                  << " type=" << Layer::frameRateCompatibilityString(rate.type)
                  << " seamlessness=" << toString(rate.seamlessness) << "}";
}
};
#if defined(__gl_h_)
#error "don't include gl/gl.h in this file"
#endif
#if defined(__gl2_h_)
#error "don't include gl2/gl2.h in this file"
#endif
#pragma clang diagnostic pop
