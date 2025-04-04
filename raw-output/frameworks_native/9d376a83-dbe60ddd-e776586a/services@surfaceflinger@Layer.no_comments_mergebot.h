#ifndef ANDROID_LAYER_H
#define ANDROID_LAYER_H 
#include <stdint.h>
#include <sys/types.h>
#include <gui/SurfaceTexture.h>
#include <pixelflinger/pixelflinger.h>
#include <ui/GraphicBuffer.h>
#include <ui/PixelFormat.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES/gl.h>
#include <GLES/glext.h>
#include "LayerBase.h"
#include "SurfaceTextureLayer.h"
#include "Transform.h"
#include "TextureManager.h"
namespace android {
class FreezeLock;
class Client;
class GLExtensions;
class Layer : public LayerBaseClient, private RefBase::Destroyer
{
private:
    friend class SurfaceTextureLayer;
    void onFrameQueued();
    virtual sp<ISurface> createSurface();
public:
            Layer(SurfaceFlinger* flinger, DisplayID display,
                    const sp<Client>& client);
    virtual ~Layer();
    virtual const char* getTypeId() const { return "Layer"; }
    status_t setBuffers(uint32_t w, uint32_t h,
            PixelFormat format, uint32_t flags=0);
    bool isFixedSize() const;
    virtual void setGeometry(hwc_layer_t* hwcl);
    virtual void setPerFrameData(hwc_layer_t* hwcl);
    virtual void onDraw(const Region& clip) const;
    virtual uint32_t doTransaction(uint32_t transactionFlags);
    virtual void lockPageFlip(bool& recomputeVisibleRegions);
    virtual void unlockPageFlip(const Transform& planeTransform, Region& outDirtyRegion);
    virtual bool isOpaque() const;
    virtual bool needsDithering() const { return mNeedsDithering; }
    virtual bool isSecure() const { return mSecure; }
    virtual bool isProtected() const;
    virtual void onRemoved();
    inline const sp<FreezeLock>& getFreezeLock() const { return mFreezeLock; }
protected:
    virtual void destroy(RefBase const* base);
    virtual void onFirstRef();
    virtual void dump(String8& result, char* scratch, size_t size) const;
private:
    uint32_t getEffectiveUsage(uint32_t usage) const;
    void setFixedSize(bool fixedSize);
    bool isCropped() const;
    static bool getOpacityForFormat(uint32_t format);
    sp<SurfaceTextureLayer> mSurfaceTexture;
    GLuint mTextureName;
    volatile int32_t mQueuedFrames;
    sp<GraphicBuffer> mActiveBuffer;
    GLfloat mTextureMatrix[16];
    Rect mCurrentCrop;
    uint32_t mCurrentTransform;
    bool mCurrentOpacity;
    PixelFormat mFormat;
    const GLExtensions& mGLExtensions;
    bool mOpaqueLayer;
    bool mNeedsDithering;
bool mSecure;
bool mProtectedByApp;
    Region mPostedDirtyRegion;
    sp<FreezeLock> mFreezeLock;
    mutable Mutex mLock;
    bool mFixedSize;
};
}
#endif
