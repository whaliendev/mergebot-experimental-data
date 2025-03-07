#ifndef ANDROID_SERVERS_CAMERA_SESSION_CONFIGURATION_UTILS_H
#define ANDROID_SERVERS_CAMERA_SESSION_CONFIGURATION_UTILS_H 
#include <android/hardware/camera2/BnCameraDeviceUser.h>
#include <android/hardware/camera2/ICameraDeviceCallbacks.h>
#include <camera/camera2/OutputConfiguration.h>
#include <camera/camera2/SessionConfiguration.h>
#include <camera/camera2/SubmitInfo.h>
#include <camera/StringUtils.h>
#include <aidl/android/hardware/camera/device/ICameraDevice.h>
#include <android/hardware/camera/device/3.4/ICameraDeviceSession.h>
#include <android/hardware/camera/device/3.7/ICameraDeviceSession.h>
#include <device3/Camera3StreamInterface.h>
#include <utils/IPCTransport.h>
#include <set>
#include <stdint.h>
#include "SessionConfigurationUtilsHost.h"
#define STATUS_ERROR(errorCode,errorString) \
    binder::Status::fromServiceSpecificError( \
            errorCode, fmt::sprintf("%s:%d: %s", __FUNCTION__, __LINE__, errorString).c_str())
#define STATUS_ERROR_FMT(errorCode,errorString,...) \
    binder::Status::fromServiceSpecificError( \
            errorCode, \
            fmt::sprintf("%s:%d: " errorString, __FUNCTION__, __LINE__, __VA_ARGS__).c_str())
namespace android {
namespace camera3 {
typedef enum camera_request_template {
    CAMERA_TEMPLATE_PREVIEW = 1,
    CAMERA_TEMPLATE_STILL_CAPTURE = 2,
    CAMERA_TEMPLATE_VIDEO_RECORD = 3,
    CAMERA_TEMPLATE_VIDEO_SNAPSHOT = 4,
    CAMERA_TEMPLATE_ZERO_SHUTTER_LAG = 5,
    CAMERA_TEMPLATE_MANUAL = 6,
    CAMERA_TEMPLATE_COUNT,
    CAMERA_VENDOR_TEMPLATE_START = 0x40000000
} camera_request_template_t;
typedef std::function<CameraMetadata(const std::string&, bool overrideForPerfClass)> metadataGetter;
class StreamConfiguration {
  public:
    int32_t format;
    int32_t width;
    int32_t height;
    int32_t isInput;
    static void getStreamConfigurations(
            const CameraMetadata& static_info, bool maxRes,
            std::unordered_map<int, std::vector<StreamConfiguration>>* scm);
    static void getStreamConfigurations(
            const CameraMetadata& static_info, int configuration,
            std::unordered_map<int, std::vector<StreamConfiguration>>* scm);
};
struct StreamConfigurationPair {
    std::unordered_map<int, std::vector<camera3::StreamConfiguration>>
            mDefaultStreamConfigurationMap;
    std::unordered_map<int, std::vector<camera3::StreamConfiguration>>
            mMaximumResolutionStreamConfigurationMap;
};
namespace SessionConfigurationUtils {
camera3::Size getMaxJpegResolution(const CameraMetadata& metadata, bool ultraHighResolution);
size_t getUHRMaxJpegBufferSize(camera3::Size uhrMaxJpegSize, camera3::Size defaultMaxJpegSize,
                               size_t defaultMaxJpegBufferSize);
int64_t euclidDistSquare(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
bool roundBufferDimensionNearest(int32_t width, int32_t height, int32_t format,
                                 android_dataspace dataSpace, const CameraMetadata& info,
                                 bool maxResolution,
                                         int32_t* outWidth, int32_t* outHeight);
bool isPublicFormat(int32_t format);
binder::Status createSurfaceFromGbp(camera3::OutputStreamInfo& streamInfo, bool isStreamInfoValid,
                                    sp<Surface>& surface, const sp<IGraphicBufferProducer>& gbp,
                                    const std::string& logicalCameraId,
                                    const CameraMetadata& physicalCameraMetadata,
                                    const std::vector<int32_t>& sensorPixelModesUsed,
                                    int64_t dynamicRangeProfile, int64_t streamUseCase,
                                    int timestampBase, int mirrorMode, int32_t colorSpace,
                                    bool respectSurfaceSize);
bool is10bitCompatibleFormat(int32_t format, android_dataspace_t dataSpace);
bool is10bitDynamicRangeProfile(int64_t dynamicRangeProfile);
bool isDynamicRangeProfileSupported(int64_t dynamicRangeProfile, const CameraMetadata& staticMeta);
bool deviceReportsColorSpaces(const CameraMetadata& staticMeta);
bool isColorSpaceSupported(int32_t colorSpace, int32_t format, android_dataspace dataSpace,
                           int64_t dynamicRangeProfile, const CameraMetadata& staticMeta);
bool dataSpaceFromColorSpace(android_dataspace* dataSpace, int32_t colorSpace);
bool isStreamUseCaseSupported(int64_t streamUseCase, const CameraMetadata& deviceInfo);
void mapStreamInfo(const OutputStreamInfo& streamInfo, camera3::camera_stream_rotation_t rotation,
                   const std::string& physicalId, int32_t groupId,
                   aidl::android::hardware::camera::device::Stream* stream );
binder::Status checkPhysicalCameraId(const std::vector<std::string>& physicalCameraIds,
                                     const std::string& physicalCameraId,
                                     const std::string& logicalCameraId);
binder::Status checkSurfaceType(size_t numBufferProducers, bool deferredConsumer, int surfaceType,
                                bool isConfigurationComplete);
binder::Status checkOperatingMode(int operatingMode, const CameraMetadata& staticInfo,
                                  const std::string& cameraId);
binder::Status convertToHALStreamCombination(
        const SessionConfiguration& sessionConfiguration, const std::string& logicalCameraId,
        const CameraMetadata& deviceInfo, bool isCompositeJpegRDisabled, metadataGetter getMetadata,
        const std::vector<std::string>& physicalCameraIds,
        aidl::android::hardware::camera::device::StreamConfiguration& streamConfiguration,
        bool overrideForPerfClass, metadata_vendor_id_t vendorTagId, bool checkSessionParams,
        const std::vector<int32_t>& additionalKeys, bool* earlyExit);
StreamConfigurationPair getStreamConfigurationPair(const CameraMetadata& metadata);
status_t checkAndOverrideSensorPixelModesUsed(
        const std::vector<int32_t>& sensorPixelModesUsed, int format, int width, int height,
        const CameraMetadata& staticInfo,
        std::unordered_set<int32_t>* overriddenSensorPixelModesUsed);
bool targetPerfClassPrimaryCamera(const std::set<std::string>& perfClassPrimaryCameraIds,
                                  const std::string& cameraId, int32_t targetSdkVersion);
binder::Status mapRequestTemplateFromClient(const std::string& cameraId, int templateId,
                                            camera_request_template_t* tempId );
status_t mapRequestTemplateToAidl(
        camera_request_template_t templateId,
        aidl::android::hardware::camera::device::RequestTemplate* tempId );
void filterParameters(const CameraMetadata& src, const CameraMetadata& deviceInfo,
                      const std::vector<int32_t>& additionalKeys, metadata_vendor_id_t vendorTagId,
                      CameraMetadata& dst);
status_t overrideDefaultRequestKeys(CameraMetadata* request);
template <typename T>
bool contains(std::set<T> container, T value) {
    return container.find(value) != container.end();
}
constexpr int32_t MAX_SURFACES_PER_STREAM = 4;
constexpr int32_t ROUNDING_WIDTH_CAP = 1920;
constexpr int32_t SDK_VERSION_S = 31;
extern int32_t PERF_CLASS_LEVEL;
extern bool IS_PERF_CLASS;
constexpr int32_t PERF_CLASS_JPEG_THRESH_W = 1920;
constexpr int32_t PERF_CLASS_JPEG_THRESH_H = 1080;
}
}
}
#endif
