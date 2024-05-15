       
#include <string>
#include <unordered_map>
namespace android::mediametrics::types {
const std::unordered_map<std::string, int32_t>& getAudioCallerNameMap();
const std::unordered_map<std::string, int64_t>& getAudioDeviceInMap();
const std::unordered_map<std::string, int64_t>& getAudioDeviceOutMap();
const std::unordered_map<std::string, int32_t>& getAudioThreadTypeMap();
const std::unordered_map<std::string, int32_t>& getAudioTrackTraitsMap();
const std::unordered_map<std::string, int32_t>& getHeadTrackingModeMap();
const std::unordered_map<std::string, int32_t>& getSpatializerLevelMap();
const std::unordered_map<std::string, int32_t>& getSpatializerModeMap();
std::vector<int32_t> vectorFromMap(const std::string& str,
                                   const std::unordered_map<std::string, int32_t>& map);
std::vector<int64_t> channelMaskVectorFromString(const std::string& s);
enum DeviceConnectionResult : int32_t {
    DEVICE_CONNECTION_RESULT_SUCCESS = 0,
    DEVICE_CONNECTION_RESULT_UNKNOWN = 1,
    DEVICE_CONNECTION_RESULT_JAVA_SERVICE_CANCEL =
            2,
};
enum AudioEnumCategory {
    AAUDIO_DIRECTION,
    AAUDIO_PERFORMANCE_MODE,
    AAUDIO_SHARING_MODE,
    AUDIO_DEVICE_INFO_TYPE,
    CALLER_NAME,
    CONTENT_TYPE,
    ENCODING,
    HEAD_TRACKING_MODE,
    INPUT_DEVICE,
    INPUT_FLAG,
    OUTPUT_DEVICE,
    OUTPUT_FLAG,
    SOURCE_TYPE,
    SPATIALIZER_LEVEL,
    SPATIALIZER_MODE,
    STATUS,
    STREAM_TYPE,
    THREAD_TYPE,
    TRACK_TRAITS,
    USAGE,
};
template <AudioEnumCategory C, typename T>
T lookup(const char* str) {
    return lookup<C, T, std::string>(str);
}
bool isInputThreadType(const std::string& threadType);
}
