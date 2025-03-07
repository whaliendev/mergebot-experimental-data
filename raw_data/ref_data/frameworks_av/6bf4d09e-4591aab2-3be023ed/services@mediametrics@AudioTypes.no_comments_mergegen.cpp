#include "AudioTypes.h"
#include "MediaMetricsConstants.h"
#include "StringUtils.h"
#include <media/TypeConverter.h>
#include <statslog.h>
namespace android::mediametrics::types {
const std::unordered_map<std::string, int32_t>& getAudioCallerNameMap() {
    static std::unordered_map<std::string, int32_t> map{
        {"unknown", 0},
        {"aaudio", 1},
        {"java", 2},
        {"media", 3},
        {"opensles", 4},
        {"rtp", 5},
        {"soundpool", 6},
        {"tonegenerator", 7},
    };
    return map;
}
const std::unordered_map<std::string, int64_t>& getAudioDeviceInMap() {
    static std::unordered_map<std::string, int64_t> map{
        {"AUDIO_DEVICE_IN_COMMUNICATION", 1LL << 0},
        {"AUDIO_DEVICE_IN_AMBIENT", 1LL << 1},
        {"AUDIO_DEVICE_IN_BUILTIN_MIC", 1LL << 2},
        {"AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET", 1LL << 3},
        {"AUDIO_DEVICE_IN_WIRED_HEADSET", 1LL << 4},
        {"AUDIO_DEVICE_IN_AUX_DIGITAL", 1LL << 5},
        {"AUDIO_DEVICE_IN_HDMI", 1LL << 5},
        {"AUDIO_DEVICE_IN_VOICE_CALL", 1LL << 7},
        {"AUDIO_DEVICE_IN_TELEPHONY_RX", 1LL << 7},
        {"AUDIO_DEVICE_IN_BACK_MIC", 1LL << 9},
        {"AUDIO_DEVICE_IN_REMOTE_SUBMIX", 1LL << 10},
        {"AUDIO_DEVICE_IN_ANLG_DOCK_HEADSET", 1LL << 11},
        {"AUDIO_DEVICE_IN_DGTL_DOCK_HEADSET", 1LL << 12},
        {"AUDIO_DEVICE_IN_USB_ACCESSORY", 1LL << 13},
        {"AUDIO_DEVICE_IN_USB_DEVICE", 1LL << 14},
        {"AUDIO_DEVICE_IN_FM_TUNER", 1LL << 15},
        {"AUDIO_DEVICE_IN_TV_TUNER", 1LL << 16},
        {"AUDIO_DEVICE_IN_LINE", 1LL << 17},
        {"AUDIO_DEVICE_IN_SPDIF", 1LL << 18},
        {"AUDIO_DEVICE_IN_BLUETOOTH_A2DP", 1LL << 19},
        {"AUDIO_DEVICE_IN_LOOPBACK", 1LL << 20},
        {"AUDIO_DEVICE_IN_IP", 1LL << 21},
        {"AUDIO_DEVICE_IN_BUS", 1LL << 22},
        {"AUDIO_DEVICE_IN_PROXY", 1LL << 23},
        {"AUDIO_DEVICE_IN_USB_HEADSET", 1LL << 24},
        {"AUDIO_DEVICE_IN_BLUETOOTH_BLE", 1LL << 25},
        {"AUDIO_DEVICE_IN_HDMI_ARC", 1LL << 26},
        {"AUDIO_DEVICE_IN_ECHO_REFERENCE", 1LL << 27},
        {"AUDIO_DEVICE_IN_DEFAULT", 1LL << 28},
        {"AUDIO_DEVICE_IN_BLE_HEADSET", 1LL << 29},
        {"AUDIO_DEVICE_IN_HDMI_EARC", 1LL << 30},
    };
    return map;
}
const std::unordered_map<std::string, int64_t>& getAudioDeviceOutMap() {
    static std::unordered_map<std::string, int64_t> map{
        {"AUDIO_DEVICE_OUT_EARPIECE", 1LL << 0},
        {"AUDIO_DEVICE_OUT_SPEAKER", 1LL << 1},
        {"AUDIO_DEVICE_OUT_WIRED_HEADSET", 1LL << 2},
        {"AUDIO_DEVICE_OUT_WIRED_HEADPHONE", 1LL << 3},
        {"AUDIO_DEVICE_OUT_BLUETOOTH_SCO", 1LL << 4},
        {"AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET", 1LL << 5},
        {"AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT", 1LL << 6},
        {"AUDIO_DEVICE_OUT_BLUETOOTH_A2DP", 1LL << 7},
        {"AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES", 1LL << 8},
        {"AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER", 1LL << 9},
        {"AUDIO_DEVICE_OUT_AUX_DIGITAL", 1LL << 10},
        {"AUDIO_DEVICE_OUT_HDMI", 1LL << 10},
        {"AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET", 1LL << 12},
        {"AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET", 1LL << 13},
        {"AUDIO_DEVICE_OUT_USB_ACCESSORY", 1LL << 14},
        {"AUDIO_DEVICE_OUT_USB_DEVICE", 1LL << 15},
        {"AUDIO_DEVICE_OUT_REMOTE_SUBMIX", 1LL << 16},
        {"AUDIO_DEVICE_OUT_TELEPHONY_TX", 1LL << 17},
        {"AUDIO_DEVICE_OUT_LINE", 1LL << 18},
        {"AUDIO_DEVICE_OUT_HDMI_ARC", 1LL << 19},
        {"AUDIO_DEVICE_OUT_SPDIF", 1LL << 20},
        {"AUDIO_DEVICE_OUT_FM", 1LL << 21},
        {"AUDIO_DEVICE_OUT_AUX_LINE", 1LL << 22},
        {"AUDIO_DEVICE_OUT_SPEAKER_SAFE", 1LL << 23},
        {"AUDIO_DEVICE_OUT_IP", 1LL << 24},
        {"AUDIO_DEVICE_OUT_BUS", 1LL << 25},
        {"AUDIO_DEVICE_OUT_PROXY", 1LL << 26},
        {"AUDIO_DEVICE_OUT_USB_HEADSET", 1LL << 27},
        {"AUDIO_DEVICE_OUT_HEARING_AID", 1LL << 28},
        {"AUDIO_DEVICE_OUT_ECHO_CANCELLER", 1LL << 29},
        {"AUDIO_DEVICE_OUT_DEFAULT", 1LL << 30},
        {"AUDIO_DEVICE_OUT_BLE_HEADSET", 1LL << 31},
        {"AUDIO_DEVICE_OUT_BLE_SPEAKER", 1LL << 32},
        {"AUDIO_DEVICE_OUT_HDMI_EARC", 1LL << 33},
        {"AUDIO_DEVICE_OUT_BLE_BROADCAST", 1LL << 34},
    };
    return map;
}
const std::unordered_map<std::string, int32_t>& getAudioThreadTypeMap() {
    static std::unordered_map<std::string, int32_t> map{
        {"MIXER", 0},
        {"DIRECT", 1},
        {"DUPLICATING", 2},
        {"RECORD", 3},
        {"OFFLOAD", 4},
        {"MMAP_PLAYBACK", 5},
        {"MMAP_CAPTURE", 6},
        {"SPATIALIZER", 7},
    };
    return map;
}
const std::unordered_map<std::string, int32_t>& getAudioTrackTraitsMap() {
    static std::unordered_map<std::string, int32_t> map{
        {"static", (1 << 0)},
    };
    return map;
}
const std::unordered_map<std::string, int32_t>& getAAudioDirection() {
    static std::unordered_map<std::string, int32_t> map {
        {"AAUDIO_DIRECTION_OUTPUT", 1 },
        {"AAUDIO_DIRECTION_INPUT", 2 },
    };
    return map;
}
const std::unordered_map<std::string, int32_t>& getAAudioPerformanceMode() {
    static std::unordered_map<std::string, int32_t> map {
        {"AAUDIO_PERFORMANCE_MODE_NONE", 10},
        {"AAUDIO_PERFORMANCE_MODE_POWER_SAVING", 11},
        {"AAUDIO_PERFORMANCE_MODE_LOW_LATENCY", 12},
    };
    return map;
}
const std::unordered_map<std::string, int32_t>& getAAudioSharingMode() {
    static std::unordered_map<std::string, int32_t> map {
        {"AAUDIO_SHARING_MODE_EXCLUSIVE", 1 },
        {"AAUDIO_SHARING_MODE_SHARED", 2 },
    };
    return map;
}
const std::unordered_map<std::string, int32_t>& getHeadTrackingModeMap() {
    static std::unordered_map<std::string, int32_t> map {
        {"OTHER", 0},
        {"DISABLED", -1},
        {"RELATIVE_WORLD", 1},
        {"RELATIVE_SCREEN", 2},
    };
    return map;
}
const std::unordered_map<std::string, int32_t>& getSpatializerLevelMap() {
    static std::unordered_map<std::string, int32_t> map {
        {"NONE", 0},
        {"SPATIALIZER_MULTICHANNEL", 1},
        {"SPATIALIZER_MCHAN_BED_PLUS_OBJECTS", 2},
    };
    return map;
}
const std::unordered_map<std::string, int32_t>& getSpatializerModeMap() {
    static std::unordered_map<std::string, int32_t> map {
        {"SPATIALIZER_BINAURAL", 0},
        {"SPATIALIZER_TRANSAURAL", 1},
    };
    return map;
}
const std::unordered_map<std::string, int32_t>& getStatusMap() {
    static std::unordered_map<std::string, int32_t> map {
        {"",
            util::MEDIAMETRICS_AUDIO_TRACK_STATUS_REPORTED__STATUS__NO_ERROR},
        {AMEDIAMETRICS_PROP_STATUS_VALUE_OK,
            util::MEDIAMETRICS_AUDIO_TRACK_STATUS_REPORTED__STATUS__NO_ERROR},
        {AMEDIAMETRICS_PROP_STATUS_VALUE_ARGUMENT,
            util::MEDIAMETRICS_AUDIO_TRACK_STATUS_REPORTED__STATUS__ERROR_ARGUMENT},
        {AMEDIAMETRICS_PROP_STATUS_VALUE_IO,
            util::MEDIAMETRICS_AUDIO_TRACK_STATUS_REPORTED__STATUS__ERROR_IO},
        {AMEDIAMETRICS_PROP_STATUS_VALUE_MEMORY,
            util::MEDIAMETRICS_AUDIO_TRACK_STATUS_REPORTED__STATUS__ERROR_MEMORY},
        {AMEDIAMETRICS_PROP_STATUS_VALUE_SECURITY,
            util::MEDIAMETRICS_AUDIO_TRACK_STATUS_REPORTED__STATUS__ERROR_SECURITY},
        {AMEDIAMETRICS_PROP_STATUS_VALUE_STATE,
            util::MEDIAMETRICS_AUDIO_TRACK_STATUS_REPORTED__STATUS__ERROR_STATE},
        {AMEDIAMETRICS_PROP_STATUS_VALUE_TIMEOUT,
            util::MEDIAMETRICS_AUDIO_TRACK_STATUS_REPORTED__STATUS__ERROR_TIMEOUT},
        {AMEDIAMETRICS_PROP_STATUS_VALUE_UNKNOWN,
            util::MEDIAMETRICS_AUDIO_TRACK_STATUS_REPORTED__STATUS__ERROR_UNKNOWN},
    };
    return map;
}
template <typename Traits>
int32_t int32FromFlags(const std::string &flags)
{
    const auto result = stringutils::split(flags, "|");
    int32_t intFlags = 0;
    for (const auto& flag : result) {
        typename Traits::Type value;
        if (!TypeConverter<Traits>::fromString(flag, value)) {
            break;
        }
        intFlags |= value;
    }
    return intFlags;
}
template <typename Traits>
std::string stringFromFlags(const std::string &flags, size_t len)
{
    const auto result = stringutils::split(flags, "|");
    std::string sFlags;
    for (const auto& flag : result) {
        typename Traits::Type value;
        if (!TypeConverter<Traits>::fromString(flag, value)) {
            break;
        }
        if (len >= flag.size()) continue;
        if (!sFlags.empty()) sFlags += "|";
        sFlags += flag.c_str() + len;
    }
    return sFlags;
}
template <typename M>
std::string validateStringFromMap(const std::string &str, const M& map)
{
    if (str.empty()) return {};
    const auto result = stringutils::split(str, "|");
    std::stringstream ss;
    for (const auto &s : result) {
        if (map.count(s) > 0) {
            if (ss.tellp() > 0) ss << "|";
            ss << s;
        }
    }
    return ss.str();
}
template <typename M>
typename M::mapped_type flagsFromMap(const std::string &str, const M& map)
{
    if (str.empty()) return {};
    const auto result = stringutils::split(str, "|");
    typename M::mapped_type value{};
    for (const auto &s : result) {
        auto it = map.find(s);
        if (it == map.end()) continue;
        value |= it->second;
    }
    return value;
}
std::vector<int32_t> vectorFromMap( const std::string &str, const std::unordered_map<std::string, int32_t>& map) { std::vector<int32_t> v; if (str.empty()) return v; const auto result = stringutils::split(str, "|"); for (const auto &s : result) { auto it = map.find(s); if (it == map.end()) continue; v.push_back(it->second); } return v; } std::vector<int64_t> channelMaskVectorFromString(const std::string &s) { std::vector<int64_t> v; const auto result = stringutils::split(s, "|"); for (const auto &mask : result) {
template <>
int32_t lookup<CONTENT_TYPE>(const std::string &contentType)
{
    AudioContentTraits::Type value;
    if (!TypeConverter<AudioContentTraits>::fromString(contentType, value)) {
        value = AUDIO_CONTENT_TYPE_UNKNOWN;
    }
    return (int32_t)value;
}
template <>
std::string lookup<CONTENT_TYPE>(const std::string &contentType)
{
    AudioContentTraits::Type value;
    if (!TypeConverter<AudioContentTraits>::fromString(contentType, value)) {
        return "";
    }
    return contentType.c_str() + sizeof("AUDIO_CONTENT_TYPE");
}
template <>
int32_t lookup<ENCODING>(const std::string &encoding)
{
    FormatTraits::Type value;
    if (!TypeConverter<FormatTraits>::fromString(encoding, value)) {
        value = AUDIO_FORMAT_INVALID;
    }
    return (int32_t)value;
}
template <>
std::string lookup<ENCODING>(const std::string &encoding)
{
    FormatTraits::Type value;
    if (!TypeConverter<FormatTraits>::fromString(encoding, value)) {
        return "";
    }
    return encoding.c_str() + sizeof("AUDIO_FORMAT");
}
template <>
int32_t lookup<INPUT_FLAG>(const std::string &inputFlag)
{
    return int32FromFlags<InputFlagTraits>(inputFlag);
}
template <>
std::string lookup<INPUT_FLAG>(const std::string &inputFlag)
{
    return stringFromFlags<InputFlagTraits>(inputFlag, sizeof("AUDIO_INPUT_FLAG"));
}
template <>
int32_t lookup<OUTPUT_FLAG>(const std::string &outputFlag)
{
    return int32FromFlags<OutputFlagTraits>(outputFlag);
}
template <>
std::string lookup<OUTPUT_FLAG>(const std::string &outputFlag)
{
    return stringFromFlags<OutputFlagTraits>(outputFlag, sizeof("AUDIO_OUTPUT_FLAG"));
}
template <>
int32_t lookup<SOURCE_TYPE>(const std::string &sourceType)
{
    SourceTraits::Type value;
    if (!TypeConverter<SourceTraits>::fromString(sourceType, value)) {
        value = AUDIO_SOURCE_DEFAULT;
    }
    return (int32_t)value;
}
template <>
std::string lookup<SOURCE_TYPE>(const std::string &sourceType)
{
    SourceTraits::Type value;
    if (!TypeConverter<SourceTraits>::fromString(sourceType, value)) {
        return "";
    }
    return sourceType.c_str() + sizeof("AUDIO_SOURCE");
}
template <>
int32_t lookup<STREAM_TYPE>(const std::string &streamType)
{
    StreamTraits::Type value;
    if (!TypeConverter<StreamTraits>::fromString(streamType, value)) {
        value = AUDIO_STREAM_DEFAULT;
    }
    return (int32_t)value;
}
template <>
std::string lookup<STREAM_TYPE>(const std::string &streamType)
{
    StreamTraits::Type value;
    if (!TypeConverter<StreamTraits>::fromString(streamType, value)) {
        return "";
    }
    return streamType.c_str() + sizeof("AUDIO_STREAM");
}
template <>
int32_t lookup<USAGE>(const std::string &usage)
{
    UsageTraits::Type value;
    if (!TypeConverter<UsageTraits>::fromString(usage, value)) {
        value = AUDIO_USAGE_UNKNOWN;
    }
    return (int32_t)value;
}
template <>
std::string lookup<USAGE>(const std::string &usage)
{
    UsageTraits::Type value;
    if (!TypeConverter<UsageTraits>::fromString(usage, value)) {
        return "";
    }
    return usage.c_str() + sizeof("AUDIO_USAGE");
}
template <>
int64_t lookup<INPUT_DEVICE>(const std::string &inputDevice)
{
    return flagsFromMap(inputDevice, getAudioDeviceInMap());
}
template <>
std::string lookup<INPUT_DEVICE>(const std::string &inputDevice)
{
    return stringFromFlags<InputDeviceTraits>(inputDevice, sizeof("AUDIO_DEVICE_IN"));
}
template <>
int64_t lookup<OUTPUT_DEVICE>(const std::string &outputDevice)
{
    return flagsFromMap(outputDevice, getAudioDeviceOutMap());
}
template <>
std::string lookup<OUTPUT_DEVICE>(const std::string &outputDevice)
{
    return stringFromFlags<OutputDeviceTraits>(outputDevice, sizeof("AUDIO_DEVICE_OUT"));
}
template <>
int32_t lookup<AUDIO_DEVICE_INFO_TYPE>(const std::string& audioDeviceInfoType)
{
    auto& map = getAudioDeviceInfoTypeMap();
    auto it = map.find(audioDeviceInfoType);
    if (it == map.end()) {
        return 0;
    }
    return it->second;
}
template <>
int32_t lookup<CALLER_NAME>(const std::string &callerName)
{
    auto& map = getAudioCallerNameMap();
    auto it = map.find(callerName);
    if (it == map.end()) {
        return 0;
    }
    return it->second;
}
template <>
std::string lookup<CALLER_NAME>(const std::string &callerName)
{
    auto& map = getAudioCallerNameMap();
    auto it = map.find(callerName);
    if (it == map.end()) {
        return "";
    }
    return callerName;
}
template <>
int32_t lookup<HEAD_TRACKING_MODE>(const std::string& headTrackingMode)
{
    auto& map = getHeadTrackingModeMap();
    auto it = map.find(headTrackingMode);
    if (it == map.end()) {
        return 0;
    }
    return it->second;
}
template <>
int32_t lookup<SPATIALIZER_LEVEL>(const std::string& spatializerLevel)
{
    auto& map = getSpatializerLevelMap();
    auto it = map.find(spatializerLevel);
    if (it == map.end()) {
        return 0;
    }
    return it->second;
}
template <>
int32_t lookup<SPATIALIZER_MODE>(const std::string& spatializerMode)
{
    auto& map = getSpatializerModeMap();
    auto it = map.find(spatializerMode);
    if (it == map.end()) {
        return 0;
    }
    return it->second;
}
template <>
int32_t lookup<STATUS>(const std::string &status)
{
    auto& map = getStatusMap();
    auto it = map.find(status);
    if (it == map.end()) {
        return util::MEDIAMETRICS_AUDIO_TRACK_STATUS_REPORTED__STATUS__ERROR_UNKNOWN;
    }
    return it->second;
}
template <>
int32_t lookup<THREAD_TYPE>(const std::string &threadType)
{
    auto& map = getAudioThreadTypeMap();
    auto it = map.find(threadType);
    if (it == map.end()) {
        return -1;
    }
    return it->second;
}
template <>
std::string lookup<THREAD_TYPE>(const std::string &threadType)
{
    auto& map = getAudioThreadTypeMap();
    auto it = map.find(threadType);
    if (it == map.end()) {
        return "";
    }
    return threadType;
}
bool isInputThreadType(const std::string &threadType)
{
    return threadType == "RECORD" || threadType == "MMAP_CAPTURE";
}
template <>
std::string lookup<TRACK_TRAITS>(const std::string &traits)
{
    return validateStringFromMap(traits, getAudioTrackTraitsMap());
}
template <>
int32_t lookup<TRACK_TRAITS>(const std::string &traits)
{
    return flagsFromMap(traits, getAudioTrackTraitsMap());
}
template <>
std::string lookup<AAUDIO_DIRECTION>(const std::string &direction)
{
    auto& map = getAAudioDirection();
    auto it = map.find(direction);
    if (it == map.end()) {
        return "";
    }
    return direction;
}
template <>
int32_t lookup<AAUDIO_DIRECTION>(const std::string &direction)
{
    auto& map = getAAudioDirection();
    auto it = map.find(direction);
    if (it == map.end()) {
        return 0;
    }
    return it->second;
}
template <>
std::string lookup<AAUDIO_PERFORMANCE_MODE>(const std::string &performanceMode)
{
    auto& map = getAAudioPerformanceMode();
    auto it = map.find(performanceMode);
    if (it == map.end()) {
        return "";
    }
    return performanceMode;
}
template <>
int32_t lookup<AAUDIO_PERFORMANCE_MODE>(const std::string &performanceMode)
{
    auto& map = getAAudioPerformanceMode();
    auto it = map.find(performanceMode);
    if (it == map.end()) {
        return 0;
    }
    return it->second;
}
template <>
std::string lookup<AAUDIO_SHARING_MODE>(const std::string &sharingMode)
{
    auto& map = getAAudioSharingMode();
    auto it = map.find(sharingMode);
    if (it == map.end()) {
        return "";
    }
    return sharingMode;
}
template <>
int32_t lookup<AAUDIO_SHARING_MODE>(const std::string &sharingMode)
{
    auto& map = getAAudioSharingMode();
    auto it = map.find(sharingMode);
    if (it == map.end()) {
        return 0;
    }
    return it->second;
}
}
