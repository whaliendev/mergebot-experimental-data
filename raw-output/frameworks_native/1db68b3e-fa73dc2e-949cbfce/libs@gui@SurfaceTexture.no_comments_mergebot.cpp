#define LOG_TAG "SurfaceTexture"
#define GL_GLEXT_PROTOTYPES 
#define EGL_EGLEXT_PROTOTYPES 
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gui/SurfaceTexture.h>
#include <hardware/hardware.h>
#include <private/gui/ComposerService.h>
#include <surfaceflinger/ISurfaceComposer.h>
#include <surfaceflinger/SurfaceComposerClient.h>
#include <surfaceflinger/IGraphicBufferAlloc.h>
#include <utils/Log.h>
#include <utils/String8.h>
#ifdef ALLOW_DEQUEUE_CURRENT_BUFFER
#define FLAG_ALLOW_DEQUEUE_CURRENT_BUFFER true
#warning "ALLOW_DEQUEUE_CURRENT_BUFFER enabled"
#else
#define FLAG_ALLOW_DEQUEUE_CURRENT_BUFFER false
#endif
#ifdef USE_FENCE_SYNC
#ifdef ALLOW_DEQUEUE_CURRENT_BUFFER
#error "USE_FENCE_SYNC and ALLOW_DEQUEUE_CURRENT_BUFFER are incompatible"
#endif
#endif
#define ST_LOGV(x,...) ALOGV("[%s] "x, mName.string(), ##__VA_ARGS__)
#define ST_LOGD(x,...) ALOGD("[%s] "x, mName.string(), ##__VA_ARGS__)
#define ST_LOGI(x,...) ALOGI("[%s] "x, mName.string(), ##__VA_ARGS__)
#define ST_LOGW(x,...) ALOGW("[%s] "x, mName.string(), ##__VA_ARGS__)
#define ST_LOGE(x,...) ALOGE("[%s] "x, mName.string(), ##__VA_ARGS__)
#define ST_LOGV(x,...) LOGV("[%s] "x, mName.string(), ##__VA_ARGS__)
#define ST_LOGD(x,...) LOGD("[%s] "x, mName.string(), ##__VA_ARGS__)
#define ST_LOGI(x,...) LOGI("[%s] "x, mName.string(), ##__VA_ARGS__)
#define ST_LOGW(x,...) LOGW("[%s] "x, mName.string(), ##__VA_ARGS__)
#define ST_LOGE(x,...) LOGE("[%s] "x, mName.string(), ##__VA_ARGS__)
namespace android {
static float mtxIdentity[16] = {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1,
};
static float mtxFlipH[16] = {
    -1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    1, 0, 0, 1,
};
static float mtxFlipV[16] = {
    1, 0, 0, 0,
    0, -1, 0, 0,
    0, 0, 1, 0,
    0, 1, 0, 1,
};
static float mtxRot90[16] = {
    0, 1, 0, 0,
    -1, 0, 0, 0,
    0, 0, 1, 0,
    1, 0, 0, 1,
};
static float mtxRot180[16] = {
    -1, 0, 0, 0,
    0, -1, 0, 0,
    0, 0, 1, 0,
    1, 1, 0, 1,
};
static float mtxRot270[16] = {
    0, -1, 0, 0,
    1, 0, 0, 0,
    0, 0, 1, 0,
    0, 1, 0, 1,
};
static void mtxMul(float out[16], const float a[16], const float b[16]);
static int32_t createProcessUniqueId() {
    static volatile int32_t globalCounter = 0;
    return android_atomic_inc(&globalCounter);
}
SurfaceTexture::SurfaceTexture(GLuint tex, bool allowSynchronousMode,
        GLenum texTarget, bool useFenceSync) :
    mDefaultWidth(1),
    mDefaultHeight(1),
    mPixelFormat(PIXEL_FORMAT_RGBA_8888),
    mBufferCount(MIN_ASYNC_BUFFER_SLOTS),
    mClientBufferCount(0),
    mServerBufferCount(MIN_ASYNC_BUFFER_SLOTS),
    mCurrentTexture(INVALID_BUFFER_SLOT),
    mCurrentTransform(0),
    mCurrentTimestamp(0),
    mNextTransform(0),
    mNextScalingMode(NATIVE_WINDOW_SCALING_MODE_FREEZE),
    mTexName(tex),
    mSynchronousMode(false),
    mAllowSynchronousMode(allowSynchronousMode),
    mConnectedApi(NO_CONNECTED_API),
    mAbandoned(false),
#ifdef USE_FENCE_SYNC
    mUseFenceSync(useFenceSync),
#else
    mUseFenceSync(false),
#endif
    mTexTarget(texTarget),
    mFrameCounter(0) {
    mName = String8::format("unnamed-%d-%d", getpid(), createProcessUniqueId());
    ST_LOGV("SurfaceTexture");
    sp<ISurfaceComposer> composer(ComposerService::getComposerService());
    mGraphicBufferAlloc = composer->createGraphicBufferAlloc();
    mNextCrop.makeInvalid();
    memcpy(mCurrentTransformMatrix, mtxIdentity,
            sizeof(mCurrentTransformMatrix));
}
SurfaceTexture::~SurfaceTexture() {
    ST_LOGV("~SurfaceTexture");
    freeAllBuffersLocked();
}
status_t SurfaceTexture::setBufferCountServerLocked(int bufferCount) {
    if (bufferCount > NUM_BUFFER_SLOTS)
        return BAD_VALUE;
    if (bufferCount == mBufferCount)
        return OK;
    if (!mClientBufferCount &&
        bufferCount >= mBufferCount) {
        mBufferCount = bufferCount;
        mServerBufferCount = bufferCount;
        mDequeueCondition.signal();
    } else {
        if (bufferCount < 2)
            return BAD_VALUE;
        mServerBufferCount = bufferCount;
    }
    return OK;
}
status_t SurfaceTexture::setBufferCountServer(int bufferCount) {
    Mutex::Autolock lock(mMutex);
    return setBufferCountServerLocked(bufferCount);
}
status_t SurfaceTexture::setBufferCount(int bufferCount) {
    ST_LOGV("setBufferCount: count=%d", bufferCount);
    Mutex::Autolock lock(mMutex);
    if (mAbandoned) {
        ST_LOGE("setBufferCount: SurfaceTexture has been abandoned!");
        return NO_INIT;
    }
    if (bufferCount > NUM_BUFFER_SLOTS) {
        ST_LOGE("setBufferCount: bufferCount larger than slots available");
        return BAD_VALUE;
    }
    for (int i=0 ; i<mBufferCount ; i++) {
        if (mSlots[i].mBufferState == BufferSlot::DEQUEUED) {
            ST_LOGE("setBufferCount: client owns some buffers");
            return -EINVAL;
        }
    }
    const int minBufferSlots = mSynchronousMode ?
            MIN_SYNC_BUFFER_SLOTS : MIN_ASYNC_BUFFER_SLOTS;
    if (bufferCount == 0) {
        mClientBufferCount = 0;
        bufferCount = (mServerBufferCount >= minBufferSlots) ?
                mServerBufferCount : minBufferSlots;
        return setBufferCountServerLocked(bufferCount);
    }
    if (bufferCount < minBufferSlots) {
        ST_LOGE("setBufferCount: requested buffer count (%d) is less than "
                "minimum (%d)", bufferCount, minBufferSlots);
        return BAD_VALUE;
    }
    freeAllBuffersLocked();
    mBufferCount = bufferCount;
    mClientBufferCount = bufferCount;
    mCurrentTexture = INVALID_BUFFER_SLOT;
    mQueue.clear();
    mDequeueCondition.signal();
    return OK;
}
status_t SurfaceTexture::setDefaultBufferSize(uint32_t w, uint32_t h)
{
    ST_LOGV("setDefaultBufferSize: w=%d, h=%d", w, h);
    if (!w || !h) {
        ST_LOGE("setDefaultBufferSize: dimensions cannot be 0 (w=%d, h=%d)",
                w, h);
        return BAD_VALUE;
    }
    Mutex::Autolock lock(mMutex);
    mDefaultWidth = w;
    mDefaultHeight = h;
    return OK;
}
status_t SurfaceTexture::requestBuffer(int slot, sp<GraphicBuffer>* buf) {
    ST_LOGV("requestBuffer: slot=%d", slot);
    Mutex::Autolock lock(mMutex);
    if (mAbandoned) {
        ST_LOGE("requestBuffer: SurfaceTexture has been abandoned!");
        return NO_INIT;
    }
    if (slot < 0 || mBufferCount <= slot) {
        ST_LOGE("requestBuffer: slot index out of range [0, %d]: %d",
                mBufferCount, slot);
        return BAD_VALUE;
    }
    mSlots[slot].mRequestBufferCalled = true;
    *buf = mSlots[slot].mGraphicBuffer;
    return NO_ERROR;
}
status_t SurfaceTexture::dequeueBuffer(int *outBuf, uint32_t w, uint32_t h,
        uint32_t format, uint32_t usage) {
    ST_LOGV("dequeueBuffer: w=%d h=%d fmt=%#x usage=%#x", w, h, format, usage);
    if ((w && !h) || (!w && h)) {
        ST_LOGE("dequeueBuffer: invalid size: w=%u, h=%u", w, h);
        return BAD_VALUE;
    }
    status_t returnFlags(OK);
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLSyncKHR fence = EGL_NO_SYNC_KHR;
    {
        Mutex::Autolock lock(mMutex);
        int found = -1;
        int foundSync = -1;
        int dequeuedCount = 0;
        bool tryAgain = true;
        while (tryAgain) {
            if (mAbandoned) {
                ST_LOGE("dequeueBuffer: SurfaceTexture has been abandoned!");
                return NO_INIT;
            }
            const int minBufferCountNeeded = mSynchronousMode ?
                    MIN_SYNC_BUFFER_SLOTS : MIN_ASYNC_BUFFER_SLOTS;
            const bool numberOfBuffersNeedsToChange = !mClientBufferCount &&
                    ((mServerBufferCount != mBufferCount) ||
                            (mServerBufferCount < minBufferCountNeeded));
            if (!mQueue.isEmpty() && numberOfBuffersNeedsToChange) {
                mDequeueCondition.wait(mMutex);
                continue;
            }
            if (numberOfBuffersNeedsToChange) {
                freeAllBuffersLocked();
                mBufferCount = mServerBufferCount;
                if (mBufferCount < minBufferCountNeeded)
                    mBufferCount = minBufferCountNeeded;
                mCurrentTexture = INVALID_BUFFER_SLOT;
                returnFlags |= ISurfaceTexture::RELEASE_ALL_BUFFERS;
            }
            found = INVALID_BUFFER_SLOT;
            foundSync = INVALID_BUFFER_SLOT;
            dequeuedCount = 0;
            for (int i = 0; i < mBufferCount; i++) {
                const int state = mSlots[i].mBufferState;
                if (state == BufferSlot::DEQUEUED) {
                    dequeuedCount++;
                }
                ALOGW_IF((state == BufferSlot::FREE) && (mCurrentTexture==i),
                        "dequeueBuffer: buffer %d is both FREE and current!",
                        i);
                if (FLAG_ALLOW_DEQUEUE_CURRENT_BUFFER) {
                    if (state == BufferSlot::FREE || i == mCurrentTexture) {
                        foundSync = i;
                        if (i != mCurrentTexture) {
                            found = i;
                            break;
                        }
                    }
                } else {
                    if (state == BufferSlot::FREE) {
                        bool isOlder = mSlots[i].mFrameNumber <
                                mSlots[found].mFrameNumber;
                        if (found < 0 || isOlder) {
                            foundSync = i;
                            found = i;
                        }
                    }
                }
            }
            if (!mClientBufferCount && dequeuedCount) {
                ST_LOGE("dequeueBuffer: can't dequeue multiple buffers without "
                        "setting the buffer count");
                return -EINVAL;
            }
            bool bufferHasBeenQueued = mCurrentTexture != INVALID_BUFFER_SLOT;
            if (bufferHasBeenQueued) {
                const int avail = mBufferCount - (dequeuedCount+1);
                if (avail < (MIN_UNDEQUEUED_BUFFERS-int(mSynchronousMode))) {
                    ST_LOGE("dequeueBuffer: MIN_UNDEQUEUED_BUFFERS=%d exceeded "
                            "(dequeued=%d)",
                            MIN_UNDEQUEUED_BUFFERS-int(mSynchronousMode),
                            dequeuedCount);
                    return -EBUSY;
                }
            }
            tryAgain = mSynchronousMode && (foundSync == INVALID_BUFFER_SLOT);
            if (tryAgain) {
                mDequeueCondition.wait(mMutex);
            }
        }
        if (mSynchronousMode && found == INVALID_BUFFER_SLOT) {
            found = foundSync;
        }
        if (found == INVALID_BUFFER_SLOT) {
            ST_LOGE("dequeueBuffer: no available buffer slots");
            return -EBUSY;
        }
        const int buf = found;
        *outBuf = found;
        const bool useDefaultSize = !w && !h;
        if (useDefaultSize) {
            w = mDefaultWidth;
            h = mDefaultHeight;
        }
        const bool updateFormat = (format != 0);
        if (!updateFormat) {
            format = mPixelFormat;
        }
        mSlots[buf].mBufferState = BufferSlot::DEQUEUED;
        const sp<GraphicBuffer>& buffer(mSlots[buf].mGraphicBuffer);
        if ((buffer == NULL) ||
            (uint32_t(buffer->width) != w) ||
            (uint32_t(buffer->height) != h) ||
            (uint32_t(buffer->format) != format) ||
            ((uint32_t(buffer->usage) & usage) != usage))
        {
            usage |= GraphicBuffer::USAGE_HW_TEXTURE;
            status_t error;
            sp<GraphicBuffer> graphicBuffer(
                    mGraphicBufferAlloc->createGraphicBuffer(
                            w, h, format, usage, &error));
            if (graphicBuffer == 0) {
                ST_LOGE("dequeueBuffer: SurfaceComposer::createGraphicBuffer "
                        "failed");
                return error;
            }
            if (updateFormat) {
                mPixelFormat = format;
            }
            mSlots[buf].mGraphicBuffer = graphicBuffer;
            mSlots[buf].mRequestBufferCalled = false;
            mSlots[buf].mFence = EGL_NO_SYNC_KHR;
            if (mSlots[buf].mEglImage != EGL_NO_IMAGE_KHR) {
                eglDestroyImageKHR(mSlots[buf].mEglDisplay,
                        mSlots[buf].mEglImage);
                mSlots[buf].mEglImage = EGL_NO_IMAGE_KHR;
                mSlots[buf].mEglDisplay = EGL_NO_DISPLAY;
            }
            if (mCurrentTexture == buf) {
                mCurrentTexture = INVALID_BUFFER_SLOT;
            }
            returnFlags |= ISurfaceTexture::BUFFER_NEEDS_REALLOCATION;
        }
        dpy = mSlots[buf].mEglDisplay;
        fence = mSlots[buf].mFence;
        mSlots[buf].mFence = EGL_NO_SYNC_KHR;
    }
    if (fence != EGL_NO_SYNC_KHR) {
        EGLint result = eglClientWaitSyncKHR(dpy, fence, 0, 1000000000);
        if (result == EGL_FALSE) {
            ALOGE("dequeueBuffer: error waiting for fence: %#x", eglGetError());
        } else if (result == EGL_TIMEOUT_EXPIRED_KHR) {
            ALOGE("dequeueBuffer: timeout waiting for fence");
        }
        eglDestroySyncKHR(dpy, fence);
    }
    ST_LOGV("dequeueBuffer: returning slot=%d buf=%p flags=%#x", *outBuf,
            mSlots[*outBuf].mGraphicBuffer->handle, returnFlags);
    return returnFlags;
}
status_t SurfaceTexture::setSynchronousMode(bool enabled) {
    ST_LOGV("setSynchronousMode: enabled=%d", enabled);
    Mutex::Autolock lock(mMutex);
    if (mAbandoned) {
        ST_LOGE("setSynchronousMode: SurfaceTexture has been abandoned!");
        return NO_INIT;
    }
    status_t err = OK;
    if (!mAllowSynchronousMode && enabled)
        return err;
    if (!enabled) {
        err = drainQueueLocked();
        if (err != NO_ERROR)
            return err;
    }
    if (mSynchronousMode != enabled) {
        mSynchronousMode = enabled;
        mDequeueCondition.signal();
    }
    return err;
}
status_t SurfaceTexture::queueBuffer(int buf, int64_t timestamp,
        uint32_t* outWidth, uint32_t* outHeight, uint32_t* outTransform) {
    ST_LOGV("queueBuffer: slot=%d time=%lld", buf, timestamp);
    sp<FrameAvailableListener> listener;
    {
        Mutex::Autolock lock(mMutex);
        if (mAbandoned) {
            ST_LOGE("queueBuffer: SurfaceTexture has been abandoned!");
            return NO_INIT;
        }
        if (buf < 0 || buf >= mBufferCount) {
            ST_LOGE("queueBuffer: slot index out of range [0, %d]: %d",
                    mBufferCount, buf);
            return -EINVAL;
        } else if (mSlots[buf].mBufferState != BufferSlot::DEQUEUED) {
            ST_LOGE("queueBuffer: slot %d is not owned by the client "
                    "(state=%d)", buf, mSlots[buf].mBufferState);
            return -EINVAL;
        } else if (buf == mCurrentTexture) {
            ST_LOGE("queueBuffer: slot %d is current!", buf);
            return -EINVAL;
        } else if (!mSlots[buf].mRequestBufferCalled) {
            ST_LOGE("queueBuffer: slot %d was enqueued without requesting a "
                    "buffer", buf);
            return -EINVAL;
        }
        if (mSynchronousMode) {
            mQueue.push_back(buf);
            listener = mFrameAvailableListener;
        } else {
            if (mQueue.empty()) {
                mQueue.push_back(buf);
                listener = mFrameAvailableListener;
            } else {
                Fifo::iterator front(mQueue.begin());
                mSlots[*front].mBufferState = BufferSlot::FREE;
                *front = buf;
            }
        }
        mSlots[buf].mBufferState = BufferSlot::QUEUED;
        mSlots[buf].mCrop = mNextCrop;
        mSlots[buf].mTransform = mNextTransform;
        mSlots[buf].mScalingMode = mNextScalingMode;
        mSlots[buf].mTimestamp = timestamp;
        mFrameCounter++;
        mSlots[buf].mFrameNumber = mFrameCounter;
        mDequeueCondition.signal();
        *outWidth = mDefaultWidth;
        *outHeight = mDefaultHeight;
        *outTransform = 0;
    }
    if (listener != 0) {
        listener->onFrameAvailable();
    }
    return OK;
}
void SurfaceTexture::cancelBuffer(int buf) {
    ST_LOGV("cancelBuffer: slot=%d", buf);
    Mutex::Autolock lock(mMutex);
    if (mAbandoned) {
        ST_LOGW("cancelBuffer: SurfaceTexture has been abandoned!");
        return;
    }
    if (buf < 0 || buf >= mBufferCount) {
        ST_LOGE("cancelBuffer: slot index out of range [0, %d]: %d",
                mBufferCount, buf);
        return;
    } else if (mSlots[buf].mBufferState != BufferSlot::DEQUEUED) {
        ST_LOGE("cancelBuffer: slot %d is not owned by the client (state=%d)",
                buf, mSlots[buf].mBufferState);
        return;
    }
    mSlots[buf].mBufferState = BufferSlot::FREE;
    mSlots[buf].mFrameNumber = 0;
    mDequeueCondition.signal();
}
status_t SurfaceTexture::setCrop(const Rect& crop) {
    ST_LOGV("setCrop: crop=[%d,%d,%d,%d]", crop.left, crop.top, crop.right,
            crop.bottom);
    Mutex::Autolock lock(mMutex);
    if (mAbandoned) {
        ST_LOGE("setCrop: SurfaceTexture has been abandoned!");
        return NO_INIT;
    }
    mNextCrop = crop;
    return OK;
}
status_t SurfaceTexture::setTransform(uint32_t transform) {
    ST_LOGV("setTransform: xform=%#x", transform);
    Mutex::Autolock lock(mMutex);
    if (mAbandoned) {
        ST_LOGE("setTransform: SurfaceTexture has been abandoned!");
        return NO_INIT;
    }
    mNextTransform = transform;
    return OK;
}
status_t SurfaceTexture::connect(int api,
        uint32_t* outWidth, uint32_t* outHeight, uint32_t* outTransform) {
    ST_LOGV("connect: api=%d", api);
    Mutex::Autolock lock(mMutex);
    if (mAbandoned) {
        ST_LOGE("connect: SurfaceTexture has been abandoned!");
        return NO_INIT;
    }
    int err = NO_ERROR;
    switch (api) {
        case NATIVE_WINDOW_API_EGL:
        case NATIVE_WINDOW_API_CPU:
        case NATIVE_WINDOW_API_MEDIA:
        case NATIVE_WINDOW_API_CAMERA:
            if (mConnectedApi != NO_CONNECTED_API) {
                ST_LOGE("connect: already connected (cur=%d, req=%d)",
                        mConnectedApi, api);
                err = -EINVAL;
            } else {
                mConnectedApi = api;
                *outWidth = mDefaultWidth;
                *outHeight = mDefaultHeight;
                *outTransform = 0;
            }
            break;
        default:
            err = -EINVAL;
            break;
    }
    return err;
}
status_t SurfaceTexture::disconnect(int api) {
    ST_LOGV("disconnect: api=%d", api);
    Mutex::Autolock lock(mMutex);
    if (mAbandoned) {
        return NO_ERROR;
    }
    int err = NO_ERROR;
    switch (api) {
        case NATIVE_WINDOW_API_EGL:
        case NATIVE_WINDOW_API_CPU:
        case NATIVE_WINDOW_API_MEDIA:
        case NATIVE_WINDOW_API_CAMERA:
            if (mConnectedApi == api) {
                drainQueueAndFreeBuffersLocked();
                mConnectedApi = NO_CONNECTED_API;
                mNextCrop.makeInvalid();
                mNextScalingMode = NATIVE_WINDOW_SCALING_MODE_FREEZE;
                mNextTransform = 0;
                mDequeueCondition.signal();
            } else {
                ST_LOGE("disconnect: connected to another api (cur=%d, req=%d)",
                        mConnectedApi, api);
                err = -EINVAL;
            }
            break;
        default:
            ST_LOGE("disconnect: unknown API %d", api);
            err = -EINVAL;
            break;
    }
    return err;
}
status_t SurfaceTexture::setScalingMode(int mode) {
    ST_LOGV("setScalingMode: mode=%d", mode);
    switch (mode) {
        case NATIVE_WINDOW_SCALING_MODE_FREEZE:
        case NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW:
            break;
        default:
            ST_LOGE("unknown scaling mode: %d", mode);
            return BAD_VALUE;
    }
    Mutex::Autolock lock(mMutex);
    mNextScalingMode = mode;
    return OK;
}
status_t SurfaceTexture::updateTexImage() {
    ST_LOGV("updateTexImage");
    Mutex::Autolock lock(mMutex);
    if (mAbandoned) {
        ST_LOGE("calling updateTexImage() on an abandoned SurfaceTexture");
        return NO_INIT;
    }
    if (!mQueue.empty()) {
        Fifo::iterator front(mQueue.begin());
        int buf = *front;
        EGLImageKHR image = mSlots[buf].mEglImage;
        EGLDisplay dpy = eglGetCurrentDisplay();
        if (image == EGL_NO_IMAGE_KHR) {
            if (mSlots[buf].mGraphicBuffer == 0) {
                ST_LOGE("buffer at slot %d is null", buf);
                return BAD_VALUE;
            }
            image = createImage(dpy, mSlots[buf].mGraphicBuffer);
            mSlots[buf].mEglImage = image;
            mSlots[buf].mEglDisplay = dpy;
            if (image == EGL_NO_IMAGE_KHR) {
                return -EINVAL;
            }
        }
        GLint error;
        while ((error = glGetError()) != GL_NO_ERROR) {
            ST_LOGW("updateTexImage: clearing GL error: %#04x", error);
        }
        glBindTexture(mTexTarget, mTexName);
        glEGLImageTargetTexture2DOES(mTexTarget, (GLeglImageOES)image);
        bool failed = false;
        while ((error = glGetError()) != GL_NO_ERROR) {
            ST_LOGE("error binding external texture image %p (slot %d): %#04x",
                    image, buf, error);
            failed = true;
        }
        if (failed) {
            return -EINVAL;
        }
        if (mCurrentTexture != INVALID_BUFFER_SLOT) {
            if (mUseFenceSync) {
                EGLSyncKHR fence = eglCreateSyncKHR(dpy, EGL_SYNC_FENCE_KHR,
                        NULL);
                if (fence == EGL_NO_SYNC_KHR) {
                    ALOGE("updateTexImage: error creating fence: %#x",
                            eglGetError());
                    return -EINVAL;
                }
                glFlush();
                mSlots[mCurrentTexture].mFence = fence;
            }
        }
        ST_LOGV("updateTexImage: (slot=%d buf=%p) -> (slot=%d buf=%p)",
                mCurrentTexture,
                mCurrentTextureBuf != NULL ? mCurrentTextureBuf->handle : 0,
                buf, mSlots[buf].mGraphicBuffer->handle);
        if (mCurrentTexture != INVALID_BUFFER_SLOT) {
            if (mSlots[mCurrentTexture].mBufferState == BufferSlot::QUEUED) {
                mSlots[mCurrentTexture].mBufferState = BufferSlot::FREE;
            }
        }
        mCurrentTexture = buf;
        mCurrentTextureBuf = mSlots[buf].mGraphicBuffer;
        mCurrentCrop = mSlots[buf].mCrop;
        mCurrentTransform = mSlots[buf].mTransform;
        mCurrentScalingMode = mSlots[buf].mScalingMode;
        mCurrentTimestamp = mSlots[buf].mTimestamp;
        computeCurrentTransformMatrix();
        mQueue.erase(front);
        mDequeueCondition.signal();
    } else {
        glBindTexture(mTexTarget, mTexName);
    }
    return OK;
}
bool SurfaceTexture::isExternalFormat(uint32_t format)
{
    switch (format) {
    case HAL_PIXEL_FORMAT_YV12:
    case HAL_PIXEL_FORMAT_YCbCr_422_SP:
    case HAL_PIXEL_FORMAT_YCrCb_420_SP:
    case HAL_PIXEL_FORMAT_YCbCr_422_I:
        return true;
    }
    if (format>=0x100 && format<=0x1FF)
        return true;
    return false;
}
GLenum SurfaceTexture::getCurrentTextureTarget() const {
    return mTexTarget;
}
void SurfaceTexture::getTransformMatrix(float mtx[16]) {
    Mutex::Autolock lock(mMutex);
    memcpy(mtx, mCurrentTransformMatrix, sizeof(mCurrentTransformMatrix));
}
void SurfaceTexture::computeCurrentTransformMatrix() {
    ST_LOGV("computeCurrentTransformMatrix");
    float xform[16];
    for (int i = 0; i < 16; i++) {
        xform[i] = mtxIdentity[i];
    }
    if (mCurrentTransform & NATIVE_WINDOW_TRANSFORM_FLIP_H) {
        float result[16];
        mtxMul(result, xform, mtxFlipH);
        for (int i = 0; i < 16; i++) {
            xform[i] = result[i];
        }
    }
    if (mCurrentTransform & NATIVE_WINDOW_TRANSFORM_FLIP_V) {
        float result[16];
        mtxMul(result, xform, mtxFlipV);
        for (int i = 0; i < 16; i++) {
            xform[i] = result[i];
        }
    }
    if (mCurrentTransform & NATIVE_WINDOW_TRANSFORM_ROT_90) {
        float result[16];
        mtxMul(result, xform, mtxRot90);
        for (int i = 0; i < 16; i++) {
            xform[i] = result[i];
        }
    }
    sp<GraphicBuffer>& buf(mSlots[mCurrentTexture].mGraphicBuffer);
    float tx, ty, sx, sy;
    if (!mCurrentCrop.isEmpty()) {
        int xshrink = 0, yshrink = 0;
        if (mCurrentCrop.left > 0) {
            tx = float(mCurrentCrop.left + 1) / float(buf->getWidth());
            xshrink++;
        } else {
            tx = 0.0f;
        }
        if (mCurrentCrop.right < int32_t(buf->getWidth())) {
            xshrink++;
        }
        if (mCurrentCrop.bottom < int32_t(buf->getHeight())) {
            ty = (float(buf->getHeight() - mCurrentCrop.bottom) + 1.0f) /
                    float(buf->getHeight());
            yshrink++;
        } else {
            ty = 0.0f;
        }
        if (mCurrentCrop.top > 0) {
            yshrink++;
        }
        sx = float(mCurrentCrop.width() - xshrink) / float(buf->getWidth());
        sy = float(mCurrentCrop.height() - yshrink) / float(buf->getHeight());
    } else {
        tx = 0.0f;
        ty = 0.0f;
        sx = 1.0f;
        sy = 1.0f;
    }
    float crop[16] = {
        sx, 0, 0, 0,
        0, sy, 0, 0,
        0, 0, 1, 0,
        tx, ty, 0, 1,
    };
    float mtxBeforeFlipV[16];
    mtxMul(mtxBeforeFlipV, crop, xform);
    mtxMul(mCurrentTransformMatrix, mtxFlipV, mtxBeforeFlipV);
}
nsecs_t SurfaceTexture::getTimestamp() {
    ST_LOGV("getTimestamp");
    Mutex::Autolock lock(mMutex);
    return mCurrentTimestamp;
}
void SurfaceTexture::setFrameAvailableListener(
        const sp<FrameAvailableListener>& listener) {
    ST_LOGV("setFrameAvailableListener");
    Mutex::Autolock lock(mMutex);
    mFrameAvailableListener = listener;
}
void SurfaceTexture::freeBufferLocked(int i) {
    mSlots[i].mGraphicBuffer = 0;
    mSlots[i].mBufferState = BufferSlot::FREE;
    mSlots[i].mFrameNumber = 0;
    if (mSlots[i].mEglImage != EGL_NO_IMAGE_KHR) {
        eglDestroyImageKHR(mSlots[i].mEglDisplay, mSlots[i].mEglImage);
        mSlots[i].mEglImage = EGL_NO_IMAGE_KHR;
        mSlots[i].mEglDisplay = EGL_NO_DISPLAY;
    }
}
void SurfaceTexture::freeAllBuffersLocked() {
    ALOGW_IF(!mQueue.isEmpty(),
            "freeAllBuffersLocked called but mQueue is not empty");
    mCurrentTexture = INVALID_BUFFER_SLOT;
    for (int i = 0; i < NUM_BUFFER_SLOTS; i++) {
        freeBufferLocked(i);
    }
}
void SurfaceTexture::freeAllBuffersExceptHeadLocked() {
    ALOGW_IF(!mQueue.isEmpty(),
            "freeAllBuffersExceptCurrentLocked called but mQueue is not empty");
    int head = -1;
    if (!mQueue.empty()) {
        Fifo::iterator front(mQueue.begin());
        head = *front;
    }
    mCurrentTexture = INVALID_BUFFER_SLOT;
    for (int i = 0; i < NUM_BUFFER_SLOTS; i++) {
        if (i != head) {
            freeBufferLocked(i);
        }
    }
}
status_t SurfaceTexture::drainQueueLocked() {
    while (mSynchronousMode && !mQueue.isEmpty()) {
        mDequeueCondition.wait(mMutex);
        if (mAbandoned) {
            ST_LOGE("drainQueueLocked: SurfaceTexture has been abandoned!");
            return NO_INIT;
        }
        if (mConnectedApi == NO_CONNECTED_API) {
            ST_LOGE("drainQueueLocked: SurfaceTexture is not connected!");
            return NO_INIT;
        }
    }
    return NO_ERROR;
}
status_t SurfaceTexture::drainQueueAndFreeBuffersLocked() {
    status_t err = drainQueueLocked();
    if (err == NO_ERROR) {
        if (mSynchronousMode) {
            freeAllBuffersLocked();
        } else {
            freeAllBuffersExceptHeadLocked();
        }
    }
    return err;
}
EGLImageKHR SurfaceTexture::createImage(EGLDisplay dpy,
        const sp<GraphicBuffer>& graphicBuffer) {
    EGLClientBuffer cbuf = (EGLClientBuffer)graphicBuffer->getNativeBuffer();
    EGLint attrs[] = {
        EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
        EGL_NONE,
    };
    EGLImageKHR image = eglCreateImageKHR(dpy, EGL_NO_CONTEXT,
            EGL_NATIVE_BUFFER_ANDROID, cbuf, attrs);
    if (image == EGL_NO_IMAGE_KHR) {
        EGLint error = eglGetError();
        ST_LOGE("error creating EGLImage: %#x", error);
    }
    return image;
}
sp<GraphicBuffer> SurfaceTexture::getCurrentBuffer() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentTextureBuf;
}
Rect SurfaceTexture::getCurrentCrop() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentCrop;
}
uint32_t SurfaceTexture::getCurrentTransform() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentTransform;
}
uint32_t SurfaceTexture::getCurrentScalingMode() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentScalingMode;
}
bool SurfaceTexture::isSynchronousMode() const {
    Mutex::Autolock lock(mMutex);
    return mSynchronousMode;
}
int SurfaceTexture::query(int what, int* outValue)
{
    Mutex::Autolock lock(mMutex);
    if (mAbandoned) {
        ST_LOGE("query: SurfaceTexture has been abandoned!");
        return NO_INIT;
    }
    int value;
    switch (what) {
    case NATIVE_WINDOW_WIDTH:
        value = mDefaultWidth;
        break;
    case NATIVE_WINDOW_HEIGHT:
        value = mDefaultHeight;
        break;
    case NATIVE_WINDOW_FORMAT:
        value = mPixelFormat;
        break;
    case NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS:
        value = mSynchronousMode ?
                (MIN_UNDEQUEUED_BUFFERS-1) : MIN_UNDEQUEUED_BUFFERS;
        break;
    default:
        return BAD_VALUE;
    }
    outValue[0] = value;
    return NO_ERROR;
}
void SurfaceTexture::abandon() {
    Mutex::Autolock lock(mMutex);
    mQueue.clear();
    mAbandoned = true;
    mCurrentTextureBuf.clear();
    freeAllBuffersLocked();
    mDequeueCondition.signal();
}
void SurfaceTexture::setName(const String8& name) {
    mName = name;
}
void SurfaceTexture::dump(String8& result) const
{
    char buffer[1024];
    dump(result, "", buffer, 1024);
}
void SurfaceTexture::dump(String8& result, const char* prefix,
        char* buffer, size_t SIZE) const
{
    Mutex::Autolock _l(mMutex);
    snprintf(buffer, SIZE,
            "%smBufferCount=%d, mSynchronousMode=%d, default-size=[%dx%d], "
            "mPixelFormat=%d, mTexName=%d\n",
            prefix, mBufferCount, mSynchronousMode, mDefaultWidth,
            mDefaultHeight, mPixelFormat, mTexName);
    result.append(buffer);
    String8 fifo;
    int fifoSize = 0;
    Fifo::const_iterator i(mQueue.begin());
    while (i != mQueue.end()) {
        snprintf(buffer, SIZE, "%02d ", *i++);
        fifoSize++;
        fifo.append(buffer);
    }
    snprintf(buffer, SIZE,
            "%scurrent: {crop=[%d,%d,%d,%d], transform=0x%02x, current=%d}\n"
            "%snext   : {crop=[%d,%d,%d,%d], transform=0x%02x, FIFO(%d)={%s}}\n"
            ,
            prefix, mCurrentCrop.left,
            mCurrentCrop.top, mCurrentCrop.right, mCurrentCrop.bottom,
            mCurrentTransform, mCurrentTexture,
            prefix, mNextCrop.left, mNextCrop.top, mNextCrop.right,
            mNextCrop.bottom, mNextTransform, fifoSize, fifo.string()
    );
    result.append(buffer);
    struct {
        const char * operator()(int state) const {
            switch (state) {
                case BufferSlot::DEQUEUED: return "DEQUEUED";
                case BufferSlot::QUEUED: return "QUEUED";
                case BufferSlot::FREE: return "FREE";
                default: return "Unknown";
            }
        }
    } stateName;
    for (int i=0 ; i<mBufferCount ; i++) {
        const BufferSlot& slot(mSlots[i]);
        snprintf(buffer, SIZE,
                "%s%s[%02d] "
                "state=%-8s, crop=[%d,%d,%d,%d], "
                "transform=0x%02x, timestamp=%lld",
                prefix, (i==mCurrentTexture)?">":" ", i,
                stateName(slot.mBufferState),
                slot.mCrop.left, slot.mCrop.top, slot.mCrop.right,
                slot.mCrop.bottom, slot.mTransform, slot.mTimestamp
        );
        result.append(buffer);
        const sp<GraphicBuffer>& buf(slot.mGraphicBuffer);
        if (buf != NULL) {
            snprintf(buffer, SIZE,
                    ", %p [%4ux%4u:%4u,%3X]",
                    buf->handle, buf->width, buf->height, buf->stride,
                    buf->format);
            result.append(buffer);
        }
        result.append("\n");
    }
}
static void mtxMul(float out[16], const float a[16], const float b[16]) {
    out[0] = a[0]*b[0] + a[4]*b[1] + a[8]*b[2] + a[12]*b[3];
    out[1] = a[1]*b[0] + a[5]*b[1] + a[9]*b[2] + a[13]*b[3];
    out[2] = a[2]*b[0] + a[6]*b[1] + a[10]*b[2] + a[14]*b[3];
    out[3] = a[3]*b[0] + a[7]*b[1] + a[11]*b[2] + a[15]*b[3];
    out[4] = a[0]*b[4] + a[4]*b[5] + a[8]*b[6] + a[12]*b[7];
    out[5] = a[1]*b[4] + a[5]*b[5] + a[9]*b[6] + a[13]*b[7];
    out[6] = a[2]*b[4] + a[6]*b[5] + a[10]*b[6] + a[14]*b[7];
    out[7] = a[3]*b[4] + a[7]*b[5] + a[11]*b[6] + a[15]*b[7];
    out[8] = a[0]*b[8] + a[4]*b[9] + a[8]*b[10] + a[12]*b[11];
    out[9] = a[1]*b[8] + a[5]*b[9] + a[9]*b[10] + a[13]*b[11];
    out[10] = a[2]*b[8] + a[6]*b[9] + a[10]*b[10] + a[14]*b[11];
    out[11] = a[3]*b[8] + a[7]*b[9] + a[11]*b[10] + a[15]*b[11];
    out[12] = a[0]*b[12] + a[4]*b[13] + a[8]*b[14] + a[12]*b[15];
    out[13] = a[1]*b[12] + a[5]*b[13] + a[9]*b[14] + a[13]*b[15];
    out[14] = a[2]*b[12] + a[6]*b[13] + a[10]*b[14] + a[14]*b[15];
    out[15] = a[3]*b[12] + a[7]*b[13] + a[11]*b[14] + a[15]*b[15];
}
}
