#define LOG_TAG "AudioFlinger"
#define ATRACE_TAG ATRACE_TAG_AUDIO
#include "Threads.h"
#include "Client.h"
#include "IAfEffect.h"
#include "MelReporter.h"
#include "ResamplerBufferProvider.h"
#include <afutils/DumpTryLock.h>
#include <afutils/Permission.h>
#include <afutils/TypedLogger.h>
#include <afutils/Vibrator.h>
#include <audio_utils/MelProcessor.h>
#include <audio_utils/Metadata.h>
#ifdef DEBUG_CPU_USAGE
#include <audio_utils/Statistics.h>
#include <cpustats/ThreadCpuUsage.h>
#endif
#include <audio_utils/channels.h>
#include <audio_utils/format.h>
#include <audio_utils/minifloat.h>
#include <audio_utils/mono_blend.h>
#include <audio_utils/primitives.h>
#include <audio_utils/safe_math.h>
#include <audiomanager/AudioManager.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/PersistableBundle.h>
#include <cutils/bitops.h>
#include <cutils/properties.h>
#include <fastpath/AutoPark.h>
#include <media/AudioContainers.h>
#include <media/AudioDeviceTypeAddr.h>
#include <media/AudioParameter.h>
#include <media/AudioResamplerPublic.h>
#ifdef ADD_BATTERY_DATA
#include <media/IMediaPlayerService.h>
#include <media/IMediaDeathNotifier.h>
#endif
#include <media/MmapStreamCallback.h>
#include <media/RecordBufferConverter.h>
#include <media/TypeConverter.h>
#include <media/audiohal/EffectsFactoryHalInterface.h>
#include <media/audiohal/StreamHalInterface.h>
#include <media/nbaio/AudioStreamInSource.h>
#include <media/nbaio/AudioStreamOutSink.h>
#include <media/nbaio/MonoPipe.h>
#include <media/nbaio/MonoPipeReader.h>
#include <media/nbaio/Pipe.h>
#include <media/nbaio/PipeReader.h>
#include <media/nbaio/SourceAudioBufferProvider.h>
#include <mediautils/BatteryNotifier.h>
#include <mediautils/Process.h>
#include <mediautils/SchedulingPolicyService.h>
#include <mediautils/ServiceUtilities.h>
#include <powermanager/PowerManager.h>
#include <private/android_filesystem_config.h>
#include <private/media/AudioTrackShared.h>
#include <system/audio_effects/effect_aec.h>
#include <system/audio_effects/effect_downmix.h>
#include <system/audio_effects/effect_ns.h>
#include <system/audio_effects/effect_spatializer.h>
#include <utils/Log.h>
#include <utils/Trace.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <math.h>
#include <memory>
#include <pthread.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#ifdef VERY_VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif
#define max(a,b) ((a) > (b) ? (a) : (b))
template <typename T>
static inline T min(const T& a, const T& b)
{
    return a < b ? a : b;
}
namespace android {
using audioflinger::SyncEvent;
using media::IEffectClient;
using content::AttributionSourceState;
static constexpr int32_t kMaxSharedAudioHistoryMs = 5000;
static const int8_t kMaxTrackRetries = 50;
static const int8_t kMaxTrackStartupRetries = 50;
static const int32_t kMaxTrackRetriesDirectMs = 200;
static const nsecs_t kWarningThrottleNs = seconds(5);
static const int kRecordThreadSleepUs = 5000;
static const nsecs_t kConfigEventTimeoutNs = seconds(2);
static const uint32_t kMinThreadSleepTimeUs = 5000;
static const uint32_t kMaxThreadSleepTimeShift = 2;
static const uint32_t kMinNormalSinkBufferSizeMs = 20;
static const uint32_t kMaxNormalSinkBufferSizeMs = 24;
static const uint32_t kMinNormalCaptureBufferSizeMs = 12;
static const nsecs_t kOffloadStandbyDelayNs = seconds(1);
static const nsecs_t kDirectMinSleepTimeUs = 10000;
static const nsecs_t kMinimumTimeBetweenTimestampChecksNs = 150 * 1000;
#define FMS_20 20
static const enum {
    FastMixer_Never,
    FastMixer_Always,
    FastMixer_Static,
    FastMixer_Dynamic,
} kUseFastMixer = FastMixer_Static;
static const enum {
    FastCapture_Never,
    FastCapture_Always,
    FastCapture_Static,
} kUseFastCapture = FastCapture_Static;
static const int kPriorityAudioApp = 2;
static const int kPriorityFastMixer = 3;
static const int kPriorityFastCapture = 3;
static const int kFastTrackMultiplier = 2;
static const int kFastTrackMultiplierMin = 1;
static const int kFastTrackMultiplierMax = 2;
static int sFastTrackMultiplier = kFastTrackMultiplier;
static const size_t kRecordThreadReadOnlyHeapSize = 0xD000;
static const nsecs_t kDefaultStandbyTimeInNsecs = seconds(3);
static nsecs_t getStandbyTimeInNanos() {
    static nsecs_t standbyTimeInNanos = []() {
        const int ms = property_get_int32("ro.audio.flinger_standbytime_ms",
                    kDefaultStandbyTimeInNsecs / NANOS_PER_MILLISECOND);
        ALOGI("%s: Using %d ms as standby time", __func__, ms);
        return milliseconds(ms);
    }();
    return standbyTimeInNanos;
}
constexpr bool kEnableExtendedChannels = true;
bool IAfThreadBase::isValidPcmSinkChannelMask(audio_channel_mask_t channelMask) {
    switch (audio_channel_mask_get_representation(channelMask)) {
    case AUDIO_CHANNEL_REPRESENTATION_POSITION: {
        const uint32_t channelCount = audio_channel_count_from_out_mask(
                static_cast<audio_channel_mask_t>(channelMask & ~AUDIO_CHANNEL_HAPTIC_ALL));
        const uint32_t maxChannelCount = kEnableExtendedChannels
                ? FCC_LIMIT : FCC_2;
        if (channelCount < FCC_2
                || channelCount > maxChannelCount) {
            return false;
        }
        return audio_channel_position_mask_is_out_canonical(channelMask);
        }
    case AUDIO_CHANNEL_REPRESENTATION_INDEX:
        if (kEnableExtendedChannels) {
            const uint32_t channelCount = audio_channel_count_from_out_mask(channelMask);
            if (channelCount >= FCC_2
                    && channelCount <= FCC_LIMIT) {
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}
constexpr bool kEnableExtendedPrecision = true;
bool IAfThreadBase::isValidPcmSinkFormat(audio_format_t format) {
    switch (format) {
    case AUDIO_FORMAT_PCM_16_BIT:
        return true;
    case AUDIO_FORMAT_PCM_FLOAT:
    case AUDIO_FORMAT_PCM_24_BIT_PACKED:
    case AUDIO_FORMAT_PCM_32_BIT:
    case AUDIO_FORMAT_PCM_8_24_BIT:
        return kEnableExtendedPrecision;
    default:
        return false;
    }
}
std::string IAfThreadBase::formatToString(audio_format_t format) {
    std::string result;
    FormatConverter::toString(format, result);
    return result;
}
static std::string patchSinksToString(const struct audio_patch *patch)
{
    std::stringstream ss;
    for (size_t i = 0; i < patch->num_sinks; ++i) {
        if (i > 0) {
            ss << "|";
        }
        ss << "(" << toString(patch->sinks[i].ext.device.type)
            << ", " << patch->sinks[i].ext.device.address << ")";
    }
    return ss.str();
}
static std::string patchSourcesToString(const struct audio_patch *patch)
{
    std::stringstream ss;
    for (size_t i = 0; i < patch->num_sources; ++i) {
        if (i > 0) {
            ss << "|";
        }
        ss << "(" << toString(patch->sources[i].ext.device.type)
            << ", " << patch->sources[i].ext.device.address << ")";
    }
    return ss.str();
}
static std::string toString(audio_latency_mode_t mode) {
    const auto result = legacy2aidl_audio_latency_mode_t_AudioLatencyMode(mode);
    return result.has_value() ? media::audio::common::toString(*result) : "UNKNOWN";
}
static std::string toString(const std::vector<audio_latency_mode_t>& elements) {
    std::string s("{ ");
    for (const auto& e : elements) {
        s.append(toString(e));
        s.append(" ");
    }
    s.append("}");
    return s;
}
static pthread_once_t sFastTrackMultiplierOnce = PTHREAD_ONCE_INIT;
static void sFastTrackMultiplierInit()
{
    char value[PROPERTY_VALUE_MAX];
    if (property_get("af.fast_track_multiplier", value, NULL) > 0) {
        char *endptr;
        unsigned long ul = strtoul(value, &endptr, 0);
        if (*endptr == '\0' && kFastTrackMultiplierMin <= ul && ul <= kFastTrackMultiplierMax) {
            sFastTrackMultiplier = (int) ul;
        }
    }
}
#ifdef ADD_BATTERY_DATA
static void addBatteryData(uint32_t params) {
    sp<IMediaPlayerService> service = IMediaDeathNotifier::getMediaPlayerService();
    if (service == NULL) {
        return;
    }
    service->addBatteryData(params);
}
#endif
struct {
    void acquire(const sp<IBinder> &wakeLockToken) {
        pthread_mutex_lock(&mLock);
        if (wakeLockToken.get() == nullptr) {
            adjustTimebaseOffset(&mBoottimeOffset, ExtendedTimestamp::TIMEBASE_BOOTTIME);
        } else {
            if (mCount == 0) {
                adjustTimebaseOffset(&mBoottimeOffset, ExtendedTimestamp::TIMEBASE_BOOTTIME);
            }
            ++mCount;
        }
        pthread_mutex_unlock(&mLock);
    }
    void release(const sp<IBinder> &wakeLockToken) {
        if (wakeLockToken.get() == nullptr) {
            return;
        }
        pthread_mutex_lock(&mLock);
        if (--mCount < 0) {
            ALOGE("negative wakelock count");
            mCount = 0;
        }
        pthread_mutex_unlock(&mLock);
    }
    int64_t getBoottimeOffset() {
        pthread_mutex_lock(&mLock);
        int64_t boottimeOffset = mBoottimeOffset;
        pthread_mutex_unlock(&mLock);
        return boottimeOffset;
    }
    static void adjustTimebaseOffset(int64_t *offset, ExtendedTimestamp::Timebase timebase) {
        int clockbase;
        switch (timebase) {
        case ExtendedTimestamp::TIMEBASE_BOOTTIME:
            clockbase = SYSTEM_TIME_BOOTTIME;
            break;
        default:
            LOG_ALWAYS_FATAL("invalid timebase %d", timebase);
            break;
        }
        const int tries = 3;
        nsecs_t bestGap = 0, measured = 0;
        for (int i = 0; i < tries; ++i) {
            const nsecs_t tmono = systemTime(SYSTEM_TIME_MONOTONIC);
            const nsecs_t tbase = systemTime(clockbase);
            const nsecs_t tmono2 = systemTime(SYSTEM_TIME_MONOTONIC);
            const nsecs_t gap = tmono2 - tmono;
            if (i == 0 || gap < bestGap) {
                bestGap = gap;
                measured = tbase - ((tmono + tmono2) >> 1);
            }
        }
        static int64_t toleranceNs = 10000;
        if (llabs(*offset - measured) > toleranceNs) {
            ALOGV("Adjusting timebase offset old: %lld  new: %lld",
                    (long long)*offset, (long long)measured);
            *offset = measured;
        }
    }
    pthread_mutex_t mLock;
    int32_t mCount;
    int64_t mBoottimeOffset;
} gBoottime = { PTHREAD_MUTEX_INITIALIZER, 0, 0 };
class CpuStats {
public:
    CpuStats();
    void sample(const String8 &title);
#ifdef DEBUG_CPU_USAGE
private:
    ThreadCpuUsage mCpuUsage;
    audio_utils::Statistics<double> mWcStats;
    audio_utils::Statistics<double> mHzStats;
    int mCpuNum;
    int mCpukHz;
#endif
};
CpuStats::CpuStats()
#ifdef DEBUG_CPU_USAGE
    : mCpuNum(-1), mCpukHz(-1)
#endif
{
}
void CpuStats::sample(const String8 &title
#ifndef DEBUG_CPU_USAGE
                __unused
#endif
        ) {
#ifdef DEBUG_CPU_USAGE
    double wcNs;
    bool valid = mCpuUsage.sampleAndEnable(wcNs);
    if (valid) {
        mWcStats.add(wcNs);
    }
    int cpuNum = sched_getcpu();
    int cpukHz = mCpuUsage.getCpukHz(cpuNum);
    if (cpuNum != mCpuNum || cpukHz != mCpukHz) {
        mCpuNum = cpuNum;
        mCpukHz = cpukHz;
        valid = false;
    }
    if (valid && mCpukHz > 0) {
        const double cycles = wcNs * cpukHz * 0.000001;
        mHzStats.add(cycles);
    }
    const unsigned n = mWcStats.getN();
    if ((n & 127) == 1) {
        const long long elapsed = mCpuUsage.elapsed();
        if (elapsed >= DEBUG_CPU_USAGE * 1000000000LL) {
            const double perLoop = elapsed / (double) n;
            const double perLoop100 = perLoop * 0.01;
            const double perLoop1k = perLoop * 0.001;
            const double mean = mWcStats.getMean();
            const double stddev = mWcStats.getStdDev();
            const double minimum = mWcStats.getMin();
            const double maximum = mWcStats.getMax();
            const double meanCycles = mHzStats.getMean();
            const double stddevCycles = mHzStats.getStdDev();
            const double minCycles = mHzStats.getMin();
            const double maxCycles = mHzStats.getMax();
            mCpuUsage.resetElapsed();
            mWcStats.reset();
            mHzStats.reset();
            ALOGD("CPU usage for %s over past %.1f secs\n"
                "  (%u mixer loops at %.1f mean ms per loop):\n"
                "  us per mix loop: mean=%.0f stddev=%.0f min=%.0f max=%.0f\n"
                "  %% of wall: mean=%.1f stddev=%.1f min=%.1f max=%.1f\n"
                "  MHz: mean=%.1f, stddev=%.1f, min=%.1f max=%.1f",
                    title.c_str(),
                    elapsed * .000000001, n, perLoop * .000001,
                    mean * .001,
                    stddev * .001,
                    minimum * .001,
                    maximum * .001,
                    mean / perLoop100,
                    stddev / perLoop100,
                    minimum / perLoop100,
                    maximum / perLoop100,
                    meanCycles / perLoop1k,
                    stddevCycles / perLoop1k,
                    minCycles / perLoop1k,
                    maxCycles / perLoop1k);
        }
    }
#endif
};
const char* ThreadBase::threadTypeToString(ThreadBase::type_t type)
{
    switch (type) {
    case MIXER:
        return "MIXER";
    case DIRECT:
        return "DIRECT";
    case DUPLICATING:
        return "DUPLICATING";
    case RECORD:
        return "RECORD";
    case OFFLOAD:
        return "OFFLOAD";
    case MMAP_PLAYBACK:
        return "MMAP_PLAYBACK";
    case MMAP_CAPTURE:
        return "MMAP_CAPTURE";
    case SPATIALIZER:
        return "SPATIALIZER";
    case BIT_PERFECT:
        return "BIT_PERFECT";
    default:
        return "unknown";
    }
}
ThreadBase::ThreadBase(const sp<IAfThreadCallback>& afThreadCallback, audio_io_handle_t id,
        type_t type, bool systemReady, bool isOut)
    : Thread(false ),
        mType(type),
        mAfThreadCallback(afThreadCallback),
        mThreadMetrics(std::string(AMEDIAMETRICS_KEY_PREFIX_AUDIO_THREAD) + std::to_string(id),
               isOut),
        mIsOut(isOut),
        mStandby(false),
        mAudioSource(AUDIO_SOURCE_DEFAULT), mId(id),
        mDeathRecipient(new PMDeathRecipient(this)),
        mSystemReady(systemReady),
        mSignalPending(false)
{
    mThreadMetrics.logConstructor(getpid(), threadTypeToString(type), id);
    memset(&mPatch, 0, sizeof(struct audio_patch));
}
ThreadBase::~ThreadBase()
{
    mConfigEvents.clear();
    releaseWakeLock_l();
    if (mPowerManager != 0) {
        sp<IBinder> binder = IInterface::asBinder(mPowerManager);
        binder->unlinkToDeath(mDeathRecipient);
    }
    sendStatistics(true );
}
status_t ThreadBase::readyToRun()
{
    status_t status = initCheck();
    if (status == NO_ERROR) {
        ALOGI("AudioFlinger's thread %p tid=%d ready to run", this, getTid());
    } else {
        ALOGE("No working audio driver found.");
    }
    return status;
}
void ThreadBase::exit()
{
    ALOGV("ThreadBase::exit");
    preExit();
    {
        audio_utils::lock_guard lock(mutex());
        requestExit();
        mWaitWorkCV.notify_all();
    }
    requestExitAndWait();
}
status_t ThreadBase::setParameters(const String8& keyValuePairs)
{
    ALOGV("ThreadBase::setParameters() %s", keyValuePairs.c_str());
    audio_utils::lock_guard _l(mutex());
    return sendSetParameterConfigEvent_l(keyValuePairs);
}
status_t ThreadBase::sendConfigEvent_l(sp<ConfigEvent>& event)
NO_THREAD_SAFETY_ANALYSIS
{
    status_t status = NO_ERROR;
    if (event->mRequiresSystemReady && !mSystemReady) {
        event->mWaitStatus = false;
        mPendingConfigEvents.add(event);
        return status;
    }
    mConfigEvents.add(event);
    ALOGV("sendConfigEvent_l() num events %zu event %d", mConfigEvents.size(), event->mType);
    mWaitWorkCV.notify_one();
    mutex().unlock();
    {
        audio_utils::unique_lock _l(event->mutex());
        while (event->mWaitStatus) {
            if (event->mCondition.wait_for(_l, std::chrono::nanoseconds(kConfigEventTimeoutNs))
                        == std::cv_status::timeout) {
                event->mStatus = TIMED_OUT;
                event->mWaitStatus = false;
            }
        }
        status = event->mStatus;
    }
    mutex().lock();
    return status;
}
void ThreadBase::sendIoConfigEvent(audio_io_config_event_t event, pid_t pid,
                                                 audio_port_handle_t portId)
{
    audio_utils::lock_guard _l(mutex());
    sendIoConfigEvent_l(event, pid, portId);
}
void ThreadBase::sendIoConfigEvent_l(audio_io_config_event_t event, pid_t pid,
                                                   audio_port_handle_t portId)
{
    mIoJitterMs.reset();
    mLatencyMs.reset();
    mProcessTimeMs.reset();
    mMonopipePipeDepthStats.reset();
    mTimestampVerifier.discontinuity(mTimestampVerifier.DISCONTINUITY_MODE_CONTINUOUS);
    sp<ConfigEvent> configEvent = (ConfigEvent *)new IoConfigEvent(event, pid, portId);
    sendConfigEvent_l(configEvent);
}
void ThreadBase::sendPrioConfigEvent(pid_t pid, pid_t tid, int32_t prio, bool forApp)
{
    audio_utils::lock_guard _l(mutex());
    sendPrioConfigEvent_l(pid, tid, prio, forApp);
}
void ThreadBase::sendPrioConfigEvent_l(
        pid_t pid, pid_t tid, int32_t prio, bool forApp)
{
    sp<ConfigEvent> configEvent = (ConfigEvent *)new PrioConfigEvent(pid, tid, prio, forApp);
    sendConfigEvent_l(configEvent);
}
status_t ThreadBase::sendSetParameterConfigEvent_l(const String8& keyValuePair)
{
    sp<ConfigEvent> configEvent;
    AudioParameter param(keyValuePair);
    int value;
    if (param.getInt(String8(AudioParameter::keyMonoOutput), value) == NO_ERROR) {
        setMasterMono_l(value != 0);
        if (param.size() == 1) {
            return NO_ERROR;
        }
        param.remove(String8(AudioParameter::keyMonoOutput));
        configEvent = new SetParameterConfigEvent(param.toString());
    } else {
        configEvent = new SetParameterConfigEvent(keyValuePair);
    }
    return sendConfigEvent_l(configEvent);
}
status_t ThreadBase::sendCreateAudioPatchConfigEvent(
                                                        const struct audio_patch *patch,
                                                        audio_patch_handle_t *handle)
{
    audio_utils::lock_guard _l(mutex());
    sp<ConfigEvent> configEvent = (ConfigEvent *)new CreateAudioPatchConfigEvent(*patch, *handle);
    status_t status = sendConfigEvent_l(configEvent);
    if (status == NO_ERROR) {
        CreateAudioPatchConfigEventData *data =
                                        (CreateAudioPatchConfigEventData *)configEvent->mData.get();
        *handle = data->mHandle;
    }
    return status;
}
status_t ThreadBase::sendReleaseAudioPatchConfigEvent(
                                                                const audio_patch_handle_t handle)
{
    audio_utils::lock_guard _l(mutex());
    sp<ConfigEvent> configEvent = (ConfigEvent *)new ReleaseAudioPatchConfigEvent(handle);
    return sendConfigEvent_l(configEvent);
}
status_t ThreadBase::sendUpdateOutDeviceConfigEvent(
        const DeviceDescriptorBaseVector& outDevices)
{
    if (type() != RECORD) {
        return INVALID_OPERATION;
    }
    audio_utils::lock_guard _l(mutex());
    sp<ConfigEvent> configEvent = (ConfigEvent *)new UpdateOutDevicesConfigEvent(outDevices);
    return sendConfigEvent_l(configEvent);
}
void ThreadBase::sendResizeBufferConfigEvent_l(int32_t maxSharedAudioHistoryMs)
{
    ALOG_ASSERT(type() == RECORD, "sendResizeBufferConfigEvent_l() called on non record thread");
    sp<ConfigEvent> configEvent =
            (ConfigEvent *)new ResizeBufferConfigEvent(maxSharedAudioHistoryMs);
    sendConfigEvent_l(configEvent);
}
void ThreadBase::sendCheckOutputStageEffectsEvent()
{
    audio_utils::lock_guard _l(mutex());
    sendCheckOutputStageEffectsEvent_l();
}
void ThreadBase::sendCheckOutputStageEffectsEvent_l()
{
    sp<ConfigEvent> configEvent =
            (ConfigEvent *)new CheckOutputStageEffectsEvent();
    sendConfigEvent_l(configEvent);
}
void ThreadBase::sendHalLatencyModesChangedEvent_l()
{
    sp<ConfigEvent> configEvent = sp<HalLatencyModesChangedEvent>::make();
    sendConfigEvent_l(configEvent);
}
void ThreadBase::processConfigEvents_l()
{
    bool configChanged = false;
    while (!mConfigEvents.isEmpty()) {
        ALOGV("processConfigEvents_l() remaining events %zu", mConfigEvents.size());
        sp<ConfigEvent> event = mConfigEvents[0];
        mConfigEvents.removeAt(0);
        switch (event->mType) {
        case CFG_EVENT_PRIO: {
            PrioConfigEventData *data = (PrioConfigEventData *)event->mData.get();
            int err = requestPriority(data->mPid, data->mTid, data->mPrio, data->mForApp,
                    true );
            if (err != 0) {
                ALOGW("Policy SCHED_FIFO priority %d is unavailable for pid %d tid %d; error %d",
                      data->mPrio, data->mPid, data->mTid, err);
            }
        } break;
        case CFG_EVENT_IO: {
            IoConfigEventData *data = (IoConfigEventData *)event->mData.get();
            ioConfigChanged(data->mEvent, data->mPid, data->mPortId);
        } break;
        case CFG_EVENT_SET_PARAMETER: {
            SetParameterConfigEventData *data = (SetParameterConfigEventData *)event->mData.get();
            if (checkForNewParameter_l(data->mKeyValuePairs, event->mStatus)) {
                configChanged = true;
                mLocalLog.log("CFG_EVENT_SET_PARAMETER: (%s) configuration changed",
                        data->mKeyValuePairs.c_str());
            }
        } break;
        case CFG_EVENT_CREATE_AUDIO_PATCH: {
            const DeviceTypeSet oldDevices = getDeviceTypes();
            CreateAudioPatchConfigEventData *data =
                                            (CreateAudioPatchConfigEventData *)event->mData.get();
            event->mStatus = createAudioPatch_l(&data->mPatch, &data->mHandle);
            const DeviceTypeSet newDevices = getDeviceTypes();
            configChanged = oldDevices != newDevices;
            mLocalLog.log("CFG_EVENT_CREATE_AUDIO_PATCH: old device %s (%s) new device %s (%s)",
                    dumpDeviceTypes(oldDevices).c_str(), toString(oldDevices).c_str(),
                    dumpDeviceTypes(newDevices).c_str(), toString(newDevices).c_str());
        } break;
        case CFG_EVENT_RELEASE_AUDIO_PATCH: {
            const DeviceTypeSet oldDevices = getDeviceTypes();
            ReleaseAudioPatchConfigEventData *data =
                                            (ReleaseAudioPatchConfigEventData *)event->mData.get();
            event->mStatus = releaseAudioPatch_l(data->mHandle);
            const DeviceTypeSet newDevices = getDeviceTypes();
            configChanged = oldDevices != newDevices;
            mLocalLog.log("CFG_EVENT_RELEASE_AUDIO_PATCH: old device %s (%s) new device %s (%s)",
                    dumpDeviceTypes(oldDevices).c_str(), toString(oldDevices).c_str(),
                    dumpDeviceTypes(newDevices).c_str(), toString(newDevices).c_str());
        } break;
        case CFG_EVENT_UPDATE_OUT_DEVICE: {
            UpdateOutDevicesConfigEventData *data =
                    (UpdateOutDevicesConfigEventData *)event->mData.get();
            updateOutDevices(data->mOutDevices);
        } break;
        case CFG_EVENT_RESIZE_BUFFER: {
            ResizeBufferConfigEventData *data =
                    (ResizeBufferConfigEventData *)event->mData.get();
            resizeInputBuffer_l(data->mMaxSharedAudioHistoryMs);
        } break;
        case CFG_EVENT_CHECK_OUTPUT_STAGE_EFFECTS: {
            setCheckOutputStageEffects();
        } break;
        case CFG_EVENT_HAL_LATENCY_MODES_CHANGED: {
            onHalLatencyModesChanged_l();
        } break;
        default:
            ALOG_ASSERT(false, "processConfigEvents_l() unknown event type %d", event->mType);
            break;
        }
        {
            audio_utils::lock_guard _l(event->mutex());
            if (event->mWaitStatus) {
                event->mWaitStatus = false;
                event->mCondition.notify_one();
            }
        }
        ALOGV_IF(mConfigEvents.isEmpty(), "processConfigEvents_l() DONE thread %p", this);
    }
    if (configChanged) {
        cacheParameters_l();
    }
}
String8 channelMaskToString(audio_channel_mask_t mask, bool output) {
    String8 s;
    const audio_channel_representation_t representation =
            audio_channel_mask_get_representation(mask);
    switch (representation) {
    case AUDIO_CHANNEL_REPRESENTATION_POSITION: {
        if (output) {
            if (mask & AUDIO_CHANNEL_OUT_FRONT_LEFT) s.append("front-left, ");
            if (mask & AUDIO_CHANNEL_OUT_FRONT_RIGHT) s.append("front-right, ");
            if (mask & AUDIO_CHANNEL_OUT_FRONT_CENTER) s.append("front-center, ");
            if (mask & AUDIO_CHANNEL_OUT_LOW_FREQUENCY) s.append("low-frequency, ");
            if (mask & AUDIO_CHANNEL_OUT_BACK_LEFT) s.append("back-left, ");
            if (mask & AUDIO_CHANNEL_OUT_BACK_RIGHT) s.append("back-right, ");
            if (mask & AUDIO_CHANNEL_OUT_FRONT_LEFT_OF_CENTER) s.append("front-left-of-center, ");
            if (mask & AUDIO_CHANNEL_OUT_FRONT_RIGHT_OF_CENTER) s.append("front-right-of-center, ");
            if (mask & AUDIO_CHANNEL_OUT_BACK_CENTER) s.append("back-center, ");
            if (mask & AUDIO_CHANNEL_OUT_SIDE_LEFT) s.append("side-left, ");
            if (mask & AUDIO_CHANNEL_OUT_SIDE_RIGHT) s.append("side-right, ");
            if (mask & AUDIO_CHANNEL_OUT_TOP_CENTER) s.append("top-center ,");
            if (mask & AUDIO_CHANNEL_OUT_TOP_FRONT_LEFT) s.append("top-front-left, ");
            if (mask & AUDIO_CHANNEL_OUT_TOP_FRONT_CENTER) s.append("top-front-center, ");
            if (mask & AUDIO_CHANNEL_OUT_TOP_FRONT_RIGHT) s.append("top-front-right, ");
            if (mask & AUDIO_CHANNEL_OUT_TOP_BACK_LEFT) s.append("top-back-left, ");
            if (mask & AUDIO_CHANNEL_OUT_TOP_BACK_CENTER) s.append("top-back-center, ");
            if (mask & AUDIO_CHANNEL_OUT_TOP_BACK_RIGHT) s.append("top-back-right, ");
            if (mask & AUDIO_CHANNEL_OUT_TOP_SIDE_LEFT) s.append("top-side-left, ");
            if (mask & AUDIO_CHANNEL_OUT_TOP_SIDE_RIGHT) s.append("top-side-right, ");
            if (mask & AUDIO_CHANNEL_OUT_BOTTOM_FRONT_LEFT) s.append("bottom-front-left, ");
            if (mask & AUDIO_CHANNEL_OUT_BOTTOM_FRONT_CENTER) s.append("bottom-front-center, ");
            if (mask & AUDIO_CHANNEL_OUT_BOTTOM_FRONT_RIGHT) s.append("bottom-front-right, ");
            if (mask & AUDIO_CHANNEL_OUT_LOW_FREQUENCY_2) s.append("low-frequency-2, ");
            if (mask & AUDIO_CHANNEL_OUT_HAPTIC_B) s.append("haptic-B, ");
            if (mask & AUDIO_CHANNEL_OUT_HAPTIC_A) s.append("haptic-A, ");
            if (mask & ~AUDIO_CHANNEL_OUT_ALL) s.append("unknown,  ");
        } else {
            if (mask & AUDIO_CHANNEL_IN_LEFT) s.append("left, ");
            if (mask & AUDIO_CHANNEL_IN_RIGHT) s.append("right, ");
            if (mask & AUDIO_CHANNEL_IN_FRONT) s.append("front, ");
            if (mask & AUDIO_CHANNEL_IN_BACK) s.append("back, ");
            if (mask & AUDIO_CHANNEL_IN_LEFT_PROCESSED) s.append("left-processed, ");
            if (mask & AUDIO_CHANNEL_IN_RIGHT_PROCESSED) s.append("right-processed, ");
            if (mask & AUDIO_CHANNEL_IN_FRONT_PROCESSED) s.append("front-processed, ");
            if (mask & AUDIO_CHANNEL_IN_BACK_PROCESSED) s.append("back-processed, ");
            if (mask & AUDIO_CHANNEL_IN_PRESSURE) s.append("pressure, ");
            if (mask & AUDIO_CHANNEL_IN_X_AXIS) s.append("X, ");
            if (mask & AUDIO_CHANNEL_IN_Y_AXIS) s.append("Y, ");
            if (mask & AUDIO_CHANNEL_IN_Z_AXIS) s.append("Z, ");
            if (mask & AUDIO_CHANNEL_IN_BACK_LEFT) s.append("back-left, ");
            if (mask & AUDIO_CHANNEL_IN_BACK_RIGHT) s.append("back-right, ");
            if (mask & AUDIO_CHANNEL_IN_CENTER) s.append("center, ");
            if (mask & AUDIO_CHANNEL_IN_LOW_FREQUENCY) s.append("low-frequency, ");
            if (mask & AUDIO_CHANNEL_IN_TOP_LEFT) s.append("top-left, ");
            if (mask & AUDIO_CHANNEL_IN_TOP_RIGHT) s.append("top-right, ");
            if (mask & AUDIO_CHANNEL_IN_VOICE_UPLINK) s.append("voice-uplink, ");
            if (mask & AUDIO_CHANNEL_IN_VOICE_DNLINK) s.append("voice-dnlink, ");
            if (mask & ~AUDIO_CHANNEL_IN_ALL) s.append("unknown,  ");
        }
        const int len = s.length();
        if (len > 2) {
            (void) s.lockBuffer(len);
            s.unlockBuffer(len - 2);
        }
        return s;
    }
    case AUDIO_CHANNEL_REPRESENTATION_INDEX:
        s.appendFormat("index mask, bits:%#x", audio_channel_mask_get_bits(mask));
        return s;
    default:
        s.appendFormat("unknown mask, representation:%d  bits:%#x",
                representation, audio_channel_mask_get_bits(mask));
        return s;
    }
}
void ThreadBase::dump(int fd, const Vector<String16>& args)
NO_THREAD_SAFETY_ANALYSIS
{
    dprintf(fd, "\n%s thread %p, name %s, tid %d, type %d (%s):\n", isOutput() ? "Output" : "Input",
            this, mThreadName, getTid(), type(), threadTypeToString(type()));
    const bool locked = afutils::dumpTryLock(mutex());
    if (!locked) {
        dprintf(fd, "  Thread may be deadlocked\n");
    }
    dumpBase_l(fd, args);
    dumpInternals_l(fd, args);
    dumpTracks_l(fd, args);
    dumpEffectChains_l(fd, args);
    if (locked) {
        mutex().unlock();
    }
    dprintf(fd, "  Local log:\n");
    mLocalLog.dump(fd, "   " , 40 );
    bool dumpAll = false;
    for (const auto &arg : args) {
        if (arg == String16("--all")) {
            dumpAll = true;
        }
    }
    if (dumpAll || type() == SPATIALIZER) {
        const std::string sched = mThreadSnapshot.toString();
        if (!sched.empty()) {
            (void)write(fd, sched.c_str(), sched.size());
        }
    }
}
void ThreadBase::dumpBase_l(int fd, const Vector<String16>& )
{
    dprintf(fd, "  I/O handle: %d\n", mId);
    dprintf(fd, "  Standby: %s\n", mStandby ? "yes" : "no");
    dprintf(fd, "  Sample rate: %u Hz\n", mSampleRate);
    dprintf(fd, "  HAL frame count: %zu\n", mFrameCount);
    dprintf(fd, "  HAL format: 0x%x (%s)\n", mHALFormat,
            IAfThreadBase::formatToString(mHALFormat).c_str());
    dprintf(fd, "  HAL buffer size: %zu bytes\n", mBufferSize);
    dprintf(fd, "  Channel count: %u\n", mChannelCount);
    dprintf(fd, "  Channel mask: 0x%08x (%s)\n", mChannelMask,
            channelMaskToString(mChannelMask, mType != RECORD).c_str());
    dprintf(fd, "  Processing format: 0x%x (%s)\n", mFormat,
            IAfThreadBase::formatToString(mFormat).c_str());
    dprintf(fd, "  Processing frame size: %zu bytes\n", mFrameSize);
    dprintf(fd, "  Pending config events:");
    size_t numConfig = mConfigEvents.size();
    if (numConfig) {
        const size_t SIZE = 256;
        char buffer[SIZE];
        for (size_t i = 0; i < numConfig; i++) {
            mConfigEvents[i]->dump(buffer, SIZE);
            dprintf(fd, "\n    %s", buffer);
        }
        dprintf(fd, "\n");
    } else {
        dprintf(fd, " none\n");
    }
    dprintf(fd, "  Output devices: %s (%s)\n",
            dumpDeviceTypes(outDeviceTypes()).c_str(), toString(outDeviceTypes()).c_str());
    dprintf(fd, "  Input device: %#x (%s)\n",
            inDeviceType(), toString(inDeviceType()).c_str());
    dprintf(fd, "  Audio source: %d (%s)\n", mAudioSource, toString(mAudioSource).c_str());
    if (mType == RECORD
            || mType == MIXER
            || mType == DUPLICATING
            || mType == DIRECT
            || mType == OFFLOAD
            || mType == SPATIALIZER) {
        dprintf(fd, "  Timestamp stats: %s\n", mTimestampVerifier.toString().c_str());
        dprintf(fd, "  Timestamp corrected: %s\n", isTimestampCorrectionEnabled() ? "yes" : "no");
    }
    if (mLastIoBeginNs > 0) {
        dprintf(fd, "  Last %s occurred (msecs): %lld\n",
                isOutput() ? "write" : "read",
                (long long) (systemTime() - mLastIoBeginNs) / NANOS_PER_MILLISECOND);
    }
    if (mProcessTimeMs.getN() > 0) {
        dprintf(fd, "  Process time ms stats: %s\n", mProcessTimeMs.toString().c_str());
    }
    if (mIoJitterMs.getN() > 0) {
        dprintf(fd, "  Hal %s jitter ms stats: %s\n",
                isOutput() ? "write" : "read",
                mIoJitterMs.toString().c_str());
    }
    if (mLatencyMs.getN() > 0) {
        dprintf(fd, "  Threadloop %s latency stats: %s\n",
                isOutput() ? "write" : "read",
                mLatencyMs.toString().c_str());
    }
    if (mMonopipePipeDepthStats.getN() > 0) {
        dprintf(fd, "  Monopipe %s pipe depth stats: %s\n",
            isOutput() ? "write" : "read",
            mMonopipePipeDepthStats.toString().c_str());
    }
}
void ThreadBase::dumpEffectChains_l(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    size_t numEffectChains = mEffectChains.size();
    snprintf(buffer, SIZE, "  %zu Effect Chains\n", numEffectChains);
    write(fd, buffer, strlen(buffer));
    for (size_t i = 0; i < numEffectChains; ++i) {
        sp<IAfEffectChain> chain = mEffectChains[i];
        if (chain != 0) {
            chain->dump(fd, args);
        }
    }
}
void ThreadBase::acquireWakeLock()
{
    audio_utils::lock_guard _l(mutex());
    acquireWakeLock_l();
}
String16 ThreadBase::getWakeLockTag()
{
    switch (mType) {
    case MIXER:
        return String16("AudioMix");
    case DIRECT:
        return String16("AudioDirectOut");
    case DUPLICATING:
        return String16("AudioDup");
    case RECORD:
        return String16("AudioIn");
    case OFFLOAD:
        return String16("AudioOffload");
    case MMAP_PLAYBACK:
        return String16("MmapPlayback");
    case MMAP_CAPTURE:
        return String16("MmapCapture");
    case SPATIALIZER:
        return String16("AudioSpatial");
    default:
        ALOG_ASSERT(false);
        return String16("AudioUnknown");
    }
}
void ThreadBase::acquireWakeLock_l()
{
    getPowerManager_l();
    if (mPowerManager != 0) {
        sp<IBinder> binder = new BBinder();
        binder::Status status = mPowerManager->acquireWakeLockAsync(binder,
                    POWERMANAGER_PARTIAL_WAKE_LOCK,
                    getWakeLockTag(),
                    String16("audioserver"),
                    {} ,
                    {} );
        if (status.isOk()) {
            mWakeLockToken = binder;
        }
        ALOGV("acquireWakeLock_l() %s status %d", mThreadName, status.exceptionCode());
    }
    gBoottime.acquire(mWakeLockToken);
    mTimestamp.mTimebaseOffset[ExtendedTimestamp::TIMEBASE_BOOTTIME] =
            gBoottime.getBoottimeOffset();
}
void ThreadBase::releaseWakeLock()
{
    audio_utils::lock_guard _l(mutex());
    releaseWakeLock_l();
}
void ThreadBase::releaseWakeLock_l()
{
    gBoottime.release(mWakeLockToken);
    if (mWakeLockToken != 0) {
        ALOGV("releaseWakeLock_l() %s", mThreadName);
        if (mPowerManager != 0) {
            mPowerManager->releaseWakeLockAsync(mWakeLockToken, 0);
        }
        mWakeLockToken.clear();
    }
}
void ThreadBase::getPowerManager_l() {
    if (mSystemReady && mPowerManager == 0) {
        sp<IBinder> binder =
            defaultServiceManager()->checkService(String16("power"));
        if (binder == 0) {
            ALOGW("Thread %s cannot connect to the power manager service", mThreadName);
        } else {
            mPowerManager = interface_cast<os::IPowerManager>(binder);
            binder->linkToDeath(mDeathRecipient);
        }
    }
}
void ThreadBase::updateWakeLockUids_l(const SortedVector<uid_t>& uids) {
    getPowerManager_l();
#if !LOG_NDEBUG
    std::stringstream s;
    for (uid_t uid : uids) {
        s << uid << " ";
    }
    ALOGD("updateWakeLockUids_l %s uids:%s", mThreadName, s.str().c_str());
#endif
    if (mWakeLockToken == NULL) {
        if (mSystemReady) {
            ALOGE("no wake lock to update, but system ready!");
        } else {
            ALOGW("no wake lock to update, system not ready yet");
        }
        return;
    }
    if (mPowerManager != 0) {
        std::vector<int> uidsAsInt(uids.begin(), uids.end());
        binder::Status status = mPowerManager->updateWakeLockUidsAsync(
                mWakeLockToken, uidsAsInt);
        ALOGV("updateWakeLockUids_l() %s status %d", mThreadName, status.exceptionCode());
    }
}
void ThreadBase::clearPowerManager()
{
    audio_utils::lock_guard _l(mutex());
    releaseWakeLock_l();
    mPowerManager.clear();
}
void ThreadBase::updateOutDevices(
        const DeviceDescriptorBaseVector& outDevices __unused)
{
    ALOGE("%s should only be called in RecordThread", __func__);
}
void ThreadBase::resizeInputBuffer_l(int32_t )
{
    ALOGE("%s should only be called in RecordThread", __func__);
}
void ThreadBase::PMDeathRecipient::binderDied(const wp<IBinder>& )
{
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        thread->clearPowerManager();
    }
    ALOGW("power manager service died !!!");
}
void ThreadBase::setEffectSuspended_l(
        const effect_uuid_t *type, bool suspend, audio_session_t sessionId)
{
    sp<IAfEffectChain> chain = getEffectChain_l(sessionId);
    if (chain != 0) {
        if (type != NULL) {
            chain->setEffectSuspended_l(type, suspend);
        } else {
            chain->setEffectSuspendedAll_l(suspend);
        }
    }
    updateSuspendedSessions_l(type, suspend, sessionId);
}
void ThreadBase::checkSuspendOnAddEffectChain_l(const sp<IAfEffectChain>& chain)
{
    ssize_t index = mSuspendedSessions.indexOfKey(chain->sessionId());
    if (index < 0) {
        return;
    }
    const KeyedVector <int, sp<SuspendedSessionDesc> >& sessionEffects =
            mSuspendedSessions.valueAt(index);
    for (size_t i = 0; i < sessionEffects.size(); i++) {
        const sp<SuspendedSessionDesc>& desc = sessionEffects.valueAt(i);
        for (int j = 0; j < desc->mRefCount; j++) {
            if (sessionEffects.keyAt(i) == IAfEffectChain::kKeyForSuspendAll) {
                chain->setEffectSuspendedAll_l(true);
            } else {
                ALOGV("checkSuspendOnAddEffectChain_l() suspending effects %08x",
                    desc->mType.timeLow);
                chain->setEffectSuspended_l(&desc->mType, true);
            }
        }
    }
}
void ThreadBase::updateSuspendedSessions_l(const effect_uuid_t* type,
                                                         bool suspend,
                                                         audio_session_t sessionId)
{
    ssize_t index = mSuspendedSessions.indexOfKey(sessionId);
    KeyedVector <int, sp<SuspendedSessionDesc> > sessionEffects;
    if (suspend) {
        if (index >= 0) {
            sessionEffects = mSuspendedSessions.valueAt(index);
        } else {
            mSuspendedSessions.add(sessionId, sessionEffects);
        }
    } else {
        if (index < 0) {
            return;
        }
        sessionEffects = mSuspendedSessions.valueAt(index);
    }
    int key = IAfEffectChain::kKeyForSuspendAll;
    if (type != NULL) {
        key = type->timeLow;
    }
    index = sessionEffects.indexOfKey(key);
    sp<SuspendedSessionDesc> desc;
    if (suspend) {
        if (index >= 0) {
            desc = sessionEffects.valueAt(index);
        } else {
            desc = new SuspendedSessionDesc();
            if (type != NULL) {
                desc->mType = *type;
            }
            sessionEffects.add(key, desc);
            ALOGV("updateSuspendedSessions_l() suspend adding effect %08x", key);
        }
        desc->mRefCount++;
    } else {
        if (index < 0) {
            return;
        }
        desc = sessionEffects.valueAt(index);
        if (--desc->mRefCount == 0) {
            ALOGV("updateSuspendedSessions_l() restore removing effect %08x", key);
            sessionEffects.removeItemsAt(index);
            if (sessionEffects.isEmpty()) {
                ALOGV("updateSuspendedSessions_l() restore removing session %d",
                                 sessionId);
                mSuspendedSessions.removeItem(sessionId);
            }
        }
    }
    if (!sessionEffects.isEmpty()) {
        mSuspendedSessions.replaceValueFor(sessionId, sessionEffects);
    }
}
void ThreadBase::checkSuspendOnEffectEnabled(bool enabled,
                                                           audio_session_t sessionId,
                                                           bool threadLocked)
NO_THREAD_SAFETY_ANALYSIS
{
    if (!threadLocked) {
        mutex().lock();
    }
    if (mType != RECORD) {
        if (!audio_is_global_session(sessionId)) {
            setEffectSuspended_l(NULL, enabled, AUDIO_SESSION_OUTPUT_MIX);
        }
    }
    if (!threadLocked) {
        mutex().unlock();
    }
}
status_t RecordThread::checkEffectCompatibility_l(
        const effect_descriptor_t *desc, audio_session_t sessionId)
{
    if (sessionId == AUDIO_SESSION_OUTPUT_MIX
            || sessionId == AUDIO_SESSION_OUTPUT_STAGE) {
        ALOGW("checkEffectCompatibility_l(): global effect %s on record thread %s",
                desc->name, mThreadName);
        return BAD_VALUE;
    }
    if ((desc->flags & EFFECT_FLAG_TYPE_MASK) != EFFECT_FLAG_TYPE_PRE_PROC) {
        ALOGW("checkEffectCompatibility_l(): non pre processing effect %s on record thread %s",
                desc->name, mThreadName);
        return BAD_VALUE;
    }
    if ((desc->flags & EFFECT_FLAG_NO_PROCESS_MASK) == EFFECT_FLAG_NO_PROCESS) {
        return NO_ERROR;
    }
    audio_input_flags_t flags = mInput->flags;
    if (hasFastCapture() || (flags & AUDIO_INPUT_FLAG_FAST)) {
        if (flags & AUDIO_INPUT_FLAG_RAW) {
            ALOGW("checkEffectCompatibility_l(): effect %s on record thread %s in raw mode",
                  desc->name, mThreadName);
            return BAD_VALUE;
        }
        if ((desc->flags & EFFECT_FLAG_HW_ACC_TUNNEL) == 0) {
            ALOGW("checkEffectCompatibility_l(): non HW effect %s on record thread %s in fast mode",
                  desc->name, mThreadName);
            return BAD_VALUE;
        }
    }
    if (IAfEffectModule::isHapticGenerator(&desc->type)) {
        ALOGE("%s(): HapticGenerator is not supported in RecordThread", __func__);
        return BAD_VALUE;
    }
    return NO_ERROR;
}
status_t PlaybackThread::checkEffectCompatibility_l(
        const effect_descriptor_t *desc, audio_session_t sessionId)
{
    if ((desc->flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_PRE_PROC) {
        ALOGW("%s: pre processing effect %s created on playback"
                " thread %s", __func__, desc->name, mThreadName);
        return BAD_VALUE;
    }
    if ((desc->flags & EFFECT_FLAG_NO_PROCESS_MASK) == EFFECT_FLAG_NO_PROCESS) {
        return NO_ERROR;
    }
    if (IAfEffectModule::isHapticGenerator(&desc->type) && mHapticChannelCount == 0) {
        ALOGW("%s: thread doesn't support haptic playback while the effect is HapticGenerator",
                __func__);
        return BAD_VALUE;
    }
    if (memcmp(&desc->type, FX_IID_SPATIALIZER, sizeof(effect_uuid_t)) == 0
            && mType != SPATIALIZER) {
        ALOGW("%s: attempt to create a spatializer effect on a thread of type %d",
                __func__, mType);
        return BAD_VALUE;
    }
    switch (mType) {
    case MIXER: {
        audio_output_flags_t flags = mOutput->flags;
        if (hasFastMixer() || (flags & AUDIO_OUTPUT_FLAG_FAST)) {
            if (sessionId == AUDIO_SESSION_OUTPUT_MIX) {
                if ((desc->flags & EFFECT_FLAG_HW_ACC_TUNNEL) == 0) {
                    break;
                }
            } else if (sessionId == AUDIO_SESSION_OUTPUT_STAGE) {
                if ((desc->flags & EFFECT_FLAG_TYPE_MASK) != EFFECT_FLAG_TYPE_POST_PROC) {
                    ALOGW("%s: non post processing effect %s not allowed on output stage session",
                            __func__, desc->name);
                    return BAD_VALUE;
                }
            } else if (sessionId == AUDIO_SESSION_DEVICE) {
                if ((desc->flags & EFFECT_FLAG_TYPE_MASK) != EFFECT_FLAG_TYPE_POST_PROC) {
                    ALOGW("%s: non post processing effect %s not allowed on device session",
                            __func__, desc->name);
                    return BAD_VALUE;
                }
            } else {
                if ((hasAudioSession_l(sessionId) & ThreadBase::FAST_SESSION) == 0) {
                    break;
                }
            }
            if (flags & AUDIO_OUTPUT_FLAG_RAW) {
                ALOGW("%s: effect %s on playback thread in raw mode", __func__, desc->name);
                return BAD_VALUE;
            }
            if ((desc->flags & EFFECT_FLAG_HW_ACC_TUNNEL) == 0) {
                ALOGW("%s: non HW effect %s on playback thread in fast mode",
                        __func__, desc->name);
                return BAD_VALUE;
            }
        }
    } break;
    case OFFLOAD:
        break;
    case DIRECT:
        ALOGW("%s: effect %s on DIRECT output thread %s",
                __func__, desc->name, mThreadName);
        return BAD_VALUE;
    case DUPLICATING:
        if (audio_is_global_session(sessionId)) {
            ALOGW("%s: global effect %s on DUPLICATING thread %s",
                    __func__, desc->name, mThreadName);
            return BAD_VALUE;
        }
        if ((desc->flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_POST_PROC) {
            ALOGW("%s: post processing effect %s on DUPLICATING thread %s",
                __func__, desc->name, mThreadName);
            return BAD_VALUE;
        }
        if ((desc->flags & EFFECT_FLAG_HW_ACC_TUNNEL) != 0) {
            ALOGW("%s: HW tunneled effect %s on DUPLICATING thread %s",
                    __func__, desc->name, mThreadName);
            return BAD_VALUE;
        }
        break;
    case SPATIALIZER:
        if (sessionId == AUDIO_SESSION_OUTPUT_MIX) {
            ALOGW("%s: global effect %s not supported on spatializer thread %s",
                    __func__, desc->name, mThreadName);
            return BAD_VALUE;
        } else if (sessionId == AUDIO_SESSION_OUTPUT_STAGE) {
            if (memcmp(&desc->type, FX_IID_SPATIALIZER, sizeof(effect_uuid_t)) == 0
                    || memcmp(&desc->type, EFFECT_UIID_DOWNMIX, sizeof(effect_uuid_t)) == 0) {
                break;
            }
            if ((desc->flags & EFFECT_FLAG_TYPE_MASK) != EFFECT_FLAG_TYPE_POST_PROC) {
                ALOGW("%s: non post processing effect %s not allowed on output stage session",
                        __func__, desc->name);
                return BAD_VALUE;
            }
        } else if (sessionId == AUDIO_SESSION_DEVICE) {
            if ((desc->flags & EFFECT_FLAG_TYPE_MASK) != EFFECT_FLAG_TYPE_POST_PROC) {
                ALOGW("%s: non post processing effect %s not allowed on device session",
                        __func__, desc->name);
                return BAD_VALUE;
            }
        }
        break;
    case BIT_PERFECT:
        if ((desc->flags & EFFECT_FLAG_HW_ACC_TUNNEL) != 0) {
            break;
        }
        if (sessionId == AUDIO_SESSION_OUTPUT_MIX || sessionId == AUDIO_SESSION_OUTPUT_STAGE ||
            sessionId == AUDIO_SESSION_DEVICE) {
            ALOGW("%s: effect %s not supported on bit-perfect thread %s",
                  __func__, desc->name, mThreadName);
            return BAD_VALUE;
        } else if ((hasAudioSession_l(sessionId) & ThreadBase::BIT_PERFECT_SESSION) != 0) {
            ALOGW("%s: effect %s not supported as there is a bit-perfect track with session as %d",
                  __func__, desc->name, sessionId);
            return BAD_VALUE;
        }
        break;
    default:
        LOG_ALWAYS_FATAL("checkEffectCompatibility_l(): wrong thread type %d", mType);
    }
    return NO_ERROR;
}
sp<IAfEffectHandle> ThreadBase::createEffect_l(
        const sp<Client>& client,
        const sp<IEffectClient>& effectClient,
        int32_t priority,
        audio_session_t sessionId,
        effect_descriptor_t *desc,
        int *enabled,
        status_t *status,
        bool pinned,
        bool probe,
        bool notifyFramesProcessed)
{
    sp<IAfEffectModule> effect;
    sp<IAfEffectHandle> handle;
    status_t lStatus;
    sp<IAfEffectChain> chain;
    bool chainCreated = false;
    bool effectCreated = false;
    audio_unique_id_t effectId = AUDIO_UNIQUE_ID_USE_UNSPECIFIED;
    lStatus = initCheck();
    if (lStatus != NO_ERROR) {
        ALOGW("createEffect_l() Audio driver not initialized.");
        goto Exit;
    }
    ALOGV("createEffect_l() thread %p effect %s on session %d", this, desc->name, sessionId);
    {
        audio_utils::lock_guard _l(mutex());
        lStatus = checkEffectCompatibility_l(desc, sessionId);
        if (probe || lStatus != NO_ERROR) {
            goto Exit;
        }
        chain = getEffectChain_l(sessionId);
        if (chain == 0) {
            ALOGV("createEffect_l() new effect chain for session %d", sessionId);
            chain = IAfEffectChain::create(this, sessionId);
            addEffectChain_l(chain);
            chain->setStrategy(getStrategyForSession_l(sessionId));
            chainCreated = true;
        } else {
            effect = chain->getEffectFromDesc_l(desc);
        }
        ALOGV("createEffect_l() got effect %p on chain %p", effect.get(), chain.get());
        if (effect == 0) {
            effectId = mAfThreadCallback->nextUniqueId(AUDIO_UNIQUE_ID_USE_EFFECT);
            lStatus = chain->createEffect_l(effect, desc, effectId, sessionId, pinned);
            if (lStatus != NO_ERROR) {
                goto Exit;
            }
            effectCreated = true;
            effect->setDevices(outDeviceTypeAddrs());
            effect->setInputDevice(inDeviceTypeAddr());
            effect->setMode(mAfThreadCallback->getMode());
            effect->setAudioSource(mAudioSource);
        }
        if (effect->isHapticGenerator()) {
            const std::optional<media::AudioVibratorInfo> defaultVibratorInfo =
                    std::move(mAfThreadCallback->getDefaultVibratorInfo_l());
            if (defaultVibratorInfo) {
                effect->setVibratorInfo(*defaultVibratorInfo);
            }
        }
        handle = IAfEffectHandle::create(
                effect, client, effectClient, priority, notifyFramesProcessed);
        lStatus = handle->initCheck();
        if (lStatus == OK) {
            lStatus = effect->addHandle(handle.get());
            sendCheckOutputStageEffectsEvent_l();
        }
        if (enabled != NULL) {
            *enabled = (int)effect->isEnabled();
        }
    }
Exit:
    if (!probe && lStatus != NO_ERROR && lStatus != ALREADY_EXISTS) {
        audio_utils::lock_guard _l(mutex());
        if (effectCreated) {
            chain->removeEffect_l(effect);
        }
        if (chainCreated) {
            removeEffectChain_l(chain);
        }
    }
    *status = lStatus;
    return handle;
}
void ThreadBase::disconnectEffectHandle(IAfEffectHandle* handle,
                                                      bool unpinIfLast)
{
    bool remove = false;
    sp<IAfEffectModule> effect;
    {
        audio_utils::lock_guard _l(mutex());
        sp<IAfEffectBase> effectBase = handle->effect().promote();
        if (effectBase == nullptr) {
            return;
        }
        effect = effectBase->asEffectModule();
        if (effect == nullptr) {
            return;
        }
        remove = (effect->removeHandle(handle) == 0) && (!effect->isPinned() || unpinIfLast);
        if (remove) {
            removeEffect_l(effect, true);
        }
        sendCheckOutputStageEffectsEvent_l();
    }
    if (remove) {
        mAfThreadCallback->updateOrphanEffectChains(effect);
        if (handle->enabled()) {
            effect->checkSuspendOnEffectEnabled(false, false );
        }
    }
}
void ThreadBase::onEffectEnable(const sp<IAfEffectModule>& effect) {
    if (isOffloadOrMmap()) {
        audio_utils::lock_guard _l(mutex());
        broadcast_l();
    }
    if (!effect->isOffloadable()) {
        if (mType == ThreadBase::OFFLOAD) {
            PlaybackThread *t = (PlaybackThread *)this;
            t->invalidateTracks(AUDIO_STREAM_MUSIC);
        }
        if (effect->sessionId() == AUDIO_SESSION_OUTPUT_MIX) {
            mAfThreadCallback->onNonOffloadableGlobalEffectEnable();
        }
    }
}
void ThreadBase::onEffectDisable() {
    if (isOffloadOrMmap()) {
        audio_utils::lock_guard _l(mutex());
        broadcast_l();
    }
}
sp<IAfEffectModule> ThreadBase::getEffect(audio_session_t sessionId,
        int effectId) const
{
    audio_utils::lock_guard _l(mutex());
    return getEffect_l(sessionId, effectId);
}
sp<IAfEffectModule> ThreadBase::getEffect_l(audio_session_t sessionId,
        int effectId) const
{
    sp<IAfEffectChain> chain = getEffectChain_l(sessionId);
    return chain != 0 ? chain->getEffectFromId_l(effectId) : 0;
}
std::vector<int> ThreadBase::getEffectIds_l(audio_session_t sessionId) const
{
    sp<IAfEffectChain> chain = getEffectChain_l(sessionId);
    return chain != nullptr ? chain->getEffectIds() : std::vector<int>{};
}
status_t ThreadBase::addEffect_ll(const sp<IAfEffectModule>& effect)
{
    audio_session_t sessionId = effect->sessionId();
    sp<IAfEffectChain> chain = getEffectChain_l(sessionId);
    bool chainCreated = false;
    ALOGD_IF((mType == OFFLOAD) && !effect->isOffloadable(),
             "%s: on offloaded thread %p: effect %s does not support offload flags %#x",
             __func__, this, effect->desc().name, effect->desc().flags);
    if (chain == 0) {
        ALOGV("%s: new effect chain for session %d", __func__, sessionId);
        chain = IAfEffectChain::create(this, sessionId);
        addEffectChain_l(chain);
        chain->setStrategy(getStrategyForSession_l(sessionId));
        chainCreated = true;
    }
    ALOGV("%s: %p chain %p effect %p", __func__, this, chain.get(), effect.get());
    if (chain->getEffectFromId_l(effect->id()) != 0) {
        ALOGW("%s: %p effect %s already present in chain %p",
                __func__, this, effect->desc().name, chain.get());
        return BAD_VALUE;
    }
    effect->setOffloaded(mType == OFFLOAD, mId);
    status_t status = chain->addEffect_l(effect);
    if (status != NO_ERROR) {
        if (chainCreated) {
            removeEffectChain_l(chain);
        }
        return status;
    }
    effect->setDevices(outDeviceTypeAddrs());
    effect->setInputDevice(inDeviceTypeAddr());
    effect->setMode(mAfThreadCallback->getMode());
    effect->setAudioSource(mAudioSource);
    return NO_ERROR;
}
void ThreadBase::removeEffect_l(const sp<IAfEffectModule>& effect, bool release) {
    ALOGV("%s %p effect %p", __FUNCTION__, this, effect.get());
    effect_descriptor_t desc = effect->desc();
    if ((desc.flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_AUXILIARY) {
        detachAuxEffect_l(effect->id());
    }
    sp<IAfEffectChain> chain = effect->getCallback()->chain().promote();
    if (chain != 0) {
        if (chain->removeEffect_l(effect, release) == 0) {
            removeEffectChain_l(chain);
        }
    } else {
        ALOGW("removeEffect_l() %p cannot promote chain for effect %p", this, effect.get());
    }
}
void ThreadBase::lockEffectChains_l(
        Vector<sp<IAfEffectChain>>& effectChains)
NO_THREAD_SAFETY_ANALYSIS
{
    effectChains = mEffectChains;
    for (size_t i = 0; i < mEffectChains.size(); i++) {
        mEffectChains[i]->mutex().lock();
    }
}
void ThreadBase::unlockEffectChains(
        const Vector<sp<IAfEffectChain>>& effectChains)
NO_THREAD_SAFETY_ANALYSIS
{
    for (size_t i = 0; i < effectChains.size(); i++) {
        effectChains[i]->mutex().unlock();
    }
}
sp<IAfEffectChain> ThreadBase::getEffectChain(audio_session_t sessionId) const
{
    audio_utils::lock_guard _l(mutex());
    return getEffectChain_l(sessionId);
}
sp<IAfEffectChain> ThreadBase::getEffectChain_l(audio_session_t sessionId)
        const
{
    size_t size = mEffectChains.size();
    for (size_t i = 0; i < size; i++) {
        if (mEffectChains[i]->sessionId() == sessionId) {
            return mEffectChains[i];
        }
    }
    return 0;
}
void ThreadBase::setMode(audio_mode_t mode)
{
    audio_utils::lock_guard _l(mutex());
    size_t size = mEffectChains.size();
    for (size_t i = 0; i < size; i++) {
        mEffectChains[i]->setMode_l(mode);
    }
}
void ThreadBase::toAudioPortConfig(struct audio_port_config* config)
{
    config->type = AUDIO_PORT_TYPE_MIX;
    config->ext.mix.handle = mId;
    config->sample_rate = mSampleRate;
    config->format = mHALFormat;
    config->channel_mask = mChannelMask;
    config->config_mask = AUDIO_PORT_CONFIG_SAMPLE_RATE|AUDIO_PORT_CONFIG_CHANNEL_MASK|
                            AUDIO_PORT_CONFIG_FORMAT;
}
void ThreadBase::systemReady()
{
    audio_utils::lock_guard _l(mutex());
    if (mSystemReady) {
        return;
    }
    mSystemReady = true;
    for (size_t i = 0; i < mPendingConfigEvents.size(); i++) {
        sendConfigEvent_l(mPendingConfigEvents.editItemAt(i));
    }
    mPendingConfigEvents.clear();
}
template <typename T>
ssize_t ThreadBase::ActiveTracks<T>::add(const sp<T>& track) {
    ssize_t index = mActiveTracks.indexOf(track);
    if (index >= 0) {
        ALOGW("ActiveTracks<T>::add track %p already there", track.get());
        return index;
    }
    logTrack("add", track);
    mActiveTracksGeneration++;
    mLatestActiveTrack = track;
    ++mBatteryCounter[track->uid()].second;
    mHasChanged = true;
    return mActiveTracks.add(track);
}
template <typename T>
ssize_t ThreadBase::ActiveTracks<T>::remove(const sp<T>& track) {
    ssize_t index = mActiveTracks.remove(track);
    if (index < 0) {
        ALOGW("ActiveTracks<T>::remove nonexistent track %p", track.get());
        return index;
    }
    logTrack("remove", track);
    mActiveTracksGeneration++;
    --mBatteryCounter[track->uid()].second;
    mHasChanged = true;
#ifdef TEE_SINK
    track->dumpTee(-1 , "_REMOVE");
#endif
    track->logEndInterval();
    return index;
}
template <typename T>
void ThreadBase::ActiveTracks<T>::clear() {
    for (const sp<T> &track : mActiveTracks) {
        BatteryNotifier::getInstance().noteStopAudio(track->uid());
        logTrack("clear", track);
    }
    mLastActiveTracksGeneration = mActiveTracksGeneration;
    if (!mActiveTracks.empty()) { mHasChanged = true; }
    mActiveTracks.clear();
    mLatestActiveTrack.clear();
    mBatteryCounter.clear();
}
template <typename T>
void ThreadBase::ActiveTracks<T>::updatePowerState(
        const sp<ThreadBase>& thread, bool force) {
    if (mActiveTracksGeneration != mLastActiveTracksGeneration || force) {
        thread->updateWakeLockUids_l(getWakeLockUids());
        mLastActiveTracksGeneration = mActiveTracksGeneration;
    }
    for (auto it = mBatteryCounter.begin(); it != mBatteryCounter.end();) {
        const uid_t uid = it->first;
        ssize_t &previous = it->second.first;
        ssize_t &current = it->second.second;
        if (current > 0) {
            if (previous == 0) {
                BatteryNotifier::getInstance().noteStartAudio(uid);
            }
            previous = current;
            ++it;
        } else if (current == 0) {
            if (previous > 0) {
                BatteryNotifier::getInstance().noteStopAudio(uid);
            }
            it = mBatteryCounter.erase(it);
        } else {
            LOG_ALWAYS_FATAL("negative battery count %zd", current);
        }
    }
}
template <typename T>
bool ThreadBase::ActiveTracks<T>::readAndClearHasChanged() {
    bool hasChanged = mHasChanged;
    mHasChanged = false;
    for (const sp<T> &track : mActiveTracks) {
        hasChanged |= track->readAndClearHasChanged();
    }
    return hasChanged;
}
template <typename T>
void ThreadBase::ActiveTracks<T>::logTrack(
        const char *funcName, const sp<T> &track) const {
    if (mLocalLog != nullptr) {
        String8 result;
        track->appendDump(result, false );
        mLocalLog->log("AT::%-10s(%p) %s", funcName, track.get(), result.c_str());
    }
}
void ThreadBase::broadcast_l()
{
    mSignalPending = true;
    mWaitWorkCV.notify_all();
}
void ThreadBase::sendStatistics(bool force)
{
    const int64_t nstats = mTimestampVerifier.getN() - mLastRecordedTimestampVerifierN;
    if (nstats == 0) {
        return;
    }
    const int64_t timeNs = systemTime(SYSTEM_TIME_BOOTTIME);
    const int64_t sinceNs = timeNs - mLastRecordedTimeNs;
    if (!force && sinceNs <= 12 * NANOS_PER_HOUR) {
        return;
    }
    mLastRecordedTimestampVerifierN = mTimestampVerifier.getN();
    mLastRecordedTimeNs = timeNs;
    std::unique_ptr<mediametrics::Item> item(mediametrics::Item::create("audiothread"));
#define MM_PREFIX "android.media.audiothread."
    item->setInt32(MM_PREFIX "id", (int32_t)mId);
    item->setCString(MM_PREFIX "type", threadTypeToString(mType));
    item->setInt32(MM_PREFIX "sampleRate", (int32_t)mSampleRate);
    item->setInt64(MM_PREFIX "channelMask", (int64_t)mChannelMask);
    item->setCString(MM_PREFIX "encoding", toString(mFormat).c_str());
    item->setInt32(MM_PREFIX "frameCount", (int32_t)mFrameCount);
    item->setCString(MM_PREFIX "outDevice", toString(outDeviceTypes()).c_str());
    item->setCString(MM_PREFIX "inDevice", toString(inDeviceType()).c_str());
    if (mIoJitterMs.getN() > 0) {
        item->setDouble(MM_PREFIX "ioJitterMs.mean", mIoJitterMs.getMean());
        item->setDouble(MM_PREFIX "ioJitterMs.std", mIoJitterMs.getStdDev());
    }
    if (mProcessTimeMs.getN() > 0) {
        item->setDouble(MM_PREFIX "processTimeMs.mean", mProcessTimeMs.getMean());
        item->setDouble(MM_PREFIX "processTimeMs.std", mProcessTimeMs.getStdDev());
    }
    const auto tsjitter = mTimestampVerifier.getJitterMs();
    if (tsjitter.getN() > 0) {
        item->setDouble(MM_PREFIX "timestampJitterMs.mean", tsjitter.getMean());
        item->setDouble(MM_PREFIX "timestampJitterMs.std", tsjitter.getStdDev());
    }
    if (mLatencyMs.getN() > 0) {
        item->setDouble(MM_PREFIX "latencyMs.mean", mLatencyMs.getMean());
        item->setDouble(MM_PREFIX "latencyMs.std", mLatencyMs.getStdDev());
    }
    if (mMonopipePipeDepthStats.getN() > 0) {
        item->setDouble(MM_PREFIX "monopipePipeDepthStats.mean",
                        mMonopipePipeDepthStats.getMean());
        item->setDouble(MM_PREFIX "monopipePipeDepthStats.std",
                        mMonopipePipeDepthStats.getStdDev());
    }
    item->selfrecord();
}
product_strategy_t ThreadBase::getStrategyForStream(audio_stream_type_t stream) const
{
    if (!mAfThreadCallback->isAudioPolicyReady()) {
        return PRODUCT_STRATEGY_NONE;
    }
    return AudioSystem::getStrategyForStream(stream);
}
void ThreadBase::startMelComputation_l(
        const sp<audio_utils::MelProcessor>& )
{
    ALOGW("%s: ThreadBase does not support CSD", __func__);
}
void ThreadBase::stopMelComputation_l()
{
    ALOGW("%s: ThreadBase does not support CSD", __func__);
}
PlaybackThread::PlaybackThread(const sp<IAfThreadCallback>& afThreadCallback,
                                             AudioStreamOut* output,
                                             audio_io_handle_t id,
                                             type_t type,
                                             bool systemReady,
                                             audio_config_base_t *mixerConfig)
    : ThreadBase(afThreadCallback, id, type, systemReady, true ),
        mNormalFrameCount(0), mSinkBuffer(NULL),
        mMixerBufferEnabled(kEnableExtendedPrecision || type == SPATIALIZER),
        mMixerBuffer(NULL),
        mMixerBufferSize(0),
        mMixerBufferFormat(AUDIO_FORMAT_INVALID),
        mMixerBufferValid(false),
        mEffectBufferEnabled(kEnableExtendedPrecision || type == SPATIALIZER),
        mEffectBuffer(NULL),
        mEffectBufferSize(0),
        mEffectBufferFormat(AUDIO_FORMAT_INVALID),
        mEffectBufferValid(false),
        mSuspended(0), mBytesWritten(0),
        mFramesWritten(0),
        mSuspendedFrames(0),
        mActiveTracks(&this->mLocalLog),
        mTracks(type == MIXER),
        mOutput(output),
        mNumWrites(0), mNumDelayedWrites(0), mInWrite(false),
        mMixerStatus(MIXER_IDLE),
        mMixerStatusIgnoringFastTracks(MIXER_IDLE),
        mStandbyDelayNs(getStandbyTimeInNanos()),
        mBytesRemaining(0),
        mCurrentWriteLength(0),
        mUseAsyncWrite(false),
        mWriteAckSequence(0),
        mDrainSequence(0),
        mScreenState(mAfThreadCallback->getScreenState()),
        mFastTrackAvailMask(((1 << FastMixerState::sMaxFastTracks) - 1) & ~1),
        mHwSupportsPause(false), mHwPaused(false), mFlushPending(false),
        mLeftVolFloat(-1.0), mRightVolFloat(-1.0),
        mDownStreamPatch{},
        mIsTimestampAdvancing(kMinimumTimeBetweenTimestampChecksNs)
{
    snprintf(mThreadName, kThreadNameLength, "AudioOut_%X", id);
    mNBLogWriter = afThreadCallback->newWriter_l(kLogSize, mThreadName);
    mMasterVolume = afThreadCallback->masterVolume_l();
    mMasterMute = afThreadCallback->masterMute_l();
    if (mOutput->audioHwDev) {
        if (mOutput->audioHwDev->canSetMasterVolume()) {
            mMasterVolume = 1.0;
        }
        if (mOutput->audioHwDev->canSetMasterMute()) {
            mMasterMute = false;
        }
        mIsMsdDevice = strcmp(
                mOutput->audioHwDev->moduleName(), AUDIO_HARDWARE_MODULE_ID_MSD) == 0;
    }
    if (mixerConfig != nullptr && mixerConfig->channel_mask != AUDIO_CHANNEL_NONE) {
        mMixerChannelMask = mixerConfig->channel_mask;
    }
    readOutputParameters_l();
    if (mType != SPATIALIZER
            && mMixerChannelMask != mChannelMask) {
        LOG_ALWAYS_FATAL("HAL channel mask %#x does not match mixer channel mask %#x",
                mChannelMask, mMixerChannelMask);
    }
    if (type == MIXER || type == DIRECT || type == OFFLOAD) {
        mTimestampCorrectedDevice = (audio_devices_t)property_get_int64(
                "audio.timestamp.corrected_output_device",
                (int64_t)(mIsMsdDevice ? AUDIO_DEVICE_OUT_BUS
                                       : AUDIO_DEVICE_NONE));
    }
    for (int i = AUDIO_STREAM_MIN; i < AUDIO_STREAM_FOR_POLICY_CNT; ++i) {
        const audio_stream_type_t stream{static_cast<audio_stream_type_t>(i)};
        mStreamTypes[stream].volume = 0.0f;
        mStreamTypes[stream].mute = mAfThreadCallback->streamMute_l(stream);
    }
    mStreamTypes[AUDIO_STREAM_PATCH].volume = 1.0f;
    mStreamTypes[AUDIO_STREAM_PATCH].mute = false;
    mStreamTypes[AUDIO_STREAM_CALL_ASSISTANT].volume = 1.0f;
    mStreamTypes[AUDIO_STREAM_CALL_ASSISTANT].mute = false;
}
PlaybackThread::~PlaybackThread()
{
    mAfThreadCallback->unregisterWriter(mNBLogWriter);
    free(mSinkBuffer);
    free(mMixerBuffer);
    free(mEffectBuffer);
    free(mPostSpatializerBuffer);
}
void PlaybackThread::onFirstRef()
{
    if (!isStreamInitialized()) {
        ALOGE("The stream is not open yet");
    } else {
        if (mOutput->flags & AUDIO_OUTPUT_FLAG_NON_BLOCKING &&
                mOutput->stream->setCallback(this) == OK) {
            mUseAsyncWrite = true;
            mCallbackThread = sp<AsyncCallbackThread>::make(this);
        }
        if (mOutput->stream->setEventCallback(this) != OK) {
            ALOGD("Failed to add event callback");
        }
    }
    run(mThreadName, ANDROID_PRIORITY_URGENT_AUDIO);
    mThreadSnapshot.setTid(getTid());
}
void PlaybackThread::preExit()
{
    ALOGV("  preExit()");
    status_t result = mOutput->stream->exit();
    ALOGE_IF(result != OK, "Error when calling exit(): %d", result);
}
void PlaybackThread::dumpTracks_l(int fd, const Vector<String16>& )
{
    String8 result;
    result.appendFormat("  Stream volumes in dB: ");
    for (int i = 0; i < AUDIO_STREAM_CNT; ++i) {
        const stream_type_t *st = &mStreamTypes[i];
        if (i > 0) {
            result.appendFormat(", ");
        }
        result.appendFormat("%d:%.2g", i, 20.0 * log10(st->volume));
        if (st->mute) {
            result.append("M");
        }
    }
    result.append("\n");
    write(fd, result.c_str(), result.length());
    result.clear();
    FastTrackUnderruns underruns = getFastTrackUnderruns(0);
    dprintf(fd, "  Normal mixer raw underrun counters: partial=%u empty=%u\n",
            underruns.mBitFields.mPartial, underruns.mBitFields.mEmpty);
    size_t numtracks = mTracks.size();
    size_t numactive = mActiveTracks.size();
    dprintf(fd, "  %zu Tracks", numtracks);
    size_t numactiveseen = 0;
    const char *prefix = "    ";
    if (numtracks) {
        dprintf(fd, " of which %zu are active\n", numactive);
        result.append(prefix);
        mTracks[0]->appendDumpHeader(result);
        for (size_t i = 0; i < numtracks; ++i) {
            sp<IAfTrack> track = mTracks[i];
            if (track != 0) {
                bool active = mActiveTracks.indexOf(track) >= 0;
                if (active) {
                    numactiveseen++;
                }
                result.append(prefix);
                track->appendDump(result, active);
            }
        }
    } else {
        result.append("\n");
    }
    if (numactiveseen != numactive) {
        result.append("  The following tracks are in the active list but"
                " not in the track list\n");
        result.append(prefix);
        mActiveTracks[0]->appendDumpHeader(result);
        for (size_t i = 0; i < numactive; ++i) {
            sp<IAfTrack> track = mActiveTracks[i];
            if (mTracks.indexOf(track) < 0) {
                result.append(prefix);
                track->appendDump(result, true );
            }
        }
    }
    write(fd, result.c_str(), result.size());
}
void PlaybackThread::dumpInternals_l(int fd, const Vector<String16>& args)
{
    dprintf(fd, "  Master volume: %f\n", mMasterVolume);
    dprintf(fd, "  Master mute: %s\n", mMasterMute ? "on" : "off");
    dprintf(fd, "  Mixer channel Mask: %#x (%s)\n",
            mMixerChannelMask, channelMaskToString(mMixerChannelMask, true ).c_str());
    if (mHapticChannelMask != AUDIO_CHANNEL_NONE) {
        dprintf(fd, "  Haptic channel mask: %#x (%s)\n", mHapticChannelMask,
                channelMaskToString(mHapticChannelMask, true ).c_str());
    }
    dprintf(fd, "  Normal frame count: %zu\n", mNormalFrameCount);
    dprintf(fd, "  Total writes: %d\n", mNumWrites);
    dprintf(fd, "  Delayed writes: %d\n", mNumDelayedWrites);
    dprintf(fd, "  Blocked in write: %s\n", mInWrite ? "yes" : "no");
    dprintf(fd, "  Suspend count: %d\n", mSuspended);
    dprintf(fd, "  Sink buffer : %p\n", mSinkBuffer);
    dprintf(fd, "  Mixer buffer: %p\n", mMixerBuffer);
    dprintf(fd, "  Effect buffer: %p\n", mEffectBuffer);
    dprintf(fd, "  Fast track availMask=%#x\n", mFastTrackAvailMask);
    dprintf(fd, "  Standby delay ns=%lld\n", (long long)mStandbyDelayNs);
    AudioStreamOut *output = mOutput;
    audio_output_flags_t flags = output != NULL ? output->flags : AUDIO_OUTPUT_FLAG_NONE;
    dprintf(fd, "  AudioStreamOut: %p flags %#x (%s)\n",
            output, flags, toString(flags).c_str());
    dprintf(fd, "  Frames written: %lld\n", (long long)mFramesWritten);
    dprintf(fd, "  Suspended frames: %lld\n", (long long)mSuspendedFrames);
    if (mPipeSink.get() != nullptr) {
        dprintf(fd, "  PipeSink frames written: %lld\n", (long long)mPipeSink->framesWritten());
    }
    if (output != nullptr) {
        dprintf(fd, "  Hal stream dump:\n");
        (void)output->stream->dump(fd, args);
    }
}
sp<IAfTrack> PlaybackThread::createTrack_l(
        const sp<Client>& client,
        audio_stream_type_t streamType,
        const audio_attributes_t& attr,
        uint32_t *pSampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        size_t *pFrameCount,
        size_t *pNotificationFrameCount,
        uint32_t notificationsPerBuffer,
        float speed,
        const sp<IMemory>& sharedBuffer,
        audio_session_t sessionId,
        audio_output_flags_t *flags,
        pid_t creatorPid,
        const AttributionSourceState& attributionSource,
        pid_t tid,
        status_t *status,
        audio_port_handle_t portId,
        const sp<media::IAudioTrackCallback>& callback,
        bool isSpatialized,
        bool isBitPerfect)
{
    size_t frameCount = *pFrameCount;
    size_t notificationFrameCount = *pNotificationFrameCount;
    sp<IAfTrack> track;
    status_t lStatus;
    audio_output_flags_t outputFlags = mOutput->flags;
    audio_output_flags_t requestedFlags = *flags;
    uint32_t sampleRate;
    if (sharedBuffer != 0 && checkIMemory(sharedBuffer) != NO_ERROR) {
        lStatus = BAD_VALUE;
        goto Exit;
    }
    if (*pSampleRate == 0) {
        *pSampleRate = mSampleRate;
    }
    sampleRate = *pSampleRate;
    if (hasFastMixer()) {
        outputFlags = (audio_output_flags_t)(outputFlags | AUDIO_OUTPUT_FLAG_FAST);
    }
    if ((*flags & outputFlags) != *flags) {
        ALOGW("createTrack_l(): mismatch between requested flags (%08x) and output flags (%08x)",
              *flags, outputFlags);
        *flags = (audio_output_flags_t)(*flags & outputFlags);
    }
    if (isBitPerfect) {
        sp<IAfEffectChain> chain = getEffectChain_l(sessionId);
        if (chain.get() != nullptr) {
            audio_output_flags_t flagsToCheck =
                    (audio_output_flags_t)(*flags & AUDIO_OUTPUT_FLAG_BIT_PERFECT);
            chain->checkOutputFlagCompatibility(&flagsToCheck);
            if ((flagsToCheck & AUDIO_OUTPUT_FLAG_BIT_PERFECT) == AUDIO_OUTPUT_FLAG_NONE) {
                ALOGE("%s cannot create track as there is data-processing effect attached to "
                      "given session id(%d)", __func__, sessionId);
                lStatus = BAD_VALUE;
                goto Exit;
            }
            *flags = flagsToCheck;
        }
    }
    if (*flags & AUDIO_OUTPUT_FLAG_FAST) {
      if (
            audio_is_linear_pcm(format) &&
            (channelMask == (mChannelMask | mHapticChannelMask) ||
                    mChannelMask != AUDIO_CHANNEL_OUT_STEREO ||
                    (channelMask == AUDIO_CHANNEL_OUT_MONO
                                                                             )) &&
            (sampleRate == mSampleRate) &&
            hasFastMixer() &&
            (mFastTrackAvailMask != 0)
        ) {
        if (sharedBuffer == 0) {
            int ok = pthread_once(&sFastTrackMultiplierOnce, sFastTrackMultiplierInit);
            if (ok != 0) {
                ALOGE("%s pthread_once failed: %d", __func__, ok);
            }
            frameCount = max(frameCount, mFrameCount * sFastTrackMultiplier);
        }
        {
            audio_utils::lock_guard _l(mutex());
            for (audio_session_t session : {
                    AUDIO_SESSION_DEVICE,
                    AUDIO_SESSION_OUTPUT_STAGE,
                    AUDIO_SESSION_OUTPUT_MIX,
                    sessionId,
                }) {
                sp<IAfEffectChain> chain = getEffectChain_l(session);
                if (chain.get() != nullptr) {
                    audio_output_flags_t old = *flags;
                    chain->checkOutputFlagCompatibility(flags);
                    if (old != *flags) {
                        ALOGV("AUDIO_OUTPUT_FLAGS denied by effect, session=%d old=%#x new=%#x",
                                (int)session, (int)old, (int)*flags);
                    }
                }
            }
        }
        ALOGV_IF((*flags & AUDIO_OUTPUT_FLAG_FAST) != 0,
                 "AUDIO_OUTPUT_FLAG_FAST accepted: frameCount=%zu mFrameCount=%zu",
                 frameCount, mFrameCount);
      } else {
        ALOGD("AUDIO_OUTPUT_FLAG_FAST denied: sharedBuffer=%p frameCount=%zu "
                "mFrameCount=%zu format=%#x mFormat=%#x isLinear=%d channelMask=%#x "
                "sampleRate=%u mSampleRate=%u "
                "hasFastMixer=%d tid=%d fastTrackAvailMask=%#x",
                sharedBuffer.get(), frameCount, mFrameCount, format, mFormat,
                audio_is_linear_pcm(format), channelMask, sampleRate,
                mSampleRate, hasFastMixer(), tid, mFastTrackAvailMask);
        *flags = (audio_output_flags_t)(*flags & ~AUDIO_OUTPUT_FLAG_FAST);
      }
    }
    if (!audio_has_proportional_frames(format)) {
        if (sharedBuffer != 0) {
            frameCount = sharedBuffer->size();
        } else if (frameCount == 0) {
            frameCount = mNormalFrameCount;
        }
        if (notificationFrameCount != frameCount) {
            notificationFrameCount = frameCount;
        }
    } else if (sharedBuffer != 0) {
        size_t alignment = audio_bytes_per_sample(format);
        if (alignment & 1) {
            alignment = 1;
        }
        uint32_t channelCount = audio_channel_count_from_out_mask(channelMask);
        size_t frameSize = channelCount * audio_bytes_per_sample(format);
        if (channelCount > 1) {
            alignment <<= 1;
        }
        if (((uintptr_t)sharedBuffer->unsecurePointer() & (alignment - 1)) != 0) {
            ALOGE("Invalid buffer alignment: address %p, channel count %u",
                  sharedBuffer->unsecurePointer(), channelCount);
            lStatus = BAD_VALUE;
            goto Exit;
        }
        frameCount = sharedBuffer->size() / frameSize;
    } else {
        size_t minFrameCount = 0;
        if (*flags & AUDIO_OUTPUT_FLAG_FAST) {
            if (notificationsPerBuffer > 0) {
                if (notificationsPerBuffer > SIZE_MAX / mFrameCount) {
                    ALOGE("Requested notificationPerBuffer=%u ignored for HAL frameCount=%zu",
                          notificationsPerBuffer, mFrameCount);
                } else {
                    minFrameCount = mFrameCount * notificationsPerBuffer;
                }
            }
        } else {
            uint32_t latencyMs = latency_l();
            if (latencyMs == 0) {
                ALOGE("Error when retrieving output stream latency");
                lStatus = UNKNOWN_ERROR;
                goto Exit;
            }
            minFrameCount = AudioSystem::calculateMinFrameCount(latencyMs, mNormalFrameCount,
                                mSampleRate, sampleRate, speed );
        }
        if (frameCount < minFrameCount) {
            frameCount = minFrameCount;
        }
    }
    if (sharedBuffer == 0 && audio_is_linear_pcm(format)) {
        size_t maxNotificationFrames;
        if (*flags & AUDIO_OUTPUT_FLAG_FAST) {
            maxNotificationFrames = mFrameCount;
        } else {
            const int nBuffering =
                    (uint64_t{frameCount} * mSampleRate)
                            / (uint64_t{mNormalFrameCount} * sampleRate) == 3 ? 3 : 2;
            maxNotificationFrames = frameCount / nBuffering;
            if (requestedFlags & AUDIO_OUTPUT_FLAG_FAST) {
                size_t maxNotificationFramesFastDenied = FMS_20 * sampleRate / 1000;
                if (maxNotificationFrames > maxNotificationFramesFastDenied) {
                    maxNotificationFrames = maxNotificationFramesFastDenied;
                }
            }
        }
        if (notificationFrameCount == 0 || notificationFrameCount > maxNotificationFrames) {
            if (notificationFrameCount == 0) {
                ALOGD("Client defaulted notificationFrames to %zu for frameCount %zu",
                    maxNotificationFrames, frameCount);
            } else {
                ALOGW("Client adjusted notificationFrames from %zu to %zu for frameCount %zu",
                      notificationFrameCount, maxNotificationFrames, frameCount);
            }
            notificationFrameCount = maxNotificationFrames;
        }
    }
    *pFrameCount = frameCount;
    *pNotificationFrameCount = notificationFrameCount;
    switch (mType) {
    case BIT_PERFECT:
        if (isBitPerfect) {
            if (sampleRate != mSampleRate || format != mFormat || channelMask != mChannelMask) {
                ALOGE("%s, bad parameter when request streaming bit-perfect, sampleRate=%u, "
                      "format=%#x, channelMask=%#x, mSampleRate=%u, mFormat=%#x, mChannelMask=%#x",
                      __func__, sampleRate, format, channelMask, mSampleRate, mFormat,
                      mChannelMask);
                lStatus = BAD_VALUE;
                goto Exit;
            }
        }
        break;
    case DIRECT:
        if (audio_is_linear_pcm(format)) {
            if (sampleRate != mSampleRate || format != mFormat || channelMask != mChannelMask) {
                ALOGE("createTrack_l() Bad parameter: sampleRate %u format %#x, channelMask 0x%08x "
                        "for output %p with format %#x",
                        sampleRate, format, channelMask, mOutput, mFormat);
                lStatus = BAD_VALUE;
                goto Exit;
            }
        }
        break;
    case OFFLOAD:
        if (sampleRate != mSampleRate || format != mFormat || channelMask != mChannelMask) {
            ALOGE("createTrack_l() Bad parameter: sampleRate %d format %#x, channelMask 0x%08x \""
                    "for output %p with format %#x",
                    sampleRate, format, channelMask, mOutput, mFormat);
            lStatus = BAD_VALUE;
            goto Exit;
        }
        break;
    default:
        if (!audio_is_linear_pcm(format)) {
                ALOGE("createTrack_l() Bad parameter: format %#x \""
                        "for output %p with format %#x",
                        format, mOutput, mFormat);
                lStatus = BAD_VALUE;
                goto Exit;
        }
        if (sampleRate > mSampleRate * AUDIO_RESAMPLER_DOWN_RATIO_MAX) {
            ALOGE("Sample rate out of range: %u mSampleRate %u", sampleRate, mSampleRate);
            lStatus = BAD_VALUE;
            goto Exit;
        }
        break;
    }
    lStatus = initCheck();
    if (lStatus != NO_ERROR) {
        ALOGE("createTrack_l() audio driver not initialized");
        goto Exit;
    }
    {
        audio_utils::lock_guard _l(mutex());
        product_strategy_t strategy = getStrategyForStream(streamType);
        for (size_t i = 0; i < mTracks.size(); ++i) {
            sp<IAfTrack> t = mTracks[i];
            if (t != 0 && t->isExternalTrack()) {
                product_strategy_t actual = getStrategyForStream(t->streamType());
                if (sessionId == t->sessionId() && strategy != actual) {
                    ALOGE("createTrack_l() mismatched strategy; expected %u but found %u",
                            strategy, actual);
                    lStatus = BAD_VALUE;
                    goto Exit;
                }
            }
        }
        audio_output_flags_t trackFlags = *flags;
        if (mType == DIRECT) {
            trackFlags = static_cast<audio_output_flags_t>(trackFlags | AUDIO_OUTPUT_FLAG_DIRECT);
        }
        track = IAfTrack::create(this, client, streamType, attr, sampleRate, format,
                          channelMask, frameCount,
                          nullptr , (size_t)0 , sharedBuffer,
                          sessionId, creatorPid, attributionSource, trackFlags,
                          IAfTrackBase::TYPE_DEFAULT, portId, SIZE_MAX ,
                          speed, isSpatialized, isBitPerfect);
        lStatus = track != 0 ? track->initCheck() : (status_t) NO_MEMORY;
        if (lStatus != NO_ERROR) {
            ALOGE("createTrack_l() initCheck failed %d; no control block?", lStatus);
            goto Exit;
        }
        mTracks.add(track);
        {
            audio_utils::lock_guard _atCbL(audioTrackCbMutex());
            if (callback.get() != nullptr) {
                mAudioTrackCallbacks.emplace(track, callback);
            }
        }
        sp<IAfEffectChain> chain = getEffectChain_l(sessionId);
        if (chain != 0) {
            ALOGV("createTrack_l() setting main buffer %p", chain->inBuffer());
            track->setMainBuffer(chain->inBuffer());
            chain->setStrategy(getStrategyForStream(track->streamType()));
            chain->incTrackCnt();
        }
        if ((*flags & AUDIO_OUTPUT_FLAG_FAST) && (tid != -1)) {
            pid_t callingPid = IPCThreadState::self()->getCallingPid();
            sendPrioConfigEvent_l(callingPid, tid, kPriorityAudioApp, true );
        }
    }
    lStatus = NO_ERROR;
Exit:
    *status = lStatus;
    return track;
}
template<typename T>
ssize_t PlaybackThread::Tracks<T>::remove(const sp<T>& track)
{
    const int trackId = track->id();
    const ssize_t index = mTracks.remove(track);
    if (index >= 0) {
        if (mSaveDeletedTrackIds) {
            mDeletedTrackIds.emplace(trackId);
        }
    }
    return index;
}
uint32_t PlaybackThread::correctLatency_l(uint32_t latency) const
{
    return latency;
}
uint32_t PlaybackThread::latency() const
{
    audio_utils::lock_guard _l(mutex());
    return latency_l();
}
uint32_t PlaybackThread::latency_l() const
{
    uint32_t latency;
    if (initCheck() == NO_ERROR && mOutput->stream->getLatency(&latency) == OK) {
        return correctLatency_l(latency);
    }
    return 0;
}
void PlaybackThread::setMasterVolume(float value)
{
    audio_utils::lock_guard _l(mutex());
    if (mOutput && mOutput->audioHwDev &&
        mOutput->audioHwDev->canSetMasterVolume()) {
        mMasterVolume = 1.0;
    } else {
        mMasterVolume = value;
    }
}
void PlaybackThread::setMasterBalance(float balance)
{
    mMasterBalance.store(balance);
}
void PlaybackThread::setMasterMute(bool muted)
{
    if (isDuplicating()) {
        return;
    }
    audio_utils::lock_guard _l(mutex());
    if (mOutput && mOutput->audioHwDev &&
        mOutput->audioHwDev->canSetMasterMute()) {
        mMasterMute = false;
    } else {
        mMasterMute = muted;
    }
}
void PlaybackThread::setStreamVolume(audio_stream_type_t stream, float value)
{
    audio_utils::lock_guard _l(mutex());
    mStreamTypes[stream].volume = value;
    broadcast_l();
}
void PlaybackThread::setStreamMute(audio_stream_type_t stream, bool muted)
{
    audio_utils::lock_guard _l(mutex());
    mStreamTypes[stream].mute = muted;
    broadcast_l();
}
float PlaybackThread::streamVolume(audio_stream_type_t stream) const
{
    audio_utils::lock_guard _l(mutex());
    return mStreamTypes[stream].volume;
}
void PlaybackThread::setVolumeForOutput_l(float left, float right) const
{
    mOutput->stream->setVolume(left, right);
}
status_t PlaybackThread::addTrack_l(const sp<IAfTrack>& track)
NO_THREAD_SAFETY_ANALYSIS
{
    status_t status = ALREADY_EXISTS;
    if (mActiveTracks.indexOf(track) < 0) {
        if (track->isExternalTrack()) {
            IAfTrackBase::track_state state = track->state();
            mutex().unlock();
            status = AudioSystem::startOutput(track->portId());
            mutex().lock();
            if (state != track->state()) {
                if (status == NO_ERROR) {
                    mutex().unlock();
                    AudioSystem::stopOutput(track->portId());
                    mutex().lock();
                }
                return INVALID_OPERATION;
            }
            if (status != NO_ERROR) {
                return status == DEAD_OBJECT ? status : PERMISSION_DENIED;
            }
#ifdef ADD_BATTERY_DATA
            addBatteryData(IMediaPlayerService::kBatteryDataAudioFlingerStart);
#endif
            sendIoConfigEvent_l(AUDIO_CLIENT_STARTED, track->creatorPid(), track->portId());
        }
        if (track->isOffloaded()) {
            if (track->isStopping_1()) {
                track->retryCount() = kMaxTrackStopRetriesOffload;
            } else {
                track->retryCount() = kMaxTrackStartupRetriesOffload;
            }
            track->fillingStatus() = mStandby ? IAfTrack::FS_FILLING : IAfTrack::FS_FILLED;
        } else {
            track->retryCount() = kMaxTrackStartupRetries;
            track->fillingStatus() =
                    track->sharedBuffer() != 0 ? IAfTrack::FS_FILLED : IAfTrack::FS_FILLING;
        }
        sp<IAfEffectChain> chain = getEffectChain_l(track->sessionId());
        if (mHapticChannelMask != AUDIO_CHANNEL_NONE
                && ((track->channelMask() & AUDIO_CHANNEL_HAPTIC_ALL) != AUDIO_CHANNEL_NONE
                        || (chain != nullptr && chain->containsHapticGeneratingEffect_l()))) {
            mutex().unlock();
            const os::HapticScale intensity = afutils::onExternalVibrationStart(
                    track->getExternalVibration());
            std::optional<media::AudioVibratorInfo> vibratorInfo;
            {
                 audio_utils::lock_guard _l(mAfThreadCallback->mutex());
                vibratorInfo = std::move(mAfThreadCallback->getDefaultVibratorInfo_l());
            }
            mutex().lock();
            track->setHapticIntensity(intensity);
            if (vibratorInfo) {
                track->setHapticMaxAmplitude(vibratorInfo->maxAmplitude);
            }
            if (track->getHapticPlaybackEnabled()) {
                for (const auto &t : mActiveTracks) {
                    t->setHapticPlaybackEnabled(false);
                }
            }
            if (chain != nullptr) {
                chain->setHapticIntensity_l(track->id(), intensity);
            }
        }
        track->setResetDone(false);
        track->resetPresentationComplete();
        mActiveTracks.add(track);
        if (chain != 0) {
            ALOGV("addTrack_l() starting track on chain %p for session %d", chain.get(),
                    track->sessionId());
            chain->incActiveTrackCnt();
        }
        track->logBeginInterval(patchSinksToString(&mPatch));
        status = NO_ERROR;
    }
    onAddNewTrack_l();
    return status;
}
bool PlaybackThread::destroyTrack_l(const sp<IAfTrack>& track)
{
    track->terminate();
    bool trackActive = (mActiveTracks.indexOf(track) >= 0);
    track->setState(IAfTrackBase::STOPPED);
    if (!trackActive) {
        removeTrack_l(track);
    } else if (track->isFastTrack() || track->isOffloaded() || track->isDirect()) {
        if (track->isPausePending()) {
            track->pauseAck();
        }
        track->setState(IAfTrackBase::STOPPING_1);
    }
    return trackActive;
}
void PlaybackThread::removeTrack_l(const sp<IAfTrack>& track)
{
    track->triggerEvents(AudioSystem::SYNC_EVENT_PRESENTATION_COMPLETE);
    String8 result;
    track->appendDump(result, false );
    mLocalLog.log("removeTrack_l (%p) %s", track.get(), result.c_str());
    mTracks.remove(track);
    {
        audio_utils::lock_guard _atCbL(audioTrackCbMutex());
        mAudioTrackCallbacks.erase(track);
    }
    if (track->isFastTrack()) {
        int index = track->fastIndex();
        ALOG_ASSERT(0 < index && index < (int)FastMixerState::sMaxFastTracks);
        ALOG_ASSERT(!(mFastTrackAvailMask & (1 << index)));
        mFastTrackAvailMask |= 1 << index;
        track->fastIndex() = -1;
    }
    sp<IAfEffectChain> chain = getEffectChain_l(track->sessionId());
    if (chain != 0) {
        chain->decTrackCnt();
    }
}
String8 PlaybackThread::getParameters(const String8& keys)
{
    audio_utils::lock_guard _l(mutex());
    String8 out_s8;
    if (initCheck() == NO_ERROR && mOutput->stream->getParameters(keys, &out_s8) == OK) {
        return out_s8;
    }
    return {};
}
status_t DirectOutputThread::selectPresentation(int presentationId, int programId) {
    audio_utils::lock_guard _l(mutex());
    if (!isStreamInitialized()) {
        return NO_INIT;
    }
    return mOutput->stream->selectPresentation(presentationId, programId);
}
void PlaybackThread::ioConfigChanged(audio_io_config_event_t event, pid_t pid,
                                                   audio_port_handle_t portId) {
    ALOGV("PlaybackThread::ioConfigChanged, thread %p, event %d", this, event);
    sp<AudioIoDescriptor> desc;
    const struct audio_patch patch = isMsdDevice() ? mDownStreamPatch : mPatch;
    switch (event) {
    case AUDIO_OUTPUT_OPENED:
    case AUDIO_OUTPUT_REGISTERED:
    case AUDIO_OUTPUT_CONFIG_CHANGED:
        desc = sp<AudioIoDescriptor>::make(mId, patch, false ,
                mSampleRate, mFormat, mChannelMask,
                mNormalFrameCount, mFrameCount, latency_l());
        break;
    case AUDIO_CLIENT_STARTED:
        desc = sp<AudioIoDescriptor>::make(mId, patch, portId);
        break;
    case AUDIO_OUTPUT_CLOSED:
    default:
        desc = sp<AudioIoDescriptor>::make(mId);
        break;
    }
    mAfThreadCallback->ioConfigChanged(event, desc, pid);
}
void PlaybackThread::onWriteReady()
{
    mCallbackThread->resetWriteBlocked();
}
void PlaybackThread::onDrainReady()
{
    mCallbackThread->resetDraining();
}
void PlaybackThread::onError()
{
    mCallbackThread->setAsyncError();
}
void PlaybackThread::onCodecFormatChanged(
        const std::basic_string<uint8_t>& metadataBs)
{
    const auto weakPointerThis = wp<PlaybackThread>::fromExisting(this);
    std::thread([this, metadataBs, weakPointerThis]() {
            const sp<PlaybackThread> playbackThread = weakPointerThis.promote();
            if (playbackThread == nullptr) {
                ALOGW("PlaybackThread was destroyed, skip codec format change event");
                return;
            }
            audio_utils::metadata::Data metadata =
                    audio_utils::metadata::dataFromByteString(metadataBs);
            if (metadata.empty()) {
                ALOGW("Can not transform the buffer to audio metadata, %s, %d",
                      reinterpret_cast<char*>(const_cast<uint8_t*>(metadataBs.data())),
                      (int)metadataBs.size());
                return;
            }
            audio_utils::metadata::ByteString metaDataStr =
                    audio_utils::metadata::byteStringFromData(metadata);
            std::vector metadataVec(metaDataStr.begin(), metaDataStr.end());
            audio_utils::lock_guard _l(audioTrackCbMutex());
            for (const auto& callbackPair : mAudioTrackCallbacks) {
                callbackPair.second->onCodecFormatChanged(metadataVec);
            }
    }).detach();
}
void PlaybackThread::resetWriteBlocked(uint32_t sequence)
{
    audio_utils::lock_guard _l(mutex());
    if ((mWriteAckSequence & 1) && (sequence == mWriteAckSequence)) {
        mWriteAckSequence &= ~1;
        mWaitWorkCV.notify_one();
    }
}
void PlaybackThread::resetDraining(uint32_t sequence)
{
    audio_utils::lock_guard _l(mutex());
    if ((mDrainSequence & 1) && (sequence == mDrainSequence)) {
        mTimestampVerifier.discontinuity(mTimestampVerifier.DISCONTINUITY_MODE_ZERO);
        mDrainSequence &= ~1;
        mWaitWorkCV.notify_one();
    }
}
void PlaybackThread::readOutputParameters_l()
NO_THREAD_SAFETY_ANALYSIS
{
    const audio_config_base_t audioConfig = mOutput->getAudioProperties();
    mSampleRate = audioConfig.sample_rate;
    mChannelMask = audioConfig.channel_mask;
    if (!audio_is_output_channel(mChannelMask)) {
        LOG_ALWAYS_FATAL("HAL channel mask %#x not valid for output", mChannelMask);
    }
    if (hasMixer() && !isValidPcmSinkChannelMask(mChannelMask)) {
        LOG_ALWAYS_FATAL("HAL channel mask %#x not supported for mixed output",
                mChannelMask);
    }
    if (mMixerChannelMask == AUDIO_CHANNEL_NONE) {
        mMixerChannelMask = mChannelMask;
    }
    mChannelCount = audio_channel_count_from_out_mask(mChannelMask);
    mBalance.setChannelMask(mChannelMask);
    uint32_t mixerChannelCount = audio_channel_count_from_out_mask(mMixerChannelMask);
    status_t result = mOutput->stream->getAudioProperties(nullptr, nullptr, &mHALFormat);
    LOG_ALWAYS_FATAL_IF(result != OK, "Error when retrieving output stream format: %d", result);
    mFormat = audioConfig.format;
    if (!audio_is_valid_format(mFormat)) {
        LOG_ALWAYS_FATAL("HAL format %#x not valid for output", mFormat);
    }
    if (hasMixer() && !isValidPcmSinkFormat(mFormat)) {
        LOG_FATAL("HAL format %#x not supported for mixed output",
                mFormat);
    }
    mFrameSize = mOutput->getFrameSize();
    result = mOutput->stream->getBufferSize(&mBufferSize);
    LOG_ALWAYS_FATAL_IF(result != OK,
            "Error when retrieving output stream buffer size: %d", result);
    mFrameCount = mBufferSize / mFrameSize;
    if (hasMixer() && (mFrameCount & 15)) {
        ALOGW("HAL output buffer size is %zu frames but AudioMixer requires multiples of 16 frames",
                mFrameCount);
    }
    mHwSupportsPause = false;
    if (mOutput->flags & AUDIO_OUTPUT_FLAG_DIRECT) {
        bool supportsPause = false, supportsResume = false;
        if (mOutput->stream->supportsPauseAndResume(&supportsPause, &supportsResume) == OK) {
            if (supportsPause && supportsResume) {
                mHwSupportsPause = true;
            } else if (supportsPause) {
                ALOGW("direct output implements pause but not resume");
            } else if (supportsResume) {
                ALOGW("direct output implements resume but not pause");
            }
        }
    }
    if (!mHwSupportsPause && mOutput->flags & AUDIO_OUTPUT_FLAG_HW_AV_SYNC) {
        LOG_ALWAYS_FATAL("HW_AV_SYNC requested but HAL does not implement pause and resume");
    }
    if (mType == DUPLICATING && mMixerBufferEnabled && mEffectBufferEnabled) {
        mFormat = AUDIO_FORMAT_PCM_FLOAT;
        mFrameSize = mChannelCount * audio_bytes_per_sample(mFormat);
        mBufferSize = mFrameSize * mFrameCount;
    }
    double multiplier = 1.0;
    if (mType == MIXER && (kUseFastMixer == FastMixer_Static ||
            kUseFastMixer == FastMixer_Dynamic)) {
        size_t minNormalFrameCount = (kMinNormalSinkBufferSizeMs * mSampleRate) / 1000;
        size_t maxNormalFrameCount = (kMaxNormalSinkBufferSizeMs * mSampleRate) / 1000;
        minNormalFrameCount = (minNormalFrameCount + 15) & ~15;
        maxNormalFrameCount = maxNormalFrameCount & ~15;
        if (maxNormalFrameCount < minNormalFrameCount) {
            maxNormalFrameCount = minNormalFrameCount;
        }
        multiplier = (double) minNormalFrameCount / (double) mFrameCount;
        if (multiplier <= 1.0) {
            multiplier = 1.0;
        } else if (multiplier <= 2.0) {
            if (2 * mFrameCount <= maxNormalFrameCount) {
                multiplier = 2.0;
            } else {
                multiplier = (double) maxNormalFrameCount / (double) mFrameCount;
            }
        } else {
            multiplier = floor(multiplier);
        }
    }
    mNormalFrameCount = multiplier * mFrameCount;
    if (hasMixer()) {
        mNormalFrameCount = (mNormalFrameCount + 15) & ~15;
    }
    ALOGI("HAL output buffer size %zu frames, normal sink buffer size %zu frames", mFrameCount,
            mNormalFrameCount);
    mThreadThrottle = property_get_bool("af.thread.throttle", true );
    mThreadThrottleTimeMs = 0;
    mThreadThrottleEndMs = 0;
    mHalfBufferMs = mNormalFrameCount * 1000 / (2 * mSampleRate);
    free(mSinkBuffer);
    mSinkBuffer = NULL;
    const size_t sinkBufferSize = mNormalFrameCount * mFrameSize;
    (void)posix_memalign(&mSinkBuffer, 32, sinkBufferSize);
    free(mMixerBuffer);
    mMixerBuffer = NULL;
    if (mMixerBufferEnabled) {
        mMixerBufferFormat = AUDIO_FORMAT_PCM_FLOAT;
        mMixerBufferSize = mNormalFrameCount * mixerChannelCount
                * audio_bytes_per_sample(mMixerBufferFormat);
        (void)posix_memalign(&mMixerBuffer, 32, mMixerBufferSize);
    }
    free(mEffectBuffer);
    mEffectBuffer = NULL;
    if (mEffectBufferEnabled) {
        mEffectBufferFormat = AUDIO_FORMAT_PCM_FLOAT;
        mEffectBufferSize = mNormalFrameCount * mixerChannelCount
                * audio_bytes_per_sample(mEffectBufferFormat);
        (void)posix_memalign(&mEffectBuffer, 32, mEffectBufferSize);
    }
    if (mType == SPATIALIZER) {
        free(mPostSpatializerBuffer);
        mPostSpatializerBuffer = nullptr;
        mPostSpatializerBufferSize = mNormalFrameCount * mChannelCount
                * audio_bytes_per_sample(mEffectBufferFormat);
        (void)posix_memalign(&mPostSpatializerBuffer, 32, mPostSpatializerBufferSize);
    }
    mHapticChannelMask = static_cast<audio_channel_mask_t>(mChannelMask & AUDIO_CHANNEL_HAPTIC_ALL);
    mChannelMask = static_cast<audio_channel_mask_t>(mChannelMask & ~mHapticChannelMask);
    mHapticChannelCount = audio_channel_count_from_out_mask(mHapticChannelMask);
    mChannelCount -= mHapticChannelCount;
    mMixerChannelMask = static_cast<audio_channel_mask_t>(mMixerChannelMask & ~mHapticChannelMask);
    Vector<sp<IAfEffectChain>> effectChains = mEffectChains;
    for (size_t i = 0; i < effectChains.size(); i ++) {
        mAfThreadCallback->moveEffectChain_ll(effectChains[i]->sessionId(),
            this , this );
    }
    audio_output_flags_t flags = mOutput->flags;
    mediametrics::LogItem item(mThreadMetrics.getMetricsId());
    item.set(AMEDIAMETRICS_PROP_EVENT, AMEDIAMETRICS_PROP_EVENT_VALUE_READPARAMETERS)
        .set(AMEDIAMETRICS_PROP_ENCODING, IAfThreadBase::formatToString(mFormat).c_str())
        .set(AMEDIAMETRICS_PROP_SAMPLERATE, (int32_t)mSampleRate)
        .set(AMEDIAMETRICS_PROP_CHANNELMASK, (int32_t)mChannelMask)
        .set(AMEDIAMETRICS_PROP_CHANNELCOUNT, (int32_t)mChannelCount)
        .set(AMEDIAMETRICS_PROP_FRAMECOUNT, (int32_t)mNormalFrameCount)
        .set(AMEDIAMETRICS_PROP_FLAGS, toString(flags).c_str())
        .set(AMEDIAMETRICS_PROP_PREFIX_HAPTIC AMEDIAMETRICS_PROP_CHANNELMASK,
                (int32_t)mHapticChannelMask)
        .set(AMEDIAMETRICS_PROP_PREFIX_HAPTIC AMEDIAMETRICS_PROP_CHANNELCOUNT,
                (int32_t)mHapticChannelCount)
        .set(AMEDIAMETRICS_PROP_PREFIX_HAL AMEDIAMETRICS_PROP_ENCODING,
                IAfThreadBase::formatToString(mHALFormat).c_str())
        .set(AMEDIAMETRICS_PROP_PREFIX_HAL AMEDIAMETRICS_PROP_FRAMECOUNT,
                (int32_t)mFrameCount)
        ;
    uint32_t latencyMs;
    if (mOutput->stream->getLatency(&latencyMs) == NO_ERROR) {
        item.set(AMEDIAMETRICS_PROP_PREFIX_HAL AMEDIAMETRICS_PROP_LATENCYMS, (double)latencyMs);
    }
    item.record();
}
ThreadBase::MetadataUpdate PlaybackThread::updateMetadata_l()
{
    if (!isStreamInitialized() || !mActiveTracks.readAndClearHasChanged()) {
        return {};
    }
    StreamOutHalInterface::SourceMetadata metadata;
    auto backInserter = std::back_inserter(metadata.tracks);
    for (const sp<IAfTrack>& track : mActiveTracks) {
        track->copyMetadataTo(backInserter);
    }
    sendMetadataToBackend_l(metadata);
    MetadataUpdate change;
    change.playbackMetadataUpdate = metadata.tracks;
    return change;
}
void PlaybackThread::sendMetadataToBackend_l(
        const StreamOutHalInterface::SourceMetadata& metadata)
{
    mOutput->stream->updateSourceMetadata(metadata);
};
status_t PlaybackThread::getRenderPosition(
        uint32_t* halFrames, uint32_t* dspFrames) const
{
    if (halFrames == NULL || dspFrames == NULL) {
        return BAD_VALUE;
    }
    audio_utils::lock_guard _l(mutex());
    if (initCheck() != NO_ERROR) {
        return INVALID_OPERATION;
    }
    int64_t framesWritten = mBytesWritten / mFrameSize;
    *halFrames = framesWritten;
    if (isSuspended()) {
        size_t latencyFrames = (latency_l() * mSampleRate) / 1000;
        *dspFrames = (uint32_t)
                (framesWritten >= (int64_t)latencyFrames ? framesWritten - latencyFrames : 0);
        return NO_ERROR;
    } else {
        status_t status;
        uint32_t frames;
        status = mOutput->getRenderPosition(&frames);
        *dspFrames = (size_t)frames;
        return status;
    }
}
product_strategy_t PlaybackThread::getStrategyForSession_l(audio_session_t sessionId) const
{
    if (sessionId == AUDIO_SESSION_OUTPUT_MIX) {
        return getStrategyForStream(AUDIO_STREAM_MUSIC);
    }
    for (size_t i = 0; i < mTracks.size(); i++) {
        sp<IAfTrack> track = mTracks[i];
        if (sessionId == track->sessionId() && !track->isInvalid()) {
            return getStrategyForStream(track->streamType());
        }
    }
    return getStrategyForStream(AUDIO_STREAM_MUSIC);
}
AudioStreamOut* PlaybackThread::getOutput() const
{
    audio_utils::lock_guard _l(mutex());
    return mOutput;
}
AudioStreamOut* PlaybackThread::clearOutput()
{
    audio_utils::lock_guard _l(mutex());
    AudioStreamOut *output = mOutput;
    mOutput = NULL;
    mOutputSink.clear();
    mPipeSink.clear();
    mNormalSink.clear();
    return output;
}
sp<StreamHalInterface> PlaybackThread::stream() const
{
    if (mOutput == NULL) {
        return NULL;
    }
    return mOutput->stream;
}
uint32_t PlaybackThread::activeSleepTimeUs() const
{
    return (uint32_t)((uint32_t)((mNormalFrameCount * 1000) / mSampleRate) * 1000);
}
status_t PlaybackThread::setSyncEvent(const sp<SyncEvent>& event)
{
    if (!isValidSyncEvent(event)) {
        return BAD_VALUE;
    }
    audio_utils::lock_guard _l(mutex());
    for (size_t i = 0; i < mTracks.size(); ++i) {
        sp<IAfTrack> track = mTracks[i];
        if (event->triggerSession() == track->sessionId()) {
            (void) track->setSyncEvent(event);
            return NO_ERROR;
        }
    }
    return NAME_NOT_FOUND;
}
bool PlaybackThread::isValidSyncEvent(const sp<SyncEvent>& event) const
{
    return event->type() == AudioSystem::SYNC_EVENT_PRESENTATION_COMPLETE;
}
void PlaybackThread::threadLoop_removeTracks(
        [[maybe_unused]] const Vector<sp<IAfTrack>>& tracksToRemove)
{
#ifdef ADD_BATTERY_DATA
    for (const auto& track : tracksToRemove) {
        if (track->isExternalTrack()) {
            addBatteryData(IMediaPlayerService::kBatteryDataAudioFlingerStop);
        }
    }
#endif
}
void PlaybackThread::checkSilentMode_l()
{
    if (!mMasterMute) {
        char value[PROPERTY_VALUE_MAX];
        if (mOutDeviceTypeAddrs.empty()) {
            ALOGD("ro.audio.silent is ignored since no output device is set");
            return;
        }
        if (isSingleDeviceType(outDeviceTypes(), AUDIO_DEVICE_OUT_REMOTE_SUBMIX)) {
            ALOGD("ro.audio.silent will be ignored for threads on AUDIO_DEVICE_OUT_REMOTE_SUBMIX");
            return;
        }
        if (property_get("ro.audio.silent", value, "0") > 0) {
            char *endptr;
            unsigned long ul = strtoul(value, &endptr, 0);
            if (*endptr == '\0' && ul != 0) {
                ALOGD("Silence is golden");
                setMasterMute_l(true);
            }
        }
    }
}
ssize_t PlaybackThread::threadLoop_write()
{
    LOG_HIST_TS();
    mInWrite = true;
    ssize_t bytesWritten;
    const size_t offset = mCurrentWriteLength - mBytesRemaining;
    if (mNormalSink != 0) {
        const size_t count = mBytesRemaining / mFrameSize;
        ATRACE_BEGIN("write");
        const uint32_t screenState = mAfThreadCallback->getScreenState();
        if (screenState != mScreenState) {
            mScreenState = screenState;
            MonoPipe *pipe = (MonoPipe *)mPipeSink.get();
            if (pipe != NULL) {
                pipe->setAvgFrames((mScreenState & 1) ?
                        (pipe->maxFrames() * 7) / 8 : mNormalFrameCount * 2);
            }
        }
        ssize_t framesWritten = mNormalSink->write((char *)mSinkBuffer + offset, count);
        ATRACE_END();
        if (framesWritten > 0) {
            bytesWritten = framesWritten * mFrameSize;
#ifdef TEE_SINK
            mTee.write((char *)mSinkBuffer + offset, framesWritten);
#endif
        } else {
            bytesWritten = framesWritten;
        }
    } else {
        if (mUseAsyncWrite) {
            ALOGW_IF(mWriteAckSequence & 1, "threadLoop_write(): out of sequence write request");
            mWriteAckSequence += 2;
            mWriteAckSequence |= 1;
            ALOG_ASSERT(mCallbackThread != 0);
            mCallbackThread->setWriteBlocked(mWriteAckSequence);
        }
        ATRACE_BEGIN("write");
        bytesWritten = mOutput->write((char *)mSinkBuffer + offset, mBytesRemaining);
        ATRACE_END();
        if (mUseAsyncWrite &&
                ((bytesWritten < 0) || (bytesWritten == (ssize_t)mBytesRemaining))) {
            mWriteAckSequence &= ~1;
            ALOG_ASSERT(mCallbackThread != 0);
            mCallbackThread->setWriteBlocked(mWriteAckSequence);
        }
    }
    mNumWrites++;
    mInWrite = false;
    if (mStandby) {
        mThreadMetrics.logBeginInterval();
        mThreadSnapshot.onBegin();
        mStandby = false;
    }
    return bytesWritten;
}
void PlaybackThread::startMelComputation_l(
        const sp<audio_utils::MelProcessor>& processor)
{
    auto outputSink = static_cast<AudioStreamOutSink*>(mOutputSink.get());
    if (outputSink != nullptr) {
        outputSink->startMelComputation(processor);
    }
}
void PlaybackThread::stopMelComputation_l()
{
    auto outputSink = static_cast<AudioStreamOutSink*>(mOutputSink.get());
    if (outputSink != nullptr) {
        outputSink->stopMelComputation();
    }
}
void PlaybackThread::threadLoop_drain()
{
    bool supportsDrain = false;
    if (mOutput->stream->supportsDrain(&supportsDrain) == OK && supportsDrain) {
        ALOGV("draining %s", (mMixerStatus == MIXER_DRAIN_TRACK) ? "early" : "full");
        if (mUseAsyncWrite) {
            ALOGW_IF(mDrainSequence & 1, "threadLoop_drain(): out of sequence drain request");
            mDrainSequence |= 1;
            ALOG_ASSERT(mCallbackThread != 0);
            mCallbackThread->setDraining(mDrainSequence);
        }
        status_t result = mOutput->stream->drain(mMixerStatus == MIXER_DRAIN_TRACK);
        ALOGE_IF(result != OK, "Error when draining stream: %d", result);
    }
}
void PlaybackThread::threadLoop_exit()
{
    {
        audio_utils::lock_guard _l(mutex());
        for (size_t i = 0; i < mTracks.size(); i++) {
            sp<IAfTrack> track = mTracks[i];
            track->invalidate();
        }
        mActiveTracks.clear();
    }
}
void PlaybackThread::cacheParameters_l()
{
    mSinkBufferSize = mNormalFrameCount * mFrameSize;
    mActiveSleepTimeUs = activeSleepTimeUs();
    mIdleSleepTimeUs = idleSleepTimeUs();
    mStandbyDelayNs = getStandbyTimeInNanos();
    if (!Intersection(outDeviceTypes(), getAudioDeviceOutAllA2dpSet()).empty()) {
        if (mStandbyDelayNs < kDefaultStandbyTimeInNsecs) {
            mStandbyDelayNs = kDefaultStandbyTimeInNsecs;
        }
    }
}
bool PlaybackThread::invalidateTracks_l(audio_stream_type_t streamType)
{
    ALOGV("MixerThread::invalidateTracks() mixer %p, streamType %d, mTracks.size %zu",
            this, streamType, mTracks.size());
    bool trackMatch = false;
    size_t size = mTracks.size();
    for (size_t i = 0; i < size; i++) {
        sp<IAfTrack> t = mTracks[i];
        if (t->streamType() == streamType && t->isExternalTrack()) {
            t->invalidate();
            trackMatch = true;
        }
    }
    return trackMatch;
}
void PlaybackThread::invalidateTracks(audio_stream_type_t streamType)
{
    audio_utils::lock_guard _l(mutex());
    invalidateTracks_l(streamType);
}
void PlaybackThread::invalidateTracks(std::set<audio_port_handle_t>& portIds) {
    audio_utils::lock_guard _l(mutex());
    invalidateTracks_l(portIds);
}
bool PlaybackThread::invalidateTracks_l(std::set<audio_port_handle_t>& portIds) {
    bool trackMatch = false;
    const size_t size = mTracks.size();
    for (size_t i = 0; i < size; i++) {
        sp<IAfTrack> t = mTracks[i];
        if (t->isExternalTrack() && portIds.find(t->portId()) != portIds.end()) {
            t->invalidate();
            portIds.erase(t->portId());
            trackMatch = true;
        }
        if (portIds.empty()) {
            break;
        }
    }
    return trackMatch;
}
IAfTrack* PlaybackThread::getTrackById_l(
        audio_port_handle_t trackPortId) {
    for (size_t i = 0; i < mTracks.size(); i++) {
        if (mTracks[i]->portId() == trackPortId) {
            return mTracks[i].get();
        }
    }
    return nullptr;
}
status_t PlaybackThread::addEffectChain_l(const sp<IAfEffectChain>& chain)
{
    audio_session_t session = chain->sessionId();
    sp<EffectBufferHalInterface> halInBuffer, halOutBuffer;
    float *buffer = nullptr;
    if (mType == SPATIALIZER) {
        if (!audio_is_global_session(session)) {
            uint32_t channelMask;
            bool isSessionSpatialized =
                (hasAudioSession_l(session) & ThreadBase::SPATIALIZED_SESSION) != 0;
            if (isSessionSpatialized) {
                channelMask = mMixerChannelMask;
            } else {
                channelMask = mChannelMask;
            }
            size_t numSamples = mNormalFrameCount
                    * (audio_channel_count_from_out_mask(channelMask) + mHapticChannelCount);
            status_t result = mAfThreadCallback->getEffectsFactoryHal()->allocateBuffer(
                    numSamples * sizeof(float),
                    &halInBuffer);
            if (result != OK) return result;
            result = mAfThreadCallback->getEffectsFactoryHal()->mirrorBuffer(
                    isSessionSpatialized ? mEffectBuffer : mPostSpatializerBuffer,
                    isSessionSpatialized ? mEffectBufferSize : mPostSpatializerBufferSize,
                    &halOutBuffer);
            if (result != OK) return result;
            buffer = halInBuffer ? halInBuffer->audioBuffer()->f32 : buffer;
            ALOGV("addEffectChain_l() creating new input buffer %p session %d",
                    buffer, session);
        } else {
            status_t result = mAfThreadCallback->getEffectsFactoryHal()->mirrorBuffer(
                    mEffectBuffer, mEffectBufferSize, &halInBuffer);
            if (result != OK) return result;
            result = mAfThreadCallback->getEffectsFactoryHal()->mirrorBuffer(
                    mPostSpatializerBuffer, mPostSpatializerBufferSize, &halOutBuffer);
            if (result != OK) return result;
            if (session == AUDIO_SESSION_DEVICE) {
                halInBuffer = halOutBuffer;
            }
        }
    } else {
        status_t result = mAfThreadCallback->getEffectsFactoryHal()->mirrorBuffer(
                mEffectBufferEnabled ? mEffectBuffer : mSinkBuffer,
                mEffectBufferEnabled ? mEffectBufferSize : mSinkBufferSize,
                &halInBuffer);
        if (result != OK) return result;
        halOutBuffer = halInBuffer;
        ALOGV("addEffectChain_l() %p on thread %p for session %d", chain.get(), this, session);
        if (!audio_is_global_session(session)) {
            buffer = halInBuffer ? reinterpret_cast<float*>(halInBuffer->externalData())
                                 : buffer;
            if (mType != DIRECT) {
                size_t numSamples = mNormalFrameCount
                        * (audio_channel_count_from_out_mask(mMixerChannelMask)
                                                             + mHapticChannelCount);
                const status_t allocateStatus =
                        mAfThreadCallback->getEffectsFactoryHal()->allocateBuffer(
                        numSamples * sizeof(float),
                        &halInBuffer);
                if (allocateStatus != OK) return allocateStatus;
                buffer = halInBuffer ? halInBuffer->audioBuffer()->f32 : buffer;
                ALOGV("addEffectChain_l() creating new input buffer %p session %d",
                        buffer, session);
            }
        }
    }
    if (!audio_is_global_session(session)) {
        for (size_t i = 0; i < mTracks.size(); ++i) {
            sp<IAfTrack> track = mTracks[i];
            if (session == track->sessionId()) {
                ALOGV("addEffectChain_l() track->setMainBuffer track %p buffer %p",
                        track.get(), buffer);
                track->setMainBuffer(buffer);
                chain->incTrackCnt();
            }
        }
        for (const sp<IAfTrack>& track : mActiveTracks) {
            if (session == track->sessionId()) {
                ALOGV("addEffectChain_l() activating track %p on session %d",
                        track.get(), session);
                chain->incActiveTrackCnt();
            }
        }
    }
    chain->setThread(this);
    chain->setInBuffer(halInBuffer);
    chain->setOutBuffer(halOutBuffer);
    static_assert(AUDIO_SESSION_OUTPUT_MIX == 0 &&
            AUDIO_SESSION_OUTPUT_STAGE < AUDIO_SESSION_OUTPUT_MIX &&
            AUDIO_SESSION_DEVICE < AUDIO_SESSION_OUTPUT_STAGE,
            "audio_session_t constants misdefined");
    size_t size = mEffectChains.size();
    size_t i = 0;
    for (i = 0; i < size; i++) {
        if (mEffectChains[i]->sessionId() < session) {
            break;
        }
    }
    mEffectChains.insertAt(chain, i);
    checkSuspendOnAddEffectChain_l(chain);
    return NO_ERROR;
}
size_t PlaybackThread::removeEffectChain_l(const sp<IAfEffectChain>& chain)
{
    audio_session_t session = chain->sessionId();
    ALOGV("removeEffectChain_l() %p from thread %p for session %d", chain.get(), this, session);
    for (size_t i = 0; i < mEffectChains.size(); i++) {
        if (chain == mEffectChains[i]) {
            mEffectChains.removeAt(i);
            for (const sp<IAfTrack>& track : mActiveTracks) {
                if (session == track->sessionId()) {
                    ALOGV("removeEffectChain_l(): stopping track on chain %p for session Id: %d",
                            chain.get(), session);
                    chain->decActiveTrackCnt();
                }
            }
            for (size_t j = 0; j < mTracks.size(); ++j) {
                sp<IAfTrack> track = mTracks[j];
                if (session == track->sessionId()) {
                    track->setMainBuffer(reinterpret_cast<float*>(mSinkBuffer));
                    chain->decTrackCnt();
                }
            }
            break;
        }
    }
    return mEffectChains.size();
}
status_t PlaybackThread::attachAuxEffect(
        const sp<IAfTrack>& track, int EffectId)
{
    audio_utils::lock_guard _l(mutex());
    return attachAuxEffect_l(track, EffectId);
}
status_t PlaybackThread::attachAuxEffect_l(
        const sp<IAfTrack>& track, int EffectId)
{
    status_t status = NO_ERROR;
    if (EffectId == 0) {
        track->setAuxBuffer(0, NULL);
    } else {
        sp<IAfEffectModule> effect = getEffect_l(AUDIO_SESSION_OUTPUT_MIX, EffectId);
        if (effect != 0) {
            if ((effect->desc().flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_AUXILIARY) {
                track->setAuxBuffer(EffectId, (int32_t *)effect->inBuffer());
            } else {
                status = INVALID_OPERATION;
            }
        } else {
            status = BAD_VALUE;
        }
    }
    return status;
}
void PlaybackThread::detachAuxEffect_l(int effectId)
{
    for (size_t i = 0; i < mTracks.size(); ++i) {
        sp<IAfTrack> track = mTracks[i];
        if (track->auxEffectId() == effectId) {
            attachAuxEffect_l(track, 0);
        }
    }
}
bool PlaybackThread::threadLoop()
NO_THREAD_SAFETY_ANALYSIS
{
    aflog::setThreadWriter(mNBLogWriter.get());
    if (mType == SPATIALIZER) {
        const pid_t tid = getTid();
        if (tid == -1) {
            ALOGW("%s: Cannot update Spatializer mixer thread priority, no tid", __func__);
        } else {
            const int priorityBoost = requestSpatializerPriority(getpid(), tid);
            if (priorityBoost > 0) {
                stream()->setHalThreadPriority(priorityBoost);
            }
        }
    }
    Vector<sp<IAfTrack>> tracksToRemove;
    mStandbyTimeNs = systemTime();
    int64_t lastLoopCountWritten = -2;
    nsecs_t lastWarning = 0;
    writeFrames = 0;
    cacheParameters_l();
    mSleepTimeUs = mIdleSleepTimeUs;
    if (mType == MIXER || mType == SPATIALIZER) {
        sleepTimeShift = 0;
    }
    CpuStats cpuStats;
    const String8 myName(String8::format("thread %p type %d TID %d", this, mType, gettid()));
    acquireWakeLock();
    const char *logString = NULL;
    nsecs_t timeLoopNextNs = 0;
    checkSilentMode_l();
    audio_patch_handle_t lastDownstreamPatchHandle = AUDIO_PATCH_HANDLE_NONE;
    sendCheckOutputStageEffectsEvent();
    for (int64_t loopCount = 0; !exitPending(); ++loopCount)
    {
        mAfThreadCallback->requestLogMerge();
        cpuStats.sample(myName);
        Vector<sp<IAfEffectChain>> effectChains;
        audio_session_t activeHapticSessionId = AUDIO_SESSION_NONE;
        bool isHapticSessionSpatialized = false;
        std::vector<sp<IAfTrack>> activeTracks;
        if (isMsdDevice() && outDeviceTypes().count(AUDIO_DEVICE_OUT_BUS) != 0) {
            if (mAfThreadCallback->mutex().try_lock()) {
                std::vector<SoftwarePatch> swPatches;
                double latencyMs = 0.;
                status_t status = INVALID_OPERATION;
                audio_patch_handle_t downstreamPatchHandle = AUDIO_PATCH_HANDLE_NONE;
                if (mAfThreadCallback->getPatchPanel()->getDownstreamSoftwarePatches(
                                id(), &swPatches) == OK
                        && swPatches.size() > 0) {
                        status = swPatches[0].getLatencyMs_l(&latencyMs);
                        downstreamPatchHandle = swPatches[0].getPatchHandle();
                }
                if (downstreamPatchHandle != lastDownstreamPatchHandle) {
                    mDownstreamLatencyStatMs.reset();
                    lastDownstreamPatchHandle = downstreamPatchHandle;
                }
                if (status == OK) {
                    const double minLatency = 0., maxLatency = 5000.;
                    if (latencyMs >= minLatency && latencyMs <= maxLatency) {
                        ALOGVV("new downstream latency %lf ms", latencyMs);
                    } else {
                        ALOGD("out of range downstream latency %lf ms", latencyMs);
                        latencyMs = std::clamp(latencyMs, minLatency, maxLatency);
                    }
                    mDownstreamLatencyStatMs.add(latencyMs);
                }
                mAfThreadCallback->mutex().unlock();
            }
        } else {
            if (lastDownstreamPatchHandle != AUDIO_PATCH_HANDLE_NONE) {
                mDownstreamLatencyStatMs.reset();
                lastDownstreamPatchHandle = AUDIO_PATCH_HANDLE_NONE;
            }
        }
        if (mCheckOutputStageEffects.exchange(false)) {
            checkOutputStageEffects();
        }
        MetadataUpdate metadataUpdate;
        {
            audio_utils::unique_lock _l(mutex());
            processConfigEvents_l();
            if (mCheckOutputStageEffects.load()) {
                continue;
            }
            if (logString != NULL) {
                mNBLogWriter->logTimestamp();
                mNBLogWriter->log(logString);
                logString = NULL;
            }
            collectTimestamps_l();
            saveOutputTracks();
            if (mSignalPending) {
                mSignalPending = false;
            } else if (waitingAsyncCallback_l()) {
                if (exitPending()) {
                    break;
                }
                bool released = false;
                if (!keepWakeLock()) {
                    releaseWakeLock_l();
                    released = true;
                }
                const int64_t waitNs = computeWaitTimeNs_l();
                ALOGV("wait async completion (wait time: %lld)", (long long)waitNs);
                std::cv_status cvstatus =
                        mWaitWorkCV.wait_for(_l, std::chrono::nanoseconds(waitNs));
                if (cvstatus == std::cv_status::timeout) {
                    mSignalPending = true;
                }
                ALOGV("async completion/wake");
                if (released) {
                    acquireWakeLock_l();
                }
                mStandbyTimeNs = systemTime() + mStandbyDelayNs;
                mSleepTimeUs = 0;
                continue;
            }
            if ((mActiveTracks.isEmpty() && systemTime() > mStandbyTimeNs) ||
                                   isSuspended()) {
                if (shouldStandby_l()) {
                    threadLoop_standby();
                    if (!mStandby) {
                        LOG_AUDIO_STATE();
                        mThreadMetrics.logEndInterval();
                        mThreadSnapshot.onEnd();
                        setStandby_l();
                    }
                    sendStatistics(false );
                }
                if (mActiveTracks.isEmpty() && mConfigEvents.isEmpty()) {
                    IPCThreadState::self()->flushCommands();
                    clearOutputTracks();
                    if (exitPending()) {
                        break;
                    }
                    releaseWakeLock_l();
                    ALOGV("%s going to sleep", myName.c_str());
                    mWaitWorkCV.wait(_l);
                    ALOGV("%s waking up", myName.c_str());
                    acquireWakeLock_l();
                    mMixerStatus = MIXER_IDLE;
                    mMixerStatusIgnoringFastTracks = MIXER_IDLE;
                    mBytesWritten = 0;
                    mBytesRemaining = 0;
                    checkSilentMode_l();
                    mStandbyTimeNs = systemTime() + mStandbyDelayNs;
                    mSleepTimeUs = mIdleSleepTimeUs;
                    if (mType == MIXER || mType == SPATIALIZER) {
                        sleepTimeShift = 0;
                    }
                    continue;
                }
            }
            mMixerStatus = prepareTracks_l(&tracksToRemove);
            mActiveTracks.updatePowerState(this);
            metadataUpdate = updateMetadata_l();
            lockEffectChains_l(effectChains);
            if (mHapticChannelCount > 0) {
                for (const auto& track : mActiveTracks) {
                    sp<IAfEffectChain> effectChain = getEffectChain_l(track->sessionId());
                    if (effectChain != nullptr
                            && effectChain->containsHapticGeneratingEffect_l()) {
                        activeHapticSessionId = track->sessionId();
                        isHapticSessionSpatialized =
                                mType == SPATIALIZER && track->isSpatialized();
                        break;
                    }
                    if (activeHapticSessionId == AUDIO_SESSION_NONE
                            && track->getHapticPlaybackEnabled()) {
                        activeHapticSessionId = track->sessionId();
                        isHapticSessionSpatialized =
                                mType == SPATIALIZER && track->isSpatialized();
                    }
                }
            }
            activeTracks.insert(activeTracks.end(), mActiveTracks.begin(), mActiveTracks.end());
            setHalLatencyMode_l();
            for (const auto &track : mActiveTracks ) {
                track->updateTeePatches();
            }
            if (!mHalStarted && ((isSuspended() && (mBytesWritten != 0)) || (!mStandby
                    && (mKernelPositionOnStandby
                            != mTimestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL])))) {
                mHalStarted = true;
                mWaitHalStartCV.notify_all();
            }
        }
        if (mBytesRemaining == 0) {
            mCurrentWriteLength = 0;
            if (mMixerStatus == MIXER_TRACKS_READY) {
                threadLoop_mix();
            } else if ((mMixerStatus != MIXER_DRAIN_TRACK)
                        && (mMixerStatus != MIXER_DRAIN_ALL)) {
                threadLoop_sleepTime();
                if (mSleepTimeUs == 0) {
                    mCurrentWriteLength = mSinkBufferSize;
                    for (const auto& track : activeTracks) {
                        if (track->fillingStatus() == IAfTrack::FS_ACTIVE
                                && !track->isStopped()
                                && !track->isPaused()
                                && !track->isTerminated()) {
                            ALOGV("%s: track(%d) %s underrun due to thread sleep of %zu frames",
                                    __func__, track->id(), track->getTrackStateAsString(),
                                    mNormalFrameCount);
                            track->audioTrackServerProxy()->tallyUnderrunFrames(
                                    mNormalFrameCount);
                        }
                    }
                }
            }
            uint32_t mixerChannelCount = mEffectBufferValid ?
                        audio_channel_count_from_out_mask(mMixerChannelMask) : mChannelCount;
            if (mMixerBufferValid && (mEffectBufferValid || !mHasDataCopiedToSinkBuffer)) {
                void *buffer = mEffectBufferValid ? mEffectBuffer : mSinkBuffer;
                audio_format_t format = mEffectBufferValid ? mEffectBufferFormat : mFormat;
                if (!mEffectBufferValid) {
                    if (requireMonoBlend()) {
                        mono_blend(mMixerBuffer, mMixerBufferFormat, mChannelCount,
                                mNormalFrameCount, true );
                    }
                    if (!hasFastMixer()) {
                        mBalance.setBalance(mMasterBalance.load());
                        mBalance.process((float *)mMixerBuffer, mNormalFrameCount);
                    }
                }
                memcpy_by_audio_format(buffer, format, mMixerBuffer, mMixerBufferFormat,
                        mNormalFrameCount * (mixerChannelCount + mHapticChannelCount));
                if (!mEffectBufferValid && mHapticChannelCount > 0) {
                    adjust_channels_non_destructive(buffer, mChannelCount, buffer,
                            mChannelCount + mHapticChannelCount,
                            audio_bytes_per_sample(format),
                            audio_bytes_per_frame(mChannelCount, format) * mNormalFrameCount);
                }
            }
            mBytesRemaining = mCurrentWriteLength;
            if (isSuspended()) {
                mSleepTimeUs = suspendSleepTimeUs();
                const size_t framesRemaining = mBytesRemaining / mFrameSize;
                mBytesWritten += mBytesRemaining;
                mFramesWritten += framesRemaining;
                mSuspendedFrames += framesRemaining;
                mBytesRemaining = 0;
            }
            if (mSleepTimeUs == 0 && mType != OFFLOAD) {
                for (size_t i = 0; i < effectChains.size(); i ++) {
                    effectChains[i]->process_l();
                    if (activeHapticSessionId != AUDIO_SESSION_NONE
                            && activeHapticSessionId == effectChains[i]->sessionId()) {
                        uint32_t hapticSessionChannelCount = mEffectBufferValid ?
                                            audio_channel_count_from_out_mask(mMixerChannelMask) :
                                            mChannelCount;
                        if (mType == SPATIALIZER && !isHapticSessionSpatialized) {
                            hapticSessionChannelCount = mChannelCount;
                        }
                        const size_t audioBufferSize = mNormalFrameCount
                            * audio_bytes_per_frame(hapticSessionChannelCount,
                                                    AUDIO_FORMAT_PCM_FLOAT);
                        memcpy_by_audio_format(
                                (uint8_t*)effectChains[i]->outBuffer() + audioBufferSize,
                                AUDIO_FORMAT_PCM_FLOAT,
                                (const uint8_t*)effectChains[i]->inBuffer() + audioBufferSize,
                                AUDIO_FORMAT_PCM_FLOAT, mNormalFrameCount * mHapticChannelCount);
                    }
                }
            }
        }
        if (mType == OFFLOAD) {
            for (size_t i = 0; i < effectChains.size(); i ++) {
                effectChains[i]->process_l();
            }
        }
        if (mEffectBufferValid && !mHasDataCopiedToSinkBuffer) {
            void *effectBuffer = (mType == SPATIALIZER) ? mPostSpatializerBuffer : mEffectBuffer;
            if (requireMonoBlend()) {
                mono_blend(effectBuffer, mEffectBufferFormat, mChannelCount, mNormalFrameCount,
                           true );
            }
            if (!hasFastMixer()) {
                mBalance.setBalance(mMasterBalance.load());
                mBalance.process((float *)effectBuffer, mNormalFrameCount);
            }
            if (mType == SPATIALIZER && isHapticSessionSpatialized) {
                const size_t srcBufferSize = mNormalFrameCount *
                        audio_bytes_per_frame(audio_channel_count_from_out_mask(mMixerChannelMask),
                                              mEffectBufferFormat);
                const size_t dstBufferSize = mNormalFrameCount
                        * audio_bytes_per_frame(mChannelCount, mEffectBufferFormat);
                memcpy_by_audio_format((uint8_t*)mPostSpatializerBuffer + dstBufferSize,
                                       mEffectBufferFormat,
                                       (uint8_t*)mEffectBuffer + srcBufferSize,
                                       mEffectBufferFormat,
                                       mNormalFrameCount * mHapticChannelCount);
            }
            const size_t framesToCopy = mNormalFrameCount * (mChannelCount + mHapticChannelCount);
            if (mFormat == AUDIO_FORMAT_PCM_FLOAT &&
                    mEffectBufferFormat == AUDIO_FORMAT_PCM_FLOAT) {
                static constexpr float HAL_FLOAT_SAMPLE_LIMIT = 2.0f;
                memcpy_to_float_from_float_with_clamping(static_cast<float*>(mSinkBuffer),
                        static_cast<const float*>(effectBuffer),
                        framesToCopy, HAL_FLOAT_SAMPLE_LIMIT );
            } else {
                memcpy_by_audio_format(mSinkBuffer, mFormat,
                        effectBuffer, mEffectBufferFormat, framesToCopy);
            }
            if (mHapticChannelCount > 0) {
                adjust_channels_non_destructive(mSinkBuffer, mChannelCount, mSinkBuffer,
                        mChannelCount + mHapticChannelCount,
                        audio_bytes_per_sample(mFormat),
                        audio_bytes_per_frame(mChannelCount, mFormat) * mNormalFrameCount);
            }
        }
        unlockEffectChains(effectChains);
        if (!metadataUpdate.playbackMetadataUpdate.empty()) {
            mAfThreadCallback->getMelReporter()->updateMetadataForCsd(id(),
                    metadataUpdate.playbackMetadataUpdate);
        }
        if (!waitingAsyncCallback()) {
            if (mSleepTimeUs == 0) {
                ssize_t ret = 0;
                int64_t writePeriodNs = -1;
                if (mBytesRemaining) {
                    const int64_t lastIoBeginNs = systemTime();
                    ret = threadLoop_write();
                    const int64_t lastIoEndNs = systemTime();
                    if (ret < 0) {
                        mBytesRemaining = 0;
                    } else if (ret > 0) {
                        mBytesWritten += ret;
                        mBytesRemaining -= ret;
                        const int64_t frames = ret / mFrameSize;
                        mFramesWritten += frames;
                        writePeriodNs = lastIoEndNs - mLastIoEndNs;
                        if (audio_has_proportional_frames(mFormat)) {
                            if (mMixerStatus == MIXER_TRACKS_READY &&
                                    loopCount == lastLoopCountWritten + 1) {
                                const double jitterMs =
                                        TimestampVerifier<int64_t, int64_t>::computeJitterMs(
                                                {frames, writePeriodNs},
                                                {0, 0} , mSampleRate);
                                const double processMs =
                                       (lastIoBeginNs - mLastIoEndNs) * 1e-6;
                                audio_utils::lock_guard _l(mutex());
                                mIoJitterMs.add(jitterMs);
                                mProcessTimeMs.add(processMs);
                                if (mPipeSink.get() != nullptr) {
                                    MonoPipe* monoPipe = static_cast<MonoPipe*>(mPipeSink.get());
                                    const ssize_t
                                            availableToWrite = mPipeSink->availableToWrite();
                                    const size_t pipeFrames = monoPipe->maxFrames();
                                    const size_t
                                            remainingFrames = pipeFrames - max(availableToWrite, 0);
                                    mMonopipePipeDepthStats.add(remainingFrames);
                                }
                            }
                            const int64_t deltaWriteNs = lastIoEndNs - lastIoBeginNs;
                            if ((mType == MIXER || mType == SPATIALIZER)
                                    && deltaWriteNs > maxPeriod) {
                                mNumDelayedWrites++;
                                if ((lastIoEndNs - lastWarning) > kWarningThrottleNs) {
                                    ATRACE_NAME("underrun");
                                    ALOGW("write blocked for %lld msecs, "
                                            "%d delayed writes, thread %d",
                                            (long long)deltaWriteNs / NANOS_PER_MILLISECOND,
                                            mNumDelayedWrites, mId);
                                    lastWarning = lastIoEndNs;
                                }
                            }
                        }
                        mLastIoBeginNs = lastIoBeginNs;
                        mLastIoEndNs = lastIoEndNs;
                        lastLoopCountWritten = loopCount;
                    }
                } else if ((mMixerStatus == MIXER_DRAIN_TRACK) ||
                        (mMixerStatus == MIXER_DRAIN_ALL)) {
                    threadLoop_drain();
                }
                if ((mType == MIXER || mType == SPATIALIZER) && !mStandby) {
                    if (mThreadThrottle
                            && mMixerStatus == MIXER_TRACKS_READY
                            && writePeriodNs > 0) {
                        const int32_t deltaMs = writePeriodNs / NANOS_PER_MILLISECOND;
                        const int32_t throttleMs = (int32_t)mHalfBufferMs - deltaMs;
                        if ((signed)mHalfBufferMs >= throttleMs && throttleMs > 0) {
                            mThreadMetrics.logThrottleMs((double)throttleMs);
                            usleep(throttleMs * 1000);
                            ALOGV_IF(mThreadThrottleEndMs == mThreadThrottleTimeMs,
                                    "mixer(%p) throttle begin:"
                                    " ret(%zd) deltaMs(%d) requires sleep %d ms",
                                    this, ret, deltaMs, throttleMs);
                            mThreadThrottleTimeMs += throttleMs;
                            mLastIoEndNs = systemTime();
                        } else {
                            uint32_t diff = mThreadThrottleTimeMs - mThreadThrottleEndMs;
                            if (diff > 0) {
                                ALOGD_IF(!isSingleDeviceType(
                                                 outDeviceTypes(), audio_is_a2dp_out_device) &&
                                         !isSingleDeviceType(
                                                 outDeviceTypes(), audio_is_hearing_aid_out_device),
                                        "mixer(%p) throttle end: throttle time(%u)", this, diff);
                                mThreadThrottleEndMs = mThreadThrottleTimeMs;
                            }
                        }
                    }
                }
            } else {
                ATRACE_BEGIN("sleep");
                audio_utils::unique_lock _l(mutex());
                if (isSuspended()) {
                    timeLoopNextNs += microseconds((nsecs_t)mSleepTimeUs);
                    const nsecs_t nowNs = systemTime();
                    nsecs_t deltaNs = timeLoopNextNs - nowNs;
                    if (deltaNs < -kMaxNextBufferDelayNs) {
                        ALOGV("DelayNs: %lld, resetting timeLoopNextNs", (long long) deltaNs);
                        deltaNs = microseconds((nsecs_t)mSleepTimeUs);
                        timeLoopNextNs = nowNs + deltaNs;
                    } else if (deltaNs < 0) {
                        ALOGV("DelayNs: %lld, catching-up", (long long) deltaNs);
                        deltaNs = 0;
                    }
                    mSleepTimeUs = deltaNs / 1000;
                }
                if (!mSignalPending && mConfigEvents.isEmpty() && !exitPending()) {
                    mWaitWorkCV.wait_for(_l, std::chrono::microseconds(mSleepTimeUs));
                }
                ATRACE_END();
            }
        }
        threadLoop_removeTracks(tracksToRemove);
        tracksToRemove.clear();
        clearOutputTracks();
        effectChains.clear();
    }
    threadLoop_exit();
    if (!mStandby) {
        threadLoop_standby();
        setStandby();
    }
    releaseWakeLock();
    ALOGV("Thread %p type %d exiting", this, mType);
    return false;
}
void PlaybackThread::collectTimestamps_l()
{
    if (mStandby) {
        mTimestampVerifier.discontinuity(discontinuityForStandbyOrFlush());
        return;
    } else if (mHwPaused) {
        mTimestampVerifier.discontinuity(mTimestampVerifier.DISCONTINUITY_MODE_CONTINUOUS);
        return;
    }
    bool kernelLocationUpdate = false;
    ExtendedTimestamp timestamp;
    if (threadloop_getHalTimestamp_l(&timestamp) == OK) {
        mTimestampVerifier.add(timestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL],
                timestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL],
                mSampleRate);
        if (isTimestampCorrectionEnabled()) {
            ALOGVV("TS_BEFORE: %d %lld %lld", id(),
                    (long long)timestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL],
                    (long long)timestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL]);
            auto correctedTimestamp = mTimestampVerifier.getLastCorrectedTimestamp();
            timestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL]
                    = correctedTimestamp.mFrames;
            timestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL]
                    = correctedTimestamp.mTimeNs;
            ALOGVV("TS_AFTER: %d %lld %lld", id(),
                    (long long)timestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL],
                    (long long)timestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL]);
            if (mDownstreamLatencyStatMs.getN() > 0) {
                const int64_t newPosition =
                        timestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL]
                        - int64_t(mDownstreamLatencyStatMs.getMean() * mSampleRate * 1e-3);
                timestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL] = max(
                        newPosition,
                        (mTimestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL]
                                - mSuspendedFrames));
            }
        }
        if (mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL] >= 0) {
            mTimestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL_LASTKERNELOK] =
                    mTimestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL];
            mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL_LASTKERNELOK] =
                    mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL];
            mTimestamp.mPosition[ExtendedTimestamp::LOCATION_SERVER_LASTKERNELOK] =
                    mTimestamp.mPosition[ExtendedTimestamp::LOCATION_SERVER];
            mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_SERVER_LASTKERNELOK] =
                    mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_SERVER];
        }
        if (timestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL] >= 0) {
            kernelLocationUpdate = true;
        } else {
            ALOGVV("getTimestamp error - no valid kernel position");
        }
        mTimestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL] =
                timestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL]
                + mSuspendedFrames;
        mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL] =
                timestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL];
    } else {
        mTimestampVerifier.error();
    }
    bool serverLocationUpdate = false;
    if (mFramesWritten != mLastFramesWritten) {
        serverLocationUpdate = true;
        mLastFramesWritten = mFramesWritten;
    }
    if (kernelLocationUpdate || serverLocationUpdate) {
        if (serverLocationUpdate) {
            mTimestamp.mPosition[ExtendedTimestamp::LOCATION_SERVER] = mFramesWritten;
            mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_SERVER] = mLastIoBeginNs == -1
                    ? systemTime() : mLastIoBeginNs;
        }
        for (const sp<IAfTrack>& t : mActiveTracks) {
            if (!t->isFastTrack()) {
                t->updateTrackFrameInfo(
                        t->audioTrackServerProxy()->framesReleased(),
                        mFramesWritten,
                        mSampleRate,
                        mTimestamp);
            }
        }
    }
    if (audio_has_proportional_frames(mFormat)) {
        const double latencyMs = mTimestamp.getOutputServerLatencyMs(mSampleRate);
        if (latencyMs != 0.) {
            mLatencyMs.add(latencyMs);
        }
    }
#if 0
    if (z % 100 == 0) {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        LOGT("This is an integer %d, this is a float %f, this is my "
            "pid %p %% %s %t", 42, 3.14, "and this is a timestamp", ts);
        LOGT("A deceptive null-terminated string %\0");
    }
    ++z;
#endif
}
void PlaybackThread::removeTracks_l(const Vector<sp<IAfTrack>>& tracksToRemove)
NO_THREAD_SAFETY_ANALYSIS
{
    for (const auto& track : tracksToRemove) {
        mActiveTracks.remove(track);
        ALOGV("%s(%d): removing track on session %d", __func__, track->id(), track->sessionId());
        sp<IAfEffectChain> chain = getEffectChain_l(track->sessionId());
        if (chain != 0) {
            ALOGV("%s(%d): stopping track on chain %p for session Id: %d",
                    __func__, track->id(), chain.get(), track->sessionId());
            chain->decActiveTrackCnt();
        }
        if (track->isExternalTrack()) {
            AudioSystem::stopOutput(track->portId());
            if (track->isTerminated()) {
                AudioSystem::releaseOutput(track->portId());
            }
        }
        if (track->isTerminated()) {
            removeTrack_l(track);
        }
        if (mHapticChannelCount > 0 &&
                ((track->channelMask() & AUDIO_CHANNEL_HAPTIC_ALL) != AUDIO_CHANNEL_NONE
                        || (chain != nullptr && chain->containsHapticGeneratingEffect_l()))) {
            mutex().unlock();
            afutils::onExternalVibrationStop(track->getExternalVibration());
            mutex().lock();
            if (chain != nullptr) {
                chain->setHapticIntensity_l(track->id(), os::HapticScale::MUTE);
            }
        }
    }
}
status_t PlaybackThread::getTimestamp_l(AudioTimestamp& timestamp)
{
    if (mNormalSink != 0) {
        ExtendedTimestamp ets;
        status_t status = mNormalSink->getTimestamp(ets);
        if (status == NO_ERROR) {
            status = ets.getBestTimestamp(&timestamp);
        }
        return status;
    }
    if ((mType == OFFLOAD || mType == DIRECT) && mOutput != NULL) {
        collectTimestamps_l();
        if (mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL] <= 0) {
            return INVALID_OPERATION;
        }
        timestamp.mPosition = mTimestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL];
        const int64_t timeNs = mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL];
        timestamp.mTime.tv_sec = timeNs / NANOS_PER_SECOND;
        timestamp.mTime.tv_nsec = timeNs - (timestamp.mTime.tv_sec * NANOS_PER_SECOND);
        return NO_ERROR;
    }
    return INVALID_OPERATION;
}
status_t PlaybackThread::handleVoipVolume_l(float* volume)
{
    status_t result = NO_ERROR;
    if ((mOutput->flags & AUDIO_OUTPUT_FLAG_VOIP_RX) != 0) {
        if (*volume != mLeftVolFloat) {
            result = mOutput->stream->setVolume(*volume, *volume);
            ALOGE_IF(result != OK && result != INVALID_OPERATION,
                     "Error when setting output stream volume: %d", result);
            if (result == NO_ERROR) {
                mLeftVolFloat = *volume;
            }
        }
        if (mLeftVolFloat == *volume) {
            *volume = 1.0f;
        }
    }
    return result;
}
status_t MixerThread::createAudioPatch_l(const struct audio_patch* patch,
                                                          audio_patch_handle_t *handle)
{
    status_t status;
    if (property_get_bool("af.patch_park", false )) {
        AutoPark<FastMixer> park(mFastMixer);
        status = PlaybackThread::createAudioPatch_l(patch, handle);
    } else {
        status = PlaybackThread::createAudioPatch_l(patch, handle);
    }
    updateHalSupportedLatencyModes_l();
    return status;
}
status_t PlaybackThread::createAudioPatch_l(const struct audio_patch *patch,
                                                          audio_patch_handle_t *handle)
{
    status_t status = NO_ERROR;
    audio_devices_t type = AUDIO_DEVICE_NONE;
    AudioDeviceTypeAddrVector deviceTypeAddrs;
    for (unsigned int i = 0; i < patch->num_sinks; i++) {
        LOG_ALWAYS_FATAL_IF(popcount(patch->sinks[i].ext.device.type) > 1
                            && !mOutput->audioHwDev->supportsAudioPatches(),
                            "Enumerated device type(%#x) must not be used "
                            "as it does not support audio patches",
                            patch->sinks[i].ext.device.type);
        type = static_cast<audio_devices_t>(type | patch->sinks[i].ext.device.type);
        deviceTypeAddrs.emplace_back(patch->sinks[i].ext.device.type,
                patch->sinks[i].ext.device.address);
    }
    audio_port_handle_t sinkPortId = patch->sinks[0].id;
#ifdef ADD_BATTERY_DATA
    if (outDeviceTypes() != deviceTypes) {
        uint32_t params = 0;
        if (deviceTypes.count(AUDIO_DEVICE_OUT_SPEAKER) > 0) {
            params |= IMediaPlayerService::kBatteryDataSpeakerOn;
        }
        if (!isSingleDeviceType(deviceTypes, AUDIO_DEVICE_OUT_SPEAKER)) {
            params |= IMediaPlayerService::kBatteryDataOtherAudioDeviceOn;
        }
        if (params != 0) {
            addBatteryData(params);
        }
    }
#endif
    for (size_t i = 0; i < mEffectChains.size(); i++) {
        mEffectChains[i]->setDevices_l(deviceTypeAddrs);
    }
    bool configChanged = (mPatch.num_sinks == 0) ||
                         (mPatch.sinks[0].id != sinkPortId);
    mPatch = *patch;
    mOutDeviceTypeAddrs = deviceTypeAddrs;
    checkSilentMode_l();
    if (mOutput->audioHwDev->supportsAudioPatches()) {
        sp<DeviceHalInterface> hwDevice = mOutput->audioHwDev->hwDevice();
        status = hwDevice->createAudioPatch(patch->num_sources,
                                            patch->sources,
                                            patch->num_sinks,
                                            patch->sinks,
                                            handle);
    } else {
        status = mOutput->stream->legacyCreateAudioPatch(patch->sinks[0], std::nullopt, type);
        *handle = AUDIO_PATCH_HANDLE_NONE;
    }
    const std::string patchSinksAsString = patchSinksToString(patch);
    mThreadMetrics.logEndInterval();
    mThreadMetrics.logCreatePatch( {}, patchSinksAsString);
    mThreadMetrics.logBeginInterval();
    for (const auto &track : mActiveTracks) {
        track->logEndInterval();
        track->logBeginInterval(patchSinksAsString);
    }
    if (configChanged) {
        sendIoConfigEvent_l(AUDIO_OUTPUT_CONFIG_CHANGED);
    }
    mActiveTracks.setHasChanged();
    return status;
}
status_t MixerThread::releaseAudioPatch_l(const audio_patch_handle_t handle)
{
    status_t status;
    if (property_get_bool("af.patch_park", false )) {
        AutoPark<FastMixer> park(mFastMixer);
        status = PlaybackThread::releaseAudioPatch_l(handle);
    } else {
        status = PlaybackThread::releaseAudioPatch_l(handle);
    }
    return status;
}
status_t PlaybackThread::releaseAudioPatch_l(const audio_patch_handle_t handle)
{
    status_t status = NO_ERROR;
    mPatch = audio_patch{};
    mOutDeviceTypeAddrs.clear();
    if (mOutput->audioHwDev->supportsAudioPatches()) {
        sp<DeviceHalInterface> hwDevice = mOutput->audioHwDev->hwDevice();
        status = hwDevice->releaseAudioPatch(handle);
    } else {
        status = mOutput->stream->legacyReleaseAudioPatch();
    }
    mActiveTracks.setHasChanged();
    return status;
}
void PlaybackThread::addPatchTrack(const sp<IAfPatchTrack>& track)
{
    audio_utils::lock_guard _l(mutex());
    mTracks.add(track);
}
void PlaybackThread::deletePatchTrack(const sp<IAfPatchTrack>& track)
{
    audio_utils::lock_guard _l(mutex());
    destroyTrack_l(track);
}
void PlaybackThread::toAudioPortConfig(struct audio_port_config* config)
{
    ThreadBase::toAudioPortConfig(config);
    config->role = AUDIO_PORT_ROLE_SOURCE;
    config->ext.mix.hw_module = mOutput->audioHwDev->handle();
    config->ext.mix.usecase.stream = AUDIO_STREAM_DEFAULT;
    if (mOutput && mOutput->flags != AUDIO_OUTPUT_FLAG_NONE) {
        config->config_mask |= AUDIO_PORT_CONFIG_FLAGS;
        config->flags.output = mOutput->flags;
    }
}
sp<IAfPlaybackThread> IAfPlaybackThread::createMixerThread(
        const sp<IAfThreadCallback>& afThreadCallback, AudioStreamOut* output,
        audio_io_handle_t id, bool systemReady, type_t type, audio_config_base_t* mixerConfig) {
    return sp<MixerThread>::make(afThreadCallback, output, id, systemReady, type, mixerConfig);
}
MixerThread::MixerThread(const sp<IAfThreadCallback>& afThreadCallback, AudioStreamOut* output,
        audio_io_handle_t id, bool systemReady, type_t type, audio_config_base_t *mixerConfig)
    : PlaybackThread(afThreadCallback, output, id, type, systemReady, mixerConfig),
        mBluetoothLatencyModesEnabled(false),
        mFastMixerFutex(0),
        mMasterMono(false)
{
    setMasterBalance(afThreadCallback->getMasterBalance_l());
    ALOGV("MixerThread() id=%d type=%d", id, type);
    ALOGV("mSampleRate=%u, mChannelMask=%#x, mChannelCount=%u, mFormat=%#x, mFrameSize=%zu, "
            "mFrameCount=%zu, mNormalFrameCount=%zu",
            mSampleRate, mChannelMask, mChannelCount, mFormat, mFrameSize, mFrameCount,
            mNormalFrameCount);
    mAudioMixer = new AudioMixer(mNormalFrameCount, mSampleRate);
    if (type == DUPLICATING) {
        return;
    }
    mOutputSink = new AudioStreamOutSink(output->stream);
    size_t numCounterOffers = 0;
    const NBAIO_Format offers[1] = {Format_from_SR_C(
            mSampleRate, mChannelCount + mHapticChannelCount, mFormat)};
#if !LOG_NDEBUG
    ssize_t index =
#else
    (void)
#endif
            mOutputSink->negotiate(offers, 1, NULL, numCounterOffers);
    ALOG_ASSERT(index == 0);
    bool initFastMixer;
    if (mType == SPATIALIZER || mType == BIT_PERFECT) {
        initFastMixer = false;
    } else {
        switch (kUseFastMixer) {
        case FastMixer_Never:
            initFastMixer = false;
            break;
        case FastMixer_Always:
            initFastMixer = true;
            break;
        case FastMixer_Static:
        case FastMixer_Dynamic:
            initFastMixer = mFrameCount < mNormalFrameCount;
            break;
        }
        ALOGW_IF(initFastMixer == false && mFrameCount < mNormalFrameCount,
                "FastMixer is preferred for this sink as frameCount %zu is less than threshold %zu",
                mFrameCount, mNormalFrameCount);
    }
    if (initFastMixer) {
        audio_format_t fastMixerFormat;
        if (mMixerBufferEnabled && mEffectBufferEnabled) {
            fastMixerFormat = AUDIO_FORMAT_PCM_FLOAT;
        } else {
            fastMixerFormat = AUDIO_FORMAT_PCM_16_BIT;
        }
        if (mFormat != fastMixerFormat) {
            mFormat = fastMixerFormat;
            free(mSinkBuffer);
            mFrameSize = audio_bytes_per_frame(mChannelCount + mHapticChannelCount, mFormat);
            const size_t sinkBufferSize = mNormalFrameCount * mFrameSize;
            (void)posix_memalign(&mSinkBuffer, 32, sinkBufferSize);
        }
        NBAIO_Format format = mOutputSink->format();
        ALOGV("format changed from %#x to %#x", format.mFormat, fastMixerFormat);
        format.mFormat = fastMixerFormat;
        format.mFrameSize = audio_bytes_per_sample(format.mFormat) * format.mChannelCount;
        MonoPipe *monoPipe = new MonoPipe(mNormalFrameCount * 4, format, true );
        const NBAIO_Format offersFast[1] = {format};
        size_t numCounterOffersFast = 0;
#if !LOG_NDEBUG
        index =
#else
        (void)
#endif
                monoPipe->negotiate(offersFast, std::size(offersFast),
                        nullptr , numCounterOffersFast);
        ALOG_ASSERT(index == 0);
        monoPipe->setAvgFrames((mScreenState & 1) ?
                (monoPipe->maxFrames() * 7) / 8 : mNormalFrameCount * 2);
        mPipeSink = monoPipe;
        mFastMixer = new FastMixer(mId);
        FastMixerStateQueue *sq = mFastMixer->sq();
#ifdef STATE_QUEUE_DUMP
        sq->setObserverDump(&mStateQueueObserverDump);
        sq->setMutatorDump(&mStateQueueMutatorDump);
#endif
        FastMixerState *state = sq->begin();
        FastTrack *fastTrack = &state->mFastTracks[0];
        fastTrack->mBufferProvider = new SourceAudioBufferProvider(new MonoPipeReader(monoPipe));
        fastTrack->mVolumeProvider = NULL;
        fastTrack->mChannelMask = static_cast<audio_channel_mask_t>(
                mChannelMask | mHapticChannelMask);
        fastTrack->mFormat = mFormat;
        fastTrack->mHapticPlaybackEnabled = mHapticChannelMask != AUDIO_CHANNEL_NONE;
        fastTrack->mHapticIntensity = os::HapticScale::NONE;
        fastTrack->mHapticMaxAmplitude = NAN;
        fastTrack->mGeneration++;
        state->mFastTracksGen++;
        state->mTrackMask = 1;
        state->mOutputSink = mOutputSink.get();
        state->mOutputSinkGen++;
        state->mFrameCount = mFrameCount;
        state->mSinkChannelMask = mHapticChannelMask == AUDIO_CHANNEL_NONE
                ? AUDIO_CHANNEL_NONE
                : static_cast<audio_channel_mask_t>(mChannelMask | mHapticChannelMask);
        state->mCommand = FastMixerState::COLD_IDLE;
        state->mColdFutexAddr = &mFastMixerFutex;
        state->mColdGen++;
        state->mDumpState = &mFastMixerDumpState;
        mFastMixerNBLogWriter = afThreadCallback->newWriter_l(kFastMixerLogSize, "FastMixer");
        state->mNBLogWriter = mFastMixerNBLogWriter.get();
        sq->end();
        sq->push(FastMixerStateQueue::BLOCK_UNTIL_PUSHED);
        NBLog::thread_info_t info;
        info.id = mId;
        info.type = NBLog::FASTMIXER;
        mFastMixerNBLogWriter->log<NBLog::EVENT_THREAD_INFO>(info);
        mFastMixer->run("FastMixer", PRIORITY_URGENT_AUDIO);
        pid_t tid = mFastMixer->getTid();
        sendPrioConfigEvent(getpid(), tid, kPriorityFastMixer, false );
        stream()->setHalThreadPriority(kPriorityFastMixer);
#ifdef AUDIO_WATCHDOG
        mAudioWatchdog = new AudioWatchdog();
        mAudioWatchdog->setDump(&mAudioWatchdogDump);
        mAudioWatchdog->run("AudioWatchdog", PRIORITY_URGENT_AUDIO);
        tid = mAudioWatchdog->getTid();
        sendPrioConfigEvent(getpid(), tid, kPriorityFastMixer, false );
#endif
    } else {
#ifdef TEE_SINK
        mTee.set(mOutputSink->format(), NBAIO_Tee::TEE_FLAG_OUTPUT_THREAD);
        mTee.setId(std::string("_") + std::to_string(mId) + "_M");
#endif
    }
    switch (kUseFastMixer) {
    case FastMixer_Never:
    case FastMixer_Dynamic:
        mNormalSink = mOutputSink;
        break;
    case FastMixer_Always:
        mNormalSink = mPipeSink;
        break;
    case FastMixer_Static:
        mNormalSink = initFastMixer ? mPipeSink : mOutputSink;
        break;
    }
}
MixerThread::~MixerThread()
{
    if (mFastMixer != 0) {
        FastMixerStateQueue *sq = mFastMixer->sq();
        FastMixerState *state = sq->begin();
        if (state->mCommand == FastMixerState::COLD_IDLE) {
            int32_t old = android_atomic_inc(&mFastMixerFutex);
            if (old == -1) {
                (void) syscall(__NR_futex, &mFastMixerFutex, FUTEX_WAKE_PRIVATE, 1);
            }
        }
        state->mCommand = FastMixerState::EXIT;
        sq->end();
        sq->push(FastMixerStateQueue::BLOCK_UNTIL_PUSHED);
        mFastMixer->join();
        state = sq->begin();
        ALOG_ASSERT(state->mTrackMask == 1);
        FastTrack *fastTrack = &state->mFastTracks[0];
        ALOG_ASSERT(fastTrack->mBufferProvider != NULL);
        delete fastTrack->mBufferProvider;
        sq->end(false );
        mFastMixer.clear();
#ifdef AUDIO_WATCHDOG
        if (mAudioWatchdog != 0) {
            mAudioWatchdog->requestExit();
            mAudioWatchdog->requestExitAndWait();
            mAudioWatchdog.clear();
        }
#endif
    }
    mAfThreadCallback->unregisterWriter(mFastMixerNBLogWriter);
    delete mAudioMixer;
}
void MixerThread::onFirstRef() {
    PlaybackThread::onFirstRef();
    audio_utils::lock_guard _l(mutex());
    if (mOutput != nullptr && mOutput->stream != nullptr) {
        status_t status = mOutput->stream->setLatencyModeCallback(this);
        if (status != INVALID_OPERATION) {
            updateHalSupportedLatencyModes_l();
        }
        mBluetoothLatencyModesEnabled.store(
                mOutput->audioHwDev->supportsBluetoothVariableLatency());
    }
}
uint32_t MixerThread::correctLatency_l(uint32_t latency) const
{
    if (mFastMixer != 0) {
        MonoPipe *pipe = (MonoPipe *)mPipeSink.get();
        latency += (pipe->getAvgFrames() * 1000) / mSampleRate;
    }
    return latency;
}
ssize_t MixerThread::threadLoop_write()
{
    if (mFastMixer != 0) {
        FastMixerStateQueue *sq = mFastMixer->sq();
        FastMixerState *state = sq->begin();
        if (state->mCommand != FastMixerState::MIX_WRITE &&
                (kUseFastMixer != FastMixer_Dynamic || state->mTrackMask > 1)) {
            if (state->mCommand == FastMixerState::COLD_IDLE) {
                ATRACE_BEGIN("write");
                mOutput->write((char *)mSinkBuffer, 0);
                ATRACE_END();
                int32_t old = android_atomic_inc(&mFastMixerFutex);
                if (old == -1) {
                    (void) syscall(__NR_futex, &mFastMixerFutex, FUTEX_WAKE_PRIVATE, 1);
                }
#ifdef AUDIO_WATCHDOG
                if (mAudioWatchdog != 0) {
                    mAudioWatchdog->resume();
                }
#endif
            }
            state->mCommand = FastMixerState::MIX_WRITE;
#ifdef FAST_THREAD_STATISTICS
            mFastMixerDumpState.increaseSamplingN(mAfThreadCallback->isLowRamDevice() ?
                FastThreadDumpState::kSamplingNforLowRamDevice : FastThreadDumpState::kSamplingN);
#endif
            sq->end();
            sq->push(FastMixerStateQueue::BLOCK_UNTIL_PUSHED);
            if (kUseFastMixer == FastMixer_Dynamic) {
                mNormalSink = mPipeSink;
            }
        } else {
            sq->end(false );
        }
    }
    return PlaybackThread::threadLoop_write();
}
void MixerThread::threadLoop_standby()
{
    if (mFastMixer != 0) {
        FastMixerStateQueue *sq = mFastMixer->sq();
        FastMixerState *state = sq->begin();
        if (!(state->mCommand & FastMixerState::IDLE)) {
            MonoPipe *monoPipe = (MonoPipe *)mPipeSink.get();
            const long long pipeFrames = monoPipe->maxFrames() - monoPipe->availableToWrite();
            mLocalLog.log("threadLoop_standby: framesWritten:%lld  suspendedFrames:%lld  "
                    "monoPipeWritten:%lld  monoPipeLeft:%lld",
                    (long long)mFramesWritten, (long long)mSuspendedFrames,
                    (long long)mPipeSink->framesWritten(), pipeFrames);
            mLocalLog.log("threadLoop_standby: %s", mTimestamp.toString().c_str());
            state->mCommand = FastMixerState::COLD_IDLE;
            state->mColdFutexAddr = &mFastMixerFutex;
            state->mColdGen++;
            mFastMixerFutex = 0;
            sq->end();
            sq->push(FastMixerStateQueue::BLOCK_UNTIL_ACKED);
            if (kUseFastMixer == FastMixer_Dynamic) {
                mNormalSink = mOutputSink;
            }
#ifdef AUDIO_WATCHDOG
            if (mAudioWatchdog != 0) {
                mAudioWatchdog->pause();
            }
#endif
        } else {
            sq->end(false );
        }
    }
    PlaybackThread::threadLoop_standby();
}
bool PlaybackThread::waitingAsyncCallback_l()
{
    return false;
}
bool PlaybackThread::shouldStandby_l()
{
    return !mStandby;
}
bool PlaybackThread::waitingAsyncCallback()
{
    audio_utils::lock_guard _l(mutex());
    return waitingAsyncCallback_l();
}
void PlaybackThread::threadLoop_standby()
{
    ALOGV("Audio hardware entering standby, mixer %p, suspend count %d", this, mSuspended);
    mOutput->standby();
    if (mUseAsyncWrite != 0) {
        mWriteAckSequence = (mWriteAckSequence + 2) & ~1;
        mDrainSequence = (mDrainSequence + 2) & ~1;
        ALOG_ASSERT(mCallbackThread != 0);
        mCallbackThread->setWriteBlocked(mWriteAckSequence);
        mCallbackThread->setDraining(mDrainSequence);
    }
    mHwPaused = false;
    setHalLatencyMode_l();
}
void PlaybackThread::onAddNewTrack_l()
{
    ALOGV("signal playback thread");
    broadcast_l();
}
void PlaybackThread::onAsyncError()
{
    for (int i = AUDIO_STREAM_SYSTEM; i < (int)AUDIO_STREAM_CNT; i++) {
        invalidateTracks((audio_stream_type_t)i);
    }
}
void MixerThread::threadLoop_mix()
{
    mAudioMixer->process();
    mCurrentWriteLength = mSinkBufferSize;
    if ((mSleepTimeUs == 0) && (sleepTimeShift > 0)) {
        sleepTimeShift--;
    }
    mSleepTimeUs = 0;
    mStandbyTimeNs = systemTime() + mStandbyDelayNs;
}
void MixerThread::threadLoop_sleepTime()
{
    if (mSleepTimeUs == 0) {
        if (mMixerStatus == MIXER_TRACKS_ENABLED) {
            if (mPipeSink.get() != nullptr && mPipeSink == mNormalSink) {
                MonoPipe *monoPipe = static_cast<MonoPipe *>(mPipeSink.get());
                const ssize_t availableToWrite = mPipeSink->availableToWrite();
                const size_t pipeFrames = monoPipe->maxFrames();
                const size_t framesLeft = pipeFrames - max(availableToWrite, 0);
                const size_t framesDelay = std::min(
                        mNormalFrameCount, max(framesLeft / 2, mFrameCount));
                ALOGV("pipeFrames:%zu framesLeft:%zu framesDelay:%zu",
                        pipeFrames, framesLeft, framesDelay);
                mSleepTimeUs = framesDelay * MICROS_PER_SECOND / mSampleRate;
            } else {
                mSleepTimeUs = mActiveSleepTimeUs >> sleepTimeShift;
                if (mSleepTimeUs < kMinThreadSleepTimeUs) {
                    mSleepTimeUs = kMinThreadSleepTimeUs;
                }
                if (sleepTimeShift < kMaxThreadSleepTimeShift) {
                    sleepTimeShift++;
                }
            }
        } else {
            mSleepTimeUs = mIdleSleepTimeUs;
        }
    } else if (mBytesWritten != 0 || (mMixerStatus == MIXER_TRACKS_ENABLED)) {
        if (mMixerBufferValid) {
            memset(mMixerBuffer, 0, mMixerBufferSize);
            if (mType == SPATIALIZER) {
                memset(mSinkBuffer, 0, mSinkBufferSize);
            }
        } else {
            memset(mSinkBuffer, 0, mSinkBufferSize);
        }
        mSleepTimeUs = 0;
        ALOGV_IF(mBytesWritten == 0 && (mMixerStatus == MIXER_TRACKS_ENABLED),
                "anticipated start");
    }
}
PlaybackThread::mixer_state MixerThread::prepareTracks_l(
        Vector<sp<IAfTrack>>* tracksToRemove)
{
    (void)mTracks.processDeletedTrackIds([this](int trackId) {
        if (mAudioMixer->exists(trackId)) {
            mAudioMixer->destroy(trackId);
        }
    });
    mTracks.clearDeletedTrackIds();
    mixer_state mixerStatus = MIXER_IDLE;
    size_t count = mActiveTracks.size();
    size_t mixedTracks = 0;
    size_t tracksWithEffect = 0;
    size_t fastTracks = 0;
    uint32_t resetMask = 0;
    float masterVolume = mMasterVolume;
    bool masterMute = mMasterMute;
    if (masterMute) {
        masterVolume = 0;
    }
    sp<IAfEffectChain> chain = getEffectChain_l(AUDIO_SESSION_OUTPUT_MIX);
    if (chain != 0) {
        uint32_t v = (uint32_t)(masterVolume * (1 << 24));
        chain->setVolume_l(&v, &v);
        masterVolume = (float)((v + (1 << 23)) >> 24);
        chain.clear();
    }
    FastMixerStateQueue *sq = NULL;
    FastMixerState *state = NULL;
    bool didModify = false;
    FastMixerStateQueue::block_t block = FastMixerStateQueue::BLOCK_UNTIL_PUSHED;
    bool coldIdle = false;
    if (mFastMixer != 0) {
        sq = mFastMixer->sq();
        state = sq->begin();
        coldIdle = state->mCommand == FastMixerState::COLD_IDLE;
    }
    mMixerBufferValid = false;
    mEffectBufferValid = false;
    class DeferredOperations {
    public:
        DeferredOperations(mixer_state *mixerStatus, ThreadMetrics *threadMetrics)
            : mMixerStatus(mixerStatus)
            , mThreadMetrics(threadMetrics) {}
        ~DeferredOperations() {
            size_t maxUnderrunFrames = 0;
            if (*mMixerStatus == MIXER_TRACKS_READY && mUnderrunFrames.size() > 0) {
                for (const auto &underrun : mUnderrunFrames) {
                    underrun.first->tallyUnderrunFrames(underrun.second);
                    maxUnderrunFrames = max(underrun.second, maxUnderrunFrames);
                }
            }
            mThreadMetrics->logUnderrunFrames(maxUnderrunFrames);
        }
        void tallyUnderrunFrames(const sp<IAfTrack>& track, size_t underrunFrames) {
            mUnderrunFrames.emplace_back(track, underrunFrames);
        }
    private:
        const mixer_state * const mMixerStatus;
        ThreadMetrics * const mThreadMetrics;
        std::vector<std::pair<sp<IAfTrack>, size_t>> mUnderrunFrames;
    } deferredOperations(&mixerStatus, &mThreadMetrics);
    bool noFastHapticTrack = true;
    for (size_t i=0 ; i<count ; i++) {
        const sp<IAfTrack> t = mActiveTracks[i];
        IAfTrack* const track = t.get();
        if (track->isFastTrack()) {
            LOG_ALWAYS_FATAL_IF(mFastMixer.get() == nullptr,
                    "%s(%d): FastTrack(%d) present without FastMixer",
                     __func__, id(), track->id());
            if (track->getHapticPlaybackEnabled()) {
                noFastHapticTrack = false;
            }
            int j = track->fastIndex();
            ALOG_ASSERT(0 < j && j < (int)FastMixerState::sMaxFastTracks);
            ALOG_ASSERT(!(mFastTrackAvailMask & (1 << j)));
            FastTrack *fastTrack = &state->mFastTracks[j];
            FastTrackDump *ftDump = &mFastMixerDumpState.mTracks[j];
            FastTrackUnderruns underruns = ftDump->mUnderruns;
            uint32_t recentFull = (underruns.mBitFields.mFull -
                    track->fastTrackUnderruns().mBitFields.mFull) & UNDERRUN_MASK;
            uint32_t recentPartial = (underruns.mBitFields.mPartial -
                    track->fastTrackUnderruns().mBitFields.mPartial) & UNDERRUN_MASK;
            uint32_t recentEmpty = (underruns.mBitFields.mEmpty -
                    track->fastTrackUnderruns().mBitFields.mEmpty) & UNDERRUN_MASK;
            uint32_t recentUnderruns = recentPartial + recentEmpty;
            track->fastTrackUnderruns() = underruns;
            size_t underrunFrames = 0;
            if (!(track->isStopping() || track->isPausing() || track->isStopped()) &&
                    recentUnderruns > 0) {
                underrunFrames = recentUnderruns * mFrameCount;
            }
            track->audioTrackServerProxy()->tallyUnderrunFrames(underrunFrames);
            bool isActive = true;
            switch (track->state()) {
            case IAfTrackBase::STOPPING_1:
                if (recentUnderruns > 0 || track->isTerminated()) {
                    track->setState(IAfTrackBase::STOPPING_2);
                }
                break;
            case IAfTrackBase::PAUSING:
                track->setPaused();
                break;
            case IAfTrackBase::RESUMING:
                track->setState(IAfTrackBase::ACTIVE);
                break;
            case IAfTrackBase::ACTIVE:
                if (recentFull > 0 || recentPartial > 0) {
                    track->retryCount() = kMaxTrackRetries;
                }
                if (recentUnderruns == 0) {
                    break;
                }
                if (track->sharedBuffer() == 0) {
                    if (recentEmpty == 0) {
                        break;
                    }
                    if (--(track->retryCount()) > 0) {
                        break;
                    }
                    track->disable();
                    isActive = false;
                    break;
                }
                FALLTHROUGH_INTENDED;
            case IAfTrackBase::STOPPING_2:
            case IAfTrackBase::PAUSED:
            case IAfTrackBase::STOPPED:
            case IAfTrackBase::FLUSHED:
                {
                    uint32_t latency = 0;
                    status_t result = mOutput->stream->getLatency(&latency);
                    ALOGE_IF(result != OK,
                            "Error when retrieving output stream latency: %d", result);
                    size_t audioHALFrames = (latency * mSampleRate) / 1000;
                    int64_t framesWritten = mBytesWritten / mFrameSize;
                    if (!(mStandby || track->presentationComplete(framesWritten, audioHALFrames))) {
                        break;
                    }
                }
                if (track->isStopping_2()) {
                    track->setState(IAfTrackBase::STOPPED);
                }
                if (track->isStopped()) {
                    resetMask |= 1 << i;
                }
                isActive = false;
                break;
            case IAfTrackBase::IDLE:
            default:
                LOG_ALWAYS_FATAL("unexpected track state %d", (int)track->state());
            }
            if (isActive) {
                if (!(state->mTrackMask & (1 << j))) {
                    ExtendedAudioBufferProvider *eabp = track->asExtendedAudioBufferProvider();
                    VolumeProvider *vp = track->asVolumeProvider();
                    fastTrack->mBufferProvider = eabp;
                    fastTrack->mVolumeProvider = vp;
                    fastTrack->mChannelMask = track->channelMask();
                    fastTrack->mFormat = track->format();
                    fastTrack->mHapticPlaybackEnabled = track->getHapticPlaybackEnabled();
                    fastTrack->mHapticIntensity = track->getHapticIntensity();
                    fastTrack->mHapticMaxAmplitude = track->getHapticMaxAmplitude();
                    fastTrack->mGeneration++;
                    state->mTrackMask |= 1 << j;
                    didModify = true;
                }
                sp<AudioTrackServerProxy> proxy = track->audioTrackServerProxy();
                float volume;
                if (track->isPlaybackRestricted() || mStreamTypes[track->streamType()].mute) {
                    volume = 0.f;
                } else {
                    volume = masterVolume * mStreamTypes[track->streamType()].volume;
                }
                handleVoipVolume_l(&volume);
                const float vh = track->getVolumeHandler()->getVolume(
                    proxy->framesReleased()).first;
                volume *= vh;
                track->setCachedVolume(volume);
                gain_minifloat_packed_t vlr = proxy->getVolumeLR();
                float vlf = float_from_gain(gain_minifloat_unpack_left(vlr));
                float vrf = float_from_gain(gain_minifloat_unpack_right(vlr));
                track->processMuteEvent_l(mAfThreadCallback->getOrCreateAudioManager(),
                                  {masterVolume == 0.f,
                                   mStreamTypes[track->streamType()].volume == 0.f,
                                   mStreamTypes[track->streamType()].mute,
                                   track->isPlaybackRestricted(),
                                   vlf == 0.f && vrf == 0.f,
                                   vh == 0.f});
                vlf *= volume;
                vrf *= volume;
                track->setFinalVolume(vlf, vrf);
                ++fastTracks;
            } else {
                if (state->mTrackMask & (1 << j)) {
                    fastTrack->mBufferProvider = NULL;
                    fastTrack->mGeneration++;
                    state->mTrackMask &= ~(1 << j);
                    didModify = true;
                    block = FastMixerStateQueue::BLOCK_UNTIL_ACKED;
                } else {
                    ALOGW("fast track %d should have been active; "
                            "mState=%d, mTrackMask=%#x, recentUnderruns=%u, isShared=%d",
                            j, (int)track->state(), state->mTrackMask, recentUnderruns,
                            track->sharedBuffer() != 0);
                }
                tracksToRemove->add(track);
                track->fastTrackUnderruns().mBitFields.mMostRecent = UNDERRUN_FULL;
            }
            if (fastTrack->mHapticPlaybackEnabled != track->getHapticPlaybackEnabled()) {
                fastTrack->mHapticPlaybackEnabled = track->getHapticPlaybackEnabled();
                didModify = true;
            }
            continue;
        }
        {
        audio_track_cblk_t* cblk = track->cblk();
        const int trackId = track->id();
        if (!mAudioMixer->exists(trackId)) {
            status_t status = mAudioMixer->create(
                    trackId,
                    track->channelMask(),
                    track->format(),
                    track->sessionId());
            if (status != OK) {
                ALOGW("%s(): AudioMixer cannot create track(%d)"
                        " mask %#x, format %#x, sessionId %d",
                        __func__, trackId,
                        track->channelMask(), track->format(), track->sessionId());
                tracksToRemove->add(track);
                track->invalidate();
                continue;
            }
        }
        size_t desiredFrames;
        const uint32_t sampleRate = track->audioTrackServerProxy()->getSampleRate();
        const AudioPlaybackRate playbackRate = track->audioTrackServerProxy()->getPlaybackRate();
        desiredFrames = sourceFramesNeededWithTimestretch(
                sampleRate, mNormalFrameCount, mSampleRate, playbackRate.mSpeed);
        desiredFrames += mAudioMixer->getUnreleasedFrames(trackId);
        uint32_t minFrames = 1;
        if ((track->sharedBuffer() == 0) && !track->isStopped() && !track->isPausing() &&
                (mMixerStatusIgnoringFastTracks == MIXER_TRACKS_READY)) {
            minFrames = desiredFrames;
        }
        size_t framesReady = track->framesReady();
        if (ATRACE_ENABLED()) {
            std::string traceName("nRdy");
            traceName += std::to_string(trackId);
            ATRACE_INT(traceName.c_str(), framesReady);
        }
        if ((framesReady >= minFrames) && track->isReady() &&
                !track->isPaused() && !track->isTerminated())
        {
            ALOGVV("track(%d) s=%08x [OK] on thread %p", trackId, cblk->mServer, this);
            mixedTracks++;
            chain.clear();
            if (track->mainBuffer() != mSinkBuffer &&
                    track->mainBuffer() != mMixerBuffer) {
                if (mEffectBufferEnabled) {
                    mEffectBufferValid = true;
                }
                chain = getEffectChain_l(track->sessionId());
                if (chain != 0) {
                    tracksWithEffect++;
                } else {
                    ALOGW("prepareTracks_l(): track(%d) attached to effect but no chain found on "
                            "session %d",
                            trackId, track->sessionId());
                }
            }
            int param = AudioMixer::VOLUME;
            if (track->fillingStatus() == IAfTrack::FS_FILLED) {
                track->fillingStatus() = IAfTrack::FS_ACTIVE;
                if (track->state() == IAfTrackBase::RESUMING) {
                    track->setState(IAfTrackBase::ACTIVE);
                    if (cblk->mServer != 0) {
                        param = AudioMixer::RAMP_VOLUME;
                    }
                }
                mAudioMixer->setParameter(trackId, AudioMixer::RESAMPLE, AudioMixer::RESET, NULL);
                mLeftVolFloat = -1.0;
            } else if (cblk->mServer != 0) {
                param = AudioMixer::RAMP_VOLUME;
            }
            uint32_t vl, vr;
            float vlf, vrf, vaf;
            float v = masterVolume * mStreamTypes[track->streamType()].volume;
            const sp<AudioTrackServerProxy> proxy = track->audioTrackServerProxy();
            const float vh = track->getVolumeHandler()->getVolume(
                    track->audioTrackServerProxy()->framesReleased()).first;
            if (mStreamTypes[track->streamType()].mute || track->isPlaybackRestricted()) {
                v = 0;
            }
            handleVoipVolume_l(&v);
            if (track->isPausing()) {
                vl = vr = 0;
                vlf = vrf = vaf = 0.;
                track->setPaused();
            } else {
                gain_minifloat_packed_t vlr = proxy->getVolumeLR();
                vlf = float_from_gain(gain_minifloat_unpack_left(vlr));
                vrf = float_from_gain(gain_minifloat_unpack_right(vlr));
                if (vlf > GAIN_FLOAT_UNITY) {
                    ALOGV("Track left volume out of range: %.3g", vlf);
                    vlf = GAIN_FLOAT_UNITY;
                }
                if (vrf > GAIN_FLOAT_UNITY) {
                    ALOGV("Track right volume out of range: %.3g", vrf);
                    vrf = GAIN_FLOAT_UNITY;
                }
                track->processMuteEvent_l(mAfThreadCallback->getOrCreateAudioManager(),
                                  {masterVolume == 0.f,
                                   mStreamTypes[track->streamType()].volume == 0.f,
                                   mStreamTypes[track->streamType()].mute,
                                   track->isPlaybackRestricted(),
                                   vlf == 0.f && vrf == 0.f,
                                   vh == 0.f});
                vlf *= v * vh;
                vrf *= v * vh;
                const float scaleto8_24 = MAX_GAIN_INT * MAX_GAIN_INT;
                vl = (uint32_t) (scaleto8_24 * vlf);
                vr = (uint32_t) (scaleto8_24 * vrf);
                uint16_t sendLevel = proxy->getSendLevel_U4_12();
                if (sendLevel > MAX_GAIN_INT) {
                    ALOGV("Track send level out of range: %04X", sendLevel);
                    sendLevel = MAX_GAIN_INT;
                }
                vaf = v * sendLevel * (1. / MAX_GAIN_INT);
            }
            track->setFinalVolume(vrf, vlf);
            if (chain != 0 && chain->setVolume_l(&vl, &vr)) {
                param = AudioMixer::VOLUME;
                vlf = (float)vl / (1 << 24);
                vrf = (float)vr / (1 << 24);
                track->setHasVolumeController(true);
            } else {
                if (track->hasVolumeController()) {
                    param = AudioMixer::VOLUME;
                }
                track->setHasVolumeController(false);
            }
            mAudioMixer->setBufferProvider(trackId, track->asExtendedAudioBufferProvider());
            mAudioMixer->enable(trackId);
            mAudioMixer->setParameter(trackId, param, AudioMixer::VOLUME0, &vlf);
            mAudioMixer->setParameter(trackId, param, AudioMixer::VOLUME1, &vrf);
            mAudioMixer->setParameter(trackId, param, AudioMixer::AUXLEVEL, &vaf);
            mAudioMixer->setParameter(
                trackId,
                AudioMixer::TRACK,
                AudioMixer::FORMAT, (void *)track->format());
            mAudioMixer->setParameter(
                trackId,
                AudioMixer::TRACK,
                AudioMixer::CHANNEL_MASK, (void *)(uintptr_t)track->channelMask());
            if (mType == SPATIALIZER && !track->isSpatialized()) {
                mAudioMixer->setParameter(
                    trackId,
                    AudioMixer::TRACK,
                    AudioMixer::MIXER_CHANNEL_MASK,
                    (void *)(uintptr_t)(mChannelMask | mHapticChannelMask));
            } else {
                mAudioMixer->setParameter(
                    trackId,
                    AudioMixer::TRACK,
                    AudioMixer::MIXER_CHANNEL_MASK,
                    (void *)(uintptr_t)(mMixerChannelMask | mHapticChannelMask));
            }
            uint32_t maxSampleRate = mSampleRate * AUDIO_RESAMPLER_DOWN_RATIO_MAX;
            uint32_t reqSampleRate = proxy->getSampleRate();
            if (reqSampleRate == 0) {
                reqSampleRate = mSampleRate;
            } else if (reqSampleRate > maxSampleRate) {
                reqSampleRate = maxSampleRate;
            }
            mAudioMixer->setParameter(
                trackId,
                AudioMixer::RESAMPLE,
                AudioMixer::SAMPLE_RATE,
                (void *)(uintptr_t)reqSampleRate);
            mAudioMixer->setParameter(
                trackId,
                AudioMixer::TIMESTRETCH,
                AudioMixer::PLAYBACK_RATE,
                const_cast<void *>(reinterpret_cast<const void *>(&playbackRate)));
            if (mMixerBufferEnabled
                    && (track->mainBuffer() == mSinkBuffer
                            || track->mainBuffer() == mMixerBuffer)) {
                if (mType == SPATIALIZER && !track->isSpatialized()) {
                    mAudioMixer->setParameter(
                            trackId,
                            AudioMixer::TRACK,
                            AudioMixer::MIXER_FORMAT, (void *)mEffectBufferFormat);
                    mAudioMixer->setParameter(
                            trackId,
                            AudioMixer::TRACK,
                            AudioMixer::MAIN_BUFFER, (void *)mPostSpatializerBuffer);
                } else {
                    mAudioMixer->setParameter(
                            trackId,
                            AudioMixer::TRACK,
                            AudioMixer::MIXER_FORMAT, (void *)mMixerBufferFormat);
                    mAudioMixer->setParameter(
                            trackId,
                            AudioMixer::TRACK,
                            AudioMixer::MAIN_BUFFER, (void *)mMixerBuffer);
                    mMixerBufferValid = true;
                }
            } else {
                mAudioMixer->setParameter(
                        trackId,
                        AudioMixer::TRACK,
                        AudioMixer::MIXER_FORMAT, (void *)AUDIO_FORMAT_PCM_FLOAT);
                mAudioMixer->setParameter(
                        trackId,
                        AudioMixer::TRACK,
                        AudioMixer::MAIN_BUFFER, (void *)track->mainBuffer());
            }
            mAudioMixer->setParameter(
                trackId,
                AudioMixer::TRACK,
                AudioMixer::AUX_BUFFER, (void *)track->auxBuffer());
            mAudioMixer->setParameter(
                trackId,
                AudioMixer::TRACK,
                AudioMixer::HAPTIC_ENABLED, (void *)(uintptr_t)track->getHapticPlaybackEnabled());
            mAudioMixer->setParameter(
                trackId,
                AudioMixer::TRACK,
                AudioMixer::HAPTIC_INTENSITY, (void *)(uintptr_t)track->getHapticIntensity());
            const float hapticMaxAmplitude = track->getHapticMaxAmplitude();
            mAudioMixer->setParameter(
                trackId,
                AudioMixer::TRACK,
                AudioMixer::HAPTIC_MAX_AMPLITUDE, (void *)&hapticMaxAmplitude);
            track->retryCount() = kMaxTrackRetries;
            if (mMixerStatusIgnoringFastTracks != MIXER_TRACKS_READY ||
                    mixerStatus != MIXER_TRACKS_ENABLED) {
                mixerStatus = MIXER_TRACKS_READY;
            }
#if 0
            static int i;
            if ((++i & 0xf) == 0) {
                deferredOperations.tallyUnderrunFrames(track, 10 );
            }
#endif
        } else {
            size_t underrunFrames = 0;
            if (framesReady < desiredFrames && !track->isStopped() && !track->isPaused()) {
                ALOGV("track(%d) underrun, track state %s  framesReady(%zu) < framesDesired(%zd)",
                        trackId, track->getTrackStateAsString(), framesReady, desiredFrames);
                underrunFrames = desiredFrames;
            }
            deferredOperations.tallyUnderrunFrames(track, underrunFrames);
            chain = getEffectChain_l(track->sessionId());
            if (chain != 0) {
                chain->clearInputBuffer();
            }
            ALOGVV("track(%d) s=%08x [NOT READY] on thread %p", trackId, cblk->mServer, this);
            if ((track->sharedBuffer() != 0) || track->isTerminated() ||
                    track->isStopped() || track->isPaused()) {
                size_t audioHALFrames = (latency_l() * mSampleRate) / 1000;
                int64_t framesWritten = mBytesWritten / mFrameSize;
                if (mStandby || track->presentationComplete(framesWritten, audioHALFrames)) {
                    if (track->isStopped()) {
                        track->reset();
                    }
                    tracksToRemove->add(track);
                }
            } else {
                if (--(track->retryCount()) <= 0) {
                    ALOGI("BUFFER TIMEOUT: remove(%d) from active list on thread %p",
                            trackId, this);
                    tracksToRemove->add(track);
                    track->disable();
                } else if (mMixerStatusIgnoringFastTracks == MIXER_TRACKS_READY ||
                                mixerStatus != MIXER_TRACKS_READY) {
                    mixerStatus = MIXER_TRACKS_ENABLED;
                }
            }
            mAudioMixer->disable(trackId);
        }
        }
    }
    if (mHapticChannelMask != AUDIO_CHANNEL_NONE && sq != NULL) {
        FastTrack *fastTrack = &state->mFastTracks[0];
        if (fastTrack->mHapticPlaybackEnabled != noFastHapticTrack) {
            fastTrack->mHapticPlaybackEnabled = noFastHapticTrack;
            didModify = true;
        }
    }
    [[maybe_unused]] bool pauseAudioWatchdog = false;
    if (didModify) {
        state->mFastTracksGen++;
        if (kUseFastMixer == FastMixer_Dynamic &&
                state->mCommand == FastMixerState::MIX_WRITE && state->mTrackMask <= 1) {
            state->mCommand = FastMixerState::COLD_IDLE;
            state->mColdFutexAddr = &mFastMixerFutex;
            state->mColdGen++;
            mFastMixerFutex = 0;
            if (kUseFastMixer == FastMixer_Dynamic) {
                mNormalSink = mOutputSink;
            }
            block = FastMixerStateQueue::BLOCK_UNTIL_ACKED;
            pauseAudioWatchdog = true;
        }
    }
    if (sq != NULL) {
        sq->end(didModify);
        sq->push(coldIdle ? FastMixerStateQueue::BLOCK_NEVER : block);
    }
#ifdef AUDIO_WATCHDOG
    if (pauseAudioWatchdog && mAudioWatchdog != 0) {
        mAudioWatchdog->pause();
    }
#endif
    while (resetMask != 0) {
        size_t i = __builtin_ctz(resetMask);
        ALOG_ASSERT(i < count);
        resetMask &= ~(1 << i);
        sp<IAfTrack> track = mActiveTracks[i];
        ALOG_ASSERT(track->isFastTrack() && track->isStopped());
        track->reset();
    }
    for (const auto &track : *tracksToRemove) {
        const int trackId = track->id();
        if (mAudioMixer->exists(trackId)) {
            mAudioMixer->setBufferProvider(trackId, nullptr );
        }
    }
    removeTracks_l(*tracksToRemove);
    if (getEffectChain_l(AUDIO_SESSION_OUTPUT_MIX) != 0 ||
            getEffectChain_l(AUDIO_SESSION_OUTPUT_STAGE) != 0) {
        mEffectBufferValid = true;
    }
    if (mEffectBufferValid) {
        memset(mEffectBuffer, 0, mEffectBufferSize);
        if (mType == SPATIALIZER) {
            memset(mPostSpatializerBuffer, 0, mPostSpatializerBufferSize);
        }
    }
    if ((mBytesRemaining == 0) && (((mixedTracks != 0 && mixedTracks == tracksWithEffect) ||
            (mixedTracks == 0 && fastTracks > 0)) || (mType == SPATIALIZER))) {
        if (mMixerBufferValid) {
            memset(mMixerBuffer, 0, mMixerBufferSize);
        }
        memset(mSinkBuffer, 0, mNormalFrameCount * mFrameSize);
    }
    mMixerStatusIgnoringFastTracks = mixerStatus;
    if (fastTracks > 0) {
        mixerStatus = MIXER_TRACKS_READY;
    }
    return mixerStatus;
}
uint32_t PlaybackThread::trackCountForUid_l(uid_t uid) const
{
    uint32_t trackCount = 0;
    for (size_t i = 0; i < mTracks.size() ; i++) {
        if (mTracks[i]->uid() == uid) {
            trackCount++;
        }
    }
    return trackCount;
}
bool PlaybackThread::IsTimestampAdvancing::check(AudioStreamOut* output)
{
    const nsecs_t nowNs = systemTime();
    if (nowNs - mPreviousNs < mMinimumTimeBetweenChecksNs) {
        return mLatchedValue;
    }
    mPreviousNs = nowNs;
    mLatchedValue = false;
    uint64_t position = 0;
    struct timespec unused;
    const status_t ret = output->getPresentationPosition(&position, &unused);
    if (ret == NO_ERROR) {
        if (position != mPreviousPosition) {
            mPreviousPosition = position;
            mLatchedValue = true;
        }
    }
    return mLatchedValue;
}
void PlaybackThread::IsTimestampAdvancing::clear()
{
    mLatchedValue = true;
    mPreviousPosition = 0;
    mPreviousNs = 0;
}
bool MixerThread::isTrackAllowed_l(
        audio_channel_mask_t channelMask, audio_format_t format,
        audio_session_t sessionId, uid_t uid) const
{
    if (!PlaybackThread::isTrackAllowed_l(channelMask, format, sessionId, uid)) {
        return false;
    }
    if (!mAudioMixer->isValidFormat(format)) {
        ALOGW("%s: invalid format: %#x", __func__, format);
        return false;
    }
    if (!mAudioMixer->isValidChannelMask(channelMask)) {
        ALOGW("%s: invalid channelMask: %#x", __func__, channelMask);
        return false;
    }
    return true;
}
bool MixerThread::checkForNewParameter_l(const String8& keyValuePair,
                                                       status_t& status)
{
    bool reconfig = false;
    status = NO_ERROR;
    AutoPark<FastMixer> park(mFastMixer);
    AudioParameter param = AudioParameter(keyValuePair);
    int value;
    if (param.getInt(String8(AudioParameter::keySamplingRate), value) == NO_ERROR) {
        reconfig = true;
    }
    if (param.getInt(String8(AudioParameter::keyFormat), value) == NO_ERROR) {
        if (!isValidPcmSinkFormat(static_cast<audio_format_t>(value))) {
            status = BAD_VALUE;
        } else {
            reconfig = true;
        }
    }
    if (param.getInt(String8(AudioParameter::keyChannels), value) == NO_ERROR) {
        if (!isValidPcmSinkChannelMask(static_cast<audio_channel_mask_t>(value))) {
            status = BAD_VALUE;
        } else {
            reconfig = true;
        }
    }
    if (param.getInt(String8(AudioParameter::keyFrameCount), value) == NO_ERROR) {
        if (!mTracks.isEmpty()) {
            status = INVALID_OPERATION;
        } else {
            reconfig = true;
        }
    }
    if (param.getInt(String8(AudioParameter::keyRouting), value) == NO_ERROR) {
        LOG_FATAL("Should not set routing device in MixerThread");
    }
    if (status == NO_ERROR) {
        status = mOutput->stream->setParameters(keyValuePair);
        if (!mStandby && status == INVALID_OPERATION) {
            ALOGW("%s: setParameters failed with keyValuePair %s, entering standby",
                    __func__, keyValuePair.c_str());
            mOutput->standby();
            mThreadMetrics.logEndInterval();
            mThreadSnapshot.onEnd();
            setStandby_l();
            mBytesWritten = 0;
            status = mOutput->stream->setParameters(keyValuePair);
        }
        if (status == NO_ERROR && reconfig) {
            readOutputParameters_l();
            delete mAudioMixer;
            mAudioMixer = new AudioMixer(mNormalFrameCount, mSampleRate);
            for (const auto &track : mTracks) {
                const int trackId = track->id();
                const status_t createStatus = mAudioMixer->create(
                        trackId,
                        track->channelMask(),
                        track->format(),
                        track->sessionId());
                ALOGW_IF(createStatus != NO_ERROR,
                        "%s(): AudioMixer cannot create track(%d)"
                        " mask %#x, format %#x, sessionId %d",
                        __func__,
                        trackId, track->channelMask(), track->format(), track->sessionId());
            }
            sendIoConfigEvent_l(AUDIO_OUTPUT_CONFIG_CHANGED);
        }
    }
    return reconfig;
}
void MixerThread::dumpInternals_l(int fd, const Vector<String16>& args)
{
    PlaybackThread::dumpInternals_l(fd, args);
    dprintf(fd, "  Thread throttle time (msecs): %u\n", mThreadThrottleTimeMs);
    dprintf(fd, "  AudioMixer tracks: %s\n", mAudioMixer->trackNames().c_str());
    dprintf(fd, "  Master mono: %s\n", mMasterMono ? "on" : "off");
    dprintf(fd, "  Master balance: %f (%s)\n", mMasterBalance.load(),
            (hasFastMixer() ? std::to_string(mFastMixer->getMasterBalance())
                            : mBalance.toString()).c_str());
    if (hasFastMixer()) {
        dprintf(fd, "  FastMixer thread %p tid=%d", mFastMixer.get(), mFastMixer->getTid());
        const std::unique_ptr<FastMixerDumpState> copy =
                std::make_unique<FastMixerDumpState>(mFastMixerDumpState);
        copy->dump(fd);
#ifdef STATE_QUEUE_DUMP
        StateQueueObserverDump observerCopy = mStateQueueObserverDump;
        observerCopy.dump(fd);
        StateQueueMutatorDump mutatorCopy = mStateQueueMutatorDump;
        mutatorCopy.dump(fd);
#endif
#ifdef AUDIO_WATCHDOG
        if (mAudioWatchdog != 0) {
            AudioWatchdogDump wdCopy = mAudioWatchdogDump;
            wdCopy.dump(fd);
        }
#endif
    } else {
        dprintf(fd, "  No FastMixer\n");
    }
     dprintf(fd, "Bluetooth latency modes are %senabled\n",
            mBluetoothLatencyModesEnabled ? "" : "not ");
     dprintf(fd, "HAL does %ssupport Bluetooth latency modes\n", mOutput != nullptr &&
             mOutput->audioHwDev->supportsBluetoothVariableLatency() ? "" : "not ");
     dprintf(fd, "Supported latency modes: %s\n", toString(mSupportedLatencyModes).c_str());
}
uint32_t MixerThread::idleSleepTimeUs() const
{
    return (uint32_t)(((mNormalFrameCount * 1000) / mSampleRate) * 1000) / 2;
}
uint32_t MixerThread::suspendSleepTimeUs() const
{
    return (uint32_t)(((mNormalFrameCount * 1000) / mSampleRate) * 1000);
}
void MixerThread::cacheParameters_l()
{
    PlaybackThread::cacheParameters_l();
    maxPeriod = seconds(mNormalFrameCount) / mSampleRate * 15;
}
void MixerThread::onHalLatencyModesChanged_l() {
    mAfThreadCallback->onSupportedLatencyModesChanged(mId, mSupportedLatencyModes);
}
void MixerThread::setHalLatencyMode_l() {
    if (!mBluetoothLatencyModesEnabled.load() || mSupportedLatencyModes.empty()) {
        return;
    }
    if (mOutDeviceTypeAddrs.size() != 1
            || !(audio_is_a2dp_out_device(mOutDeviceTypeAddrs[0].mType)
                 || audio_is_ble_out_device(mOutDeviceTypeAddrs[0].mType))) {
        return;
    }
    audio_latency_mode_t latencyMode = AUDIO_LATENCY_MODE_FREE;
    if (mSupportedLatencyModes.size() == 1) {
        latencyMode = mSupportedLatencyModes[0];
    } else if (mSupportedLatencyModes.size() > 1) {
        for (const auto& track : mActiveTracks) {
            if ((track->isFastTrack() && track->attributes().usage == AUDIO_USAGE_GAME)
                    || track->attributes().usage == AUDIO_USAGE_ASSISTANCE_ACCESSIBILITY) {
                latencyMode = AUDIO_LATENCY_MODE_LOW;
                break;
            }
        }
    }
    if (latencyMode != mSetLatencyMode) {
        status_t status = mOutput->stream->setLatencyMode(latencyMode);
        ALOGD("%s: thread(%d) setLatencyMode(%s) returned %d",
                __func__, mId, toString(latencyMode).c_str(), status);
        if (status == NO_ERROR) {
            mSetLatencyMode = latencyMode;
        }
    }
}
void MixerThread::updateHalSupportedLatencyModes_l() {
    if (mOutput == nullptr || mOutput->stream == nullptr) {
        return;
    }
    std::vector<audio_latency_mode_t> latencyModes;
    const status_t status = mOutput->stream->getRecommendedLatencyModes(&latencyModes);
    if (status != NO_ERROR) {
        latencyModes.clear();
    }
    if (latencyModes != mSupportedLatencyModes) {
        ALOGD("%s: thread(%d) status %d supported latency modes: %s",
            __func__, mId, status, toString(latencyModes).c_str());
        mSupportedLatencyModes.swap(latencyModes);
        sendHalLatencyModesChangedEvent_l();
    }
}
status_t MixerThread::getSupportedLatencyModes(
        std::vector<audio_latency_mode_t>* modes) {
    if (modes == nullptr) {
        return BAD_VALUE;
    }
    audio_utils::lock_guard _l(mutex());
    *modes = mSupportedLatencyModes;
    return NO_ERROR;
}
void MixerThread::onRecommendedLatencyModeChanged(
        std::vector<audio_latency_mode_t> modes) {
    audio_utils::lock_guard _l(mutex());
    if (modes != mSupportedLatencyModes) {
        ALOGD("%s: thread(%d) supported latency modes: %s",
            __func__, mId, toString(modes).c_str());
        mSupportedLatencyModes.swap(modes);
        sendHalLatencyModesChangedEvent_l();
    }
}
status_t MixerThread::setBluetoothVariableLatencyEnabled(bool enabled) {
    if (mOutput == nullptr || mOutput->audioHwDev == nullptr
            || !mOutput->audioHwDev->supportsBluetoothVariableLatency()) {
        return INVALID_OPERATION;
    }
    mBluetoothLatencyModesEnabled.store(enabled);
    return NO_ERROR;
}
sp<IAfPlaybackThread> IAfPlaybackThread::createDirectOutputThread(
        const sp<IAfThreadCallback>& afThreadCallback,
        AudioStreamOut* output, audio_io_handle_t id, bool systemReady,
        const audio_offload_info_t& offloadInfo) {
    return sp<DirectOutputThread>::make(
            afThreadCallback, output, id, systemReady, offloadInfo);
}
DirectOutputThread::DirectOutputThread(const sp<IAfThreadCallback>& afThreadCallback,
        AudioStreamOut* output, audio_io_handle_t id, ThreadBase::type_t type, bool systemReady,
        const audio_offload_info_t& offloadInfo)
    : PlaybackThread(afThreadCallback, output, id, type, systemReady)
    , mOffloadInfo(offloadInfo)
{
    setMasterBalance(afThreadCallback->getMasterBalance_l());
}
DirectOutputThread::~DirectOutputThread()
{
}
void DirectOutputThread::dumpInternals_l(int fd, const Vector<String16>& args)
{
    PlaybackThread::dumpInternals_l(fd, args);
    dprintf(fd, "  Master balance: %f  Left: %f  Right: %f\n",
            mMasterBalance.load(), mMasterBalanceLeft, mMasterBalanceRight);
}
void DirectOutputThread::setMasterBalance(float balance)
{
    audio_utils::lock_guard _l(mutex());
    if (mMasterBalance != balance) {
        mMasterBalance.store(balance);
        mBalance.computeStereoBalance(balance, &mMasterBalanceLeft, &mMasterBalanceRight);
        broadcast_l();
    }
}
void DirectOutputThread::processVolume_l(IAfTrack* track, bool lastTrack)
{
    float left, right;
    const sp<AudioTrackServerProxy> proxy = track->audioTrackServerProxy();
    const int64_t frames = mTimestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL];
    const int64_t time = mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL];
    ALOGVV("%s: Direct/Offload bufferConsumed:%zu  timestamp frames:%lld  time:%lld",
            __func__, proxy->framesReleased(), (long long)frames, (long long)time);
    const int64_t volumeShaperFrames =
            mMonotonicFrameCounter.updateAndGetMonotonicFrameCount(frames, time);
    const auto [shaperVolume, shaperActive] =
            track->getVolumeHandler()->getVolume(volumeShaperFrames);
    mVolumeShaperActive = shaperActive;
    gain_minifloat_packed_t vlr = proxy->getVolumeLR();
    left = float_from_gain(gain_minifloat_unpack_left(vlr));
    right = float_from_gain(gain_minifloat_unpack_right(vlr));
    const bool clientVolumeMute = (left == 0.f && right == 0.f);
    if (mMasterMute || mStreamTypes[track->streamType()].mute || track->isPlaybackRestricted()) {
        left = right = 0;
    } else {
        float typeVolume = mStreamTypes[track->streamType()].volume;
        const float v = mMasterVolume * typeVolume * shaperVolume;
        if (left > GAIN_FLOAT_UNITY) {
            left = GAIN_FLOAT_UNITY;
        }
        if (right > GAIN_FLOAT_UNITY) {
            right = GAIN_FLOAT_UNITY;
        }
        left *= v;
        right *= v;
        if (mAfThreadCallback->getMode() != AUDIO_MODE_IN_COMMUNICATION
                || audio_channel_count_from_out_mask(mChannelMask) > 1) {
            left *= mMasterBalanceLeft;
            right *= mMasterBalanceRight;
        }
    }
    track->processMuteEvent_l(mAfThreadCallback->getOrCreateAudioManager(),
                      {mMasterMute,
                       mStreamTypes[track->streamType()].volume == 0.f,
                       mStreamTypes[track->streamType()].mute,
                       track->isPlaybackRestricted(),
                       clientVolumeMute,
                       shaperVolume == 0.f});
    if (lastTrack) {
        track->setFinalVolume(left, right);
        if (left != mLeftVolFloat || right != mRightVolFloat) {
            mLeftVolFloat = left;
            mRightVolFloat = right;
            if (!mEffectChains.isEmpty()) {
                uint32_t vl = (uint32_t)(left * (1 << 24));
                uint32_t vr = (uint32_t)(right * (1 << 24));
                (void)mEffectChains[0]->setVolume_l(&vl, &vr);
            } else {
                setVolumeForOutput_l(left, right);
            }
        }
    }
}
void DirectOutputThread::onAddNewTrack_l()
{
    sp<IAfTrack> previousTrack = mPreviousTrack.promote();
    sp<IAfTrack> latestTrack = mActiveTracks.getLatest();
    if (previousTrack != 0 && latestTrack != 0) {
        if (mType == DIRECT) {
            if (previousTrack.get() != latestTrack.get()) {
                mFlushPending = true;
            }
        } else {
            if (previousTrack->sessionId() != latestTrack->sessionId() ||
                previousTrack->isFlushPending()) {
                mFlushPending = true;
            }
        }
    } else if (previousTrack == 0) {
        mFlushPending = true;
    }
    PlaybackThread::onAddNewTrack_l();
}
PlaybackThread::mixer_state DirectOutputThread::prepareTracks_l(
    Vector<sp<IAfTrack>>* tracksToRemove
)
{
    size_t count = mActiveTracks.size();
    mixer_state mixerStatus = MIXER_IDLE;
    bool doHwPause = false;
    bool doHwResume = false;
    for (const sp<IAfTrack>& t : mActiveTracks) {
        if (t->isInvalid()) {
            ALOGW("An invalidated track shouldn't be in active list");
            tracksToRemove->add(t);
            continue;
        }
        IAfTrack* const track = t.get();
#ifdef VERY_VERY_VERBOSE_LOGGING
        audio_track_cblk_t* cblk = track->cblk();
#endif
        sp<IAfTrack> l = mActiveTracks.getLatest();
        bool last = l.get() == track;
        if (track->isPausePending()) {
            track->pauseAck();
            if (track->isPausing()) {
                track->setPaused();
            }
            if (mHwSupportsPause && last && !mHwPaused) {
                doHwPause = true;
                mHwPaused = true;
            }
        } else if (track->isFlushPending()) {
            track->flushAck();
            if (last) {
                mFlushPending = true;
            }
        } else if (track->isResumePending()) {
            track->resumeAck();
            if (last) {
                mLeftVolFloat = mRightVolFloat = -1.0;
                if (mHwPaused) {
                    doHwResume = true;
                    mHwPaused = false;
                }
            }
        }
        const int32_t targetRetryCount = kMaxTrackRetriesDirectMs * 1000 / mActiveSleepTimeUs;
        const int32_t retryThreshold = targetRetryCount > 2 ? targetRetryCount - 1 : 1;
        uint32_t minFrames;
        if ((track->sharedBuffer() == 0) && !track->isStopping_1() && !track->isPausing()
            && (track->retryCount() > retryThreshold) && audio_has_proportional_frames(mFormat)) {
            minFrames = mNormalFrameCount;
        } else {
            minFrames = 1;
        }
        const size_t framesReady = track->framesReady();
        const int trackId = track->id();
        if (ATRACE_ENABLED()) {
            std::string traceName("nRdy");
            traceName += std::to_string(trackId);
            ATRACE_INT(traceName.c_str(), framesReady);
        }
        if ((framesReady >= minFrames) && track->isReady() && !track->isPaused() &&
                !track->isStopping_2() && !track->isStopped())
        {
            ALOGVV("track(%d) s=%08x [OK]", trackId, cblk->mServer);
            if (track->fillingStatus() == IAfTrack::FS_FILLED) {
                track->fillingStatus() = IAfTrack::FS_ACTIVE;
                if (last) {
                    mLeftVolFloat = mRightVolFloat = -1.0;
                }
                if (!mHwSupportsPause) {
                    track->resumeAck();
                }
            }
            processVolume_l(track, last);
            if (last) {
                sp<IAfTrack> previousTrack = mPreviousTrack.promote();
                if (previousTrack != 0) {
                    if (track != previousTrack.get()) {
                        mBytesRemaining = 0;
                        previousTrack->invalidate();
                    }
                }
                mPreviousTrack = track;
                track->retryCount() = targetRetryCount;
                mActiveTrack = t;
                mixerStatus = MIXER_TRACKS_READY;
                if (mHwPaused) {
                    doHwResume = true;
                    mHwPaused = false;
                }
            }
        } else {
            if (!mEffectChains.isEmpty() && last) {
                mEffectChains[0]->clearInputBuffer();
            }
            if (track->isStopping_1()) {
                track->setState(IAfTrackBase::STOPPING_2);
                if (last && mHwPaused) {
                     doHwResume = true;
                     mHwPaused = false;
                 }
            }
            if ((track->sharedBuffer() != 0) || track->isStopped() ||
                    track->isStopping_2() || track->isPaused()) {
                bool presComplete = false;
                if (mStandby || !last ||
                        (presComplete = track->presentationComplete(latency_l())) ||
                        track->isPaused() || mHwPaused) {
                    if (presComplete) {
                        mOutput->presentationComplete();
                    }
                    if (track->isStopping_2()) {
                        track->setState(IAfTrackBase::STOPPED);
                    }
                    if (track->isStopped()) {
                        track->reset();
                    }
                    tracksToRemove->add(track);
                }
            } else {
                bool isTimestampAdvancing = mIsTimestampAdvancing.check(mOutput);
                if (!isTunerStream()
                        && --(track->retryCount()) <= 0) {
                    if (isTimestampAdvancing) {
                        track->retryCount() = kMaxTrackRetriesOffload;
                    } else {
                        ALOGV("BUFFER TIMEOUT: remove track(%d) from active list", trackId);
                        tracksToRemove->add(track);
                        track->disable();
                        ALOGW("pause because of UNDERRUN, framesReady = %zu,"
                                "minFrames = %u, mFormat = %#x",
                                framesReady, minFrames, mFormat);
                        if (last && mHwSupportsPause && !mHwPaused && !mStandby) {
                            doHwPause = true;
                            mHwPaused = true;
                        }
                    }
                } else if (last) {
                    mixerStatus = MIXER_TRACKS_ENABLED;
                }
            }
        }
    }
    if (!mFlushPending) {
        for (size_t i = 0; i < mTracks.size(); i++) {
            if (mTracks[i]->isFlushPending()) {
                mTracks[i]->flushAck();
                mFlushPending = true;
            }
        }
    }
    if (mHwSupportsPause && !mStandby &&
            (doHwPause || (mFlushPending && !mHwPaused && (count != 0)))) {
        status_t result = mOutput->stream->pause();
        ALOGE_IF(result != OK, "Error when pausing output stream: %d", result);
        doHwResume = !doHwPause;
    }
    if (mFlushPending) {
        flushHw_l();
    }
    if (mHwSupportsPause && !mStandby && doHwResume) {
        status_t result = mOutput->stream->resume();
        ALOGE_IF(result != OK, "Error when resuming output stream: %d", result);
    }
    removeTracks_l(*tracksToRemove);
    return mixerStatus;
}
void DirectOutputThread::threadLoop_mix()
{
    size_t frameCount = mFrameCount;
    int8_t *curBuf = (int8_t *)mSinkBuffer;
    while (frameCount) {
        AudioBufferProvider::Buffer buffer;
        buffer.frameCount = frameCount;
        status_t status = mActiveTrack->getNextBuffer(&buffer);
        if (status != NO_ERROR || buffer.raw == NULL) {
            if (audio_has_proportional_frames(mFormat)) {
                memset(curBuf, 0, frameCount * mFrameSize);
            }
            break;
        }
        memcpy(curBuf, buffer.raw, buffer.frameCount * mFrameSize);
        frameCount -= buffer.frameCount;
        curBuf += buffer.frameCount * mFrameSize;
        mActiveTrack->releaseBuffer(&buffer);
    }
    mCurrentWriteLength = curBuf - (int8_t *)mSinkBuffer;
    mSleepTimeUs = 0;
    mStandbyTimeNs = systemTime() + mStandbyDelayNs;
    mActiveTrack.clear();
}
void DirectOutputThread::threadLoop_sleepTime()
{
    if (mHwPaused || (usesHwAvSync() && mStandby)) {
        mSleepTimeUs = mIdleSleepTimeUs;
        return;
    }
    if (mMixerStatus == MIXER_TRACKS_ENABLED) {
        mSleepTimeUs = mActiveSleepTimeUs;
    } else {
        mSleepTimeUs = mIdleSleepTimeUs;
    }
}
void DirectOutputThread::threadLoop_exit()
{
    {
        audio_utils::lock_guard _l(mutex());
        for (size_t i = 0; i < mTracks.size(); i++) {
            if (mTracks[i]->isFlushPending()) {
                mTracks[i]->flushAck();
                mFlushPending = true;
            }
        }
        if (mFlushPending) {
            flushHw_l();
        }
    }
    PlaybackThread::threadLoop_exit();
}
bool DirectOutputThread::shouldStandby_l()
{
    bool trackPaused = false;
    bool trackStopped = false;
    if (mTracks.size() > 0) {
        trackPaused = mTracks[mTracks.size() - 1]->isPaused();
        trackStopped = mTracks[mTracks.size() - 1]->isStopped() ||
                           mTracks[mTracks.size() - 1]->state() == IAfTrackBase::IDLE;
    }
    return !mStandby && !(trackPaused || (mHwPaused && !trackStopped));
}
bool DirectOutputThread::checkForNewParameter_l(const String8& keyValuePair,
                                                              status_t& status)
{
    bool reconfig = false;
    status = NO_ERROR;
    AudioParameter param = AudioParameter(keyValuePair);
    int value;
    if (param.getInt(String8(AudioParameter::keyRouting), value) == NO_ERROR) {
        LOG_FATAL("Should not set routing device in DirectOutputThread");
    }
    if (param.getInt(String8(AudioParameter::keyFrameCount), value) == NO_ERROR) {
        if (!mTracks.isEmpty()) {
            status = INVALID_OPERATION;
        } else {
            reconfig = true;
        }
    }
    if (status == NO_ERROR) {
        status = mOutput->stream->setParameters(keyValuePair);
        if (!mStandby && status == INVALID_OPERATION) {
            mOutput->standby();
            if (!mStandby) {
                mThreadMetrics.logEndInterval();
                mThreadSnapshot.onEnd();
                setStandby_l();
            }
            mBytesWritten = 0;
            status = mOutput->stream->setParameters(keyValuePair);
        }
        if (status == NO_ERROR && reconfig) {
            readOutputParameters_l();
            sendIoConfigEvent_l(AUDIO_OUTPUT_CONFIG_CHANGED);
        }
    }
    return reconfig;
}
uint32_t DirectOutputThread::activeSleepTimeUs() const
{
    uint32_t time;
    if (audio_has_proportional_frames(mFormat)) {
        time = PlaybackThread::activeSleepTimeUs();
    } else {
        time = kDirectMinSleepTimeUs;
    }
    return time;
}
uint32_t DirectOutputThread::idleSleepTimeUs() const
{
    uint32_t time;
    if (audio_has_proportional_frames(mFormat)) {
        time = (uint32_t)(((mFrameCount * 1000) / mSampleRate) * 1000) / 2;
    } else {
        time = kDirectMinSleepTimeUs;
    }
    return time;
}
uint32_t DirectOutputThread::suspendSleepTimeUs() const
{
    uint32_t time;
    if (audio_has_proportional_frames(mFormat)) {
        time = (uint32_t)(((mFrameCount * 1000) / mSampleRate) * 1000);
    } else {
        time = kDirectMinSleepTimeUs;
    }
    return time;
}
void DirectOutputThread::cacheParameters_l()
{
    PlaybackThread::cacheParameters_l();
    if (usesHwAvSync()) {
        mStandbyDelayNs = 0;
    } else if ((mType == OFFLOAD) && !audio_has_proportional_frames(mFormat)) {
        mStandbyDelayNs = kOffloadStandbyDelayNs;
    } else {
        mStandbyDelayNs = microseconds(mActiveSleepTimeUs*2);
    }
}
void DirectOutputThread::flushHw_l()
{
    PlaybackThread::flushHw_l();
    mOutput->flush();
    mHwPaused = false;
    mFlushPending = false;
    mTimestampVerifier.discontinuity(discontinuityForStandbyOrFlush());
    mTimestamp.clear();
    mMonotonicFrameCounter.onFlush();
}
int64_t DirectOutputThread::computeWaitTimeNs_l() const {
    const int64_t NS_PER_MS = 1000000;
    return mVolumeShaperActive ?
            kMinNormalSinkBufferSizeMs * NS_PER_MS : PlaybackThread::computeWaitTimeNs_l();
}
AsyncCallbackThread::AsyncCallbackThread(
        const wp<PlaybackThread>& playbackThread)
    : Thread(false ),
        mPlaybackThread(playbackThread),
        mWriteAckSequence(0),
        mDrainSequence(0),
        mAsyncError(false)
{
}
void AsyncCallbackThread::onFirstRef()
{
    run("Offload Cbk", ANDROID_PRIORITY_URGENT_AUDIO);
}
bool AsyncCallbackThread::threadLoop()
{
    while (!exitPending()) {
        uint32_t writeAckSequence;
        uint32_t drainSequence;
        bool asyncError;
        {
            audio_utils::unique_lock _l(mutex());
            while (!((mWriteAckSequence & 1) ||
                     (mDrainSequence & 1) ||
                     mAsyncError ||
                     exitPending())) {
                mWaitWorkCV.wait(_l);
            }
            if (exitPending()) {
                break;
            }
            ALOGV("AsyncCallbackThread mWriteAckSequence %d mDrainSequence %d",
                  mWriteAckSequence, mDrainSequence);
            writeAckSequence = mWriteAckSequence;
            mWriteAckSequence &= ~1;
            drainSequence = mDrainSequence;
            mDrainSequence &= ~1;
            asyncError = mAsyncError;
            mAsyncError = false;
        }
        {
            const sp<PlaybackThread> playbackThread = mPlaybackThread.promote();
            if (playbackThread != 0) {
                if (writeAckSequence & 1) {
                    playbackThread->resetWriteBlocked(writeAckSequence >> 1);
                }
                if (drainSequence & 1) {
                    playbackThread->resetDraining(drainSequence >> 1);
                }
                if (asyncError) {
                    playbackThread->onAsyncError();
                }
            }
        }
    }
    return false;
}
void AsyncCallbackThread::exit()
{
    ALOGV("AsyncCallbackThread::exit");
    audio_utils::lock_guard _l(mutex());
    requestExit();
    mWaitWorkCV.notify_all();
}
void AsyncCallbackThread::setWriteBlocked(uint32_t sequence)
{
    audio_utils::lock_guard _l(mutex());
    mWriteAckSequence = sequence << 1;
}
void AsyncCallbackThread::resetWriteBlocked()
{
    audio_utils::lock_guard _l(mutex());
    if (mWriteAckSequence & 2) {
        mWriteAckSequence |= 1;
        mWaitWorkCV.notify_one();
    }
}
void AsyncCallbackThread::setDraining(uint32_t sequence)
{
    audio_utils::lock_guard _l(mutex());
    mDrainSequence = sequence << 1;
}
void AsyncCallbackThread::resetDraining()
{
    audio_utils::lock_guard _l(mutex());
    if (mDrainSequence & 2) {
        mDrainSequence |= 1;
        mWaitWorkCV.notify_one();
    }
}
void AsyncCallbackThread::setAsyncError()
{
    audio_utils::lock_guard _l(mutex());
    mAsyncError = true;
    mWaitWorkCV.notify_one();
}
sp<IAfPlaybackThread> IAfPlaybackThread::createOffloadThread(
        const sp<IAfThreadCallback>& afThreadCallback,
        AudioStreamOut* output, audio_io_handle_t id, bool systemReady,
        const audio_offload_info_t& offloadInfo) {
    return sp<OffloadThread>::make(afThreadCallback, output, id, systemReady, offloadInfo);
}
OffloadThread::OffloadThread(const sp<IAfThreadCallback>& afThreadCallback,
        AudioStreamOut* output, audio_io_handle_t id, bool systemReady,
        const audio_offload_info_t& offloadInfo)
    : DirectOutputThread(afThreadCallback, output, id, OFFLOAD, systemReady, offloadInfo),
        mPausedWriteLength(0), mPausedBytesRemaining(0), mKeepWakeLock(true)
{
    mStandby = true;
    mKeepWakeLock = property_get_bool("ro.audio.offload_wakelock", true );
}
void OffloadThread::threadLoop_exit()
{
    if (mFlushPending || mHwPaused) {
        flushHw_l();
    } else {
        mMixerStatus = MIXER_DRAIN_ALL;
        threadLoop_drain();
    }
    if (mUseAsyncWrite) {
        ALOG_ASSERT(mCallbackThread != 0);
        mCallbackThread->exit();
    }
    PlaybackThread::threadLoop_exit();
}
PlaybackThread::mixer_state OffloadThread::prepareTracks_l(
    Vector<sp<IAfTrack>>* tracksToRemove
)
{
    size_t count = mActiveTracks.size();
    mixer_state mixerStatus = MIXER_IDLE;
    bool doHwPause = false;
    bool doHwResume = false;
    ALOGV("OffloadThread::prepareTracks_l active tracks %zu", count);
    for (const sp<IAfTrack>& t : mActiveTracks) {
        IAfTrack* const track = t.get();
#ifdef VERY_VERY_VERBOSE_LOGGING
        audio_track_cblk_t* cblk = track->cblk();
#endif
        sp<IAfTrack> l = mActiveTracks.getLatest();
        bool last = l.get() == track;
        if (track->isInvalid()) {
            ALOGW("An invalidated track shouldn't be in active list");
            tracksToRemove->add(track);
            continue;
        }
        if (track->state() == IAfTrackBase::IDLE) {
            ALOGW("An idle track shouldn't be in active list");
            continue;
        }
        if (track->isPausePending()) {
            track->pauseAck();
            if (track->isPausing()) {
                track->setPaused();
            }
            if (last) {
                if (mHwSupportsPause && !mHwPaused) {
                    doHwPause = true;
                    mHwPaused = true;
                }
                mPausedWriteLength = mCurrentWriteLength;
                mPausedBytesRemaining = mBytesRemaining;
                mBytesRemaining = 0;
            }
            tracksToRemove->add(track);
        } else if (track->isFlushPending()) {
            if (track->isStopping_1()) {
                track->retryCount() = kMaxTrackStopRetriesOffload;
            } else {
                track->retryCount() = kMaxTrackRetriesOffload;
            }
            track->flushAck();
            if (last) {
                mFlushPending = true;
            }
        } else if (track->isResumePending()){
            track->resumeAck();
            if (last) {
                if (mPausedBytesRemaining) {
                    mCurrentWriteLength = mPausedWriteLength;
                    mBytesRemaining = mPausedBytesRemaining;
                    mPausedBytesRemaining = 0;
                }
                if (mHwPaused) {
                    doHwResume = true;
                    mHwPaused = false;
                }
                mSleepTimeUs = 0;
                mLeftVolFloat = mRightVolFloat = -1.0;
                mixerStatus = MIXER_TRACKS_ENABLED;
            }
        } else if (track->framesReady() && track->isReady() &&
                !track->isPaused() && !track->isTerminated() && !track->isStopping_2()) {
            ALOGVV("OffloadThread: track(%d) s=%08x [OK]", track->id(), cblk->mServer);
            if (track->fillingStatus() == IAfTrack::FS_FILLED) {
                track->fillingStatus() = IAfTrack::FS_ACTIVE;
                if (last) {
                    mLeftVolFloat = mRightVolFloat = -1.0;
                }
            }
            if (last) {
                sp<IAfTrack> previousTrack = mPreviousTrack.promote();
                if (previousTrack != 0) {
                    if (track != previousTrack.get()) {
                        mBytesRemaining = 0;
                        if (mPausedBytesRemaining) {
                            mPausedBytesRemaining = 0;
                            previousTrack->invalidate();
                        }
                        if (previousTrack->sessionId() != track->sessionId()) {
                            previousTrack->invalidate();
                        }
                    }
                }
                mPreviousTrack = track;
                if (track->isStopping_1()) {
                    track->retryCount() = kMaxTrackStopRetriesOffload;
                } else {
                    track->retryCount() = kMaxTrackRetriesOffload;
                }
                mActiveTrack = t;
                mixerStatus = MIXER_TRACKS_READY;
            }
        } else {
            ALOGVV("OffloadThread: track(%d) s=%08x [NOT READY]", track->id(), cblk->mServer);
            if (track->isStopping_1()) {
                if (--(track->retryCount()) <= 0) {
                    if (mBytesRemaining == 0) {
                        ALOGV("OffloadThread: underrun and STOPPING_1 -> draining, STOPPING_2");
                        track->setState(IAfTrackBase::STOPPING_2);
                        if (last && !mStandby) {
                            if ((mDrainSequence & 1) == 0) {
                                mSleepTimeUs = 0;
                                mStandbyTimeNs = systemTime() + mStandbyDelayNs;
                                mixerStatus = MIXER_DRAIN_TRACK;
                                mDrainSequence += 2;
                            }
                            if (mHwPaused) {
                                doHwResume = true;
                                mHwPaused = false;
                            }
                        }
                    }
                } else if (last) {
                    ALOGV("stopping1 underrun retries left %d", track->retryCount());
                    mixerStatus = MIXER_TRACKS_ENABLED;
                }
            } else if (track->isStopping_2()) {
                if (!(mDrainSequence & 1) || !last || mStandby) {
                    track->setState(IAfTrackBase::STOPPED);
                    mOutput->presentationComplete();
                    track->presentationComplete(latency_l());
                    track->reset();
                    tracksToRemove->add(track);
                    if (!mUseAsyncWrite) {
                        mTimestampVerifier.discontinuity(
                                mTimestampVerifier.DISCONTINUITY_MODE_ZERO);
                    }
                }
            } else {
                bool isTimestampAdvancing = mIsTimestampAdvancing.check(mOutput);
                if (!isTunerStream()
                        && --(track->retryCount()) <= 0) {
                    if (isTimestampAdvancing) {
                        track->retryCount() = kMaxTrackRetriesOffload;
                    } else {
                        ALOGV("OffloadThread: BUFFER TIMEOUT: remove track(%d) from active list",
                                track->id());
                        tracksToRemove->add(track);
                        track->disable();
                    }
                } else if (last){
                    mixerStatus = MIXER_TRACKS_ENABLED;
                }
            }
        }
        if (track->isReady()) {
            processVolume_l(track, last);
        }
    }
    if (!mStandby && (doHwPause || (mFlushPending && !mHwPaused && (count != 0)))) {
        status_t result = mOutput->stream->pause();
        ALOGE_IF(result != OK, "Error when pausing output stream: %d", result);
        doHwResume = !doHwPause;
    }
    if (mFlushPending) {
        flushHw_l();
    }
    if (!mStandby && doHwResume) {
        status_t result = mOutput->stream->resume();
        ALOGE_IF(result != OK, "Error when resuming output stream: %d", result);
    }
    removeTracks_l(*tracksToRemove);
    return mixerStatus;
}
bool OffloadThread::waitingAsyncCallback_l()
{
    ALOGVV("waitingAsyncCallback_l mWriteAckSequence %d mDrainSequence %d",
          mWriteAckSequence, mDrainSequence);
    if (mUseAsyncWrite && ((mWriteAckSequence & 1) || (mDrainSequence & 1))) {
        return true;
    }
    return false;
}
bool OffloadThread::waitingAsyncCallback()
{
    audio_utils::lock_guard _l(mutex());
    return waitingAsyncCallback_l();
}
void OffloadThread::flushHw_l()
{
    DirectOutputThread::flushHw_l();
    mCurrentWriteLength = 0;
    mBytesRemaining = 0;
    mPausedWriteLength = 0;
    mPausedBytesRemaining = 0;
    mBytesWritten = 0;
    if (mUseAsyncWrite) {
        mWriteAckSequence = (mWriteAckSequence + 2) & ~1;
        mDrainSequence = (mDrainSequence + 2) & ~1;
        ALOG_ASSERT(mCallbackThread != 0);
        mCallbackThread->setWriteBlocked(mWriteAckSequence);
        mCallbackThread->setDraining(mDrainSequence);
    }
}
void OffloadThread::invalidateTracks(audio_stream_type_t streamType)
{
    audio_utils::lock_guard _l(mutex());
    if (PlaybackThread::invalidateTracks_l(streamType)) {
        mFlushPending = true;
    }
}
void OffloadThread::invalidateTracks(std::set<audio_port_handle_t>& portIds) {
    audio_utils::lock_guard _l(mutex());
    if (PlaybackThread::invalidateTracks_l(portIds)) {
        mFlushPending = true;
    }
}
sp<IAfDuplicatingThread> IAfDuplicatingThread::create(
        const sp<IAfThreadCallback>& afThreadCallback,
        IAfPlaybackThread* mainThread, audio_io_handle_t id, bool systemReady) {
    return sp<DuplicatingThread>::make(afThreadCallback, mainThread, id, systemReady);
}
DuplicatingThread::DuplicatingThread(const sp<IAfThreadCallback>& afThreadCallback,
       IAfPlaybackThread* mainThread, audio_io_handle_t id, bool systemReady)
    : MixerThread(afThreadCallback, mainThread->getOutput(), id,
                    systemReady, DUPLICATING),
        mWaitTimeMs(UINT_MAX)
{
    addOutputTrack(mainThread);
}
DuplicatingThread::~DuplicatingThread()
{
    for (size_t i = 0; i < mOutputTracks.size(); i++) {
        mOutputTracks[i]->destroy();
    }
}
void DuplicatingThread::threadLoop_mix()
{
    if (outputsReady()) {
        mAudioMixer->process();
    } else {
        if (mMixerBufferValid) {
            memset(mMixerBuffer, 0, mMixerBufferSize);
        } else {
            memset(mSinkBuffer, 0, mSinkBufferSize);
        }
    }
    mSleepTimeUs = 0;
    writeFrames = mNormalFrameCount;
    mCurrentWriteLength = mSinkBufferSize;
    mStandbyTimeNs = systemTime() + mStandbyDelayNs;
}
void DuplicatingThread::threadLoop_sleepTime()
{
    if (mSleepTimeUs == 0) {
        if (mMixerStatus == MIXER_TRACKS_ENABLED) {
            mSleepTimeUs = mActiveSleepTimeUs;
        } else {
            mSleepTimeUs = mIdleSleepTimeUs;
        }
    } else if (mBytesWritten != 0) {
        if (mMixerStatus == MIXER_TRACKS_ENABLED) {
            writeFrames = mNormalFrameCount;
            memset(mSinkBuffer, 0, mSinkBufferSize);
        } else {
            writeFrames = 0;
        }
        mSleepTimeUs = 0;
    }
}
ssize_t DuplicatingThread::threadLoop_write()
{
    for (size_t i = 0; i < outputTracks.size(); i++) {
        const ssize_t actualWritten = outputTracks[i]->write(mSinkBuffer, writeFrames);
        if (i == 0) {
            const ssize_t correction = mSinkBufferSize / mFrameSize - actualWritten;
            ALOGD_IF(correction != 0 && writeFrames != 0,
                    "%s: writeFrames:%u  actualWritten:%zd  correction:%zd  mFramesWritten:%lld",
                    __func__, writeFrames, actualWritten, correction, (long long)mFramesWritten);
            mFramesWritten -= correction;
        }
    }
    if (mStandby) {
        mThreadMetrics.logBeginInterval();
        mThreadSnapshot.onBegin();
        mStandby = false;
    }
    return (ssize_t)mSinkBufferSize;
}
void DuplicatingThread::threadLoop_standby()
{
    for (size_t i = 0; i < outputTracks.size(); i++) {
        outputTracks[i]->stop();
    }
}
void DuplicatingThread::dumpInternals_l(int fd, const Vector<String16>& args)
{
    MixerThread::dumpInternals_l(fd, args);
    std::stringstream ss;
    const size_t numTracks = mOutputTracks.size();
    ss << "  " << numTracks << " OutputTracks";
    if (numTracks > 0) {
        ss << ":";
        for (const auto &track : mOutputTracks) {
            const auto thread = track->thread().promote();
            ss << " (" << track->id() << " : ";
            if (thread.get() != nullptr) {
                ss << thread.get() << ", " << thread->id();
            } else {
                ss << "null";
            }
            ss << ")";
        }
    }
    ss << "\n";
    std::string result = ss.str();
    write(fd, result.c_str(), result.size());
}
void DuplicatingThread::saveOutputTracks()
{
    outputTracks = mOutputTracks;
}
void DuplicatingThread::clearOutputTracks()
{
    outputTracks.clear();
}
void DuplicatingThread::addOutputTrack(IAfPlaybackThread* thread)
{
    audio_utils::lock_guard _l(mutex());
    const size_t frameCount =
            3 * sourceFramesNeeded(mSampleRate, thread->frameCount(), thread->sampleRate());
    AttributionSourceState attributionSource = AttributionSourceState();
    attributionSource.uid = VALUE_OR_FATAL(legacy2aidl_uid_t_int32_t(
        IPCThreadState::self()->getCallingUid()));
    attributionSource.pid = VALUE_OR_FATAL(legacy2aidl_pid_t_int32_t(
      IPCThreadState::self()->getCallingPid()));
    attributionSource.token = sp<BBinder>::make();
    sp<IAfOutputTrack> outputTrack = IAfOutputTrack::create(thread,
                                            this,
                                            mSampleRate,
                                            mFormat,
                                            mChannelMask,
                                            frameCount,
                                            attributionSource);
    status_t status = outputTrack != 0 ? outputTrack->initCheck() : (status_t) NO_MEMORY;
    if (status != NO_ERROR) {
        ALOGE("addOutputTrack() initCheck failed %d", status);
        return;
    }
    thread->setStreamVolume(AUDIO_STREAM_PATCH, 1.0f);
    mOutputTracks.add(outputTrack);
    ALOGV("addOutputTrack() track %p, on thread %p", outputTrack.get(), thread);
    updateWaitTime_l();
}
void DuplicatingThread::removeOutputTrack(IAfPlaybackThread* thread)
{
    audio_utils::lock_guard _l(mutex());
    for (size_t i = 0; i < mOutputTracks.size(); i++) {
        if (mOutputTracks[i]->thread() == thread) {
            mOutputTracks[i]->destroy();
            mOutputTracks.removeAt(i);
            updateWaitTime_l();
            if (thread->getOutput() == mOutput) {
                mOutput = NULL;
            }
            return;
        }
    }
    ALOGV("removeOutputTrack(): unknown thread: %p", thread);
}
void DuplicatingThread::updateWaitTime_l()
{
    mWaitTimeMs = UINT_MAX;
    for (size_t i = 0; i < mOutputTracks.size(); i++) {
        const auto strong = mOutputTracks[i]->thread().promote();
        if (strong != 0) {
            uint32_t waitTimeMs = (strong->frameCount() * 2 * 1000) / strong->sampleRate();
            if (waitTimeMs < mWaitTimeMs) {
                mWaitTimeMs = waitTimeMs;
            }
        }
    }
}
bool DuplicatingThread::outputsReady()
{
    for (size_t i = 0; i < outputTracks.size(); i++) {
        const auto thread = outputTracks[i]->thread().promote();
        if (thread == 0) {
            ALOGW("DuplicatingThread::outputsReady() could not promote thread on output track %p",
                    outputTracks[i].get());
            return false;
        }
        IAfPlaybackThread* const playbackThread = thread->asIAfPlaybackThread().get();
        if (playbackThread->inStandby() && !playbackThread->isSuspended()) {
            ALOGV("DuplicatingThread output track %p on thread %p Not Ready", outputTracks[i].get(),
                    thread.get());
            return false;
        }
    }
    return true;
}
void DuplicatingThread::sendMetadataToBackend_l(
        const StreamOutHalInterface::SourceMetadata& metadata)
{
    for (auto& outputTrack : outputTracks) {
        outputTrack->setMetadatas(metadata.tracks);
    }
}
uint32_t DuplicatingThread::activeSleepTimeUs() const
{
    return (mWaitTimeMs * 1000) / 2;
}
void DuplicatingThread::cacheParameters_l()
{
    updateWaitTime_l();
    MixerThread::cacheParameters_l();
}
sp<IAfPlaybackThread> IAfPlaybackThread::createSpatializerThread(
        const sp<IAfThreadCallback>& afThreadCallback,
        AudioStreamOut* output,
        audio_io_handle_t id,
        bool systemReady,
        audio_config_base_t* mixerConfig) {
    return sp<SpatializerThread>::make(afThreadCallback, output, id, systemReady, mixerConfig);
}
SpatializerThread::SpatializerThread(const sp<IAfThreadCallback>& afThreadCallback,
                                                             AudioStreamOut* output,
                                                             audio_io_handle_t id,
                                                             bool systemReady,
                                                             audio_config_base_t *mixerConfig)
    : MixerThread(afThreadCallback, output, id, systemReady, SPATIALIZER, mixerConfig)
{
}
void SpatializerThread::setHalLatencyMode_l() {
    if (mSupportedLatencyModes.empty()) {
        return;
    }
    audio_latency_mode_t latencyMode = AUDIO_LATENCY_MODE_FREE;
    if (mSupportedLatencyModes.size() == 1) {
        latencyMode = mSupportedLatencyModes[0];
    } else if (mSupportedLatencyModes.size() > 1) {
        bool hasSpatializedActiveTrack = false;
        for (const auto& track : mActiveTracks) {
            if (track->isSpatialized()) {
                hasSpatializedActiveTrack = true;
                break;
            }
        }
        if (hasSpatializedActiveTrack && mRequestedLatencyMode == AUDIO_LATENCY_MODE_LOW) {
            latencyMode = AUDIO_LATENCY_MODE_LOW;
        }
    }
    if (latencyMode != mSetLatencyMode) {
        status_t status = mOutput->stream->setLatencyMode(latencyMode);
        ALOGD("%s: thread(%d) setLatencyMode(%s) returned %d",
                __func__, mId, toString(latencyMode).c_str(), status);
        if (status == NO_ERROR) {
            mSetLatencyMode = latencyMode;
        }
    }
}
status_t SpatializerThread::setRequestedLatencyMode(audio_latency_mode_t mode) {
    if (mode != AUDIO_LATENCY_MODE_LOW && mode != AUDIO_LATENCY_MODE_FREE) {
        return BAD_VALUE;
    }
    audio_utils::lock_guard _l(mutex());
    mRequestedLatencyMode = mode;
    return NO_ERROR;
}
void SpatializerThread::checkOutputStageEffects()
NO_THREAD_SAFETY_ANALYSIS
{
    bool hasVirtualizer = false;
    bool hasDownMixer = false;
    sp<IAfEffectHandle> finalDownMixer;
    {
        audio_utils::lock_guard _l(mutex());
        sp<IAfEffectChain> chain = getEffectChain_l(AUDIO_SESSION_OUTPUT_STAGE);
        if (chain != 0) {
            hasVirtualizer = chain->getEffectFromType_l(FX_IID_SPATIALIZER) != nullptr;
            hasDownMixer = chain->getEffectFromType_l(EFFECT_UIID_DOWNMIX) != nullptr;
        }
        finalDownMixer = mFinalDownMixer;
        mFinalDownMixer.clear();
    }
    if (hasVirtualizer) {
        if (finalDownMixer != nullptr) {
            int32_t ret;
            finalDownMixer->asIEffect()->disable(&ret);
        }
        finalDownMixer.clear();
    } else if (!hasDownMixer) {
        std::vector<effect_descriptor_t> descriptors;
        status_t status = mAfThreadCallback->getEffectsFactoryHal()->getDescriptors(
                                                        EFFECT_UIID_DOWNMIX, &descriptors);
        if (status != NO_ERROR) {
            return;
        }
        ALOG_ASSERT(!descriptors.empty(),
                "%s getDescriptors() returned no error but empty list", __func__);
        finalDownMixer = createEffect_l(nullptr , nullptr ,
                0 , AUDIO_SESSION_OUTPUT_STAGE, &descriptors[0], nullptr ,
                &status, false , false , false );
        if (finalDownMixer == nullptr || (status != NO_ERROR && status != ALREADY_EXISTS)) {
            ALOGW("%s error creating downmixer %d", __func__, status);
            finalDownMixer.clear();
        } else {
            int32_t ret;
            finalDownMixer->asIEffect()->enable(&ret);
        }
    }
    {
        audio_utils::lock_guard _l(mutex());
        mFinalDownMixer = finalDownMixer;
    }
}
sp<IAfRecordThread> IAfRecordThread::create(const sp<IAfThreadCallback>& afThreadCallback,
        AudioStreamIn* input,
        audio_io_handle_t id,
        bool systemReady) {
    return sp<RecordThread>::make(afThreadCallback, input, id, systemReady);
}
RecordThread::RecordThread(const sp<IAfThreadCallback>& afThreadCallback,
                                         AudioStreamIn *input,
                                         audio_io_handle_t id,
                                         bool systemReady
                                         ) :
    ThreadBase(afThreadCallback, id, RECORD, systemReady, false ),
    mInput(input),
    mSource(mInput),
    mActiveTracks(&this->mLocalLog),
    mRsmpInBuffer(NULL),
    mRsmpInRear(0)
    , mReadOnlyHeap(new MemoryDealer(kRecordThreadReadOnlyHeapSize,
            "RecordThreadRO", MemoryHeapBase::READ_ONLY))
    , mFastCaptureFutex(0)
    , mPipeFramesP2(0)
    , mFastTrackAvail(false)
    , mBtNrecSuspended(false)
{
    snprintf(mThreadName, kThreadNameLength, "AudioIn_%X", id);
    mNBLogWriter = afThreadCallback->newWriter_l(kLogSize, mThreadName);
    if (mInput->audioHwDev != nullptr) {
        mIsMsdDevice = strcmp(
                mInput->audioHwDev->moduleName(), AUDIO_HARDWARE_MODULE_ID_MSD) == 0;
    }
    readInputParameters_l();
    mTimestampCorrectedDevice = (audio_devices_t)property_get_int64(
            "audio.timestamp.corrected_input_device",
            (int64_t)(mIsMsdDevice ? AUDIO_DEVICE_IN_BUS
                                   : AUDIO_DEVICE_NONE));
    mInputSource = new AudioStreamInSource(input->stream);
    size_t numCounterOffers = 0;
    const NBAIO_Format offers[1] = {Format_from_SR_C(mSampleRate, mChannelCount, mFormat)};
#if !LOG_NDEBUG
    [[maybe_unused]] ssize_t index =
#else
    (void)
#endif
            mInputSource->negotiate(offers, 1, NULL, numCounterOffers);
    ALOG_ASSERT(index == 0);
    bool initFastCapture;
    switch (kUseFastCapture) {
    case FastCapture_Never:
        initFastCapture = false;
        ALOGV("%p kUseFastCapture = Never, initFastCapture = false", this);
        break;
    case FastCapture_Always:
        initFastCapture = true;
        ALOGV("%p kUseFastCapture = Always, initFastCapture = true", this);
        break;
    case FastCapture_Static:
        initFastCapture = !mIsMsdDevice
                && (mFrameCount * 1000) / mSampleRate < kMinNormalCaptureBufferSizeMs;
        ALOGV("%p kUseFastCapture = Static, (%lld * 1000) / %u vs %u, initFastCapture = %d "
                "mIsMsdDevice = %d", this, (long long)mFrameCount, mSampleRate,
                kMinNormalCaptureBufferSizeMs, initFastCapture, mIsMsdDevice);
        break;
    }
    if (initFastCapture) {
        NBAIO_Format format = mInputSource->format();
        size_t pipeFramesP2 = roundup(4 * FMS_20 * mSampleRate / 1000);
        size_t pipeSize = pipeFramesP2 * Format_frameSize(format);
        void *pipeBuffer = nullptr;
        const sp<MemoryDealer> roHeap(readOnlyHeap());
        sp<IMemory> pipeMemory;
        if ((roHeap == 0) ||
                (pipeMemory = roHeap->allocate(pipeSize)) == 0 ||
                (pipeBuffer = pipeMemory->unsecurePointer()) == nullptr) {
            ALOGE("not enough memory for pipe buffer size=%zu; "
                    "roHeap=%p, pipeMemory=%p, pipeBuffer=%p; roHeapSize: %lld",
                    pipeSize, roHeap.get(), pipeMemory.get(), pipeBuffer,
                    (long long)kRecordThreadReadOnlyHeapSize);
            goto failed;
        }
        memset(pipeBuffer, 0, pipeSize);
        Pipe *pipe = new Pipe(pipeFramesP2, format, pipeBuffer);
        const NBAIO_Format offersFast[1] = {format};
        size_t numCounterOffersFast = 0;
        [[maybe_unused]] ssize_t index2 = pipe->negotiate(offersFast, std::size(offersFast),
                nullptr , numCounterOffersFast);
        ALOG_ASSERT(index2 == 0);
        mPipeSink = pipe;
        PipeReader *pipeReader = new PipeReader(*pipe);
        numCounterOffersFast = 0;
        index2 = pipeReader->negotiate(offersFast, std::size(offersFast),
                nullptr , numCounterOffersFast);
        ALOG_ASSERT(index2 == 0);
        mPipeSource = pipeReader;
        mPipeFramesP2 = pipeFramesP2;
        mPipeMemory = pipeMemory;
        mFastCapture = new FastCapture();
        FastCaptureStateQueue *sq = mFastCapture->sq();
#ifdef STATE_QUEUE_DUMP
#endif
        FastCaptureState *state = sq->begin();
        state->mCblk = NULL;
        state->mInputSource = mInputSource.get();
        state->mInputSourceGen++;
        state->mPipeSink = pipe;
        state->mPipeSinkGen++;
        state->mFrameCount = mFrameCount;
        state->mCommand = FastCaptureState::COLD_IDLE;
        state->mColdFutexAddr = &mFastCaptureFutex;
        state->mColdGen++;
        state->mDumpState = &mFastCaptureDumpState;
#ifdef TEE_SINK
#endif
        mFastCaptureNBLogWriter =
                afThreadCallback->newWriter_l(kFastCaptureLogSize, "FastCapture");
        state->mNBLogWriter = mFastCaptureNBLogWriter.get();
        sq->end();
        sq->push(FastCaptureStateQueue::BLOCK_UNTIL_PUSHED);
        mFastCapture->run("FastCapture", ANDROID_PRIORITY_URGENT_AUDIO);
        pid_t tid = mFastCapture->getTid();
        sendPrioConfigEvent(getpid(), tid, kPriorityFastCapture, false );
        stream()->setHalThreadPriority(kPriorityFastCapture);
#ifdef AUDIO_WATCHDOG
#endif
        mFastTrackAvail = true;
    }
#ifdef TEE_SINK
    mTee.set(mInputSource->format(), NBAIO_Tee::TEE_FLAG_INPUT_THREAD);
    mTee.setId(std::string("_") + std::to_string(mId) + "_C");
#endif
failed: ;
}
RecordThread::~RecordThread()
{
    if (mFastCapture != 0) {
        FastCaptureStateQueue *sq = mFastCapture->sq();
        FastCaptureState *state = sq->begin();
        if (state->mCommand == FastCaptureState::COLD_IDLE) {
            int32_t old = android_atomic_inc(&mFastCaptureFutex);
            if (old == -1) {
                (void) syscall(__NR_futex, &mFastCaptureFutex, FUTEX_WAKE_PRIVATE, 1);
            }
        }
        state->mCommand = FastCaptureState::EXIT;
        sq->end();
        sq->push(FastCaptureStateQueue::BLOCK_UNTIL_PUSHED);
        mFastCapture->join();
        mFastCapture.clear();
    }
    mAfThreadCallback->unregisterWriter(mFastCaptureNBLogWriter);
    mAfThreadCallback->unregisterWriter(mNBLogWriter);
    free(mRsmpInBuffer);
}
void RecordThread::onFirstRef()
{
    run(mThreadName, PRIORITY_URGENT_AUDIO);
}
void RecordThread::preExit()
{
    ALOGV("  preExit()");
    audio_utils::lock_guard _l(mutex());
    for (size_t i = 0; i < mTracks.size(); i++) {
        sp<IAfRecordTrack> track = mTracks[i];
        track->invalidate();
    }
    mActiveTracks.clear();
    mStartStopCV.notify_all();
}
bool RecordThread::threadLoop()
{
    nsecs_t lastWarning = 0;
    inputStandBy();
reacquire_wakelock:
    sp<IAfRecordTrack> activeTrack;
    {
        audio_utils::lock_guard _l(mutex());
        acquireWakeLock_l();
    }
    uint32_t sleepUs = 0;
    int64_t lastLoopCountRead = -2;
    for (int64_t loopCount = 0;; ++loopCount) {
        Vector<sp<IAfEffectChain>> effectChains;
        Vector<sp<IAfRecordTrack>> activeTracks;
        sp<IAfRecordTrack> fastTrack;
        sp<IAfRecordTrack> fastTrackToRemove;
        bool silenceFastCapture = false;
        {
            audio_utils::unique_lock _l(mutex());
            processConfigEvents_l();
            if (exitPending()) {
                break;
            }
            if (sleepUs > 0) {
                ATRACE_BEGIN("sleepC");
                (void)mWaitWorkCV.wait_for(_l, std::chrono::microseconds(sleepUs));
                ATRACE_END();
                sleepUs = 0;
                continue;
            }
            size_t size = mActiveTracks.size();
            if (size == 0) {
                standbyIfNotAlreadyInStandby();
                releaseWakeLock_l();
                ALOGV("RecordThread: loop stopping");
                mWaitWorkCV.wait(_l);
                ALOGV("RecordThread: loop starting");
                goto reacquire_wakelock;
            }
            bool doBroadcast = false;
            bool allStopped = true;
            for (size_t i = 0; i < size; ) {
                activeTrack = mActiveTracks[i];
                if (activeTrack->isTerminated()) {
                    if (activeTrack->isFastTrack()) {
                        ALOG_ASSERT(fastTrackToRemove == 0);
                        fastTrackToRemove = activeTrack;
                    }
                    removeTrack_l(activeTrack);
                    mActiveTracks.remove(activeTrack);
                    size--;
                    continue;
                }
                IAfTrackBase::track_state activeTrackState = activeTrack->state();
                switch (activeTrackState) {
                case IAfTrackBase::PAUSING:
                    mActiveTracks.remove(activeTrack);
                    activeTrack->setState(IAfTrackBase::PAUSED);
                    doBroadcast = true;
                    size--;
                    continue;
                case IAfTrackBase::STARTING_1:
                    sleepUs = 10000;
                    i++;
                    allStopped = false;
                    continue;
                case IAfTrackBase::STARTING_2:
                    doBroadcast = true;
                    if (mStandby) {
                        mThreadMetrics.logBeginInterval();
                        mThreadSnapshot.onBegin();
                        mStandby = false;
                    }
                    activeTrack->setState(IAfTrackBase::ACTIVE);
                    allStopped = false;
                    break;
                case IAfTrackBase::ACTIVE:
                    allStopped = false;
                    break;
                case IAfTrackBase::IDLE:
                case IAfTrackBase::PAUSED:
                case IAfTrackBase::STOPPED:
                default:
                    LOG_ALWAYS_FATAL("%s: Unexpected active track state:%d, id:%d, tracks:%zu",
                            __func__, activeTrackState, activeTrack->id(), size);
                }
                if (activeTrack->isFastTrack()) {
                    ALOG_ASSERT(!mFastTrackAvail);
                    ALOG_ASSERT(fastTrack == 0);
                    bool invalidate = false;
                    if (activeTrack->isSilenced()) {
                        if (size > 1) {
                            invalidate = true;
                        } else {
                            silenceFastCapture = true;
                        }
                    }
                    if (mMaxSharedAudioHistoryMs != 0) {
                        invalidate = true;
                    }
                    if (invalidate) {
                        activeTrack->invalidate();
                        ALOG_ASSERT(fastTrackToRemove == 0);
                        fastTrackToRemove = activeTrack;
                        removeTrack_l(activeTrack);
                        mActiveTracks.remove(activeTrack);
                        size--;
                        continue;
                    }
                    fastTrack = activeTrack;
                }
                activeTracks.add(activeTrack);
                i++;
            }
            mActiveTracks.updatePowerState(this);
            updateMetadata_l();
            if (allStopped) {
                standbyIfNotAlreadyInStandby();
            }
            if (doBroadcast) {
                mStartStopCV.notify_all();
            }
            if (activeTracks.isEmpty()) {
                if (sleepUs == 0) {
                    sleepUs = kRecordThreadSleepUs;
                }
                continue;
            }
            sleepUs = 0;
            lockEffectChains_l(effectChains);
        }
        size_t size = effectChains.size();
        for (size_t i = 0; i < size; i++) {
            effectChains[i]->process_l();
        }
        if (mFastCapture != 0) {
            FastCaptureStateQueue *sq = mFastCapture->sq();
            FastCaptureState *state = sq->begin();
            bool didModify = false;
            FastCaptureStateQueue::block_t block = FastCaptureStateQueue::BLOCK_UNTIL_PUSHED;
            if (state->mCommand != FastCaptureState::READ_WRITE ) {
                if (state->mCommand == FastCaptureState::COLD_IDLE) {
                    int32_t old = android_atomic_inc(&mFastCaptureFutex);
                    if (old == -1) {
                        (void) syscall(__NR_futex, &mFastCaptureFutex, FUTEX_WAKE_PRIVATE, 1);
                    }
                }
                state->mCommand = FastCaptureState::READ_WRITE;
#if 0
                mFastCaptureDumpState.increaseSamplingN(mAfThreadCallback->isLowRamDevice() ?
                        FastThreadDumpState::kSamplingNforLowRamDevice :
                        FastThreadDumpState::kSamplingN);
#endif
                didModify = true;
            }
            audio_track_cblk_t *cblkOld = state->mCblk;
            audio_track_cblk_t *cblkNew = fastTrack != 0 ? fastTrack->cblk() : NULL;
            if (cblkNew != cblkOld) {
                state->mCblk = cblkNew;
                if (cblkOld != NULL) {
                    block = FastCaptureStateQueue::BLOCK_UNTIL_ACKED;
                }
                didModify = true;
            }
            AudioBufferProvider* abp = (fastTrack != 0 && fastTrack->isPatchTrack()) ?
                    reinterpret_cast<AudioBufferProvider*>(fastTrack.get()) : nullptr;
            if (state->mFastPatchRecordBufferProvider != abp) {
                state->mFastPatchRecordBufferProvider = abp;
                state->mFastPatchRecordFormat = fastTrack == 0 ?
                        AUDIO_FORMAT_INVALID : fastTrack->format();
                didModify = true;
            }
            if (state->mSilenceCapture != silenceFastCapture) {
                state->mSilenceCapture = silenceFastCapture;
                didModify = true;
            }
            sq->end(didModify);
            if (didModify) {
                sq->push(block);
#if 0
                if (kUseFastCapture == FastCapture_Dynamic) {
                    mNormalSource = mPipeSource;
                }
#endif
            }
        }
        fastTrackToRemove.clear();
        int32_t rear = mRsmpInRear & (mRsmpInFramesP2 - 1);
        ssize_t framesRead = 0;
        const int64_t lastIoBeginNs = systemTime();
        if (mPipeSource != 0) {
            size_t framesToRead = min(mRsmpInFramesOA - rear, mRsmpInFramesP2 / 2);
            for (int retries = 0; retries <= 2; ++retries) {
                ALOGW_IF(retries > 0, "overrun on read from pipe, retry #%d", retries);
                framesRead = mPipeSource->read((uint8_t*)mRsmpInBuffer + rear * mFrameSize,
                        framesToRead);
                if (framesRead != OVERRUN) break;
            }
            const ssize_t availableToRead = mPipeSource->availableToRead();
            if (availableToRead >= 0) {
                mMonopipePipeDepthStats.add(availableToRead);
                LOG_ALWAYS_FATAL_IF((size_t)availableToRead > mPipeFramesP2,
                        "more frames to read than fifo size, %zd > %zu",
                        availableToRead, mPipeFramesP2);
                const size_t pipeFramesFree = mPipeFramesP2 - availableToRead;
                const size_t sleepFrames = min(pipeFramesFree, mRsmpInFramesP2) / 2;
                ALOGVV("mPipeFramesP2:%zu mRsmpInFramesP2:%zu sleepFrames:%zu availableToRead:%zd",
                        mPipeFramesP2, mRsmpInFramesP2, sleepFrames, availableToRead);
                sleepUs = (sleepFrames * 1000000LL) / mSampleRate;
            }
            if (framesRead < 0) {
                status_t status = (status_t) framesRead;
                switch (status) {
                case OVERRUN:
                    ALOGW("overrun on read from pipe");
                    framesRead = 0;
                    break;
                case NEGOTIATE:
                    ALOGE("re-negotiation is needed");
                    framesRead = -1;
                    break;
                default:
                    ALOGE("unknown error %d on read from pipe", status);
                    break;
                }
            }
        } else {
            ATRACE_BEGIN("read");
            size_t bytesRead;
            status_t result = mSource->read(
                    (uint8_t*)mRsmpInBuffer + rear * mFrameSize, mBufferSize, &bytesRead);
            ATRACE_END();
            if (result < 0) {
                framesRead = result;
            } else {
                framesRead = bytesRead / mFrameSize;
            }
        }
        const int64_t lastIoEndNs = systemTime();
        if (framesRead >= 0) {
            mTimestamp.mPosition[ExtendedTimestamp::LOCATION_SERVER] += framesRead;
            mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_SERVER] = lastIoEndNs;
        }
        if (mPipeSource.get() == nullptr ) {
            int64_t position, time;
            if (mStandby) {
                mTimestampVerifier.discontinuity(audio_is_linear_pcm(mFormat) ?
                    mTimestampVerifier.DISCONTINUITY_MODE_CONTINUOUS :
                    mTimestampVerifier.DISCONTINUITY_MODE_ZERO);
            } else if (mSource->getCapturePosition(&position, &time) == NO_ERROR
                    && time > mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL]) {
                mTimestampVerifier.add(position, time, mSampleRate);
                if (isTimestampCorrectionEnabled()) {
                    ALOGVV("TS_BEFORE: %d %lld %lld",
                            id(), (long long)time, (long long)position);
                    auto correctedTimestamp = mTimestampVerifier.getLastCorrectedTimestamp();
                    position = correctedTimestamp.mFrames;
                    time = correctedTimestamp.mTimeNs;
                    ALOGVV("TS_AFTER: %d %lld %lld",
                            id(), (long long)time, (long long)position);
                }
                mTimestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL] = position;
                mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL] = time;
            } else {
                mTimestampVerifier.error();
            }
        }
        const audio_input_flags_t flags = mInput != NULL ? mInput->flags : AUDIO_INPUT_FLAG_NONE;
        const double latencyMs = IAfRecordTrack::checkServerLatencySupported(mFormat, flags)
                ? - mTimestamp.getOutputServerLatencyMs(mSampleRate) : 0.;
        if (latencyMs != 0.) {
            mLatencyMs.add(latencyMs);
        }
        if (framesRead < 0 || (framesRead == 0 && mPipeSource == 0)) {
            ALOGE("read failed: framesRead=%zd", framesRead);
            inputStandBy();
            sleepUs = kRecordThreadSleepUs;
        }
        if (framesRead <= 0) {
            goto unlock;
        }
        ALOG_ASSERT(framesRead > 0);
        mFramesRead += framesRead;
#ifdef TEE_SINK
        (void)mTee.write((uint8_t*)mRsmpInBuffer + rear * mFrameSize, framesRead);
#endif
        {
            size_t part1 = mRsmpInFramesP2 - rear;
            if ((size_t) framesRead > part1) {
                memcpy(mRsmpInBuffer, (uint8_t*)mRsmpInBuffer + mRsmpInFramesP2 * mFrameSize,
                        (framesRead - part1) * mFrameSize);
            }
        }
        mRsmpInRear = audio_utils::safe_add_overflow(mRsmpInRear, (int32_t)framesRead);
        size = activeTracks.size();
        for (size_t i = 0; i < size; i++) {
            activeTrack = activeTracks[i];
            if (activeTrack->isFastTrack()) {
                continue;
            }
            enum {
                OVERRUN_UNKNOWN,
                OVERRUN_TRUE,
                OVERRUN_FALSE
            } overrun = OVERRUN_UNKNOWN;
            for (;;) {
                activeTrack->sinkBuffer().frameCount = ~0;
                status_t status = activeTrack->getNextBuffer(&activeTrack->sinkBuffer());
                size_t framesOut = activeTrack->sinkBuffer().frameCount;
                LOG_ALWAYS_FATAL_IF((status == OK) != (framesOut > 0));
                bool hasOverrun;
                size_t framesIn;
                activeTrack->resamplerBufferProvider()->sync(&framesIn, &hasOverrun);
                if (hasOverrun) {
                    overrun = OVERRUN_TRUE;
                }
                if (framesOut == 0 || framesIn == 0) {
                    break;
                }
                framesOut = min(framesOut,
                        destinationFramesPossible(
                                framesIn, mSampleRate, activeTrack->sampleRate()));
                if (activeTrack->isDirect()) {
                    AudioBufferProvider::Buffer buffer;
                    buffer.frameCount = framesOut;
                    const status_t getNextBufferStatus =
                            activeTrack->resamplerBufferProvider()->getNextBuffer(&buffer);
                    if (getNextBufferStatus == OK && buffer.frameCount != 0) {
                        ALOGV_IF(buffer.frameCount != framesOut,
                                "%s() read less than expected (%zu vs %zu)",
                                __func__, buffer.frameCount, framesOut);
                        framesOut = buffer.frameCount;
                        memcpy(activeTrack->sinkBuffer().raw,
                                buffer.raw, buffer.frameCount * mFrameSize);
                        activeTrack->resamplerBufferProvider()->releaseBuffer(&buffer);
                    } else {
                        framesOut = 0;
                        ALOGE("%s() cannot fill request, status: %d, frameCount: %zu",
                            __func__, getNextBufferStatus, buffer.frameCount);
                    }
                } else {
                    framesOut = activeTrack->recordBufferConverter()->convert(
                            activeTrack->sinkBuffer().raw,
                            activeTrack->resamplerBufferProvider(),
                            framesOut);
                }
                if (framesOut > 0 && (overrun == OVERRUN_UNKNOWN)) {
                    overrun = OVERRUN_FALSE;
                }
                const ssize_t framesToDrop =
                        activeTrack->synchronizedRecordState().updateRecordFrames(framesOut);
                if (framesToDrop == 0) {
                    if (framesOut > 0) {
                        activeTrack->sinkBuffer().frameCount = framesOut;
                        if (activeTrack->isSilenced()) {
                            memset(activeTrack->sinkBuffer().raw,
                                    0, framesOut * activeTrack->frameSize());
                        }
                        activeTrack->releaseBuffer(&activeTrack->sinkBuffer());
                    }
                }
                if (framesOut == 0) {
                    break;
                }
            }
            switch (overrun) {
            case OVERRUN_TRUE:
                if (!activeTrack->setOverflow()) {
                    nsecs_t now = systemTime();
                    if ((now - lastWarning) > kWarningThrottleNs) {
                        ALOGW("RecordThread: buffer overflow");
                        lastWarning = now;
                    }
                }
                break;
            case OVERRUN_FALSE:
                activeTrack->clearOverflow();
                break;
            case OVERRUN_UNKNOWN:
                break;
            }
            activeTrack->updateTrackFrameInfo(
                    activeTrack->serverProxy()->framesReleased(),
                    mTimestamp.mPosition[ExtendedTimestamp::LOCATION_SERVER],
                    mSampleRate, mTimestamp);
        }
unlock:
        unlockEffectChains(effectChains);
        if (audio_has_proportional_frames(mFormat)
            && loopCount == lastLoopCountRead + 1) {
            const int64_t readPeriodNs = lastIoEndNs - mLastIoEndNs;
            const double jitterMs =
                TimestampVerifier<int64_t, int64_t>::computeJitterMs(
                    {framesRead, readPeriodNs},
                    {0, 0} , mSampleRate);
            const double processMs = (lastIoBeginNs - mLastIoEndNs) * 1e-6;
            audio_utils::lock_guard _l(mutex());
            mIoJitterMs.add(jitterMs);
            mProcessTimeMs.add(processMs);
        }
        mLastIoBeginNs = lastIoBeginNs;
        mLastIoEndNs = lastIoEndNs;
        lastLoopCountRead = loopCount;
    }
    standbyIfNotAlreadyInStandby();
    {
        audio_utils::lock_guard _l(mutex());
        for (size_t i = 0; i < mTracks.size(); i++) {
            sp<IAfRecordTrack> track = mTracks[i];
            track->invalidate();
        }
        mActiveTracks.clear();
        mStartStopCV.notify_all();
    }
    releaseWakeLock();
    ALOGV("RecordThread %p exiting", this);
    return false;
}
void RecordThread::standbyIfNotAlreadyInStandby()
{
    if (!mStandby) {
        inputStandBy();
        mThreadMetrics.logEndInterval();
        mThreadSnapshot.onEnd();
        mStandby = true;
    }
}
void RecordThread::inputStandBy()
{
    if (mFastCapture != 0) {
        FastCaptureStateQueue *sq = mFastCapture->sq();
        FastCaptureState *state = sq->begin();
        if (!(state->mCommand & FastCaptureState::IDLE)) {
            state->mCommand = FastCaptureState::COLD_IDLE;
            state->mColdFutexAddr = &mFastCaptureFutex;
            state->mColdGen++;
            mFastCaptureFutex = 0;
            sq->end();
            sq->push(FastCaptureStateQueue::BLOCK_UNTIL_ACKED);
#if 0
            if (kUseFastCapture == FastCapture_Dynamic) {
            }
#endif
#ifdef AUDIO_WATCHDOG
#endif
        } else {
            sq->end(false );
        }
    }
    status_t result = mSource->standby();
    ALOGE_IF(result != OK, "Error when putting input stream into standby: %d", result);
    if (mPipeSource.get() != nullptr) {
        const ssize_t flushed = mPipeSource->flush();
        if (flushed > 0) {
            ALOGV("Input standby flushed PipeSource %zd frames", flushed);
            mTimestamp.mPosition[ExtendedTimestamp::LOCATION_SERVER] += flushed;
            mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_SERVER] = systemTime();
        }
    }
}
sp<IAfRecordTrack> RecordThread::createRecordTrack_l(
        const sp<Client>& client,
        const audio_attributes_t& attr,
        uint32_t *pSampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        size_t *pFrameCount,
        audio_session_t sessionId,
        size_t *pNotificationFrameCount,
        pid_t creatorPid,
        const AttributionSourceState& attributionSource,
        audio_input_flags_t *flags,
        pid_t tid,
        status_t *status,
        audio_port_handle_t portId,
        int32_t maxSharedAudioHistoryMs)
{
    size_t frameCount = *pFrameCount;
    size_t notificationFrameCount = *pNotificationFrameCount;
    sp<IAfRecordTrack> track;
    status_t lStatus;
    audio_input_flags_t inputFlags = mInput->flags;
    audio_input_flags_t requestedFlags = *flags;
    uint32_t sampleRate;
    lStatus = initCheck();
    if (lStatus != NO_ERROR) {
        ALOGE("createRecordTrack_l() audio driver not initialized");
        goto Exit;
    }
    if (!audio_is_linear_pcm(mFormat) && (*flags & AUDIO_INPUT_FLAG_DIRECT) == 0) {
        ALOGE("createRecordTrack_l() on an encoded stream requires AUDIO_INPUT_FLAG_DIRECT");
        lStatus = BAD_VALUE;
        goto Exit;
    }
    if (maxSharedAudioHistoryMs != 0) {
        if (!captureHotwordAllowed(attributionSource)) {
            lStatus = PERMISSION_DENIED;
            goto Exit;
        }
        if (maxSharedAudioHistoryMs < 0
                || maxSharedAudioHistoryMs > kMaxSharedAudioHistoryMs) {
            lStatus = BAD_VALUE;
            goto Exit;
        }
    }
    if (*pSampleRate == 0) {
        *pSampleRate = mSampleRate;
    }
    sampleRate = *pSampleRate;
    if (hasFastCapture() && mMaxSharedAudioHistoryMs == 0) {
        inputFlags = (audio_input_flags_t)(inputFlags | AUDIO_INPUT_FLAG_FAST);
    }
    if ((*flags & inputFlags) != *flags) {
        ALOGW("createRecordTrack_l(): mismatch between requested flags (%08x) and"
                " input flags (%08x)",
              *flags, inputFlags);
        *flags = (audio_input_flags_t)(*flags & inputFlags);
    }
    if (*flags & AUDIO_INPUT_FLAG_FAST && maxSharedAudioHistoryMs == 0) {
      if (
            (frameCount <= mPipeFramesP2) &&
            audio_is_linear_pcm(format) &&
            (channelMask == mChannelMask) &&
            (sampleRate == mSampleRate) &&
            hasFastCapture() &&
            mFastTrackAvail
        ) {
          audio_utils::lock_guard _l(mutex());
          sp<IAfEffectChain> chain = getEffectChain_l(sessionId);
          if (chain != 0) {
              audio_input_flags_t old = *flags;
              chain->checkInputFlagCompatibility(flags);
              if (old != *flags) {
                  ALOGV("%p AUDIO_INPUT_FLAGS denied by effect old=%#x new=%#x",
                          this, (int)old, (int)*flags);
              }
          }
          ALOGV_IF((*flags & AUDIO_INPUT_FLAG_FAST) != 0,
                   "%p AUDIO_INPUT_FLAG_FAST accepted: frameCount=%zu mFrameCount=%zu",
                   this, frameCount, mFrameCount);
      } else {
        ALOGV("%p AUDIO_INPUT_FLAG_FAST denied: frameCount=%zu mFrameCount=%zu mPipeFramesP2=%zu "
                "format=%#x isLinear=%d mFormat=%#x channelMask=%#x sampleRate=%u mSampleRate=%u "
                "hasFastCapture=%d tid=%d mFastTrackAvail=%d",
                this, frameCount, mFrameCount, mPipeFramesP2,
                format, audio_is_linear_pcm(format), mFormat, channelMask, sampleRate, mSampleRate,
                hasFastCapture(), tid, mFastTrackAvail);
        *flags = (audio_input_flags_t)(*flags & ~AUDIO_INPUT_FLAG_FAST);
      }
    }
    if ((*flags & AUDIO_INPUT_FLAG_FAST) !=
            (requestedFlags & AUDIO_INPUT_FLAG_FAST)) {
        *flags = (audio_input_flags_t) (*flags & ~(AUDIO_INPUT_FLAG_FAST | AUDIO_INPUT_FLAG_RAW));
        lStatus = BAD_TYPE;
        goto Exit;
    }
    if (*flags & AUDIO_INPUT_FLAG_FAST) {
        frameCount = mPipeFramesP2;
        notificationFrameCount = mFrameCount;
    } else {
        size_t maxNotificationFrames = ((int64_t) (hasFastCapture() ? mSampleRate/50 : mFrameCount)
                * sampleRate + mSampleRate - 1) / mSampleRate;
        static const size_t kMinNotifications = 3;
        static const uint32_t kMinMs = 30;
        const size_t minFramesByMs = (sampleRate * kMinMs + 1000 - 1) / 1000;
        const size_t minNotificationsByMs = (minFramesByMs + maxNotificationFrames - 1) /
                maxNotificationFrames;
        const size_t minFrameCount = maxNotificationFrames *
                max(kMinNotifications, minNotificationsByMs);
        frameCount = max(frameCount, minFrameCount);
        if (notificationFrameCount == 0 || notificationFrameCount > maxNotificationFrames) {
            notificationFrameCount = maxNotificationFrames;
        }
    }
    *pFrameCount = frameCount;
    *pNotificationFrameCount = notificationFrameCount;
    {
        audio_utils::lock_guard _l(mutex());
        int32_t startFrames = -1;
        if (!mSharedAudioPackageName.empty()
                && mSharedAudioPackageName == attributionSource.packageName
                && mSharedAudioSessionId == sessionId
                && captureHotwordAllowed(attributionSource)) {
            startFrames = mSharedAudioStartFrames;
        }
        track = IAfRecordTrack::create(this, client, attr, sampleRate,
                      format, channelMask, frameCount,
                      nullptr , (size_t)0 , sessionId, creatorPid,
                      attributionSource, *flags, IAfTrackBase::TYPE_DEFAULT, portId,
                      startFrames);
        lStatus = track->initCheck();
        if (lStatus != NO_ERROR) {
            ALOGE("createRecordTrack_l() initCheck failed %d; no control block?", lStatus);
            goto Exit;
        }
        mTracks.add(track);
        if ((*flags & AUDIO_INPUT_FLAG_FAST) && (tid != -1)) {
            pid_t callingPid = IPCThreadState::self()->getCallingPid();
            sendPrioConfigEvent_l(callingPid, tid, kPriorityAudioApp, true );
        }
        if (maxSharedAudioHistoryMs != 0) {
            sendResizeBufferConfigEvent_l(maxSharedAudioHistoryMs);
        }
    }
    lStatus = NO_ERROR;
Exit:
    *status = lStatus;
    return track;
}
status_t RecordThread::start(IAfRecordTrack* recordTrack,
                                           AudioSystem::sync_event_t event,
                                           audio_session_t triggerSession)
{
    ALOGV("RecordThread::start event %d, triggerSession %d", event, triggerSession);
    sp<ThreadBase> strongMe = this;
    status_t status = NO_ERROR;
    if (event == AudioSystem::SYNC_EVENT_NONE) {
        recordTrack->clearSyncStartEvent();
    } else if (event != AudioSystem::SYNC_EVENT_SAME) {
        recordTrack->synchronizedRecordState().startRecording(
                mAfThreadCallback->createSyncEvent(
                        event, triggerSession,
                        recordTrack->sessionId(), syncStartEventCallback, recordTrack));
    }
    {
         audio_utils::lock_guard lock(mutex());
        if (recordTrack->isInvalid()) {
            recordTrack->clearSyncStartEvent();
            ALOGW("%s track %d: invalidated before startInput", __func__, recordTrack->portId());
            return DEAD_OBJECT;
        }
        if (mActiveTracks.indexOf(recordTrack) >= 0) {
            if (recordTrack->state() == IAfTrackBase::PAUSING) {
                ALOGV("active record track PAUSING -> ACTIVE");
                recordTrack->setState(IAfTrackBase::ACTIVE);
            } else {
                ALOGV("active record track state %d", (int)recordTrack->state());
            }
            return status;
        }
        recordTrack->setState(IAfTrackBase::STARTING_1);
        mActiveTracks.add(recordTrack);
        if (recordTrack->isExternalTrack()) {
            mutex().unlock();
            status = AudioSystem::startInput(recordTrack->portId());
            mutex().lock();
            if (recordTrack->isInvalid()) {
                recordTrack->clearSyncStartEvent();
                if (status == NO_ERROR && recordTrack->state() == IAfTrackBase::STARTING_1) {
                    recordTrack->setState(IAfTrackBase::STARTING_2);
                }
                ALOGW("%s track %d: invalidated after startInput", __func__, recordTrack->portId());
                return DEAD_OBJECT;
            }
            if (recordTrack->state() != IAfTrackBase::STARTING_1) {
                ALOGW("%s(%d): unsynchronized mState:%d change",
                    __func__, recordTrack->id(), (int)recordTrack->state());
                recordTrack->clearSyncStartEvent();
                return INVALID_OPERATION;
            }
            if (status != NO_ERROR) {
                ALOGW("%s(%d): startInput failed, status %d",
                    __func__, recordTrack->id(), status);
                mActiveTracks.remove(recordTrack);
                recordTrack->clearSyncStartEvent();
                return status;
            }
            sendIoConfigEvent_l(
                AUDIO_CLIENT_STARTED, recordTrack->creatorPid(), recordTrack->portId());
        }
        recordTrack->logBeginInterval(patchSourcesToString(&mPatch));
        recordTrack->resamplerBufferProvider()->reset();
        if (!recordTrack->isDirect()) {
            recordTrack->recordBufferConverter()->reset();
        }
        recordTrack->setState(IAfTrackBase::STARTING_2);
        mWaitWorkCV.notify_all();
        return status;
    }
}
void RecordThread::syncStartEventCallback(const wp<SyncEvent>& event)
{
    const sp<SyncEvent> strongEvent = event.promote();
    if (strongEvent != 0) {
        sp<IAfTrackBase> ptr =
                std::any_cast<const wp<IAfTrackBase>>(strongEvent->cookie()).promote();
        if (ptr != nullptr) {
            ptr->handleSyncStartEvent(strongEvent);
        }
    }
}
bool RecordThread::stop(IAfRecordTrack* recordTrack) {
    ALOGV("RecordThread::stop");
    audio_utils::unique_lock _l(mutex());
    if (mActiveTracks.indexOf(recordTrack) < 0 || recordTrack->state() == IAfTrackBase::PAUSING) {
        return false;
    }
    recordTrack->setState(IAfTrackBase::PAUSING);
    while (recordTrack->state() == IAfTrackBase::PAUSING && !recordTrack->isInvalid()) {
        mWaitWorkCV.notify_all();
        mStartStopCV.wait(_l);
    }
    if (recordTrack->state() == IAfTrackBase::PAUSED) {
        ALOGV("Record stopped OK");
        return true;
    }
    ALOGW_IF("%s(%d): unsynchronized stop, state: %d",
            __func__, recordTrack->id(), recordTrack->state());
    return false;
}
bool RecordThread::isValidSyncEvent(const sp<SyncEvent>& ) const
{
    return false;
}
status_t RecordThread::setSyncEvent(const sp<SyncEvent>& )
{
#if 0
    if (!isValidSyncEvent(event)) {
        return BAD_VALUE;
    }
    audio_session_t eventSession = event->triggerSession();
    status_t ret = NAME_NOT_FOUND;
    audio_utils::lock_guard _l(mutex());
    for (size_t i = 0; i < mTracks.size(); i++) {
        sp<IAfRecordTrack> track = mTracks[i];
        if (eventSession == track->sessionId()) {
            (void) track->setSyncEvent(event);
            ret = NO_ERROR;
        }
    }
    return ret;
#else
    return BAD_VALUE;
#endif
}
status_t RecordThread::getActiveMicrophones(
        std::vector<media::MicrophoneInfoFw>* activeMicrophones) const
{
    ALOGV("RecordThread::getActiveMicrophones");
     audio_utils::lock_guard _l(mutex());
    if (!isStreamInitialized()) {
        return NO_INIT;
    }
    status_t status = mInput->stream->getActiveMicrophones(activeMicrophones);
    return status;
}
status_t RecordThread::setPreferredMicrophoneDirection(
            audio_microphone_direction_t direction)
{
    ALOGV("setPreferredMicrophoneDirection(%d)", direction);
     audio_utils::lock_guard _l(mutex());
    if (!isStreamInitialized()) {
        return NO_INIT;
    }
    return mInput->stream->setPreferredMicrophoneDirection(direction);
}
status_t RecordThread::setPreferredMicrophoneFieldDimension(float zoom)
{
    ALOGV("setPreferredMicrophoneFieldDimension(%f)", zoom);
     audio_utils::lock_guard _l(mutex());
    if (!isStreamInitialized()) {
        return NO_INIT;
    }
    return mInput->stream->setPreferredMicrophoneFieldDimension(zoom);
}
status_t RecordThread::shareAudioHistory(
        const std::string& sharedAudioPackageName, audio_session_t sharedSessionId,
        int64_t sharedAudioStartMs) {
     audio_utils::lock_guard _l(mutex());
    return shareAudioHistory_l(sharedAudioPackageName, sharedSessionId, sharedAudioStartMs);
}
status_t RecordThread::shareAudioHistory_l(
        const std::string& sharedAudioPackageName, audio_session_t sharedSessionId,
        int64_t sharedAudioStartMs) {
    if ((hasAudioSession_l(sharedSessionId) & ThreadBase::TRACK_SESSION) == 0) {
        return BAD_VALUE;
    }
    if (sharedAudioStartMs < 0
        || sharedAudioStartMs > INT64_MAX / mSampleRate) {
        return BAD_VALUE;
    }
    int64_t sharedAudioStartFrames = sharedAudioStartMs * mSampleRate / 1000;
    const int32_t sharedOffset = audio_utils::safe_sub_overflow(mRsmpInRear,
          (int32_t)sharedAudioStartFrames);
    if (sharedOffset < 0) {
        sharedAudioStartFrames = mRsmpInRear;
    } else if (sharedOffset > static_cast<signed>(mRsmpInFrames)) {
        sharedAudioStartFrames =
                audio_utils::safe_sub_overflow(mRsmpInRear, (int32_t)mRsmpInFrames);
    }
    mSharedAudioPackageName = sharedAudioPackageName;
    if (mSharedAudioPackageName.empty()) {
        resetAudioHistory_l();
    } else {
        mSharedAudioSessionId = sharedSessionId;
        mSharedAudioStartFrames = (int32_t)sharedAudioStartFrames;
    }
    return NO_ERROR;
}
void RecordThread::resetAudioHistory_l() {
    mSharedAudioSessionId = AUDIO_SESSION_NONE;
    mSharedAudioStartFrames = -1;
    mSharedAudioPackageName = "";
}
ThreadBase::MetadataUpdate RecordThread::updateMetadata_l()
{
    if (!isStreamInitialized() || !mActiveTracks.readAndClearHasChanged()) {
        return {};
    }
    StreamInHalInterface::SinkMetadata metadata;
    auto backInserter = std::back_inserter(metadata.tracks);
    for (const sp<IAfRecordTrack>& track : mActiveTracks) {
        track->copyMetadataTo(backInserter);
    }
    mInput->stream->updateSinkMetadata(metadata);
    MetadataUpdate change;
    change.recordMetadataUpdate = metadata.tracks;
    return change;
}
void RecordThread::destroyTrack_l(const sp<IAfRecordTrack>& track)
{
    track->terminate();
    track->setState(IAfTrackBase::STOPPED);
    if (mActiveTracks.indexOf(track) < 0) {
        removeTrack_l(track);
    }
}
void RecordThread::removeTrack_l(const sp<IAfRecordTrack>& track)
{
    String8 result;
    track->appendDump(result, false );
    mLocalLog.log("removeTrack_l (%p) %s", track.get(), result.c_str());
    mTracks.remove(track);
    if (track->isFastTrack()) {
        ALOG_ASSERT(!mFastTrackAvail);
        mFastTrackAvail = true;
    }
}
void RecordThread::dumpInternals_l(int fd, const Vector<String16>& )
{
    AudioStreamIn *input = mInput;
    audio_input_flags_t flags = input != NULL ? input->flags : AUDIO_INPUT_FLAG_NONE;
    dprintf(fd, "  AudioStreamIn: %p flags %#x (%s)\n",
            input, flags, toString(flags).c_str());
    dprintf(fd, "  Frames read: %lld\n", (long long)mFramesRead);
    if (mActiveTracks.isEmpty()) {
        dprintf(fd, "  No active record clients\n");
    }
    if (input != nullptr) {
        dprintf(fd, "  Hal stream dump:\n");
        (void)input->stream->dump(fd);
    }
    dprintf(fd, "  Fast capture thread: %s\n", hasFastCapture() ? "yes" : "no");
    dprintf(fd, "  Fast track available: %s\n", mFastTrackAvail ? "yes" : "no");
    const std::unique_ptr<FastCaptureDumpState> copy =
            std::make_unique<FastCaptureDumpState>(mFastCaptureDumpState);
    copy->dump(fd);
}
void RecordThread::dumpTracks_l(int fd, const Vector<String16>& )
{
    String8 result;
    size_t numtracks = mTracks.size();
    size_t numactive = mActiveTracks.size();
    size_t numactiveseen = 0;
    dprintf(fd, "  %zu Tracks", numtracks);
    const char *prefix = "    ";
    if (numtracks) {
        dprintf(fd, " of which %zu are active\n", numactive);
        result.append(prefix);
        mTracks[0]->appendDumpHeader(result);
        for (size_t i = 0; i < numtracks ; ++i) {
            sp<IAfRecordTrack> track = mTracks[i];
            if (track != 0) {
                bool active = mActiveTracks.indexOf(track) >= 0;
                if (active) {
                    numactiveseen++;
                }
                result.append(prefix);
                track->appendDump(result, active);
            }
        }
    } else {
        dprintf(fd, "\n");
    }
    if (numactiveseen != numactive) {
        result.append("  The following tracks are in the active list but"
                " not in the track list\n");
        result.append(prefix);
        mActiveTracks[0]->appendDumpHeader(result);
        for (size_t i = 0; i < numactive; ++i) {
            sp<IAfRecordTrack> track = mActiveTracks[i];
            if (mTracks.indexOf(track) < 0) {
                result.append(prefix);
                track->appendDump(result, true );
            }
        }
    }
    write(fd, result.c_str(), result.size());
}
void RecordThread::setRecordSilenced(audio_port_handle_t portId, bool silenced)
{
    audio_utils::lock_guard _l(mutex());
    for (size_t i = 0; i < mTracks.size() ; i++) {
        sp<IAfRecordTrack> track = mTracks[i];
        if (track != 0 && track->portId() == portId) {
            track->setSilenced(silenced);
        }
    }
}
void ResamplerBufferProvider::reset()
{
    const auto threadBase = mRecordTrack->thread().promote();
    auto* const recordThread = static_cast<RecordThread *>(threadBase->asIAfRecordThread().get());
    mRsmpInUnrel = 0;
    const int32_t rear = recordThread->mRsmpInRear;
    ssize_t deltaFrames = 0;
    if (mRecordTrack->startFrames() >= 0) {
        int32_t startFrames = mRecordTrack->startFrames();
        if (startFrames <= rear) {
            deltaFrames = rear - startFrames;
        } else {
            deltaFrames = (int32_t)((int64_t)rear + UINT32_MAX + 1 - startFrames);
        }
        if ((size_t) deltaFrames > recordThread->mRsmpInFrames) {
            deltaFrames = recordThread->mRsmpInFrames;
        }
    }
    mRsmpInFront = audio_utils::safe_sub_overflow(rear, static_cast<int32_t>(deltaFrames));
}
void ResamplerBufferProvider::sync(
        size_t *framesAvailable, bool *hasOverrun)
{
    const auto threadBase = mRecordTrack->thread().promote();
    auto* const recordThread = static_cast<RecordThread *>(threadBase->asIAfRecordThread().get());
    const int32_t rear = recordThread->mRsmpInRear;
    const int32_t front = mRsmpInFront;
    const ssize_t filled = audio_utils::safe_sub_overflow(rear, front);
    size_t framesIn;
    bool overrun = false;
    if (filled < 0) {
        framesIn = 0;
        mRsmpInFront = rear;
        overrun = true;
    } else if ((size_t) filled <= recordThread->mRsmpInFrames) {
        framesIn = (size_t) filled;
    } else {
        framesIn = recordThread->mRsmpInFrames;
        mRsmpInFront = audio_utils::safe_sub_overflow(
                rear, static_cast<int32_t>(framesIn));
        overrun = true;
    }
    if (framesAvailable != NULL) {
        *framesAvailable = framesIn;
    }
    if (hasOverrun != NULL) {
        *hasOverrun = overrun;
    }
}
status_t ResamplerBufferProvider::getNextBuffer(
        AudioBufferProvider::Buffer* buffer)
{
    const auto threadBase = mRecordTrack->thread().promote();
    if (threadBase == 0) {
        buffer->frameCount = 0;
        buffer->raw = NULL;
        return NOT_ENOUGH_DATA;
    }
    auto* const recordThread = static_cast<RecordThread *>(threadBase->asIAfRecordThread().get());
    int32_t rear = recordThread->mRsmpInRear;
    int32_t front = mRsmpInFront;
    ssize_t filled = audio_utils::safe_sub_overflow(rear, front);
    LOG_ALWAYS_FATAL_IF(!(0 <= filled && (size_t) filled <= recordThread->mRsmpInFrames));
    front &= recordThread->mRsmpInFramesP2 - 1;
    size_t part1 = recordThread->mRsmpInFramesP2 - front;
    if (part1 > (size_t) filled) {
        part1 = filled;
    }
    size_t ask = buffer->frameCount;
    ALOG_ASSERT(ask > 0);
    if (part1 > ask) {
        part1 = ask;
    }
    if (part1 == 0) {
        buffer->raw = NULL;
        buffer->frameCount = 0;
        mRsmpInUnrel = 0;
        return NOT_ENOUGH_DATA;
    }
    buffer->raw = (uint8_t*)recordThread->mRsmpInBuffer + front * recordThread->mFrameSize;
    buffer->frameCount = part1;
    mRsmpInUnrel = part1;
    return NO_ERROR;
}
void ResamplerBufferProvider::releaseBuffer(
        AudioBufferProvider::Buffer* buffer)
{
    int32_t stepCount = static_cast<int32_t>(buffer->frameCount);
    if (stepCount == 0) {
        return;
    }
    ALOG_ASSERT(stepCount <= (int32_t)mRsmpInUnrel);
    mRsmpInUnrel -= stepCount;
    mRsmpInFront = audio_utils::safe_add_overflow(mRsmpInFront, stepCount);
    buffer->raw = NULL;
    buffer->frameCount = 0;
}
void RecordThread::checkBtNrec()
{
    audio_utils::lock_guard _l(mutex());
    checkBtNrec_l();
}
void RecordThread::checkBtNrec_l()
{
    bool suspend = audio_is_bluetooth_sco_device(inDeviceType()) &&
                        mAfThreadCallback->btNrecIsOff();
    if (mBtNrecSuspended.exchange(suspend) != suspend) {
        for (size_t i = 0; i < mEffectChains.size(); i++) {
            setEffectSuspended_l(FX_IID_AEC, suspend, mEffectChains[i]->sessionId());
            setEffectSuspended_l(FX_IID_NS, suspend, mEffectChains[i]->sessionId());
        }
    }
}
bool RecordThread::checkForNewParameter_l(const String8& keyValuePair,
                                                        status_t& status)
{
    bool reconfig = false;
    status = NO_ERROR;
    audio_format_t reqFormat = mFormat;
    uint32_t samplingRate = mSampleRate;
    [[maybe_unused]] audio_channel_mask_t channelMask =
                                audio_channel_in_mask_from_count(mChannelCount);
    AudioParameter param = AudioParameter(keyValuePair);
    int value;
    AutoPark<FastCapture> park(mFastCapture);
    if (param.getInt(String8(AudioParameter::keySamplingRate), value) == NO_ERROR) {
        samplingRate = value;
        reconfig = true;
    }
    if (param.getInt(String8(AudioParameter::keyFormat), value) == NO_ERROR) {
        if (!audio_is_linear_pcm((audio_format_t) value)) {
            status = BAD_VALUE;
        } else {
            reqFormat = (audio_format_t) value;
            reconfig = true;
        }
    }
    if (param.getInt(String8(AudioParameter::keyChannels), value) == NO_ERROR) {
        audio_channel_mask_t mask = (audio_channel_mask_t) value;
        if (!audio_is_input_channel(mask) ||
                audio_channel_count_from_in_mask(mask) > FCC_LIMIT) {
            status = BAD_VALUE;
        } else {
            channelMask = mask;
            reconfig = true;
        }
    }
    if (param.getInt(String8(AudioParameter::keyFrameCount), value) == NO_ERROR) {
        if (mActiveTracks.size() > 0) {
            status = INVALID_OPERATION;
        } else {
            reconfig = true;
        }
    }
    if (param.getInt(String8(AudioParameter::keyRouting), value) == NO_ERROR) {
        LOG_FATAL("Should not set routing device in RecordThread");
    }
    if (param.getInt(String8(AudioParameter::keyInputSource), value) == NO_ERROR &&
            mAudioSource != (audio_source_t)value) {
        LOG_FATAL("Should not set audio source in RecordThread");
    }
    if (status == NO_ERROR) {
        status = mInput->stream->setParameters(keyValuePair);
        if (status == INVALID_OPERATION) {
            inputStandBy();
            status = mInput->stream->setParameters(keyValuePair);
        }
        if (reconfig) {
            if (status == BAD_VALUE) {
                audio_config_base_t config = AUDIO_CONFIG_BASE_INITIALIZER;
                if (mInput->stream->getAudioProperties(&config) == OK &&
                        audio_is_linear_pcm(config.format) && audio_is_linear_pcm(reqFormat) &&
                        config.sample_rate <= (AUDIO_RESAMPLER_DOWN_RATIO_MAX * samplingRate) &&
                        audio_channel_count_from_in_mask(config.channel_mask) <= FCC_LIMIT) {
                    status = NO_ERROR;
                }
            }
            if (status == NO_ERROR) {
                readInputParameters_l();
                sendIoConfigEvent_l(AUDIO_INPUT_CONFIG_CHANGED);
            }
        }
    }
    return reconfig;
}
String8 RecordThread::getParameters(const String8& keys)
{
    audio_utils::lock_guard _l(mutex());
    if (initCheck() == NO_ERROR) {
        String8 out_s8;
        if (mInput->stream->getParameters(keys, &out_s8) == OK) {
            return out_s8;
        }
    }
    return {};
}
void RecordThread::ioConfigChanged(audio_io_config_event_t event, pid_t pid,
                                                 audio_port_handle_t portId) {
    sp<AudioIoDescriptor> desc;
    switch (event) {
    case AUDIO_INPUT_OPENED:
    case AUDIO_INPUT_REGISTERED:
    case AUDIO_INPUT_CONFIG_CHANGED:
        desc = sp<AudioIoDescriptor>::make(mId, mPatch, true ,
                mSampleRate, mFormat, mChannelMask, mFrameCount, mFrameCount);
        break;
    case AUDIO_CLIENT_STARTED:
        desc = sp<AudioIoDescriptor>::make(mId, mPatch, portId);
        break;
    case AUDIO_INPUT_CLOSED:
    default:
        desc = sp<AudioIoDescriptor>::make(mId);
        break;
    }
    mAfThreadCallback->ioConfigChanged(event, desc, pid);
}
void RecordThread::readInputParameters_l()
{
    status_t result = mInput->stream->getAudioProperties(&mSampleRate, &mChannelMask, &mHALFormat);
    LOG_ALWAYS_FATAL_IF(result != OK, "Error retrieving audio properties from HAL: %d", result);
    mFormat = mHALFormat;
    mChannelCount = audio_channel_count_from_in_mask(mChannelMask);
    if (audio_is_linear_pcm(mFormat)) {
        LOG_ALWAYS_FATAL_IF(mChannelCount > FCC_LIMIT, "HAL channel count %d > %d",
                mChannelCount, FCC_LIMIT);
    } else {
        ALOGI("HAL format %#x is not linear pcm", mFormat);
    }
    result = mInput->stream->getFrameSize(&mFrameSize);
    LOG_ALWAYS_FATAL_IF(result != OK, "Error retrieving frame size from HAL: %d", result);
    LOG_ALWAYS_FATAL_IF(mFrameSize <= 0, "Error frame size was %zu but must be greater than zero",
            mFrameSize);
    result = mInput->stream->getBufferSize(&mBufferSize);
    LOG_ALWAYS_FATAL_IF(result != OK, "Error retrieving buffer size from HAL: %d", result);
    mFrameCount = mBufferSize / mFrameSize;
    ALOGV("%p RecordThread params: mChannelCount=%u, mFormat=%#x, mFrameSize=%zu, "
            "mBufferSize=%zu, mFrameCount=%zu",
            this, mChannelCount, mFormat, mFrameSize, mBufferSize, mFrameCount);
    mRsmpInFrames = 0;
    resizeInputBuffer_l(0 );
    audio_input_flags_t flags = mInput->flags;
    mediametrics::LogItem item(mThreadMetrics.getMetricsId());
    item.set(AMEDIAMETRICS_PROP_EVENT, AMEDIAMETRICS_PROP_EVENT_VALUE_READPARAMETERS)
        .set(AMEDIAMETRICS_PROP_ENCODING, IAfThreadBase::formatToString(mFormat).c_str())
        .set(AMEDIAMETRICS_PROP_FLAGS, toString(flags).c_str())
        .set(AMEDIAMETRICS_PROP_SAMPLERATE, (int32_t)mSampleRate)
        .set(AMEDIAMETRICS_PROP_CHANNELMASK, (int32_t)mChannelMask)
        .set(AMEDIAMETRICS_PROP_CHANNELCOUNT, (int32_t)mChannelCount)
        .set(AMEDIAMETRICS_PROP_FRAMECOUNT, (int32_t)mFrameCount)
        .record();
}
uint32_t RecordThread::getInputFramesLost() const
{
    audio_utils::lock_guard _l(mutex());
    uint32_t result;
    if (initCheck() == NO_ERROR && mInput->stream->getInputFramesLost(&result) == OK) {
        return result;
    }
    return 0;
}
KeyedVector<audio_session_t, bool> RecordThread::sessionIds() const
{
    KeyedVector<audio_session_t, bool> ids;
    audio_utils::lock_guard _l(mutex());
    for (size_t j = 0; j < mTracks.size(); ++j) {
        sp<IAfRecordTrack> track = mTracks[j];
        audio_session_t sessionId = track->sessionId();
        if (ids.indexOfKey(sessionId) < 0) {
            ids.add(sessionId, true);
        }
    }
    return ids;
}
AudioStreamIn* RecordThread::clearInput()
{
    audio_utils::lock_guard _l(mutex());
    AudioStreamIn *input = mInput;
    mInput = NULL;
    mInputSource.clear();
    return input;
}
sp<StreamHalInterface> RecordThread::stream() const
{
    if (mInput == NULL) {
        return NULL;
    }
    return mInput->stream;
}
status_t RecordThread::addEffectChain_l(const sp<IAfEffectChain>& chain)
{
    ALOGV("addEffectChain_l() %p on thread %p", chain.get(), this);
    chain->setThread(this);
    chain->setInBuffer(NULL);
    chain->setOutBuffer(NULL);
    checkSuspendOnAddEffectChain_l(chain);
    chain->syncHalEffectsState();
    mEffectChains.add(chain);
    return NO_ERROR;
}
size_t RecordThread::removeEffectChain_l(const sp<IAfEffectChain>& chain)
{
    ALOGV("removeEffectChain_l() %p from thread %p", chain.get(), this);
    for (size_t i = 0; i < mEffectChains.size(); i++) {
        if (chain == mEffectChains[i]) {
            mEffectChains.removeAt(i);
            break;
        }
    }
    return mEffectChains.size();
}
status_t RecordThread::createAudioPatch_l(const struct audio_patch* patch,
                                                          audio_patch_handle_t *handle)
{
    status_t status = NO_ERROR;
    mInDeviceTypeAddr.mType = patch->sources[0].ext.device.type;
    mInDeviceTypeAddr.setAddress(patch->sources[0].ext.device.address);
    audio_port_handle_t deviceId = patch->sources[0].id;
    for (size_t i = 0; i < mEffectChains.size(); i++) {
        mEffectChains[i]->setInputDevice_l(inDeviceTypeAddr());
    }
    checkBtNrec_l();
    if (mAudioSource != patch->sinks[0].ext.mix.usecase.source) {
        mAudioSource = patch->sinks[0].ext.mix.usecase.source;
        for (size_t i = 0; i < mEffectChains.size(); i++) {
            mEffectChains[i]->setAudioSource_l(mAudioSource);
        }
    }
    if (mInput->audioHwDev->supportsAudioPatches()) {
        sp<DeviceHalInterface> hwDevice = mInput->audioHwDev->hwDevice();
        status = hwDevice->createAudioPatch(patch->num_sources,
                                            patch->sources,
                                            patch->num_sinks,
                                            patch->sinks,
                                            handle);
    } else {
        status = mInput->stream->legacyCreateAudioPatch(patch->sources[0],
                                                        patch->sinks[0].ext.mix.usecase.source,
                                                        patch->sources[0].ext.device.type);
        *handle = AUDIO_PATCH_HANDLE_NONE;
    }
    if ((mPatch.num_sources == 0) || (mPatch.sources[0].id != deviceId)) {
        sendIoConfigEvent_l(AUDIO_INPUT_CONFIG_CHANGED);
        mPatch = *patch;
    }
    const std::string pathSourcesAsString = patchSourcesToString(patch);
    mThreadMetrics.logEndInterval();
    mThreadMetrics.logCreatePatch(pathSourcesAsString, {});
    mThreadMetrics.logBeginInterval();
    for (const auto &track : mActiveTracks) {
        track->logEndInterval();
        track->logBeginInterval(pathSourcesAsString);
    }
    mActiveTracks.setHasChanged();
    return status;
}
status_t RecordThread::releaseAudioPatch_l(const audio_patch_handle_t handle)
{
    status_t status = NO_ERROR;
    mPatch = audio_patch{};
    mInDeviceTypeAddr.reset();
    if (mInput->audioHwDev->supportsAudioPatches()) {
        sp<DeviceHalInterface> hwDevice = mInput->audioHwDev->hwDevice();
        status = hwDevice->releaseAudioPatch(handle);
    } else {
        status = mInput->stream->legacyReleaseAudioPatch();
    }
    mActiveTracks.setHasChanged();
    return status;
}
void RecordThread::updateOutDevices(const DeviceDescriptorBaseVector& outDevices)
{
    audio_utils::lock_guard _l(mutex());
    mOutDevices = outDevices;
    mOutDeviceTypeAddrs = deviceTypeAddrsFromDescriptors(mOutDevices);
    for (size_t i = 0; i < mEffectChains.size(); i++) {
        mEffectChains[i]->setDevices_l(outDeviceTypeAddrs());
    }
}
int32_t RecordThread::getOldestFront_l()
{
    if (mTracks.size() == 0) {
        return mRsmpInRear;
    }
    int32_t oldestFront = mRsmpInRear;
    int32_t maxFilled = 0;
    for (size_t i = 0; i < mTracks.size(); i++) {
        int32_t front = mTracks[i]->resamplerBufferProvider()->getFront();
        int32_t filled;
        (void)__builtin_sub_overflow(mRsmpInRear, front, &filled);
        if (filled > maxFilled) {
            oldestFront = front;
            maxFilled = filled;
        }
    }
    if (maxFilled > static_cast<signed>(mRsmpInFrames)) {
        (void)__builtin_sub_overflow(mRsmpInRear, mRsmpInFrames, &oldestFront);
    }
    return oldestFront;
}
void RecordThread::updateFronts_l(int32_t offset)
{
    if (offset == 0) {
        return;
    }
    for (size_t i = 0; i < mTracks.size(); i++) {
        int32_t front = mTracks[i]->resamplerBufferProvider()->getFront();
        front = audio_utils::safe_sub_overflow(front, offset);
        mTracks[i]->resamplerBufferProvider()->setFront(front);
    }
}
void RecordThread::resizeInputBuffer_l(int32_t maxSharedAudioHistoryMs)
{
    size_t minRsmpInFrames = mFrameCount * 7;
    int32_t previousFront = getOldestFront_l();
    size_t previousRsmpInFramesP2 = mRsmpInFramesP2;
    int32_t previousRear = mRsmpInRear;
    mRsmpInRear = 0;
    ALOG_ASSERT(maxSharedAudioHistoryMs >= 0
            && maxSharedAudioHistoryMs <= kMaxSharedAudioHistoryMs,
            "resizeInputBuffer_l() called with invalid max shared history %d",
            maxSharedAudioHistoryMs);
    if (maxSharedAudioHistoryMs != 0) {
        ALOG_ASSERT(mRsmpInBuffer != nullptr && mRsmpInFrames != 0,
                "resizeInputBuffer_l() called with shared history and unallocated buffer");
        size_t rsmpInFrames = (size_t)maxSharedAudioHistoryMs * mSampleRate / 1000;
        if (rsmpInFrames <= mRsmpInFrames) {
            return;
        }
        mRsmpInFrames = rsmpInFrames;
    }
    mMaxSharedAudioHistoryMs = maxSharedAudioHistoryMs;
    if (mRsmpInFrames < minRsmpInFrames) {
        mRsmpInFrames = minRsmpInFrames;
    }
    mRsmpInFramesP2 = roundup(mRsmpInFrames);
    mRsmpInFramesOA = mRsmpInFramesP2 + mFrameCount - 1;
    void *rsmpInBuffer;
    (void)posix_memalign(&rsmpInBuffer, 32, mRsmpInFramesOA * mFrameSize);
    memset(rsmpInBuffer, 0, mRsmpInFramesOA * mFrameSize);
    if (previousRear != 0) {
        ALOG_ASSERT(mRsmpInBuffer != nullptr,
                "resizeInputBuffer_l() called with null buffer but frames already read from HAL");
        ssize_t unread = audio_utils::safe_sub_overflow(previousRear, previousFront);
        previousFront &= previousRsmpInFramesP2 - 1;
        size_t part1 = previousRsmpInFramesP2 - previousFront;
        if (part1 > (size_t) unread) {
            part1 = unread;
        }
        if (part1 != 0) {
            memcpy(rsmpInBuffer, (const uint8_t*)mRsmpInBuffer + previousFront * mFrameSize,
                   part1 * mFrameSize);
            mRsmpInRear = part1;
            part1 = unread - part1;
            if (part1 != 0) {
                memcpy((uint8_t*)rsmpInBuffer + mRsmpInRear * mFrameSize,
                       (const uint8_t*)mRsmpInBuffer, part1 * mFrameSize);
                mRsmpInRear += part1;
            }
        }
        updateFronts_l(audio_utils::safe_sub_overflow(previousRear, mRsmpInRear));
    } else {
        mRsmpInRear = 0;
    }
    free(mRsmpInBuffer);
    mRsmpInBuffer = rsmpInBuffer;
}
void RecordThread::addPatchTrack(const sp<IAfPatchRecord>& record)
{
    audio_utils::lock_guard _l(mutex());
    mTracks.add(record);
    if (record->getSource()) {
        mSource = record->getSource();
    }
}
void RecordThread::deletePatchTrack(const sp<IAfPatchRecord>& record)
{
    audio_utils::lock_guard _l(mutex());
    if (mSource == record->getSource()) {
        mSource = mInput;
    }
    destroyTrack_l(record);
}
void RecordThread::toAudioPortConfig(struct audio_port_config* config)
{
    ThreadBase::toAudioPortConfig(config);
    config->role = AUDIO_PORT_ROLE_SINK;
    config->ext.mix.hw_module = mInput->audioHwDev->handle();
    config->ext.mix.usecase.source = mAudioSource;
    if (mInput && mInput->flags != AUDIO_INPUT_FLAG_NONE) {
        config->config_mask |= AUDIO_PORT_CONFIG_FLAGS;
        config->flags.input = mInput->flags;
    }
}
class MmapThreadHandle : public MmapStreamInterface {
public:
    explicit MmapThreadHandle(const sp<IAfMmapThread>& thread);
    ~MmapThreadHandle() override;
    status_t createMmapBuffer(int32_t minSizeFrames,
        struct audio_mmap_buffer_info* info) final;
    status_t getMmapPosition(struct audio_mmap_position* position) final;
    status_t getExternalPosition(uint64_t* position, int64_t* timeNanos) final;
    status_t start(const AudioClient& client,
           const audio_attributes_t* attr, audio_port_handle_t* handle) final;
    status_t stop(audio_port_handle_t handle) final;
    status_t standby() final;
    status_t reportData(const void* buffer, size_t frameCount) final;
private:
    const sp<IAfMmapThread> mThread;
};
sp<MmapStreamInterface> IAfMmapThread::createMmapStreamInterfaceAdapter(
        const sp<IAfMmapThread>& mmapThread) {
    return sp<MmapThreadHandle>::make(mmapThread);
}
MmapThreadHandle::MmapThreadHandle(const sp<IAfMmapThread>& thread)
    : mThread(thread)
{
    assert(thread != 0);
}
MmapThreadHandle::~MmapThreadHandle()
{
    mThread->disconnect();
}
status_t MmapThreadHandle::createMmapBuffer(int32_t minSizeFrames,
                                  struct audio_mmap_buffer_info *info)
{
    return mThread->createMmapBuffer(minSizeFrames, info);
}
status_t MmapThreadHandle::getMmapPosition(struct audio_mmap_position* position)
{
    return mThread->getMmapPosition(position);
}
status_t MmapThreadHandle::getExternalPosition(uint64_t* position,
                                                             int64_t *timeNanos) {
    return mThread->getExternalPosition(position, timeNanos);
}
status_t MmapThreadHandle::start(const AudioClient& client,
        const audio_attributes_t *attr, audio_port_handle_t *handle)
{
    return mThread->start(client, attr, handle);
}
status_t MmapThreadHandle::stop(audio_port_handle_t handle)
{
    return mThread->stop(handle);
}
status_t MmapThreadHandle::standby()
{
    return mThread->standby();
}
status_t MmapThreadHandle::reportData(const void* buffer, size_t frameCount)
{
    return mThread->reportData(buffer, frameCount);
}
MmapThread::MmapThread(
        const sp<IAfThreadCallback>& afThreadCallback, audio_io_handle_t id,
        AudioHwDevice *hwDev, const sp<StreamHalInterface>& stream, bool systemReady, bool isOut)
    : ThreadBase(afThreadCallback, id, (isOut ? MMAP_PLAYBACK : MMAP_CAPTURE), systemReady, isOut),
      mSessionId(AUDIO_SESSION_NONE),
      mPortId(AUDIO_PORT_HANDLE_NONE),
      mHalStream(stream), mHalDevice(hwDev->hwDevice()), mAudioHwDev(hwDev),
      mActiveTracks(&this->mLocalLog),
      mHalVolFloat(-1.0f),
      mNoCallbackWarningCount(0)
{
    mStandby = true;
    readHalParameters_l();
}
void MmapThread::onFirstRef()
{
    run(mThreadName, ANDROID_PRIORITY_URGENT_AUDIO);
}
void MmapThread::disconnect()
{
    ActiveTracks<IAfMmapTrack> activeTracks;
    {
        audio_utils::lock_guard _l(mutex());
        for (const sp<IAfMmapTrack>& t : mActiveTracks) {
            activeTracks.add(t);
        }
    }
    for (const sp<IAfMmapTrack>& t : activeTracks) {
        stop(t->portId());
    }
    if (isOutput()) {
        AudioSystem::releaseOutput(mPortId);
    } else {
        AudioSystem::releaseInput(mPortId);
    }
}
void MmapThread::configure(const audio_attributes_t* attr,
                                                audio_stream_type_t streamType __unused,
                                                audio_session_t sessionId,
                                                const sp<MmapStreamCallback>& callback,
                                                audio_port_handle_t deviceId,
                                                audio_port_handle_t portId)
{
    mAttr = *attr;
    mSessionId = sessionId;
    mCallback = callback;
    mDeviceId = deviceId;
    mPortId = portId;
}
status_t MmapThread::createMmapBuffer(int32_t minSizeFrames,
                                  struct audio_mmap_buffer_info *info)
{
    if (mHalStream == 0) {
        return NO_INIT;
    }
    mStandby = true;
    return mHalStream->createMmapBuffer(minSizeFrames, info);
}
status_t MmapThread::getMmapPosition(struct audio_mmap_position* position) const
{
    if (mHalStream == 0) {
        return NO_INIT;
    }
    return mHalStream->getMmapPosition(position);
}
status_t MmapThread::exitStandby_l()
{
    updateMetadata_l();
    status_t ret = mHalStream->start();
    if (ret != NO_ERROR) {
        ALOGE("%s: error mHalStream->start() = %d for first track", __FUNCTION__, ret);
        return ret;
    }
    if (mStandby) {
        mThreadMetrics.logBeginInterval();
        mThreadSnapshot.onBegin();
        mStandby = false;
    }
    return NO_ERROR;
}
status_t MmapThread::start(const AudioClient& client,
                                         const audio_attributes_t *attr,
                                         audio_port_handle_t *handle)
{
    ALOGV("%s clientUid %d mStandby %d mPortId %d *handle %d", __FUNCTION__,
          client.attributionSource.uid, mStandby, mPortId, *handle);
    if (mHalStream == 0) {
        return NO_INIT;
    }
    status_t ret;
    if (*handle == mPortId) {
        acquireWakeLock();
        return NO_ERROR;
    }
    audio_port_handle_t portId = AUDIO_PORT_HANDLE_NONE;
    audio_io_handle_t io = mId;
    const AttributionSourceState adjAttributionSource = afutils::checkAttributionSourcePackage(
            client.attributionSource);
    if (isOutput()) {
        audio_config_t config = AUDIO_CONFIG_INITIALIZER;
        config.sample_rate = mSampleRate;
        config.channel_mask = mChannelMask;
        config.format = mFormat;
        audio_stream_type_t stream = streamType();
        audio_output_flags_t flags =
                (audio_output_flags_t)(AUDIO_OUTPUT_FLAG_MMAP_NOIRQ | AUDIO_OUTPUT_FLAG_DIRECT);
        audio_port_handle_t deviceId = mDeviceId;
        std::vector<audio_io_handle_t> secondaryOutputs;
        bool isSpatialized;
        bool isBitPerfect;
        ret = AudioSystem::getOutputForAttr(&mAttr, &io,
                                            mSessionId,
                                            &stream,
                                            adjAttributionSource,
                                            &config,
                                            flags,
                                            &deviceId,
                                            &portId,
                                            &secondaryOutputs,
                                            &isSpatialized,
                                            &isBitPerfect);
        ALOGD_IF(!secondaryOutputs.empty(),
                 "MmapThread::start does not support secondary outputs, ignoring them");
    } else {
        audio_config_base_t config;
        config.sample_rate = mSampleRate;
        config.channel_mask = mChannelMask;
        config.format = mFormat;
        audio_port_handle_t deviceId = mDeviceId;
        ret = AudioSystem::getInputForAttr(&mAttr, &io,
                                              RECORD_RIID_INVALID,
                                              mSessionId,
                                              adjAttributionSource,
                                              &config,
                                              AUDIO_INPUT_FLAG_MMAP_NOIRQ,
                                              &deviceId,
                                              &portId);
    }
    if (ret != NO_ERROR || io != mId) {
        ALOGE("%s: error getting output or input from APM (error %d, io %d expected io %d)",
              __FUNCTION__, ret, io, mId);
        return BAD_VALUE;
    }
    if (isOutput()) {
        ret = AudioSystem::startOutput(portId);
    } else {
        {
            audio_utils::lock_guard _l(mutex());
            setClientSilencedState_l(portId, false );
        }
        ret = AudioSystem::startInput(portId);
    }
    audio_utils::lock_guard _l(mutex());
    if (ret != NO_ERROR) {
        ALOGE("%s: error start rejected by AudioPolicyManager = %d", __FUNCTION__, ret);
        if (!mActiveTracks.isEmpty()) {
            mutex().unlock();
            if (isOutput()) {
                AudioSystem::releaseOutput(portId);
            } else {
                AudioSystem::releaseInput(portId);
            }
            mutex().lock();
        } else {
            mHalStream->stop();
        }
        eraseClientSilencedState_l(portId);
        return PERMISSION_DENIED;
    }
    sp<IAfMmapTrack> track = IAfMmapTrack::create(
            this, attr == nullptr ? mAttr : *attr, mSampleRate, mFormat,
                                        mChannelMask, mSessionId, isOutput(),
                                        client.attributionSource,
                                        IPCThreadState::self()->getCallingPid(), portId);
    if (!isOutput()) {
        track->setSilenced_l(isClientSilenced_l(portId));
    }
    if (isOutput()) {
        mHalVolFloat = -1.0f;
    } else if (!track->isSilenced_l()) {
        for (const sp<IAfMmapTrack>& t : mActiveTracks) {
            if (t->isSilenced_l()
                    && t->uid() != static_cast<uid_t>(client.attributionSource.uid)) {
                t->invalidate();
            }
        }
    }
    mActiveTracks.add(track);
    sp<IAfEffectChain> chain = getEffectChain_l(mSessionId);
    if (chain != 0) {
        chain->setStrategy(getStrategyForStream(streamType()));
        chain->incTrackCnt();
        chain->incActiveTrackCnt();
    }
    track->logBeginInterval(patchSinksToString(&mPatch));
    *handle = portId;
    if (mActiveTracks.size() == 1) {
        ret = exitStandby_l();
    }
    broadcast_l();
    ALOGV("%s DONE status %d handle %d stream %p", __FUNCTION__, ret, *handle, mHalStream.get());
    return ret;
}
status_t MmapThread::stop(audio_port_handle_t handle)
{
    ALOGV("%s handle %d", __FUNCTION__, handle);
    if (mHalStream == 0) {
        return NO_INIT;
    }
    if (handle == mPortId) {
        releaseWakeLock();
        return NO_ERROR;
    }
    audio_utils::lock_guard _l(mutex());
    sp<IAfMmapTrack> track;
    for (const sp<IAfMmapTrack>& t : mActiveTracks) {
        if (handle == t->portId()) {
            track = t;
            break;
        }
    }
    if (track == 0) {
        return BAD_VALUE;
    }
    mActiveTracks.remove(track);
    eraseClientSilencedState_l(track->portId());
    mutex().unlock();
    if (isOutput()) {
        AudioSystem::stopOutput(track->portId());
        AudioSystem::releaseOutput(track->portId());
    } else {
        AudioSystem::stopInput(track->portId());
        AudioSystem::releaseInput(track->portId());
    }
    mutex().lock();
    sp<IAfEffectChain> chain = getEffectChain_l(track->sessionId());
    if (chain != 0) {
        chain->decActiveTrackCnt();
        chain->decTrackCnt();
    }
    if (mActiveTracks.isEmpty()) {
        mHalStream->stop();
    }
    broadcast_l();
    return NO_ERROR;
}
status_t MmapThread::standby()
{
    ALOGV("%s", __FUNCTION__);
    if (mHalStream == 0) {
        return NO_INIT;
    }
    if (!mActiveTracks.isEmpty()) {
        return INVALID_OPERATION;
    }
    mHalStream->standby();
    if (!mStandby) {
        mThreadMetrics.logEndInterval();
        mThreadSnapshot.onEnd();
        mStandby = true;
    }
    releaseWakeLock();
    return NO_ERROR;
}
status_t MmapThread::reportData(const void* , size_t ) {
    return INVALID_OPERATION;
}
void MmapThread::readHalParameters_l()
{
    status_t result = mHalStream->getAudioProperties(&mSampleRate, &mChannelMask, &mHALFormat);
    LOG_ALWAYS_FATAL_IF(result != OK, "Error retrieving audio properties from HAL: %d", result);
    mFormat = mHALFormat;
    LOG_ALWAYS_FATAL_IF(!audio_is_linear_pcm(mFormat), "HAL format %#x is not linear pcm", mFormat);
    result = mHalStream->getFrameSize(&mFrameSize);
    LOG_ALWAYS_FATAL_IF(result != OK, "Error retrieving frame size from HAL: %d", result);
    LOG_ALWAYS_FATAL_IF(mFrameSize <= 0, "Error frame size was %zu but must be greater than zero",
            mFrameSize);
    result = mHalStream->getBufferSize(&mBufferSize);
    LOG_ALWAYS_FATAL_IF(result != OK, "Error retrieving buffer size from HAL: %d", result);
    mFrameCount = mBufferSize / mFrameSize;
    mediametrics::LogItem item(mThreadMetrics.getMetricsId());
    item.set(AMEDIAMETRICS_PROP_EVENT, AMEDIAMETRICS_PROP_EVENT_VALUE_READPARAMETERS)
        .set(AMEDIAMETRICS_PROP_ENCODING, IAfThreadBase::formatToString(mFormat).c_str())
        .set(AMEDIAMETRICS_PROP_SAMPLERATE, (int32_t)mSampleRate)
        .set(AMEDIAMETRICS_PROP_CHANNELMASK, (int32_t)mChannelMask)
        .set(AMEDIAMETRICS_PROP_CHANNELCOUNT, (int32_t)mChannelCount)
        .set(AMEDIAMETRICS_PROP_FRAMECOUNT, (int32_t)mFrameCount)
        .set(AMEDIAMETRICS_PROP_PREFIX_HAL AMEDIAMETRICS_PROP_ENCODING,
                IAfThreadBase::formatToString(mHALFormat).c_str())
        .set(AMEDIAMETRICS_PROP_PREFIX_HAL AMEDIAMETRICS_PROP_FRAMECOUNT,
                (int32_t)mFrameCount)
        .record();
}
bool MmapThread::threadLoop()
{
    checkSilentMode_l();
    const String8 myName(String8::format("thread %p type %d TID %d", this, mType, gettid()));
    while (!exitPending())
    {
        Vector<sp<IAfEffectChain>> effectChains;
        {
        audio_utils::unique_lock _l(mutex());
        if (mSignalPending) {
            mSignalPending = false;
        } else {
            if (mConfigEvents.isEmpty()) {
                IPCThreadState::self()->flushCommands();
                if (exitPending()) {
                    break;
                }
                ALOGV("%s going to sleep", myName.c_str());
                mWaitWorkCV.wait(_l);
                ALOGV("%s waking up", myName.c_str());
                checkSilentMode_l();
                continue;
            }
        }
        processConfigEvents_l();
        processVolume_l();
        checkInvalidTracks_l();
        mActiveTracks.updatePowerState(this);
        updateMetadata_l();
        lockEffectChains_l(effectChains);
        }
        for (size_t i = 0; i < effectChains.size(); i ++) {
            effectChains[i]->process_l();
        }
        unlockEffectChains(effectChains);
    }
    threadLoop_exit();
    if (!mStandby) {
        threadLoop_standby();
        mStandby = true;
    }
    ALOGV("Thread %p type %d exiting", this, mType);
    return false;
}
bool MmapThread::checkForNewParameter_l(const String8& keyValuePair,
                                                              status_t& status)
{
    AudioParameter param = AudioParameter(keyValuePair);
    int value;
    bool sendToHal = true;
    if (param.getInt(String8(AudioParameter::keyRouting), value) == NO_ERROR) {
        LOG_FATAL("Should not happen set routing device in MmapThread");
    }
    if (sendToHal) {
        status = mHalStream->setParameters(keyValuePair);
    } else {
        status = NO_ERROR;
    }
    return false;
}
String8 MmapThread::getParameters(const String8& keys)
{
    audio_utils::lock_guard _l(mutex());
    String8 out_s8;
    if (initCheck() == NO_ERROR && mHalStream->getParameters(keys, &out_s8) == OK) {
        return out_s8;
    }
    return {};
}
void MmapThread::ioConfigChanged(audio_io_config_event_t event, pid_t pid,
                                               audio_port_handle_t portId __unused) {
    sp<AudioIoDescriptor> desc;
    bool isInput = false;
    switch (event) {
    case AUDIO_INPUT_OPENED:
    case AUDIO_INPUT_REGISTERED:
    case AUDIO_INPUT_CONFIG_CHANGED:
        isInput = true;
        FALLTHROUGH_INTENDED;
    case AUDIO_OUTPUT_OPENED:
    case AUDIO_OUTPUT_REGISTERED:
    case AUDIO_OUTPUT_CONFIG_CHANGED:
        desc = sp<AudioIoDescriptor>::make(mId, mPatch, isInput,
                mSampleRate, mFormat, mChannelMask, mFrameCount, mFrameCount);
        break;
    case AUDIO_INPUT_CLOSED:
    case AUDIO_OUTPUT_CLOSED:
    default:
        desc = sp<AudioIoDescriptor>::make(mId);
        break;
    }
    mAfThreadCallback->ioConfigChanged(event, desc, pid);
}
status_t MmapThread::createAudioPatch_l(const struct audio_patch* patch,
                                                          audio_patch_handle_t *handle)
NO_THREAD_SAFETY_ANALYSIS
{
    status_t status = NO_ERROR;
    audio_devices_t type = AUDIO_DEVICE_NONE;
    audio_port_handle_t deviceId;
    AudioDeviceTypeAddrVector sinkDeviceTypeAddrs;
    AudioDeviceTypeAddr sourceDeviceTypeAddr;
    uint32_t numDevices = 0;
    if (isOutput()) {
        for (unsigned int i = 0; i < patch->num_sinks; i++) {
            LOG_ALWAYS_FATAL_IF(popcount(patch->sinks[i].ext.device.type) > 1
                                && !mAudioHwDev->supportsAudioPatches(),
                                "Enumerated device type(%#x) must not be used "
                                "as it does not support audio patches",
                                patch->sinks[i].ext.device.type);
            type = static_cast<audio_devices_t>(type | patch->sinks[i].ext.device.type);
            sinkDeviceTypeAddrs.emplace_back(patch->sinks[i].ext.device.type,
                    patch->sinks[i].ext.device.address);
        }
        deviceId = patch->sinks[0].id;
        numDevices = mPatch.num_sinks;
    } else {
        type = patch->sources[0].ext.device.type;
        deviceId = patch->sources[0].id;
        numDevices = mPatch.num_sources;
        sourceDeviceTypeAddr.mType = patch->sources[0].ext.device.type;
        sourceDeviceTypeAddr.setAddress(patch->sources[0].ext.device.address);
    }
    for (size_t i = 0; i < mEffectChains.size(); i++) {
        if (isOutput()) {
            mEffectChains[i]->setDevices_l(sinkDeviceTypeAddrs);
        } else {
            mEffectChains[i]->setInputDevice_l(sourceDeviceTypeAddr);
        }
    }
    if (!isOutput()) {
        if (mAudioSource != patch->sinks[0].ext.mix.usecase.source) {
            mAudioSource = patch->sinks[0].ext.mix.usecase.source;
            for (size_t i = 0; i < mEffectChains.size(); i++) {
                mEffectChains[i]->setAudioSource_l(mAudioSource);
            }
        }
    }
    if (mAudioHwDev->supportsAudioPatches()) {
        status = mHalDevice->createAudioPatch(patch->num_sources, patch->sources, patch->num_sinks,
                                              patch->sinks, handle);
    } else {
        audio_port_config port;
        std::optional<audio_source_t> source;
        if (isOutput()) {
            port = patch->sinks[0];
        } else {
            port = patch->sources[0];
            source = patch->sinks[0].ext.mix.usecase.source;
        }
        status = mHalStream->legacyCreateAudioPatch(port, source, type);
        *handle = AUDIO_PATCH_HANDLE_NONE;
    }
    if (numDevices == 0 || mDeviceId != deviceId) {
        if (isOutput()) {
            sendIoConfigEvent_l(AUDIO_OUTPUT_CONFIG_CHANGED);
            mOutDeviceTypeAddrs = sinkDeviceTypeAddrs;
            checkSilentMode_l();
        } else {
            sendIoConfigEvent_l(AUDIO_INPUT_CONFIG_CHANGED);
            mInDeviceTypeAddr = sourceDeviceTypeAddr;
        }
        sp<MmapStreamCallback> callback = mCallback.promote();
        if (mDeviceId != deviceId && callback != 0) {
            mutex().unlock();
            callback->onRoutingChanged(deviceId);
            mutex().lock();
        }
        mPatch = *patch;
        mDeviceId = deviceId;
    }
    mActiveTracks.setHasChanged();
    return status;
}
status_t MmapThread::releaseAudioPatch_l(const audio_patch_handle_t handle)
{
    status_t status = NO_ERROR;
    mPatch = audio_patch{};
    mOutDeviceTypeAddrs.clear();
    mInDeviceTypeAddr.reset();
    bool supportsAudioPatches = mHalDevice->supportsAudioPatches(&supportsAudioPatches) == OK ?
                                        supportsAudioPatches : false;
    if (supportsAudioPatches) {
        status = mHalDevice->releaseAudioPatch(handle);
    } else {
        status = mHalStream->legacyReleaseAudioPatch();
    }
    mActiveTracks.setHasChanged();
    return status;
}
void MmapThread::toAudioPortConfig(struct audio_port_config* config)
{
    ThreadBase::toAudioPortConfig(config);
    if (isOutput()) {
        config->role = AUDIO_PORT_ROLE_SOURCE;
        config->ext.mix.hw_module = mAudioHwDev->handle();
        config->ext.mix.usecase.stream = AUDIO_STREAM_DEFAULT;
    } else {
        config->role = AUDIO_PORT_ROLE_SINK;
        config->ext.mix.hw_module = mAudioHwDev->handle();
        config->ext.mix.usecase.source = mAudioSource;
    }
}
status_t MmapThread::addEffectChain_l(const sp<IAfEffectChain>& chain)
{
    audio_session_t session = chain->sessionId();
    ALOGV("addEffectChain_l() %p on thread %p for session %d", chain.get(), this, session);
    for (const sp<IAfMmapTrack>& track : mActiveTracks) {
        if (session == track->sessionId()) {
            chain->incTrackCnt();
            chain->incActiveTrackCnt();
        }
    }
    chain->setThread(this);
    chain->setInBuffer(nullptr);
    chain->setOutBuffer(nullptr);
    chain->syncHalEffectsState();
    mEffectChains.add(chain);
    checkSuspendOnAddEffectChain_l(chain);
    return NO_ERROR;
}
size_t MmapThread::removeEffectChain_l(const sp<IAfEffectChain>& chain)
{
    audio_session_t session = chain->sessionId();
    ALOGV("removeEffectChain_l() %p from thread %p for session %d", chain.get(), this, session);
    for (size_t i = 0; i < mEffectChains.size(); i++) {
        if (chain == mEffectChains[i]) {
            mEffectChains.removeAt(i);
            for (const sp<IAfMmapTrack>& track : mActiveTracks) {
                if (session == track->sessionId()) {
                    chain->decActiveTrackCnt();
                    chain->decTrackCnt();
                }
            }
            break;
        }
    }
    return mEffectChains.size();
}
void MmapThread::threadLoop_standby()
{
    mHalStream->standby();
}
void MmapThread::threadLoop_exit()
{
}
status_t MmapThread::setSyncEvent(const sp<SyncEvent>& )
{
    return BAD_VALUE;
}
bool MmapThread::isValidSyncEvent(
        const sp<SyncEvent>& ) const
{
    return false;
}
status_t MmapThread::checkEffectCompatibility_l(
        const effect_descriptor_t *desc, audio_session_t sessionId)
{
    if (audio_is_global_session(sessionId)) {
        ALOGW("checkEffectCompatibility_l(): global effect %s on MMAP thread %s",
                desc->name, mThreadName);
        return BAD_VALUE;
    }
    if (!isOutput() && ((desc->flags & EFFECT_FLAG_TYPE_MASK) != EFFECT_FLAG_TYPE_PRE_PROC)) {
        ALOGW("checkEffectCompatibility_l(): non pre processing effect %s on capture mmap thread",
                desc->name);
        return BAD_VALUE;
    }
    if (isOutput() && ((desc->flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_PRE_PROC)) {
        ALOGW("checkEffectCompatibility_l(): pre processing effect %s created on playback mmap "
              "thread", desc->name);
        return BAD_VALUE;
    }
    if ((desc->flags & EFFECT_FLAG_NO_PROCESS_MASK) != EFFECT_FLAG_NO_PROCESS) {
        return BAD_VALUE;
    }
    if (IAfEffectModule::isHapticGenerator(&desc->type)) {
        ALOGE("%s(): HapticGenerator is not supported for MmapThread", __func__);
        return BAD_VALUE;
    }
    return NO_ERROR;
}
void MmapThread::checkInvalidTracks_l()
NO_THREAD_SAFETY_ANALYSIS
{
    sp<MmapStreamCallback> callback;
    for (const sp<IAfMmapTrack>& track : mActiveTracks) {
        if (track->isInvalid()) {
            callback = mCallback.promote();
            if (callback == nullptr && mNoCallbackWarningCount < kMaxNoCallbackWarnings) {
                ALOGW("Could not notify MMAP stream tear down: no onRoutingChanged callback!");
                mNoCallbackWarningCount++;
            }
            break;
        }
    }
    if (callback != 0) {
        mutex().unlock();
        callback->onRoutingChanged(AUDIO_PORT_HANDLE_NONE);
        mutex().lock();
    }
}
void MmapThread::dumpInternals_l(int fd, const Vector<String16>& )
{
    dprintf(fd, "  Attributes: content type %d usage %d source %d\n",
            mAttr.content_type, mAttr.usage, mAttr.source);
    dprintf(fd, "  Session: %d port Id: %d\n", mSessionId, mPortId);
    if (mActiveTracks.isEmpty()) {
        dprintf(fd, "  No active clients\n");
    }
}
void MmapThread::dumpTracks_l(int fd, const Vector<String16>& )
{
    String8 result;
    size_t numtracks = mActiveTracks.size();
    dprintf(fd, "  %zu Tracks\n", numtracks);
    const char *prefix = "    ";
    if (numtracks) {
        result.append(prefix);
        mActiveTracks[0]->appendDumpHeader(result);
        for (size_t i = 0; i < numtracks ; ++i) {
            sp<IAfMmapTrack> track = mActiveTracks[i];
            result.append(prefix);
            track->appendDump(result, true );
        }
    } else {
        dprintf(fd, "\n");
    }
    write(fd, result.c_str(), result.size());
}
sp<IAfMmapPlaybackThread> IAfMmapPlaybackThread::create(
        const sp<IAfThreadCallback>& afThreadCallback, audio_io_handle_t id,
        AudioHwDevice* hwDev, AudioStreamOut* output, bool systemReady) {
    return sp<MmapPlaybackThread>::make(afThreadCallback, id, hwDev, output, systemReady);
}
MmapPlaybackThread::MmapPlaybackThread(
        const sp<IAfThreadCallback>& afThreadCallback, audio_io_handle_t id,
        AudioHwDevice *hwDev, AudioStreamOut *output, bool systemReady)
    : MmapThread(afThreadCallback, id, hwDev, output->stream, systemReady, true ),
      mStreamType(AUDIO_STREAM_MUSIC),
      mStreamVolume(1.0),
      mStreamMute(false),
      mOutput(output)
{
    snprintf(mThreadName, kThreadNameLength, "AudioMmapOut_%X", id);
    mChannelCount = audio_channel_count_from_out_mask(mChannelMask);
    mMasterVolume = afThreadCallback->masterVolume_l();
    mMasterMute = afThreadCallback->masterMute_l();
    if (mAudioHwDev) {
        if (mAudioHwDev->canSetMasterVolume()) {
            mMasterVolume = 1.0;
        }
        if (mAudioHwDev->canSetMasterMute()) {
            mMasterMute = false;
        }
    }
}
void MmapPlaybackThread::configure(const audio_attributes_t* attr,
                                                audio_stream_type_t streamType,
                                                audio_session_t sessionId,
                                                const sp<MmapStreamCallback>& callback,
                                                audio_port_handle_t deviceId,
                                                audio_port_handle_t portId)
{
    MmapThread::configure(attr, streamType, sessionId, callback, deviceId, portId);
    mStreamType = streamType;
}
AudioStreamOut* MmapPlaybackThread::clearOutput()
{
    audio_utils::lock_guard _l(mutex());
    AudioStreamOut *output = mOutput;
    mOutput = NULL;
    return output;
}
void MmapPlaybackThread::setMasterVolume(float value)
{
    audio_utils::lock_guard _l(mutex());
    if (mAudioHwDev &&
            mAudioHwDev->canSetMasterVolume()) {
        mMasterVolume = 1.0;
    } else {
        mMasterVolume = value;
    }
}
void MmapPlaybackThread::setMasterMute(bool muted)
{
    audio_utils::lock_guard _l(mutex());
    if (mAudioHwDev && mAudioHwDev->canSetMasterMute()) {
        mMasterMute = false;
    } else {
        mMasterMute = muted;
    }
}
void MmapPlaybackThread::setStreamVolume(audio_stream_type_t stream, float value)
{
    audio_utils::lock_guard _l(mutex());
    if (stream == mStreamType) {
        mStreamVolume = value;
        broadcast_l();
    }
}
float MmapPlaybackThread::streamVolume(audio_stream_type_t stream) const
{
    audio_utils::lock_guard _l(mutex());
    if (stream == mStreamType) {
        return mStreamVolume;
    }
    return 0.0f;
}
void MmapPlaybackThread::setStreamMute(audio_stream_type_t stream, bool muted)
{
    audio_utils::lock_guard _l(mutex());
    if (stream == mStreamType) {
        mStreamMute= muted;
        broadcast_l();
    }
}
void MmapPlaybackThread::invalidateTracks(audio_stream_type_t streamType)
{
    audio_utils::lock_guard _l(mutex());
    if (streamType == mStreamType) {
        for (const sp<IAfMmapTrack>& track : mActiveTracks) {
            track->invalidate();
        }
        broadcast_l();
    }
}
void MmapPlaybackThread::invalidateTracks(std::set<audio_port_handle_t>& portIds)
{
    audio_utils::lock_guard _l(mutex());
    bool trackMatch = false;
    for (const sp<IAfMmapTrack>& track : mActiveTracks) {
        if (portIds.find(track->portId()) != portIds.end()) {
            track->invalidate();
            trackMatch = true;
            portIds.erase(track->portId());
        }
        if (portIds.empty()) {
            break;
        }
    }
    if (trackMatch) {
        broadcast_l();
    }
}
void MmapPlaybackThread::processVolume_l()
NO_THREAD_SAFETY_ANALYSIS
{
    float volume;
    if (mMasterMute || mStreamMute) {
        volume = 0;
    } else {
        volume = mMasterVolume * mStreamVolume;
    }
    if (volume != mHalVolFloat) {
        uint32_t vol = (uint32_t)(volume * (1 << 24));
        if (!mEffectChains.isEmpty()) {
            mEffectChains[0]->setVolume_l(&vol, &vol);
            volume = (float)vol / (1 << 24);
        }
        if (mOutput->stream->setVolume(volume, volume) == NO_ERROR) {
            mHalVolFloat = volume;
            mNoCallbackWarningCount = 0;
        } else {
            sp<MmapStreamCallback> callback = mCallback.promote();
            if (callback != 0) {
                mHalVolFloat = volume;
                mNoCallbackWarningCount = 0;
                mutex().unlock();
                callback->onVolumeChanged(volume);
                mutex().lock();
            } else {
                if (mNoCallbackWarningCount < kMaxNoCallbackWarnings) {
                    ALOGW("Could not set MMAP stream volume: no volume callback!");
                    mNoCallbackWarningCount++;
                }
            }
        }
        for (const sp<IAfMmapTrack>& track : mActiveTracks) {
            track->setMetadataHasChanged();
            track->processMuteEvent_l(mAfThreadCallback->getOrCreateAudioManager(),
                              {mMasterMute,
                               mStreamVolume == 0.f,
                               mStreamMute,
                               false ,
                               false ,
                               false });
        }
    }
}
ThreadBase::MetadataUpdate MmapPlaybackThread::updateMetadata_l()
{
    if (!isStreamInitialized() || !mActiveTracks.readAndClearHasChanged()) {
        return {};
    }
    StreamOutHalInterface::SourceMetadata metadata;
    for (const sp<IAfMmapTrack>& track : mActiveTracks) {
        playback_track_metadata_v7_t trackMetadata;
        trackMetadata.base = {
                .usage = track->attributes().usage,
                .content_type = track->attributes().content_type,
                .gain = mHalVolFloat,
        };
        trackMetadata.channel_mask = track->channelMask(),
        strncpy(trackMetadata.tags, track->attributes().tags, AUDIO_ATTRIBUTES_TAGS_MAX_SIZE);
        metadata.tracks.push_back(trackMetadata);
    }
    mOutput->stream->updateSourceMetadata(metadata);
    MetadataUpdate change;
    change.playbackMetadataUpdate = metadata.tracks;
    return change;
};
void MmapPlaybackThread::checkSilentMode_l()
{
    if (!mMasterMute) {
        char value[PROPERTY_VALUE_MAX];
        if (property_get("ro.audio.silent", value, "0") > 0) {
            char *endptr;
            unsigned long ul = strtoul(value, &endptr, 0);
            if (*endptr == '\0' && ul != 0) {
                ALOGD("Silence is golden");
                setMasterMute_l(true);
            }
        }
    }
}
void MmapPlaybackThread::toAudioPortConfig(struct audio_port_config* config)
{
    MmapThread::toAudioPortConfig(config);
    if (mOutput && mOutput->flags != AUDIO_OUTPUT_FLAG_NONE) {
        config->config_mask |= AUDIO_PORT_CONFIG_FLAGS;
        config->flags.output = mOutput->flags;
    }
}
status_t MmapPlaybackThread::getExternalPosition(uint64_t* position,
        int64_t* timeNanos) const
{
    if (mOutput == nullptr) {
        return NO_INIT;
    }
    struct timespec timestamp;
    status_t status = mOutput->getPresentationPosition(position, &timestamp);
    if (status == NO_ERROR) {
        *timeNanos = timestamp.tv_sec * NANOS_PER_SECOND + timestamp.tv_nsec;
    }
    return status;
}
status_t MmapPlaybackThread::reportData(const void* buffer, size_t frameCount) {
    auto processor = mMelProcessor.load();
    if (processor) {
        processor->process(buffer, frameCount * mFrameSize);
    }
    return NO_ERROR;
}
void MmapPlaybackThread::startMelComputation_l(
        const sp<audio_utils::MelProcessor>& processor)
{
    ALOGV("%s: starting mel processor for thread %d", __func__, id());
    mMelProcessor.store(processor);
    if (processor) {
        processor->resume();
    }
}
void MmapPlaybackThread::stopMelComputation_l()
{
    ALOGV("%s: pausing mel processor for thread %d", __func__, id());
    auto melProcessor = mMelProcessor.load();
    if (melProcessor != nullptr) {
        melProcessor->pause();
    }
}
void MmapPlaybackThread::dumpInternals_l(int fd, const Vector<String16>& args)
{
    MmapThread::dumpInternals_l(fd, args);
    dprintf(fd, "  Stream type: %d Stream volume: %f HAL volume: %f Stream mute %d\n",
            mStreamType, mStreamVolume, mHalVolFloat, mStreamMute);
    dprintf(fd, "  Master volume: %f Master mute %d\n", mMasterVolume, mMasterMute);
}
sp<IAfMmapCaptureThread> IAfMmapCaptureThread::create(
        const sp<IAfThreadCallback>& afThreadCallback, audio_io_handle_t id,
        AudioHwDevice* hwDev, AudioStreamIn* input, bool systemReady) {
    return sp<MmapCaptureThread>::make(afThreadCallback, id, hwDev, input, systemReady);
}
MmapCaptureThread::MmapCaptureThread(
        const sp<IAfThreadCallback>& afThreadCallback, audio_io_handle_t id,
        AudioHwDevice *hwDev, AudioStreamIn *input, bool systemReady)
    : MmapThread(afThreadCallback, id, hwDev, input->stream, systemReady, false ),
      mInput(input)
{
    snprintf(mThreadName, kThreadNameLength, "AudioMmapIn_%X", id);
    mChannelCount = audio_channel_count_from_in_mask(mChannelMask);
}
status_t MmapCaptureThread::exitStandby_l()
{
    {
        if (mInput != nullptr && mInput->stream != nullptr) {
            mInput->stream->setGain(1.0f);
        }
    }
    return MmapThread::exitStandby_l();
}
AudioStreamIn* MmapCaptureThread::clearInput()
{
    audio_utils::lock_guard _l(mutex());
    AudioStreamIn *input = mInput;
    mInput = NULL;
    return input;
}
void MmapCaptureThread::processVolume_l()
{
    bool changed = false;
    bool silenced = false;
    sp<MmapStreamCallback> callback = mCallback.promote();
    if (callback == 0) {
        if (mNoCallbackWarningCount < kMaxNoCallbackWarnings) {
            ALOGW("Could not set MMAP stream silenced: no onStreamSilenced callback!");
            mNoCallbackWarningCount++;
        }
    }
    for (size_t i = 0; i < mActiveTracks.size() && !silenced; i++) {
        if (!mActiveTracks[i]->getAndSetSilencedNotified_l()) {
            changed = true;
            silenced = mActiveTracks[i]->isSilenced_l();
        }
    }
    if (changed) {
        mInput->stream->setGain(silenced ? 0.0f: 1.0f);
    }
}
ThreadBase::MetadataUpdate MmapCaptureThread::updateMetadata_l()
{
    if (!isStreamInitialized() || !mActiveTracks.readAndClearHasChanged()) {
        return {};
    }
    StreamInHalInterface::SinkMetadata metadata;
    for (const sp<IAfMmapTrack>& track : mActiveTracks) {
        record_track_metadata_v7_t trackMetadata;
        trackMetadata.base = {
                .source = track->attributes().source,
                .gain = 1,
        };
        trackMetadata.channel_mask = track->channelMask(),
        strncpy(trackMetadata.tags, track->attributes().tags, AUDIO_ATTRIBUTES_TAGS_MAX_SIZE);
        metadata.tracks.push_back(trackMetadata);
    }
    mInput->stream->updateSinkMetadata(metadata);
    MetadataUpdate change;
    change.recordMetadataUpdate = metadata.tracks;
    return change;
}
void MmapCaptureThread::setRecordSilenced(audio_port_handle_t portId, bool silenced)
{
    audio_utils::lock_guard _l(mutex());
    for (size_t i = 0; i < mActiveTracks.size() ; i++) {
        if (mActiveTracks[i]->portId() == portId) {
            mActiveTracks[i]->setSilenced_l(silenced);
            broadcast_l();
        }
    }
    setClientSilencedIfExists_l(portId, silenced);
}
void MmapCaptureThread::toAudioPortConfig(struct audio_port_config* config)
{
    MmapThread::toAudioPortConfig(config);
    if (mInput && mInput->flags != AUDIO_INPUT_FLAG_NONE) {
        config->config_mask |= AUDIO_PORT_CONFIG_FLAGS;
        config->flags.input = mInput->flags;
    }
}
status_t MmapCaptureThread::getExternalPosition(
        uint64_t* position, int64_t* timeNanos) const
{
    if (mInput == nullptr) {
        return NO_INIT;
    }
    return mInput->getCapturePosition((int64_t*)position, timeNanos);
}
sp<IAfPlaybackThread> IAfPlaybackThread::createBitPerfectThread(
        const sp<IAfThreadCallback>& afThreadCallback,
        AudioStreamOut* output, audio_io_handle_t id, bool systemReady) {
    return sp<BitPerfectThread>::make(afThreadCallback, output, id, systemReady);
}
BitPerfectThread::BitPerfectThread(const sp<IAfThreadCallback> &afThreadCallback,
        AudioStreamOut *output, audio_io_handle_t id, bool systemReady)
        : MixerThread(afThreadCallback, output, id, systemReady, BIT_PERFECT) {}
PlaybackThread::mixer_state BitPerfectThread::prepareTracks_l(
        Vector<sp<IAfTrack>>* tracksToRemove) {
    mixer_state result = MixerThread::prepareTracks_l(tracksToRemove);
    float volumeLeft = 1.0f;
    float volumeRight = 1.0f;
    if (mActiveTracks.size() == 1 && mActiveTracks[0]->isBitPerfect()) {
        const int trackId = mActiveTracks[0]->id();
        mAudioMixer->setParameter(
                    trackId, AudioMixer::TRACK, AudioMixer::TEE_BUFFER, (void *)mSinkBuffer);
        mAudioMixer->setParameter(
                    trackId, AudioMixer::TRACK, AudioMixer::TEE_BUFFER_FRAME_COUNT,
                    (void *)(uintptr_t)mNormalFrameCount);
        mActiveTracks[0]->getFinalVolume(&volumeLeft, &volumeRight);
        mIsBitPerfect = true;
    } else {
        mIsBitPerfect = false;
        for (const auto& track : mActiveTracks) {
            const int trackId = track->id();
            mAudioMixer->setParameter(
                        trackId, AudioMixer::TRACK, AudioMixer::TEE_BUFFER, nullptr);
        }
    }
    if (mVolumeLeft != volumeLeft || mVolumeRight != volumeRight) {
        mVolumeLeft = volumeLeft;
        mVolumeRight = volumeRight;
        setVolumeForOutput_l(volumeLeft, volumeRight);
    }
    return result;
}
void BitPerfectThread::threadLoop_mix() {
    MixerThread::threadLoop_mix();
    mHasDataCopiedToSinkBuffer = mIsBitPerfect;
}
}
