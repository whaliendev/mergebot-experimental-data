#ifndef ANDROID_SURFACE_CONTROL_H
#define ANDROID_SURFACE_CONTROL_H 
#include <sys/cdefs.h>
#include <android/choreographer.h>
#include <android/data_space.h>
#include <android/hardware_buffer.h>
#include <android/hdr_metadata.h>
#include <android/native_window.h>
__BEGIN_DECLS
struct ASurfaceControl;
typedef struct ASurfaceControl ASurfaceControl;
ASurfaceControl* _Nullable ASurfaceControl_createFromWindow(ANativeWindow* _Nonnull parent,
                                                            const char* _Nonnull debug_name)
        __INTRODUCED_IN(29);
ASurfaceControl* _Nullable ASurfaceControl_create(ASurfaceControl* _Nonnull parent,
                                                  const char* _Nonnull debug_name)
        __INTRODUCED_IN(29);
void ASurfaceControl_acquire(ASurfaceControl* _Nonnull surface_control) __INTRODUCED_IN(31);
void ASurfaceControl_release(ASurfaceControl* _Nonnull surface_control) __INTRODUCED_IN(29);
struct ASurfaceTransaction;
typedef struct ASurfaceTransaction ASurfaceTransaction;
ASurfaceTransaction* _Nonnull ASurfaceTransaction_create() __INTRODUCED_IN(29);
void ASurfaceTransaction_delete(ASurfaceTransaction* _Nullable transaction) __INTRODUCED_IN(29);
void ASurfaceTransaction_apply(ASurfaceTransaction* _Nonnull transaction) __INTRODUCED_IN(29);
typedef struct ASurfaceTransactionStats ASurfaceTransactionStats;
typedef void (*ASurfaceTransaction_OnComplete)(void* _Null_unspecified context,
                                               ASurfaceTransactionStats* _Nonnull stats)
        __INTRODUCED_IN(29);
typedef void (*ASurfaceTransaction_OnCommit)(void* _Null_unspecified context,
                                             ASurfaceTransactionStats* _Nonnull stats)
        __INTRODUCED_IN(31);
int64_t ASurfaceTransactionStats_getLatchTime(
        ASurfaceTransactionStats* _Nonnull surface_transaction_stats) __INTRODUCED_IN(29);
int ASurfaceTransactionStats_getPresentFenceFd(
        ASurfaceTransactionStats* _Nonnull surface_transaction_stats) __INTRODUCED_IN(29);
void ASurfaceTransactionStats_getASurfaceControls(
        ASurfaceTransactionStats* _Nonnull surface_transaction_stats,
        ASurfaceControl* _Nullable* _Nullable* _Nonnull outASurfaceControls,
        size_t* _Nonnull outASurfaceControlsSize) __INTRODUCED_IN(29);
void ASurfaceTransactionStats_releaseASurfaceControls(
        ASurfaceControl* _Nonnull* _Nonnull surface_controls) __INTRODUCED_IN(29);
int64_t ASurfaceTransactionStats_getAcquireTime(
        ASurfaceTransactionStats* _Nonnull surface_transaction_stats,
        ASurfaceControl* _Nonnull surface_control) __INTRODUCED_IN(29);
int ASurfaceTransactionStats_getPreviousReleaseFenceFd(
        ASurfaceTransactionStats* _Nonnull surface_transaction_stats,
        ASurfaceControl* _Nonnull surface_control) __INTRODUCED_IN(29);
void ASurfaceTransaction_setOnComplete(ASurfaceTransaction* _Nonnull transaction,
                                       void* _Null_unspecified context,
                                       ASurfaceTransaction_OnComplete _Nonnull func)
        __INTRODUCED_IN(29);
void ASurfaceTransaction_setOnCommit(ASurfaceTransaction* _Nonnull transaction,
                                     void* _Null_unspecified context,
                                     ASurfaceTransaction_OnCommit _Nonnull func)
        __INTRODUCED_IN(31);
void ASurfaceTransaction_reparent(ASurfaceTransaction* _Nonnull transaction,
                                  ASurfaceControl* _Nonnull surface_control,
                                  ASurfaceControl* _Nullable new_parent) __INTRODUCED_IN(29);
enum ASurfaceTransactionVisibility : int8_t {
    ASURFACE_TRANSACTION_VISIBILITY_HIDE = 0,
    ASURFACE_TRANSACTION_VISIBILITY_SHOW = 1,
};
void ASurfaceTransaction_setVisibility(ASurfaceTransaction* _Nonnull transaction,
                                       ASurfaceControl* _Nonnull surface_control,
                                       enum ASurfaceTransactionVisibility visibility)
        __INTRODUCED_IN(29);
void ASurfaceTransaction_setZOrder(ASurfaceTransaction* _Nonnull transaction,
                                   ASurfaceControl* _Nonnull surface_control, int32_t z_order)
        __INTRODUCED_IN(29);
void ASurfaceTransaction_setBuffer(ASurfaceTransaction* _Nonnull transaction,
                                   ASurfaceControl* _Nonnull surface_control,
                                   AHardwareBuffer* _Nonnull buffer, int acquire_fence_fd)
        __INTRODUCED_IN(29);
void ASurfaceTransaction_setColor(ASurfaceTransaction* _Nonnull transaction,
                                  ASurfaceControl* _Nonnull surface_control, float r, float g,
                                  float b, float alpha, enum ADataSpace dataspace)
        __INTRODUCED_IN(29);
void ASurfaceTransaction_setGeometry(ASurfaceTransaction* _Nonnull transaction,
                                     ASurfaceControl* _Nonnull surface_control, const ARect& source,
                                     const ARect& destination, int32_t transform)
        __INTRODUCED_IN(29);
void ASurfaceTransaction_setCrop(ASurfaceTransaction* _Nonnull transaction,
                                 ASurfaceControl* _Nonnull surface_control, const ARect& crop)
        __INTRODUCED_IN(31);
void ASurfaceTransaction_setPosition(ASurfaceTransaction* _Nonnull transaction,
                                     ASurfaceControl* _Nonnull surface_control, int32_t x,
                                     int32_t y) __INTRODUCED_IN(31);
void ASurfaceTransaction_setBufferTransform(ASurfaceTransaction* _Nonnull transaction,
                                            ASurfaceControl* _Nonnull surface_control,
                                            int32_t transform) __INTRODUCED_IN(31);
void ASurfaceTransaction_setScale(ASurfaceTransaction* _Nonnull transaction,
                                  ASurfaceControl* _Nonnull surface_control, float xScale,
                                  float yScale) __INTRODUCED_IN(31);
enum ASurfaceTransactionTransparency : int8_t {
    ASURFACE_TRANSACTION_TRANSPARENCY_TRANSPARENT = 0,
    ASURFACE_TRANSACTION_TRANSPARENCY_TRANSLUCENT = 1,
    ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE = 2,
};
void ASurfaceTransaction_setBufferTransparency(ASurfaceTransaction* _Nonnull transaction,
                                               ASurfaceControl* _Nonnull surface_control,
                                               enum ASurfaceTransactionTransparency transparency)
                                               __INTRODUCED_IN(29);
void ASurfaceTransaction_setDamageRegion(ASurfaceTransaction* _Nonnull transaction,
                                         ASurfaceControl* _Nonnull surface_control,
                                         const ARect* _Nullable rects, uint32_t count)
                                         __INTRODUCED_IN(29);
void ASurfaceTransaction_setDesiredPresentTime(ASurfaceTransaction* _Nonnull transaction,
                                               int64_t desiredPresentTime) __INTRODUCED_IN(29);
void ASurfaceTransaction_setBufferAlpha(ASurfaceTransaction* _Nonnull transaction,
                                        ASurfaceControl* _Nonnull surface_control, float alpha)
                                        __INTRODUCED_IN(29);
void ASurfaceTransaction_setBufferDataSpace(ASurfaceTransaction* _Nonnull transaction,
                                            ASurfaceControl* _Nonnull surface_control,
                                            enum ADataSpace data_space) __INTRODUCED_IN(29);
void ASurfaceTransaction_setHdrMetadata_smpte2086(ASurfaceTransaction* _Nonnull transaction,
                                                  ASurfaceControl* _Nonnull surface_control,
                                                  struct AHdrMetadata_smpte2086* _Nullable metadata)
                                                  __INTRODUCED_IN(29);
void ASurfaceTransaction_setHdrMetadata_cta861_3(ASurfaceTransaction* _Nonnull transaction,
                                                 ASurfaceControl* _Nonnull surface_control,
                                                 struct AHdrMetadata_cta861_3* _Nullable metadata)
                                                 __INTRODUCED_IN(29);
void ASurfaceTransaction_setExtendedRangeBrightness(ASurfaceTransaction* _Nonnull transaction, ASurfaceControl* _Nonnull surface_control, float currentBufferRatio, float desiredRatio) __INTRODUCED_IN(__ANDROID_API_U__);
void ASurfaceTransaction_setFrameRate(ASurfaceTransaction* _Nonnull transaction,
                                      ASurfaceControl* _Nonnull surface_control, float frameRate,
                                      int8_t compatibility) __INTRODUCED_IN(30);
void ASurfaceTransaction_setFrameRateWithChangeStrategy(ASurfaceTransaction* _Nonnull transaction,
                                                        ASurfaceControl* _Nonnull surface_control,
                                                        float frameRate, int8_t compatibility,
                                                        int8_t changeFrameRateStrategy)
                                                        __INTRODUCED_IN(31);
void ASurfaceTransaction_clearFrameRate(ASurfaceTransaction* _Nonnull transaction,
                                        ASurfaceControl* _Nonnull surface_control)
                                        __INTRODUCED_IN(__ANDROID_API_U__);
void ASurfaceTransaction_setEnableBackPressure(ASurfaceTransaction* _Nonnull transaction,
                                               ASurfaceControl* _Nonnull surface_control,
                                               bool enableBackPressure) __INTRODUCED_IN(31);
void ASurfaceTransaction_setFrameTimeline(ASurfaceTransaction* _Nonnull transaction,
                                          AVsyncId vsyncId) __INTRODUCED_IN(33);
__END_DECLS
#endif
