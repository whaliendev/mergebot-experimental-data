#include "AutoBackendTexture.h"
#undef LOG_TAG
#define LOG_TAG "RenderEngine"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <include/core/SkImage.h>
#include <include/core/SkSurface.h>
#include "compat/SkiaBackendTexture.h"
#include <log/log_main.h>
#include <utils/Trace.h>
#include <SkImage.h>
#include <include/gpu/ganesh/SkImageGanesh.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <include/gpu/ganesh/gl/GrGLBackendSurface.h>
#include <include/gpu/ganesh/vk/GrVkBackendSurface.h>
#include <include/gpu/vk/GrVkTypes.h>
#include <android/hardware_buffer.h>
#include "ColorSpaces.h"
#include "log/log_main.h"
#include "utils/Trace.h"
namespace android {
namespace renderengine {
namespace skia {
AutoBackendTexture::AutoBackendTexture(std::unique_ptr<SkiaBackendTexture> backendTexture,
                                       CleanupManager& cleanupMgr)
      : mCleanupMgr(cleanupMgr), mBackendTexture(std::move(backendTexture)) {}
void AutoBackendTexture::unref(bool releaseLocalResources) {
    if (releaseLocalResources) {
        mSurface = nullptr;
        mImage = nullptr;
    }
    mUsageCount--;
    if (mUsageCount <= 0) {
        mCleanupMgr.add(this);
    }
}
void AutoBackendTexture::releaseSurfaceProc(SkSurface::ReleaseContext releaseContext) {
    AutoBackendTexture* textureRelease = reinterpret_cast<AutoBackendTexture*>(releaseContext);
    textureRelease->unref(false);
}
void AutoBackendTexture::releaseImageProc(SkImages::ReleaseContext releaseContext) {
    AutoBackendTexture* textureRelease = reinterpret_cast<AutoBackendTexture*>(releaseContext);
    textureRelease->unref(false);
}
sk_sp<SkImage> AutoBackendTexture::makeImage(ui::Dataspace dataspace, SkAlphaType alphaType) {
    ATRACE_CALL();
    sk_sp<SkImage> image = mBackendTexture->makeImage(alphaType, dataspace, releaseImageProc, this);
    ref();
    mImage = image;
    mDataspace = dataspace;
    return mImage;
}
sk_sp<SkSurface> AutoBackendTexture::getOrCreateSurface(ui::Dataspace dataspace) {
    ATRACE_CALL();
    LOG_ALWAYS_FATAL_IF(!mBackendTexture->isOutputBuffer(),
                        "You can't generate an SkSurface for a read-only texture");
    if (!mSurface.get() || mDataspace != dataspace) {
        sk_sp<SkSurface> surface =
                mBackendTexture->makeSurface(dataspace, releaseSurfaceProc, this);
        ref();
        mSurface = surface;
    }
    mDataspace = dataspace;
    return mSurface;
}
}
}
}
