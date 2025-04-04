/*
**
** Copyright 2012, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "AudioFlinger"
// #define LOG_NDEBUG 0

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
// NBAIO implementations

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
// ----------------------------------------------------------------------------
// Note: the following macro is used for extremely verbose logging message.  In
// order to run with ALOG_ASSERT turned on, we need to have LOG_NDEBUG set to
// 0; but one side effect of this is to turn all LOGV's as well.  Some messages
// are so verbose that we want to suppress them even when we have ALOG_ASSERT
// turned on.  Do not uncomment the #def below unless you really know what you
// are doing and want to see all of the extremely verbose messages.
//#define VERY_VERY_VERBOSE_LOGGING

#ifdef VERY_VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif
// TODO: Move these macro/inlines to a header file.

#define max(a, b) ((a) > (b) ? (a) : (b))

template <typename T>
static inline T min(const T& a, const T& b)
{
    return a < b ? a : b;
}

namespace android {

using audioflinger::SyncEvent;
using media::IEffectClient;
using content::AttributionSourceState;

// Keep in sync with java definition in media/java/android/media/AudioRecord.java
static constexpr int32_t kMaxSharedAudioHistoryMs = 5000;

// retry counts for buffer fill timeout
// 50 * ~20msecs = 1 second
static const int8_t kMaxTrackRetries = 50;
static const int8_t kMaxTrackStartupRetries = 50;

// allow less retry attempts on direct output thread.
// direct outputs can be a scarce resource in audio hardware and should
// be released as quickly as possible.
// Notes:
// 1) The retry duration kMaxTrackRetriesDirectMs may be increased
//    in case the data write is bursty for the AudioTrack.  The application
//    should endeavor to write at least once every kMaxTrackRetriesDirectMs
//    to prevent an underrun situation.  If the data is bursty, then
//    the application can also throttle the data sent to be even.
// 2) For compressed audio data, any data present in the AudioTrack buffer
//    will be sent and reset the retry count.  This delivers data as
//    it arrives, with approximately kDirectMinSleepTimeUs = 10ms checking interval.
// 3) For linear PCM or proportional PCM, we wait one period for a period's worth
//    of data to be available, then any remaining data is delivered.
//    This is required to ensure the last bit of data is delivered before underrun.
//
// Sleep time per cycle is kDirectMinSleepTimeUs for compressed tracks
// or the size of the HAL period for proportional / linear PCM tracks.
static const int32_t kMaxTrackRetriesDirectMs = 200;

// don't warn about blocked writes or record buffer overflows more often than this
static const nsecs_t kWarningThrottleNs = seconds(5);

// RecordThread loop sleep time upon application overrun or audio HAL read error
static const int kRecordThreadSleepUs = 5000;

// maximum time to wait in sendConfigEvent_l() for a status to be received
static const nsecs_t kConfigEventTimeoutNs = seconds(2);

// minimum sleep time for the mixer thread loop when tracks are active but in underrun
static const uint32_t kMinThreadSleepTimeUs = 5000;
// maximum divider applied to the active sleep time in the mixer thread loop
static const uint32_t kMaxThreadSleepTimeShift = 2;

// minimum normal sink buffer size, expressed in milliseconds rather than frames
// FIXME This should be based on experimentally observed scheduling jitter
static const uint32_t kMinNormalSinkBufferSizeMs = 20;
// maximum normal sink buffer size
static const uint32_t kMaxNormalSinkBufferSizeMs = 24;

// minimum capture buffer size in milliseconds to _not_ need a fast capture thread
// FIXME This should be based on experimentally observed scheduling jitter
static const uint32_t kMinNormalCaptureBufferSizeMs = 12;

// Offloaded output thread standby delay: allows track transition without going to standby
static const nsecs_t kOffloadStandbyDelayNs = seconds(1);

// Direct output thread minimum sleep time in idle or active(underrun) state
static const nsecs_t kDirectMinSleepTimeUs = 10000;

// Minimum amount of time between checking to see if the timestamp is advancing
// for underrun detection. If we check too frequently, we may not detect a
// timestamp update and will falsely detect underrun.
static const nsecs_t kMinimumTimeBetweenTimestampChecksNs = 150 /* ms */ * 1000;

// The universal constant for ubiquitous 20ms value. The value of 20ms seems to provide a good
// balance between power consumption and latency, and allows threads to be scheduled reliably
// by the CFS scheduler.
// FIXME Express other hardcoded references to 20ms with references to this constant and move
// it appropriately.
#define FMS_20 20

// Whether to use fast mixer
static const enum {
    FastMixer_Never,    // never initialize or use: for debugging only
    FastMixer_Always,   // always initialize and use, even if not needed: for debugging only
                        // normal mixer multiplier is 1
    FastMixer_Static,   // initialize if needed, then use all the time if initialized,
                        // multiplier is calculated based on min & max normal mixer buffer size
    FastMixer_Dynamic,  // initialize if needed, then use dynamically depending on track load,
                        // multiplier is calculated based on min & max normal mixer buffer size
    // FIXME for FastMixer_Dynamic:
    //  Supporting this option will require fixing HALs that can't handle large writes.
    //  For example, one HAL implementation returns an error from a large write,
    //  and another HAL implementation corrupts memory, possibly in the sample rate converter.
    //  We could either fix the HAL implementations, or provide a wrapper that breaks
    //  up large writes into smaller ones, and the wrapper would need to deal with scheduler.
} kUseFastMixer = FastMixer_Static;

// Whether to use fast capture
static const enum {
    FastCapture_Never,  // never initialize or use: for debugging only
    FastCapture_Always, // always initialize and use, even if not needed: for debugging only
    FastCapture_Static, // initialize if needed, then use all the time if initialized
} kUseFastCapture = FastCapture_Static;

// Priorities for requestPriority
static const int kPriorityAudioApp = 2;
static const int kPriorityFastMixer = 3;
static const int kPriorityFastCapture = 3;

// IAudioFlinger::createTrack() has an in/out parameter 'pFrameCount' for the total size of the
// track buffer in shared memory.  Zero on input means to use a default value.  For fast tracks,
// AudioFlinger derives the default from HAL buffer size and 'fast track multiplier'.
// This is the default value, if not specified by property.
static const int kFastTrackMultiplier = 2;

// The minimum and maximum allowed values
static const int kFastTrackMultiplierMin = 1;
static const int kFastTrackMultiplierMax = 2;

// The actual value to use, which can be specified per-device via property af.fast_track_multiplier.
static int sFastTrackMultiplier = kFastTrackMultiplier;

// See Thread::readOnlyHeap().
// Initially this heap is used to allocate client buffers for "fast" AudioRecord.
// Eventually it will be the single buffer that FastCapture writes into via HAL read(),
// and that all "fast" AudioRecord clients read from.  In either case, the size can be small.
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

// Set kEnableExtendedChannels to true to enable greater than stereo output
// for the MixerThread and device sink.  Number of channels allowed is
// FCC_2 <= channels <= FCC_LIMIT.
constexpr bool kEnableExtendedChannels = true;

// Returns true if channel mask is permitted for the PCM sink in the MixerThread
/* static */
bool IAfThreadBase::isValidPcmSinkChannelMask(audio_channel_mask_t channelMask) {
    switch (audio_channel_mask_get_representation(channelMask)) {
    case AUDIO_CHANNEL_REPRESENTATION_POSITION: {
        // Haptic channel mask is only applicable for channel position mask.
        const uint32_t channelCount = audio_channel_count_from_out_mask(
                static_cast<audio_channel_mask_t>(channelMask & ~AUDIO_CHANNEL_HAPTIC_ALL));
        const uint32_t maxChannelCount = kEnableExtendedChannels
                ? FCC_LIMIT : FCC_2;
        if (channelCount < FCC_2 // mono is not supported at this time
                || channelCount > maxChannelCount) {
            return false;
        }
        // check that channelMask is the "canonical" one we expect for the channelCount.
        return audio_channel_position_mask_is_out_canonical(channelMask);
        }
    case AUDIO_CHANNEL_REPRESENTATION_INDEX:
        if (kEnableExtendedChannels) {
            const uint32_t channelCount = audio_channel_count_from_out_mask(channelMask);
            if (channelCount >= FCC_2 // mono is not supported at this time
                    && channelCount <= FCC_LIMIT) {
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

// Set kEnableExtendedPrecision to true to use extended precision in MixerThread
constexpr bool kEnableExtendedPrecision = true;

// Returns true if format is permitted for the PCM sink in the MixerThread
/* static */
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

// ----------------------------------------------------------------------------
// formatToString() needs to be exact for MediaMetrics purposes.
// Do not use media/TypeConverter.h toString().
/* static */
std::string IAfThreadBase::formatToString(audio_format_t format) {
    std::string result;
    FormatConverter::toString(format, result);
    return result;
}

// TODO: move all toString helpers to audio.h
// under  #ifdef __cplusplus #endif
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
    // We convert to the AIDL type to print (eventually the legacy type will be removed).
    const auto result = legacy2aidl_audio_latency_mode_t_AudioLatencyMode(mode);
    return result.has_value() ? media::audio::common::toString(*result) : "UNKNOWN";
}

// Could be made a template, but other toString overloads for std::vector are confused.
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

// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
#ifdef ADD_BATTERY_DATA
// To collect the amplifier usage
static void addBatteryData(uint32_t params) {
    sp<IMediaPlayerService> service = IMediaDeathNotifier::getMediaPlayerService();
    if (service == NULL) {
        // it already logged
        return;
    }

    service->addBatteryData(params);
}
#endif

// Track the CLOCK_BOOTTIME versus CLOCK_MONOTONIC timebase offset
struct {
    // call when you acquire a partial wakelock
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

    // call when you release a partial wakelock.
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

    // retrieves the boottime timebase offset from monotonic.
    int64_t getBoottimeOffset() {
        pthread_mutex_lock(&mLock);
        int64_t boottimeOffset = mBoottimeOffset;
        pthread_mutex_unlock(&mLock);
        return boottimeOffset;
    }

    // Adjusts the timebase offset between TIMEBASE_MONOTONIC
    // and the selected timebase.
    // Currently only TIMEBASE_BOOTTIME is allowed.
    //
    // This only needs to be called upon acquiring the first partial wakelock
    // after all other partial wakelocks are released.
    //
    // We do an empirical measurement of the offset rather than parsing
    // /proc/timer_list since the latter is not a formal kernel ABI.
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
        // try three times to get the clock offset, choose the one
        // with the minimum gap in measurements.
        const int tries = 3;
        nsecs_t bestGap = 0, measured = 0; // not required, initialized for clang-tidy
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

        // to avoid micro-adjusting, we don't change the timebase
        // unless it is significantly different.
        //
        // Assumption: It probably takes more than toleranceNs to
        // suspend and resume the device.
        static int64_t toleranceNs = 10000; // 10 us
        if (llabs(*offset - measured) > toleranceNs) {
            ALOGV("Adjusting timebase offset old: %lld  new: %lld",
                    (long long)*offset, (long long)measured);
            *offset = measured;
        }
    }

    pthread_mutex_t mLock;
    int32_t mCount;
    int64_t mBoottimeOffset;
} gBoottime = { PTHREAD_MUTEX_INITIALIZER, 0, 0 };                                                   // static, so use POD initialization
                                                   // ----------------------------------------------------------------------------
                                                   //      CPU Stats
                                                   // ----------------------------------------------------------------------------
class CpuStats {
public:
    CpuStats();
    void sample(const String8 &title);
#ifdef DEBUG_CPU_USAGE
private:
    ThreadCpuUsage mCpuUsage;           // instantaneous thread CPU usage in wall clock ns
    audio_utils::Statistics<double> mWcStats; // statistics on thread CPU usage in wall clock ns

    audio_utils::Statistics<double> mHzStats; // statistics on thread CPU usage in cycles

    int mCpuNum;                        // thread's current CPU number
    int mCpukHz;                        // frequency of thread's current CPU in kHz
#endif
};

void CpuStats::sample(const String8 &title
#ifndef DEBUG_CPU_USAGE
                __unused
#endif
        ) {
#ifdef DEBUG_CPU_USAGE
    // get current thread's delta CPU time in wall clock ns
    double wcNs;
    bool valid = mCpuUsage.sampleAndEnable(wcNs);

    // record sample for wall clock statistics
    if (valid) {
        mWcStats.add(wcNs);
    }

    // get the current CPU number
    int cpuNum = sched_getcpu();

    // get the current CPU frequency in kHz
    int cpukHz = mCpuUsage.getCpukHz(cpuNum);

    // check if either CPU number or frequency changed
    if (cpuNum != mCpuNum || cpukHz != mCpukHz) {
        mCpuNum = cpuNum;
        mCpukHz = cpukHz;
        // ignore sample for purposes of cycles
        valid = false;
    }

    // if no change in CPU number or frequency, then record sample for cycle statistics
    if (valid && mCpukHz > 0) {
        const double cycles = wcNs * cpukHz * 0.000001;
        mHzStats.add(cycles);
    }

    const unsigned n = mWcStats.getN();
    // mCpuUsage.elapsed() is expensive, so don't call it every loop
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
}// ----------------------------------------------------------------------------
//      ThreadBase
// ----------------------------------------------------------------------------
// static
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

ThreadBase::ThreadBase(const sp<AudioFlinger>& audioFlinger, const sp<IAfThreadCallback>& afThreadCallback, audio_io_handle_t id, type_t type, bool systemReady, bool isOut): Thread(false /*canCallJava*/), mType(type), mAudioFlinger(audioFlinger), mAfThreadCallback(afThreadCallback), mThreadMetrics(std::string(AMEDIAMETRICS_KEY_PREFIX_AUDIO_THREAD) + std::to_string(id), isOut), mIsOut(isOut), // mSampleRate, mFrameCount, mChannelMask, mChannelCount, mFrameSize, mFormat, mBufferSize
        // are set by PlaybackThread::readOutputParameters_l() or
        // RecordThread::readInputParameters_l()
        //FIXME: mStandby should be true here. Is this some kind of hack?
        mStandby(false), mAudioSource(AUDIO_SOURCE_DEFAULT), mId(id), // mName will be set by concrete (non-virtual) subclass
        mDeathRecipient(new PMDeathRecipient(this)), mSystemReady(systemReady), mSignalPending(false) {
    mThreadMetrics.logConstructor(getpid(), threadTypeToString(type), id);
    memset(&mPatch, 0, sizeof(struct audio_patch));
}

ThreadBase::~ThreadBase()
{
    // mConfigEvents should be empty, but just in case it isn't, free the memory it owns
    mConfigEvents.clear();

    // do not lock the mutex in destructor
    releaseWakeLock_l();
    if (mPowerManager != 0) {
        sp<IBinder> binder = IInterface::asBinder(mPowerManager);
        binder->unlinkToDeath(mDeathRecipient);
    }

    sendStatistics(true /* force */);
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
    // do any cleanup required for exit to succeed
    preExit();
    {
        // This lock prevents the following race in thread (uniprocessor for illustration):
        //  if (!exitPending()) {
        //      // context switch from here to exit()
        //      // exit() calls requestExit(), what exitPending() observes
        //      // exit() calls signal(), which is dropped since no waiters
        //      // context switch back from exit() to here
        //      mWaitWorkCV.wait(...);
        //      // now thread is hung
        //  }
        audio_utils::lock_guard lock(mutex());
        requestExit();
        mWaitWorkCV.notify_all();
    }
    // When Thread::requestExitAndWait is made virtual and this method is renamed to
    // "virtual status_t requestExitAndWait()", replace by "return Thread::requestExitAndWait();"
    requestExitAndWait();
}

status_t ThreadBase::setParameters(const String8& keyValuePairs)
{
    ALOGV("ThreadBase::setParameters() %s", keyValuePairs.c_str());
    audio_utils::lock_guard _l(mutex());

    return sendSetParameterConfigEvent_l(keyValuePairs);
}

// addTrack_l() must be called with ThreadBase::mLock held
{
    status_t status = ALREADY_EXISTS;

    if (mActiveTracks.indexOf(track) < 0) {
        // the track is newly added, make sure it fills up all its
        // buffers before playing. This is to ensure the client will
        // effectively get the latency it requested.
        if (track->isExternalTrack()) {
            IAfTrackBase::track_state state = track->state();
            mLock.unlock();
            status = AudioSystem::startOutput(track->portId());
            mLock.lock();
            // abort track was stopped/paused while we released the lock
            if (state != track->state()) {
                if (status == NO_ERROR) {
                    mLock.unlock();
                    AudioSystem::stopOutput(track->portId());
                    mLock.lock();
                }
                return INVALID_OPERATION;
            }
            // abort if start is rejected by audio policy manager
            if (status != NO_ERROR) {
                // Do not replace the error if it is DEAD_OBJECT. When this happens, it indicates
                // current playback thread is reopened, which may happen when clients set preferred
                // mixer configuration. Returning DEAD_OBJECT will make the client restore track
                // immediately.
                return status == DEAD_OBJECT ? status : PERMISSION_DENIED;
            }
#ifdef ADD_BATTERY_DATA
            // to track the speaker usage
            addBatteryData(IMediaPlayerService::kBatteryDataAudioFlingerStart);
#endif
            sendIoConfigEvent_l(AUDIO_CLIENT_STARTED, track->creatorPid(), track->portId());
        }

        // set retry count for buffer fill
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
            // Unlock due to VibratorService will lock for this call and will
            // call Tracks.mute/unmute which also require thread's lock.
            mLock.unlock();
            const os::HapticScale intensity = AudioFlinger::onExternalVibrationStart(
                    track->getExternalVibration());
            std::optional<media::AudioVibratorInfo> vibratorInfo;
            {
                // TODO(b/184194780): Use the vibrator information from the vibrator that will be
                // used to play this track.
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

            // Haptic playback should be enabled by vibrator service.
            if (track->getHapticPlaybackEnabled()) {
                // Disable haptic playback of all active track to ensure only
                // one track playing haptic if current track should play haptic.
                for (const auto &t : mActiveTracks) {
                    t->setHapticPlaybackEnabled(false);
                }
            }

            // Set haptic intensity for effect
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

        track->logBeginInterval(patchSinksToString(&mPatch)); // log to MediaMetrics
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

// ----------------------------------------------------------------------------
//      Playback
// ----------------------------------------------------------------------------

RecordHandle::~RecordHandle() {
    stop_nonvirtual();
    mRecordTrack->destroy();
}

RecordHandle::~RecordHandle() {
    stop_nonvirtual();
    mRecordTrack->destroy();
}

// Thread virtuals

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

// addTrack_l() must be called with ThreadBase::mLock held
{
    status_t status = ALREADY_EXISTS;

    if (mActiveTracks.indexOf(track) < 0) {
        // the track is newly added, make sure it fills up all its
        // buffers before playing. This is to ensure the client will
        // effectively get the latency it requested.
        if (track->isExternalTrack()) {
            IAfTrackBase::track_state state = track->state();
            mLock.unlock();
            status = AudioSystem::startOutput(track->portId());
            mLock.lock();
            // abort track was stopped/paused while we released the lock
            if (state != track->state()) {
                if (status == NO_ERROR) {
                    mLock.unlock();
                    AudioSystem::stopOutput(track->portId());
                    mLock.lock();
                }
                return INVALID_OPERATION;
            }
            // abort if start is rejected by audio policy manager
            if (status != NO_ERROR) {
                // Do not replace the error if it is DEAD_OBJECT. When this happens, it indicates
                // current playback thread is reopened, which may happen when clients set preferred
                // mixer configuration. Returning DEAD_OBJECT will make the client restore track
                // immediately.
                return status == DEAD_OBJECT ? status : PERMISSION_DENIED;
            }
#ifdef ADD_BATTERY_DATA
            // to track the speaker usage
            addBatteryData(IMediaPlayerService::kBatteryDataAudioFlingerStart);
#endif
            sendIoConfigEvent_l(AUDIO_CLIENT_STARTED, track->creatorPid(), track->portId());
        }

        // set retry count for buffer fill
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
            // Unlock due to VibratorService will lock for this call and will
            // call Tracks.mute/unmute which also require thread's lock.
            mLock.unlock();
            const os::HapticScale intensity = AudioFlinger::onExternalVibrationStart(
                    track->getExternalVibration());
            std::optional<media::AudioVibratorInfo> vibratorInfo;
            {
                // TODO(b/184194780): Use the vibrator information from the vibrator that will be
                // used to play this track.
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

            // Haptic playback should be enabled by vibrator service.
            if (track->getHapticPlaybackEnabled()) {
                // Disable haptic playback of all active track to ensure only
                // one track playing haptic if current track should play haptic.
                for (const auto &t : mActiveTracks) {
                    t->setHapticPlaybackEnabled(false);
                }
            }

            // Set haptic intensity for effect
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

        track->logBeginInterval(patchSinksToString(&mPatch)); // log to MediaMetrics
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
        // return an estimation of rendered frames when the output is suspended
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
    // session AUDIO_SESSION_OUTPUT_MIX is placed in same strategy as MUSIC stream so that
    // it is moved to correct output by audio policy manager when A2DP is connected or disconnected
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
    // FIXME FastMixer might also have a raw ptr to mOutputSink;
    //       must push a NULL and wait for ack
    mOutputSink.clear();
    mPipeSink.clear();
    mNormalSink.clear();
    return output;
}

// this method must always be called either with ThreadBase mutex() held or inside the thread loop
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
    // Miscellaneous track cleanup when removed from the active list,
    // called without Thread lock but synchronized with threadLoop processing.
#ifdef ADD_BATTERY_DATA
    for (const auto& track : tracksToRemove) {
        if (track->isExternalTrack()) {
            // to track the speaker usage
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
                // The setprop command will not allow a property to be changed after
                // the first time it is set, so we don't have to worry about un-muting.
                setMasterMute_l(true);
            }
        }
    }
}

// shared by MIXER and DIRECT, overridden by DUPLICATING
ssize_t PlaybackThread::threadLoop_write()
{
    LOG_HIST_TS();
    mInWrite = true;
    ssize_t bytesWritten;
    const size_t offset = mCurrentWriteLength - mBytesRemaining;

    // If an NBAIO sink is present, use it to write the normal mixer's submix
    if (mNormalSink != 0) {

        const size_t count = mBytesRemaining / mFrameSize;

        ATRACE_BEGIN("write");
        // update the setpoint when AudioFlinger::mScreenState changes
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
    // otherwise use the HAL / AudioStreamOut directly
    } else {
        // Direct output and offload threads

        if (mUseAsyncWrite) {
            ALOGW_IF(mWriteAckSequence & 1, "threadLoop_write(): out of sequence write request");
            mWriteAckSequence += 2;
            mWriteAckSequence |= 1;
            ALOG_ASSERT(mCallbackThread != 0);
            mCallbackThread->setWriteBlocked(mWriteAckSequence);
        }
        ATRACE_BEGIN("write");
        // FIXME We should have an implementation of timestamps for direct output threads.
        // They are used e.g for multichannel PCM playback over HDMI.
        bytesWritten = mOutput->write((char *)mSinkBuffer + offset, mBytesRemaining);
        ATRACE_END();

        if (mUseAsyncWrite &&
                ((bytesWritten < 0) || (bytesWritten == (ssize_t)mBytesRemaining))) {
            // do not wait for async callback in case of error of full write
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

// startMelComputation_l() must be called with AudioFlinger::mutex() held
void PlaybackThread::startMelComputation_l(
        const sp<audio_utils::MelProcessor>& processor)
{
    auto outputSink = static_cast<AudioStreamOutSink*>(mOutputSink.get());
    if (outputSink != nullptr) {
        outputSink->startMelComputation(processor);
    }
}

// stopMelComputation_l() must be called with AudioFlinger::mutex() held
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
        // Clear ActiveTracks to update BatteryNotifier in case active tracks remain.
        // After we exit there are no more track changes sent to BatteryNotifier
        // because that requires an active threadLoop.
        // TODO: should we decActiveTrackCnt() of the cleared track effect chain?
        mActiveTracks.clear();
    }
}

/*
The derived values that are cached:
 - mSinkBufferSize from frame count * frame size
 - mActiveSleepTimeUs from activeSleepTimeUs()
 - mIdleSleepTimeUs from idleSleepTimeUs()
 - mStandbyDelayNs from mActiveSleepTimeUs (DIRECT only) or forced to at least
   kDefaultStandbyTimeInNsecs when connected to an A2DP device.
 - maxPeriod from frame count and sample rate (MIXER only)

The parameters that affect these derived values are:
 - frame count
 - frame size
 - sample rate
 - device type: A2DP or not
 - device latency
 - format: PCM or not
 - active sleep time
 - idle sleep time
*/

/*
The derived values that are cached:
 - mSinkBufferSize from frame count * frame size
 - mActiveSleepTimeUs from activeSleepTimeUs()
 - mIdleSleepTimeUs from idleSleepTimeUs()
 - mStandbyDelayNs from mActiveSleepTimeUs (DIRECT only) or forced to at least
   kDefaultStandbyTimeInNsecs when connected to an A2DP device.
 - maxPeriod from frame count and sample rate (MIXER only)

The parameters that affect these derived values are:
 - frame count
 - frame size
 - sample rate
 - device type: A2DP or not
 - device latency
 - format: PCM or not
 - active sleep time
 - idle sleep time
*/
void PlaybackThread::cacheParameters_l()
{
    mSinkBufferSize = mNormalFrameCount * mFrameSize;
    mActiveSleepTimeUs = activeSleepTimeUs();
    mIdleSleepTimeUs = idleSleepTimeUs();

    mStandbyDelayNs = getStandbyTimeInNanos();

    // make sure standby delay is not too short when connected to an A2DP sink to avoid
    // truncating audio when going to standby.
    if (!Intersection(outDeviceTypes(),  getAudioDeviceOutAllA2dpSet()).empty()) {
        if (mStandbyDelayNs < kDefaultStandbyTimeInNsecs) {
            mStandbyDelayNs = kDefaultStandbyTimeInNsecs;
        }
    }
}

bool PlaybackThread::invalidateTracks_l(audio_stream_type_t streamType)
{
    ALOGV("MixerThread::invalidateTracks() mixer %p, streamType %d, mTracks.size %zu",
            this,  streamType, mTracks.size());
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

  // namespace android