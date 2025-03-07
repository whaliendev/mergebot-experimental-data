/*
 * Copyright 2020 The Android Open Source Project
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

#include "AutoBackendTexture.h"

#undef LOG_TAG
#define LOG_TAG "RenderEngine"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <include/core/SkImage.h>
#include <include/core/SkSurface.h>

#include "compat/SkiaBackendTexture.h"

#include <log/log_main.h>
#include <utils/Trace.h>

namespace android {
namespace renderengine {
namespace skia {

<<<<<<< HEAD
AutoBackendTexture::AutoBackendTexture(std::unique_ptr<SkiaBackendTexture> backendTexture,
                                       CleanupManager& cleanupMgr)
      : mCleanupMgr(cleanupMgr), mBackendTexture(std::move(backendTexture)) {}
||||||| 9d37e2ab0a
AutoBackendTexture::AutoBackendTexture(GrDirectContext* context, AHardwareBuffer* buffer,
                                       bool isOutputBuffer, CleanupManager& cleanupMgr)
      : mCleanupMgr(cleanupMgr), mIsOutputBuffer(isOutputBuffer) {
    ATRACE_CALL();
    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(buffer, &desc);
    bool createProtectedImage = 0 != (desc.usage & AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT);
    GrBackendFormat backendFormat;

    GrBackendApi backend = context->backend();
    if (backend == GrBackendApi::kOpenGL) {
        backendFormat =
                GrAHardwareBufferUtils::GetGLBackendFormat(context, desc.format, false);
        mBackendTexture =
                GrAHardwareBufferUtils::MakeGLBackendTexture(context,
                                                             buffer,
                                                             desc.width,
                                                             desc.height,
                                                             &mDeleteProc,
                                                             &mUpdateProc,
                                                             &mImageCtx,
                                                             createProtectedImage,
                                                             backendFormat,
                                                             isOutputBuffer);
    } else if (backend == GrBackendApi::kVulkan) {
        backendFormat =
                GrAHardwareBufferUtils::GetVulkanBackendFormat(context,
                                                               buffer,
                                                               desc.format,
                                                               false);
        mBackendTexture =
                GrAHardwareBufferUtils::MakeVulkanBackendTexture(context,
                                                                 buffer,
                                                                 desc.width,
                                                                 desc.height,
                                                                 &mDeleteProc,
                                                                 &mUpdateProc,
                                                                 &mImageCtx,
                                                                 createProtectedImage,
                                                                 backendFormat,
                                                                 isOutputBuffer);
    } else {
        LOG_ALWAYS_FATAL("Unexpected backend %d", backend);
    }

    mColorType = GrAHardwareBufferUtils::GetSkColorTypeFromBufferFormat(desc.format);
    if (!mBackendTexture.isValid() || !desc.width || !desc.height) {
        LOG_ALWAYS_FATAL("Failed to create a valid texture. [%p]:[%d,%d] isProtected:%d "
                         "isWriteable:%d format:%d",
                         this, desc.width, desc.height, createProtectedImage, isOutputBuffer,
                         desc.format);
    }
}

AutoBackendTexture::~AutoBackendTexture() {
    if (mBackendTexture.isValid()) {
        mDeleteProc(mImageCtx);
        mBackendTexture = {};
    }
}
=======
AutoBackendTexture::AutoBackendTexture(GrDirectContext* context, AHardwareBuffer* buffer,
                                       bool isOutputBuffer, CleanupManager& cleanupMgr)
      : mCleanupMgr(cleanupMgr), mIsOutputBuffer(isOutputBuffer) {
    ATRACE_CALL();
    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(buffer, &desc);
    bool createProtectedImage = 0 != (desc.usage & AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT);
    GrBackendFormat backendFormat;

    GrBackendApi backend = context->backend();
    if (backend == GrBackendApi::kOpenGL) {
        backendFormat =
                GrAHardwareBufferUtils::GetGLBackendFormat(context, desc.format, false);
        mBackendTexture =
                GrAHardwareBufferUtils::MakeGLBackendTexture(context,
                                                             buffer,
                                                             desc.width,
                                                             desc.height,
                                                             &mDeleteProc,
                                                             &mUpdateProc,
                                                             &mImageCtx,
                                                             createProtectedImage,
                                                             backendFormat,
                                                             isOutputBuffer);
    } else if (backend == GrBackendApi::kVulkan) {
        backendFormat =
                GrAHardwareBufferUtils::GetVulkanBackendFormat(context,
                                                               buffer,
                                                               desc.format,
                                                               false);
        mBackendTexture =
                GrAHardwareBufferUtils::MakeVulkanBackendTexture(context,
                                                                 buffer,
                                                                 desc.width,
                                                                 desc.height,
                                                                 &mDeleteProc,
                                                                 &mUpdateProc,
                                                                 &mImageCtx,
                                                                 createProtectedImage,
                                                                 backendFormat,
                                                                 isOutputBuffer);
    } else {
        LOG_ALWAYS_FATAL("Unexpected backend %u", static_cast<unsigned>(backend));
    }

    mColorType = GrAHardwareBufferUtils::GetSkColorTypeFromBufferFormat(desc.format);
    if (!mBackendTexture.isValid() || !desc.width || !desc.height) {
        LOG_ALWAYS_FATAL("Failed to create a valid texture. [%p]:[%d,%d] isProtected:%d "
                         "isWriteable:%d format:%d",
                         this, desc.width, desc.height, createProtectedImage, isOutputBuffer,
                         desc.format);
    }
}

AutoBackendTexture::~AutoBackendTexture() {
    if (mBackendTexture.isValid()) {
        mDeleteProc(mImageCtx);
        mBackendTexture = {};
    }
}
>>>>>>> 5ff8b6e5

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

// releaseSurfaceProc is invoked by SkSurface, when the texture is no longer in use.
// "releaseContext" contains an "AutoBackendTexture*".
void AutoBackendTexture::releaseSurfaceProc(SkSurface::ReleaseContext releaseContext) {
    AutoBackendTexture* textureRelease = reinterpret_cast<AutoBackendTexture*>(releaseContext);
    textureRelease->unref(false);
}

// releaseImageProc is invoked by SkImage, when the texture is no longer in use.
// "releaseContext" contains an "AutoBackendTexture*".
void AutoBackendTexture::releaseImageProc(SkImages::ReleaseContext releaseContext) {
    AutoBackendTexture* textureRelease = reinterpret_cast<AutoBackendTexture*>(releaseContext);
    textureRelease->unref(false);
}

<<<<<<< HEAD
sk_sp<SkImage> AutoBackendTexture::makeImage(ui::Dataspace dataspace, SkAlphaType alphaType) {
||||||| 9d37e2ab0a
void logFatalTexture(const char* msg, const GrBackendTexture& tex, ui::Dataspace dataspace,
                     SkColorType colorType) {
    switch (tex.backend()) {
        case GrBackendApi::kOpenGL: {
            GrGLTextureInfo textureInfo;
            bool retrievedTextureInfo = GrBackendTextures::GetGLTextureInfo(tex, &textureInfo);
            LOG_ALWAYS_FATAL("%s isTextureValid:%d dataspace:%d"
                             "\n\tGrBackendTexture: (%i x %i) hasMipmaps: %i isProtected: %i "
                             "texType: %i\n\t\tGrGLTextureInfo: success: %i fTarget: %u fFormat: %u"
                             " colorType %i",
                             msg, tex.isValid(), static_cast<int32_t>(dataspace), tex.width(),
                             tex.height(), tex.hasMipmaps(), tex.isProtected(),
                             static_cast<int>(tex.textureType()), retrievedTextureInfo,
                             textureInfo.fTarget, textureInfo.fFormat, colorType);
            break;
        }
        case GrBackendApi::kVulkan: {
            GrVkImageInfo imageInfo;
            bool retrievedImageInfo = GrBackendTextures::GetVkImageInfo(tex, &imageInfo);
            LOG_ALWAYS_FATAL("%s isTextureValid:%d dataspace:%d"
                             "\n\tGrBackendTexture: (%i x %i) hasMipmaps: %i isProtected: %i "
                             "texType: %i\n\t\tVkImageInfo: success: %i fFormat: %i "
                             "fSampleCount: %u fLevelCount: %u colorType %i",
                             msg, tex.isValid(), dataspace, tex.width(), tex.height(),
                             tex.hasMipmaps(), tex.isProtected(),
                             static_cast<int>(tex.textureType()), retrievedImageInfo,
                             imageInfo.fFormat, imageInfo.fSampleCount, imageInfo.fLevelCount,
                             colorType);
            break;
        }
        default:
            LOG_ALWAYS_FATAL("%s Unexpected backend %u", msg, static_cast<unsigned>(tex.backend()));
            break;
    }
}

sk_sp<SkImage> AutoBackendTexture::makeImage(ui::Dataspace dataspace, SkAlphaType alphaType,
                                             GrDirectContext* context) {
=======
void logFatalTexture(const char* msg, const GrBackendTexture& tex, ui::Dataspace dataspace,
                     SkColorType colorType) {
    switch (tex.backend()) {
        case GrBackendApi::kOpenGL: {
            GrGLTextureInfo textureInfo;
            bool retrievedTextureInfo = GrBackendTextures::GetGLTextureInfo(tex, &textureInfo);
            LOG_ALWAYS_FATAL("%s isTextureValid:%d dataspace:%d"
                             "\n\tGrBackendTexture: (%i x %i) hasMipmaps: %i isProtected: %i "
                             "texType: %i\n\t\tGrGLTextureInfo: success: %i fTarget: %u fFormat: %u"
                             " colorType %i",
                             msg, tex.isValid(), static_cast<int32_t>(dataspace), tex.width(),
                             tex.height(), tex.hasMipmaps(), tex.isProtected(),
                             static_cast<int>(tex.textureType()), retrievedTextureInfo,
                             textureInfo.fTarget, textureInfo.fFormat, colorType);
            break;
        }
        case GrBackendApi::kVulkan: {
            GrVkImageInfo imageInfo;
            bool retrievedImageInfo = GrBackendTextures::GetVkImageInfo(tex, &imageInfo);
            LOG_ALWAYS_FATAL("%s isTextureValid:%d dataspace:%d"
                             "\n\tGrBackendTexture: (%i x %i) hasMipmaps: %i isProtected: %i "
                             "texType: %i\n\t\tVkImageInfo: success: %i fFormat: %i "
                             "fSampleCount: %u fLevelCount: %u colorType %i",
                             msg, tex.isValid(), static_cast<int32_t>(dataspace), tex.width(),
                             tex.height(), tex.hasMipmaps(), tex.isProtected(),
                             static_cast<int>(tex.textureType()), retrievedImageInfo,
                             imageInfo.fFormat, imageInfo.fSampleCount, imageInfo.fLevelCount,
                             colorType);
            break;
        }
        default:
            LOG_ALWAYS_FATAL("%s Unexpected backend %u", msg, static_cast<unsigned>(tex.backend()));
            break;
    }
}

sk_sp<SkImage> AutoBackendTexture::makeImage(ui::Dataspace dataspace, SkAlphaType alphaType,
                                             GrDirectContext* context) {
>>>>>>> 5ff8b6e5
    ATRACE_CALL();

    sk_sp<SkImage> image = mBackendTexture->makeImage(alphaType, dataspace, releaseImageProc, this);
    // The following ref will be counteracted by releaseProc, when SkImage is discarded.
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
        // The following ref will be counteracted by releaseProc, when SkSurface is discarded.
        ref();

        mSurface = surface;
    }

    mDataspace = dataspace;
    return mSurface;
}

} // namespace skia
} // namespace renderengine
} // namespace android
