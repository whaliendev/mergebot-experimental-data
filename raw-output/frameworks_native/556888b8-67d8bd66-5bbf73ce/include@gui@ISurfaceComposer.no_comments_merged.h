#ifndef ANDROID_GUI_ISURFACE_COMPOSER_H
#define ANDROID_GUI_ISURFACE_COMPOSER_H 
#include <stdint.h>
#include <sys/types.h>
#include <utils/RefBase.h>
#include <utils/Errors.h>
#include <utils/Timers.h>
#include <utils/Vector.h>
#include <binder/IInterface.h>
#include <ui/FrameStats.h>
#include <ui/PixelFormat.h>
#include <gui/IGraphicBufferAlloc.h>
#include <gui/ISurfaceComposerClient.h>
namespace android {
class ComposerState;
class DisplayState;
<<<<<<< HEAD
struct DisplayInfo;
||||||| 5bbf73ced3
class DisplayInfo;
=======
class DisplayInfo;
class DisplayStatInfo;
>>>>>>> 67d8bd66
class IDisplayEventConnection;
class IMemoryHeap;
class Rect;
class ISurfaceComposer: public IInterface {
public:
    DECLARE_META_INTERFACE(SurfaceComposer);
    enum {
        eSynchronous = 0x01,
        eAnimation = 0x02,
    };
    enum {
        eDisplayIdMain = 0,
        eDisplayIdHdmi = 1
    };
    enum Rotation {
        eRotateNone = 0,
        eRotate90 = 1,
        eRotate180 = 2,
        eRotate270 = 3
    };
    virtual sp<ISurfaceComposerClient> createConnection() = 0;
    virtual sp<IGraphicBufferAlloc> createGraphicBufferAlloc() = 0;
    virtual sp<IDisplayEventConnection> createDisplayEventConnection() = 0;
    virtual sp<IBinder> createDisplay(const String8& displayName,
            bool secure) = 0;
    virtual void destroyDisplay(const sp<IBinder>& display) = 0;
    virtual sp<IBinder> getBuiltInDisplay(int32_t id) = 0;
    virtual void setTransactionState(const Vector<ComposerState>& state,
            const Vector<DisplayState>& displays, uint32_t flags) = 0;
    virtual void bootFinished() = 0;
    virtual bool authenticateSurfaceTexture(
            const sp<IGraphicBufferProducer>& surface) const = 0;
    virtual void setPowerMode(const sp<IBinder>& display, int mode) = 0;
    virtual status_t getDisplayConfigs(const sp<IBinder>& display,
            Vector<DisplayInfo>* configs) = 0;
    virtual status_t getDisplayStats(const sp<IBinder>& display,
            DisplayStatInfo* stats) = 0;
    virtual int getActiveConfig(const sp<IBinder>& display) = 0;
    virtual status_t setActiveConfig(const sp<IBinder>& display, int id) = 0;
    virtual status_t captureScreen(const sp<IBinder>& display,
            const sp<IGraphicBufferProducer>& producer,
            Rect sourceCrop, uint32_t reqWidth, uint32_t reqHeight,
            uint32_t minLayerZ, uint32_t maxLayerZ,
            bool useIdentityTransform,
            Rotation rotation = eRotateNone) = 0;
    virtual status_t clearAnimationFrameStats() = 0;
    virtual status_t getAnimationFrameStats(FrameStats* outStats) const = 0;
};
class BnSurfaceComposer: public BnInterface<ISurfaceComposer> {
public:
    enum {
        BOOT_FINISHED = IBinder::FIRST_CALL_TRANSACTION,
        CREATE_CONNECTION,
        CREATE_GRAPHIC_BUFFER_ALLOC,
        CREATE_DISPLAY_EVENT_CONNECTION,
        CREATE_DISPLAY,
        DESTROY_DISPLAY,
        GET_BUILT_IN_DISPLAY,
        SET_TRANSACTION_STATE,
        AUTHENTICATE_SURFACE,
        GET_DISPLAY_CONFIGS,
        GET_ACTIVE_CONFIG,
        SET_ACTIVE_CONFIG,
        CONNECT_DISPLAY,
        CAPTURE_SCREEN,
        CLEAR_ANIMATION_FRAME_STATS,
        GET_ANIMATION_FRAME_STATS,
        SET_POWER_MODE,
        GET_DISPLAY_STATS,
    };
    virtual status_t onTransact(uint32_t code, const Parcel& data,
            Parcel* reply, uint32_t flags = 0);
};
};
#endif
