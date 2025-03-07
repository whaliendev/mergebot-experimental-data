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
class SkData
        struct SkPoint3;
namespace android {
namespace renderengine {
namespace skia {
class SkiaGLRenderEngine : public skia::SkiaRenderEngine {
protected:
    virtual SkiaRenderEngine::Contexts createDirectContexts(const GrContextOptions& options);
    bool supportsProtectedContentImpl() const override;
    bool useProtectedContextImpl(GrProtected isProtected) override;
    void waitFence(GrDirectContext* grContext, base::borrowed_fd fenceFd) override;
    base::unique_fd flushAndSubmit(GrDirectContext* context) override;
    void appendBackendSpecificInfoToDump(std::string& result) override;
public:
    static std::unique_ptr<SkiaGLRenderEngine> create(const RenderEngineCreationArgs& args);
    SkiaGLRenderEngine(const RenderEngineCreationArgs& args, EGLDisplay display, EGLContext ctxt,
                       EGLSurface placeholder, EGLContext protectedContext,
                       EGLSurface protectedPlaceholder);
    ~SkiaGLRenderEngine() override;
    int getContextPriority() override;
private:
    static EGLConfig chooseEglConfig(EGLDisplay display, int format, bool logConfig);
    static EGLContext createEglContext(EGLDisplay display, EGLConfig config,
                                       EGLContext shareContext,
                                       std::optional<ContextPriority> contextPriority,
                                       Protection protection);
    static std::optional<RenderEngine::ContextPriority> createContextPriority(
            const RenderEngineCreationArgs& args);
    static EGLSurface createPlaceholderEglPbufferSurface(EGLDisplay display, EGLConfig config,
                                                         int hwcFormat, Protection protection);
    base::unique_fd flush();
    bool waitGpuFence(base::borrowed_fd fenceFd);
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
