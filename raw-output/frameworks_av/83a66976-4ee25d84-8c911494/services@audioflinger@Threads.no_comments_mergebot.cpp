#define LOG_TAG "AudioFlinger"
#define ATRACE_TAG ATRACE_TAG_AUDIO
#include "Configuration.h"
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
#include "AudioFlinger.h"
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
#include <audio_utils/Balance.h>
#include <mediautils/ServiceUtilities.h>
#include <powermanager/PowerManager.h>
#include <private/android_filesystem_config.h>
#include <system/audio.h>
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
}
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
ThreadBase::ThreadBase(const sp<AudioFlinger>& audioFlinger, const sp<IAfThreadCallback>& afThreadCallback, audio_io_handle_t id, type_t type, bool systemReady, bool isOut): Thread(false ), mType(type), mAudioFlinger(audioFlinger), mAfThreadCallback(afThreadCallback), mThreadMetrics(std::string(AMEDIAMETRICS_KEY_PREFIX_AUDIO_THREAD) + std::to_string(id), isOut), mIsOut(isOut),
        mStandby(false), mAudioSource(AUDIO_SOURCE_DEFAULT), mId(id),
        mDeathRecipient(new PMDeathRecipient(this)), mSystemReady(systemReady), mSignalPending(false) {
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
{
    status_t status = ALREADY_EXISTS;
    if (mActiveTracks.indexOf(track) < 0) {
        if (track->isExternalTrack()) {
            IAfTrackBase::track_state state = track->state();
            mLock.unlock();
            status = AudioSystem::startOutput(track->portId());
            mLock.lock();
            if (state != track->state()) {
                if (status == NO_ERROR) {
                    mLock.unlock();
                    AudioSystem::stopOutput(track->portId());
                    mLock.lock();
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
            mLock.unlock();
            const os::HapticScale intensity = AudioFlinger::onExternalVibrationStart(
                    track->getExternalVibration());
            std::optional<media::AudioVibratorInfo> vibratorInfo;
            {
<<<<<<< HEAD
                 audio_utils::lock_guard _l(mAfThreadCallback->mutex());
                vibratorInfo = std::move(mAfThreadCallback->getDefaultVibratorInfo_l());
||||||| 8c9114940a
<<<<<<< HEAD
                 audio_utils::lock_guard _l(mAfThreadCallback->mutex());
                vibratorInfo = std::move(mAfThreadCallback->getDefaultVibratorInfo_l());
||||||| 8c9114940a
                Mutex::Autolock _l(mAudioFlinger->mLock);
                vibratorInfo = std::move(mAudioFlinger->getDefaultVibratorInfo_l());
=======
                Mutex::Autolock _l(mAfThreadCallback->mutex());
                vibratorInfo = std::move(mAfThreadCallback->getDefaultVibratorInfo_l());
>>>>>>> 4ee25d84
=======
                Mutex::Autolock _l(mAfThreadCallback->mutex());
                vibratorInfo = std::move(mAfThreadCallback->getDefaultVibratorInfo_l());
>>>>>>> 4ee25d84
            }
            mLock.lock();
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
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
private:
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l()
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l()
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l()
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l()
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
<<<<<<< HEAD
void stop_nonvirtual();
||||||| 8c9114940a
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l()
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
=======
bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l() const
>>>>>>> 4ee25d84
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
RecordHandle::~RecordHandle() {
    stop_nonvirtual();
    mRecordTrack->destroy();
}
RecordHandle::~RecordHandle() {
    stop_nonvirtual();
    mRecordTrack->destroy();
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
{
    status_t status = ALREADY_EXISTS;
    if (mActiveTracks.indexOf(track) < 0) {
        if (track->isExternalTrack()) {
            IAfTrackBase::track_state state = track->state();
            mLock.unlock();
            status = AudioSystem::startOutput(track->portId());
            mLock.lock();
            if (state != track->state()) {
                if (status == NO_ERROR) {
                    mLock.unlock();
                    AudioSystem::stopOutput(track->portId());
                    mLock.lock();
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
            mLock.unlock();
            const os::HapticScale intensity = AudioFlinger::onExternalVibrationStart(
                    track->getExternalVibration());
            std::optional<media::AudioVibratorInfo> vibratorInfo;
            {
<<<<<<< HEAD
                 audio_utils::lock_guard _l(mAfThreadCallback->mutex());
                vibratorInfo = std::move(mAfThreadCallback->getDefaultVibratorInfo_l());
||||||| 8c9114940a
<<<<<<< HEAD
                 audio_utils::lock_guard _l(mAfThreadCallback->mutex());
                vibratorInfo = std::move(mAfThreadCallback->getDefaultVibratorInfo_l());
||||||| 8c9114940a
                Mutex::Autolock _l(mAudioFlinger->mLock);
                vibratorInfo = std::move(mAudioFlinger->getDefaultVibratorInfo_l());
=======
                Mutex::Autolock _l(mAfThreadCallback->mutex());
                vibratorInfo = std::move(mAfThreadCallback->getDefaultVibratorInfo_l());
>>>>>>> 4ee25d84
=======
                Mutex::Autolock _l(mAfThreadCallback->mutex());
                vibratorInfo = std::move(mAfThreadCallback->getDefaultVibratorInfo_l());
>>>>>>> 4ee25d84
            }
            mLock.lock();
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
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
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
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
