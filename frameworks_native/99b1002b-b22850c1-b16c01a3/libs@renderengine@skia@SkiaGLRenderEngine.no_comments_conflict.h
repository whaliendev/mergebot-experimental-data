#ifndef SF_SKIAGLRENDERENGINE_H_
#define SF_SKIAGLRENDERENGINE_H_ 
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GrDirectContext.h>
#include <SkSurface.h>
#include <android-base/thread_annotations.h>
#include <renderengine/ExternalTexture.h>
#include <renderengine/RenderEngine.h>
#include <sys/types.h>
#include <mutex>
#include <unordered_map>
#include "AutoBackendTexture.h"
#include "EGL/egl.h"
#include "GrContextOptions.h"
#include "SkImageInfo.h"
#include "SkiaRenderEngine.h"
#include "android-base/macros.h"
#include "debug/SkiaCapture.h"
#include "filters/BlurFilter.h"
#include "filters/LinearEffect.h"
#include "filters/StretchShaderFactory.h"
class SkData;
struct SkPoint3;
namespace android {
namespace renderengine {
namespace skia {
class SkiaGLRenderEngine : public skia::SkiaRenderEngine {
public:
    static std::unique_ptr<SkiaGLRenderEngine> create(const RenderEngineCreationArgs& args);
    SkiaGLRenderEngine(const RenderEngineCreationArgs& args, EGLDisplay display, EGLContext ctxt,
                       EGLSurface placeholder, EGLContext protectedContext,
                       EGLSurface protectedPlaceholder);
    ~SkiaGLRenderEngine() override;
    int getContextPriority() override;
protected:
    virtual SkiaRenderEngine::Contexts createDirectContexts(const GrContextOptions& options);
    bool supportsProtectedContentImpl() const override;
    bool useProtectedContextImpl(GrProtected isProtected) override;
    void waitFence(GrDirectContext* grContext, base::borrowed_fd fenceFd) override;
    base::unique_fd flushAndSubmit(GrDirectContext* context) override;
    void appendBackendSpecificInfoToDump(std::string& result) override;
private:
    bool waitGpuFence(base::borrowed_fd fenceFd);
    base::unique_fd flush();
    static EGLConfig chooseEglConfig(EGLDisplay display, int format, bool logConfig);
    static EGLContext createEglContext(EGLDisplay display, EGLConfig config,
                                       EGLContext shareContext,
                                       std::optional<ContextPriority> contextPriority,
                                       Protection protection);
    static std::optional<RenderEngine::ContextPriority> createContextPriority(
            const RenderEngineCreationArgs& args);
<<<<<<< HEAD
    static EGLSurface createPlaceholderEglPbufferSurface(
            EGLDisplay display, EGLConfig config, int hwcFormat, Protection protection);
||||||| b16c01a3e6
    static EGLSurface createPlaceholderEglPbufferSurface(EGLDisplay display, EGLConfig config,
                                                         int hwcFormat, Protection protection);
    inline SkRect getSkRect(const FloatRect& layer);
    inline SkRect getSkRect(const Rect& layer);
    inline std::pair<SkRRect, SkRRect> getBoundsAndClip(const FloatRect& bounds,
                                                        const FloatRect& crop, float cornerRadius);
    inline bool layerHasBlur(const LayerSettings& layer, bool colorTransformModifiesAlpha);
    inline SkColor getSkColor(const vec4& color);
    inline SkM44 getSkM44(const mat4& matrix);
    inline SkPoint3 getSkPoint3(const vec3& vector);
    inline GrDirectContext* getActiveGrContext() const;
    base::unique_fd flush();
    void waitFence(base::borrowed_fd fenceFd);
    bool waitGpuFence(base::borrowed_fd fenceFd);
    void initCanvas(SkCanvas* canvas, const DisplaySettings& display);
    void drawShadow(SkCanvas* canvas, const SkRRect& casterRRect,
                    const ShadowSettings& shadowSettings);
    struct RuntimeEffectShaderParameters {
        sk_sp<SkShader> shader;
        const LayerSettings& layer;
        const DisplaySettings& display;
        bool undoPremultipliedAlpha;
        bool requiresLinearEffect;
        float layerDimmingRatio;
    };
    sk_sp<SkShader> createRuntimeEffectShader(const RuntimeEffectShaderParameters&);
=======
    static EGLSurface createPlaceholderEglPbufferSurface(EGLDisplay display, EGLConfig config,
                                                         int hwcFormat, Protection protection);
    inline SkRect getSkRect(const FloatRect& layer);
    inline SkRect getSkRect(const Rect& layer);
    inline std::pair<SkRRect, SkRRect> getBoundsAndClip(const FloatRect& bounds,
                                                        const FloatRect& crop,
                                                        const vec2& cornerRadius);
    inline bool layerHasBlur(const LayerSettings& layer, bool colorTransformModifiesAlpha);
    inline SkColor getSkColor(const vec4& color);
    inline SkM44 getSkM44(const mat4& matrix);
    inline SkPoint3 getSkPoint3(const vec3& vector);
    inline GrDirectContext* getActiveGrContext() const;
    base::unique_fd flush();
    void waitFence(base::borrowed_fd fenceFd);
    bool waitGpuFence(base::borrowed_fd fenceFd);
    void initCanvas(SkCanvas* canvas, const DisplaySettings& display);
    void drawShadow(SkCanvas* canvas, const SkRRect& casterRRect,
                    const ShadowSettings& shadowSettings);
    struct RuntimeEffectShaderParameters {
        sk_sp<SkShader> shader;
        const LayerSettings& layer;
        const DisplaySettings& display;
        bool undoPremultipliedAlpha;
        bool requiresLinearEffect;
        float layerDimmingRatio;
    };
    sk_sp<SkShader> createRuntimeEffectShader(const RuntimeEffectShaderParameters&);
>>>>>>> b22850c1
    EGLDisplay mEGLDisplay;
    EGLContext mEGLContext;
    EGLSurface mPlaceholderSurface;
    EGLContext mProtectedEGLContext;
    EGLSurface mProtectedPlaceholderSurface;
};
}
}
}
#endif
