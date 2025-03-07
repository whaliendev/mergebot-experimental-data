#ifndef ANDROID_UI_GRAPHICS_ENV_H
#define ANDROID_UI_GRAPHICS_ENV_H 1
#include <graphicsenv/GpuStatsInfo.h>
#include <mutex>
#include <string>
#include <vector>
struct android_namespace_t;
namespace android {
struct NativeLoaderNamespace;
class GraphicsEnv {
public:
    static GraphicsEnv& getInstance();
    bool isDebuggable();
    void setDriverPathAndSphalLibraries(const std::string& path, const std::string& sphalLibraries);
    android_namespace_t* getDriverNamespace();
    std::string getDriverPath() const;
    void hintActivityLaunch();
    void setGpuStats(const std::string& driverPackageName, const std::string& driverVersionName,
                     uint64_t versionCode, int64_t driverBuildTime,
                     const std::string& appPackageName, const int32_t vulkanVersion);
    void setTargetStats(const GpuStatsInfo::Stats stats, const uint64_t value = 0);
    void setTargetStatsArray(const GpuStatsInfo::Stats stats, const uint64_t* values,
                             const uint32_t valueCount);
    void setDriverToLoad(GpuStatsInfo::Driver driver);
    void setDriverLoaded(GpuStatsInfo::Api api, bool isDriverLoaded, int64_t driverLoadingTime);
    void setVulkanInstanceExtensions(uint32_t enabledExtensionCount,
                                     const char* const* ppEnabledExtensionNames);
    void setVulkanDeviceExtensions(uint32_t enabledExtensionCount,
                                   const char* const* ppEnabledExtensionNames);
    bool setInjectLayersPrSetDumpable();
    bool shouldUseAngle();
<<<<<<< HEAD
    void setAngleInfo(const std::string& path, const bool shouldUseNativeDriver,
                      const std::string& packageName, const std::vector<std::string> eglFeatures);
||||||| e631a0223f
    void setAngleInfo(const std::string path, const std::string appName, std::string devOptIn,
                      const std::vector<std::string> eglFeatures);
=======
    void setAngleInfo(const std::string& path, const std::string& packageName,
                      const std::string& devOptIn, const std::vector<std::string> eglFeatures);
>>>>>>> e20d8548
    android_namespace_t* getAngleNamespace();
    std::string& getPackageName();
    const std::vector<std::string>& getAngleEglFeatures();
<<<<<<< HEAD
    void nativeToggleAngleAsSystemDriver(bool enabled);
    bool shouldUseSystemAngle();
    bool shouldUseNativeDriver();
||||||| e631a0223f
=======
    void nativeToggleAngleAsSystemDriver(bool enabled);
>>>>>>> e20d8548
    void setLayerPaths(NativeLoaderNamespace* appNamespace, const std::string& layerPaths);
    NativeLoaderNamespace* getAppNamespace();
    const std::string& getLayerPaths();
    void setDebugLayers(const std::string& layers);
    void setDebugLayersGLES(const std::string& layers);
    const std::string& getDebugLayers();
    const std::string& getDebugLayersGLES();
private:
<<<<<<< HEAD
||||||| e631a0223f
    enum UseAngle { UNKNOWN, YES, NO };
    void* loadLibrary(std::string name);
    void updateUseAngle();
=======
    enum UseAngle { UNKNOWN, YES, NO };
    void updateShouldUseAngle();
>>>>>>> e20d8548
    bool linkDriverNamespaceLocked(android_namespace_t* destNamespace,
                                   android_namespace_t* vndkNamespace,
                                   const std::string& sharedSphalLibraries);
    bool readyToSendGpuStatsLocked();
    void sendGpuStatsLocked(GpuStatsInfo::Api api, bool isDriverLoaded, int64_t driverLoadingTime);
    GraphicsEnv() = default;
    std::mutex mNamespaceMutex;
    std::string mDriverPath;
    std::string mSphalLibraries;
<<<<<<< HEAD
    android_namespace_t* mDriverNamespace = nullptr;
    std::string mAnglePath;
    std::string mPackageName;
    std::vector<std::string> mAngleEglFeatures;
    bool mShouldUseAngle = false;
    bool mShouldUseSystemAngle = false;
    bool mShouldUseNativeDriver = false;
    android_namespace_t* mAngleNamespace = nullptr;
||||||| e631a0223f
=======
    android_namespace_t* mDriverNamespace = nullptr;
    std::string mAnglePath;
    std::string mPackageName;
    std::string mAngleDeveloperOptIn;
    std::vector<std::string> mAngleEglFeatures;
    UseAngle mShouldUseAngle = UNKNOWN;
    android_namespace_t* mAngleNamespace = nullptr;
>>>>>>> e20d8548
    std::mutex mStatsLock;
    bool mActivityLaunched = false;
    GpuStatsInfo mGpuStats;
    std::string mDebugLayers;
    std::string mDebugLayersGLES;
    std::string mLayerPaths;
    NativeLoaderNamespace* mAppNamespace = nullptr;
};
}
#endif
