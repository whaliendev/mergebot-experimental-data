#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <cutils/compiler.h>
#include <cutils/native_handle.h>
#include <cutils/properties.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/StopWatch.h>
#include <ui/GraphicBuffer.h>
#include <ui/PixelFormat.h>
#include <surfaceflinger/Surface.h>
#include "clz.h"
#include "DisplayHardware/DisplayHardware.h"
#include "DisplayHardware/HWComposer.h"
#include "GLExtensions.h"
#include "Layer.h"
#include "SurfaceFlinger.h"
#include "SurfaceTextureLayer.h"
#define DEBUG_RESIZE 0
namespace android {
template <typename T> inline T min(T a, T b) {
    return a<b ? a : b;
}
Layer::Layer(SurfaceFlinger* flinger,
        DisplayID display, const sp<Client>& client)
    : LayerBaseClient(flinger, display, client),
        mTextureName(-1U),
        mQueuedFrames(0),
        mCurrentTransform(0),
        mCurrentOpacity(true),
        mFormat(PIXEL_FORMAT_NONE),
        mGLExtensions(GLExtensions::getInstance()),
        mOpaqueLayer(true),
        mNeedsDithering(false),
        mSecure(false),
        mProtectedByApp(false),
        mFixedSize(false)
{
    mCurrentCrop.makeInvalid();
    glGenTextures(1, &mTextureName);
}
void Layer::destroy(RefBase const* base) {
    mFlinger->destroyLayer(static_cast<LayerBase const*>(base));
}
void Layer::onFirstRef()
{
    LayerBaseClient::onFirstRef();
    setDestroyer(this);
    struct FrameQueuedListener : public SurfaceTexture::FrameAvailableListener {
        FrameQueuedListener(Layer* layer) : mLayer(layer) { }
    private:
        wp<Layer> mLayer;
        virtual void onFrameAvailable() {
            sp<Layer> that(mLayer.promote());
            if (that != 0) {
                that->onFrameQueued();
            }
        }
    };
    mSurfaceTexture = new SurfaceTextureLayer(mTextureName, this);
    mSurfaceTexture->setFrameAvailableListener(new FrameQueuedListener(this));
    mSurfaceTexture->setSynchronousMode(true);
    mSurfaceTexture->setBufferCountServer(2);
}
Layer::~Layer()
{
    glDeleteTextures(1, &mTextureName);
}
void Layer::onFrameQueued() {
    if (android_atomic_or(1, &mQueuedFrames) == 0) {
        mFlinger->signalEvent();
    }
}
void Layer::onRemoved()
{
}
sp<ISurface> Layer::createSurface()
{
    class BSurface : public BnSurface, public LayerCleaner {
        wp<const Layer> mOwner;
        virtual sp<ISurfaceTexture> getSurfaceTexture() const {
            sp<ISurfaceTexture> res;
            sp<const Layer> that( mOwner.promote() );
            if (that != NULL) {
                res = that->mSurfaceTexture;
            }
            return res;
        }
    public:
        BSurface(const sp<SurfaceFlinger>& flinger,
                const sp<Layer>& layer)
            : LayerCleaner(flinger, layer), mOwner(layer) { }
    };
    sp<ISurface> sur(new BSurface(mFlinger, this));
    return sur;
}
status_t Layer::setBuffers( uint32_t w, uint32_t h,
                            PixelFormat format, uint32_t flags)
{
    PixelFormatInfo info;
    status_t err = getPixelFormatInfo(format, &info);
    if (err) return err;
    const DisplayHardware& hw(graphicPlane(0).displayHardware());
    uint32_t const maxSurfaceDims = min(
            hw.getMaxTextureSize(), hw.getMaxViewportDims());
    if ((uint32_t(w)>maxSurfaceDims) || (uint32_t(h)>maxSurfaceDims)) {
        return BAD_VALUE;
    }
    PixelFormatInfo displayInfo;
    getPixelFormatInfo(hw.getFormat(), &displayInfo);
    const uint32_t hwFlags = hw.getFlags();
    mFormat = format;
    mSecure = (flags & ISurfaceComposer::eSecure) ? true : false;
    mProtectedByApp = (flags & ISurfaceComposer::eProtectedByApp) ? true : false;
    mOpaqueLayer = (flags & ISurfaceComposer::eOpaque);
    mCurrentOpacity = getOpacityForFormat(format);
    mSurfaceTexture->setDefaultBufferSize(w, h);
    mSurfaceTexture->setDefaultBufferFormat(format);
    int displayRedSize = displayInfo.getSize(PixelFormatInfo::INDEX_RED);
    int layerRedsize = info.getSize(PixelFormatInfo::INDEX_RED);
    mNeedsDithering = layerRedsize > displayRedSize;
    return NO_ERROR;
}
void Layer::setGeometry(hwc_layer_t* hwcl)
{
    hwcl->compositionType = HWC_FRAMEBUFFER;
    hwcl->hints = 0;
    hwcl->flags = 0;
    hwcl->transform = 0;
    hwcl->blending = HWC_BLENDING_NONE;
    const State& s(drawingState());
    if (s.alpha < 0xFF) {
        hwcl->flags = HWC_SKIP_LAYER;
        return;
    }
    if (mOrientation & Transform::ROT_INVALID) {
        hwcl->flags = HWC_SKIP_LAYER;
        return;
    }
    Transform tr(Transform(mOrientation) * Transform(mCurrentTransform));
    hwcl->transform = tr.getOrientation();
    if (!isOpaque()) {
        hwcl->blending = mPremultipliedAlpha ?
                HWC_BLENDING_PREMULT : HWC_BLENDING_COVERAGE;
    }
    hwcl->displayFrame.left = mTransformedBounds.left;
    hwcl->displayFrame.top = mTransformedBounds.top;
    hwcl->displayFrame.right = mTransformedBounds.right;
    hwcl->displayFrame.bottom = mTransformedBounds.bottom;
    hwcl->visibleRegionScreen.rects =
            reinterpret_cast<hwc_rect_t const *>(
                    visibleRegionScreen.getArray(
                            &hwcl->visibleRegionScreen.numRects));
}
void Layer::setPerFrameData(hwc_layer_t* hwcl) {
    const sp<GraphicBuffer>& buffer(mActiveBuffer);
    if (buffer == NULL) {
        hwcl->flags |= HWC_SKIP_LAYER;
        hwcl->handle = NULL;
        return;
    }
    hwcl->handle = buffer->handle;
    if (isCropped()) {
        hwcl->sourceCrop.left = mCurrentCrop.left;
        hwcl->sourceCrop.top = mCurrentCrop.top;
        hwcl->sourceCrop.right = mCurrentCrop.right;
        hwcl->sourceCrop.bottom = mCurrentCrop.bottom;
    } else {
        hwcl->sourceCrop.left = 0;
        hwcl->sourceCrop.top = 0;
        hwcl->sourceCrop.right = buffer->width;
        hwcl->sourceCrop.bottom = buffer->height;
    }
}
static inline uint16_t pack565(int r, int g, int b) {
    return (r<<11)|(g<<5)|b;
}
void Layer::onDraw(const Region& clip) const
{
    if (CC_UNLIKELY(mActiveBuffer == 0)) {
        Region under;
        const SurfaceFlinger::LayerVector& drawingLayers(mFlinger->mDrawingState.layersSortedByZ);
        const size_t count = drawingLayers.size();
        for (size_t i=0 ; i<count ; ++i) {
            const sp<LayerBase>& layer(drawingLayers[i]);
            if (layer.get() == static_cast<LayerBase const*>(this))
                break;
            under.orSelf(layer->visibleRegionScreen);
        }
        Region holes(clip.subtract(under));
        if (!holes.isEmpty()) {
            clearWithOpenGL(holes, 0, 0, 0, 1);
        }
        return;
    }
    GLenum target = mSurfaceTexture->getCurrentTextureTarget();
    glBindTexture(target, mTextureName);
    if (getFiltering() || needsFiltering() || isFixedSize() || isCropped()) {
        glTexParameterx(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameterx(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    } else {
        glTexParameterx(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameterx(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    }
    glEnable(target);
    glMatrixMode(GL_TEXTURE);
    glLoadMatrixf(mTextureMatrix);
    glMatrixMode(GL_MODELVIEW);
    drawWithOpenGL(clip);
    glDisable(target);
}
#define HARDWARE_IS_DEVICE_FORMAT(f) ((f) >= 0x100 && (f) <= 0x1FF)
bool Layer::getOpacityForFormat(uint32_t format)
{
    if (HARDWARE_IS_DEVICE_FORMAT(format)) {
        return true;
    }
    PixelFormatInfo info;
    status_t err = getPixelFormatInfo(PixelFormat(format), &info);
    return (err || info.h_alpha <= info.l_alpha);
}
bool Layer::isOpaque() const
{
    if (mActiveBuffer == 0)
        return false;
    return mOpaqueLayer || mCurrentOpacity;
}
bool Layer::isProtected() const
{
    const sp<GraphicBuffer>& activeBuffer(mActiveBuffer);
    return (activeBuffer != 0) &&
            (activeBuffer->getUsage() & GRALLOC_USAGE_PROTECTED);
}
uint32_t Layer::doTransaction(uint32_t flags)
{
    const Layer::State& front(drawingState());
    const Layer::State& temp(currentState());
    const bool sizeChanged = (front.requested_w != temp.requested_w) ||
            (front.requested_h != temp.requested_h);
    if (sizeChanged) {
        LOGD_IF(DEBUG_RESIZE,
                "resize (layer=%p), requested (%dx%d), drawing (%d,%d), "
                "fixedSize=%d",
                this,
                int(temp.requested_w), int(temp.requested_h),
                int(front.requested_w), int(front.requested_h),
                isFixedSize());
        if (!isFixedSize()) {
            if (mFlinger->hasFreezeRequest()) {
                if (!(front.flags & ISurfaceComposer::eLayerHidden)) {
                    mFreezeLock = mFlinger->getFreezeLock();
                }
            }
            Layer::State& editDraw(mDrawingState);
            editDraw.requested_w = temp.requested_w;
            editDraw.requested_h = temp.requested_h;
            mSurfaceTexture->setDefaultBufferSize(temp.requested_w, temp.requested_h);
        }
    }
    if (temp.sequence != front.sequence) {
        if (temp.flags & ISurfaceComposer::eLayerHidden || temp.alpha == 0) {
            mFreezeLock.clear();
        }
    }
    return LayerBase::doTransaction(flags);
}
bool Layer::isFixedSize() const {
    Mutex::Autolock _l(mLock);
    return mFixedSize;
}
void Layer::setFixedSize(bool fixedSize)
{
    Mutex::Autolock _l(mLock);
    mFixedSize = fixedSize;
}
bool Layer::isCropped() const {
    return !mCurrentCrop.isEmpty();
}
void Layer::lockPageFlip(bool& recomputeVisibleRegions)
{
    if (android_atomic_and(0, &mQueuedFrames)) {
        if (mSurfaceTexture->updateTexImage() < NO_ERROR) {
            recomputeVisibleRegions = true;
            return;
        }
        if (mSurfaceTexture->getQueuedCount()) {
            if (android_atomic_or(1, &mQueuedFrames) == 0) {
                mFlinger->signalEvent();
            }
        }
        mActiveBuffer = mSurfaceTexture->getCurrentBuffer();
        mSurfaceTexture->getTransformMatrix(mTextureMatrix);
        const Rect crop(mSurfaceTexture->getCurrentCrop());
        const uint32_t transform(mSurfaceTexture->getCurrentTransform());
        if ((crop != mCurrentCrop) || (transform != mCurrentTransform)) {
            mCurrentCrop = crop;
            mCurrentTransform = transform;
            mFlinger->invalidateHwcGeometry();
        }
        const bool opacity(getOpacityForFormat(mActiveBuffer->format));
        if (opacity != mCurrentOpacity) {
            mCurrentOpacity = opacity;
            recomputeVisibleRegions = true;
        }
        const GLenum target(mSurfaceTexture->getCurrentTextureTarget());
        glTexParameterx(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameterx(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        const Layer::State& front(drawingState());
        mPostedDirtyRegion.set(front.w, front.h);
        sp<GraphicBuffer> newFrontBuffer(mActiveBuffer);
        if ((newFrontBuffer->getWidth() == front.requested_w &&
            newFrontBuffer->getHeight() == front.requested_h) ||
            isFixedSize())
        {
            if ((front.w != front.requested_w) ||
                (front.h != front.requested_h))
            {
                Layer::State& editDraw(mDrawingState);
                editDraw.w = editDraw.requested_w;
                editDraw.h = editDraw.requested_h;
                Layer::State& editTemp(currentState());
                editTemp.w = editDraw.w;
                editTemp.h = editDraw.h;
                recomputeVisibleRegions = true;
            }
            mFreezeLock.clear();
        }
    }
}
void Layer::unlockPageFlip(
        const Transform& planeTransform, Region& outDirtyRegion)
{
    Region dirtyRegion(mPostedDirtyRegion);
    if (!dirtyRegion.isEmpty()) {
        mPostedDirtyRegion.clear();
        const Layer::State& s(drawingState());
        const Transform tr(planeTransform * s.transform);
        dirtyRegion = tr.transform(dirtyRegion);
        dirtyRegion.andSelf(visibleRegionScreen);
        outDirtyRegion.orSelf(dirtyRegion);
    }
    if (visibleRegionScreen.isEmpty()) {
        mFreezeLock.clear();
    }
}
void Layer::dump(String8& result, char* buffer, size_t SIZE) const
{
    LayerBaseClient::dump(result, buffer, SIZE);
    sp<const GraphicBuffer> buf0(mActiveBuffer);
    uint32_t w0=0, h0=0, s0=0, f0=0;
    if (buf0 != 0) {
        w0 = buf0->getWidth();
        h0 = buf0->getHeight();
        s0 = buf0->getStride();
        f0 = buf0->format;
    }
    snprintf(buffer, SIZE,
            "      "
            "format=%2d, activeBuffer=[%3ux%3u:%3u,%3u],"
            " freezeLock=%p, queued-frames=%d\n",
            mFormat, w0, h0, s0,f0,
            getFreezeLock().get(), mQueuedFrames);
    result.append(buffer);
    if (mSurfaceTexture != 0) {
        mSurfaceTexture->dump(result, "            ", buffer, SIZE);
    }
}
uint32_t Layer::getEffectiveUsage(uint32_t usage) const
{
    if (mProtectedByApp) {
        usage |= GraphicBuffer::USAGE_PROTECTED;
    }
    return usage;
}
};
