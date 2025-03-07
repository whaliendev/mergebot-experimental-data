#define LOG_TAG "AudioAnalytics"
#include <android-base/logging.h>
#include <utils/Log.h>
#include "AudioAnalytics.h"
#include <aaudio/AAudio.h>
#include <audio_utils/clock.h>
#include <cutils/properties.h>
#include <statslog.h>
#include <system/audio.h>
#include "AudioTypes.h"
#include "MediaMetricsService.h"
#include "StringUtils.h"
#include "ValidateId.h"
#define PROP_AUDIO_ANALYTICS_CLOUD_ENABLED "persist.audio.analytics.cloud.enabled"
namespace android::mediametrics {
#define STATSD_ENABLE 
#ifdef STATSD_ENABLE
#define CONDITION(INT_VALUE) (INT_VALUE)
#else
#define CONDITION(INT_VALUE) (int(0))
#endif
static constexpr bool STATSD_USE_INT_FOR_ENUM = false;
using short_enum_type_t = std::conditional_t<STATSD_USE_INT_FOR_ENUM, int32_t, std::string>;
using long_enum_type_t = std::conditional_t<STATSD_USE_INT_FOR_ENUM, int64_t, std::string>;
template <typename T>
auto ENUM_EXTRACT(const T& x) {
    if constexpr (std::is_same_v<std::decay_t<T>, std::string>) {
        return x.c_str();
    } else {
        return x;
    }
}
inline constexpr const char* extendedStatusToStatusString(status_t status) {
    switch (status) {
    case BAD_VALUE:
    case AAUDIO_ERROR_ILLEGAL_ARGUMENT:
    case AAUDIO_ERROR_INVALID_FORMAT:
    case AAUDIO_ERROR_INVALID_RATE:
    case AAUDIO_ERROR_NULL:
    case AAUDIO_ERROR_OUT_OF_RANGE:
        return AMEDIAMETRICS_PROP_STATUS_VALUE_ARGUMENT;
    case DEAD_OBJECT:
    case FAILED_TRANSACTION:
    case AAUDIO_ERROR_DISCONNECTED:
    case AAUDIO_ERROR_INVALID_HANDLE:
    case AAUDIO_ERROR_NO_SERVICE:
        return AMEDIAMETRICS_PROP_STATUS_VALUE_IO;
    case NO_MEMORY:
    case AAUDIO_ERROR_NO_FREE_HANDLES:
    case AAUDIO_ERROR_NO_MEMORY:
        return AMEDIAMETRICS_PROP_STATUS_VALUE_MEMORY;
    case PERMISSION_DENIED:
        return AMEDIAMETRICS_PROP_STATUS_VALUE_SECURITY;
    case INVALID_OPERATION:
    case NO_INIT:
    case AAUDIO_ERROR_INVALID_STATE:
    case AAUDIO_ERROR_UNAVAILABLE:
    case AAUDIO_ERROR_UNIMPLEMENTED:
        return AMEDIAMETRICS_PROP_STATUS_VALUE_STATE;
    case WOULD_BLOCK:
    case AAUDIO_ERROR_TIMEOUT:
    case AAUDIO_ERROR_WOULD_BLOCK:
        return AMEDIAMETRICS_PROP_STATUS_VALUE_TIMEOUT;
    default:
        if (status >= 0) return AMEDIAMETRICS_PROP_STATUS_VALUE_OK;
        [[fallthrough]];
    case UNKNOWN_ERROR:
        return AMEDIAMETRICS_PROP_STATUS_VALUE_UNKNOWN;
    }
}
static constexpr const auto LOG_LEVEL = android::base::VERBOSE;
static constexpr int PREVIOUS_STATE_EXPIRE_SEC = 60 * 60;
static constexpr const char * SUPPRESSED = "SUPPRESSED";
static constexpr const char * const AudioRecordDeviceUsageFields[] = {
    "mediametrics_audiorecorddeviceusage_reported",
    "devices",
    "device_names",
    "device_time_nanos",
    "encoding",
    "frame_count",
    "interval_count",
    "sample_rate",
    "flags",
    "package_name",
    "selected_device_id",
    "caller",
    "source",
    "log_session_id",
};
static constexpr const char * const AudioThreadDeviceUsageFields[] = {
    "mediametrics_audiothreaddeviceusage_reported",
    "devices",
    "device_names",
    "device_time_nanos",
    "encoding",
    "frame_count",
    "interval_count",
    "sample_rate",
    "flags",
    "xruns",
    "type",
};
static constexpr const char * const AudioTrackDeviceUsageFields[] = {
    "mediametrics_audiotrackdeviceusage_reported",
    "devices",
    "device_names",
    "device_time_nanos",
    "encoding",
    "frame_count",
    "interval_count",
    "sample_rate",
    "flags",
    "xruns",
    "package_name",
    "device_latency_millis",
    "device_startup_millis",
    "device_volume",
    "selected_device_id",
    "stream_type",
    "usage",
    "content_type",
    "caller",
    "traits",
    "log_session_id",
};
static constexpr const char * const AudioRecordStatusFields[] {
    "mediametrics_audiorecordstatus_reported",
    "status",
    "debug_message",
    "status_subcode",
    "uid",
    "event",
    "input_flags",
    "source",
    "encoding",
    "channel_mask",
    "buffer_frame_count",
    "sample_rate",
};
static constexpr const char * const AudioTrackStatusFields[] {
    "mediametrics_audiotrackstatus_reported",
    "status",
    "debug_message",
    "status_subcode",
    "uid",
    "event",
    "output_flags",
    "content_type",
    "usage",
    "encoding",
    "channel_mask",
    "buffer_frame_count",
    "sample_rate",
    "speed",
    "pitch",
};
static constexpr const char * const AudioDeviceConnectionFields[] = {
    "mediametrics_audiodeviceconnection_reported",
    "input_devices",
    "output_devices",
    "device_names",
    "result",
    "time_to_connect_millis",
    "connection_count",
};
static constexpr const char * const AAudioStreamFields[] {
    "mediametrics_aaudiostream_reported",
    "path",
    "direction",
    "frames_per_burst",
    "buffer_size",
    "buffer_capacity",
    "channel_count",
    "total_frames_transferred",
    "perf_mode_requested",
    "perf_mode_actual",
    "sharing",
    "xrun_count",
    "device_type",
    "format_app",
    "format_device",
    "log_session_id",
    "sample_rate",
    "content_type",
    "sharing_requested",
};
static constexpr const char * HeadTrackerDeviceEnabledFields[] {
    "mediametrics_headtrackerdeviceenabled_reported",
    "type",
    "event",
    "enabled",
};
static constexpr const char * HeadTrackerDeviceSupportedFields[] {
    "mediametrics_headtrackerdevicesupported_reported",
    "type",
    "event",
    "supported",
};
static constexpr const char * SpatializerCapabilitiesFields[] {
    "mediametrics_spatializer_reported",
    "head_tracking_modes",
    "spatializer_levels",
    "spatializer_modes",
    "channel_masks",
};
static constexpr const char * SpatializerDeviceEnabledFields[] {
    "mediametrics_spatializerdeviceenabled_reported",
    "type",
    "event",
    "enabled",
};
template <size_t N, typename ...Types>
std::string printFields(const char * const (& fields)[N], Types ... args)
{
    std::stringstream ss;
    ss << " { ";
    stringutils::fieldPrint(ss, fields, args...);
    ss << "}";
    return ss.str();
}
template <typename ...Types>
int sendToStatsd(Types ... args)
{
    int result = 0;
#ifdef STATSD_ENABLE
    result = android::util::stats_write(args...);
#endif
    return result;
}
template <size_t N, typename ...Types>
std::pair<int, std::string> sendToStatsd(const char * const (& fields)[N], Types ... args)
{
    int result = 0;
    std::stringstream ss;
#ifdef STATSD_ENABLE
    result = android::util::stats_write(args...);
    ss << "result:" << result;
#endif
    ss << " { ";
    stringutils::fieldPrint(ss, fields, args...);
    ss << "}";
    return { result, ss.str() };
}
AudioAnalytics::AudioAnalytics(const std::shared_ptr<StatsdLog>& statsdLog)
    : mDeliverStatistics(property_get_bool(PROP_AUDIO_ANALYTICS_CLOUD_ENABLED, true))
    , mStatsdLog(statsdLog)
    , mAudioPowerUsage(this, statsdLog)
{
    SetMinimumLogSeverity(android::base::DEBUG);
    ALOGD("%s", __func__);
    mActions.addAction(
        AMEDIAMETRICS_KEY_AUDIO_FLINGER "." AMEDIAMETRICS_PROP_EVENT,
        std::string(AMEDIAMETRICS_PROP_EVENT_VALUE_CTOR),
        std::make_shared<AnalyticsActions::Function>(
            [this](const std::shared_ptr<const android::mediametrics::Item> &item){
                mHealth.onAudioServerStart(Health::Module::AUDIOFLINGER, item);
            }));
    mActions.addAction(
        AMEDIAMETRICS_KEY_AUDIO_POLICY "." AMEDIAMETRICS_PROP_EVENT,
        std::string(AMEDIAMETRICS_PROP_EVENT_VALUE_CTOR),
        std::make_shared<AnalyticsActions::Function>(
            [this](const std::shared_ptr<const android::mediametrics::Item> &item){
                mHealth.onAudioServerStart(Health::Module::AUDIOPOLICY, item);
            }));
    mActions.addAction(
        AMEDIAMETRICS_KEY_AUDIO_FLINGER "." AMEDIAMETRICS_PROP_EVENT,
        std::string(AMEDIAMETRICS_PROP_EVENT_VALUE_TIMEOUT),
        std::make_shared<AnalyticsActions::Function>(
            [this](const std::shared_ptr<const android::mediametrics::Item> &item){
                mHealth.onAudioServerTimeout(Health::Module::AUDIOFLINGER, item);
            }));
    mActions.addAction(
        AMEDIAMETRICS_KEY_AUDIO_POLICY "." AMEDIAMETRICS_PROP_EVENT,
        std::string(AMEDIAMETRICS_PROP_EVENT_VALUE_TIMEOUT),
        std::make_shared<AnalyticsActions::Function>(
            [this](const std::shared_ptr<const android::mediametrics::Item> &item){
                mHealth.onAudioServerTimeout(Health::Module::AUDIOPOLICY, item);
            }));
    mActions.addAction(
        AMEDIAMETRICS_KEY_PREFIX_AUDIO_TRACK "*." AMEDIAMETRICS_PROP_EVENT,
        std::string(AMEDIAMETRICS_PROP_EVENT_VALUE_ENDAAUDIOSTREAM),
        std::make_shared<AnalyticsActions::Function>(
            [this](const std::shared_ptr<const android::mediametrics::Item> &item) {
                mAAudioStreamInfo.endAAudioStream(item, AAudioStreamInfo::CALLER_PATH_LEGACY);
            }));
    mActions.addAction(
        AMEDIAMETRICS_KEY_PREFIX_AUDIO_RECORD "*." AMEDIAMETRICS_PROP_EVENT,
        std::string(AMEDIAMETRICS_PROP_EVENT_VALUE_ENDAAUDIOSTREAM),
        std::make_shared<AnalyticsActions::Function>(
            [this](const std::shared_ptr<const android::mediametrics::Item> &item) {
                mAAudioStreamInfo.endAAudioStream(item, AAudioStreamInfo::CALLER_PATH_LEGACY);
            }));
    mActions.addAction(
        AMEDIAMETRICS_KEY_PREFIX_AUDIO_STREAM "*." AMEDIAMETRICS_PROP_EVENT,
        std::string(AMEDIAMETRICS_PROP_EVENT_VALUE_ENDAAUDIOSTREAM),
        std::make_shared<AnalyticsActions::Function>(
            [this](const std::shared_ptr<const android::mediametrics::Item> &item) {
                mAAudioStreamInfo.endAAudioStream(item, AAudioStreamInfo::CALLER_PATH_MMAP);
            }));
    mActions.addAction(
        AMEDIAMETRICS_KEY_PREFIX_AUDIO_RECORD "*." AMEDIAMETRICS_PROP_EVENT,
        std::string(AMEDIAMETRICS_PROP_EVENT_VALUE_ENDAUDIOINTERVALGROUP),
        std::make_shared<AnalyticsActions::Function>(
            [this](const std::shared_ptr<const android::mediametrics::Item> &item){
                mDeviceUse.endAudioIntervalGroup(item, DeviceUse::RECORD);
            }));
    mActions.addAction(
        AMEDIAMETRICS_KEY_PREFIX_AUDIO_THREAD "*." AMEDIAMETRICS_PROP_EVENT,
        std::string(AMEDIAMETRICS_PROP_EVENT_VALUE_ENDAUDIOINTERVALGROUP),
        std::make_shared<AnalyticsActions::Function>(
            [this](const std::shared_ptr<const android::mediametrics::Item> &item){
                mDeviceUse.endAudioIntervalGroup(item, DeviceUse::THREAD);
            }));
    mActions.addAction(
        AMEDIAMETRICS_KEY_PREFIX_AUDIO_TRACK "*." AMEDIAMETRICS_PROP_EVENT,
        std::string(AMEDIAMETRICS_PROP_EVENT_VALUE_ENDAUDIOINTERVALGROUP),
        std::make_shared<AnalyticsActions::Function>(
            [this](const std::shared_ptr<const android::mediametrics::Item> &item){
                mDeviceUse.endAudioIntervalGroup(item, DeviceUse::TRACK);
            }));
    mActions.addAction(
        "audio.device.a2dp.state",
        "connected",
        std::make_shared<AnalyticsActions::Function>(
            [this](const std::shared_ptr<const android::mediametrics::Item> &item){
                mDeviceConnection.a2dpConnected(item);
            }));
    mActions.addAction(
        AMEDIAMETRICS_KEY_PREFIX_AUDIO_THREAD "*." AMEDIAMETRICS_PROP_EVENT,
        std::string("createAudioPatch"),
        std::make_shared<AnalyticsActions::Function>(
            [this](const std::shared_ptr<const android::mediametrics::Item> &item){
                mDeviceConnection.createPatch(item);
            }));
    mActions.addAction(
        AMEDIAMETRICS_KEY_PREFIX_AUDIO_DEVICE
        "postBluetoothA2dpDeviceConnectionStateSuppressNoisyIntent"
        "." AMEDIAMETRICS_PROP_STATE,
        "connected",
        std::make_shared<AnalyticsActions::Function>(
            [this](const std::shared_ptr<const android::mediametrics::Item> &item){
                mDeviceConnection.postBluetoothA2dpDeviceConnectionStateSuppressNoisyIntent(item);
            }));
    mActions.addAction(
        AMEDIAMETRICS_KEY_PREFIX_AUDIO_TRACK "*." AMEDIAMETRICS_PROP_EVENT,
        std::string(AMEDIAMETRICS_PROP_EVENT_VALUE_ENDAUDIOINTERVALGROUP),
        std::make_shared<AnalyticsActions::Function>(
            [this](const std::shared_ptr<const android::mediametrics::Item> &item){
                mAudioPowerUsage.checkTrackRecord(item, true );
            }));
    mActions.addAction(
        AMEDIAMETRICS_KEY_PREFIX_AUDIO_RECORD "*." AMEDIAMETRICS_PROP_EVENT,
        std::string(AMEDIAMETRICS_PROP_EVENT_VALUE_ENDAUDIOINTERVALGROUP),
        std::make_shared<AnalyticsActions::Function>(
            [this](const std::shared_ptr<const android::mediametrics::Item> &item){
                mAudioPowerUsage.checkTrackRecord(item, false );
            }));
    mActions.addAction(
        AMEDIAMETRICS_KEY_AUDIO_FLINGER "." AMEDIAMETRICS_PROP_EVENT,
        std::string(AMEDIAMETRICS_PROP_EVENT_VALUE_SETMODE),
        std::make_shared<AnalyticsActions::Function>(
            [this](const std::shared_ptr<const android::mediametrics::Item> &item){
                mAudioPowerUsage.checkMode(item);
            }));
    mActions.addAction(
        AMEDIAMETRICS_KEY_AUDIO_FLINGER "." AMEDIAMETRICS_PROP_EVENT,
        std::string(AMEDIAMETRICS_PROP_EVENT_VALUE_SETVOICEVOLUME),
        std::make_shared<AnalyticsActions::Function>(
            [this](const std::shared_ptr<const android::mediametrics::Item> &item){
                mAudioPowerUsage.checkVoiceVolume(item);
            }));
    mActions.addAction(
        AMEDIAMETRICS_KEY_PREFIX_AUDIO_THREAD "*." AMEDIAMETRICS_PROP_EVENT,
        std::string("createAudioPatch"),
        std::make_shared<AnalyticsActions::Function>(
            [this](const std::shared_ptr<const android::mediametrics::Item> &item){
                mAudioPowerUsage.checkCreatePatch(item);
            }));
    mActions.addAction(
        AMEDIAMETRICS_KEY_PREFIX_AUDIO_SPATIALIZER "*." AMEDIAMETRICS_PROP_EVENT,
        std::monostate{},
        std::make_shared<AnalyticsActions::Function>(
            [this](const std::shared_ptr<const android::mediametrics::Item> &item){
                mSpatializer.onEvent(item);
            }));
}
AudioAnalytics::~AudioAnalytics()
{
    ALOGD("%s", __func__);
    mTimedAction.quit();
}
status_t AudioAnalytics::submit(
        const std::shared_ptr<const mediametrics::Item>& item, bool isTrusted)
{
    if (!startsWith(item->getKey(), AMEDIAMETRICS_KEY_PREFIX_AUDIO)) return BAD_VALUE;
    status_t status = mAnalyticsState->submit(item, isTrusted);
    processStatus(item);
    if (status != NO_ERROR) return status;
    processActions(item);
    return NO_ERROR;
}
std::pair<std::string, int32_t> AudioAnalytics::dump(
        int32_t lines, int64_t sinceNs, const char *prefix) const
{
    std::stringstream ss;
    int32_t ll = lines;
    if (ll > 0) {
        auto [s, l] = mAnalyticsState->dump(ll, sinceNs, prefix);
        ss << s;
        ll -= l;
    }
    if (ll > 0) {
        ss << "Prior audioserver state:\n";
        --ll;
    }
    if (ll > 0) {
        auto [s, l] = mPreviousAnalyticsState->dump(ll, sinceNs, prefix);
        ss << s;
        ll -= l;
    }
    if (ll > 0 && prefix == nullptr) {
        auto [s, l] = mAudioPowerUsage.dump(ll);
        ss << s;
        ll -= l;
    }
    return { ss.str(), lines - ll };
}
void AudioAnalytics::processActions(const std::shared_ptr<const mediametrics::Item>& item)
{
    auto actions = mActions.getActionsForItem(item);
    for (const auto& action : actions) {
        (*action)(item);
    }
}
void AudioAnalytics::processStatus(const std::shared_ptr<const mediametrics::Item>& item)
{
    int32_t status;
    if (!item->get(AMEDIAMETRICS_PROP_STATUS, &status)) return;
    const auto key = item->getKey();
    const auto uid = item->getUid();
    const auto [prefixKey, suffixKey] = stringutils::splitPrefixKey(key);
    std::string message;
    item->get(AMEDIAMETRICS_PROP_STATUSMESSAGE, &message);
    int32_t subCode = 0;
    (void)item->get(AMEDIAMETRICS_PROP_STATUSSUBCODE, &subCode);
    std::string eventStr;
    item->get(AMEDIAMETRICS_PROP_EVENT, &eventStr);
    const std::string statusString = extendedStatusToStatusString(status);
    mHeatMap.add(prefixKey, suffixKey, eventStr, statusString, uid, message, subCode);
    if (reportAudioRecordStatus(item, key, eventStr, statusString, uid, message, subCode)) return;
    if (reportAudioTrackStatus(item, key, eventStr, statusString, uid, message, subCode)) return;
}
bool AudioAnalytics::reportAudioRecordStatus(
        const std::shared_ptr<const mediametrics::Item>& item,
        const std::string& key, const std::string& eventStr,
        const std::string& statusString, uid_t uid, const std::string& message,
        int32_t subCode) const
{
    if (!startsWith(key, AMEDIAMETRICS_KEY_PREFIX_AUDIO_RECORD)) return false;
    if (eventStr == AMEDIAMETRICS_PROP_EVENT_VALUE_CREATE) {
        const int atom_status = types::lookup<types::STATUS, int32_t>(statusString);
        const int32_t event = android::util::
                MEDIAMETRICS_AUDIO_RECORD_STATUS_REPORTED__EVENT__AUDIO_RECORD_EVENT_CREATE;
        std::string flagsStr;
        ALOGD_IF(!item->get(AMEDIAMETRICS_PROP_ORIGINALFLAGS, &flagsStr),
                "%s: %s missing %s field", __func__,
                AMEDIAMETRICS_KEY_PREFIX_AUDIO_RECORD, AMEDIAMETRICS_PROP_ORIGINALFLAGS);
        const auto flags = types::lookup<types::INPUT_FLAG, int32_t>(flagsStr);
        std::string sourceStr;
        ALOGD_IF(!item->get(AMEDIAMETRICS_PROP_SOURCE, &sourceStr),
                "%s: %s missing %s field",
                __func__, AMEDIAMETRICS_KEY_PREFIX_AUDIO_RECORD, AMEDIAMETRICS_PROP_SOURCE);
        const int32_t source = types::lookup<types::SOURCE_TYPE, int32_t>(sourceStr);
        std::string encodingStr;
        ALOGD_IF(!item->get(AMEDIAMETRICS_PROP_ENCODING, &encodingStr),
                "%s: %s missing %s field",
                __func__, AMEDIAMETRICS_KEY_PREFIX_AUDIO_RECORD, AMEDIAMETRICS_PROP_ENCODING);
        const auto encoding = types::lookup<types::ENCODING, int32_t>(encodingStr);
        int32_t channelMask = 0;
        ALOGD_IF(!item->get(AMEDIAMETRICS_PROP_CHANNELMASK, &channelMask),
                "%s: %s missing %s field",
                __func__, AMEDIAMETRICS_KEY_PREFIX_AUDIO_RECORD, AMEDIAMETRICS_PROP_CHANNELMASK);
        int32_t frameCount = 0;
        ALOGD_IF(!item->get(AMEDIAMETRICS_PROP_FRAMECOUNT, &frameCount),
                "%s: %s missing %s field",
                __func__, AMEDIAMETRICS_KEY_PREFIX_AUDIO_RECORD, AMEDIAMETRICS_PROP_FRAMECOUNT);
        int32_t sampleRate = 0;
        ALOGD_IF(!item->get(AMEDIAMETRICS_PROP_SAMPLERATE, &sampleRate),
                "%s: %s missing %s field",
                __func__, AMEDIAMETRICS_KEY_PREFIX_AUDIO_RECORD, AMEDIAMETRICS_PROP_SAMPLERATE);
        const auto [ result, str ] = sendToStatsd(AudioRecordStatusFields,
                CONDITION(android::util::MEDIAMETRICS_AUDIORECORDSTATUS_REPORTED)
                , atom_status
                , message.c_str()
                , subCode
                , uid
                , event
                , flags
                , source
                , encoding
                , (int64_t)channelMask
                , frameCount
                , sampleRate
                );
        ALOGV("%s: statsd %s", __func__, str.c_str());
        mStatsdLog->log(android::util::MEDIAMETRICS_AUDIORECORDSTATUS_REPORTED, str);
        return true;
    }
    return false;
}
bool AudioAnalytics::reportAudioTrackStatus(
        const std::shared_ptr<const mediametrics::Item>& item,
        const std::string& key, const std::string& eventStr,
        const std::string& statusString, uid_t uid, const std::string& message,
        int32_t subCode) const
{
    if (!startsWith(key, AMEDIAMETRICS_KEY_PREFIX_AUDIO_TRACK)) return false;
    if (eventStr == AMEDIAMETRICS_PROP_EVENT_VALUE_CREATE) {
        const int atom_status = types::lookup<types::STATUS, int32_t>(statusString);
        const int32_t event = android::util::
                MEDIAMETRICS_AUDIO_TRACK_STATUS_REPORTED__EVENT__AUDIO_TRACK_EVENT_CREATE;
        std::string flagsStr;
        ALOGD_IF(!item->get(AMEDIAMETRICS_PROP_ORIGINALFLAGS, &flagsStr),
                "%s: %s missing %s field",
                __func__, AMEDIAMETRICS_KEY_PREFIX_AUDIO_TRACK, AMEDIAMETRICS_PROP_ORIGINALFLAGS);
        const auto flags = types::lookup<types::OUTPUT_FLAG, int32_t>(flagsStr);
        std::string contentTypeStr;
        ALOGD_IF(!item->get(AMEDIAMETRICS_PROP_CONTENTTYPE, &contentTypeStr),
                "%s: %s missing %s field",
                __func__, AMEDIAMETRICS_KEY_PREFIX_AUDIO_TRACK, AMEDIAMETRICS_PROP_CONTENTTYPE);
        const auto contentType = types::lookup<types::CONTENT_TYPE, int32_t>(contentTypeStr);
        std::string usageStr;
        ALOGD_IF(!item->get(AMEDIAMETRICS_PROP_USAGE, &usageStr),
                "%s: %s missing %s field",
                __func__, AMEDIAMETRICS_KEY_PREFIX_AUDIO_TRACK, AMEDIAMETRICS_PROP_USAGE);
        const auto usage = types::lookup<types::USAGE, int32_t>(usageStr);
        std::string encodingStr;
        ALOGD_IF(!item->get(AMEDIAMETRICS_PROP_ENCODING, &encodingStr),
                "%s: %s missing %s field",
                __func__, AMEDIAMETRICS_KEY_PREFIX_AUDIO_TRACK, AMEDIAMETRICS_PROP_ENCODING);
        const auto encoding = types::lookup<types::ENCODING, int32_t>(encodingStr);
        int32_t channelMask = 0;
        ALOGD_IF(!item->get(AMEDIAMETRICS_PROP_CHANNELMASK, &channelMask),
                "%s: %s missing %s field",
                __func__, AMEDIAMETRICS_KEY_PREFIX_AUDIO_TRACK, AMEDIAMETRICS_PROP_CHANNELMASK);
        int32_t frameCount = 0;
        ALOGD_IF(!item->get(AMEDIAMETRICS_PROP_FRAMECOUNT, &frameCount),
                "%s: %s missing %s field",
                __func__, AMEDIAMETRICS_KEY_PREFIX_AUDIO_TRACK, AMEDIAMETRICS_PROP_FRAMECOUNT);
        int32_t sampleRate = 0;
        ALOGD_IF(!item->get(AMEDIAMETRICS_PROP_SAMPLERATE, &sampleRate),
                "%s: %s missing %s field",
                __func__, AMEDIAMETRICS_KEY_PREFIX_AUDIO_TRACK, AMEDIAMETRICS_PROP_SAMPLERATE);
        double speed = 0.f;
        ALOGD_IF(!item->get(AMEDIAMETRICS_PROP_PLAYBACK_SPEED, &speed),
                "%s: %s missing %s field",
                __func__,
                AMEDIAMETRICS_KEY_PREFIX_AUDIO_TRACK, AMEDIAMETRICS_PROP_PLAYBACK_SPEED);
        double pitch = 0.f;
        ALOGD_IF(!item->get(AMEDIAMETRICS_PROP_PLAYBACK_PITCH, &pitch),
                "%s: %s missing %s field",
                __func__,
                AMEDIAMETRICS_KEY_PREFIX_AUDIO_TRACK, AMEDIAMETRICS_PROP_PLAYBACK_PITCH);
        const auto [ result, str ] = sendToStatsd(AudioTrackStatusFields,
                CONDITION(android::util::MEDIAMETRICS_AUDIOTRACKSTATUS_REPORTED)
                , atom_status
                , message.c_str()
                , subCode
                , uid
                , event
                , flags
                , contentType
                , usage
                , encoding
                , (int64_t)channelMask
                , frameCount
                , sampleRate
                , (float)speed
                , (float)pitch
                );
        ALOGV("%s: statsd %s", __func__, str.c_str());
        mStatsdLog->log(android::util::MEDIAMETRICS_AUDIOTRACKSTATUS_REPORTED, str);
        return true;
    }
    return false;
}
std::string AudioAnalytics::getThreadFromTrack(const std::string& track) const
{
    int32_t threadId_int32{};
    if (mAnalyticsState->timeMachine().get(
            track, AMEDIAMETRICS_PROP_THREADID, &threadId_int32) != NO_ERROR) {
        return {};
    }
    return std::string(AMEDIAMETRICS_KEY_PREFIX_AUDIO_THREAD) + std::to_string(threadId_int32);
}
void AudioAnalytics::DeviceUse::endAudioIntervalGroup(
       const std::shared_ptr<const android::mediametrics::Item> &item, ItemType itemType) const {
    const std::string& key = item->getKey();
    const std::string id = key.substr(
            (itemType == THREAD ? sizeof(AMEDIAMETRICS_KEY_PREFIX_AUDIO_THREAD)
            : itemType == TRACK ? sizeof(AMEDIAMETRICS_KEY_PREFIX_AUDIO_TRACK)
            : sizeof(AMEDIAMETRICS_KEY_PREFIX_AUDIO_RECORD))
             - 1);
    int64_t deviceTimeNs = 0;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_DEVICETIMENS, &deviceTimeNs);
    std::string encoding;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_ENCODING, &encoding);
    int32_t frameCount = 0;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_FRAMECOUNT, &frameCount);
    std::string inputDevicePairs;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_INPUTDEVICES, &inputDevicePairs);
    int32_t intervalCount = 0;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_INTERVALCOUNT, &intervalCount);
    std::string outputDevicePairs;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_OUTPUTDEVICES, &outputDevicePairs);
    int32_t sampleRate = 0;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_SAMPLERATE, &sampleRate);
    std::string flags;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_FLAGS, &flags);
    std::stringstream oss;
    long_enum_type_t outputDeviceBits{};
    {
        const auto devaddrvec = stringutils::getDeviceAddressPairs(outputDevicePairs);
        for (const auto& [device, addr] : devaddrvec) {
            if (oss.tellp() > 0) oss << "|";
            oss << device;
            outputDeviceBits += types::lookup<types::OUTPUT_DEVICE, long_enum_type_t>(device);
        }
    }
    const std::string outputDevices = oss.str();
    std::stringstream iss;
    long_enum_type_t inputDeviceBits{};
    {
        const auto devaddrvec = stringutils::getDeviceAddressPairs(inputDevicePairs);
        for (const auto& [device, addr] : devaddrvec) {
            if (iss.tellp() > 0) iss << "|";
            iss << device;
            inputDeviceBits += types::lookup<types::INPUT_DEVICE, long_enum_type_t>(device);
        }
    }
    const std::string inputDevices = iss.str();
    bool isBluetooth = false;
    std::string inputDeviceNames;
    std::string outputDeviceNames;
    if (outputDevices.find("AUDIO_DEVICE_OUT_BLUETOOTH") != std::string::npos) {
        isBluetooth = true;
        outputDeviceNames = SUPPRESSED;
#if 0
        mAudioAnalytics.mAnalyticsState->timeMachine().get(
            "audio.device.bt_a2dp", AMEDIAMETRICS_PROP_NAME, &outputDeviceNames);
        stringutils::replace(outputDeviceNames, "|", '?');
        if (outputDeviceNames.size() > STATSD_DEVICE_NAME_MAX_LENGTH) {
            outputDeviceNames.resize(STATSD_DEVICE_NAME_MAX_LENGTH);
        }
#endif
    }
    switch (itemType) {
    case RECORD: {
        std::string callerName;
        const bool clientCalled = mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_CALLERNAME, &callerName) == OK;
        std::string packageName;
        int64_t versionCode = 0;
        int32_t uid = -1;
        mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_ALLOWUID, &uid);
        if (uid != -1) {
            std::tie(packageName, versionCode) =
                    MediaMetricsService::getSanitizedPackageNameAndVersionCode(uid);
        }
        int32_t selectedDeviceId = 0;
        mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_SELECTEDDEVICEID, &selectedDeviceId);
        std::string source;
        mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_SOURCE, &source);
        std::string logSessionId;
        mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_LOGSESSIONID, &logSessionId);
        const auto callerNameForStats =
                types::lookup<types::CALLER_NAME, short_enum_type_t>(callerName);
        const auto encodingForStats = types::lookup<types::ENCODING, short_enum_type_t>(encoding);
        const auto flagsForStats = types::lookup<types::INPUT_FLAG, short_enum_type_t>(flags);
        const auto sourceForStats = types::lookup<types::SOURCE_TYPE, short_enum_type_t>(source);
        const auto logSessionIdForStats = ValidateId::get()->validateId(logSessionId);
        LOG(LOG_LEVEL) << "key:" << key
              << " id:" << id
              << " inputDevices:" << inputDevices << "(" << inputDeviceBits
              << ") inputDeviceNames:" << inputDeviceNames
              << " deviceTimeNs:" << deviceTimeNs
              << " encoding:" << encoding << "(" << encodingForStats
              << ") frameCount:" << frameCount
              << " intervalCount:" << intervalCount
              << " sampleRate:" << sampleRate
              << " flags:" << flags << "(" << flagsForStats
              << ") packageName:" << packageName
              << " selectedDeviceId:" << selectedDeviceId
              << " callerName:" << callerName << "(" << callerNameForStats
              << ") source:" << source << "(" << sourceForStats
              << ") logSessionId:" << logSessionId << "(" << logSessionIdForStats
              << ")";
        if (clientCalled
                && mAudioAnalytics.mDeliverStatistics) {
            const auto [ result, str ] = sendToStatsd(AudioRecordDeviceUsageFields,
                    CONDITION(android::util::MEDIAMETRICS_AUDIORECORDDEVICEUSAGE_REPORTED)
                    , ENUM_EXTRACT(inputDeviceBits)
                    , inputDeviceNames.c_str()
                    , deviceTimeNs
                    , ENUM_EXTRACT(encodingForStats)
                    , frameCount
                    , intervalCount
                    , sampleRate
                    , ENUM_EXTRACT(flagsForStats)
                    , packageName.c_str()
                    , selectedDeviceId
                    , ENUM_EXTRACT(callerNameForStats)
                    , ENUM_EXTRACT(sourceForStats)
                    , logSessionIdForStats.c_str()
                    );
            ALOGV("%s: statsd %s", __func__, str.c_str());
            mAudioAnalytics.mStatsdLog->log(
                    android::util::MEDIAMETRICS_AUDIORECORDDEVICEUSAGE_REPORTED, str);
        }
    } break;
    case THREAD: {
        std::string type;
        mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_TYPE, &type);
        int32_t underrun = 0;
        mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_UNDERRUN, &underrun);
        const bool isInput = types::isInputThreadType(type);
        const auto encodingForStats = types::lookup<types::ENCODING, short_enum_type_t>(encoding);
        const auto flagsForStats =
                (isInput ? types::lookup<types::INPUT_FLAG, short_enum_type_t>(flags)
                        : types::lookup<types::OUTPUT_FLAG, short_enum_type_t>(flags));
        const auto typeForStats = types::lookup<types::THREAD_TYPE, short_enum_type_t>(type);
        LOG(LOG_LEVEL) << "key:" << key
              << " id:" << id
              << " inputDevices:" << inputDevices << "(" << inputDeviceBits
              << ") outputDevices:" << outputDevices << "(" << outputDeviceBits
              << ") inputDeviceNames:" << inputDeviceNames
              << " outputDeviceNames:" << outputDeviceNames
              << " deviceTimeNs:" << deviceTimeNs
              << " encoding:" << encoding << "(" << encodingForStats
              << ") frameCount:" << frameCount
              << " intervalCount:" << intervalCount
              << " sampleRate:" << sampleRate
              << " underrun:" << underrun
              << " flags:" << flags << "(" << flagsForStats
              << ") type:" << type << "(" << typeForStats
              << ")";
        if (mAudioAnalytics.mDeliverStatistics) {
            const auto [ result, str ] = sendToStatsd(AudioThreadDeviceUsageFields,
                CONDITION(android::util::MEDIAMETRICS_AUDIOTHREADDEVICEUSAGE_REPORTED)
                , isInput ? ENUM_EXTRACT(inputDeviceBits) : ENUM_EXTRACT(outputDeviceBits)
                , isInput ? inputDeviceNames.c_str() : outputDeviceNames.c_str()
                , deviceTimeNs
                , ENUM_EXTRACT(encodingForStats)
                , frameCount
                , intervalCount
                , sampleRate
                , ENUM_EXTRACT(flagsForStats)
                , underrun
                , ENUM_EXTRACT(typeForStats)
            );
            ALOGV("%s: statsd %s", __func__, str.c_str());
            mAudioAnalytics.mStatsdLog->log(
                    android::util::MEDIAMETRICS_AUDIOTHREADDEVICEUSAGE_REPORTED, str);
        }
    } break;
    case TRACK: {
        std::string callerName;
        const bool clientCalled = mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_CALLERNAME, &callerName) == OK;
        std::string contentType;
        mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_CONTENTTYPE, &contentType);
        double deviceLatencyMs = 0.;
        mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_DEVICELATENCYMS, &deviceLatencyMs);
        double deviceStartupMs = 0.;
        mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_DEVICESTARTUPMS, &deviceStartupMs);
        double deviceVolume = 0.;
        mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_DEVICEVOLUME, &deviceVolume);
        std::string packageName;
        int64_t versionCode = 0;
        int32_t uid = -1;
        mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_ALLOWUID, &uid);
        if (uid != -1) {
            std::tie(packageName, versionCode) =
                    MediaMetricsService::getSanitizedPackageNameAndVersionCode(uid);
        }
        double playbackPitch = 0.;
        mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_PLAYBACK_PITCH, &playbackPitch);
        double playbackSpeed = 0.;
        mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_PLAYBACK_SPEED, &playbackSpeed);
        int32_t selectedDeviceId = 0;
        mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_SELECTEDDEVICEID, &selectedDeviceId);
        std::string streamType;
        mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_STREAMTYPE, &streamType);
        std::string traits;
        mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_TRAITS, &traits);
        int32_t underrun = 0;
        mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_UNDERRUN, &underrun);
        std::string usage;
        mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_USAGE, &usage);
        std::string logSessionId;
        mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_LOGSESSIONID, &logSessionId);
        const auto callerNameForStats =
                types::lookup<types::CALLER_NAME, short_enum_type_t>(callerName);
        const auto contentTypeForStats =
                types::lookup<types::CONTENT_TYPE, short_enum_type_t>(contentType);
        const auto encodingForStats = types::lookup<types::ENCODING, short_enum_type_t>(encoding);
        const auto flagsForStats = types::lookup<types::OUTPUT_FLAG, short_enum_type_t>(flags);
        const auto streamTypeForStats =
                types::lookup<types::STREAM_TYPE, short_enum_type_t>(streamType);
        const auto traitsForStats =
                 types::lookup<types::TRACK_TRAITS, short_enum_type_t>(traits);
        const auto usageForStats = types::lookup<types::USAGE, short_enum_type_t>(usage);
        const auto logSessionIdForStats = ValidateId::get()->validateId(logSessionId);
        LOG(LOG_LEVEL) << "key:" << key
              << " id:" << id
              << " outputDevices:" << outputDevices << "(" << outputDeviceBits
              << ") outputDeviceNames:" << outputDeviceNames
              << " deviceTimeNs:" << deviceTimeNs
              << " encoding:" << encoding << "(" << encodingForStats
              << ") frameCount:" << frameCount
              << " intervalCount:" << intervalCount
              << " sampleRate:" << sampleRate
              << " underrun:" << underrun
              << " flags:" << flags << "(" << flagsForStats
              << ") callerName:" << callerName << "(" << callerNameForStats
              << ") contentType:" << contentType << "(" << contentTypeForStats
              << ") deviceLatencyMs:" << deviceLatencyMs
              << " deviceStartupMs:" << deviceStartupMs
              << " deviceVolume:" << deviceVolume
              << " packageName:" << packageName
              << " playbackPitch:" << playbackPitch
              << " playbackSpeed:" << playbackSpeed
              << " selectedDeviceId:" << selectedDeviceId
              << " streamType:" << streamType << "(" << streamTypeForStats
              << ") traits:" << traits << "(" << traitsForStats
              << ") usage:" << usage << "(" << usageForStats
              << ") logSessionId:" << logSessionId << "(" << logSessionIdForStats
              << ")";
        if (clientCalled
                && mAudioAnalytics.mDeliverStatistics) {
            const auto [ result, str ] = sendToStatsd(AudioTrackDeviceUsageFields,
                    CONDITION(android::util::MEDIAMETRICS_AUDIOTRACKDEVICEUSAGE_REPORTED)
                    , ENUM_EXTRACT(outputDeviceBits)
                    , outputDeviceNames.c_str()
                    , deviceTimeNs
                    , ENUM_EXTRACT(encodingForStats)
                    , frameCount
                    , intervalCount
                    , sampleRate
                    , ENUM_EXTRACT(flagsForStats)
                    , underrun
                    , packageName.c_str()
                    , (float)deviceLatencyMs
                    , (float)deviceStartupMs
                    , (float)deviceVolume
                    , selectedDeviceId
                    , ENUM_EXTRACT(streamTypeForStats)
                    , ENUM_EXTRACT(usageForStats)
                    , ENUM_EXTRACT(contentTypeForStats)
                    , ENUM_EXTRACT(callerNameForStats)
                    , ENUM_EXTRACT(traitsForStats)
                    , logSessionIdForStats.c_str()
                    );
            ALOGV("%s: statsd %s", __func__, str.c_str());
            mAudioAnalytics.mStatsdLog->log(
                    android::util::MEDIAMETRICS_AUDIOTRACKDEVICEUSAGE_REPORTED, str);
        }
        } break;
    }
    if (isBluetooth) {
    }
}
void AudioAnalytics::DeviceConnection::a2dpConnected(
       const std::shared_ptr<const android::mediametrics::Item> &item) {
    const std::string& key = item->getKey();
    const int64_t atNs = item->getTimestamp();
    {
        std::lock_guard l(mLock);
        mA2dpConnectionServiceNs = atNs;
        ++mA2dpConnectionServices;
        if (mA2dpConnectionRequestNs == 0) {
            mAudioAnalytics.mTimedAction.postIn(std::chrono::seconds(5), [this](){ expire(); });
        }
    }
    std::string name;
    item->get(AMEDIAMETRICS_PROP_NAME, &name);
    ALOGD("(key=%s) a2dp connected device:%s atNs:%lld",
            key.c_str(), name.c_str(), (long long)atNs);
}
void AudioAnalytics::DeviceConnection::createPatch(
       const std::shared_ptr<const android::mediametrics::Item> &item) {
    std::lock_guard l(mLock);
    if (mA2dpConnectionServiceNs == 0) return;
    const std::string& key = item->getKey();
    std::string outputDevices;
    item->get(AMEDIAMETRICS_PROP_OUTPUTDEVICES, &outputDevices);
    if (outputDevices.find("AUDIO_DEVICE_OUT_BLUETOOTH_A2DP") != std::string::npos) {
        int64_t timeDiffNs = item->getTimestamp();
        if (mA2dpConnectionRequestNs == 0) {
            ALOGD("%s: A2DP create patch didn't see a connection request", __func__);
            timeDiffNs -= mA2dpConnectionServiceNs;
        } else {
            timeDiffNs -= mA2dpConnectionRequestNs;
        }
        mA2dpConnectionRequestNs = 0;
        mA2dpConnectionServiceNs = 0;
        ++mA2dpConnectionSuccesses;
        const auto connectionTimeMs = float((double)timeDiffNs * 1e-6);
        const auto outputDeviceBits = types::lookup<types::OUTPUT_DEVICE, long_enum_type_t>(
                "AUDIO_DEVICE_OUT_BLUETOOTH_A2DP");
        LOG(LOG_LEVEL) << "key:" << key
                << " A2DP SUCCESS"
                << " outputDevices:" << outputDeviceBits
                << " deviceName:" << mA2dpDeviceName
                << " connectionTimeMs:" << connectionTimeMs;
        if (mAudioAnalytics.mDeliverStatistics) {
            const long_enum_type_t inputDeviceBits{};
            const auto [ result, str ] = sendToStatsd(AudioDeviceConnectionFields,
                    CONDITION(android::util::MEDIAMETRICS_AUDIODEVICECONNECTION_REPORTED)
                    , ENUM_EXTRACT(inputDeviceBits)
                    , ENUM_EXTRACT(outputDeviceBits)
                    , mA2dpDeviceName.c_str()
                    , types::DEVICE_CONNECTION_RESULT_SUCCESS
                    , connectionTimeMs
                    , 1
                    );
            ALOGV("%s: statsd %s", __func__, str.c_str());
            mAudioAnalytics.mStatsdLog->log(
                    android::util::MEDIAMETRICS_AUDIODEVICECONNECTION_REPORTED, str);
        }
    }
}
void AudioAnalytics::DeviceConnection::postBluetoothA2dpDeviceConnectionStateSuppressNoisyIntent(
        const std::shared_ptr<const android::mediametrics::Item> &item) {
    const int64_t atNs = item->getTimestamp();
    const std::string& key = item->getKey();
    std::string state;
    item->get(AMEDIAMETRICS_PROP_STATE, &state);
    if (state != "connected") return;
    std::string name;
    item->get(AMEDIAMETRICS_PROP_NAME, &name);
    {
        std::lock_guard l(mLock);
        mA2dpConnectionRequestNs = atNs;
        ++mA2dpConnectionRequests;
        mA2dpDeviceName = SUPPRESSED;
    }
    ALOGD("(key=%s) a2dp connection name:%s request atNs:%lld",
            key.c_str(), name.c_str(), (long long)atNs);
    mAudioAnalytics.mTimedAction.postIn(std::chrono::seconds(5), [this](){ expire(); });
}
void AudioAnalytics::DeviceConnection::expire() {
    std::lock_guard l(mLock);
    if (mA2dpConnectionRequestNs == 0) return;
    const long_enum_type_t inputDeviceBits{};
    const auto outputDeviceBits = types::lookup<types::OUTPUT_DEVICE, long_enum_type_t>(
            "AUDIO_DEVICE_OUT_BLUETOOTH_A2DP");
    if (mA2dpConnectionServiceNs == 0) {
        ++mA2dpConnectionJavaServiceCancels;
        LOG(LOG_LEVEL) << "A2DP CANCEL"
                << " outputDevices:" << outputDeviceBits
                << " deviceName:" << mA2dpDeviceName;
        if (mAudioAnalytics.mDeliverStatistics) {
            const auto [ result, str ] = sendToStatsd(AudioDeviceConnectionFields,
                    CONDITION(android::util::MEDIAMETRICS_AUDIODEVICECONNECTION_REPORTED)
                    , ENUM_EXTRACT(inputDeviceBits)
                    , ENUM_EXTRACT(outputDeviceBits)
                    , mA2dpDeviceName.c_str()
                    , types::DEVICE_CONNECTION_RESULT_JAVA_SERVICE_CANCEL
                    , 0.f
                    , 1
                    );
            ALOGV("%s: statsd %s", __func__, str.c_str());
            mAudioAnalytics.mStatsdLog->log(
                    android::util::MEDIAMETRICS_AUDIODEVICECONNECTION_REPORTED, str);
        }
        return;
    }
    mA2dpConnectionRequestNs = 0;
    mA2dpConnectionServiceNs = 0;
    ++mA2dpConnectionUnknowns;
    LOG(LOG_LEVEL) << "A2DP UNKNOWN"
            << " outputDevices:" << outputDeviceBits
            << " deviceName:" << mA2dpDeviceName;
    if (mAudioAnalytics.mDeliverStatistics) {
        const auto [ result, str ] = sendToStatsd(AudioDeviceConnectionFields,
                CONDITION(android::util::MEDIAMETRICS_AUDIODEVICECONNECTION_REPORTED)
                , ENUM_EXTRACT(inputDeviceBits)
                , ENUM_EXTRACT(outputDeviceBits)
                , mA2dpDeviceName.c_str()
                , types::DEVICE_CONNECTION_RESULT_UNKNOWN
                , 0.f
                , 1
                );
        ALOGV("%s: statsd %s", __func__, str.c_str());
        mAudioAnalytics.mStatsdLog->log(
                android::util::MEDIAMETRICS_AUDIODEVICECONNECTION_REPORTED, str);
    }
}
void AudioAnalytics::AAudioStreamInfo::endAAudioStream(
        const std::shared_ptr<const android::mediametrics::Item> &item, CallerPath path) const {
    const std::string& key = item->getKey();
    std::string directionStr;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_DIRECTION, &directionStr);
    const auto direction = types::lookup<types::AAUDIO_DIRECTION, int32_t>(directionStr);
    int32_t framesPerBurst = -1;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_BURSTFRAMES, &framesPerBurst);
    int32_t bufferSizeInFrames = -1;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_BUFFERSIZEFRAMES, &bufferSizeInFrames);
    int32_t bufferCapacityInFrames = -1;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_BUFFERCAPACITYFRAMES, &bufferCapacityInFrames);
    int32_t channelCount = -1;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_CHANNELCOUNT, &channelCount);
    if (channelCount == -1) {
        int32_t channelMask = 0;
        mAudioAnalytics.mAnalyticsState->timeMachine().get(
                key, AMEDIAMETRICS_PROP_CHANNELMASK, &channelMask);
        if (channelMask != 0) {
            switch (direction) {
                case 1:
                    channelCount = (int32_t)audio_channel_count_from_out_mask(channelMask);
                    break;
                case 2:
                    channelCount = (int32_t)audio_channel_count_from_in_mask(channelMask);
                    break;
                default:
                    ALOGW("Invalid direction %d", direction);
            }
        }
    }
    int64_t totalFramesTransferred = -1;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_FRAMESTRANSFERRED, &totalFramesTransferred);
    std::string perfModeRequestedStr;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_PERFORMANCEMODE, &perfModeRequestedStr);
    const auto perfModeRequested =
            types::lookup<types::AAUDIO_PERFORMANCE_MODE, int32_t>(perfModeRequestedStr);
    std::string perfModeActualStr;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_PERFORMANCEMODEACTUAL, &perfModeActualStr);
    const auto perfModeActual =
            types::lookup<types::AAUDIO_PERFORMANCE_MODE, int32_t>(perfModeActualStr);
    std::string sharingModeActualStr;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_SHARINGMODEACTUAL, &sharingModeActualStr);
    const auto sharingModeActual =
            types::lookup<types::AAUDIO_SHARING_MODE, int32_t>(sharingModeActualStr);
    int32_t xrunCount = -1;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_UNDERRUN, &xrunCount);
    std::string serializedDeviceTypes;
    std::string formatAppStr;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_ENCODINGCLIENT, &formatAppStr);
    const auto formatApp = types::lookup<types::ENCODING, int32_t>(formatAppStr);
    std::string formatDeviceStr;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_ENCODING, &formatDeviceStr);
    const auto formatDevice = types::lookup<types::ENCODING, int32_t>(formatDeviceStr);
    std::string logSessionId;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_LOGSESSIONID, &logSessionId);
    int32_t sampleRate = 0;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_SAMPLERATE, &sampleRate);
    std::string contentTypeStr;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_CONTENTTYPE, &contentTypeStr);
    const auto contentType = types::lookup<types::CONTENT_TYPE, int32_t>(contentTypeStr);
    std::string sharingModeRequestedStr;
    mAudioAnalytics.mAnalyticsState->timeMachine().get(
            key, AMEDIAMETRICS_PROP_SHARINGMODE, &sharingModeRequestedStr);
    const auto sharingModeRequested =
            types::lookup<types::AAUDIO_SHARING_MODE, int32_t>(sharingModeRequestedStr);
    LOG(LOG_LEVEL) << "key:" << key
            << " path:" << path
            << " direction:" << direction << "(" << directionStr << ")"
            << " frames_per_burst:" << framesPerBurst
            << " buffer_size:" << bufferSizeInFrames
            << " buffer_capacity:" << bufferCapacityInFrames
            << " channel_count:" << channelCount
            << " total_frames_transferred:" << totalFramesTransferred
            << " perf_mode_requested:" << perfModeRequested << "(" << perfModeRequestedStr << ")"
            << " perf_mode_actual:" << perfModeActual << "(" << perfModeActualStr << ")"
            << " sharing:" << sharingModeActual << "(" << sharingModeActualStr << ")"
            << " xrun_count:" << xrunCount
            << " device_type:" << serializedDeviceTypes
            << " format_app:" << formatApp << "(" << formatAppStr << ")"
            << " format_device: " << formatDevice << "(" << formatDeviceStr << ")"
            << " log_session_id: " << logSessionId
            << " sample_rate: " << sampleRate
            << " content_type: " << contentType << "(" << contentTypeStr << ")"
            << " sharing_requested:" << sharingModeRequested
                    << "(" << sharingModeRequestedStr << ")";
    if (mAudioAnalytics.mDeliverStatistics) {
        android::util::BytesField bf_serialized(
            serializedDeviceTypes.c_str(), serializedDeviceTypes.size());
        const auto result = sendToStatsd(
                CONDITION(android::util::MEDIAMETRICS_AAUDIOSTREAM_REPORTED)
                , path
                , direction
                , framesPerBurst
                , bufferSizeInFrames
                , bufferCapacityInFrames
                , channelCount
                , totalFramesTransferred
                , perfModeRequested
                , perfModeActual
                , sharingModeActual
                , xrunCount
                , bf_serialized
                , formatApp
                , formatDevice
                , logSessionId.c_str()
                , sampleRate
                , contentType
                , sharingModeRequested
                );
        std::stringstream ss;
        ss << "result:" << result;
        const auto fieldsStr = printFields(AAudioStreamFields,
                CONDITION(android::util::MEDIAMETRICS_AAUDIOSTREAM_REPORTED)
                , path
                , direction
                , framesPerBurst
                , bufferSizeInFrames
                , bufferCapacityInFrames
                , channelCount
                , totalFramesTransferred
                , perfModeRequested
                , perfModeActual
                , sharingModeActual
                , xrunCount
                , serializedDeviceTypes.c_str()
                , formatApp
                , formatDevice
                , logSessionId.c_str()
                , sampleRate
                , contentType
                , sharingModeRequested
                );
        ss << " " << fieldsStr;
        std::string str = ss.str();
        ALOGV("%s: statsd %s", __func__, str.c_str());
        mAudioAnalytics.mStatsdLog->log(android::util::MEDIAMETRICS_AAUDIOSTREAM_REPORTED, str);
    }
}
void AudioAnalytics::newState()
{
    mPreviousAnalyticsState.set(std::make_shared<AnalyticsState>(
            *mAnalyticsState.get()));
    mAnalyticsState->clear();
    const size_t size = mPreviousAnalyticsState->transactionLog().size();
    mTimedAction.postIn(
            std::chrono::seconds(PREVIOUS_STATE_EXPIRE_SEC), [this, size](){
        if (mPreviousAnalyticsState->transactionLog().size() == size) {
            ALOGD("expiring previous audio state after %d seconds.",
                    PREVIOUS_STATE_EXPIRE_SEC);
            mPreviousAnalyticsState->clear();
        }
    });
}
void AudioAnalytics::Health::onAudioServerStart(Module module,
        const std::shared_ptr<const android::mediametrics::Item> &item)
{
    const auto nowTime = std::chrono::system_clock::now();
    if (module == Module::AUDIOFLINGER) {
       {
            std::lock_guard lg(mLock);
            mAudioFlingerCtorTime = nowTime;
            mSimpleLog.log("AudioFlinger ctor");
        }
        mAudioAnalytics.newState();
        return;
    }
    if (module == Module::AUDIOPOLICY) {
        int64_t executionTimeNs = 0;
        (void)item->get(AMEDIAMETRICS_PROP_EXECUTIONTIMENS, &executionTimeNs);
        const float loadTimeMs = executionTimeNs * 1e-6f;
        std::lock_guard lg(mLock);
        const int64_t restarts = mStartCount;
        if (mStopCount == mStartCount) {
            mAudioPolicyCtorTime = nowTime;
            ++mStartCount;
            if (mStopCount == 0) {
                ALOGW("%s: (key=%s) AudioPolicy ctor, loadTimeMs:%f",
                        __func__, item->getKey().c_str(), loadTimeMs);
                mSimpleLog.log("AudioPolicy ctor, loadTimeMs:%f", loadTimeMs);
            } else {
                const float restartMs =
                        std::chrono::duration_cast<std::chrono::duration<float, std::milli>>(
                                mAudioFlingerCtorTime - mStopTime).count();
                ALOGW("%s: (key=%s) AudioPolicy ctor, "
                        "restarts:%lld restartMs:%f loadTimeMs:%f",
                        __func__, item->getKey().c_str(),
                        (long long)restarts, restartMs, loadTimeMs);
                mSimpleLog.log("AudioPolicy ctor restarts:%lld restartMs:%f loadTimeMs:%f",
                        (long long)restarts, restartMs, loadTimeMs);
            }
        } else {
            const float uptimeMs =
                    std::chrono::duration_cast<std::chrono::duration<float, std::milli>>(
                            nowTime - mAudioFlingerCtorTime).count();
            mStopCount = mStartCount;
            mAudioPolicyCtorTime = nowTime;
            ++mStartCount;
            ALOGW("%s: (key=%s) AudioPolicy ctor after uncaught failure, "
                    "mStartCount:%lld mStopCount:%lld uptimeMs:%f loadTimeMs:%f",
                    __func__, item->getKey().c_str(),
                    (long long)mStartCount, (long long)mStopCount, uptimeMs, loadTimeMs);
            mSimpleLog.log("AudioPolicy ctor after uncaught failure, "
                    "restarts:%lld uptimeMs:%f loadTimeMs:%f",
                    (long long)restarts, uptimeMs, loadTimeMs);
        }
    }
}
void AudioAnalytics::Health::onAudioServerTimeout(Module module,
        const std::shared_ptr<const android::mediametrics::Item> &item)
{
    std::string moduleName = getModuleName(module);
    int64_t methodCode{};
    std::string methodName;
    (void)item->get(AMEDIAMETRICS_PROP_METHODCODE, &methodCode);
    (void)item->get(AMEDIAMETRICS_PROP_METHODNAME, &methodName);
    std::lock_guard lg(mLock);
    if (mStopCount >= mStartCount) {
        ALOGD("%s: (key=%s) %s timeout %s(%lld) "
            "unmatched mStopCount(%lld) >= mStartCount(%lld), ignoring",
            __func__, item->getKey().c_str(), moduleName.c_str(),
            methodName.c_str(), (long long)methodCode,
            (long long)mStopCount, (long long)mStartCount);
        return;
    }
    const int64_t restarts = mStartCount - 1;
    ++mStopCount;
    mStopTime = std::chrono::system_clock::now();
    const float uptimeMs = std::chrono::duration_cast<std::chrono::duration<float, std::milli>>(
            mStopTime - mAudioFlingerCtorTime).count();
    ALOGW("%s: (key=%s) %s timeout %s(%lld) restarts:%lld uptimeMs:%f",
         __func__, item->getKey().c_str(), moduleName.c_str(),
         methodName.c_str(), (long long)methodCode,
         (long long)restarts, uptimeMs);
    mSimpleLog.log("%s timeout %s(%lld) restarts:%lld uptimeMs:%f",
            moduleName.c_str(), methodName.c_str(), (long long)methodCode,
            (long long)restarts, uptimeMs);
}
std::pair<std::string, int32_t> AudioAnalytics::Health::dump(
        int32_t lines, const char *prefix) const
{
    std::lock_guard lg(mLock);
    std::string s = mSimpleLog.dumpToString(prefix == nullptr ? "" : prefix, lines);
    size_t n = std::count(s.begin(), s.end(), '\n');
    return { s, n };
}
<<<<<<< HEAD
static int32_t classifySettingEvent(bool isSetAlready, bool withinBoot) {
    if (isSetAlready) {
        return util::MEDIAMETRICS_SPATIALIZER_DEVICE_ENABLED_REPORTED__EVENT__SPATIALIZER_SETTING_EVENT_NORMAL;
    }
    if (withinBoot) {
        return util::MEDIAMETRICS_SPATIALIZER_DEVICE_ENABLED_REPORTED__EVENT__SPATIALIZER_SETTING_EVENT_BOOT;
    }
    return util::MEDIAMETRICS_SPATIALIZER_DEVICE_ENABLED_REPORTED__EVENT__SPATIALIZER_SETTING_EVENT_FIRST;
}
void AudioAnalytics::Spatializer::onEvent(
        const std::shared_ptr<const android::mediametrics::Item> &item)
{
    const auto key = item->getKey();
    if (!startsWith(key, AMEDIAMETRICS_KEY_PREFIX_AUDIO_SPATIALIZER)) return;
    const std::string suffix =
            key.substr(std::size(AMEDIAMETRICS_KEY_PREFIX_AUDIO_SPATIALIZER) - 1);
    std::string eventStr;
    (void)item->get(AMEDIAMETRICS_PROP_EVENT, &eventStr);
    const size_t delim = suffix.find('.');
    if (delim == suffix.npos) {
        std::string headTrackingModes;
        (void)item->get(AMEDIAMETRICS_PROP_HEADTRACKINGMODES, &headTrackingModes);
        std::string levels;
        (void)item->get(AMEDIAMETRICS_PROP_LEVELS, &levels);
        std::string modes;
        (void)item->get(AMEDIAMETRICS_PROP_MODES, &modes);
        std::string channelMasks;
        (void)item->get(AMEDIAMETRICS_PROP_CHANNELMASKS, &channelMasks);
        LOG(LOG_LEVEL) << "key:" << key
                << " headTrackingModes:" << headTrackingModes
                << " levels:" << levels
                << " modes:" << modes
                << " channelMasks:" << channelMasks
                ;
        const std::vector<int32_t> headTrackingModesVector =
                types::vectorFromMap(headTrackingModes, types::getHeadTrackingModeMap());
        const std::vector<int32_t> levelsVector =
                types::vectorFromMap(levels, types::getSpatializerLevelMap());
        const std::vector<int32_t> modesVector =
                types::vectorFromMap(modes, types::getSpatializerModeMap());
        const std::vector<int64_t> channelMasksVector =
                types::channelMaskVectorFromString(channelMasks);
        const auto [ result, str ] = sendToStatsd(SpatializerCapabilitiesFields,
                CONDITION(android::util::MEDIAMETRICS_SPATIALIZERCAPABILITIES_REPORTED)
                , headTrackingModesVector
                , levelsVector
                , modesVector
                , channelMasksVector
                );
        mAudioAnalytics.mStatsdLog->log(
                android::util::MEDIAMETRICS_SPATIALIZERCAPABILITIES_REPORTED, str);
        std::lock_guard lg(mLock);
        if (mFirstCreateTimeNs == 0) {
            mFirstCreateTimeNs = item->getTimestamp();
        }
        mSimpleLog.log("%s suffix: %s item: %s",
                __func__, suffix.c_str(), item->toString().c_str());
    } else {
        std::string subtype = suffix.substr(0, delim);
        if (subtype != "device") return;
        const std::string deviceType = suffix.substr(std::size("device.") - 1);
        const int32_t deviceTypeStatsd =
                types::lookup<types::AUDIO_DEVICE_INFO_TYPE, int32_t>(deviceType);
        std::string address;
        (void)item->get(AMEDIAMETRICS_PROP_ADDRESS, &address);
        std::string enabled;
        (void)item->get(AMEDIAMETRICS_PROP_ENABLED, &enabled);
        std::string hasHeadTracker;
        (void)item->get(AMEDIAMETRICS_PROP_HASHEADTRACKER, &hasHeadTracker);
        std::string headTrackerEnabled;
        (void)item->get(AMEDIAMETRICS_PROP_HEADTRACKERENABLED, &headTrackerEnabled);
        std::lock_guard lg(mLock);
        std::string deviceKey(deviceType);
        deviceKey.append("_").append(address);
        DeviceState& deviceState = mDeviceStateMap[deviceKey];
        const bool withinBoot =
                item->getTimestamp() - mFirstCreateTimeNs < kBootDurationThreshold;
        if (!enabled.empty()) {
            if (enabled != deviceState.enabled) {
                const int32_t settingEventStatsd =
                        classifySettingEvent(!deviceState.enabled.empty(), withinBoot);
                deviceState.enabled = enabled;
                const bool enabledStatsd = enabled == "true";
                const auto [ result, str ] = sendToStatsd(SpatializerDeviceEnabledFields,
                        CONDITION(android::util::MEDIAMETRICS_SPATIALIZERDEVICEENABLED_REPORTED)
                        , deviceTypeStatsd
                        , settingEventStatsd
                        , enabledStatsd
                        );
                mAudioAnalytics.mStatsdLog->log(
                        android::util::MEDIAMETRICS_SPATIALIZERDEVICEENABLED_REPORTED, str);
            }
        }
        if (!hasHeadTracker.empty()) {
            if (hasHeadTracker != deviceState.hasHeadTracker) {
                const int32_t settingEventStatsd =
                        classifySettingEvent(!deviceState.hasHeadTracker.empty(), withinBoot);
                deviceState.hasHeadTracker = hasHeadTracker;
                const bool supportedStatsd = hasHeadTracker == "true";
                const auto [ result, str ] = sendToStatsd(HeadTrackerDeviceSupportedFields,
                        CONDITION(android::util::MEDIAMETRICS_HEADTRACKERDEVICESUPPORTED_REPORTED)
                        , deviceTypeStatsd
                        , settingEventStatsd
                        , supportedStatsd
                        );
                mAudioAnalytics.mStatsdLog->log(
                        android::util::MEDIAMETRICS_HEADTRACKERDEVICESUPPORTED_REPORTED, str);
            }
        }
        if (!headTrackerEnabled.empty()) {
            if (headTrackerEnabled != deviceState.headTrackerEnabled) {
                const int32_t settingEventStatsd =
                        classifySettingEvent(!deviceState.headTrackerEnabled.empty(), withinBoot);
                deviceState.headTrackerEnabled = headTrackerEnabled;
                const bool enabledStatsd = headTrackerEnabled == "true";
                const auto [ result, str ] = sendToStatsd(HeadTrackerDeviceEnabledFields,
                        CONDITION(android::util::MEDIAMETRICS_HEADTRACKERDEVICEENABLED_REPORTED)
                        , deviceTypeStatsd
                        , settingEventStatsd
                        , enabledStatsd
                        );
                mAudioAnalytics.mStatsdLog->log(
                        android::util::MEDIAMETRICS_HEADTRACKERDEVICEENABLED_REPORTED, str);
            }
        }
        mSimpleLog.log("%s deviceKey: %s item: %s",
                __func__, deviceKey.c_str(), item->toString().c_str());
    }
}
std::pair<std::string, int32_t> AudioAnalytics::Spatializer::dump(
        int32_t lines, const char *prefix) const
{
    std::lock_guard lg(mLock);
    std::string s = mSimpleLog.dumpToString(prefix == nullptr ? "" : prefix, lines);
    size_t n = std::count(s.begin(), s.end(), '\n');
    return { s, n };
}
||||||| 3be023edb8
=======
void AudioAnalytics::Spatializer::onEvent(
        const std::shared_ptr<const android::mediametrics::Item> &item)
{
    const auto key = item->getKey();
    if (!startsWith(key, AMEDIAMETRICS_KEY_PREFIX_AUDIO_SPATIALIZER)) return;
    const std::string suffix =
            key.substr(std::size(AMEDIAMETRICS_KEY_PREFIX_AUDIO_SPATIALIZER) - 1);
    std::string eventStr;
    (void)item->get(AMEDIAMETRICS_PROP_EVENT, &eventStr);
    const size_t delim = suffix.find('.');
    if (delim == suffix.npos) {
        std::string headTrackingModes;
        (void)item->get(AMEDIAMETRICS_PROP_HEADTRACKINGMODES, &headTrackingModes);
        std::string levels;
        (void)item->get(AMEDIAMETRICS_PROP_LEVELS, &levels);
        std::string modes;
        (void)item->get(AMEDIAMETRICS_PROP_MODES, &modes);
        int32_t channelMask = 0;
        (void)item->get(AMEDIAMETRICS_PROP_CHANNELMASK, &channelMask);
        LOG(LOG_LEVEL) << "key:" << key
                << " headTrackingModes:" << headTrackingModes
                << " levels:" << levels
                << " modes:" << modes
                << " channelMask:" << channelMask
                ;
        const std::vector<int32_t> headTrackingModesVector =
                types::vectorFromMap(headTrackingModes, types::getHeadTrackingModeMap());
        const std::vector<int32_t> levelsVector =
                types::vectorFromMap(levels, types::getSpatializerLevelMap());
        const std::vector<int32_t> modesVector =
                types::vectorFromMap(modes, types::getSpatializerModeMap());
        std::lock_guard lg(mLock);
        mSimpleLog.log("%s suffix: %s item: %s",
                __func__, suffix.c_str(), item->toString().c_str());
    } else {
        std::string subtype = suffix.substr(0, delim);
        if (subtype != "device") return;
        std::string deviceType = suffix.substr(std::size("device.") - 1);
        std::string enabled;
        (void)item->get(AMEDIAMETRICS_PROP_ENABLED, &enabled);
        std::string hasHeadTracker;
        (void)item->get(AMEDIAMETRICS_PROP_HASHEADTRACKER, &hasHeadTracker);
        std::string headTrackerEnabled;
        (void)item->get(AMEDIAMETRICS_PROP_HEADTRACKERENABLED, &headTrackerEnabled);
        std::lock_guard lg(mLock);
        DeviceState& deviceState = mDeviceStateMap[deviceType];
        if (!enabled.empty()) {
            if (enabled != deviceState.enabled) {
                deviceState.enabled = enabled;
                const bool enabledStatsd = enabled == "true";
                (void)mAudioAnalytics;
                (void)enabledStatsd;
            }
        }
        if (!hasHeadTracker.empty()) {
            if (hasHeadTracker != deviceState.hasHeadTracker) {
                deviceState.hasHeadTracker = hasHeadTracker;
                const bool supportedStatsd = hasHeadTracker == "true";
                (void)supportedStatsd;
            }
        }
        if (!headTrackerEnabled.empty()) {
            if (headTrackerEnabled != deviceState.headTrackerEnabled) {
                deviceState.headTrackerEnabled = headTrackerEnabled;
                const bool enabledStatsd = headTrackerEnabled == "true";
                (void)enabledStatsd;
            }
        }
        mSimpleLog.log("%s deviceType: %s item: %s",
                __func__, deviceType.c_str(), item->toString().c_str());
    }
}
std::pair<std::string, int32_t> AudioAnalytics::Spatializer::dump(
        int32_t lines, const char *prefix) const
{
    std::lock_guard lg(mLock);
    std::string s = mSimpleLog.dumpToString(prefix == nullptr ? "" : prefix, lines);
    size_t n = std::count(s.begin(), s.end(), '\n');
    return { s, n };
}
>>>>>>> 4591aab2
}
