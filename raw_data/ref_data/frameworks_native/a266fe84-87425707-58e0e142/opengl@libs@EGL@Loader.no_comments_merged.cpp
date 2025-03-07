#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include "EGL/Loader.h"
#include <android-base/properties.h>
#include <android/dlext.h>
#include <cutils/properties.h>
#include <dirent.h>
#include <dlfcn.h>
#include <graphicsenv/GraphicsEnv.h>
#include <log/log.h>
#include <utils/Timers.h>
#include <vndksupport/linker.h>
#include <string>
#include "EGL/eglext_angle.h"
#include "egl_platform_entries.h"
#include "egl_trace.h"
#include "egldefs.h"
namespace android {
Loader& Loader::getInstance() {
    static Loader loader;
    return loader;
}
static void* do_dlopen(const char* path, int mode) {
    ATRACE_CALL();
    return dlopen(path, mode);
}
static void* do_android_dlopen_ext(const char* path, int mode, const android_dlextinfo* info) {
    ATRACE_CALL();
    return android_dlopen_ext(path, mode, info);
}
static void* do_android_load_sphal_library(const char* path, int mode) {
    ATRACE_CALL();
    return android_load_sphal_library(path, mode);
}
static int do_android_unload_sphal_library(void* dso) {
    ATRACE_CALL();
    return android_unload_sphal_library(dso);
}
Loader::driver_t::driver_t(void* gles)
{
    dso[0] = gles;
    for (size_t i=1 ; i<NELEM(dso) ; i++)
        dso[i] = nullptr;
}
Loader::driver_t::~driver_t()
{
    for (size_t i=0 ; i<NELEM(dso) ; i++) {
        if (dso[i]) {
            dlclose(dso[i]);
            dso[i] = nullptr;
        }
    }
}
int Loader::driver_t::set(void* hnd, int32_t api)
{
    switch (api) {
        case EGL:
            dso[0] = hnd;
            break;
        case GLESv1_CM:
            dso[1] = hnd;
            break;
        case GLESv2:
            dso[2] = hnd;
            break;
        default:
            return -EOVERFLOW;
    }
    return 0;
}
Loader::Loader()
    : getProcAddress(nullptr)
{
}
Loader::~Loader() {
}
static void* load_wrapper(const char* path) {
    void* so = do_dlopen(path, RTLD_NOW | RTLD_LOCAL);
    ALOGE_IF(!so, "dlopen(\"%s\") failed: %s", path, dlerror());
    return so;
}
#ifndef EGL_WRAPPER_DIR
#if defined(__LP64__)
#define EGL_WRAPPER_DIR "/system/lib64"
#else
#define EGL_WRAPPER_DIR "/system/lib"
#endif
#endif
static const char* PERSIST_DRIVER_SUFFIX_PROPERTY = "persist.graphics.egl";
static const char* RO_DRIVER_SUFFIX_PROPERTY = "ro.hardware.egl";
static const char* RO_BOARD_PLATFORM_PROPERTY = "ro.board.platform";
static const char* HAL_SUBNAME_KEY_PROPERTIES[3] = {
        PERSIST_DRIVER_SUFFIX_PROPERTY,
        RO_DRIVER_SUFFIX_PROPERTY,
        RO_BOARD_PLATFORM_PROPERTY,
};
static bool should_unload_system_driver(egl_connection_t* cnx) {
    if (cnx->systemDriverUnloaded) {
        return false;
    }
    android_namespace_t* ns = android::GraphicsEnv::getInstance().getAngleNamespace();
    if (ns) {
        if (!(cnx->angleLoaded && android::GraphicsEnv::getInstance().shouldUseSystemAngle())) {
            return true;
        }
    }
    if (android::GraphicsEnv::getInstance().shouldUseNativeDriver() && cnx->angleLoaded) {
        return true;
    }
    ns = android::GraphicsEnv::getInstance().getDriverNamespace();
    if (ns) {
        return true;
    }
    return false;
}
static void uninit_api(char const* const* api, __eglMustCastToProperFunctionPointerType* curr) {
    while (*api) {
        *curr++ = nullptr;
        api++;
    }
}
void Loader::unload_system_driver(egl_connection_t* cnx) {
    ATRACE_CALL();
    uninit_api(gl_names,
               (__eglMustCastToProperFunctionPointerType*)&cnx
                       ->hooks[egl_connection_t::GLESv2_INDEX]
                       ->gl);
    uninit_api(gl_names,
               (__eglMustCastToProperFunctionPointerType*)&cnx
                       ->hooks[egl_connection_t::GLESv1_INDEX]
                       ->gl);
    uninit_api(egl_names, (__eglMustCastToProperFunctionPointerType*)&cnx->egl);
    if (cnx->dso) {
        ALOGD("Unload system gl driver.");
        driver_t* hnd = (driver_t*)cnx->dso;
        if (hnd->dso[2]) {
            do_android_unload_sphal_library(hnd->dso[2]);
        }
        if (hnd->dso[1]) {
            do_android_unload_sphal_library(hnd->dso[1]);
        }
        if (hnd->dso[0]) {
            do_android_unload_sphal_library(hnd->dso[0]);
        }
        cnx->dso = nullptr;
        cnx->angleLoaded = false;
    }
    cnx->systemDriverUnloaded = true;
}
void* Loader::open(egl_connection_t* cnx) {
    ATRACE_CALL();
    const nsecs_t openTime = systemTime();
    if (cnx->dso && should_unload_system_driver(cnx)) {
        unload_system_driver(cnx);
    }
    if (cnx->dso) {
        return cnx->dso;
    }
    driver_t* hnd = nullptr;
    if (android::GraphicsEnv::getInstance().shouldUseAngle()) {
        hnd = attempt_to_load_angle(cnx);
        LOG_ALWAYS_FATAL_IF(!hnd, "Failed to load ANGLE.");
    }
    if (!hnd) {
        hnd = attempt_to_load_updated_driver(cnx);
        LOG_ALWAYS_FATAL_IF(android::GraphicsEnv::getInstance().getDriverNamespace(),
                            "couldn't find an OpenGL ES implementation from %s",
                            android::GraphicsEnv::getInstance().getDriverPath().c_str());
    }
    if (!hnd && android::GraphicsEnv::getInstance().shouldUseNativeDriver()) {
        auto driverSuffix = base::GetProperty(RO_DRIVER_SUFFIX_PROPERTY, "");
        LOG_ALWAYS_FATAL_IF(driverSuffix.empty(),
                            "Native GLES driver is selected but not specified in %s",
                            RO_DRIVER_SUFFIX_PROPERTY);
        hnd = attempt_to_load_system_driver(cnx, driverSuffix.c_str(), true);
        LOG_ALWAYS_FATAL_IF(!hnd, "Native GLES driver is selected but failed to load. %s=%s",
                            RO_DRIVER_SUFFIX_PROPERTY, driverSuffix.c_str());
    }
    bool failToLoadFromDriverSuffixProperty = false;
    if (!hnd) {
        for (auto key : HAL_SUBNAME_KEY_PROPERTIES) {
            auto prop = base::GetProperty(key, "");
            if (prop.empty()) {
                continue;
            }
            hnd = attempt_to_load_system_driver(cnx, prop.c_str(), true);
            if (!hnd) {
                ALOGD("Failed to load drivers from property %s with value %s", key, prop.c_str());
                failToLoadFromDriverSuffixProperty = true;
            }
            break;
        }
    }
    if (!hnd) {
        hnd = attempt_to_load_system_driver(cnx, nullptr, true);
    }
    if (!hnd && !failToLoadFromDriverSuffixProperty &&
        property_get_int32("ro.vendor.api_level", 0) < __ANDROID_API_U__) {
        hnd = attempt_to_load_system_driver(cnx, nullptr, false);
    }
    if (!hnd) {
        android::GraphicsEnv::getInstance().setDriverLoaded(android::GpuStatsInfo::Api::API_GL,
                                                            false, systemTime() - openTime);
    } else {
        attempt_to_init_angle_backend(hnd->dso[2], cnx);
    }
    LOG_ALWAYS_FATAL_IF(!hnd,
                        "couldn't find an OpenGL ES implementation, make sure one of %s, %s and %s "
                        "is set",
                        HAL_SUBNAME_KEY_PROPERTIES[0], HAL_SUBNAME_KEY_PROPERTIES[1],
                        HAL_SUBNAME_KEY_PROPERTIES[2]);
    if (!cnx->libEgl) {
        cnx->libEgl = load_wrapper(EGL_WRAPPER_DIR "/libEGL.so");
    }
    if (!cnx->libGles1) {
        cnx->libGles1 = load_wrapper(EGL_WRAPPER_DIR "/libGLESv1_CM.so");
    }
    if (!cnx->libGles2) {
        cnx->libGles2 = load_wrapper(EGL_WRAPPER_DIR "/libGLESv2.so");
    }
    if (!cnx->libEgl || !cnx->libGles2 || !cnx->libGles1) {
        android::GraphicsEnv::getInstance().setDriverLoaded(android::GpuStatsInfo::Api::API_GL,
                                                            false, systemTime() - openTime);
    }
    LOG_ALWAYS_FATAL_IF(!cnx->libEgl,
            "couldn't load system EGL wrapper libraries");
    LOG_ALWAYS_FATAL_IF(!cnx->libGles2 || !cnx->libGles1,
                        "couldn't load system OpenGL ES wrapper libraries");
    android::GraphicsEnv::getInstance().setDriverLoaded(android::GpuStatsInfo::Api::API_GL, true,
                                                        systemTime() - openTime);
    return (void*)hnd;
}
void Loader::close(egl_connection_t* cnx)
{
    driver_t* hnd = (driver_t*) cnx->dso;
    delete hnd;
    cnx->dso = nullptr;
    cnx->angleLoaded = false;
}
void Loader::init_api(void* dso,
        char const * const * api,
        char const * const * ref_api,
        __eglMustCastToProperFunctionPointerType* curr,
        getProcAddressType getProcAddress)
{
    ATRACE_CALL();
    const ssize_t SIZE = 256;
    char scrap[SIZE];
    while (*api) {
        char const * name = *api;
        if (ref_api) {
            char const * ref_name = *ref_api;
            if (std::strcmp(name, ref_name) != 0) {
                *curr++ = nullptr;
                ref_api++;
                continue;
            }
        }
        __eglMustCastToProperFunctionPointerType f =
            (__eglMustCastToProperFunctionPointerType)dlsym(dso, name);
        if (f == nullptr) {
            f = getProcAddress(name);
        }
        if (f == nullptr) {
            ssize_t index = ssize_t(strlen(name)) - 3;
            if ((index>0 && (index<SIZE-1)) && (!strcmp(name+index, "OES"))) {
                strncpy(scrap, name, index);
                scrap[index] = 0;
                f = (__eglMustCastToProperFunctionPointerType)dlsym(dso, scrap);
            }
        }
        if (f == nullptr) {
            ssize_t index = ssize_t(strlen(name)) - 3;
            if (index>0 && strcmp(name+index, "OES")) {
                snprintf(scrap, SIZE, "%sOES", name);
                f = (__eglMustCastToProperFunctionPointerType)dlsym(dso, scrap);
            }
        }
        if (f == nullptr) {
            f = (__eglMustCastToProperFunctionPointerType)gl_unimplemented;
            if (!strcmp(name, "glInsertEventMarkerEXT")) {
                f = (__eglMustCastToProperFunctionPointerType)gl_noop;
            } else if (!strcmp(name, "glPushGroupMarkerEXT")) {
                f = (__eglMustCastToProperFunctionPointerType)gl_noop;
            } else if (!strcmp(name, "glPopGroupMarkerEXT")) {
                f = (__eglMustCastToProperFunctionPointerType)gl_noop;
            }
        }
        *curr++ = f;
        api++;
        if (ref_api) ref_api++;
    }
}
static void* load_system_driver(const char* kind, const char* suffix, const bool exact) {
    ATRACE_CALL();
    class MatchFile {
    public:
        static std::string find(const char* libraryName, const bool exact) {
            const char* const searchPaths[] = {
#if defined(__LP64__)
                    "/vendor/lib64/egl",
                    "/system/lib64/egl"
#else
                    "/vendor/lib/egl",
                    "/system/lib/egl"
#endif
            };
            for (auto dir : searchPaths) {
                std::string absolutePath;
                if (find(absolutePath, libraryName, dir, exact)) {
                    return absolutePath;
                }
            }
            return std::string();
        }
    private:
        static bool find(std::string& result,
                const std::string& pattern, const char* const search, bool exact) {
            if (exact) {
                std::string absolutePath = std::string(search) + "/" + pattern + ".so";
                if (!access(absolutePath.c_str(), R_OK)) {
                    result = absolutePath;
                    return true;
                }
                return false;
            }
            DIR* d = opendir(search);
            if (d != nullptr) {
                struct dirent* e;
                while ((e = readdir(d)) != nullptr) {
                    if (e->d_type == DT_DIR) {
                        continue;
                    }
                    if (!strcmp(e->d_name, "libGLES_android.so")) {
                        continue;
                    }
                    if (strstr(e->d_name, pattern.c_str()) == e->d_name) {
                        if (!strcmp(e->d_name + strlen(e->d_name) - 3, ".so")) {
                            result = std::string(search) + "/" + e->d_name;
                            closedir(d);
                            return true;
                        }
                    }
                }
                closedir(d);
            }
            return false;
        }
    };
    std::string libraryName = std::string("lib") + kind;
    if (suffix) {
        libraryName += std::string("_") + suffix;
    } else if (!exact) {
        libraryName += std::string("_");
    }
    std::string absolutePath = MatchFile::find(libraryName.c_str(), exact);
    if (absolutePath.empty()) {
        return nullptr;
    }
    const char* const driver_absolute_path = absolutePath.c_str();
    void* dso = do_android_load_sphal_library(driver_absolute_path,
                                              RTLD_NOW | RTLD_LOCAL);
    if (dso == nullptr) {
        const char* err = dlerror();
        ALOGE("load_driver(%s): %s", driver_absolute_path, err ? err : "unknown");
        return nullptr;
    }
    ALOGD("loaded %s", driver_absolute_path);
    return dso;
}
static void* load_angle(const char* kind, android_namespace_t* ns) {
    const android_dlextinfo dlextinfo = {
            .flags = ANDROID_DLEXT_USE_NAMESPACE,
            .library_namespace = ns,
    };
    std::string name = std::string("lib") + kind + "_angle.so";
    void* so = do_android_dlopen_ext(name.c_str(), RTLD_LOCAL | RTLD_NOW, &dlextinfo);
    if (so) {
        return so;
    } else {
        ALOGE("dlopen_ext(\"%s\") failed: %s", name.c_str(), dlerror());
    }
    return nullptr;
}
static void* load_updated_driver(const char* kind, android_namespace_t* ns) {
    ATRACE_CALL();
    const android_dlextinfo dlextinfo = {
        .flags = ANDROID_DLEXT_USE_NAMESPACE,
        .library_namespace = ns,
    };
    void* so = nullptr;
    for (auto key : HAL_SUBNAME_KEY_PROPERTIES) {
        auto prop = base::GetProperty(key, "");
        if (prop.empty()) {
            continue;
        }
        std::string name = std::string("lib") + kind + "_" + prop + ".so";
        so = do_android_dlopen_ext(name.c_str(), RTLD_LOCAL | RTLD_NOW, &dlextinfo);
        if (so) {
            return so;
        }
        ALOGE("Could not load %s from updatable gfx driver namespace: %s.", name.c_str(),
              dlerror());
    }
    return nullptr;
}
Loader::driver_t* Loader::attempt_to_load_angle(egl_connection_t* cnx) {
    ATRACE_CALL();
    android_namespace_t* ns = android::GraphicsEnv::getInstance().getAngleNamespace();
    if (!ns) {
        return nullptr;
    }
    android::GraphicsEnv::getInstance().setDriverToLoad(android::GpuStatsInfo::Driver::ANGLE);
    driver_t* hnd = nullptr;
    void* dso = load_angle("EGL", ns);
    if (dso) {
        initialize_api(dso, cnx, EGL);
        hnd = new driver_t(dso);
        dso = load_angle("GLESv1_CM", ns);
        initialize_api(dso, cnx, GLESv1_CM);
        hnd->set(dso, GLESv1_CM);
        dso = load_angle("GLESv2", ns);
        initialize_api(dso, cnx, GLESv2);
        hnd->set(dso, GLESv2);
    }
    return hnd;
}
void Loader::attempt_to_init_angle_backend(void* dso, egl_connection_t* cnx) {
    void* pANGLEGetDisplayPlatform = dlsym(dso, "ANGLEGetDisplayPlatform");
    if (pANGLEGetDisplayPlatform) {
        ALOGV("ANGLE GLES library loaded");
        cnx->angleLoaded = true;
    } else {
        ALOGV("Native GLES library loaded");
        cnx->angleLoaded = false;
    }
}
Loader::driver_t* Loader::attempt_to_load_updated_driver(egl_connection_t* cnx) {
    ATRACE_CALL();
    android_namespace_t* ns = android::GraphicsEnv::getInstance().getDriverNamespace();
    if (!ns) {
        return nullptr;
    }
    ALOGD("Load updated gl driver.");
    android::GraphicsEnv::getInstance().setDriverToLoad(android::GpuStatsInfo::Driver::GL_UPDATED);
    driver_t* hnd = nullptr;
    void* dso = load_updated_driver("GLES", ns);
    if (dso) {
        initialize_api(dso, cnx, EGL | GLESv1_CM | GLESv2);
        hnd = new driver_t(dso);
        return hnd;
    }
    dso = load_updated_driver("EGL", ns);
    if (dso) {
        initialize_api(dso, cnx, EGL);
        hnd = new driver_t(dso);
        dso = load_updated_driver("GLESv1_CM", ns);
        initialize_api(dso, cnx, GLESv1_CM);
        hnd->set(dso, GLESv1_CM);
        dso = load_updated_driver("GLESv2", ns);
        initialize_api(dso, cnx, GLESv2);
        hnd->set(dso, GLESv2);
    }
    return hnd;
}
Loader::driver_t* Loader::attempt_to_load_system_driver(egl_connection_t* cnx, const char* suffix,
                                                        const bool exact) {
    ATRACE_CALL();
    android::GraphicsEnv::getInstance().setDriverToLoad(android::GpuStatsInfo::Driver::GL);
    driver_t* hnd = nullptr;
    void* dso = load_system_driver("GLES", suffix, exact);
    if (dso) {
        initialize_api(dso, cnx, EGL | GLESv1_CM | GLESv2);
        hnd = new driver_t(dso);
        return hnd;
    }
    dso = load_system_driver("EGL", suffix, exact);
    if (dso) {
        initialize_api(dso, cnx, EGL);
        hnd = new driver_t(dso);
        dso = load_system_driver("GLESv1_CM", suffix, exact);
        initialize_api(dso, cnx, GLESv1_CM);
        hnd->set(dso, GLESv1_CM);
        dso = load_system_driver("GLESv2", suffix, exact);
        initialize_api(dso, cnx, GLESv2);
        hnd->set(dso, GLESv2);
    }
    return hnd;
}
void Loader::initialize_api(void* dso, egl_connection_t* cnx, uint32_t mask) {
    if (mask & EGL) {
        getProcAddress = (getProcAddressType)dlsym(dso, "eglGetProcAddress");
        ALOGE_IF(!getProcAddress,
                "can't find eglGetProcAddress() in EGL driver library");
        egl_t* egl = &cnx->egl;
        __eglMustCastToProperFunctionPointerType* curr =
            (__eglMustCastToProperFunctionPointerType*)egl;
        char const * const * api = egl_names;
        while (*api) {
            char const * name = *api;
            __eglMustCastToProperFunctionPointerType f =
                (__eglMustCastToProperFunctionPointerType)dlsym(dso, name);
            if (f == nullptr) {
                f = getProcAddress(name);
                if (f == nullptr) {
                    f = (__eglMustCastToProperFunctionPointerType)nullptr;
                }
            }
            *curr++ = f;
            api++;
        }
    }
    if (mask & GLESv1_CM) {
        init_api(dso, gl_names_1, gl_names,
            (__eglMustCastToProperFunctionPointerType*)
                &cnx->hooks[egl_connection_t::GLESv1_INDEX]->gl,
            getProcAddress);
    }
    if (mask & GLESv2) {
        init_api(dso, gl_names, nullptr,
            (__eglMustCastToProperFunctionPointerType*)
                &cnx->hooks[egl_connection_t::GLESv2_INDEX]->gl,
            getProcAddress);
    }
}
}
