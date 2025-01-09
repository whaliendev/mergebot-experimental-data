#undef LOG_TAG
#define LOG_TAG "RenderEngine"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include "SkiaGLRenderEngine.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GrContextOptions.h>
#include <android-base/stringprintf.h>
#include <gl/GrGLInterface.h>
#include <gui/TraceUtils.h>
#include <sync/sync.h>
#include <ui/DebugUtils.h>
#include <utils/Trace.h>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numeric>
#include "../gl/GLExtensions.h"
#include "log/log_main.h"
bool checkGlError(const char* op, int lineNumber);
namespace android {
namespace renderengine {
namespace skia {
using base::StringAppendF;
static status_t selectConfigForAttribute(EGLDisplay dpy, EGLint const* attrs, EGLint attribute,
                                         EGLint wanted, EGLConfig* outConfig) {
    EGLint numConfigs = -1, n = 0;
    eglGetConfigs(dpy, nullptr, 0, &numConfigs);
    std::vector<EGLConfig> configs(numConfigs, EGL_NO_CONFIG_KHR);
    eglChooseConfig(dpy, attrs, configs.data(), configs.size(), &n);
    configs.resize(n);
    if (!configs.empty()) {
        if (attribute != EGL_NONE) {
            for (EGLConfig config : configs) {
                EGLint value = 0;
                eglGetConfigAttrib(dpy, config, attribute, &value);
                if (wanted == value) {
                    *outConfig = config;
                    return NO_ERROR;
                }
            }
        } else {
            *outConfig = configs[0];
            return NO_ERROR;
        }
    }
    return NAME_NOT_FOUND;
}
static status_t selectEGLConfig(EGLDisplay display, EGLint format, EGLint renderableType,
                                EGLConfig* config) {
    status_t err;
    EGLint wantedAttribute;
    EGLint wantedAttributeValue;
    std::vector<EGLint> attribs;
    if (renderableType) {
        const ui::PixelFormat pixelFormat = static_cast<ui::PixelFormat>(format);
        const bool is1010102 = pixelFormat == ui::PixelFormat::RGBA_1010102;
        const EGLint tmpAttribs[] = {
                EGL_RENDERABLE_TYPE,
                renderableType,
                EGL_RECORDABLE_ANDROID,
                EGL_TRUE,
                EGL_SURFACE_TYPE,
                EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                EGL_FRAMEBUFFER_TARGET_ANDROID,
                EGL_TRUE,
                EGL_RED_SIZE,
                is1010102 ? 10 : 8,
                EGL_GREEN_SIZE,
                is1010102 ? 10 : 8,
                EGL_BLUE_SIZE,
                is1010102 ? 10 : 8,
                EGL_ALPHA_SIZE,
                is1010102 ? 2 : 8,
                EGL_NONE,
        };
        std::copy(tmpAttribs, tmpAttribs + (sizeof(tmpAttribs) / sizeof(EGLint)),
                  std::back_inserter(attribs));
        wantedAttribute = EGL_NONE;
        wantedAttributeValue = EGL_NONE;
    } else {
        wantedAttribute = EGL_NATIVE_VISUAL_ID;
        wantedAttributeValue = format;
    }
    err = selectConfigForAttribute(display, attribs.data(), wantedAttribute, wantedAttributeValue,
                                   config);
    if (err == NO_ERROR) {
        EGLint caveat;
        if (eglGetConfigAttrib(display, *config, EGL_CONFIG_CAVEAT, &caveat))
            ALOGW_IF(caveat == EGL_SLOW_CONFIG, "EGL_SLOW_CONFIG selected!");
    }
    return err;
}
std::unique_ptr<SkiaGLRenderEngine> SkiaGLRenderEngine::create(
        const RenderEngineCreationArgs& args) {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(display, nullptr, nullptr)) {
        LOG_ALWAYS_FATAL("failed to initialize EGL");
    }
    const auto eglVersion = eglQueryString(display, EGL_VERSION);
    if (!eglVersion) {
        checkGlError(__FUNCTION__, __LINE__);
        LOG_ALWAYS_FATAL("eglQueryString(EGL_VERSION) failed");
    }
    const auto eglExtensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!eglExtensions) {
        checkGlError(__FUNCTION__, __LINE__);
        LOG_ALWAYS_FATAL("eglQueryString(EGL_EXTENSIONS) failed");
    }
    auto& extensions = gl::GLExtensions::getInstance();
    extensions.initWithEGLStrings(eglVersion, eglExtensions);
    EGLConfig config = EGL_NO_CONFIG_KHR;
    if (!extensions.hasNoConfigContext()) {
        config = chooseEglConfig(display, args.pixelFormat, true);
    }
    EGLContext protectedContext = EGL_NO_CONTEXT;
    const std::optional<RenderEngine::ContextPriority> priority = createContextPriority(args);
    if (args.enableProtectedContext && extensions.hasProtectedContent()) {
        protectedContext =
                createEglContext(display, config, nullptr, priority, Protection::PROTECTED);
        ALOGE_IF(protectedContext == EGL_NO_CONTEXT, "Can't create protected context");
    }
    EGLContext ctxt =
            createEglContext(display, config, protectedContext, priority, Protection::UNPROTECTED);
    LOG_ALWAYS_FATAL_IF(ctxt == EGL_NO_CONTEXT, "EGLContext creation failed");
    EGLSurface placeholder = EGL_NO_SURFACE;
    if (!extensions.hasSurfacelessContext()) {
        placeholder = createPlaceholderEglPbufferSurface(display, config, args.pixelFormat,
                                                         Protection::UNPROTECTED);
        LOG_ALWAYS_FATAL_IF(placeholder == EGL_NO_SURFACE, "can't create placeholder pbuffer");
    }
    EGLBoolean success = eglMakeCurrent(display, placeholder, placeholder, ctxt);
    LOG_ALWAYS_FATAL_IF(!success, "can't make placeholder pbuffer current");
    extensions.initWithGLStrings(glGetString(GL_VENDOR), glGetString(GL_RENDERER),
                                 glGetString(GL_VERSION), glGetString(GL_EXTENSIONS));
    EGLSurface protectedPlaceholder = EGL_NO_SURFACE;
    if (protectedContext != EGL_NO_CONTEXT && !extensions.hasSurfacelessContext()) {
        protectedPlaceholder = createPlaceholderEglPbufferSurface(display, config, args.pixelFormat,
                                                                  Protection::PROTECTED);
        ALOGE_IF(protectedPlaceholder == EGL_NO_SURFACE,
                 "can't create protected placeholder pbuffer");
    }
    std::unique_ptr<SkiaGLRenderEngine> engine =
            std::make_unique<SkiaGLRenderEngine>(args, display, ctxt, placeholder, protectedContext,
                                                 protectedPlaceholder);
    engine->ensureGrContextsCreated();
    ALOGI("OpenGL ES informations:");
    ALOGI("vendor    : %s", extensions.getVendor());
    ALOGI("renderer  : %s", extensions.getRenderer());
    ALOGI("version   : %s", extensions.getVersion());
    ALOGI("extensions: %s", extensions.getExtensions());
    ALOGI("GL_MAX_TEXTURE_SIZE = %zu", engine->getMaxTextureSize());
    ALOGI("GL_MAX_VIEWPORT_DIMS = %zu", engine->getMaxViewportDims());
    return engine;
}
EGLConfig SkiaGLRenderEngine::chooseEglConfig(EGLDisplay display, int format, bool logConfig) {
    status_t err;
    EGLConfig config;
    err = selectEGLConfig(display, format, EGL_OPENGL_ES3_BIT, &config);
    if (err != NO_ERROR) {
        err = selectEGLConfig(display, format, EGL_OPENGL_ES2_BIT, &config);
        if (err != NO_ERROR) {
            ALOGW("no suitable EGLConfig found, trying a simpler query");
            err = selectEGLConfig(display, format, 0, &config);
            if (err != NO_ERROR) {
                LOG_ALWAYS_FATAL("no suitable EGLConfig found, giving up");
            }
        }
    }
    if (logConfig) {
        EGLint r, g, b, a;
        eglGetConfigAttrib(display, config, EGL_RED_SIZE, &r);
        eglGetConfigAttrib(display, config, EGL_GREEN_SIZE, &g);
        eglGetConfigAttrib(display, config, EGL_BLUE_SIZE, &b);
        eglGetConfigAttrib(display, config, EGL_ALPHA_SIZE, &a);
        ALOGI("EGL information:");
        ALOGI("vendor    : %s", eglQueryString(display, EGL_VENDOR));
        ALOGI("version   : %s", eglQueryString(display, EGL_VERSION));
        ALOGI("extensions: %s", eglQueryString(display, EGL_EXTENSIONS));
        ALOGI("Client API: %s", eglQueryString(display, EGL_CLIENT_APIS) ?: "Not Supported");
        ALOGI("EGLSurface: %d-%d-%d-%d, config=%p", r, g, b, a, config);
    }
    return config;
}
SkiaGLRenderEngine::SkiaGLRenderEngine(const RenderEngineCreationArgs& args, EGLDisplay display,
                                       EGLContext ctxt, EGLSurface placeholder,
                                       EGLContext protectedContext, EGLSurface protectedPlaceholder)
      : SkiaRenderEngine(args.renderEngineType,
                         static_cast<PixelFormat>(args.pixelFormat),
                         args.useColorManagement, args.supportsBackgroundBlur),
        mEGLDisplay(display),
        mEGLContext(ctxt),
        mPlaceholderSurface(placeholder),
        mProtectedEGLContext(protectedContext),
        mProtectedPlaceholderSurface(protectedPlaceholder) { }
SkiaGLRenderEngine::~SkiaGLRenderEngine() {
    finishRenderingAndAbandonContext();
    if (mPlaceholderSurface != EGL_NO_SURFACE) {
        eglDestroySurface(mEGLDisplay, mPlaceholderSurface);
    }
    if (mProtectedPlaceholderSurface != EGL_NO_SURFACE) {
        eglDestroySurface(mEGLDisplay, mProtectedPlaceholderSurface);
    }
    if (mEGLContext != EGL_NO_CONTEXT) {
        eglDestroyContext(mEGLDisplay, mEGLContext);
    }
    if (mProtectedEGLContext != EGL_NO_CONTEXT) {
        eglDestroyContext(mEGLDisplay, mProtectedEGLContext);
    }
    eglMakeCurrent(mEGLDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(mEGLDisplay);
    eglReleaseThread();
}
SkiaRenderEngine::Contexts SkiaGLRenderEngine::createDirectContexts(
    const GrContextOptions& options) {
    LOG_ALWAYS_FATAL_IF(isProtected(),
                        "Cannot setup contexts while already in protected mode");
    sk_sp<const GrGLInterface> glInterface = GrGLMakeNativeInterface();
    LOG_ALWAYS_FATAL_IF(!glInterface.get(), "GrGLMakeNativeInterface() failed");
    SkiaRenderEngine::Contexts contexts;
    contexts.first = GrDirectContext::MakeGL(glInterface, options);
    if (supportsProtectedContentImpl()) {
        useProtectedContextImpl(GrProtected::kYes);
        contexts.second = GrDirectContext::MakeGL(glInterface, options);
        useProtectedContextImpl(GrProtected::kNo);
    }
    return contexts;
}
bool SkiaGLRenderEngine::supportsProtectedContentImpl() const {
    return mProtectedEGLContext != EGL_NO_CONTEXT;
}
bool SkiaGLRenderEngine::useProtectedContextImpl(GrProtected isProtected) {
    const EGLSurface surface =
        (isProtected == GrProtected::kYes) ?
        mProtectedPlaceholderSurface : mPlaceholderSurface;
    const EGLContext context = (isProtected == GrProtected::kYes) ?
        mProtectedEGLContext : mEGLContext;
    return eglMakeCurrent(mEGLDisplay, surface, surface, context) == EGL_TRUE;
}
void SkiaGLRenderEngine::waitFence(GrDirectContext*, base::borrowed_fd fenceFd) {
    if (fenceFd.get() >= 0 && !waitGpuFence(fenceFd)) {
        ATRACE_NAME("SkiaGLRenderEngine::waitFence");
        sync_wait(fenceFd.get(), -1);
    }
}
base::unique_fd SkiaGLRenderEngine::flushAndSubmit(GrDirectContext* grContext) {
    base::unique_fd drawFence = flush();
    bool requireSync = drawFence.get() < 0;
    if (requireSync) {
        ATRACE_BEGIN("Submit(sync=true)");
    } else {
        ATRACE_BEGIN("Submit(sync=false)");
    }
    bool success = grContext->submit(requireSync);
    ATRACE_END();
    if (!success) {
        ALOGE("Failed to flush RenderEngine commands");
        return drawFence;
    }
    return drawFence;
}
bool SkiaGLRenderEngine::waitGpuFence(base::borrowed_fd fenceFd) {
    if (!gl::GLExtensions::getInstance().hasNativeFenceSync() ||
        !gl::GLExtensions::getInstance().hasWaitSync()) {
        return false;
    }
    base::unique_fd fenceDup(dup(fenceFd.get()));
    if (fenceDup.get() < 0) {
        ALOGE("failed to create duplicate fence fd: %d", fenceDup.get());
        return false;
    }
    EGLint attribs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fenceDup.release(), EGL_NONE};
    EGLSyncKHR sync = eglCreateSyncKHR(mEGLDisplay, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
    if (sync == EGL_NO_SYNC_KHR) {
        ALOGE("failed to create EGL native fence sync: %#x", eglGetError());
        return false;
    }
    eglWaitSyncKHR(mEGLDisplay, sync, 0);
    EGLint error = eglGetError();
    eglDestroySyncKHR(mEGLDisplay, sync);
    if (error != EGL_SUCCESS) {
        ALOGE("failed to wait for EGL native fence sync: %#x", error);
        return false;
    }
    return true;
}
base::unique_fd SkiaGLRenderEngine::flush() {
    ATRACE_CALL();
    if (!gl::GLExtensions::getInstance().hasNativeFenceSync()) {
        return base::unique_fd();
    }
    EGLSyncKHR sync = eglCreateSyncKHR(mEGLDisplay, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    if (sync == EGL_NO_SYNC_KHR) {
        ALOGW("failed to create EGL native fence sync: %#x", eglGetError());
        return base::unique_fd();
    }
    glFlush();
    base::unique_fd fenceFd(eglDupNativeFenceFDANDROID(mEGLDisplay, sync));
    eglDestroySyncKHR(mEGLDisplay, sync);
    if (fenceFd == EGL_NO_NATIVE_FENCE_FD_ANDROID) {
        ALOGW("failed to dup EGL native fence sync: %#x", eglGetError());
    }
<<<<<<< HEAD
    return fenceFd;
||||||| b16c01a3e6
private:
    SkCanvas* mCanvas;
    int mSaveCount;
};
static SkRRect getBlurRRect(const BlurRegion& region) {
    const auto rect = SkRect::MakeLTRB(region.left, region.top, region.right, region.bottom);
    const SkVector radii[4] = {SkVector::Make(region.cornerRadiusTL, region.cornerRadiusTL),
                               SkVector::Make(region.cornerRadiusTR, region.cornerRadiusTR),
                               SkVector::Make(region.cornerRadiusBR, region.cornerRadiusBR),
                               SkVector::Make(region.cornerRadiusBL, region.cornerRadiusBL)};
    SkRRect roundedRect;
    roundedRect.setRectRadii(rect, radii);
    return roundedRect;
}
constexpr float kDefaultMargin = 0.0001f;
static bool equalsWithinMargin(float expected, float value, float margin = kDefaultMargin) {
    LOG_ALWAYS_FATAL_IF(margin < 0.f, "Margin is negative!");
    return std::abs(expected - value) < margin;
}
namespace {
template <typename T>
void logSettings(const T& t) {
    std::stringstream stream;
    PrintTo(t, &stream);
    auto string = stream.str();
    size_t pos = 0;
    const size_t size = string.size();
    while (pos < size) {
        const size_t end = std::min(string.find("\n", pos), size);
        ALOGD("%s", string.substr(pos, end - pos).c_str());
        pos = end + 1;
    }
}
}
void SkiaGLRenderEngine::drawLayersInternal(
        const std::shared_ptr<std::promise<RenderEngineResult>>&& resultPromise,
        const DisplaySettings& display, const std::vector<LayerSettings>& layers,
        const std::shared_ptr<ExternalTexture>& buffer, const bool ,
        base::unique_fd&& bufferFence) {
    ATRACE_NAME("SkiaGL::drawLayers");
    std::lock_guard<std::mutex> lock(mRenderingMutex);
    if (layers.empty()) {
        ALOGV("Drawing empty layer stack");
        resultPromise->set_value({NO_ERROR, base::unique_fd()});
        return;
    }
    if (buffer == nullptr) {
        ALOGE("No output buffer provided. Aborting GPU composition.");
        resultPromise->set_value({BAD_VALUE, base::unique_fd()});
        return;
    }
    validateOutputBufferUsage(buffer->getBuffer());
    auto grContext = getActiveGrContext();
    auto& cache = mTextureCache;
    DeferTextureCleanup dtc(mTextureCleanupMgr);
    std::shared_ptr<AutoBackendTexture::LocalRef> surfaceTextureRef;
    if (const auto& it = cache.find(buffer->getBuffer()->getId()); it != cache.end()) {
        surfaceTextureRef = it->second;
    } else {
        surfaceTextureRef =
                std::make_shared<AutoBackendTexture::LocalRef>(grContext,
                                                               buffer->getBuffer()
                                                                       ->toAHardwareBuffer(),
                                                               true, mTextureCleanupMgr);
    }
    waitFence(bufferFence);
    const ui::Dataspace dstDataspace =
            mUseColorManagement ? display.outputDataspace : ui::Dataspace::V0_SRGB_LINEAR;
    sk_sp<SkSurface> dstSurface = surfaceTextureRef->getOrCreateSurface(dstDataspace, grContext);
    SkCanvas* dstCanvas = mCapture->tryCapture(dstSurface.get());
    if (dstCanvas == nullptr) {
        ALOGE("Cannot acquire canvas from Skia.");
        resultPromise->set_value({BAD_VALUE, base::unique_fd()});
        return;
    }
    sk_sp<SkColorFilter> displayColorTransform;
    if (display.colorTransform != mat4() && !display.deviceHandlesColorTransform) {
        displayColorTransform = SkColorFilters::Matrix(toSkColorMatrix(display.colorTransform));
    }
    const bool ctModifiesAlpha =
            displayColorTransform && !displayColorTransform->isAlphaUnchanged();
    const float maxLayerWhitePoint = std::transform_reduce(
            layers.cbegin(), layers.cend(), 0.f,
            [](float left, float right) { return std::max(left, right); },
            [&](const auto& l) { return l.whitePointNits; });
    const float displayDimmingRatio = display.targetLuminanceNits > 0.f &&
                    maxLayerWhitePoint > 0.f && display.targetLuminanceNits > maxLayerWhitePoint
            ? maxLayerWhitePoint / display.targetLuminanceNits
            : 1.f;
    sk_sp<SkSurface> activeSurface(dstSurface);
    SkCanvas* canvas = dstCanvas;
    SkiaCapture::OffscreenState offscreenCaptureState;
    const LayerSettings* blurCompositionLayer = nullptr;
    if (mBlurFilter) {
        bool requiresCompositionLayer = false;
        for (const auto& layer : layers) {
            if (!layerHasBlur(layer, ctModifiesAlpha)) {
                continue;
            }
            if (layer.backgroundBlurRadius > 0 &&
                layer.backgroundBlurRadius < mBlurFilter->getMaxCrossFadeRadius()) {
                requiresCompositionLayer = true;
            }
            for (auto region : layer.blurRegions) {
                if (region.blurRadius < mBlurFilter->getMaxCrossFadeRadius()) {
                    requiresCompositionLayer = true;
                }
            }
            if (requiresCompositionLayer) {
                activeSurface = dstSurface->makeSurface(dstSurface->imageInfo());
                canvas = mCapture->tryOffscreenCapture(activeSurface.get(), &offscreenCaptureState);
                blurCompositionLayer = &layer;
                break;
            }
        }
    }
    AutoSaveRestore surfaceAutoSaveRestore(canvas);
    canvas->clear(SK_ColorTRANSPARENT);
    initCanvas(canvas, display);
    if (kPrintLayerSettings) {
        logSettings(display);
    }
    for (const auto& layer : layers) {
        ATRACE_FORMAT("DrawLayer: %s", layer.name.c_str());
        if (kPrintLayerSettings) {
            logSettings(layer);
        }
        sk_sp<SkImage> blurInput;
        if (blurCompositionLayer == &layer) {
            LOG_ALWAYS_FATAL_IF(activeSurface == dstSurface);
            LOG_ALWAYS_FATAL_IF(canvas == dstCanvas);
            blurInput = activeSurface->makeImageSnapshot();
            if (layer.blurRegions.size()) {
                SkPaint paint;
                paint.setBlendMode(SkBlendMode::kSrc);
                if (CC_UNLIKELY(mCapture->isCaptureRunning())) {
                    uint64_t id = mCapture->endOffscreenCapture(&offscreenCaptureState);
                    dstCanvas->drawAnnotation(SkRect::Make(dstCanvas->imageInfo().dimensions()),
                                              String8::format("SurfaceID|%" PRId64, id).c_str(),
                                              nullptr);
                    dstCanvas->drawImage(blurInput, 0, 0, SkSamplingOptions(), &paint);
                } else {
                    activeSurface->draw(dstCanvas, 0, 0, SkSamplingOptions(), &paint);
                }
            }
            canvas = dstCanvas;
            surfaceAutoSaveRestore.replace(canvas);
            initCanvas(canvas, display);
            LOG_ALWAYS_FATAL_IF(activeSurface->getCanvas()->getSaveCount() !=
                                dstSurface->getCanvas()->getSaveCount());
            LOG_ALWAYS_FATAL_IF(activeSurface->getCanvas()->getTotalMatrix() !=
                                dstSurface->getCanvas()->getTotalMatrix());
            activeSurface = dstSurface;
        }
        SkAutoCanvasRestore layerAutoSaveRestore(canvas, true);
        if (CC_UNLIKELY(mCapture->isCaptureRunning())) {
            std::stringstream layerSettings;
            PrintTo(layer, &layerSettings);
            canvas->drawAnnotation(SkRect::MakeEmpty(), layer.name.c_str(),
                                   SkData::MakeWithCString(layerSettings.str().c_str()));
        }
        canvas->concat(getSkM44(layer.geometry.positionTransform).asM33());
        const auto [bounds, roundRectClip] =
                getBoundsAndClip(layer.geometry.boundaries, layer.geometry.roundedCornersCrop,
                                 layer.geometry.roundedCornersRadius);
        if (mBlurFilter && layerHasBlur(layer, ctModifiesAlpha)) {
            std::unordered_map<uint32_t, sk_sp<SkImage>> cachedBlurs;
            if (!blurInput) {
                blurInput = activeSurface->makeImageSnapshot();
            }
            const auto blurRect = canvas->getTotalMatrix().mapRect(bounds.rect());
            SkAutoCanvasRestore acr(canvas, true);
            if (!roundRectClip.isEmpty()) {
                canvas->clipRRect(roundRectClip, true);
            }
            if (blurRect.width() > 0 && blurRect.height() > 0) {
                if (layer.backgroundBlurRadius > 0) {
                    ATRACE_NAME("BackgroundBlur");
                    auto blurredImage = mBlurFilter->generate(grContext, layer.backgroundBlurRadius,
                                                              blurInput, blurRect);
                    cachedBlurs[layer.backgroundBlurRadius] = blurredImage;
                    mBlurFilter->drawBlurRegion(canvas, bounds, layer.backgroundBlurRadius, 1.0f,
                                                blurRect, blurredImage, blurInput);
                }
                canvas->concat(getSkM44(layer.blurRegionTransform).asM33());
                for (auto region : layer.blurRegions) {
                    if (cachedBlurs[region.blurRadius] == nullptr) {
                        ATRACE_NAME("BlurRegion");
                        cachedBlurs[region.blurRadius] =
                                mBlurFilter->generate(grContext, region.blurRadius, blurInput,
                                                      blurRect);
                    }
                    mBlurFilter->drawBlurRegion(canvas, getBlurRRect(region), region.blurRadius,
                                                region.alpha, blurRect,
                                                cachedBlurs[region.blurRadius], blurInput);
                }
            }
        }
        if (layer.shadow.length > 0) {
            LOG_ALWAYS_FATAL_IF(layer.disableBlending, "Cannot disableBlending with a shadow");
            SkRRect shadowBounds, shadowClip;
            if (layer.geometry.boundaries == layer.shadow.boundaries) {
                shadowBounds = bounds;
                shadowClip = roundRectClip;
            } else {
                std::tie(shadowBounds, shadowClip) =
                        getBoundsAndClip(layer.shadow.boundaries, layer.geometry.roundedCornersCrop,
                                         layer.geometry.roundedCornersRadius);
            }
            const auto& rrect =
                    shadowBounds.isRect() && !shadowClip.isEmpty() ? shadowClip : shadowBounds;
            drawShadow(canvas, rrect, layer.shadow);
        }
        const float layerDimmingRatio = layer.whitePointNits <= 0.f
                ? displayDimmingRatio
                : (layer.whitePointNits / maxLayerWhitePoint) * displayDimmingRatio;
        const bool dimInLinearSpace = display.dimmingStage !=
                aidl::android::hardware::graphics::composer3::DimmingStage::GAMMA_OETF;
        const bool requiresLinearEffect = layer.colorTransform != mat4() ||
                (mUseColorManagement &&
                 needsToneMapping(layer.sourceDataspace, display.outputDataspace)) ||
                (dimInLinearSpace && !equalsWithinMargin(1.f, layerDimmingRatio));
        if (layer.skipContentDraw ||
            (layer.alpha == 0 && !requiresLinearEffect && !layer.disableBlending &&
             (!displayColorTransform || displayColorTransform->isAlphaUnchanged()))) {
            continue;
        }
        const ui::Dataspace layerDataspace = (!mUseColorManagement || requiresLinearEffect)
                ? dstDataspace
                : layer.sourceDataspace;
        SkPaint paint;
        if (layer.source.buffer.buffer) {
            ATRACE_NAME("DrawImage");
            validateInputBufferUsage(layer.source.buffer.buffer->getBuffer());
            const auto& item = layer.source.buffer;
            std::shared_ptr<AutoBackendTexture::LocalRef> imageTextureRef = nullptr;
            if (const auto& iter = cache.find(item.buffer->getBuffer()->getId());
                iter != cache.end()) {
                imageTextureRef = iter->second;
            } else {
                imageTextureRef = std::make_shared<
                        AutoBackendTexture::LocalRef>(grContext,
                                                      item.buffer->getBuffer()->toAHardwareBuffer(),
                                                      false, mTextureCleanupMgr);
            }
            if (layer.source.buffer.fence != nullptr) {
                waitFence(layer.source.buffer.fence->get());
            }
            const bool useIsOpaqueWorkaround = item.isOpaque &&
                    (imageTextureRef->colorType() == kRGBA_1010102_SkColorType ||
                     imageTextureRef->colorType() == kRGBA_F16_SkColorType);
            const auto alphaType = useIsOpaqueWorkaround ? kPremul_SkAlphaType
                    : item.isOpaque ? kOpaque_SkAlphaType
                    : item.usePremultipliedAlpha ? kPremul_SkAlphaType
                                                         : kUnpremul_SkAlphaType;
            sk_sp<SkImage> image = imageTextureRef->makeImage(layerDataspace, alphaType, grContext);
            auto texMatrix = getSkM44(item.textureTransform).asM33();
            texMatrix.preScale(1.0f / bounds.width(), 1.0f / bounds.height());
            texMatrix.postScale(image->width(), image->height());
            SkMatrix matrix;
            if (!texMatrix.invert(&matrix)) {
                matrix = texMatrix;
            }
            matrix.postTranslate(bounds.rect().fLeft, bounds.rect().fTop);
            sk_sp<SkShader> shader;
            if (layer.source.buffer.useTextureFiltering) {
                shader = image->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                           SkSamplingOptions(
                                                   {SkFilterMode::kLinear, SkMipmapMode::kNone}),
                                           &matrix);
            } else {
                shader = image->makeShader(SkSamplingOptions(), matrix);
            }
            if (useIsOpaqueWorkaround) {
                shader = SkShaders::Blend(SkBlendMode::kPlus, shader,
                                          SkShaders::Color(SkColors::kBlack,
                                                           toSkColorSpace(layerDataspace)));
            }
            paint.setShader(createRuntimeEffectShader(
                    RuntimeEffectShaderParameters{.shader = shader,
                                                  .layer = layer,
                                                  .display = display,
                                                  .undoPremultipliedAlpha = !item.isOpaque &&
                                                          item.usePremultipliedAlpha,
                                                  .requiresLinearEffect = requiresLinearEffect,
                                                  .layerDimmingRatio = dimInLinearSpace
                                                          ? layerDimmingRatio
                                                          : 1.f}));
            static constexpr float kDimmingThreshold = 0.2f;
            const bool requiresDownsample = isHdrDataspace(layer.sourceDataspace) &&
                    buffer->getPixelFormat() == PIXEL_FORMAT_RGBA_8888;
            if (layerDimmingRatio <= kDimmingThreshold || requiresDownsample) {
                paint.setDither(true);
            }
            paint.setAlphaf(layer.alpha);
            if (imageTextureRef->colorType() == kAlpha_8_SkColorType) {
                LOG_ALWAYS_FATAL_IF(layer.disableBlending, "Cannot disableBlending with A8");
                SkV4 black{0.0f, 0.0f, 0.0f, 1.0f};
                if (display.colorTransform != mat4() && display.deviceHandlesColorTransform) {
                    SkM44 colorSpaceMatrix = getSkM44(display.colorTransform);
                    if (colorSpaceMatrix.invert(&colorSpaceMatrix)) {
                        black = colorSpaceMatrix * black;
                    } else {
                        ALOGI("Could not invert colorTransform!");
                    }
                }
                SkColorMatrix colorMatrix(0, 0, 0, 0, black[0],
                                          0, 0, 0, 0, black[1],
                                          0, 0, 0, 0, black[2],
                                          0, 0, 0, -1, 1);
                if (display.colorTransform != mat4() && !display.deviceHandlesColorTransform) {
                    colorMatrix.postConcat(toSkColorMatrix(display.colorTransform));
                }
                paint.setColorFilter(SkColorFilters::Matrix(colorMatrix));
            }
        } else {
            ATRACE_NAME("DrawColor");
            const auto color = layer.source.solidColor;
            sk_sp<SkShader> shader = SkShaders::Color(SkColor4f{.fR = color.r,
                                                                .fG = color.g,
                                                                .fB = color.b,
                                                                .fA = layer.alpha},
                                                      toSkColorSpace(layerDataspace));
            paint.setShader(createRuntimeEffectShader(
                    RuntimeEffectShaderParameters{.shader = shader,
                                                  .layer = layer,
                                                  .display = display,
                                                  .undoPremultipliedAlpha = false,
                                                  .requiresLinearEffect = requiresLinearEffect,
                                                  .layerDimmingRatio = layerDimmingRatio}));
        }
        if (layer.disableBlending) {
            paint.setBlendMode(SkBlendMode::kSrc);
        }
        if (!paint.getColorFilter()) {
            if (!dimInLinearSpace && !equalsWithinMargin(1.0, layerDimmingRatio)) {
                static constexpr float kInverseGamma22 = 1.f / 2.2f;
                const auto gammaCorrectedDimmingRatio =
                        std::pow(layerDimmingRatio, kInverseGamma22);
                auto dimmingMatrix =
                        mat4::scale(vec4(gammaCorrectedDimmingRatio, gammaCorrectedDimmingRatio,
                                         gammaCorrectedDimmingRatio, 1.f));
                const auto colorFilter =
                        SkColorFilters::Matrix(toSkColorMatrix(std::move(dimmingMatrix)));
                paint.setColorFilter(displayColorTransform
                                             ? displayColorTransform->makeComposed(colorFilter)
                                             : colorFilter);
            } else {
                paint.setColorFilter(displayColorTransform);
            }
        }
        if (!roundRectClip.isEmpty()) {
            canvas->clipRRect(roundRectClip, true);
        }
        if (!bounds.isRect()) {
            paint.setAntiAlias(true);
            canvas->drawRRect(bounds, paint);
        } else {
            canvas->drawRect(bounds.rect(), paint);
        }
        if (kFlushAfterEveryLayer) {
            ATRACE_NAME("flush surface");
            activeSurface->flush();
        }
    }
    surfaceAutoSaveRestore.restore();
    mCapture->endCapture();
    {
        ATRACE_NAME("flush surface");
        LOG_ALWAYS_FATAL_IF(activeSurface != dstSurface);
        activeSurface->flush();
    }
    base::unique_fd drawFence = flush();
    bool requireSync = drawFence.get() < 0;
    if (requireSync) {
        ATRACE_BEGIN("Submit(sync=true)");
    } else {
        ATRACE_BEGIN("Submit(sync=false)");
    }
    bool success = grContext->submit(requireSync);
    ATRACE_END();
    if (!success) {
        ALOGE("Failed to flush RenderEngine commands");
        resultPromise->set_value({INVALID_OPERATION, std::move(drawFence)});
        return;
    }
    resultPromise->set_value({NO_ERROR, std::move(drawFence)});
    return;
}
inline SkRect SkiaGLRenderEngine::getSkRect(const FloatRect& rect) {
    return SkRect::MakeLTRB(rect.left, rect.top, rect.right, rect.bottom);
}
inline SkRect SkiaGLRenderEngine::getSkRect(const Rect& rect) {
    return SkRect::MakeLTRB(rect.left, rect.top, rect.right, rect.bottom);
}
static bool intersectionIsRoundRect(const SkRect& bounds, const SkRect& crop,
                                    const SkRect& insetCrop, float cornerRadius,
                                    SkVector radii[4]) {
    const bool leftEqual = bounds.fLeft == crop.fLeft;
    const bool topEqual = bounds.fTop == crop.fTop;
    const bool rightEqual = bounds.fRight == crop.fRight;
    const bool bottomEqual = bounds.fBottom == crop.fBottom;
    const bool requiredWidth = bounds.width() > (cornerRadius * 2);
    const bool requiredHeight = bounds.height() > (cornerRadius * 2);
    if (!requiredWidth || !requiredHeight) {
        return false;
    }
    if (leftEqual && topEqual) {
        radii[0].set(cornerRadius, cornerRadius);
    } else if ((leftEqual && bounds.fTop >= insetCrop.fTop) ||
               (topEqual && bounds.fLeft >= insetCrop.fLeft)) {
        radii[0].set(0, 0);
    } else {
        return false;
    }
    if (rightEqual && topEqual) {
        radii[1].set(cornerRadius, cornerRadius);
    } else if ((rightEqual && bounds.fTop >= insetCrop.fTop) ||
               (topEqual && bounds.fRight <= insetCrop.fRight)) {
        radii[1].set(0, 0);
    } else {
        return false;
    }
    if (rightEqual && bottomEqual) {
        radii[2].set(cornerRadius, cornerRadius);
    } else if ((rightEqual && bounds.fBottom <= insetCrop.fBottom) ||
               (bottomEqual && bounds.fRight <= insetCrop.fRight)) {
        radii[2].set(0, 0);
    } else {
        return false;
    }
    if (leftEqual && bottomEqual) {
        radii[3].set(cornerRadius, cornerRadius);
    } else if ((leftEqual && bounds.fBottom <= insetCrop.fBottom) ||
               (bottomEqual && bounds.fLeft >= insetCrop.fLeft)) {
        radii[3].set(0, 0);
    } else {
        return false;
    }
    return true;
}
inline std::pair<SkRRect, SkRRect> SkiaGLRenderEngine::getBoundsAndClip(const FloatRect& boundsRect,
                                                                        const FloatRect& cropRect,
                                                                        const float cornerRadius) {
    const SkRect bounds = getSkRect(boundsRect);
    const SkRect crop = getSkRect(cropRect);
    SkRRect clip;
    if (cornerRadius > 0) {
        if (bounds == crop || crop.isEmpty()) {
            return {SkRRect::MakeRectXY(bounds, cornerRadius, cornerRadius), clip};
        }
        if (crop.contains(bounds)) {
            const auto insetCrop = crop.makeInset(cornerRadius, cornerRadius);
            if (insetCrop.contains(bounds)) {
                return {SkRRect::MakeRect(bounds), clip};
            }
            SkVector radii[4];
            if (intersectionIsRoundRect(bounds, crop, insetCrop, cornerRadius, radii)) {
                SkRRect intersectionBounds;
                intersectionBounds.setRectRadii(bounds, radii);
                return {intersectionBounds, clip};
            }
        }
        clip.setRectXY(crop, cornerRadius, cornerRadius);
    }
    return {SkRRect::MakeRect(bounds), clip};
}
inline bool SkiaGLRenderEngine::layerHasBlur(const LayerSettings& layer,
                                             bool colorTransformModifiesAlpha) {
    if (layer.backgroundBlurRadius > 0 || layer.blurRegions.size()) {
        const bool opaqueContent = !layer.source.buffer.buffer || layer.source.buffer.isOpaque;
        const bool opaqueAlpha = layer.alpha == 1.0f && !colorTransformModifiesAlpha;
        return layer.skipContentDraw || !(opaqueContent && opaqueAlpha);
    }
    return false;
}
inline SkColor SkiaGLRenderEngine::getSkColor(const vec4& color) {
    return SkColorSetARGB(color.a * 255, color.r * 255, color.g * 255, color.b * 255);
}
inline SkM44 SkiaGLRenderEngine::getSkM44(const mat4& matrix) {
    return SkM44(matrix[0][0], matrix[1][0], matrix[2][0], matrix[3][0],
                 matrix[0][1], matrix[1][1], matrix[2][1], matrix[3][1],
                 matrix[0][2], matrix[1][2], matrix[2][2], matrix[3][2],
                 matrix[0][3], matrix[1][3], matrix[2][3], matrix[3][3]);
}
inline SkPoint3 SkiaGLRenderEngine::getSkPoint3(const vec3& vector) {
    return SkPoint3::Make(vector.x, vector.y, vector.z);
}
size_t SkiaGLRenderEngine::getMaxTextureSize() const {
    return mGrContext->maxTextureSize();
}
size_t SkiaGLRenderEngine::getMaxViewportDims() const {
    return mGrContext->maxRenderTargetSize();
}
void SkiaGLRenderEngine::drawShadow(SkCanvas* canvas, const SkRRect& casterRRect,
                                    const ShadowSettings& settings) {
    ATRACE_CALL();
    const float casterZ = settings.length / 2.0f;
    const auto flags =
            settings.casterIsTranslucent ? kTransparentOccluder_ShadowFlag : kNone_ShadowFlag;
    SkShadowUtils::DrawShadow(canvas, SkPath::RRect(casterRRect), SkPoint3::Make(0, 0, casterZ),
                              getSkPoint3(settings.lightPos), settings.lightRadius,
                              getSkColor(settings.ambientColor), getSkColor(settings.spotColor),
                              flags);
=======
private:
    SkCanvas* mCanvas;
    int mSaveCount;
};
static SkRRect getBlurRRect(const BlurRegion& region) {
    const auto rect = SkRect::MakeLTRB(region.left, region.top, region.right, region.bottom);
    const SkVector radii[4] = {SkVector::Make(region.cornerRadiusTL, region.cornerRadiusTL),
                               SkVector::Make(region.cornerRadiusTR, region.cornerRadiusTR),
                               SkVector::Make(region.cornerRadiusBR, region.cornerRadiusBR),
                               SkVector::Make(region.cornerRadiusBL, region.cornerRadiusBL)};
    SkRRect roundedRect;
    roundedRect.setRectRadii(rect, radii);
    return roundedRect;
}
constexpr float kDefaultMargin = 0.0001f;
static bool equalsWithinMargin(float expected, float value, float margin = kDefaultMargin) {
    LOG_ALWAYS_FATAL_IF(margin < 0.f, "Margin is negative!");
    return std::abs(expected - value) < margin;
}
namespace {
template <typename T>
void logSettings(const T& t) {
    std::stringstream stream;
    PrintTo(t, &stream);
    auto string = stream.str();
    size_t pos = 0;
    const size_t size = string.size();
    while (pos < size) {
        const size_t end = std::min(string.find("\n", pos), size);
        ALOGD("%s", string.substr(pos, end - pos).c_str());
        pos = end + 1;
    }
}
}
void SkiaGLRenderEngine::drawLayersInternal(
        const std::shared_ptr<std::promise<RenderEngineResult>>&& resultPromise,
        const DisplaySettings& display, const std::vector<LayerSettings>& layers,
        const std::shared_ptr<ExternalTexture>& buffer, const bool ,
        base::unique_fd&& bufferFence) {
    ATRACE_NAME("SkiaGL::drawLayers");
    std::lock_guard<std::mutex> lock(mRenderingMutex);
    if (layers.empty()) {
        ALOGV("Drawing empty layer stack");
        resultPromise->set_value({NO_ERROR, base::unique_fd()});
        return;
    }
    if (buffer == nullptr) {
        ALOGE("No output buffer provided. Aborting GPU composition.");
        resultPromise->set_value({BAD_VALUE, base::unique_fd()});
        return;
    }
    validateOutputBufferUsage(buffer->getBuffer());
    auto grContext = getActiveGrContext();
    auto& cache = mTextureCache;
    DeferTextureCleanup dtc(mTextureCleanupMgr);
    std::shared_ptr<AutoBackendTexture::LocalRef> surfaceTextureRef;
    if (const auto& it = cache.find(buffer->getBuffer()->getId()); it != cache.end()) {
        surfaceTextureRef = it->second;
    } else {
        surfaceTextureRef =
                std::make_shared<AutoBackendTexture::LocalRef>(grContext,
                                                               buffer->getBuffer()
                                                                       ->toAHardwareBuffer(),
                                                               true, mTextureCleanupMgr);
    }
    waitFence(bufferFence);
    const ui::Dataspace dstDataspace =
            mUseColorManagement ? display.outputDataspace : ui::Dataspace::V0_SRGB_LINEAR;
    sk_sp<SkSurface> dstSurface = surfaceTextureRef->getOrCreateSurface(dstDataspace, grContext);
    SkCanvas* dstCanvas = mCapture->tryCapture(dstSurface.get());
    if (dstCanvas == nullptr) {
        ALOGE("Cannot acquire canvas from Skia.");
        resultPromise->set_value({BAD_VALUE, base::unique_fd()});
        return;
    }
    sk_sp<SkColorFilter> displayColorTransform;
    if (display.colorTransform != mat4() && !display.deviceHandlesColorTransform) {
        displayColorTransform = SkColorFilters::Matrix(toSkColorMatrix(display.colorTransform));
    }
    const bool ctModifiesAlpha =
            displayColorTransform && !displayColorTransform->isAlphaUnchanged();
    const float maxLayerWhitePoint = std::transform_reduce(
            layers.cbegin(), layers.cend(), 0.f,
            [](float left, float right) { return std::max(left, right); },
            [&](const auto& l) { return l.whitePointNits; });
    const float displayDimmingRatio = display.targetLuminanceNits > 0.f &&
                    maxLayerWhitePoint > 0.f && display.targetLuminanceNits > maxLayerWhitePoint
            ? maxLayerWhitePoint / display.targetLuminanceNits
            : 1.f;
    sk_sp<SkSurface> activeSurface(dstSurface);
    SkCanvas* canvas = dstCanvas;
    SkiaCapture::OffscreenState offscreenCaptureState;
    const LayerSettings* blurCompositionLayer = nullptr;
    if (mBlurFilter) {
        bool requiresCompositionLayer = false;
        for (const auto& layer : layers) {
            if (!layerHasBlur(layer, ctModifiesAlpha)) {
                continue;
            }
            if (layer.backgroundBlurRadius > 0 &&
                layer.backgroundBlurRadius < mBlurFilter->getMaxCrossFadeRadius()) {
                requiresCompositionLayer = true;
            }
            for (auto region : layer.blurRegions) {
                if (region.blurRadius < mBlurFilter->getMaxCrossFadeRadius()) {
                    requiresCompositionLayer = true;
                }
            }
            if (requiresCompositionLayer) {
                activeSurface = dstSurface->makeSurface(dstSurface->imageInfo());
                canvas = mCapture->tryOffscreenCapture(activeSurface.get(), &offscreenCaptureState);
                blurCompositionLayer = &layer;
                break;
            }
        }
    }
    AutoSaveRestore surfaceAutoSaveRestore(canvas);
    canvas->clear(SK_ColorTRANSPARENT);
    initCanvas(canvas, display);
    if (kPrintLayerSettings) {
        logSettings(display);
    }
    for (const auto& layer : layers) {
        ATRACE_FORMAT("DrawLayer: %s", layer.name.c_str());
        if (kPrintLayerSettings) {
            logSettings(layer);
        }
        sk_sp<SkImage> blurInput;
        if (blurCompositionLayer == &layer) {
            LOG_ALWAYS_FATAL_IF(activeSurface == dstSurface);
            LOG_ALWAYS_FATAL_IF(canvas == dstCanvas);
            blurInput = activeSurface->makeImageSnapshot();
            if (layer.blurRegions.size()) {
                SkPaint paint;
                paint.setBlendMode(SkBlendMode::kSrc);
                if (CC_UNLIKELY(mCapture->isCaptureRunning())) {
                    uint64_t id = mCapture->endOffscreenCapture(&offscreenCaptureState);
                    dstCanvas->drawAnnotation(SkRect::Make(dstCanvas->imageInfo().dimensions()),
                                              String8::format("SurfaceID|%" PRId64, id).c_str(),
                                              nullptr);
                    dstCanvas->drawImage(blurInput, 0, 0, SkSamplingOptions(), &paint);
                } else {
                    activeSurface->draw(dstCanvas, 0, 0, SkSamplingOptions(), &paint);
                }
            }
            canvas = dstCanvas;
            surfaceAutoSaveRestore.replace(canvas);
            initCanvas(canvas, display);
            LOG_ALWAYS_FATAL_IF(activeSurface->getCanvas()->getSaveCount() !=
                                dstSurface->getCanvas()->getSaveCount());
            LOG_ALWAYS_FATAL_IF(activeSurface->getCanvas()->getTotalMatrix() !=
                                dstSurface->getCanvas()->getTotalMatrix());
            activeSurface = dstSurface;
        }
        SkAutoCanvasRestore layerAutoSaveRestore(canvas, true);
        if (CC_UNLIKELY(mCapture->isCaptureRunning())) {
            std::stringstream layerSettings;
            PrintTo(layer, &layerSettings);
            canvas->drawAnnotation(SkRect::MakeEmpty(), layer.name.c_str(),
                                   SkData::MakeWithCString(layerSettings.str().c_str()));
        }
        canvas->concat(getSkM44(layer.geometry.positionTransform).asM33());
        const auto [bounds, roundRectClip] =
                getBoundsAndClip(layer.geometry.boundaries, layer.geometry.roundedCornersCrop,
                                 layer.geometry.roundedCornersRadius);
        if (mBlurFilter && layerHasBlur(layer, ctModifiesAlpha)) {
            std::unordered_map<uint32_t, sk_sp<SkImage>> cachedBlurs;
            if (!blurInput) {
                blurInput = activeSurface->makeImageSnapshot();
            }
            const auto blurRect = canvas->getTotalMatrix().mapRect(bounds.rect());
            SkAutoCanvasRestore acr(canvas, true);
            if (!roundRectClip.isEmpty()) {
                canvas->clipRRect(roundRectClip, true);
            }
            if (blurRect.width() > 0 && blurRect.height() > 0) {
                if (layer.backgroundBlurRadius > 0) {
                    ATRACE_NAME("BackgroundBlur");
                    auto blurredImage = mBlurFilter->generate(grContext, layer.backgroundBlurRadius,
                                                              blurInput, blurRect);
                    cachedBlurs[layer.backgroundBlurRadius] = blurredImage;
                    mBlurFilter->drawBlurRegion(canvas, bounds, layer.backgroundBlurRadius, 1.0f,
                                                blurRect, blurredImage, blurInput);
                }
                canvas->concat(getSkM44(layer.blurRegionTransform).asM33());
                for (auto region : layer.blurRegions) {
                    if (cachedBlurs[region.blurRadius] == nullptr) {
                        ATRACE_NAME("BlurRegion");
                        cachedBlurs[region.blurRadius] =
                                mBlurFilter->generate(grContext, region.blurRadius, blurInput,
                                                      blurRect);
                    }
                    mBlurFilter->drawBlurRegion(canvas, getBlurRRect(region), region.blurRadius,
                                                region.alpha, blurRect,
                                                cachedBlurs[region.blurRadius], blurInput);
                }
            }
        }
        if (layer.shadow.length > 0) {
            LOG_ALWAYS_FATAL_IF(layer.disableBlending, "Cannot disableBlending with a shadow");
            SkRRect shadowBounds, shadowClip;
            if (layer.geometry.boundaries == layer.shadow.boundaries) {
                shadowBounds = bounds;
                shadowClip = roundRectClip;
            } else {
                std::tie(shadowBounds, shadowClip) =
                        getBoundsAndClip(layer.shadow.boundaries, layer.geometry.roundedCornersCrop,
                                         layer.geometry.roundedCornersRadius);
            }
            const auto& rrect =
                    shadowBounds.isRect() && !shadowClip.isEmpty() ? shadowClip : shadowBounds;
            drawShadow(canvas, rrect, layer.shadow);
        }
        const float layerDimmingRatio = layer.whitePointNits <= 0.f
                ? displayDimmingRatio
                : (layer.whitePointNits / maxLayerWhitePoint) * displayDimmingRatio;
        const bool dimInLinearSpace = display.dimmingStage !=
                aidl::android::hardware::graphics::composer3::DimmingStage::GAMMA_OETF;
        const bool requiresLinearEffect = layer.colorTransform != mat4() ||
                (mUseColorManagement &&
                 needsToneMapping(layer.sourceDataspace, display.outputDataspace)) ||
                (dimInLinearSpace && !equalsWithinMargin(1.f, layerDimmingRatio));
        if (layer.skipContentDraw ||
            (layer.alpha == 0 && !requiresLinearEffect && !layer.disableBlending &&
             (!displayColorTransform || displayColorTransform->isAlphaUnchanged()))) {
            continue;
        }
        const ui::Dataspace layerDataspace = (!mUseColorManagement || requiresLinearEffect)
                ? dstDataspace
                : layer.sourceDataspace;
        SkPaint paint;
        if (layer.source.buffer.buffer) {
            ATRACE_NAME("DrawImage");
            validateInputBufferUsage(layer.source.buffer.buffer->getBuffer());
            const auto& item = layer.source.buffer;
            std::shared_ptr<AutoBackendTexture::LocalRef> imageTextureRef = nullptr;
            if (const auto& iter = cache.find(item.buffer->getBuffer()->getId());
                iter != cache.end()) {
                imageTextureRef = iter->second;
            } else {
                imageTextureRef = std::make_shared<
                        AutoBackendTexture::LocalRef>(grContext,
                                                      item.buffer->getBuffer()->toAHardwareBuffer(),
                                                      false, mTextureCleanupMgr);
            }
            if (layer.source.buffer.fence != nullptr) {
                waitFence(layer.source.buffer.fence->get());
            }
            const bool useIsOpaqueWorkaround = item.isOpaque &&
                    (imageTextureRef->colorType() == kRGBA_1010102_SkColorType ||
                     imageTextureRef->colorType() == kRGBA_F16_SkColorType);
            const auto alphaType = useIsOpaqueWorkaround ? kPremul_SkAlphaType
                    : item.isOpaque ? kOpaque_SkAlphaType
                    : item.usePremultipliedAlpha ? kPremul_SkAlphaType
                                                         : kUnpremul_SkAlphaType;
            sk_sp<SkImage> image = imageTextureRef->makeImage(layerDataspace, alphaType, grContext);
            auto texMatrix = getSkM44(item.textureTransform).asM33();
            texMatrix.preScale(1.0f / bounds.width(), 1.0f / bounds.height());
            texMatrix.postScale(image->width(), image->height());
            SkMatrix matrix;
            if (!texMatrix.invert(&matrix)) {
                matrix = texMatrix;
            }
            matrix.postTranslate(bounds.rect().fLeft, bounds.rect().fTop);
            sk_sp<SkShader> shader;
            if (layer.source.buffer.useTextureFiltering) {
                shader = image->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                           SkSamplingOptions(
                                                   {SkFilterMode::kLinear, SkMipmapMode::kNone}),
                                           &matrix);
            } else {
                shader = image->makeShader(SkSamplingOptions(), matrix);
            }
            if (useIsOpaqueWorkaround) {
                shader = SkShaders::Blend(SkBlendMode::kPlus, shader,
                                          SkShaders::Color(SkColors::kBlack,
                                                           toSkColorSpace(layerDataspace)));
            }
            paint.setShader(createRuntimeEffectShader(
                    RuntimeEffectShaderParameters{.shader = shader,
                                                  .layer = layer,
                                                  .display = display,
                                                  .undoPremultipliedAlpha = !item.isOpaque &&
                                                          item.usePremultipliedAlpha,
                                                  .requiresLinearEffect = requiresLinearEffect,
                                                  .layerDimmingRatio = dimInLinearSpace
                                                          ? layerDimmingRatio
                                                          : 1.f}));
            static constexpr float kDimmingThreshold = 0.2f;
            const bool requiresDownsample = isHdrDataspace(layer.sourceDataspace) &&
                    buffer->getPixelFormat() == PIXEL_FORMAT_RGBA_8888;
            if (layerDimmingRatio <= kDimmingThreshold || requiresDownsample) {
                paint.setDither(true);
            }
            paint.setAlphaf(layer.alpha);
            if (imageTextureRef->colorType() == kAlpha_8_SkColorType) {
                LOG_ALWAYS_FATAL_IF(layer.disableBlending, "Cannot disableBlending with A8");
                SkV4 black{0.0f, 0.0f, 0.0f, 1.0f};
                if (display.colorTransform != mat4() && display.deviceHandlesColorTransform) {
                    SkM44 colorSpaceMatrix = getSkM44(display.colorTransform);
                    if (colorSpaceMatrix.invert(&colorSpaceMatrix)) {
                        black = colorSpaceMatrix * black;
                    } else {
                        ALOGI("Could not invert colorTransform!");
                    }
                }
                SkColorMatrix colorMatrix(0, 0, 0, 0, black[0],
                                          0, 0, 0, 0, black[1],
                                          0, 0, 0, 0, black[2],
                                          0, 0, 0, -1, 1);
                if (display.colorTransform != mat4() && !display.deviceHandlesColorTransform) {
                    colorMatrix.postConcat(toSkColorMatrix(display.colorTransform));
                }
                paint.setColorFilter(SkColorFilters::Matrix(colorMatrix));
            }
        } else {
            ATRACE_NAME("DrawColor");
            const auto color = layer.source.solidColor;
            sk_sp<SkShader> shader = SkShaders::Color(SkColor4f{.fR = color.r,
                                                                .fG = color.g,
                                                                .fB = color.b,
                                                                .fA = layer.alpha},
                                                      toSkColorSpace(layerDataspace));
            paint.setShader(createRuntimeEffectShader(
                    RuntimeEffectShaderParameters{.shader = shader,
                                                  .layer = layer,
                                                  .display = display,
                                                  .undoPremultipliedAlpha = false,
                                                  .requiresLinearEffect = requiresLinearEffect,
                                                  .layerDimmingRatio = layerDimmingRatio}));
        }
        if (layer.disableBlending) {
            paint.setBlendMode(SkBlendMode::kSrc);
        }
        if (!paint.getColorFilter()) {
            if (!dimInLinearSpace && !equalsWithinMargin(1.0, layerDimmingRatio)) {
                static constexpr float kInverseGamma22 = 1.f / 2.2f;
                const auto gammaCorrectedDimmingRatio =
                        std::pow(layerDimmingRatio, kInverseGamma22);
                auto dimmingMatrix =
                        mat4::scale(vec4(gammaCorrectedDimmingRatio, gammaCorrectedDimmingRatio,
                                         gammaCorrectedDimmingRatio, 1.f));
                const auto colorFilter =
                        SkColorFilters::Matrix(toSkColorMatrix(std::move(dimmingMatrix)));
                paint.setColorFilter(displayColorTransform
                                             ? displayColorTransform->makeComposed(colorFilter)
                                             : colorFilter);
            } else {
                paint.setColorFilter(displayColorTransform);
            }
        }
        if (!roundRectClip.isEmpty()) {
            canvas->clipRRect(roundRectClip, true);
        }
        if (!bounds.isRect()) {
            paint.setAntiAlias(true);
            canvas->drawRRect(bounds, paint);
        } else {
            canvas->drawRect(bounds.rect(), paint);
        }
        if (kFlushAfterEveryLayer) {
            ATRACE_NAME("flush surface");
            activeSurface->flush();
        }
    }
    surfaceAutoSaveRestore.restore();
    mCapture->endCapture();
    {
        ATRACE_NAME("flush surface");
        LOG_ALWAYS_FATAL_IF(activeSurface != dstSurface);
        activeSurface->flush();
    }
    base::unique_fd drawFence = flush();
    bool requireSync = drawFence.get() < 0;
    if (requireSync) {
        ATRACE_BEGIN("Submit(sync=true)");
    } else {
        ATRACE_BEGIN("Submit(sync=false)");
    }
    bool success = grContext->submit(requireSync);
    ATRACE_END();
    if (!success) {
        ALOGE("Failed to flush RenderEngine commands");
        resultPromise->set_value({INVALID_OPERATION, std::move(drawFence)});
        return;
    }
    resultPromise->set_value({NO_ERROR, std::move(drawFence)});
    return;
}
inline SkRect SkiaGLRenderEngine::getSkRect(const FloatRect& rect) {
    return SkRect::MakeLTRB(rect.left, rect.top, rect.right, rect.bottom);
}
inline SkRect SkiaGLRenderEngine::getSkRect(const Rect& rect) {
    return SkRect::MakeLTRB(rect.left, rect.top, rect.right, rect.bottom);
}
static bool intersectionIsRoundRect(const SkRect& bounds, const SkRect& crop,
                                    const SkRect& insetCrop, const vec2& cornerRadius,
                                    SkVector radii[4]) {
    const bool leftEqual = bounds.fLeft == crop.fLeft;
    const bool topEqual = bounds.fTop == crop.fTop;
    const bool rightEqual = bounds.fRight == crop.fRight;
    const bool bottomEqual = bounds.fBottom == crop.fBottom;
    const bool requiredWidth = bounds.width() > (cornerRadius.x * 2);
    const bool requiredHeight = bounds.height() > (cornerRadius.y * 2);
    if (!requiredWidth || !requiredHeight) {
        return false;
    }
    if (leftEqual && topEqual) {
        radii[0].set(cornerRadius.x, cornerRadius.y);
    } else if ((leftEqual && bounds.fTop >= insetCrop.fTop) ||
               (topEqual && bounds.fLeft >= insetCrop.fLeft)) {
        radii[0].set(0, 0);
    } else {
        return false;
    }
    if (rightEqual && topEqual) {
        radii[1].set(cornerRadius.x, cornerRadius.y);
    } else if ((rightEqual && bounds.fTop >= insetCrop.fTop) ||
               (topEqual && bounds.fRight <= insetCrop.fRight)) {
        radii[1].set(0, 0);
    } else {
        return false;
    }
    if (rightEqual && bottomEqual) {
        radii[2].set(cornerRadius.x, cornerRadius.y);
    } else if ((rightEqual && bounds.fBottom <= insetCrop.fBottom) ||
               (bottomEqual && bounds.fRight <= insetCrop.fRight)) {
        radii[2].set(0, 0);
    } else {
        return false;
    }
    if (leftEqual && bottomEqual) {
        radii[3].set(cornerRadius.x, cornerRadius.y);
    } else if ((leftEqual && bounds.fBottom <= insetCrop.fBottom) ||
               (bottomEqual && bounds.fLeft >= insetCrop.fLeft)) {
        radii[3].set(0, 0);
    } else {
        return false;
    }
    return true;
}
inline std::pair<SkRRect, SkRRect> SkiaGLRenderEngine::getBoundsAndClip(const FloatRect& boundsRect,
                                                                        const FloatRect& cropRect,
                                                                        const vec2& cornerRadius) {
    const SkRect bounds = getSkRect(boundsRect);
    const SkRect crop = getSkRect(cropRect);
    SkRRect clip;
    if (cornerRadius.x > 0 && cornerRadius.y > 0) {
        if (bounds == crop || crop.isEmpty()) {
            return {SkRRect::MakeRectXY(bounds, cornerRadius.x, cornerRadius.y), clip};
        }
        if (crop.contains(bounds)) {
            const auto insetCrop = crop.makeInset(cornerRadius.x, cornerRadius.y);
            if (insetCrop.contains(bounds)) {
                return {SkRRect::MakeRect(bounds), clip};
            }
            SkVector radii[4];
            if (intersectionIsRoundRect(bounds, crop, insetCrop, cornerRadius, radii)) {
                SkRRect intersectionBounds;
                intersectionBounds.setRectRadii(bounds, radii);
                return {intersectionBounds, clip};
            }
        }
        clip.setRectXY(crop, cornerRadius.x, cornerRadius.y);
    }
    return {SkRRect::MakeRect(bounds), clip};
}
inline bool SkiaGLRenderEngine::layerHasBlur(const LayerSettings& layer,
                                             bool colorTransformModifiesAlpha) {
    if (layer.backgroundBlurRadius > 0 || layer.blurRegions.size()) {
        const bool opaqueContent = !layer.source.buffer.buffer || layer.source.buffer.isOpaque;
        const bool opaqueAlpha = layer.alpha == 1.0f && !colorTransformModifiesAlpha;
        return layer.skipContentDraw || !(opaqueContent && opaqueAlpha);
    }
    return false;
}
inline SkColor SkiaGLRenderEngine::getSkColor(const vec4& color) {
    return SkColorSetARGB(color.a * 255, color.r * 255, color.g * 255, color.b * 255);
}
inline SkM44 SkiaGLRenderEngine::getSkM44(const mat4& matrix) {
    return SkM44(matrix[0][0], matrix[1][0], matrix[2][0], matrix[3][0],
                 matrix[0][1], matrix[1][1], matrix[2][1], matrix[3][1],
                 matrix[0][2], matrix[1][2], matrix[2][2], matrix[3][2],
                 matrix[0][3], matrix[1][3], matrix[2][3], matrix[3][3]);
}
inline SkPoint3 SkiaGLRenderEngine::getSkPoint3(const vec3& vector) {
    return SkPoint3::Make(vector.x, vector.y, vector.z);
}
size_t SkiaGLRenderEngine::getMaxTextureSize() const {
    return mGrContext->maxTextureSize();
}
size_t SkiaGLRenderEngine::getMaxViewportDims() const {
    return mGrContext->maxRenderTargetSize();
}
void SkiaGLRenderEngine::drawShadow(SkCanvas* canvas, const SkRRect& casterRRect,
                                    const ShadowSettings& settings) {
    ATRACE_CALL();
    const float casterZ = settings.length / 2.0f;
    const auto flags =
            settings.casterIsTranslucent ? kTransparentOccluder_ShadowFlag : kNone_ShadowFlag;
    SkShadowUtils::DrawShadow(canvas, SkPath::RRect(casterRRect), SkPoint3::Make(0, 0, casterZ),
                              getSkPoint3(settings.lightPos), settings.lightRadius,
                              getSkColor(settings.ambientColor), getSkColor(settings.spotColor),
                              flags);
>>>>>>> b22850c1
}
EGLContext SkiaGLRenderEngine::createEglContext(EGLDisplay display, EGLConfig config,
                                                EGLContext shareContext,
                                                std::optional<ContextPriority> contextPriority,
                                                Protection protection) {
    EGLint renderableType = 0;
    if (config == EGL_NO_CONFIG_KHR) {
        renderableType = EGL_OPENGL_ES3_BIT;
    } else if (!eglGetConfigAttrib(display, config, EGL_RENDERABLE_TYPE, &renderableType)) {
        LOG_ALWAYS_FATAL("can't query EGLConfig RENDERABLE_TYPE");
    }
    EGLint contextClientVersion = 0;
    if (renderableType & EGL_OPENGL_ES3_BIT) {
        contextClientVersion = 3;
    } else if (renderableType & EGL_OPENGL_ES2_BIT) {
        contextClientVersion = 2;
    } else if (renderableType & EGL_OPENGL_ES_BIT) {
        contextClientVersion = 1;
    } else {
        LOG_ALWAYS_FATAL("no supported EGL_RENDERABLE_TYPEs");
    }
    std::vector<EGLint> contextAttributes;
    contextAttributes.reserve(7);
    contextAttributes.push_back(EGL_CONTEXT_CLIENT_VERSION);
    contextAttributes.push_back(contextClientVersion);
    if (contextPriority) {
        contextAttributes.push_back(EGL_CONTEXT_PRIORITY_LEVEL_IMG);
        switch (*contextPriority) {
            case ContextPriority::REALTIME:
                contextAttributes.push_back(EGL_CONTEXT_PRIORITY_REALTIME_NV);
                break;
            case ContextPriority::MEDIUM:
                contextAttributes.push_back(EGL_CONTEXT_PRIORITY_MEDIUM_IMG);
                break;
            case ContextPriority::LOW:
                contextAttributes.push_back(EGL_CONTEXT_PRIORITY_LOW_IMG);
                break;
            case ContextPriority::HIGH:
            default:
                contextAttributes.push_back(EGL_CONTEXT_PRIORITY_HIGH_IMG);
                break;
        }
    }
    if (protection == Protection::PROTECTED) {
        contextAttributes.push_back(EGL_PROTECTED_CONTENT_EXT);
        contextAttributes.push_back(EGL_TRUE);
    }
    contextAttributes.push_back(EGL_NONE);
    EGLContext context = eglCreateContext(display, config, shareContext, contextAttributes.data());
    if (contextClientVersion == 3 && context == EGL_NO_CONTEXT) {
        if (config != EGL_NO_CONFIG_KHR) {
            return context;
        }
        contextAttributes[1] = 2;
        context = eglCreateContext(display, config, shareContext, contextAttributes.data());
    }
    return context;
}
std::optional<RenderEngine::ContextPriority> SkiaGLRenderEngine::createContextPriority(
        const RenderEngineCreationArgs& args) {
    if (!gl::GLExtensions::getInstance().hasContextPriority()) {
        return std::nullopt;
    }
    switch (args.contextPriority) {
        case RenderEngine::ContextPriority::REALTIME:
            if (gl::GLExtensions::getInstance().hasRealtimePriority()) {
                return RenderEngine::ContextPriority::REALTIME;
            } else {
                ALOGI("Realtime priority unsupported, degrading gracefully to high priority");
                return RenderEngine::ContextPriority::HIGH;
            }
        case RenderEngine::ContextPriority::HIGH:
        case RenderEngine::ContextPriority::MEDIUM:
        case RenderEngine::ContextPriority::LOW:
            return args.contextPriority;
        default:
            return std::nullopt;
    }
}
EGLSurface SkiaGLRenderEngine::createPlaceholderEglPbufferSurface(EGLDisplay display,
                                                                  EGLConfig config, int hwcFormat,
                                                                  Protection protection) {
    EGLConfig placeholderConfig = config;
    if (placeholderConfig == EGL_NO_CONFIG_KHR) {
        placeholderConfig = chooseEglConfig(display, hwcFormat, true);
    }
    std::vector<EGLint> attributes;
    attributes.reserve(7);
    attributes.push_back(EGL_WIDTH);
    attributes.push_back(1);
    attributes.push_back(EGL_HEIGHT);
    attributes.push_back(1);
    if (protection == Protection::PROTECTED) {
        attributes.push_back(EGL_PROTECTED_CONTENT_EXT);
        attributes.push_back(EGL_TRUE);
    }
    attributes.push_back(EGL_NONE);
    return eglCreatePbufferSurface(display, placeholderConfig, attributes.data());
}
int SkiaGLRenderEngine::getContextPriority() {
    int value;
    eglQueryContext(mEGLDisplay, mEGLContext, EGL_CONTEXT_PRIORITY_LEVEL_IMG, &value);
    return value;
}
void SkiaGLRenderEngine::appendBackendSpecificInfoToDump(std::string& result) {
    const gl::GLExtensions& extensions = gl::GLExtensions::getInstance();
    StringAppendF(&result, "\n ------------RE GLES------------\n");
    StringAppendF(&result, "EGL implementation : %s\n", extensions.getEGLVersion());
    StringAppendF(&result, "%s\n", extensions.getEGLExtensions());
    StringAppendF(&result, "GLES: %s, %s, %s\n", extensions.getVendor(), extensions.getRenderer(),
                  extensions.getVersion());
    StringAppendF(&result, "%s\n", extensions.getExtensions());
}
}
}
}
