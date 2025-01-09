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
void ASurfaceControl_acquire(
        ASurfaceControl* _Nonnull surface_control)
        void ASurfaceControl_release(
                ASurfaceControl* _Nonnull surface_control) struct ASurfaceTransaction;
typedef struct ASurfaceTransaction ASurfaceTransaction;
void ASurfaceTransaction_delete(
        ASurfaceTransaction* _Nullable transaction)
        void ASurfaceTransaction_apply(
                ASurfaceTransaction* _Nonnull transaction)
        typedef struct ASurfaceTransactionStats ASurfaceTransactionStats;
typedef void (*ASurfaceTransaction_OnComplete)(void* _Null_unspecified context,
                                               ASurfaceTransactionStats* _Nonnull stats)
        typedef void (*ASurfaceTransaction_OnCommit)(void* _Null_unspecified context,
                                                     ASurfaceTransactionStats* _Nonnull stats)
        int64_t ASurfaceTransactionStats_getLatchTime(
                ASurfaceTransactionStats* _Nonnull surface_transaction_stats)
        int ASurfaceTransactionStats_getPresentFenceFd(
                ASurfaceTransactionStats* _Nonnull surface_transaction_stats)
        void ASurfaceTransactionStats_getASurfaceControls(
                ASurfaceTransactionStats* _Nonnull surface_transaction_stats,
                ASurfaceControl* _Nullable* _Nullable* _Nonnull outASurfaceControls,
                size_t* _Nonnull outASurfaceControlsSize)
        void ASurfaceTransactionStats_releaseASurfaceControls(
                ASurfaceControl* _Nonnull* _Nonnull surface_controls)
        int64_t ASurfaceTransactionStats_getAcquireTime(
                ASurfaceTransactionStats* _Nonnull surface_transaction_stats,
                ASurfaceControl* _Nonnull surface_control)
        int ASurfaceTransactionStats_getPreviousReleaseFenceFd(
                ASurfaceTransactionStats* _Nonnull surface_transaction_stats,
                ASurfaceControl* _Nonnull surface_control)
        void ASurfaceTransaction_setOnComplete(ASurfaceTransaction* _Nonnull transaction,
                                               void* _Null_unspecified context,
                                               ASurfaceTransaction_OnComplete _Nonnull func)
        void ASurfaceTransaction_setOnCommit(ASurfaceTransaction* _Nonnull transaction,
                                             void* _Null_unspecified context,
                                             ASurfaceTransaction_OnCommit _Nonnull func)
        void ASurfaceTransaction_reparent(
                ASurfaceTransaction* _Nonnull transaction,
                ASurfaceControl* _Nonnull surface_control,
                ASurfaceControl* _Nullable new_parent)
        enum ASurfaceTransactionVisibility : int8_t {
            ASURFACE_TRANSACTION_VISIBILITY_HIDE = 0,
            ASURFACE_TRANSACTION_VISIBILITY_SHOW = 1,
        };
void ASurfaceTransaction_setVisibility(ASurfaceTransaction* _Nonnull transaction,
                                       ASurfaceControl* _Nonnull surface_control,
                                       enum ASurfaceTransactionVisibility visibility)
        void ASurfaceTransaction_setZOrder(ASurfaceTransaction* _Nonnull transaction,
                                           ASurfaceControl* _Nonnull surface_control,
                                           int32_t z_order)
        void ASurfaceTransaction_setBuffer(ASurfaceTransaction* _Nonnull transaction,
                                           ASurfaceControl* _Nonnull surface_control,
                                           AHardwareBuffer* _Nonnull buffer, int acquire_fence_fd)
        void ASurfaceTransaction_setColor(ASurfaceTransaction* _Nonnull transaction,
                                          ASurfaceControl* _Nonnull surface_control, float r,
                                          float g, float b, float alpha, enum ADataSpace dataspace)
        void ASurfaceTransaction_setGeometry(ASurfaceTransaction* _Nonnull transaction,
                                             ASurfaceControl* _Nonnull surface_control,
                                             const ARect& source, const ARect& destination,
                                             int32_t transform)
        void ASurfaceTransaction_setCrop(ASurfaceTransaction* _Nonnull transaction,
                                         ASurfaceControl* _Nonnull surface_control,
                                         const ARect& crop)
        void ASurfaceTransaction_setPosition(
                ASurfaceTransaction* _Nonnull transaction,
                ASurfaceControl* _Nonnull surface_control, int32_t x,
                int32_t y)
        void ASurfaceTransaction_setBufferTransform(
                ASurfaceTransaction* _Nonnull transaction,
                ASurfaceControl* _Nonnull surface_control,
                int32_t transform)
        void ASurfaceTransaction_setScale(
                ASurfaceTransaction* _Nonnull transaction,
                ASurfaceControl* _Nonnull surface_control, float xScale,
                float yScale)
        enum ASurfaceTransactionTransparency : int8_t {
            ASURFACE_TRANSACTION_TRANSPARENCY_TRANSPARENT = 0,
            ASURFACE_TRANSACTION_TRANSPARENCY_TRANSLUCENT = 1,
            ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE = 2,
        };
void ASurfaceTransaction_setBufferTransparency(ASurfaceTransaction* _Nonnull transaction,
                                               ASurfaceControl* _Nonnull surface_control,
                                               enum ASurfaceTransactionTransparency transparency)
        void ASurfaceTransaction_setDamageRegion(ASurfaceTransaction* _Nonnull transaction,
                                                 ASurfaceControl* _Nonnull surface_control,
                                                 const ARect* _Nullable rects, uint32_t count)
        void ASurfaceTransaction_setDesiredPresentTime(
                ASurfaceTransaction* _Nonnull transaction,
                int64_t desiredPresentTime)
        void ASurfaceTransaction_setBufferAlpha(ASurfaceTransaction* _Nonnull transaction,
                                                ASurfaceControl* _Nonnull surface_control,
                                                float alpha)
        void ASurfaceTransaction_setBufferDataSpace(
                ASurfaceTransaction* _Nonnull transaction,
                ASurfaceControl* _Nonnull surface_control,
                enum ADataSpace
                        data_space)
        void ASurfaceTransaction_setHdrMetadata_smpte2086(
                ASurfaceTransaction* _Nonnull transaction,
                ASurfaceControl* _Nonnull surface_control,
                struct AHdrMetadata_smpte2086* _Nullable metadata)
        void ASurfaceTransaction_setHdrMetadata_cta861_3(
                ASurfaceTransaction* _Nonnull transaction,
                ASurfaceControl* _Nonnull surface_control,
                struct AHdrMetadata_cta861_3* _Nullable metadata)
        void ASurfaceTransaction_setExtendedRangeBrightness(
                ASurfaceTransaction* _Nonnull transaction,
                ASurfaceControl* _Nonnull surface_control, float currentBufferRatio,
                float desiredRatio)
        void ASurfaceTransaction_setDesiredHdrHeadroom(ASurfaceTransaction* _Nonnull transaction,
                                                       ASurfaceControl* _Nonnull surface_control,
                                                       float desiredHeadroom)
        void ASurfaceTransaction_setFrameRate(
                ASurfaceTransaction* _Nonnull transaction,
                ASurfaceControl* _Nonnull surface_control, float frameRate,
                int8_t compatibility)
        void ASurfaceTransaction_setFrameRateWithChangeStrategy(
                ASurfaceTransaction* _Nonnull transaction,
                ASurfaceControl* _Nonnull surface_control, float frameRate, int8_t compatibility,
                int8_t changeFrameRateStrategy)
        void ASurfaceTransaction_clearFrameRate(ASurfaceTransaction* _Nonnull transaction,
                                                ASurfaceControl* _Nonnull surface_control)
        void ASurfaceTransaction_setEnableBackPressure(
                ASurfaceTransaction* _Nonnull transaction,
                ASurfaceControl* _Nonnull surface_control,
                bool enableBackPressure)
        void ASurfaceTransaction_setFrameTimeline(
                ASurfaceTransaction* _Nonnull transaction,
                AVsyncId vsyncId) #endif
