#undef LOG_TAG
#define LOG_TAG "BufferLayer"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include "BufferLayer.h"
#include "Colorizer.h"
#include "DisplayDevice.h"
#include "LayerRejecter.h"
#include "clz.h"
#include "RenderEngine/RenderEngine.h"
#include <renderengine/RenderEngine.h>
#include <gui/BufferItem.h>
#include <gui/BufferQueue.h>
#include <gui/LayerDebugInfo.h>
#include <gui/Surface.h>
#include <ui/DebugUtils.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/NativeHandle.h>
#include <utils/StopWatch.h>
#include <utils/Trace.h>
#include <cutils/compiler.h>
#include <cutils/native_handle.h>
#include <cutils/properties.h>
#include <math.h>
#include <stdlib.h>
#include <mutex>
namespace android {
BufferLayer::BufferLayer(SurfaceFlinger* flinger, const sp<Client>& client, const String8& name,
                         uint32_t w, uint32_t h, uint32_t flags)
      : Layer(flinger, client, name, w, h, flags),
        mTextureName(mFlinger->getNewTexture()),
        mCurrentScalingMode(NATIVE_WINDOW_SCALING_MODE_FREEZE),
        mBufferLatched(false),
        mRefreshPending(false) {
    ALOGV("Creating Layer %s", name.string());
    mTexture.init(renderengine::Texture::TEXTURE_EXTERNAL, mTextureName);
    mPremultipliedAlpha = !(flags & ISurfaceComposerClient::eNonPremultiplied);
    mPotentialCursor = flags & ISurfaceComposerClient::eCursorWindow;
    mProtectedByApp = flags & ISurfaceComposerClient::eProtectedByApp;
    mDrawingState = mCurrentState;
}
BufferLayer::~BufferLayer() {
    mFlinger->deleteTextureAsync(mTextureName);
    if (!getBE().mHwcLayers.empty()) {
        ALOGE("Found stale hardware composer layers when destroying "
              "surface flinger layer %s",
              mName.string());
        destroyAllHwcLayers();
    }
}{
    mFlinger->deleteTextureAsync(mTextureName);
    if (!getBE().mHwcLayers.empty()) {
        ALOGE("Found stale hardware composer layers when destroying "
              "surface flinger layer %s",
              mName.string());
        destroyAllHwcLayers();
    }
}
void BufferLayer::useSurfaceDamage() {
    if (mFlinger->mForceFullDamage) {
        surfaceDamageRegion = Region::INVALID_REGION;
    } else {
        surfaceDamageRegion = getDrawingSurfaceDamage();
    }
}
void BufferLayer::useEmptyDamage() {
    surfaceDamageRegion.clear();
}
bool BufferLayer::isProtected() const {
    const sp<GraphicBuffer>& buffer(mActiveBuffer);
    return (buffer != 0) && (buffer->getUsage() & GRALLOC_USAGE_PROTECTED);
}
bool BufferLayer::isVisible() const {
    return !(isHiddenByPolicy()) && getAlpha() > 0.0f &&
            (mActiveBuffer != nullptr || getBE().compositionInfo.hwc.sidebandStream != nullptr);
}
bool BufferLayer::isFixedSize() const {
    return getEffectiveScalingMode() != NATIVE_WINDOW_SCALING_MODE_FREEZE;
}
static constexpr mat4 inverseOrientation(uint32_t transform) {
    const mat4 flipH(-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1);
    const mat4 flipV(1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1);
    const mat4 rot90(0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1);
    mat4 tr;
    if (transform & NATIVE_WINDOW_TRANSFORM_ROT_90) {
        tr = tr * rot90;
    }
    if (transform & NATIVE_WINDOW_TRANSFORM_FLIP_H) {
        tr = tr * flipH;
    }
    if (transform & NATIVE_WINDOW_TRANSFORM_FLIP_V) {
        tr = tr * flipV;
    }
    return inverse(tr);
}
void BufferLayer::onDraw(const RenderArea& renderArea, const Region& clip,
                         bool useIdentityTransform) {
    ATRACE_CALL();
    if (CC_UNLIKELY(mActiveBuffer == 0)) {
        Region under;
        bool finished = false;
        mFlinger->mDrawingState.traverseInZOrder([&](Layer* layer) {
            if (finished || layer == static_cast<BufferLayer const*>(this)) {
                finished = true;
                return;
            }
            under.orSelf(renderArea.getTransform().transform(layer->visibleRegion));
        });
        Region holes(clip.subtract(under));
        if (!holes.isEmpty()) {
            clearWithOpenGL(renderArea, 0, 0, 0, 1);
        }
        return;
    }
    status_t err = bindTextureImage();
    if (err != NO_ERROR) {
        ALOGW("onDraw: bindTextureImage failed (err=%d)", err);
    }
    bool blackOutLayer = isProtected() || (isSecure() && !renderArea.isSecure());
    auto& engine(mFlinger->getRenderEngine());
    if (!blackOutLayer) {
        const bool useFiltering = needsFiltering(renderArea) || isFixedSize();
        float textureMatrix[16];
        setFilteringEnabled(useFiltering);
        getDrawingTransformMatrix(textureMatrix);
        if (getTransformToDisplayInverse()) {
            uint32_t transform = DisplayDevice::getPrimaryDisplayOrientationTransform();
            mat4 tr = inverseOrientation(transform);
            sp<Layer> p = mDrawingParent.promote();
            if (p != nullptr) {
                const auto parentTransform = p->getTransform();
                tr = tr * inverseOrientation(parentTransform.getOrientation());
            }
            const mat4 texTransform(mat4(static_cast<const float*>(textureMatrix)) * tr);
            memcpy(textureMatrix, texTransform.asArray(), sizeof(textureMatrix));
        }
        mTexture.setDimensions(mActiveBuffer->getWidth(), mActiveBuffer->getHeight());
        mTexture.setFiltering(useFiltering);
        mTexture.setMatrix(textureMatrix);
        engine.setupLayerTexturing(mTexture);
    } else {
        engine.setupLayerBlackedOut();
    }
    drawWithOpenGL(renderArea, useIdentityTransform);
    engine.disableTexturing();
}
bool BufferLayer::onPreComposition(nsecs_t refreshStartTime) {
    if (mBufferLatched) {
        Mutex::Autolock lock(mFrameEventHistoryMutex);
        mFrameEventHistory.addPreComposition(mCurrentFrameNumber, refreshStartTime);
    }
    mRefreshPending = false;
    return hasReadyFrame();
}
bool BufferLayer::onPostComposition(const std::shared_ptr<FenceTime>& glDoneFence,
                                    const std::shared_ptr<FenceTime>& presentFence,
                                    const CompositorTiming& compositorTiming) {
    if (!mFrameLatencyNeeded) return false;
    {
        Mutex::Autolock lock(mFrameEventHistoryMutex);
        mFrameEventHistory.addPostComposition(mCurrentFrameNumber, glDoneFence, presentFence,
                                              compositorTiming);
    }
    nsecs_t desiredPresentTime = getDesiredPresentTime();
    mFrameTracker.setDesiredPresentTime(desiredPresentTime);
    const std::string layerName(getName().c_str());
    mTimeStats.setDesiredTime(layerName, mCurrentFrameNumber, desiredPresentTime);
    std::shared_ptr<FenceTime> frameReadyFence = getCurrentFenceTime();
    if (frameReadyFence->isValid()) {
        mFrameTracker.setFrameReadyFence(std::move(frameReadyFence));
    } else {
        mFrameTracker.setFrameReadyTime(desiredPresentTime);
    }
    if (presentFence->isValid()) {
        mTimeStats.setPresentFence(layerName, mCurrentFrameNumber, presentFence);
        mFrameTracker.setActualPresentFence(std::shared_ptr<FenceTime>(presentFence));
    } else if (mFlinger->getHwComposer().isConnected(HWC_DISPLAY_PRIMARY)) {
        const nsecs_t actualPresentTime =
                mFlinger->getHwComposer().getRefreshTimestamp(HWC_DISPLAY_PRIMARY);
        mTimeStats.setPresentTime(layerName, mCurrentFrameNumber, actualPresentTime);
        mFrameTracker.setActualPresentTime(actualPresentTime);
    }
    mFrameTracker.advanceFrame();
    mFrameLatencyNeeded = false;
    return true;
}
Region BufferLayer::latchBuffer(bool& recomputeVisibleRegions, nsecs_t latchTime) {
    ATRACE_CALL();
    std::optional<Region> sidebandStreamDirtyRegion = latchSidebandStream(recomputeVisibleRegions);
    if (sidebandStreamDirtyRegion) {
        return *sidebandStreamDirtyRegion;
    }
    Region dirtyRegion;
    if (!hasReadyFrame()) {
        return dirtyRegion;
    }
    if (mRefreshPending) {
        return dirtyRegion;
    }
    if (!fenceHasSignaled()) {
        mFlinger->signalLayerUpdate();
        return dirtyRegion;
    }
    const State& s(getDrawingState());
    const bool oldOpacity = isOpaque(s);
    sp<GraphicBuffer> oldBuffer = mActiveBuffer;
    if (!allTransactionsSignaled()) {
        mFlinger->signalLayerUpdate();
        return dirtyRegion;
    }
    status_t err = updateTexImage(recomputeVisibleRegions, latchTime);
    if (err != NO_ERROR) {
        return dirtyRegion;
    }
    err = updateActiveBuffer();
    if (err != NO_ERROR) {
        return dirtyRegion;
    }
    mBufferLatched = true;
    err = updateFrameNumber(latchTime);
    if (err != NO_ERROR) {
        return dirtyRegion;
    }
    mRefreshPending = true;
    mFrameLatencyNeeded = true;
    if (oldBuffer == nullptr) {
        recomputeVisibleRegions = true;
    }
    ui::Dataspace dataSpace = getDrawingDataSpace();
    switch (dataSpace) {
        case ui::Dataspace::V0_SRGB:
            dataSpace = ui::Dataspace::SRGB;
            break;
        case ui::Dataspace::V0_SRGB_LINEAR:
            dataSpace = ui::Dataspace::SRGB_LINEAR;
            break;
        case ui::Dataspace::V0_JFIF:
            dataSpace = ui::Dataspace::JFIF;
            break;
        case ui::Dataspace::V0_BT601_625:
            dataSpace = ui::Dataspace::BT601_625;
            break;
        case ui::Dataspace::V0_BT601_525:
            dataSpace = ui::Dataspace::BT601_525;
            break;
        case ui::Dataspace::V0_BT709:
            dataSpace = ui::Dataspace::BT709;
            break;
        default:
            break;
    }
    mCurrentDataSpace = dataSpace;
    Rect crop(getDrawingCrop());
    const uint32_t transform(getDrawingTransform());
    const uint32_t scalingMode(getDrawingScalingMode());
    if ((crop != mCurrentCrop) || (transform != mCurrentTransform) ||
        (scalingMode != mCurrentScalingMode)) {
        mCurrentCrop = crop;
        mCurrentTransform = transform;
        mCurrentScalingMode = scalingMode;
        recomputeVisibleRegions = true;
    }
    if (oldBuffer != nullptr) {
        uint32_t bufWidth = mActiveBuffer->getWidth();
        uint32_t bufHeight = mActiveBuffer->getHeight();
        if (bufWidth != uint32_t(oldBuffer->width) || bufHeight != uint32_t(oldBuffer->height)) {
            recomputeVisibleRegions = true;
        }
    }
    if (oldOpacity != isOpaque(s)) {
        recomputeVisibleRegions = true;
    }
    {
        Mutex::Autolock lock(mLocalSyncPointMutex);
        auto point = mLocalSyncPoints.begin();
        while (point != mLocalSyncPoints.end()) {
            if (!(*point)->frameIsAvailable() || !(*point)->transactionIsApplied()) {
                ++point;
                continue;
            }
            if ((*point)->getFrameNumber() <= mCurrentFrameNumber) {
                point = mLocalSyncPoints.erase(point);
            } else {
                ++point;
            }
        }
    }
    return getTransform().transform(Region(Rect(getActiveWidth(s), getActiveHeight(s))));
}
void BufferLayer::setPerFrameData(const sp<const DisplayDevice>& display) {
    const ui::Transform& tr = display->getTransform();
    const auto& viewport = display->getViewport();
    Region visible = tr.transform(visibleRegion.intersect(viewport));
<<<<<<< HEAD
    const auto displayId = display->getId();
    getBE().compositionInfo.hwc.visibleRegion = visible;
    getBE().compositionInfo.hwc.surfaceDamage = surfaceDamageRegion;
||||||| d1030f6a7d
    auto hwcId = displayDevice->getHwcDisplayId();
    auto& hwcInfo = getBE().mHwcLayers[hwcId];
    auto& hwcLayer = hwcInfo.layer;
    auto error = hwcLayer->setVisibleRegion(visible);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set visible region: %s (%d)", mName.string(),
              to_string(error).c_str(), static_cast<int32_t>(error));
        visible.dump(LOG_TAG);
    }
    error = hwcLayer->setSurfaceDamage(surfaceDamageRegion);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set surface damage: %s (%d)", mName.string(),
              to_string(error).c_str(), static_cast<int32_t>(error));
        surfaceDamageRegion.dump(LOG_TAG);
    }
=======
    auto hwcId = displayDevice->getHwcDisplayId();
    if (!hasHwcLayer(hwcId)) {
        return;
    }
    auto& hwcInfo = getBE().mHwcLayers[hwcId];
    auto& hwcLayer = hwcInfo.layer;
    auto error = hwcLayer->setVisibleRegion(visible);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set visible region: %s (%d)", mName.string(),
              to_string(error).c_str(), static_cast<int32_t>(error));
        visible.dump(LOG_TAG);
    }
    error = hwcLayer->setSurfaceDamage(surfaceDamageRegion);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set surface damage: %s (%d)", mName.string(),
              to_string(error).c_str(), static_cast<int32_t>(error));
        surfaceDamageRegion.dump(LOG_TAG);
    }
>>>>>>> 8acaef00
    if (getBE().compositionInfo.hwc.sidebandStream.get()) {
        setCompositionType(displayId, HWC2::Composition::Sideband);
        getBE().compositionInfo.compositionType = HWC2::Composition::Sideband;
        return;
    }
    if (getBE().compositionInfo.hwc.skipGeometry) {
        if (mPotentialCursor) {
            ALOGV("[%s] Requesting Cursor composition", mName.string());
            setCompositionType(displayId, HWC2::Composition::Cursor);
        } else {
            ALOGV("[%s] Requesting Device composition", mName.string());
            setCompositionType(displayId, HWC2::Composition::Device);
        }
    }
    getBE().compositionInfo.hwc.dataspace = mCurrentDataSpace;
    getBE().compositionInfo.hwc.hdrMetadata = getDrawingHdrMetadata();
    getBE().compositionInfo.hwc.supportedPerFrameMetadata = display->getSupportedPerFrameMetadata();
    setHwcLayerBuffer(display);
}
bool BufferLayer::isOpaque(const Layer::State& s) const {
    if ((getBE().compositionInfo.hwc.sidebandStream == nullptr) && (mActiveBuffer == nullptr)) {
        return false;
    }
    return ((s.flags & layer_state_t::eLayerOpaque) != 0) || getOpacityForFormat(getPixelFormat());
}
bool BufferLayer::needsFiltering(const RenderArea& renderArea) const {
    return mNeedsFiltering || renderArea.needsFiltering();
}
bool BufferLayer::getOpacityForFormat(uint32_t format) {
    if (HARDWARE_IS_DEVICE_FORMAT(format)) {
        return true;
    }
    switch (format) {
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
        case HAL_PIXEL_FORMAT_RGBA_FP16:
        case HAL_PIXEL_FORMAT_RGBA_1010102:
            return false;
    }
    return true;
}
bool BufferLayer::isHdrY410() const {
    return (mCurrentDataSpace == ui::Dataspace::BT2020_ITU_PQ &&
            getDrawingApi() == NATIVE_WINDOW_API_MEDIA &&
            getBE().compositionInfo.mBuffer->getPixelFormat() == HAL_PIXEL_FORMAT_RGBA_1010102);
}
void BufferLayer::drawWithOpenGL(const RenderArea& renderArea, bool useIdentityTransform) const {
    ATRACE_CALL();
    const State& s(getDrawingState());
    computeGeometry(renderArea, getBE().mMesh, useIdentityTransform);
    const Rect bounds{computeBounds()};
    ui::Transform t = getTransform();
    Rect win = bounds;
    float left = float(win.left) / float(getActiveWidth(s));
    float top = float(win.top) / float(getActiveHeight(s));
    float right = float(win.right) / float(getActiveWidth(s));
    float bottom = float(win.bottom) / float(getActiveHeight(s));
    renderengine::Mesh::VertexArray<vec2> texCoords(getBE().mMesh.getTexCoordArray<vec2>());
    texCoords[0] = vec2(left, 1.0f - top);
    texCoords[1] = vec2(left, 1.0f - bottom);
    texCoords[2] = vec2(right, 1.0f - bottom);
    texCoords[3] = vec2(right, 1.0f - top);
    auto& engine(mFlinger->getRenderEngine());
    engine.setupLayerBlending(mPremultipliedAlpha, isOpaque(s), false ,
                              getColor());
    engine.setSourceDataSpace(mCurrentDataSpace);
    if (isHdrY410()) {
        engine.setSourceY410BT2020(true);
    }
    engine.drawMesh(getBE().mMesh);
    engine.disableBlending();
    engine.setSourceY410BT2020(false);
}
bool BufferLayer::latchUnsignaledBuffers() {
    static bool propertyLoaded = false;
    static bool latch = false;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    if (!propertyLoaded) {
        char value[PROPERTY_VALUE_MAX] = {};
        property_get("debug.sf.latch_unsignaled", value, "0");
        latch = atoi(value);
        propertyLoaded = true;
    }
    return latch;
}
uint64_t BufferLayer::getHeadFrameNumber() const {
    if (hasDrawingBuffer()) {
        return getFrameNumber();
    } else {
        return mCurrentFrameNumber;
    }
}
uint32_t BufferLayer::getEffectiveScalingMode() const {
    if (mOverrideScalingMode >= 0) {
        return mOverrideScalingMode;
    }
    return mCurrentScalingMode;
}
void BufferLayer::notifyAvailableFrames() {
    auto headFrameNumber = getHeadFrameNumber();
    bool headFenceSignaled = fenceHasSignaled();
    Mutex::Autolock lock(mLocalSyncPointMutex);
    for (auto& point : mLocalSyncPoints) {
        if (headFrameNumber >= point->getFrameNumber() && headFenceSignaled) {
            point->setFrameAvailable();
        }
    }
}
bool BufferLayer::hasReadyFrame() const {
    return hasDrawingBuffer() || getSidebandStreamChanged() || getAutoRefresh();
}
bool BufferLayer::allTransactionsSignaled() {
    auto headFrameNumber = getHeadFrameNumber();
    bool matchingFramesFound = false;
    bool allTransactionsApplied = true;
    Mutex::Autolock lock(mLocalSyncPointMutex);
    for (auto& point : mLocalSyncPoints) {
        if (point->getFrameNumber() > headFrameNumber) {
            break;
        }
        matchingFramesFound = true;
        if (!point->frameIsAvailable()) {
            point->setFrameAvailable();
            allTransactionsApplied = false;
            break;
        }
        allTransactionsApplied = allTransactionsApplied && point->transactionIsApplied();
    }
    return !matchingFramesFound || allTransactionsApplied;
}
}
