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

namespace android {

// ---------------------------------------------------------------------------

class FreezeLock;
class Client;
class GLExtensions;

// ---------------------------------------------------------------------------

class Layer : public LayerBaseClient, private RefBase::Destroyer
{
public:
            Layer(SurfaceFlinger* flinger, DisplayID display,
                    const sp<Client>& client);

    virtual ~Layer();

    virtual const char* getTypeId() const { return "Layer"; }

    // the this layer's size and format
    status_t setBuffers(uint32_t w, uint32_t h, 
            PixelFormat format, uint32_t flags=0);

    // Set this Layer's buffers size
    bool isFixedSize() const;

    // LayerBase interface
    virtual void setGeometry(hwc_layer_t* hwcl);
    virtual void setPerFrameData(hwc_layer_t* hwcl);
    virtual void onDraw(const Region& clip) const;
    virtual uint32_t doTransaction(uint32_t transactionFlags);
    virtual void lockPageFlip(bool& recomputeVisibleRegions);
    virtual void unlockPageFlip(const Transform& planeTransform, Region& outDirtyRegion);
    virtual bool isOpaque() const;
    virtual bool needsDithering() const     { return mNeedsDithering; }
    virtual bool isSecure() const           { return mSecure; }
    virtual bool isProtected() const;
    virtual void onRemoved();

    // only for debugging
    inline const sp<FreezeLock>&  getFreezeLock() const { return mFreezeLock; }

protected:
    virtual void destroy(RefBase const* base);
    virtual void onFirstRef();
    virtual void dump(String8& result, char* scratch, size_t size) const;

private:
    friend class SurfaceTextureLayer;
    void onFrameQueued();
    virtual sp<ISurface> createSurface();
    uint32_t getEffectiveUsage(uint32_t usage) const;
    void setFixedSize(bool fixedSize);
    bool isCropped() const;
    static bool getOpacityForFormat(uint32_t format);

    // -----------------------------------------------------------------------

    // constants
    sp<SurfaceTextureLayer> mSurfaceTexture;
    GLuint mTextureName;

    // thread-safe
    volatile int32_t mQueuedFrames;

    // main thread
    sp<GraphicBuffer> mActiveBuffer;
    GLfloat mTextureMatrix[16];
    Rect mCurrentCrop;
    uint32_t mCurrentTransform;
    bool mCurrentOpacity;

    // constants
    PixelFormat mFormat;
    const GLExtensions& mGLExtensions;
    bool mOpaqueLayer;
    bool mNeedsDithering;

    // page-flip thread (currently main thread)
    bool mSecure;         // no screenshots
    bool mProtectedByApp; // application requires protected path to external sink
    Region mPostedDirtyRegion;

    // page-flip thread and transaction thread (currently main thread)
    sp<FreezeLock>  mFreezeLock;

    // binder thread, transaction thread
    mutable Mutex mLock;
    bool mFixedSize;
};

// ---------------------------------------------------------------------------

}; // namespace android

#endif // ANDROID_LAYER_H
