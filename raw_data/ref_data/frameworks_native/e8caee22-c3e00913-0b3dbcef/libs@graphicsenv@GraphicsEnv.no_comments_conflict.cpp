#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#define LOG_TAG "GraphicsEnv"
#include <graphicsenv/GraphicsEnv.h>
#include <dlfcn.h>
#include <unistd.h>
#include <android-base/file.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android/dlext.h>
#include <binder/IServiceManager.h>
#include <graphicsenv/IGpuService.h>
#include <log/log.h>
#include <nativeloader/dlext_namespaces.h>
#include <sys/prctl.h>
#include <utils/Trace.h>
#include <memory>
#include <string>
#include <thread>
#if defined(__LP64__)
#define UPDATABLE_DRIVER_ABI "arm64-v8a"
#else
#define UPDATABLE_DRIVER_ABI "armeabi-v7a"
#endif
#define CURRENT_ANGLE_API_VERSION 2
typedef bool (*fpANGLEGetFeatureSupportUtilAPIVersion)(unsigned int* versionToUse);
typedef bool (*fpANGLEAndroidParseRulesString)(const char* rulesString, void** rulesHandle,
                                               int* rulesVersion);
typedef bool (*fpANGLEGetSystemInfo)(void** handle);
typedef bool (*fpANGLEAddDeviceInfoToSystemInfo)(const char* deviceMfr, const char* deviceModel,
                                                 void* handle);
typedef bool (*fpANGLEShouldBeUsedForApplication)(void* rulesHandle, int rulesVersion,
                                                  void* systemInfoHandle, const char* appName);
typedef bool (*fpANGLEFreeRulesHandle)(void* handle);
typedef bool (*fpANGLEFreeSystemInfoHandle)(void* handle);
namespace {
static bool isVndkEnabled() {
#ifdef __BIONIC__
    static bool isVndkEnabled = android::base::GetProperty("ro.vndk.version", "") != "";
    return isVndkEnabled;
#endif
    return false;
}
}
namespace android {
enum NativeLibrary {
    LLNDK = 0,
    VNDKSP = 1,
};
static constexpr const char* kNativeLibrariesSystemConfigPath[] =
        {"/apex/com.android.vndk.v{}/etc/llndk.libraries.{}.txt",
         "/apex/com.android.vndk.v{}/etc/vndksp.libraries.{}.txt"};
static const char* kLlndkLibrariesTxtPath = "/system/etc/llndk.libraries.txt";
static std::string vndkVersionStr() {
#ifdef __BIONIC__
    return base::GetProperty("ro.vndk.version", "");
#endif
    return "";
}
static void insertVndkVersionStr(std::string* fileName) {
    LOG_ALWAYS_FATAL_IF(!fileName, "fileName should never be nullptr");
    std::string version = vndkVersionStr();
    size_t pos = fileName->find("{}");
    while (pos != std::string::npos) {
        fileName->replace(pos, 2, version);
        pos = fileName->find("{}", pos + version.size());
    }
}
static bool readConfig(const std::string& configFile, std::vector<std::string>* soNames) {
    std::string fileContent;
    if (!base::ReadFileToString(configFile, &fileContent)) {
        return false;
    }
    std::vector<std::string> lines = base::Split(fileContent, "\n");
    for (auto& line : lines) {
        auto trimmedLine = base::Trim(line);
        if (!trimmedLine.empty()) {
            soNames->push_back(trimmedLine);
        }
    }
    return true;
}
static const std::string getSystemNativeLibraries(NativeLibrary type) {
    std::string nativeLibrariesSystemConfig = "";
    if (!isVndkEnabled() && type == NativeLibrary::LLNDK) {
        nativeLibrariesSystemConfig = kLlndkLibrariesTxtPath;
    } else {
        nativeLibrariesSystemConfig = kNativeLibrariesSystemConfigPath[type];
        insertVndkVersionStr(&nativeLibrariesSystemConfig);
    }
    std::vector<std::string> soNames;
    if (!readConfig(nativeLibrariesSystemConfig, &soNames)) {
        ALOGE("Failed to retrieve library names from %s", nativeLibrariesSystemConfig.c_str());
        return "";
    }
    return base::Join(soNames, ':');
}
static sp<IGpuService> getGpuService() {
    static const sp<IBinder> binder = defaultServiceManager()->checkService(String16("gpu"));
    if (!binder) {
        ALOGE("Failed to get gpu service");
        return nullptr;
    }
    return interface_cast<IGpuService>(binder);
}
           GraphicsEnv& GraphicsEnv::getInstance() {
    static GraphicsEnv env;
    return env;
}
bool GraphicsEnv::isDebuggable() {
    bool appDebuggable = prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) > 0;
#if defined(ANDROID_DEBUGGABLE)
    bool platformDebuggable = true;
#else
    bool platformDebuggable = false;
#endif
    ALOGV("GraphicsEnv::isDebuggable returning appDebuggable=%s || platformDebuggable=%s",
          appDebuggable ? "true" : "false", platformDebuggable ? "true" : "false");
    return appDebuggable || platformDebuggable;
}
void GraphicsEnv::setDriverPathAndSphalLibraries(const std::string& path,
                                                 const std::string& sphalLibraries) {
    if (!mDriverPath.empty() || !mSphalLibraries.empty()) {
        ALOGV("ignoring attempt to change driver path from '%s' to '%s' or change sphal libraries "
              "from '%s' to '%s'",
              mDriverPath.c_str(), path.c_str(), mSphalLibraries.c_str(), sphalLibraries.c_str());
        return;
    }
    ALOGV("setting driver path to '%s' and sphal libraries to '%s'", path.c_str(),
          sphalLibraries.c_str());
    mDriverPath = path;
    mSphalLibraries = sphalLibraries;
}
bool GraphicsEnv::linkDriverNamespaceLocked(android_namespace_t* destNamespace,
                                            android_namespace_t* vndkNamespace,
                                            const std::string& sharedSphalLibraries) {
    const std::string llndkLibraries = getSystemNativeLibraries(NativeLibrary::LLNDK);
    if (llndkLibraries.empty()) {
        return false;
    }
    if (!android_link_namespaces(destNamespace, nullptr, llndkLibraries.c_str())) {
        ALOGE("Failed to link default namespace[%s]", dlerror());
        return false;
    }
    const std::string vndkspLibraries = getSystemNativeLibraries(NativeLibrary::VNDKSP);
    if (vndkspLibraries.empty()) {
        return false;
    }
    if (!android_link_namespaces(destNamespace, vndkNamespace, vndkspLibraries.c_str())) {
        ALOGE("Failed to link vndk namespace[%s]", dlerror());
        return false;
    }
    if (sharedSphalLibraries.empty()) {
        return true;
    }
    auto sphalNamespace = android_get_exported_namespace("sphal");
    if (!sphalNamespace) {
        ALOGE("Depend on these libraries[%s] in sphal, but failed to get sphal namespace",
              sharedSphalLibraries.c_str());
        return false;
    }
    if (!android_link_namespaces(destNamespace, sphalNamespace, sharedSphalLibraries.c_str())) {
        ALOGE("Failed to link sphal namespace[%s]", dlerror());
        return false;
    }
    return true;
}
android_namespace_t* GraphicsEnv::getDriverNamespace() {
    std::lock_guard<std::mutex> lock(mNamespaceMutex);
    if (mDriverNamespace) {
        return mDriverNamespace;
    }
    if (mDriverPath.empty()) {
        const char* id = getenv("UPDATABLE_GFX_DRIVER");
        if (id == nullptr || std::strcmp(id, "1") != 0) {
            return nullptr;
        }
        const sp<IGpuService> gpuService = getGpuService();
        if (!gpuService) {
            return nullptr;
        }
        mDriverPath = gpuService->getUpdatableDriverPath();
        if (mDriverPath.empty()) {
            return nullptr;
        }
        mDriverPath.append(UPDATABLE_DRIVER_ABI);
        ALOGI("Driver path is setup via UPDATABLE_GFX_DRIVER: %s", mDriverPath.c_str());
    }
    auto vndkNamespace = android_get_exported_namespace("vndk");
    if (!vndkNamespace) {
        return nullptr;
    }
    mDriverNamespace = android_create_namespace("updatable gfx driver",
                                                mDriverPath.c_str(),
                                                mDriverPath.c_str(),
                                                ANDROID_NAMESPACE_TYPE_ISOLATED,
                                                nullptr,
                                                nullptr);
    if (!linkDriverNamespaceLocked(mDriverNamespace, vndkNamespace, mSphalLibraries)) {
        mDriverNamespace = nullptr;
    }
    return mDriverNamespace;
}
std::string GraphicsEnv::getDriverPath() const {
    return mDriverPath;
}
void GraphicsEnv::hintActivityLaunch() {
    ATRACE_CALL();
    {
        std::lock_guard<std::mutex> lock(mStatsLock);
        if (mActivityLaunched) return;
        mActivityLaunched = true;
    }
    std::thread trySendGpuStatsThread([this]() {
        std::lock_guard<std::mutex> lock(mStatsLock);
        if (mGpuStats.glDriverToSend) {
            mGpuStats.glDriverToSend = false;
            sendGpuStatsLocked(GpuStatsInfo::Api::API_GL, true, mGpuStats.glDriverLoadingTime);
        }
        if (mGpuStats.vkDriverToSend) {
            mGpuStats.vkDriverToSend = false;
            sendGpuStatsLocked(GpuStatsInfo::Api::API_VK, true, mGpuStats.vkDriverLoadingTime);
        }
    });
    trySendGpuStatsThread.detach();
}
void GraphicsEnv::setGpuStats(const std::string& driverPackageName,
                              const std::string& driverVersionName, uint64_t driverVersionCode,
                              int64_t driverBuildTime, const std::string& appPackageName,
                              const int vulkanVersion) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> lock(mStatsLock);
    ALOGV("setGpuStats:\n"
          "\tdriverPackageName[%s]\n"
          "\tdriverVersionName[%s]\n"
          "\tdriverVersionCode[%" PRIu64 "]\n"
          "\tdriverBuildTime[%" PRId64 "]\n"
          "\tappPackageName[%s]\n"
          "\tvulkanVersion[%d]\n",
          driverPackageName.c_str(), driverVersionName.c_str(), driverVersionCode, driverBuildTime,
          appPackageName.c_str(), vulkanVersion);
    mGpuStats.driverPackageName = driverPackageName;
    mGpuStats.driverVersionName = driverVersionName;
    mGpuStats.driverVersionCode = driverVersionCode;
    mGpuStats.driverBuildTime = driverBuildTime;
    mGpuStats.appPackageName = appPackageName;
    mGpuStats.vulkanVersion = vulkanVersion;
}
void GraphicsEnv::setDriverToLoad(GpuStatsInfo::Driver driver) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> lock(mStatsLock);
    switch (driver) {
        case GpuStatsInfo::Driver::GL:
        case GpuStatsInfo::Driver::GL_UPDATED:
        case GpuStatsInfo::Driver::ANGLE: {
            if (mGpuStats.glDriverToLoad == GpuStatsInfo::Driver::NONE ||
                mGpuStats.glDriverToLoad == GpuStatsInfo::Driver::GL) {
                mGpuStats.glDriverToLoad = driver;
                break;
            }
            if (mGpuStats.glDriverFallback == GpuStatsInfo::Driver::NONE) {
                mGpuStats.glDriverFallback = driver;
            }
            break;
        }
        case GpuStatsInfo::Driver::VULKAN:
        case GpuStatsInfo::Driver::VULKAN_UPDATED: {
            if (mGpuStats.vkDriverToLoad == GpuStatsInfo::Driver::NONE ||
                mGpuStats.vkDriverToLoad == GpuStatsInfo::Driver::VULKAN) {
                mGpuStats.vkDriverToLoad = driver;
                break;
            }
            if (mGpuStats.vkDriverFallback == GpuStatsInfo::Driver::NONE) {
                mGpuStats.vkDriverFallback = driver;
            }
            break;
        }
        default:
            break;
    }
}
void GraphicsEnv::setDriverLoaded(GpuStatsInfo::Api api, bool isDriverLoaded,
                                  int64_t driverLoadingTime) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> lock(mStatsLock);
    if (api == GpuStatsInfo::Api::API_GL) {
        mGpuStats.glDriverToSend = true;
        mGpuStats.glDriverLoadingTime = driverLoadingTime;
    } else {
        mGpuStats.vkDriverToSend = true;
        mGpuStats.vkDriverLoadingTime = driverLoadingTime;
    }
    sendGpuStatsLocked(api, isDriverLoaded, driverLoadingTime);
}
static uint64_t calculateExtensionHash(const char* word) {
    if (!word) {
        return 0;
    }
    const size_t wordLen = strlen(word);
    const uint32_t seed = 167;
    uint64_t hash = 0;
    for (size_t i = 0; i < wordLen; i++) {
        hash = (hash * seed) + word[i];
    }
    return hash;
}
void GraphicsEnv::setVulkanInstanceExtensions(uint32_t enabledExtensionCount,
                                              const char* const* ppEnabledExtensionNames) {
    ATRACE_CALL();
    if (enabledExtensionCount == 0 || ppEnabledExtensionNames == nullptr) {
        return;
    }
    const uint32_t maxNumStats = android::GpuStatsAppInfo::MAX_NUM_EXTENSIONS;
    uint64_t extensionHashes[maxNumStats];
    const uint32_t numStats = std::min(enabledExtensionCount, maxNumStats);
    for(uint32_t i = 0; i < numStats; i++) {
        extensionHashes[i] = calculateExtensionHash(ppEnabledExtensionNames[i]);
    }
    setTargetStatsArray(android::GpuStatsInfo::Stats::VULKAN_INSTANCE_EXTENSION,
                        extensionHashes, numStats);
}
void GraphicsEnv::setVulkanDeviceExtensions(uint32_t enabledExtensionCount,
                                            const char* const* ppEnabledExtensionNames) {
    ATRACE_CALL();
    if (enabledExtensionCount == 0 || ppEnabledExtensionNames == nullptr) {
        return;
    }
    const uint32_t maxNumStats = android::GpuStatsAppInfo::MAX_NUM_EXTENSIONS;
    uint64_t extensionHashes[maxNumStats];
    const uint32_t numStats = std::min(enabledExtensionCount, maxNumStats);
    for(uint32_t i = 0; i < numStats; i++) {
        extensionHashes[i] = calculateExtensionHash(ppEnabledExtensionNames[i]);
    }
    setTargetStatsArray(android::GpuStatsInfo::Stats::VULKAN_DEVICE_EXTENSION,
                        extensionHashes, numStats);
}
bool GraphicsEnv::readyToSendGpuStatsLocked() {
    return mActivityLaunched && !mGpuStats.appPackageName.empty();
}
void GraphicsEnv::setTargetStats(const GpuStatsInfo::Stats stats, const uint64_t value) {
    return setTargetStatsArray(stats, &value, 1);
}
void GraphicsEnv::setTargetStatsArray(const GpuStatsInfo::Stats stats, const uint64_t* values,
                                      const uint32_t valueCount) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> lock(mStatsLock);
    if (!readyToSendGpuStatsLocked()) return;
    const sp<IGpuService> gpuService = getGpuService();
    if (gpuService) {
        gpuService->setTargetStatsArray(mGpuStats.appPackageName, mGpuStats.driverVersionCode,
                                        stats, values, valueCount);
    }
}
void GraphicsEnv::sendGpuStatsLocked(GpuStatsInfo::Api api, bool isDriverLoaded,
                                     int64_t driverLoadingTime) {
    ATRACE_CALL();
    if (!readyToSendGpuStatsLocked()) return;
    ALOGV("sendGpuStats:\n"
          "\tdriverPackageName[%s]\n"
          "\tdriverVersionName[%s]\n"
          "\tdriverVersionCode[%" PRIu64 "]\n"
          "\tdriverBuildTime[%" PRId64 "]\n"
          "\tappPackageName[%s]\n"
          "\tvulkanVersion[%d]\n"
          "\tapi[%d]\n"
          "\tisDriverLoaded[%d]\n"
          "\tdriverLoadingTime[%" PRId64 "]",
          mGpuStats.driverPackageName.c_str(), mGpuStats.driverVersionName.c_str(),
          mGpuStats.driverVersionCode, mGpuStats.driverBuildTime, mGpuStats.appPackageName.c_str(),
          mGpuStats.vulkanVersion, static_cast<int32_t>(api), isDriverLoaded, driverLoadingTime);
    GpuStatsInfo::Driver driver = GpuStatsInfo::Driver::NONE;
    bool isIntendedDriverLoaded = false;
    if (api == GpuStatsInfo::Api::API_GL) {
        driver = mGpuStats.glDriverToLoad;
        isIntendedDriverLoaded =
                isDriverLoaded && (mGpuStats.glDriverFallback == GpuStatsInfo::Driver::NONE);
    } else {
        driver = mGpuStats.vkDriverToLoad;
        isIntendedDriverLoaded =
                isDriverLoaded && (mGpuStats.vkDriverFallback == GpuStatsInfo::Driver::NONE);
    }
    const sp<IGpuService> gpuService = getGpuService();
    if (gpuService) {
        gpuService->setGpuStats(mGpuStats.driverPackageName, mGpuStats.driverVersionName,
                                mGpuStats.driverVersionCode, mGpuStats.driverBuildTime,
                                mGpuStats.appPackageName, mGpuStats.vulkanVersion, driver,
                                isIntendedDriverLoaded, driverLoadingTime);
    }
}
bool GraphicsEnv::setInjectLayersPrSetDumpable() {
    if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) == -1) {
        return false;
    }
    return true;
}
bool GraphicsEnv::shouldUseAngle() {
    if (mPackageName.empty()) {
        ALOGV("Package name is empty. setAngleInfo() has not been called to enable ANGLE.");
        return false;
    }
    return mShouldUseAngle;
}
void GraphicsEnv::setAngleInfo(const std::string& path, const bool shouldUseNativeDriver,
                               const std::string& packageName,
                               const std::vector<std::string> eglFeatures) {
    if (mShouldUseAngle) {
        ALOGE("ANGLE is already set for %s", packageName.c_str());
        return;
    }
    mAngleEglFeatures = std::move(eglFeatures);
    ALOGV("setting ANGLE path to '%s'", path.c_str());
    mAnglePath = std::move(path);
    ALOGV("setting app package name to '%s'", packageName.c_str());
    mPackageName = std::move(packageName);
<<<<<<< HEAD
    if (mAnglePath == "system") {
        mShouldUseSystemAngle = true;
||||||| 0b3dbcefb2
    mShouldUseAngle = true;
    mShouldUseSystemAngle = shouldUseSystemAngle;
}
void GraphicsEnv::setLayerPaths(NativeLoaderNamespace* appNamespace,
                                const std::string& layerPaths) {
    if (mLayerPaths.empty()) {
        mLayerPaths = layerPaths;
        mAppNamespace = appNamespace;
    } else {
        ALOGV("Vulkan layer search path already set, not clobbering with '%s' for namespace %p'",
              layerPaths.c_str(), appNamespace);
=======
    if (path == "system") {
        mShouldUseSystemAngle = true;
    }
    if (!path.empty()) {
        mShouldUseAngle = true;
    }
    mShouldUseNativeDriver = shouldUseNativeDriver;
}
void GraphicsEnv::setLayerPaths(NativeLoaderNamespace* appNamespace,
                                const std::string& layerPaths) {
    if (mLayerPaths.empty()) {
        mLayerPaths = layerPaths;
        mAppNamespace = appNamespace;
    } else {
        ALOGV("Vulkan layer search path already set, not clobbering with '%s' for namespace %p'",
              layerPaths.c_str(), appNamespace);
>>>>>>> c3e00913
    }
    if (!mAnglePath.empty()) {
        mShouldUseAngle = true;
    }
    mShouldUseNativeDriver = shouldUseNativeDriver;
}
std::string& GraphicsEnv::getPackageName() {
    return mPackageName;
}
const std::vector<std::string>& GraphicsEnv::getAngleEglFeatures() {
    return mAngleEglFeatures;
}
<<<<<<< HEAD
||||||| 0b3dbcefb2
const std::string& GraphicsEnv::getLayerPaths() {
    return mLayerPaths;
}
const std::string& GraphicsEnv::getDebugLayers() {
    return mDebugLayers;
}
const std::string& GraphicsEnv::getDebugLayersGLES() {
    return mDebugLayersGLES;
}
void GraphicsEnv::setDebugLayers(const std::string& layers) {
    mDebugLayers = layers;
}
void GraphicsEnv::setDebugLayersGLES(const std::string& layers) {
    mDebugLayersGLES = layers;
}
bool GraphicsEnv::linkDriverNamespaceLocked(android_namespace_t* destNamespace,
                                            android_namespace_t* vndkNamespace,
                                            const std::string& sharedSphalLibraries) {
    const std::string llndkLibraries = getSystemNativeLibraries(NativeLibrary::LLNDK);
    if (llndkLibraries.empty()) {
        return false;
    }
    if (!android_link_namespaces(destNamespace, nullptr, llndkLibraries.c_str())) {
        ALOGE("Failed to link default namespace[%s]", dlerror());
        return false;
    }
    const std::string vndkspLibraries = getSystemNativeLibraries(NativeLibrary::VNDKSP);
    if (vndkspLibraries.empty()) {
        return false;
    }
    if (!android_link_namespaces(destNamespace, vndkNamespace, vndkspLibraries.c_str())) {
        ALOGE("Failed to link vndk namespace[%s]", dlerror());
        return false;
    }
    if (sharedSphalLibraries.empty()) {
        return true;
    }
    auto sphalNamespace = android_get_exported_namespace("sphal");
    if (!sphalNamespace) {
        ALOGE("Depend on these libraries[%s] in sphal, but failed to get sphal namespace",
              sharedSphalLibraries.c_str());
        return false;
    }
    if (!android_link_namespaces(destNamespace, sphalNamespace, sharedSphalLibraries.c_str())) {
        ALOGE("Failed to link sphal namespace[%s]", dlerror());
        return false;
    }
    return true;
}
android_namespace_t* GraphicsEnv::getDriverNamespace() {
    std::lock_guard<std::mutex> lock(mNamespaceMutex);
    if (mDriverNamespace) {
        return mDriverNamespace;
    }
    if (mDriverPath.empty()) {
        const char* id = getenv("UPDATABLE_GFX_DRIVER");
        if (id == nullptr || std::strcmp(id, "1")) {
            return nullptr;
        }
        const sp<IGpuService> gpuService = getGpuService();
        if (!gpuService) {
            return nullptr;
        }
        mDriverPath = gpuService->getUpdatableDriverPath();
        if (mDriverPath.empty()) {
            return nullptr;
        }
        mDriverPath.append(UPDATABLE_DRIVER_ABI);
        ALOGI("Driver path is setup via UPDATABLE_GFX_DRIVER: %s", mDriverPath.c_str());
    }
    auto vndkNamespace = android_get_exported_namespace("vndk");
    if (!vndkNamespace) {
        return nullptr;
    }
    mDriverNamespace = android_create_namespace("gfx driver",
                                                mDriverPath.c_str(),
                                                mDriverPath.c_str(),
                                                ANDROID_NAMESPACE_TYPE_ISOLATED,
                                                nullptr,
                                                nullptr);
    if (!linkDriverNamespaceLocked(mDriverNamespace, vndkNamespace, mSphalLibraries)) {
        mDriverNamespace = nullptr;
    }
    return mDriverNamespace;
}
std::string GraphicsEnv::getDriverPath() const {
    return mDriverPath;
}
=======
const std::string& GraphicsEnv::getLayerPaths() {
    return mLayerPaths;
}
const std::string& GraphicsEnv::getDebugLayers() {
    return mDebugLayers;
}
const std::string& GraphicsEnv::getDebugLayersGLES() {
    return mDebugLayersGLES;
}
void GraphicsEnv::setDebugLayers(const std::string& layers) {
    mDebugLayers = layers;
}
void GraphicsEnv::setDebugLayersGLES(const std::string& layers) {
    mDebugLayersGLES = layers;
}
bool GraphicsEnv::linkDriverNamespaceLocked(android_namespace_t* destNamespace,
                                            android_namespace_t* vndkNamespace,
                                            const std::string& sharedSphalLibraries) {
    const std::string llndkLibraries = getSystemNativeLibraries(NativeLibrary::LLNDK);
    if (llndkLibraries.empty()) {
        return false;
    }
    if (!android_link_namespaces(destNamespace, nullptr, llndkLibraries.c_str())) {
        ALOGE("Failed to link default namespace[%s]", dlerror());
        return false;
    }
    const std::string vndkspLibraries = getSystemNativeLibraries(NativeLibrary::VNDKSP);
    if (vndkspLibraries.empty()) {
        return false;
    }
    if (!android_link_namespaces(destNamespace, vndkNamespace, vndkspLibraries.c_str())) {
        ALOGE("Failed to link vndk namespace[%s]", dlerror());
        return false;
    }
    if (sharedSphalLibraries.empty()) {
        return true;
    }
    auto sphalNamespace = android_get_exported_namespace("sphal");
    if (!sphalNamespace) {
        ALOGE("Depend on these libraries[%s] in sphal, but failed to get sphal namespace",
              sharedSphalLibraries.c_str());
        return false;
    }
    if (!android_link_namespaces(destNamespace, sphalNamespace, sharedSphalLibraries.c_str())) {
        ALOGE("Failed to link sphal namespace[%s]", dlerror());
        return false;
    }
    return true;
}
android_namespace_t* GraphicsEnv::getDriverNamespace() {
    std::lock_guard<std::mutex> lock(mNamespaceMutex);
    if (mDriverNamespace) {
        return mDriverNamespace;
    }
    if (mDriverPath.empty()) {
        const char* id = getenv("UPDATABLE_GFX_DRIVER");
        if (id == nullptr || std::strcmp(id, "1")) {
            return nullptr;
        }
        const sp<IGpuService> gpuService = getGpuService();
        if (!gpuService) {
            return nullptr;
        }
        mDriverPath = gpuService->getUpdatableDriverPath();
        if (mDriverPath.empty()) {
            return nullptr;
        }
        mDriverPath.append(UPDATABLE_DRIVER_ABI);
        ALOGI("Driver path is setup via UPDATABLE_GFX_DRIVER: %s", mDriverPath.c_str());
    }
    auto vndkNamespace = android_get_exported_namespace("vndk");
    if (!vndkNamespace) {
        return nullptr;
    }
    mDriverNamespace = android_create_namespace("updatable gfx driver",
                                                mDriverPath.c_str(),
                                                mDriverPath.c_str(),
                                                ANDROID_NAMESPACE_TYPE_ISOLATED,
                                                nullptr,
                                                nullptr);
    if (!linkDriverNamespaceLocked(mDriverNamespace, vndkNamespace, mSphalLibraries)) {
        mDriverNamespace = nullptr;
    }
    return mDriverNamespace;
}
std::string GraphicsEnv::getDriverPath() const {
    return mDriverPath;
}
>>>>>>> c3e00913
android_namespace_t* GraphicsEnv::getAngleNamespace() {
    std::lock_guard<std::mutex> lock(mNamespaceMutex);
    if (mAngleNamespace) {
        return mAngleNamespace;
    }
    if (mAnglePath.empty() && !mShouldUseSystemAngle) {
        ALOGV("mAnglePath is empty and not using system ANGLE, abort creating ANGLE namespace");
        return nullptr;
    }
    const char* const defaultLibraryPaths =
#if defined(__LP64__)
            "/vendor/lib64/egl:/system/lib64/egl";
#else
            "/vendor/lib/egl:/system/lib/egl";
#endif
    mAngleNamespace =
            android_create_namespace("ANGLE",
                                     mShouldUseSystemAngle ? defaultLibraryPaths
                                                           : mAnglePath.c_str(),
                                     mShouldUseSystemAngle
                                             ? defaultLibraryPaths
                                             : mAnglePath.c_str(),
                                     ANDROID_NAMESPACE_TYPE_SHARED_ISOLATED,
                                     nullptr,
                                     mShouldUseSystemAngle ? android_get_exported_namespace("sphal")
                                                           : nullptr);
    ALOGD_IF(!mAngleNamespace, "Could not create ANGLE namespace from default");
    if (!mShouldUseSystemAngle) {
        return mAngleNamespace;
    }
    auto vndkNamespace = android_get_exported_namespace("vndk");
    if (!vndkNamespace) {
        return nullptr;
    }
    if (!linkDriverNamespaceLocked(mAngleNamespace, vndkNamespace, "")) {
        mAngleNamespace = nullptr;
    }
    return mAngleNamespace;
}
void GraphicsEnv::nativeToggleAngleAsSystemDriver(bool enabled) {
    const sp<IGpuService> gpuService = getGpuService();
    if (!gpuService) {
        ALOGE("No GPU service");
        return;
    }
    gpuService->toggleAngleAsSystemDriver(enabled);
}
bool GraphicsEnv::shouldUseSystemAngle() {
    return mShouldUseSystemAngle;
}
<<<<<<< HEAD
bool GraphicsEnv::shouldUseNativeDriver() {
    return mShouldUseNativeDriver;
}
void GraphicsEnv::setLayerPaths(NativeLoaderNamespace* appNamespace,
                                const std::string& layerPaths) {
    if (mLayerPaths.empty()) {
        mLayerPaths = layerPaths;
        mAppNamespace = appNamespace;
    } else {
        ALOGV("Vulkan layer search path already set, not clobbering with '%s' for namespace %p'",
              layerPaths.c_str(), appNamespace);
    }
}
NativeLoaderNamespace* GraphicsEnv::getAppNamespace() {
    return mAppNamespace;
}
const std::string& GraphicsEnv::getLayerPaths() {
    return mLayerPaths;
}
const std::string& GraphicsEnv::getDebugLayers() {
    return mDebugLayers;
}
const std::string& GraphicsEnv::getDebugLayersGLES() {
    return mDebugLayersGLES;
}
void GraphicsEnv::setDebugLayers(const std::string& layers) {
    mDebugLayers = layers;
}
void GraphicsEnv::setDebugLayersGLES(const std::string& layers) {
    mDebugLayersGLES = layers;
}
||||||| 0b3dbcefb2
=======
bool GraphicsEnv::shouldUseNativeDriver() {
    return mShouldUseNativeDriver;
}
>>>>>>> c3e00913
}
