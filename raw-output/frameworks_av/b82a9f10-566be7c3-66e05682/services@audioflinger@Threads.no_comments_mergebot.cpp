#define LOG_TAG "AudioFlinger"
#define ATRACE_TAG ATRACE_TAG_AUDIO
#include "Configuration.h"
#include <math.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cutils/properties.h>
#include <media/AudioParameter.h>
#include <utils/Log.h>
#include <utils/Trace.h>
#include <private/media/AudioTrackShared.h>
#include <hardware/audio.h>
#include <audio_effects/effect_ns.h>
#include <audio_effects/effect_aec.h>
#include <audio_utils/primitives.h>
#include <media/nbaio/AudioStreamOutSink.h>
#include <media/nbaio/MonoPipe.h>
#include <media/nbaio/MonoPipeReader.h>
#include <media/nbaio/Pipe.h>
#include <media/nbaio/PipeReader.h>
#include <media/nbaio/SourceAudioBufferProvider.h>
#include <powermanager/PowerManager.h>
#include <common_time/cc_helper.h>
#include <common_time/local_clock.h>
#include "AudioFlinger.h"
#include "AudioMixer.h"
#include "FastMixer.h"
#include "ServiceUtilities.h"
#include "SchedulingPolicyService.h"
#ifdef ADD_BATTERY_DATA
#include <media/IMediaPlayerService.h>
#include <media/IMediaDeathNotifier.h>
#endif
#ifdef DEBUG_CPU_USAGE
#include <cpustats/CentralTendencyStatistics.h>
#include <cpustats/ThreadCpuUsage.h>
#endif
#ifdef VERY_VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif
namespace android {
static const int8_t kMaxTrackRetries = 50;
static const int8_t kMaxTrackStartupRetries = 50;
static const int8_t kMaxTrackRetriesDirect = 2;
static const nsecs_t kWarningThrottleNs = seconds(5);
static const int kRecordThreadSleepUs = 5000;
static const nsecs_t kSetParametersTimeoutNs = seconds(2);
static const uint32_t kMinThreadSleepTimeUs = 5000;
static const uint32_t kMaxThreadSleepTimeShift = 2;
static const uint32_t kMinNormalMixBufferSizeMs = 20;
static const uint32_t kMaxNormalMixBufferSizeMs = 24;
static const nsecs_t kOffloadStandbyDelayNs = seconds(1);
static const enum {
    FastMixer_Never,
    FastMixer_Always,
    FastMixer_Static,
    FastMixer_Dynamic,
} kUseFastMixer = FastMixer_Static;
static const int kPriorityAudioApp = 2;
static const int kPriorityFastMixer = 3;
static const int kFastTrackMultiplier = 2;
#ifdef ADD_BATTERY_DATA
static void addBatteryData(uint32_t params) {
    sp<IMediaPlayerService> service = IMediaDeathNotifier::getMediaPlayerService();
    if (service == NULL) {
        return;
    }
    service->addBatteryData(params);
}
#endif
class CpuStats {
public:
    CpuStats();
    void sample(const String8 &title);
#ifdef DEBUG_CPU_USAGE
private:
    ThreadCpuUsage mCpuUsage;
    CentralTendencyStatistics mWcStats;
    CentralTendencyStatistics mHzStats;
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
        mWcStats.sample(wcNs);
    }
    int cpuNum = sched_getcpu();
    int cpukHz = mCpuUsage.getCpukHz(cpuNum);
    if (cpuNum != mCpuNum || cpukHz != mCpukHz) {
        mCpuNum = cpuNum;
        mCpukHz = cpukHz;
        valid = false;
    }
    if (valid && mCpukHz > 0) {
        double cycles = wcNs * cpukHz * 0.000001;
        mHzStats.sample(cycles);
    }
    unsigned n = mWcStats.n();
    if ((n & 127) == 1) {
        long long elapsed = mCpuUsage.elapsed();
        if (elapsed >= DEBUG_CPU_USAGE * 1000000000LL) {
            double perLoop = elapsed / (double) n;
            double perLoop100 = perLoop * 0.01;
            double perLoop1k = perLoop * 0.001;
            double mean = mWcStats.mean();
            double stddev = mWcStats.stddev();
            double minimum = mWcStats.minimum();
            double maximum = mWcStats.maximum();
            double meanCycles = mHzStats.mean();
            double stddevCycles = mHzStats.stddev();
            double minCycles = mHzStats.minimum();
            double maxCycles = mHzStats.maximum();
            mCpuUsage.resetElapsed();
            mWcStats.reset();
            mHzStats.reset();
            ALOGD("CPU usage for %s over past %.1f secs\n"
                "  (%u mixer loops at %.1f mean ms per loop):\n"
                "  us per mix loop: mean=%.0f stddev=%.0f min=%.0f max=%.0f\n"
                "  %% of wall: mean=%.1f stddev=%.1f min=%.1f max=%.1f\n"
                "  MHz: mean=%.1f, stddev=%.1f, min=%.1f max=%.1f",
                    title.string(),
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
AudioFlinger::ThreadBase::ThreadBase(const sp<AudioFlinger>& audioFlinger, audio_io_handle_t id,
        audio_devices_t outDevice, audio_devices_t inDevice, type_t type)
    : Thread(false ),
        mType(type),
        mAudioFlinger(audioFlinger),
        mParamStatus(NO_ERROR),
        mStandby(false), mOutDevice(outDevice), mInDevice(inDevice),
        mAudioSource(AUDIO_SOURCE_DEFAULT), mId(id),
        mDeathRecipient(new PMDeathRecipient(this))
{
}
AudioFlinger::ThreadBase::~ThreadBase()
{
    for (size_t i {
    for (size_t i = 0; i < mConfigEvents.size(); i++) {
        delete mConfigEvents[i];
    }
    mConfigEvents.clear();
    mParamCond.broadcast();
    releaseWakeLock_l();
    if (mPowerManager != 0) {
        sp<IBinder> binder = mPowerManager->asBinder();
        binder->unlinkToDeath(mDeathRecipient);
    }
}
status_t AudioFlinger::ThreadBase::readyToRun()
{
    status_t status = initCheck();
    if (status == NO_ERROR) {
        ALOGI("AudioFlinger's thread %p ready to run", this);
    } else {
        ALOGE("No working audio driver found.");
    }
    return status;
}
void AudioFlinger::ThreadBase::exit()
{
    ALOGV("ThreadBase::exit");
    preExit();
    {
        AutoMutex lock(mLock);
        requestExit();
        mWaitWorkCV.broadcast();
    }
    requestExitAndWait();
}
status_t AudioFlinger::ThreadBase::setParameters(const String8& keyValuePairs)
{
    status_t status;
    ALOGV("ThreadBase::setParameters() %s", keyValuePairs.string());
    Mutex::Autolock _l(mLock);
    mNewParameters.add(keyValuePairs);
    mWaitWorkCV.signal();
    if (mParamCond.waitRelative(mLock, kSetParametersTimeoutNs) == NO_ERROR) {
        status = mParamStatus;
        mWaitWorkCV.signal();
    } else {
        status = TIMED_OUT;
    }
    return status;
}
void AudioFlinger::ThreadBase::sendIoConfigEvent(int event, int param)
{
    Mutex::Autolock _l(mLock);
    sendIoConfigEvent_l(event, param);
}
void AudioFlinger::ThreadBase::sendIoConfigEvent_l(int event, int param)
{
    IoConfigEvent *ioEvent = new IoConfigEvent(event, param);
    mConfigEvents.add(static_cast<ConfigEvent *>(ioEvent));
    ALOGV("sendIoConfigEvent() num events %d event %d, param %d", mConfigEvents.size(), event,
            param);
    mWaitWorkCV.signal();
}
void AudioFlinger::ThreadBase::sendPrioConfigEvent_l(pid_t pid, pid_t tid, int32_t prio)
{
    PrioConfigEvent *prioEvent = new PrioConfigEvent(pid, tid, prio);
    mConfigEvents.add(static_cast<ConfigEvent *>(prioEvent));
    ALOGV("sendPrioConfigEvent_l() num events %d pid %d, tid %d prio %d",
          mConfigEvents.size(), pid, tid, prio);
    mWaitWorkCV.signal();
}
void AudioFlinger::ThreadBase::processConfigEvents()
{
    Mutex::Autolock _l(mLock);
    processConfigEvents_l();
}
void AudioFlinger::ThreadBase::processConfigEvents_l()
{
    while (!mConfigEvents.isEmpty()) {
        ALOGV("processConfigEvents() remaining events %d", mConfigEvents.size());
        ConfigEvent *event = mConfigEvents[0];
        mConfigEvents.removeAt(0);
        mLock.unlock();
        switch (event->type()) {
        case CFG_EVENT_PRIO: {
            PrioConfigEvent *prioEvent = static_cast<PrioConfigEvent *>(event);
            int err = requestPriority(prioEvent->pid(), prioEvent->tid(), prioEvent->prio(),
                    true );
            if (err != 0) {
                ALOGW("Policy SCHED_FIFO priority %d is unavailable for pid %d tid %d; error %d",
                      prioEvent->prio(), prioEvent->pid(), prioEvent->tid(), err);
            }
        } break;
        case CFG_EVENT_IO: {
            IoConfigEvent *ioEvent = static_cast<IoConfigEvent *>(event);
            {
                Mutex::Autolock _l(mAudioFlinger->mLock);
                audioConfigChanged_l(ioEvent->event(), ioEvent->param());
            }
        } break;
        default:
            ALOGE("processConfigEvents() unknown event type %d", event->type());
            break;
        }
        delete event;
        mLock.lock();
    }
}
String8 channelMaskToString(audio_channel_mask_t mask, bool output) {
    String8 s;
    if (output) {
        if (mask & AUDIO_CHANNEL_OUT_FRONT_LEFT) s.append("front-left, ");
        if (mask & AUDIO_CHANNEL_OUT_FRONT_RIGHT) s.append("front-right, ");
        if (mask & AUDIO_CHANNEL_OUT_FRONT_CENTER) s.append("front-center, ");
        if (mask & AUDIO_CHANNEL_OUT_LOW_FREQUENCY) s.append("low freq, ");
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
        if (mask & AUDIO_CHANNEL_OUT_TOP_BACK_CENTER) s.append("top-back-center, " );
        if (mask & AUDIO_CHANNEL_OUT_TOP_BACK_RIGHT) s.append("top-back-right, " );
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
        if (mask & AUDIO_CHANNEL_IN_VOICE_UPLINK) s.append("voice-uplink, ");
        if (mask & AUDIO_CHANNEL_IN_VOICE_DNLINK) s.append("voice-dnlink, ");
        if (mask & ~AUDIO_CHANNEL_IN_ALL) s.append("unknown,  ");
    }
    int len = s.length();
    if (s.length() > 2) {
        char *str = s.lockBuffer(len);
        s.unlockBuffer(len - 2);
    }
    return s;
}
void AudioFlinger::ThreadBase::dumpBase(int fd, const Vector<String16>& args __unused)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    bool locked = AudioFlinger::dumpTryLock(mLock);
    if (!locked) {
        fdprintf(fd, "thread %p maybe dead locked\n", this);
    }
<<<<<<< HEAD
    fdprintf(fd, "  I/O handle: %d\n", mId);
    fdprintf(fd, "  TID: %d\n", getTid());
    fdprintf(fd, "  Standby: %s\n", mStandby ? "yes" : "no");
    fdprintf(fd, "  Sample rate: %u\n", mSampleRate);
    fdprintf(fd, "  HAL frame count: %d\n", mFrameCount);
    fdprintf(fd, "  HAL buffer size: %u bytes\n", mBufferSize);
    fdprintf(fd, "  Channel Count: %u\n", mChannelCount);
    fdprintf(fd, "  Channel Mask: 0x%08x (%s)\n", mChannelMask,
            channelMaskToString(mChannelMask, mType != RECORD).string());
    fdprintf(fd, "  Format: 0x%x (%s)\n", mFormat, formatToString(mFormat));
    fdprintf(fd, "  Frame size: %u\n", mFrameSize);
    fdprintf(fd, "  Pending setParameters commands:");
    size_t numParams = mNewParameters.size();
    if (numParams) {
        fdprintf(fd, "\n   Index Command");
        for (size_t i = 0; i < numParams; ++i) {
            fdprintf(fd, "\n   %02d    ", i);
            fdprintf(fd, mNewParameters[i]);
||||||| 66e05682e4
    snprintf(buffer, SIZE, "io handle: %d\n", mId);
    result.append(buffer);
    snprintf(buffer, SIZE, "TID: %d\n", getTid());
    result.append(buffer);
    snprintf(buffer, SIZE, "standby: %d\n", mStandby);
    result.append(buffer);
    snprintf(buffer, SIZE, "Sample rate: %u\n", mSampleRate);
    result.append(buffer);
    snprintf(buffer, SIZE, "HAL frame count: %d\n", mFrameCount);
    result.append(buffer);
    snprintf(buffer, SIZE, "Channel Count: %u\n", mChannelCount);
    result.append(buffer);
    snprintf(buffer, SIZE, "Channel Mask: 0x%08x\n", mChannelMask);
    result.append(buffer);
    snprintf(buffer, SIZE, "Format: %d\n", mFormat);
    result.append(buffer);
    snprintf(buffer, SIZE, "Frame size: %u\n", mFrameSize);
    result.append(buffer);
    snprintf(buffer, SIZE, "\nPending setParameters commands: \n");
    result.append(buffer);
    result.append(" Index Command");
    for (size_t i = 0; i < mNewParameters.size(); ++i) {
        snprintf(buffer, SIZE, "\n %02d    ", i);
        result.append(buffer);
        result.append(mNewParameters[i]);
=======
    snprintf(buffer, SIZE, "io handle: %d\n", mId);
    result.append(buffer);
    snprintf(buffer, SIZE, "TID: %d\n", getTid());
    result.append(buffer);
    snprintf(buffer, SIZE, "standby: %d\n", mStandby);
    result.append(buffer);
    snprintf(buffer, SIZE, "Sample rate: %u\n", mSampleRate);
    result.append(buffer);
    snprintf(buffer, SIZE, "HAL frame count: %zu\n", mFrameCount);
    result.append(buffer);
    snprintf(buffer, SIZE, "Channel Count: %u\n", mChannelCount);
    result.append(buffer);
    snprintf(buffer, SIZE, "Channel Mask: 0x%08x\n", mChannelMask);
    result.append(buffer);
    snprintf(buffer, SIZE, "Format: %d\n", mFormat);
    result.append(buffer);
    snprintf(buffer, SIZE, "Frame size: %zu\n", mFrameSize);
    result.append(buffer);
    snprintf(buffer, SIZE, "\nPending setParameters commands: \n");
    result.append(buffer);
    result.append(" Index Command");
    for (size_t i = 0; i < mNewParameters.size(); ++i) {
        snprintf(buffer, SIZE, "\n %02zu    ", i);
        result.append(buffer);
        result.append(mNewParameters[i]);
>>>>>>> 566be7c3
        }
        fdprintf(fd, "\n");
    } else {
        fdprintf(fd, " none\n");
    }
    fdprintf(fd, "  Pending config events:");
    size_t numConfig = mConfigEvents.size();
    if (numConfig) {
        for (size_t i = 0; i < numConfig; i++) {
            mConfigEvents[i]->dump(buffer, SIZE);
            fdprintf(fd, "\n    %s", buffer);
        }
        fdprintf(fd, "\n");
    } else {
        fdprintf(fd, " none\n");
    }
    if (locked) {
        mLock.unlock();
    }
}
void AudioFlinger::ThreadBase::dumpEffectChains(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
<<<<<<< HEAD
    size_t numEffectChains = mEffectChains.size();
    snprintf(buffer, SIZE, "  %d Effect Chains\n", numEffectChains);
||||||| 66e05682e4
    snprintf(buffer, SIZE, "\n- %d Effect Chains:\n", mEffectChains.size());
=======
    snprintf(buffer, SIZE, "\n- %zu Effect Chains:\n", mEffectChains.size());
>>>>>>> 566be7c3
    write(fd, buffer, strlen(buffer));
    for (size_t i = 0; i < numEffectChains; ++i) {
        sp<EffectChain> chain = mEffectChains[i];
        if (chain != 0) {
            chain->dump(fd, args);
        }
    }
}
void AudioFlinger::ThreadBase::acquireWakeLock(int uid)
{
    Mutex::Autolock _l(mLock);
    acquireWakeLock_l(uid);
}
String16 AudioFlinger::ThreadBase::getWakeLockTag()
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
        default:
            ALOG_ASSERT(false);
            return String16("AudioUnknown");
    }
}
void AudioFlinger::ThreadBase::acquireWakeLock_l(int uid)
{
    getPowerManager_l();
    if (mPowerManager != 0) {
        sp<IBinder> binder = new BBinder();
        status_t status;
        if (uid >= 0) {
            status = mPowerManager->acquireWakeLockWithUid(POWERMANAGER_PARTIAL_WAKE_LOCK,
                    binder,
                    getWakeLockTag(),
                    String16("media"),
                    uid);
        } else {
            status = mPowerManager->acquireWakeLock(POWERMANAGER_PARTIAL_WAKE_LOCK,
                    binder,
                    getWakeLockTag(),
                    String16("media"));
        }
        if (status == NO_ERROR) {
            mWakeLockToken = binder;
        }
        ALOGV("acquireWakeLock_l() %s status %d", mName, status);
    }
}
void AudioFlinger::ThreadBase::releaseWakeLock()
{
    Mutex::Autolock _l(mLock);
    releaseWakeLock_l();
}
void AudioFlinger::ThreadBase::releaseWakeLock_l()
{
    if (mWakeLockToken != 0) {
        ALOGV("releaseWakeLock_l() %s", mName);
        if (mPowerManager != 0) {
            mPowerManager->releaseWakeLock(mWakeLockToken, 0);
        }
        mWakeLockToken.clear();
    }
}
void AudioFlinger::ThreadBase::updateWakeLockUids(const SortedVector<int> &uids) {
    Mutex::Autolock _l(mLock);
    updateWakeLockUids_l(uids);
}
void AudioFlinger::ThreadBase::getPowerManager_l() {
    if (mPowerManager == 0) {
        sp<IBinder> binder =
            defaultServiceManager()->checkService(String16("power"));
        if (binder == 0) {
            ALOGW("Thread %s cannot connect to the power manager service", mName);
        } else {
            mPowerManager = interface_cast<IPowerManager>(binder);
            binder->linkToDeath(mDeathRecipient);
        }
    }
}
void AudioFlinger::ThreadBase::updateWakeLockUids_l(const SortedVector<int> &uids) {
    getPowerManager_l();
    if (mWakeLockToken == NULL) {
        ALOGE("no wake lock to update!");
        return;
    }
    if (mPowerManager != 0) {
        sp<IBinder> binder = new BBinder();
        status_t status;
        status = mPowerManager->updateWakeLockUids(mWakeLockToken, uids.size(), uids.array());
        ALOGV("acquireWakeLock_l() %s status %d", mName, status);
    }
}
void AudioFlinger::ThreadBase::clearPowerManager()
{
    Mutex::Autolock _l(mLock);
    releaseWakeLock_l();
    mPowerManager.clear();
}
void AudioFlinger::ThreadBase::PMDeathRecipient::binderDied(const wp<IBinder>& who __unused)
{
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        thread->clearPowerManager();
    }
    ALOGW("power manager service died !!!");
}
void AudioFlinger::ThreadBase::setEffectSuspended(
        const effect_uuid_t *type, bool suspend, int sessionId)
{
    Mutex::Autolock _l(mLock);
    setEffectSuspended_l(type, suspend, sessionId);
}
void AudioFlinger::ThreadBase::setEffectSuspended_l(
        const effect_uuid_t *type, bool suspend, int sessionId)
{
    sp<EffectChain> chain = getEffectChain_l(sessionId);
    if (chain != 0) {
        if (type != NULL) {
            chain->setEffectSuspended_l(type, suspend);
        } else {
            chain->setEffectSuspendedAll_l(suspend);
        }
    }
    updateSuspendedSessions_l(type, suspend, sessionId);
}
void AudioFlinger::ThreadBase::checkSuspendOnAddEffectChain_l(const sp<EffectChain>& chain)
{
    ssize_t index = mSuspendedSessions.indexOfKey(chain->sessionId());
    if (index < 0) {
        return;
    }
    const KeyedVector <int, sp<SuspendedSessionDesc> >& sessionEffects =
            mSuspendedSessions.valueAt(index);
    for (size_t i = 0; i < sessionEffects.size(); i++) {
        sp<SuspendedSessionDesc> desc = sessionEffects.valueAt(i);
        for (int j = 0; j < desc->mRefCount; j++) {
            if (sessionEffects.keyAt(i) == EffectChain::kKeyForSuspendAll) {
                chain->setEffectSuspendedAll_l(true);
            } else {
                ALOGV("checkSuspendOnAddEffectChain_l() suspending effects %08x",
                    desc->mType.timeLow);
                chain->setEffectSuspended_l(&desc->mType, true);
            }
        }
    }
}
void AudioFlinger::ThreadBase::updateSuspendedSessions_l(const effect_uuid_t *type,
                                                         bool suspend,
                                                         int sessionId)
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
    int key = EffectChain::kKeyForSuspendAll;
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
void AudioFlinger::ThreadBase::checkSuspendOnEffectEnabled(const sp<EffectModule>& effect,
                                                            bool enabled,
                                                            int sessionId)
{
    Mutex::Autolock _l(mLock);
    checkSuspendOnEffectEnabled_l(effect, enabled, sessionId);
}
void AudioFlinger::ThreadBase::checkSuspendOnEffectEnabled_l(const sp<EffectModule>& effect,
                                                            bool enabled,
                                                            int sessionId)
{
    if (mType != RECORD) {
        if ((sessionId != AUDIO_SESSION_OUTPUT_MIX) && (sessionId != AUDIO_SESSION_OUTPUT_STAGE)) {
            setEffectSuspended_l(NULL, enabled, AUDIO_SESSION_OUTPUT_MIX);
        }
    }
    sp<EffectChain> chain = getEffectChain_l(sessionId);
    if (chain != 0) {
        chain->checkSuspendOnEffectEnabled(effect, enabled);
    }
}
sp<AudioFlinger::EffectHandle> AudioFlinger::ThreadBase::createEffect_l(
        const sp<AudioFlinger::Client>& client,
        const sp<IEffectClient>& effectClient,
        int32_t priority,
        int sessionId,
        effect_descriptor_t *desc,
        int *enabled,
        status_t *status)
{
    sp<EffectModule> effect;
    sp<EffectHandle> handle;
    status_t lStatus;
    sp<EffectChain> chain;
    bool chainCreated = false;
    bool effectCreated = false;
    bool effectRegistered = false;
    lStatus = initCheck();
    if (lStatus != NO_ERROR) {
        ALOGW("createEffect_l() Audio driver not initialized.");
        goto Exit;
    }
    if (sessionId == AUDIO_SESSION_OUTPUT_MIX) {
        switch (mType) {
        case MIXER:
        case OFFLOAD:
            break;
        case DIRECT:
        case DUPLICATING:
        case RECORD:
        default:
            ALOGW("createEffect_l() Cannot add global effect %s on thread %s", desc->name, mName);
            lStatus = BAD_VALUE;
            goto Exit;
        }
    }
    if ((mType == RECORD) != ((desc->flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_PRE_PROC)) {
        ALOGW("createEffect_l() effect %s (flags %08x) created on wrong thread type %d",
                desc->name, desc->flags, mType);
        lStatus = BAD_VALUE;
        goto Exit;
    }
    ALOGV("createEffect_l() thread %p effect %s on session %d", this, desc->name, sessionId);
    {
        Mutex::Autolock _l(mLock);
        chain = getEffectChain_l(sessionId);
        if (chain == 0) {
            ALOGV("createEffect_l() new effect chain for session %d", sessionId);
            chain = new EffectChain(this, sessionId);
            addEffectChain_l(chain);
            chain->setStrategy(getStrategyForSession_l(sessionId));
            chainCreated = true;
        } else {
            effect = chain->getEffectFromDesc_l(desc);
        }
        ALOGV("createEffect_l() got effect %p on chain %p", effect.get(), chain.get());
        if (effect == 0) {
            int id = mAudioFlinger->nextUniqueId();
            lStatus = AudioSystem::registerEffect(desc, mId, chain->strategy(), sessionId, id);
            if (lStatus != NO_ERROR) {
                goto Exit;
            }
            effectRegistered = true;
            effect = new EffectModule(this, chain, desc, id, sessionId);
            lStatus = effect->status();
            if (lStatus != NO_ERROR) {
                goto Exit;
            }
            effect->setOffloaded(mType == OFFLOAD, mId);
            lStatus = chain->addEffect_l(effect);
            if (lStatus != NO_ERROR) {
                goto Exit;
            }
            effectCreated = true;
            effect->setDevice(mOutDevice);
            effect->setDevice(mInDevice);
            effect->setMode(mAudioFlinger->getMode());
            effect->setAudioSource(mAudioSource);
        }
        handle = new EffectHandle(effect, client, effectClient, priority);
        lStatus = handle->initCheck();
        if (lStatus == OK) {
            lStatus = effect->addHandle(handle.get());
        }
        if (enabled != NULL) {
            *enabled = (int)effect->isEnabled();
        }
    }
Exit:
    if (lStatus != NO_ERROR && lStatus != ALREADY_EXISTS) {
        Mutex::Autolock _l(mLock);
        if (effectCreated) {
            chain->removeEffect_l(effect);
        }
        if (effectRegistered) {
            AudioSystem::unregisterEffect(effect->id());
        }
        if (chainCreated) {
            removeEffectChain_l(chain);
        }
        handle.clear();
    }
    *status = lStatus;
    return handle;
}
sp<AudioFlinger::EffectModule> AudioFlinger::ThreadBase::getEffect(int sessionId, int effectId)
{
    Mutex::Autolock _l(mLock);
    return getEffect_l(sessionId, effectId);
}
sp<AudioFlinger::EffectModule> AudioFlinger::ThreadBase::getEffect_l(int sessionId, int effectId)
{
    sp<EffectChain> chain = getEffectChain_l(sessionId);
    return chain != 0 ? chain->getEffectFromId_l(effectId) : 0;
}
status_t AudioFlinger::ThreadBase::addEffect_l(const sp<EffectModule>& effect)
{
    int sessionId = effect->sessionId();
    sp<EffectChain> chain = getEffectChain_l(sessionId);
    bool chainCreated = false;
    ALOGD_IF((mType == OFFLOAD) && !effect->isOffloadable(),
             "addEffect_l() on offloaded thread %p: effect %s does not support offload flags %x",
                    this, effect->desc().name, effect->desc().flags);
    if (chain == 0) {
        ALOGV("addEffect_l() new effect chain for session %d", sessionId);
        chain = new EffectChain(this, sessionId);
        addEffectChain_l(chain);
        chain->setStrategy(getStrategyForSession_l(sessionId));
        chainCreated = true;
    }
    ALOGV("addEffect_l() %p chain %p effect %p", this, chain.get(), effect.get());
    if (chain->getEffectFromId_l(effect->id()) != 0) {
        ALOGW("addEffect_l() %p effect %s already present in chain %p",
                this, effect->desc().name, chain.get());
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
    effect->setDevice(mOutDevice);
    effect->setDevice(mInDevice);
    effect->setMode(mAudioFlinger->getMode());
    effect->setAudioSource(mAudioSource);
    return NO_ERROR;
}
void AudioFlinger::ThreadBase::removeEffect_l(const sp<EffectModule>& effect) {
    ALOGV("removeEffect_l() %p effect %p", this, effect.get());
    effect_descriptor_t desc = effect->desc();
    if ((desc.flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_AUXILIARY) {
        detachAuxEffect_l(effect->id());
    }
    sp<EffectChain> chain = effect->chain().promote();
    if (chain != 0) {
        if (chain->removeEffect_l(effect) == 0) {
            removeEffectChain_l(chain);
        }
    } else {
        ALOGW("removeEffect_l() %p cannot promote chain for effect %p", this, effect.get());
    }
}
void AudioFlinger::ThreadBase::lockEffectChains_l(
        Vector< sp<AudioFlinger::EffectChain> >& effectChains)
{
    effectChains = mEffectChains;
    for (size_t i = 0; i < mEffectChains.size(); i++) {
        mEffectChains[i]->lock();
    }
}
void AudioFlinger::ThreadBase::unlockEffectChains(
        const Vector< sp<AudioFlinger::EffectChain> >& effectChains)
{
    for (size_t i = 0; i < effectChains.size(); i++) {
        effectChains[i]->unlock();
    }
}
sp<AudioFlinger::EffectChain> AudioFlinger::ThreadBase::getEffectChain(int sessionId)
{
    Mutex::Autolock _l(mLock);
    return getEffectChain_l(sessionId);
}
sp<AudioFlinger::EffectChain> AudioFlinger::ThreadBase::getEffectChain_l(int sessionId) const
{
    size_t size = mEffectChains.size();
    for (size_t i = 0; i < size; i++) {
        if (mEffectChains[i]->sessionId() == sessionId) {
            return mEffectChains[i];
        }
    }
    return 0;
}
void AudioFlinger::ThreadBase::setMode(audio_mode_t mode)
{
    Mutex::Autolock _l(mLock);
    size_t size = mEffectChains.size();
    for (size_t i = 0; i < size; i++) {
        mEffectChains[i]->setMode_l(mode);
    }
}
void AudioFlinger::ThreadBase::disconnectEffect(const sp<EffectModule>& effect,
                                                    EffectHandle *handle,
                                                    bool unpinIfLast) {
    Mutex::Autolock _l(mLock);
    ALOGV("disconnectEffect() %p effect %p", this, effect.get());
    if (effect->removeHandle(handle) == 0) {
        if (!effect->isPinned() || unpinIfLast) {
            removeEffect_l(effect);
            AudioSystem::unregisterEffect(effect->id());
        }
    }
}
AudioFlinger::PlaybackThread::PlaybackThread(const sp<AudioFlinger>& audioFlinger,
                                             AudioStreamOut* output,
                                             audio_io_handle_t id,
                                             audio_devices_t device,
                                             type_t type)
    : ThreadBase(audioFlinger, id, device, AUDIO_DEVICE_NONE, type),
        mNormalFrameCount(0), mMixBuffer(NULL),
        mSuspended(0), mBytesWritten(0),
        mActiveTracksGeneration(0),
        mOutput(output),
        mLastWriteTime(0), mNumWrites(0), mNumDelayedWrites(0), mInWrite(false),
        mMixerStatus(MIXER_IDLE),
        mMixerStatusIgnoringFastTracks(MIXER_IDLE),
        standbyDelay(AudioFlinger::mStandbyTimeInNsecs),
        mBytesRemaining(0),
        mCurrentWriteLength(0),
        mUseAsyncWrite(false),
        mWriteAckSequence(0),
        mDrainSequence(0),
        mSignalPending(false),
        mScreenState(AudioFlinger::mScreenState),
        mFastTrackAvailMask(((1 << FastMixerState::kMaxFastTracks) - 1) & ~1),
        mLatchDValid(false), mLatchQValid(false)
{
    snprintf(mName, kNameLength, "AudioOut_%X", id);
    mNBLogWriter = audioFlinger->newWriter_l(kLogSize, mName);
    mMasterVolume = audioFlinger->masterVolume_l();
    mMasterMute = audioFlinger->masterMute_l();
    if (mOutput && mOutput->audioHwDev) {
        if (mOutput->audioHwDev->canSetMasterVolume()) {
            mMasterVolume = 1.0;
        }
        if (mOutput->audioHwDev->canSetMasterMute()) {
            mMasterMute = false;
        }
    }
    readOutputParameters();
    for (audio_stream_type_t stream = (audio_stream_type_t) 0; stream < AUDIO_STREAM_CNT;
            stream = (audio_stream_type_t) (stream + 1)) {
        mStreamTypes[stream].volume = mAudioFlinger->streamVolume_l(stream);
        mStreamTypes[stream].mute = mAudioFlinger->streamMute_l(stream);
    }
}
AudioFlinger::PlaybackThread::~PlaybackThread()
{
    mAudioFlinger->unregisterWriter(mNBLogWriter);
    delete[] mMixBuffer;
}{
    mAudioFlinger->unregisterWriter(mNBLogWriter);
    delete[] mMixBuffer;
}
void AudioFlinger::PlaybackThread::dump(int fd, const Vector<String16>& args)
{
    dumpInternals(fd, args);
    dumpTracks(fd, args);
    dumpEffectChains(fd, args);
}
void AudioFlinger::PlaybackThread::dumpTracks(int fd, const Vector<String16>& args __unused)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
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
    write(fd, result.string(), result.length());
    result.clear();
    FastTrackUnderruns underruns = getFastTrackUnderruns(0);
    fdprintf(fd, "  Normal mixer raw underrun counters: partial=%u empty=%u\n",
            underruns.mBitFields.mPartial, underruns.mBitFields.mEmpty);
    size_t numtracks = mTracks.size();
    size_t numactive = mActiveTracks.size();
    fdprintf(fd, "  %d Tracks", numtracks);
    size_t numactiveseen = 0;
    if (numtracks) {
        fdprintf(fd, " of which %d are active\n", numactive);
        Track::appendDumpHeader(result);
        for (size_t i = 0; i < numtracks; ++i) {
            sp<Track> track = mTracks[i];
            if (track != 0) {
                bool active = mActiveTracks.indexOf(track) >= 0;
                if (active) {
                    numactiveseen++;
                }
                track->dump(buffer, SIZE, active);
                result.append(buffer);
            }
        }
    } else {
        result.append("\n");
    }
    if (numactiveseen != numactive) {
        snprintf(buffer, SIZE, "  The following tracks are in the active list but"
                " not in the track list\n");
        result.append(buffer);
        Track::appendDumpHeader(result);
        for (size_t i = 0; i < numactive; ++i) {
            sp<Track> track = mActiveTracks[i].promote();
            if (track != 0 && mTracks.indexOf(track) < 0) {
                track->dump(buffer, SIZE, true);
                result.append(buffer);
            }
        }
    }
    write(fd, result.string(), result.size());
}
void AudioFlinger::PlaybackThread::dumpInternals(int fd, const Vector<String16>& args)
{
<<<<<<< HEAD
    fdprintf(fd, "\nOutput thread %p:\n", this);
    fdprintf(fd, "  Normal frame count: %d\n", mNormalFrameCount);
    fdprintf(fd, "  Last write occurred (msecs): %llu\n", ns2ms(systemTime() - mLastWriteTime));
    fdprintf(fd, "  Total writes: %d\n", mNumWrites);
    fdprintf(fd, "  Delayed writes: %d\n", mNumDelayedWrites);
    fdprintf(fd, "  Blocked in write: %s\n", mInWrite ? "yes" : "no");
    fdprintf(fd, "  Suspend count: %d\n", mSuspended);
    fdprintf(fd, "  Mix buffer : %p\n", mMixBuffer);
||||||| 66e05682e4
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    snprintf(buffer, SIZE, "\nOutput thread %p internals\n", this);
    result.append(buffer);
    snprintf(buffer, SIZE, "Normal frame count: %d\n", mNormalFrameCount);
    result.append(buffer);
    snprintf(buffer, SIZE, "last write occurred (msecs): %llu\n",
            ns2ms(systemTime() - mLastWriteTime));
    result.append(buffer);
    snprintf(buffer, SIZE, "total writes: %d\n", mNumWrites);
    result.append(buffer);
    snprintf(buffer, SIZE, "delayed writes: %d\n", mNumDelayedWrites);
    result.append(buffer);
    snprintf(buffer, SIZE, "blocked in write: %d\n", mInWrite);
    result.append(buffer);
    snprintf(buffer, SIZE, "suspend count: %d\n", mSuspended);
    result.append(buffer);
    snprintf(buffer, SIZE, "mix buffer : %p\n", mMixBuffer);
    result.append(buffer);
    write(fd, result.string(), result.size());
=======
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    snprintf(buffer, SIZE, "\nOutput thread %p internals\n", this);
    result.append(buffer);
    snprintf(buffer, SIZE, "Normal frame count: %zu\n", mNormalFrameCount);
    result.append(buffer);
    snprintf(buffer, SIZE, "last write occurred (msecs): %llu\n",
            ns2ms(systemTime() - mLastWriteTime));
    result.append(buffer);
    snprintf(buffer, SIZE, "total writes: %d\n", mNumWrites);
    result.append(buffer);
    snprintf(buffer, SIZE, "delayed writes: %d\n", mNumDelayedWrites);
    result.append(buffer);
    snprintf(buffer, SIZE, "blocked in write: %d\n", mInWrite);
    result.append(buffer);
    snprintf(buffer, SIZE, "suspend count: %d\n", mSuspended);
    result.append(buffer);
    snprintf(buffer, SIZE, "mix buffer : %p\n", mMixBuffer);
    result.append(buffer);
    write(fd, result.string(), result.size());
>>>>>>> 566be7c3
    fdprintf(fd, "  Fast track availMask=%#x\n", mFastTrackAvailMask);
    dumpBase(fd, args);
}
void AudioFlinger::PlaybackThread::onFirstRef()
{
    run(mName, ANDROID_PRIORITY_URGENT_AUDIO);
}
void AudioFlinger::PlaybackThread::preExit()
{
    ALOGV("  preExit()");
    mOutput->stream->common.set_parameters(&mOutput->stream->common, "exiting=1");
}
sp<AudioFlinger::PlaybackThread::Track> AudioFlinger::PlaybackThread::createTrack_l(
        const sp<AudioFlinger::Client>& client,
        audio_stream_type_t streamType,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        size_t *pFrameCount,
        const sp<IMemory>& sharedBuffer,
        int sessionId,
        IAudioFlinger::track_flags_t *flags,
        pid_t tid,
        int uid,
        status_t *status)
{
    size_t frameCount = *pFrameCount;
    sp<Track> track;
    status_t lStatus;
    bool isTimed = (*flags & IAudioFlinger::TRACK_TIMED) != 0;
    if (*flags & IAudioFlinger::TRACK_FAST) {
      if (
            (!isTimed) &&
            (
              (
                (sharedBuffer != 0)
              ) ||
              (
                (tid != -1) &&
                ((frameCount == 0) ||
                (frameCount >= mFrameCount))
              )
            ) &&
            audio_is_linear_pcm(format) &&
            ( (channelMask == AUDIO_CHANNEL_OUT_MONO) ||
              (channelMask == AUDIO_CHANNEL_OUT_STEREO) ) &&
            (sampleRate == mSampleRate) &&
            hasFastMixer() &&
            (mFastTrackAvailMask != 0)
        ) {
        if (frameCount == 0) {
            frameCount = mFrameCount * kFastTrackMultiplier;
        }
        ALOGV("AUDIO_OUTPUT_FLAG_FAST accepted: frameCount=%d mFrameCount=%d",
                frameCount, mFrameCount);
      } else {
        ALOGV("AUDIO_OUTPUT_FLAG_FAST denied: isTimed=%d sharedBuffer=%p frameCount=%d "
                "mFrameCount=%d format=%d isLinear=%d channelMask=%#x sampleRate=%u mSampleRate=%u "
                "hasFastMixer=%d tid=%d fastTrackAvailMask=%#x",
                isTimed, sharedBuffer.get(), frameCount, mFrameCount, format,
                audio_is_linear_pcm(format),
                channelMask, sampleRate, mSampleRate, hasFastMixer(), tid, mFastTrackAvailMask);
        *flags &= ~IAudioFlinger::TRACK_FAST;
        uint32_t latencyMs = mOutput->stream->get_latency(mOutput->stream);
        uint32_t minBufCount = latencyMs / ((1000 * mNormalFrameCount) / mSampleRate);
        if (minBufCount < 2) {
            minBufCount = 2;
        }
        size_t minFrameCount = mNormalFrameCount * minBufCount;
        if (frameCount < minFrameCount) {
            frameCount = minFrameCount;
        }
      }
    }
    *pFrameCount = frameCount;
    if (mType == DIRECT) {
        if ((format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_PCM) {
            if (sampleRate != mSampleRate || format != mFormat || channelMask != mChannelMask) {
                ALOGE("createTrack_l() Bad parameter: sampleRate %u format %#x, channelMask 0x%08x "
                        "for output %p with format %#x",
                        sampleRate, format, channelMask, mOutput, mFormat);
                lStatus = BAD_VALUE;
                goto Exit;
            }
        }
    } else if (mType == OFFLOAD) {
        if (sampleRate != mSampleRate || format != mFormat || channelMask != mChannelMask) {
            ALOGE("createTrack_l() Bad parameter: sampleRate %d format %#x, channelMask 0x%08x \""
                    "for output %p with format %#x",
                    sampleRate, format, channelMask, mOutput, mFormat);
            lStatus = BAD_VALUE;
            goto Exit;
        }
    } else {
        if ((format & AUDIO_FORMAT_MAIN_MASK) != AUDIO_FORMAT_PCM) {
                ALOGE("createTrack_l() Bad parameter: format %#x \""
                        "for output %p with format %#x",
                        format, mOutput, mFormat);
                lStatus = BAD_VALUE;
                goto Exit;
        }
        if (sampleRate > mSampleRate*2) {
            ALOGE("Sample rate out of range: %u mSampleRate %u", sampleRate, mSampleRate);
            lStatus = BAD_VALUE;
            goto Exit;
        }
    }
    lStatus = initCheck();
    if (lStatus != NO_ERROR) {
        ALOGE("Audio driver not initialized.");
        goto Exit;
    }
    {
        Mutex::Autolock _l(mLock);
        uint32_t strategy = AudioSystem::getStrategyForStream(streamType);
        for (size_t i = 0; i < mTracks.size(); ++i) {
            sp<Track> t = mTracks[i];
            if (t != 0 && !t->isOutputTrack()) {
                uint32_t actual = AudioSystem::getStrategyForStream(t->streamType());
                if (sessionId == t->sessionId() && strategy != actual) {
                    ALOGE("createTrack_l() mismatched strategy; expected %u but found %u",
                            strategy, actual);
                    lStatus = BAD_VALUE;
                    goto Exit;
                }
            }
        }
        if (!isTimed) {
            track = new Track(this, client, streamType, sampleRate, format,
                    channelMask, frameCount, sharedBuffer, sessionId, uid, *flags);
        } else {
            track = TimedTrack::create(this, client, streamType, sampleRate, format,
                    channelMask, frameCount, sharedBuffer, sessionId, uid);
        }
        lStatus = track != 0 ? track->initCheck() : (status_t) NO_MEMORY;
        if (lStatus != NO_ERROR) {
            ALOGE("createTrack_l() initCheck failed %d; no control block?", lStatus);
            goto Exit;
        }
        mTracks.add(track);
        sp<EffectChain> chain = getEffectChain_l(sessionId);
        if (chain != 0) {
            ALOGV("createTrack_l() setting main buffer %p", chain->inBuffer());
            track->setMainBuffer(chain->inBuffer());
            chain->setStrategy(AudioSystem::getStrategyForStream(track->streamType()));
            chain->incTrackCnt();
        }
        if ((*flags & IAudioFlinger::TRACK_FAST) && (tid != -1)) {
            pid_t callingPid = IPCThreadState::self()->getCallingPid();
            sendPrioConfigEvent_l(callingPid, tid, kPriorityAudioApp);
        }
    }
    lStatus = NO_ERROR;
Exit:
    *status = lStatus;
    return track;
}
uint32_t AudioFlinger::PlaybackThread::correctLatency_l(uint32_t latency) const
{
    return latency;
}
uint32_t AudioFlinger::PlaybackThread::latency() const
{
    Mutex::Autolock _l(mLock);
    return latency_l();
}
uint32_t AudioFlinger::PlaybackThread::latency_l() const
{
    if (initCheck() == NO_ERROR) {
        return correctLatency_l(mOutput->stream->get_latency(mOutput->stream));
    } else {
        return 0;
    }
}
void AudioFlinger::PlaybackThread::setMasterVolume(float value)
{
    Mutex::Autolock _l(mLock);
    if (mOutput && mOutput->audioHwDev &&
        mOutput->audioHwDev->canSetMasterVolume()) {
        mMasterVolume = 1.0;
    } else {
        mMasterVolume = value;
    }
}
void AudioFlinger::PlaybackThread::setMasterMute(bool muted)
{
    Mutex::Autolock _l(mLock);
    if (mOutput && mOutput->audioHwDev &&
        mOutput->audioHwDev->canSetMasterMute()) {
        mMasterMute = false;
    } else {
        mMasterMute = muted;
    }
}
void AudioFlinger::PlaybackThread::setStreamVolume(audio_stream_type_t stream, float value)
{
    Mutex::Autolock _l(mLock);
    mStreamTypes[stream].volume = value;
    broadcast_l();
}
void AudioFlinger::PlaybackThread::setStreamMute(audio_stream_type_t stream, bool muted)
{
    Mutex::Autolock _l(mLock);
    mStreamTypes[stream].mute = muted;
    broadcast_l();
}
float AudioFlinger::PlaybackThread::streamVolume(audio_stream_type_t stream) const
{
    Mutex::Autolock _l(mLock);
    return mStreamTypes[stream].volume;
}
status_t AudioFlinger::PlaybackThread::addTrack_l(const sp<Track>& track)
{
    status_t status = ALREADY_EXISTS;
    track->mRetryCount = kMaxTrackStartupRetries;
    if (mActiveTracks.indexOf(track) < 0) {
        if (!track->isOutputTrack()) {
            TrackBase::track_state state = track->mState;
            mLock.unlock();
            status = AudioSystem::startOutput(mId, track->streamType(), track->sessionId());
            mLock.lock();
            if (state != track->mState) {
                if (status == NO_ERROR) {
                    mLock.unlock();
                    AudioSystem::stopOutput(mId, track->streamType(), track->sessionId());
                    mLock.lock();
                }
                return INVALID_OPERATION;
            }
            if (status != NO_ERROR) {
                return PERMISSION_DENIED;
            }
#ifdef ADD_BATTERY_DATA
            addBatteryData(IMediaPlayerService::kBatteryDataAudioFlingerStart);
#endif
        }
        track->mFillingUpStatus = track->sharedBuffer() != 0 ? Track::FS_FILLED : Track::FS_FILLING;
        track->mResetDone = false;
        track->mPresentationCompleteFrames = 0;
        mActiveTracks.add(track);
        mWakeLockUids.add(track->uid());
        mActiveTracksGeneration++;
        mLatestActiveTrack = track;
        sp<EffectChain> chain = getEffectChain_l(track->sessionId());
        if (chain != 0) {
            ALOGV("addTrack_l() starting track on chain %p for session %d", chain.get(),
                    track->sessionId());
            chain->incActiveTrackCnt();
        }
        status = NO_ERROR;
    }
    onAddNewTrack_l();
    return status;
}
bool AudioFlinger::PlaybackThread::destroyTrack_l(const sp<Track>& track)
{
    track->terminate();
    bool trackActive = (mActiveTracks.indexOf(track) >= 0);
    track->mState = TrackBase::STOPPED;
    if (!trackActive) {
        removeTrack_l(track);
    } else if (track->isFastTrack() || track->isOffloaded()) {
        track->mState = TrackBase::STOPPING_1;
    }
    return trackActive;
}
void AudioFlinger::PlaybackThread::removeTrack_l(const sp<Track>& track)
{
    track->triggerEvents(AudioSystem::SYNC_EVENT_PRESENTATION_COMPLETE);
    mTracks.remove(track);
    deleteTrackName_l(track->name());
    track->mName = -1;
    if (track->isFastTrack()) {
        int index = track->mFastIndex;
        ALOG_ASSERT(0 < index && index < (int)FastMixerState::kMaxFastTracks);
        ALOG_ASSERT(!(mFastTrackAvailMask & (1 << index)));
        mFastTrackAvailMask |= 1 << index;
        track->mFastIndex = -1;
    }
    sp<EffectChain> chain = getEffectChain_l(track->sessionId());
    if (chain != 0) {
        chain->decTrackCnt();
    }
}
void AudioFlinger::PlaybackThread::broadcast_l()
{
    mSignalPending = true;
    mWaitWorkCV.broadcast();
}
String8 AudioFlinger::PlaybackThread::getParameters(const String8& keys)
{
    Mutex::Autolock _l(mLock);
    if (initCheck() != NO_ERROR) {
        return String8();
    }
    char *s = mOutput->stream->common.get_parameters(&mOutput->stream->common, keys.string());
    const String8 out_s8(s);
    free(s);
    return out_s8;
}
void AudioFlinger::PlaybackThread::audioConfigChanged_l(int event, int param) {
    AudioSystem::OutputDescriptor desc;
    void *param2 = NULL;
    ALOGV("PlaybackThread::audioConfigChanged_l, thread %p, event %d, param %d", this, event,
            param);
    switch (event) {
    case AudioSystem::OUTPUT_OPENED:
    case AudioSystem::OUTPUT_CONFIG_CHANGED:
        desc.channelMask = mChannelMask;
        desc.samplingRate = mSampleRate;
        desc.format = mFormat;
        desc.frameCount = mNormalFrameCount;
        desc.latency = latency();
        param2 = &desc;
        break;
    case AudioSystem::STREAM_CONFIG_CHANGED:
        param2 = &param;
    case AudioSystem::OUTPUT_CLOSED:
    default:
        break;
    }
    mAudioFlinger->audioConfigChanged_l(event, mId, param2);
}
void AudioFlinger::PlaybackThread::writeCallback()
{
    ALOG_ASSERT(mCallbackThread != 0);
    mCallbackThread->resetWriteBlocked();
}
void AudioFlinger::PlaybackThread::drainCallback()
{
    ALOG_ASSERT(mCallbackThread != 0);
    mCallbackThread->resetDraining();
}
void AudioFlinger::PlaybackThread::resetWriteBlocked(uint32_t sequence)
{
    Mutex::Autolock _l(mLock);
    if ((mWriteAckSequence & 1) && (sequence == mWriteAckSequence)) {
        mWriteAckSequence &= ~1;
        mWaitWorkCV.signal();
    }
}
void AudioFlinger::PlaybackThread::resetDraining(uint32_t sequence)
{
    Mutex::Autolock _l(mLock);
    if ((mDrainSequence & 1) && (sequence == mDrainSequence)) {
        mDrainSequence &= ~1;
        mWaitWorkCV.signal();
    }
}
int AudioFlinger::PlaybackThread::asyncCallback(stream_callback_event_t event,
                                                void *param __unused,
                                                void *cookie)
{
    AudioFlinger::PlaybackThread *me = (AudioFlinger::PlaybackThread *)cookie;
    ALOGV("asyncCallback() event %d", event);
    switch (event) {
    case STREAM_CBK_EVENT_WRITE_READY:
        me->writeCallback();
        break;
    case STREAM_CBK_EVENT_DRAIN_READY:
        me->drainCallback();
        break;
    default:
        ALOGW("asyncCallback() unknown event %d", event);
        break;
    }
    return 0;
}
void AudioFlinger::PlaybackThread::readOutputParameters()
{
    mSampleRate = mOutput->stream->common.get_sample_rate(&mOutput->stream->common);
    mChannelMask = mOutput->stream->common.get_channels(&mOutput->stream->common);
    if (!audio_is_output_channel(mChannelMask)) {
        LOG_FATAL("HAL channel mask %#x not valid for output", mChannelMask);
    }
    if ((mType == MIXER || mType == DUPLICATING) && mChannelMask != AUDIO_CHANNEL_OUT_STEREO) {
        LOG_FATAL("HAL channel mask %#x not supported for mixed output; "
                "must be AUDIO_CHANNEL_OUT_STEREO", mChannelMask);
    }
    mChannelCount = popcount(mChannelMask);
    mFormat = mOutput->stream->common.get_format(&mOutput->stream->common);
    if (!audio_is_valid_format(mFormat)) {
        LOG_FATAL("HAL format %#x not valid for output", mFormat);
    }
    if ((mType == MIXER || mType == DUPLICATING) && mFormat != AUDIO_FORMAT_PCM_16_BIT) {
        LOG_FATAL("HAL format %#x not supported for mixed output; must be AUDIO_FORMAT_PCM_16_BIT",
                mFormat);
    }
    mFrameSize = audio_stream_frame_size(&mOutput->stream->common);
    mBufferSize = mOutput->stream->common.get_buffer_size(&mOutput->stream->common);
    mFrameCount = mBufferSize / mFrameSize;
    if (mFrameCount & 15) {
        ALOGW("HAL output buffer size is %u frames but AudioMixer requires multiples of 16 frames",
                mFrameCount);
    }
    if ((mOutput->flags & AUDIO_OUTPUT_FLAG_NON_BLOCKING) &&
            (mOutput->stream->set_callback != NULL)) {
        if (mOutput->stream->set_callback(mOutput->stream,
                                      AudioFlinger::PlaybackThread::asyncCallback, this) == 0) {
            mUseAsyncWrite = true;
            mCallbackThread = new AudioFlinger::AsyncCallbackThread(this);
        }
    }
    double multiplier = 1.0;
    if (mType == MIXER && (kUseFastMixer == FastMixer_Static ||
            kUseFastMixer == FastMixer_Dynamic)) {
        size_t minNormalFrameCount = (kMinNormalMixBufferSizeMs * mSampleRate) / 1000;
        size_t maxNormalFrameCount = (kMaxNormalMixBufferSizeMs * mSampleRate) / 1000;
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
            uint32_t truncMult = (uint32_t) multiplier;
            if ((truncMult & 1)) {
                if ((truncMult + 1) * mFrameCount <= maxNormalFrameCount) {
                    ++truncMult;
                }
            }
            multiplier = (double) truncMult;
        }
    }
    mNormalFrameCount = multiplier * mFrameCount;
    mNormalFrameCount = (mNormalFrameCount + 15) & ~15;
    ALOGI("HAL output buffer size %u frames, normal mix buffer size %u frames", mFrameCount,
            mNormalFrameCount);
    delete[] mMixBuffer;
    size_t normalBufferSize = mNormalFrameCount * mFrameSize;
    mMixBuffer = new int16_t[(normalBufferSize + 1) >> 1];
    memset(mMixBuffer, 0, normalBufferSize);
    Vector< sp<EffectChain> > effectChains = mEffectChains;
    for (size_t i = 0; i < effectChains.size(); i ++) {
        mAudioFlinger->moveEffectChain_l(effectChains[i]->sessionId(), this, this, false);
    }
}
status_t AudioFlinger::PlaybackThread::getRenderPosition(uint32_t *halFrames, uint32_t *dspFrames)
{
    if (halFrames == NULL || dspFrames == NULL) {
        return BAD_VALUE;
    }
    Mutex::Autolock _l(mLock);
    if (initCheck() != NO_ERROR) {
        return INVALID_OPERATION;
    }
    size_t framesWritten = mBytesWritten / mFrameSize;
    *halFrames = framesWritten;
    if (isSuspended()) {
        size_t latencyFrames = (latency_l() * mSampleRate) / 1000;
        *dspFrames = framesWritten >= latencyFrames ? framesWritten - latencyFrames : 0;
        return NO_ERROR;
    } else {
        status_t status;
        uint32_t frames;
        status = mOutput->stream->get_render_position(mOutput->stream, &frames);
        *dspFrames = (size_t)frames;
        return status;
    }
}
uint32_t AudioFlinger::PlaybackThread::hasAudioSession(int sessionId) const
{
    Mutex::Autolock _l(mLock);
    uint32_t result = 0;
    if (getEffectChain_l(sessionId) != 0) {
        result = EFFECT_SESSION;
    }
    for (size_t i = 0; i < mTracks.size(); ++i) {
        sp<Track> track = mTracks[i];
        if (sessionId == track->sessionId() && !track->isInvalid()) {
            result |= TRACK_SESSION;
            break;
        }
    }
    return result;
}
uint32_t AudioFlinger::PlaybackThread::getStrategyForSession_l(int sessionId)
{
    if (sessionId == AUDIO_SESSION_OUTPUT_MIX) {
        return AudioSystem::getStrategyForStream(AUDIO_STREAM_MUSIC);
    }
    for (size_t i = 0; i < mTracks.size(); i++) {
        sp<Track> track = mTracks[i];
        if (sessionId == track->sessionId() && !track->isInvalid()) {
            return AudioSystem::getStrategyForStream(track->streamType());
        }
    }
    return AudioSystem::getStrategyForStream(AUDIO_STREAM_MUSIC);
}
AudioFlinger::AudioStreamOut* AudioFlinger::PlaybackThread::getOutput() const
{
    Mutex::Autolock _l(mLock);
    return mOutput;
}
AudioFlinger::AudioStreamOut* AudioFlinger::PlaybackThread::clearOutput()
{
    Mutex::Autolock _l(mLock);
    AudioStreamOut *output = mOutput;
    mOutput = NULL;
    mOutputSink.clear();
    mPipeSink.clear();
    mNormalSink.clear();
    return output;
}
audio_stream_t* AudioFlinger::PlaybackThread::stream() const
{
    if (mOutput == NULL) {
        return NULL;
    }
    return &mOutput->stream->common;
}
uint32_t AudioFlinger::PlaybackThread::activeSleepTimeUs() const
{
    return (uint32_t)((uint32_t)((mNormalFrameCount * 1000) / mSampleRate) * 1000);
}
status_t AudioFlinger::PlaybackThread::setSyncEvent(const sp<SyncEvent>& event)
{
    if (!isValidSyncEvent(event)) {
        return BAD_VALUE;
    }
    Mutex::Autolock _l(mLock);
    for (size_t i = 0; i < mTracks.size(); ++i) {
        sp<Track> track = mTracks[i];
        if (event->triggerSession() == track->sessionId()) {
            (void) track->setSyncEvent(event);
            return NO_ERROR;
        }
    }
    return NAME_NOT_FOUND;
}
bool AudioFlinger::PlaybackThread::isValidSyncEvent(const sp<SyncEvent>& event) const
{
    return event->type() == AudioSystem::SYNC_EVENT_PRESENTATION_COMPLETE;
}
void AudioFlinger::PlaybackThread::threadLoop_removeTracks(
        const Vector< sp<Track> >& tracksToRemove)
{
    size_t count = tracksToRemove.size();
    if (count > 0) {
        for (size_t i = 0 ; i < count ; i++) {
            const sp<Track>& track = tracksToRemove.itemAt(i);
            if (!track->isOutputTrack()) {
                AudioSystem::stopOutput(mId, track->streamType(), track->sessionId());
#ifdef ADD_BATTERY_DATA
                addBatteryData(IMediaPlayerService::kBatteryDataAudioFlingerStop);
#endif
                if (track->isTerminated()) {
                    AudioSystem::releaseOutput(mId);
                }
            }
        }
    }
}
void AudioFlinger::PlaybackThread::checkSilentMode_l()
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
ssize_t AudioFlinger::PlaybackThread::threadLoop_write()
{
    mLastWriteTime = systemTime();
    mInWrite = true;
    ssize_t bytesWritten;
    if (mNormalSink != 0) {
#define mBitShift 2
        size_t count = mBytesRemaining >> mBitShift;
        size_t offset = (mCurrentWriteLength - mBytesRemaining) >> 1;
        ATRACE_BEGIN("write");
        uint32_t screenState = AudioFlinger::mScreenState;
        if (screenState != mScreenState) {
            mScreenState = screenState;
            MonoPipe *pipe = (MonoPipe *)mPipeSink.get();
            if (pipe != NULL) {
                pipe->setAvgFrames((mScreenState & 1) ?
                        (pipe->maxFrames() * 7) / 8 : mNormalFrameCount * 2);
            }
        }
        ssize_t framesWritten = mNormalSink->write(mMixBuffer + offset, count);
        ATRACE_END();
        if (framesWritten > 0) {
            bytesWritten = framesWritten << mBitShift;
        } else {
            bytesWritten = framesWritten;
        }
        status_t status = mNormalSink->getTimestamp(mLatchD.mTimestamp);
        if (status == NO_ERROR) {
            size_t totalFramesWritten = mNormalSink->framesWritten();
            if (totalFramesWritten >= mLatchD.mTimestamp.mPosition) {
                mLatchD.mUnpresentedFrames = totalFramesWritten - mLatchD.mTimestamp.mPosition;
                mLatchDValid = true;
            }
        }
    } else {
        size_t offset = (mCurrentWriteLength - mBytesRemaining);
        if (mUseAsyncWrite) {
            ALOGW_IF(mWriteAckSequence & 1, "threadLoop_write(): out of sequence write request");
            mWriteAckSequence += 2;
            mWriteAckSequence |= 1;
            ALOG_ASSERT(mCallbackThread != 0);
            mCallbackThread->setWriteBlocked(mWriteAckSequence);
        }
        bytesWritten = mOutput->stream->write(mOutput->stream,
                                                   (char *)mMixBuffer + offset, mBytesRemaining);
        if (mUseAsyncWrite &&
                ((bytesWritten < 0) || (bytesWritten == (ssize_t)mBytesRemaining))) {
            mWriteAckSequence &= ~1;
            ALOG_ASSERT(mCallbackThread != 0);
            mCallbackThread->setWriteBlocked(mWriteAckSequence);
        }
    }
    mNumWrites++;
    mInWrite = false;
    mStandby = false;
    return bytesWritten;
}
void AudioFlinger::PlaybackThread::threadLoop_drain()
{
    if (mOutput->stream->drain) {
        ALOGV("draining %s", (mMixerStatus == MIXER_DRAIN_TRACK) ? "early" : "full");
        if (mUseAsyncWrite) {
            ALOGW_IF(mDrainSequence & 1, "threadLoop_drain(): out of sequence drain request");
            mDrainSequence |= 1;
            ALOG_ASSERT(mCallbackThread != 0);
            mCallbackThread->setDraining(mDrainSequence);
        }
        mOutput->stream->drain(mOutput->stream,
            (mMixerStatus == MIXER_DRAIN_TRACK) ? AUDIO_DRAIN_EARLY_NOTIFY
                                                : AUDIO_DRAIN_ALL);
    }
}
void AudioFlinger::PlaybackThread::threadLoop_exit()
{
}
void AudioFlinger::PlaybackThread::cacheParameters_l()
{
    mixBufferSize = mNormalFrameCount * mFrameSize;
    activeSleepTime = activeSleepTimeUs();
    idleSleepTime = idleSleepTimeUs();
}
void AudioFlinger::PlaybackThread::invalidateTracks(audio_stream_type_t streamType)
{
    ALOGV("MixerThread::invalidateTracks() mixer %p, streamType %d, mTracks.size %d",
            this, streamType, mTracks.size());
    Mutex::Autolock _l(mLock);
    size_t size = mTracks.size();
    for (size_t i = 0; i < size; i++) {
        sp<Track> t = mTracks[i];
        if (t->streamType() == streamType) {
            t->invalidate();
        }
    }
}
status_t AudioFlinger::PlaybackThread::addEffectChain_l(const sp<EffectChain>& chain)
{
    int session = chain->sessionId();
    int16_t *buffer = mMixBuffer;
    bool ownsBuffer = false;
    ALOGV("addEffectChain_l() %p on thread %p for session %d", chain.get(), this, session);
    if (session > 0) {
        if (mType != DIRECT) {
            size_t numSamples = mNormalFrameCount * mChannelCount;
            buffer = new int16_t[numSamples];
            memset(buffer, 0, numSamples * sizeof(int16_t));
            ALOGV("addEffectChain_l() creating new input buffer %p session %d", buffer, session);
            ownsBuffer = true;
        }
        for (size_t i = 0; i < mTracks.size(); ++i) {
            sp<Track> track = mTracks[i];
            if (session == track->sessionId()) {
                ALOGV("addEffectChain_l() track->setMainBuffer track %p buffer %p", track.get(),
                        buffer);
                track->setMainBuffer(buffer);
                chain->incTrackCnt();
            }
        }
        for (size_t i = 0 ; i < mActiveTracks.size() ; ++i) {
            sp<Track> track = mActiveTracks[i].promote();
            if (track == 0) {
                continue;
            }
            if (session == track->sessionId()) {
                ALOGV("addEffectChain_l() activating track %p on session %d", track.get(), session);
                chain->incActiveTrackCnt();
            }
        }
    }
    chain->setInBuffer(buffer, ownsBuffer);
    chain->setOutBuffer(mMixBuffer);
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
size_t AudioFlinger::PlaybackThread::removeEffectChain_l(const sp<EffectChain>& chain)
{
    int session = chain->sessionId();
    ALOGV("removeEffectChain_l() %p from thread %p for session %d", chain.get(), this, session);
    for (size_t i = 0; i < mEffectChains.size(); i++) {
        if (chain == mEffectChains[i]) {
            mEffectChains.removeAt(i);
            for (size_t i = 0 ; i < mActiveTracks.size() ; ++i) {
                sp<Track> track = mActiveTracks[i].promote();
                if (track == 0) {
                    continue;
                }
                if (session == track->sessionId()) {
                    ALOGV("removeEffectChain_l(): stopping track on chain %p for session Id: %d",
                            chain.get(), session);
                    chain->decActiveTrackCnt();
                }
            }
            for (size_t i = 0; i < mTracks.size(); ++i) {
                sp<Track> track = mTracks[i];
                if (session == track->sessionId()) {
                    track->setMainBuffer(mMixBuffer);
                    chain->decTrackCnt();
                }
            }
            break;
        }
    }
    return mEffectChains.size();
}
status_t AudioFlinger::PlaybackThread::attachAuxEffect(
        const sp<AudioFlinger::PlaybackThread::Track> track, int EffectId)
{
    Mutex::Autolock _l(mLock);
    return attachAuxEffect_l(track, EffectId);
}
status_t AudioFlinger::PlaybackThread::attachAuxEffect_l(
        const sp<AudioFlinger::PlaybackThread::Track> track, int EffectId)
{
    status_t status = NO_ERROR;
    if (EffectId == 0) {
        track->setAuxBuffer(0, NULL);
    } else {
        sp<EffectModule> effect = getEffect_l(AUDIO_SESSION_OUTPUT_MIX, EffectId);
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
void AudioFlinger::PlaybackThread::detachAuxEffect_l(int effectId)
{
    for (size_t i = 0; i < mTracks.size(); ++i) {
        sp<Track> track = mTracks[i];
        if (track->auxEffectId() == effectId) {
            attachAuxEffect_l(track, 0);
        }
    }
}
bool AudioFlinger::PlaybackThread::threadLoop()
{
    Vector< sp<Track> > tracksToRemove;
    standbyTime = systemTime();
    nsecs_t lastWarning = 0;
    writeFrames = 0;
    int lastGeneration = 0;
    cacheParameters_l();
    sleepTime = idleSleepTime;
    if (mType == MIXER) {
        sleepTimeShift = 0;
    }
    CpuStats cpuStats;
    const String8 myName(String8::format("thread %p type %d TID %d", this, mType, gettid()));
    acquireWakeLock();
    const char *logString = NULL;
    checkSilentMode_l();
    while (!exitPending())
    {
        cpuStats.sample(myName);
        Vector< sp<EffectChain> > effectChains;
        processConfigEvents();
        {
            Mutex::Autolock _l(mLock);
            if (logString != NULL) {
                mNBLogWriter->logTimestamp();
                mNBLogWriter->log(logString);
                logString = NULL;
            }
            if (mLatchDValid) {
                mLatchQ = mLatchD;
                mLatchDValid = false;
                mLatchQValid = true;
            }
            if (checkForNewParameters_l()) {
                cacheParameters_l();
            }
            saveOutputTracks();
            if (mSignalPending) {
                mSignalPending = false;
            } else if (waitingAsyncCallback_l()) {
                if (exitPending()) {
                    break;
                }
                releaseWakeLock_l();
                mWakeLockUids.clear();
                mActiveTracksGeneration++;
                ALOGV("wait async completion");
                mWaitWorkCV.wait(mLock);
                ALOGV("async completion/wake");
                acquireWakeLock_l();
                standbyTime = systemTime() + standbyDelay;
                sleepTime = 0;
                continue;
            }
            if ((!mActiveTracks.size() && systemTime() > standbyTime) ||
                                   isSuspended()) {
                if (shouldStandby_l()) {
                    threadLoop_standby();
                    mStandby = true;
                }
                if (!mActiveTracks.size() && mConfigEvents.isEmpty()) {
                    IPCThreadState::self()->flushCommands();
                    clearOutputTracks();
                    if (exitPending()) {
                        break;
                    }
                    releaseWakeLock_l();
                    mWakeLockUids.clear();
                    mActiveTracksGeneration++;
                    ALOGV("%s going to sleep", myName.string());
                    mWaitWorkCV.wait(mLock);
                    ALOGV("%s waking up", myName.string());
                    acquireWakeLock_l();
                    mMixerStatus = MIXER_IDLE;
                    mMixerStatusIgnoringFastTracks = MIXER_IDLE;
                    mBytesWritten = 0;
                    mBytesRemaining = 0;
                    checkSilentMode_l();
                    standbyTime = systemTime() + standbyDelay;
                    sleepTime = idleSleepTime;
                    if (mType == MIXER) {
                        sleepTimeShift = 0;
                    }
                    continue;
                }
            }
            mMixerStatus = prepareTracks_l(&tracksToRemove);
            if (lastGeneration != mActiveTracksGeneration) {
                updateWakeLockUids_l(mWakeLockUids);
                lastGeneration = mActiveTracksGeneration;
            }
            lockEffectChains_l(effectChains);
        }
        if (mBytesRemaining == 0) {
            mCurrentWriteLength = 0;
            if (mMixerStatus == MIXER_TRACKS_READY) {
                threadLoop_mix();
            } else if ((mMixerStatus != MIXER_DRAIN_TRACK)
                        && (mMixerStatus != MIXER_DRAIN_ALL)) {
                threadLoop_sleepTime();
                if (sleepTime == 0) {
                    mCurrentWriteLength = mixBufferSize;
                }
            }
            mBytesRemaining = mCurrentWriteLength;
            if (isSuspended()) {
                sleepTime = suspendSleepTimeUs();
                mBytesWritten += mixBufferSize;
                mBytesRemaining = 0;
            }
            if (sleepTime == 0 && mType != OFFLOAD) {
                for (size_t i = 0; i < effectChains.size(); i ++) {
                    effectChains[i]->process_l();
                }
            }
        }
        if (mType == OFFLOAD) {
            for (size_t i = 0; i < effectChains.size(); i ++) {
                effectChains[i]->process_l();
            }
        }
        unlockEffectChains(effectChains);
        if (!waitingAsyncCallback()) {
            if (sleepTime == 0) {
                if (mBytesRemaining) {
                    ssize_t ret = threadLoop_write();
                    if (ret < 0) {
                        mBytesRemaining = 0;
                    } else {
                        mBytesWritten += ret;
                        mBytesRemaining -= ret;
                    }
                } else if ((mMixerStatus == MIXER_DRAIN_TRACK) ||
                        (mMixerStatus == MIXER_DRAIN_ALL)) {
                    threadLoop_drain();
                }
                if (mType == MIXER) {
                    nsecs_t now = systemTime();
                    nsecs_t delta = now - mLastWriteTime;
                    if (!mStandby && delta > maxPeriod) {
                        mNumDelayedWrites++;
                        if ((now - lastWarning) > kWarningThrottleNs) {
                            ATRACE_NAME("underrun");
                            ALOGW("write blocked for %llu msecs, %d delayed writes, thread %p",
                                    ns2ms(delta), mNumDelayedWrites, this);
                            lastWarning = now;
                        }
                    }
                }
            } else {
                usleep(sleepTime);
            }
        }
        threadLoop_removeTracks(tracksToRemove);
        tracksToRemove.clear();
        clearOutputTracks();
        effectChains.clear();
    }
    threadLoop_exit();
    if (mType == MIXER || mType == DIRECT || mType == OFFLOAD) {
        if (!mStandby) {
            mOutput->stream->common.standby(&mOutput->stream->common);
        }
    }
    releaseWakeLock();
    mWakeLockUids.clear();
    mActiveTracksGeneration++;
    ALOGV("Thread %p type %d exiting", this, mType);
    return false;
}
void AudioFlinger::PlaybackThread::removeTracks_l(const Vector< sp<Track> >& tracksToRemove)
{
    size_t count = tracksToRemove.size();
    if (count > 0) {
        for (size_t i=0 ; i<count ; i++) {
            const sp<Track>& track = tracksToRemove.itemAt(i);
            mActiveTracks.remove(track);
            mWakeLockUids.remove(track->uid());
            mActiveTracksGeneration++;
            ALOGV("removeTracks_l removing track on session %d", track->sessionId());
            sp<EffectChain> chain = getEffectChain_l(track->sessionId());
            if (chain != 0) {
                ALOGV("stopping track on chain %p for session Id: %d", chain.get(),
                        track->sessionId());
                chain->decActiveTrackCnt();
            }
            if (track->isTerminated()) {
                removeTrack_l(track);
            }
        }
    }
}
status_t AudioFlinger::PlaybackThread::getTimestamp_l(AudioTimestamp& timestamp)
{
    if (mNormalSink != 0) {
        return mNormalSink->getTimestamp(timestamp);
    }
    if (mType == OFFLOAD && mOutput->stream->get_presentation_position) {
        uint64_t position64;
        int ret = mOutput->stream->get_presentation_position(
                                                mOutput->stream, &position64, &timestamp.mTime);
        if (ret == 0) {
            timestamp.mPosition = (uint32_t)position64;
            return NO_ERROR;
        }
    }
    return INVALID_OPERATION;
}
AudioFlinger::MixerThread::MixerThread(const sp<AudioFlinger>& audioFlinger, AudioStreamOut* output,
        audio_io_handle_t id, audio_devices_t device, type_t type)
    : PlaybackThread(audioFlinger, output, id, device, type),
        mFastMixerFutex(0)
{
    ALOGV("MixerThread() id=%d device=%#x type=%d", id, device, type);
    ALOGV("mSampleRate=%u, mChannelMask=%#x, mChannelCount=%u, mFormat=%d, mFrameSize=%u, "
            "mFrameCount=%d, mNormalFrameCount=%d",
            mSampleRate, mChannelMask, mChannelCount, mFormat, mFrameSize, mFrameCount,
            mNormalFrameCount);
    mAudioMixer = new AudioMixer(mNormalFrameCount, mSampleRate);
    if (mChannelCount != FCC_2) {
        ALOGE("Invalid audio hardware channel count %d", mChannelCount);
    }
    mOutputSink = new AudioStreamOutSink(output->stream);
    size_t numCounterOffers = 0;
    const NBAIO_Format offers[1] = {Format_from_SR_C(mSampleRate, mChannelCount)};
    ssize_t index = mOutputSink->negotiate(offers, 1, NULL, numCounterOffers);
    ALOG_ASSERT(index == 0);
    bool initFastMixer;
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
    if (initFastMixer) {
        NBAIO_Format format = mOutputSink->format();
        MonoPipe *monoPipe = new MonoPipe(mNormalFrameCount * 4, format, true );
        const NBAIO_Format offers[1] = {format};
        size_t numCounterOffers = 0;
        ssize_t index = monoPipe->negotiate(offers, 1, NULL, numCounterOffers);
        ALOG_ASSERT(index == 0);
        monoPipe->setAvgFrames((mScreenState & 1) ?
                (monoPipe->maxFrames() * 7) / 8 : mNormalFrameCount * 2);
        mPipeSink = monoPipe;
#ifdef TEE_SINK
        if (mTeeSinkOutputEnabled) {
            Pipe *teeSink = new Pipe(mTeeSinkOutputFrames, format);
            numCounterOffers = 0;
            index = teeSink->negotiate(offers, 1, NULL, numCounterOffers);
            ALOG_ASSERT(index == 0);
            mTeeSink = teeSink;
            PipeReader *teeSource = new PipeReader(*teeSink);
            numCounterOffers = 0;
            index = teeSource->negotiate(offers, 1, NULL, numCounterOffers);
            ALOG_ASSERT(index == 0);
            mTeeSource = teeSource;
        }
#endif
        mFastMixer = new FastMixer();
        FastMixerStateQueue *sq = mFastMixer->sq();
#ifdef STATE_QUEUE_DUMP
        sq->setObserverDump(&mStateQueueObserverDump);
        sq->setMutatorDump(&mStateQueueMutatorDump);
#endif
        FastMixerState *state = sq->begin();
        FastTrack *fastTrack = &state->mFastTracks[0];
        fastTrack->mBufferProvider = new SourceAudioBufferProvider(new MonoPipeReader(monoPipe));
        fastTrack->mVolumeProvider = NULL;
        fastTrack->mGeneration++;
        state->mFastTracksGen++;
        state->mTrackMask = 1;
        state->mOutputSink = mOutputSink.get();
        state->mOutputSinkGen++;
        state->mFrameCount = mFrameCount;
        state->mCommand = FastMixerState::COLD_IDLE;
        state->mColdFutexAddr = &mFastMixerFutex;
        state->mColdGen++;
        state->mDumpState = &mFastMixerDumpState;
#ifdef TEE_SINK
        state->mTeeSink = mTeeSink.get();
#endif
        mFastMixerNBLogWriter = audioFlinger->newWriter_l(kFastMixerLogSize, "FastMixer");
        state->mNBLogWriter = mFastMixerNBLogWriter.get();
        sq->end();
        sq->push(FastMixerStateQueue::BLOCK_UNTIL_PUSHED);
        mFastMixer->run("FastMixer", PRIORITY_URGENT_AUDIO);
        pid_t tid = mFastMixer->getTid();
        int err = requestPriority(getpid_cached, tid, kPriorityFastMixer);
        if (err != 0) {
            ALOGW("Policy SCHED_FIFO priority %d is unavailable for pid %d tid %d; error %d",
                    kPriorityFastMixer, getpid_cached, tid, err);
        }
#ifdef AUDIO_WATCHDOG
        mAudioWatchdog = new AudioWatchdog();
        mAudioWatchdog->setDump(&mAudioWatchdogDump);
        mAudioWatchdog->run("AudioWatchdog", PRIORITY_URGENT_AUDIO);
        tid = mAudioWatchdog->getTid();
        err = requestPriority(getpid_cached, tid, kPriorityFastMixer);
        if (err != 0) {
            ALOGW("Policy SCHED_FIFO priority %d is unavailable for pid %d tid %d; error %d",
                    kPriorityFastMixer, getpid_cached, tid, err);
        }
#endif
    } else {
        mFastMixer = NULL;
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
AudioFlinger::MixerThread::~MixerThread()
{
    if (mFastMixer != NULL) {
        FastMixerStateQueue *sq = mFastMixer->sq();
        FastMixerState *state = sq->begin();
        if (state->mCommand == FastMixerState::COLD_IDLE) {
            int32_t old = android_atomic_inc(&mFastMixerFutex);
            if (old == -1) {
                __futex_syscall3(&mFastMixerFutex, FUTEX_WAKE_PRIVATE, 1);
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
        delete mFastMixer;
#ifdef AUDIO_WATCHDOG
        if (mAudioWatchdog != 0) {
            mAudioWatchdog->requestExit();
            mAudioWatchdog->requestExitAndWait();
            mAudioWatchdog.clear();
        }
#endif
    }
    mAudioFlinger->unregisterWriter(mFastMixerNBLogWriter);
    delete mAudioMixer;
}
uint32_t AudioFlinger::MixerThread::correctLatency_l(uint32_t latency) const
{
    if (mFastMixer != NULL) {
        MonoPipe *pipe = (MonoPipe *)mPipeSink.get();
        latency += (pipe->getAvgFrames() * 1000) / mSampleRate;
    }
    return latency;
}
void AudioFlinger::MixerThread::threadLoop_removeTracks(const Vector< sp<Track> >& tracksToRemove)
{
    PlaybackThread::threadLoop_removeTracks(tracksToRemove);
}
ssize_t AudioFlinger::MixerThread::threadLoop_write()
{
    if (mFastMixer != NULL) {
        FastMixerStateQueue *sq = mFastMixer->sq();
        FastMixerState *state = sq->begin();
        if (state->mCommand != FastMixerState::MIX_WRITE &&
                (kUseFastMixer != FastMixer_Dynamic || state->mTrackMask > 1)) {
            if (state->mCommand == FastMixerState::COLD_IDLE) {
                int32_t old = android_atomic_inc(&mFastMixerFutex);
                if (old == -1) {
                    __futex_syscall3(&mFastMixerFutex, FUTEX_WAKE_PRIVATE, 1);
                }
#ifdef AUDIO_WATCHDOG
                if (mAudioWatchdog != 0) {
                    mAudioWatchdog->resume();
                }
#endif
            }
            state->mCommand = FastMixerState::MIX_WRITE;
            mFastMixerDumpState.increaseSamplingN(mAudioFlinger->isLowRamDevice() ?
                    FastMixerDumpState::kSamplingNforLowRamDevice : FastMixerDumpState::kSamplingN);
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
void AudioFlinger::MixerThread::threadLoop_standby()
{
    if (mFastMixer != NULL) {
        FastMixerStateQueue *sq = mFastMixer->sq();
        FastMixerState *state = sq->begin();
        if (!(state->mCommand & FastMixerState::IDLE)) {
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
bool AudioFlinger::PlaybackThread::waitingAsyncCallback_l()
{
    return false;
}
bool AudioFlinger::PlaybackThread::shouldStandby_l()
{
    return !mStandby;
}
bool AudioFlinger::PlaybackThread::waitingAsyncCallback()
{
    Mutex::Autolock _l(mLock);
    return waitingAsyncCallback_l();
}
void AudioFlinger::PlaybackThread::threadLoop_standby()
{
    ALOGV("Audio hardware entering standby, mixer %p, suspend count %d", this, mSuspended);
    mOutput->stream->common.standby(&mOutput->stream->common);
    if (mUseAsyncWrite != 0) {
        mWriteAckSequence = (mWriteAckSequence + 2) & ~1;
        mDrainSequence = (mDrainSequence + 2) & ~1;
        ALOG_ASSERT(mCallbackThread != 0);
        mCallbackThread->setWriteBlocked(mWriteAckSequence);
        mCallbackThread->setDraining(mDrainSequence);
    }
}
void AudioFlinger::PlaybackThread::onAddNewTrack_l()
{
    ALOGV("signal playback thread");
    broadcast_l();
}
void AudioFlinger::MixerThread::threadLoop_mix()
{
    int64_t pts;
    status_t status = INVALID_OPERATION;
    if (mNormalSink != 0) {
        status = mNormalSink->getNextWriteTimestamp(&pts);
    } else {
        status = mOutputSink->getNextWriteTimestamp(&pts);
    }
    if (status != NO_ERROR) {
        pts = AudioBufferProvider::kInvalidPTS;
    }
    mAudioMixer->process(pts);
    mCurrentWriteLength = mixBufferSize;
    if ((sleepTime == 0) && (sleepTimeShift > 0)) {
        sleepTimeShift--;
    }
    sleepTime = 0;
    standbyTime = systemTime() + standbyDelay;
}
void AudioFlinger::MixerThread::threadLoop_sleepTime()
{
    if (sleepTime == 0) {
        if (mMixerStatus == MIXER_TRACKS_ENABLED) {
            sleepTime = activeSleepTime >> sleepTimeShift;
            if (sleepTime < kMinThreadSleepTimeUs) {
                sleepTime = kMinThreadSleepTimeUs;
            }
            if (sleepTimeShift < kMaxThreadSleepTimeShift) {
                sleepTimeShift++;
            }
        } else {
            sleepTime = idleSleepTime;
        }
    } else if (mBytesWritten != 0 || (mMixerStatus == MIXER_TRACKS_ENABLED)) {
        memset(mMixBuffer, 0, mixBufferSize);
        sleepTime = 0;
        ALOGV_IF(mBytesWritten == 0 && (mMixerStatus == MIXER_TRACKS_ENABLED),
                "anticipated start");
    }
}
AudioFlinger::PlaybackThread::mixer_state AudioFlinger::MixerThread::prepareTracks_l(
        Vector< sp<Track> > *tracksToRemove)
{
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
    sp<EffectChain> chain = getEffectChain_l(AUDIO_SESSION_OUTPUT_MIX);
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
    if (mFastMixer != NULL) {
        sq = mFastMixer->sq();
        state = sq->begin();
    }
    for (size_t i=0 ; i<count ; i++) {
        const sp<Track> t = mActiveTracks[i].promote();
        if (t == 0) {
            continue;
        }
        Track* const track = t.get();
        if (track->isFastTrack()) {
            int j = track->mFastIndex;
            ALOG_ASSERT(0 < j && j < (int)FastMixerState::kMaxFastTracks);
            ALOG_ASSERT(!(mFastTrackAvailMask & (1 << j)));
            FastTrack *fastTrack = &state->mFastTracks[j];
            FastTrackDump *ftDump = &mFastMixerDumpState.mTracks[j];
            FastTrackUnderruns underruns = ftDump->mUnderruns;
            uint32_t recentFull = (underruns.mBitFields.mFull -
                    track->mObservedUnderruns.mBitFields.mFull) & UNDERRUN_MASK;
            uint32_t recentPartial = (underruns.mBitFields.mPartial -
                    track->mObservedUnderruns.mBitFields.mPartial) & UNDERRUN_MASK;
            uint32_t recentEmpty = (underruns.mBitFields.mEmpty -
                    track->mObservedUnderruns.mBitFields.mEmpty) & UNDERRUN_MASK;
            uint32_t recentUnderruns = recentPartial + recentEmpty;
            track->mObservedUnderruns = underruns;
            if (!(track->isStopping() || track->isPausing() || track->isStopped()) &&
                    recentUnderruns > 0) {
                track->mAudioTrackServerProxy->tallyUnderrunFrames(recentUnderruns * mFrameCount);
            }
            bool isActive = true;
            switch (track->mState) {
            case TrackBase::STOPPING_1:
                if (recentUnderruns > 0 || track->isTerminated()) {
                    track->mState = TrackBase::STOPPING_2;
                }
                break;
            case TrackBase::PAUSING:
                track->setPaused();
                break;
            case TrackBase::RESUMING:
                track->mState = TrackBase::ACTIVE;
                break;
            case TrackBase::ACTIVE:
                if (recentFull > 0 || recentPartial > 0) {
                    track->mRetryCount = kMaxTrackRetries;
                }
                if (recentUnderruns == 0) {
                    break;
                }
                if (track->sharedBuffer() == 0) {
                    if (recentEmpty == 0) {
                        break;
                    }
                    if (--(track->mRetryCount) > 0) {
                        break;
                    }
                    android_atomic_or(CBLK_DISABLED, &track->mCblk->mFlags);
                    isActive = false;
                    break;
                }
            case TrackBase::STOPPING_2:
            case TrackBase::PAUSED:
            case TrackBase::STOPPED:
            case TrackBase::FLUSHED:
                {
                    size_t audioHALFrames =
                            (mOutput->stream->get_latency(mOutput->stream)*mSampleRate) / 1000;
                    size_t framesWritten = mBytesWritten / mFrameSize;
                    if (!(mStandby || track->presentationComplete(framesWritten, audioHALFrames))) {
                        break;
                    }
                }
                if (track->isStopping_2()) {
                    track->mState = TrackBase::STOPPED;
                }
                if (track->isStopped()) {
                    resetMask |= 1 << i;
                }
                isActive = false;
                break;
            case TrackBase::IDLE:
            default:
                LOG_FATAL("unexpected track state %d", track->mState);
            }
            if (isActive) {
                if (!(state->mTrackMask & (1 << j))) {
                    ExtendedAudioBufferProvider *eabp = track;
                    VolumeProvider *vp = track;
                    fastTrack->mBufferProvider = eabp;
                    fastTrack->mVolumeProvider = vp;
                    fastTrack->mChannelMask = track->mChannelMask;
                    fastTrack->mGeneration++;
                    state->mTrackMask |= 1 << j;
                    didModify = true;
                }
                track->mCachedVolume = masterVolume * mStreamTypes[track->streamType()].volume;
                ++fastTracks;
            } else {
                if (state->mTrackMask & (1 << j)) {
                    fastTrack->mBufferProvider = NULL;
                    fastTrack->mGeneration++;
                    state->mTrackMask &= ~(1 << j);
                    didModify = true;
                    block = FastMixerStateQueue::BLOCK_UNTIL_ACKED;
                } else {
                    LOG_FATAL("fast track %d should have been active", j);
                }
                tracksToRemove->add(track);
                track->mObservedUnderruns.mBitFields.mMostRecent = UNDERRUN_FULL;
            }
            continue;
        }
        {
        audio_track_cblk_t* cblk = track->cblk();
        int name = track->name();
        size_t desiredFrames;
        uint32_t sr = track->sampleRate();
        if (sr == mSampleRate) {
            desiredFrames = mNormalFrameCount;
        } else {
            desiredFrames = (mNormalFrameCount * sr) / mSampleRate + 1 + 1;
            desiredFrames += mAudioMixer->getUnreleasedFrames(track->name());
#if 0
            ALOG_ASSERT(desiredFrames <= cblk->frameCount_);
#endif
        }
        uint32_t minFrames = 1;
        if ((track->sharedBuffer() == 0) && !track->isStopped() && !track->isPausing() &&
                (mMixerStatusIgnoringFastTracks == MIXER_TRACKS_READY)) {
            minFrames = desiredFrames;
        }
        size_t framesReady = track->framesReady();
        if ((framesReady >= minFrames) && track->isReady() &&
                !track->isPaused() && !track->isTerminated())
        {
            ALOGVV("track %d s=%08x [OK] on thread %p", name, cblk->mServer, this);
            mixedTracks++;
            chain.clear();
            if (track->mainBuffer() != mMixBuffer) {
                chain = getEffectChain_l(track->sessionId());
                if (chain != 0) {
                    tracksWithEffect++;
                } else {
                    ALOGW("prepareTracks_l(): track %d attached to effect but no chain found on "
                            "session %d",
                            name, track->sessionId());
                }
            }
            int param = AudioMixer::VOLUME;
            if (track->mFillingUpStatus == Track::FS_FILLED) {
                track->mFillingUpStatus = Track::FS_ACTIVE;
                if (track->mState == TrackBase::RESUMING) {
                    track->mState = TrackBase::ACTIVE;
                    param = AudioMixer::RAMP_VOLUME;
                }
                mAudioMixer->setParameter(name, AudioMixer::RESAMPLE, AudioMixer::RESET, NULL);
            } else if (cblk->mServer != 0) {
                param = AudioMixer::RAMP_VOLUME;
            }
            uint32_t vl, vr, va;
            if (track->isPausing() || mStreamTypes[track->streamType()].mute) {
                vl = vr = va = 0;
                if (track->isPausing()) {
                    track->setPaused();
                }
            } else {
                float typeVolume = mStreamTypes[track->streamType()].volume;
                float v = masterVolume * typeVolume;
                AudioTrackServerProxy *proxy = track->mAudioTrackServerProxy;
                uint32_t vlr = proxy->getVolumeLR();
                vl = vlr & 0xFFFF;
                vr = vlr >> 16;
                if (vl > MAX_GAIN_INT) {
                    ALOGV("Track left volume out of range: %04X", vl);
                    vl = MAX_GAIN_INT;
                }
                if (vr > MAX_GAIN_INT) {
                    ALOGV("Track right volume out of range: %04X", vr);
                    vr = MAX_GAIN_INT;
                }
                vl = (uint32_t)(v * vl) << 12;
                vr = (uint32_t)(v * vr) << 12;
                uint16_t sendLevel = proxy->getSendLevel_U4_12();
                if (sendLevel > MAX_GAIN_INT) {
                    ALOGV("Track send level out of range: %04X", sendLevel);
                    sendLevel = MAX_GAIN_INT;
                }
                va = (uint32_t)(v * sendLevel);
            }
            if (chain != 0 && chain->setVolume_l(&vl, &vr)) {
                param = AudioMixer::VOLUME;
                track->mHasVolumeController = true;
            } else {
                if (track->mHasVolumeController) {
                    param = AudioMixer::VOLUME;
                }
                track->mHasVolumeController = false;
            }
            vl = (vl + (1 << 11)) >> 12;
            if (vl > MAX_GAIN_INT) {
                vl = MAX_GAIN_INT;
            }
            vr = (vr + (1 << 11)) >> 12;
            if (vr > MAX_GAIN_INT) {
                vr = MAX_GAIN_INT;
            }
            if (va > MAX_GAIN_INT) {
                va = MAX_GAIN_INT;
            }
            mAudioMixer->setBufferProvider(name, track);
            mAudioMixer->enable(name);
            mAudioMixer->setParameter(name, param, AudioMixer::VOLUME0, (void *)(uintptr_t)vl);
            mAudioMixer->setParameter(name, param, AudioMixer::VOLUME1, (void *)(uintptr_t)vr);
            mAudioMixer->setParameter(name, param, AudioMixer::AUXLEVEL, (void *)(uintptr_t)va);
            mAudioMixer->setParameter(
                name,
                AudioMixer::TRACK,
                AudioMixer::FORMAT, (void *)track->format());
            mAudioMixer->setParameter(
                name,
                AudioMixer::TRACK,
                AudioMixer::CHANNEL_MASK, (void *)(uintptr_t)track->channelMask());
            uint32_t maxSampleRate = mSampleRate * 2;
            uint32_t reqSampleRate = track->mAudioTrackServerProxy->getSampleRate();
            if (reqSampleRate == 0) {
                reqSampleRate = mSampleRate;
            } else if (reqSampleRate > maxSampleRate) {
                reqSampleRate = maxSampleRate;
            }
            mAudioMixer->setParameter(
                name,
                AudioMixer::RESAMPLE,
                AudioMixer::SAMPLE_RATE,
                (void *)(uintptr_t)reqSampleRate);
            mAudioMixer->setParameter(
                name,
                AudioMixer::TRACK,
                AudioMixer::MAIN_BUFFER, (void *)track->mainBuffer());
            mAudioMixer->setParameter(
                name,
                AudioMixer::TRACK,
                AudioMixer::AUX_BUFFER, (void *)track->auxBuffer());
            track->mRetryCount = kMaxTrackRetries;
            if (mMixerStatusIgnoringFastTracks != MIXER_TRACKS_READY ||
                    mixerStatus != MIXER_TRACKS_ENABLED) {
                mixerStatus = MIXER_TRACKS_READY;
            }
        } else {
            if (framesReady < desiredFrames && !track->isStopped() && !track->isPaused()) {
                track->mAudioTrackServerProxy->tallyUnderrunFrames(desiredFrames);
            }
            chain = getEffectChain_l(track->sessionId());
            if (chain != 0) {
                chain->clearInputBuffer();
            }
            ALOGVV("track %d s=%08x [NOT READY] on thread %p", name, cblk->mServer, this);
            if ((track->sharedBuffer() != 0) || track->isTerminated() ||
                    track->isStopped() || track->isPaused()) {
                size_t audioHALFrames = (latency_l() * mSampleRate) / 1000;
                size_t framesWritten = mBytesWritten / mFrameSize;
                if (mStandby || track->presentationComplete(framesWritten, audioHALFrames)) {
                    if (track->isStopped()) {
                        track->reset();
                    }
                    tracksToRemove->add(track);
                }
            } else {
                if (--(track->mRetryCount) <= 0) {
                    ALOGI("BUFFER TIMEOUT: remove(%d) from active list on thread %p", name, this);
                    tracksToRemove->add(track);
                    android_atomic_or(CBLK_DISABLED, &cblk->mFlags);
                } else if (mMixerStatusIgnoringFastTracks == MIXER_TRACKS_READY ||
                                mixerStatus != MIXER_TRACKS_READY) {
                    mixerStatus = MIXER_TRACKS_ENABLED;
                }
            }
            mAudioMixer->disable(name);
        }
        }
track_is_ready: ;
    }
    bool pauseAudioWatchdog = false;
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
        sq->push(block);
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
        sp<Track> t = mActiveTracks[i].promote();
        if (t == 0) {
            continue;
        }
        Track* track = t.get();
        ALOG_ASSERT(track->isFastTrack() && track->isStopped());
        track->reset();
    }
    removeTracks_l(*tracksToRemove);
    if ((mBytesRemaining == 0) && ((mixedTracks != 0 && mixedTracks == tracksWithEffect) ||
            (mixedTracks == 0 && fastTracks > 0))) {
        memset(mMixBuffer, 0, mNormalFrameCount * mChannelCount * sizeof(int16_t));
    }
    mMixerStatusIgnoringFastTracks = mixerStatus;
    if (fastTracks > 0) {
        mixerStatus = MIXER_TRACKS_READY;
    }
    return mixerStatus;
}
int AudioFlinger::MixerThread::getTrackName_l(audio_channel_mask_t channelMask, int sessionId)
{
    return mAudioMixer->getTrackName(channelMask, sessionId);
}
void AudioFlinger::MixerThread::deleteTrackName_l(int name)
{
    ALOGV("remove track (%d) and delete from mixer", name);
    mAudioMixer->deleteTrackName(name);
}
bool AudioFlinger::MixerThread::checkForNewParameters_l()
{
    FastMixerState::Command previousCommand = FastMixerState::HOT_IDLE;
    bool reconfig = false;
    while (!mNewParameters.isEmpty()) {
        if (mFastMixer != NULL) {
            FastMixerStateQueue *sq = mFastMixer->sq();
            FastMixerState *state = sq->begin();
            if (!(state->mCommand & FastMixerState::IDLE)) {
                previousCommand = state->mCommand;
                state->mCommand = FastMixerState::HOT_IDLE;
                sq->end();
                sq->push(FastMixerStateQueue::BLOCK_UNTIL_ACKED);
            } else {
                sq->end(false );
            }
        }
        status_t status = NO_ERROR;
        String8 keyValuePair = mNewParameters[0];
        AudioParameter param = AudioParameter(keyValuePair);
        int value;
        if (param.getInt(String8(AudioParameter::keySamplingRate), value) == NO_ERROR) {
            reconfig = true;
        }
        if (param.getInt(String8(AudioParameter::keyFormat), value) == NO_ERROR) {
            if ((audio_format_t) value != AUDIO_FORMAT_PCM_16_BIT) {
                status = BAD_VALUE;
            } else {
                reconfig = true;
            }
        }
        if (param.getInt(String8(AudioParameter::keyChannels), value) == NO_ERROR) {
            if ((audio_channel_mask_t) value != AUDIO_CHANNEL_OUT_STEREO) {
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
#ifdef ADD_BATTERY_DATA
            if (mOutDevice != value) {
                uint32_t params = 0;
                if (value & AUDIO_DEVICE_OUT_SPEAKER) {
                    params |= IMediaPlayerService::kBatteryDataSpeakerOn;
                }
                audio_devices_t deviceWithoutSpeaker
                    = AUDIO_DEVICE_OUT_ALL & ~AUDIO_DEVICE_OUT_SPEAKER;
                if (value & deviceWithoutSpeaker ) {
                    params |= IMediaPlayerService::kBatteryDataOtherAudioDeviceOn;
                }
                if (params != 0) {
                    addBatteryData(params);
                }
            }
#endif
            if (value != AUDIO_DEVICE_NONE) {
                mOutDevice = value;
                for (size_t i = 0; i < mEffectChains.size(); i++) {
                    mEffectChains[i]->setDevice_l(mOutDevice);
                }
            }
        }
        if (status == NO_ERROR) {
            status = mOutput->stream->common.set_parameters(&mOutput->stream->common,
                                                    keyValuePair.string());
            if (!mStandby && status == INVALID_OPERATION) {
                mOutput->stream->common.standby(&mOutput->stream->common);
                mStandby = true;
                mBytesWritten = 0;
                status = mOutput->stream->common.set_parameters(&mOutput->stream->common,
                                                       keyValuePair.string());
            }
            if (status == NO_ERROR && reconfig) {
                readOutputParameters();
                delete mAudioMixer;
                mAudioMixer = new AudioMixer(mNormalFrameCount, mSampleRate);
                for (size_t i = 0; i < mTracks.size() ; i++) {
                    int name = getTrackName_l(mTracks[i]->mChannelMask, mTracks[i]->mSessionId);
                    if (name < 0) {
                        break;
                    }
                    mTracks[i]->mName = name;
                }
                sendIoConfigEvent_l(AudioSystem::OUTPUT_CONFIG_CHANGED);
            }
        }
        mNewParameters.removeAt(0);
        mParamStatus = status;
        mParamCond.signal();
        mWaitWorkCV.waitRelative(mLock, kSetParametersTimeoutNs);
    }
    if (!(previousCommand & FastMixerState::IDLE)) {
        ALOG_ASSERT(mFastMixer != NULL);
        FastMixerStateQueue *sq = mFastMixer->sq();
        FastMixerState *state = sq->begin();
        ALOG_ASSERT(state->mCommand == FastMixerState::HOT_IDLE);
        state->mCommand = previousCommand;
        sq->end();
        sq->push(FastMixerStateQueue::BLOCK_UNTIL_PUSHED);
    }
    return reconfig;
}
void AudioFlinger::MixerThread::dumpInternals(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    PlaybackThread::dumpInternals(fd, args);
    fdprintf(fd, "  AudioMixer tracks: 0x%08x\n", mAudioMixer->trackNames());
    const FastMixerDumpState copy(mFastMixerDumpState);
    copy.dump(fd);
#ifdef STATE_QUEUE_DUMP
    StateQueueObserverDump observerCopy = mStateQueueObserverDump;
    observerCopy.dump(fd);
    StateQueueMutatorDump mutatorCopy = mStateQueueMutatorDump;
    mutatorCopy.dump(fd);
#endif
#ifdef TEE_SINK
    dumpTee(fd, mTeeSource, mId);
#endif
#ifdef AUDIO_WATCHDOG
    if (mAudioWatchdog != 0) {
        AudioWatchdogDump wdCopy = mAudioWatchdogDump;
        wdCopy.dump(fd);
    }
#endif
}
uint32_t AudioFlinger::MixerThread::idleSleepTimeUs() const
{
    return (uint32_t)(((mNormalFrameCount * 1000) / mSampleRate) * 1000) / 2;
}
uint32_t AudioFlinger::MixerThread::suspendSleepTimeUs() const
{
    return (uint32_t)(((mNormalFrameCount * 1000) / mSampleRate) * 1000);
}
void AudioFlinger::MixerThread::cacheParameters_l()
{
    PlaybackThread::cacheParameters_l();
    maxPeriod = seconds(mNormalFrameCount) / mSampleRate * 15;
}
AudioFlinger::DirectOutputThread::DirectOutputThread(const sp<AudioFlinger>& audioFlinger,
        AudioStreamOut* output, audio_io_handle_t id, audio_devices_t device)
    : PlaybackThread(audioFlinger, output, id, device, DIRECT)
{
}
AudioFlinger::DirectOutputThread::DirectOutputThread(const sp<AudioFlinger>& audioFlinger,
        AudioStreamOut* output, audio_io_handle_t id, uint32_t device,
        ThreadBase::type_t type)
    : PlaybackThread(audioFlinger, output, id, device, type)
{
}
AudioFlinger::DirectOutputThread::~DirectOutputThread()
{
}
void AudioFlinger::DirectOutputThread::processVolume_l(Track *track, bool lastTrack)
{
    audio_track_cblk_t* cblk = track->cblk();
    float left, right;
    if (mMasterMute || mStreamTypes[track->streamType()].mute) {
        left = right = 0;
    } else {
        float typeVolume = mStreamTypes[track->streamType()].volume;
        float v = mMasterVolume * typeVolume;
        AudioTrackServerProxy *proxy = track->mAudioTrackServerProxy;
        uint32_t vlr = proxy->getVolumeLR();
        float v_clamped = v * (vlr & 0xFFFF);
        if (v_clamped > MAX_GAIN) v_clamped = MAX_GAIN;
        left = v_clamped/MAX_GAIN;
        v_clamped = v * (vlr >> 16);
        if (v_clamped > MAX_GAIN) v_clamped = MAX_GAIN;
        right = v_clamped/MAX_GAIN;
    }
    if (lastTrack) {
        if (left != mLeftVolFloat || right != mRightVolFloat) {
            mLeftVolFloat = left;
            mRightVolFloat = right;
            uint32_t vl = (uint32_t)(left * (1 << 24));
            uint32_t vr = (uint32_t)(right * (1 << 24));
            if (!mEffectChains.isEmpty()) {
                mEffectChains[0]->setVolume_l(&vl, &vr);
                left = (float)vl / (1 << 24);
                right = (float)vr / (1 << 24);
            }
            if (mOutput->stream->set_volume) {
                mOutput->stream->set_volume(mOutput->stream, left, right);
            }
        }
    }
}
AudioFlinger::PlaybackThread::mixer_state AudioFlinger::DirectOutputThread::prepareTracks_l(
    Vector< sp<Track> > *tracksToRemove
)
{
    size_t count = mActiveTracks.size();
    mixer_state mixerStatus = MIXER_IDLE;
    for (size_t i = 0; i < count; i++) {
        sp<Track> t = mActiveTracks[i].promote();
        if (t == 0) {
            continue;
        }
        Track* const track = t.get();
        audio_track_cblk_t* cblk = track->cblk();
        sp<Track> l = mLatestActiveTrack.promote();
        bool last = l.get() == track;
        uint32_t minFrames;
        if ((track->sharedBuffer() == 0) && !track->isStopped() && !track->isPausing()) {
            minFrames = mNormalFrameCount;
        } else {
            minFrames = 1;
        }
        if ((track->framesReady() >= minFrames) && track->isReady() &&
                !track->isPaused() && !track->isTerminated())
        {
            ALOGVV("track %d s=%08x [OK]", track->name(), cblk->mServer);
            if (track->mFillingUpStatus == Track::FS_FILLED) {
                track->mFillingUpStatus = Track::FS_ACTIVE;
                mLeftVolFloat = mRightVolFloat = -1.0;
                if (track->mState == TrackBase::RESUMING) {
                    track->mState = TrackBase::ACTIVE;
                }
            }
            processVolume_l(track, last);
            if (last) {
                track->mRetryCount = kMaxTrackRetriesDirect;
                mActiveTrack = t;
                mixerStatus = MIXER_TRACKS_READY;
            }
        } else {
            if (!mEffectChains.isEmpty() && last) {
                mEffectChains[0]->clearInputBuffer();
            }
            ALOGVV("track %d s=%08x [NOT READY]", track->name(), cblk->mServer);
            if ((track->sharedBuffer() != 0) || track->isTerminated() ||
                    track->isStopped() || track->isPaused()) {
                size_t audioHALFrames = (latency_l() * mSampleRate) / 1000;
                size_t framesWritten = mBytesWritten / mFrameSize;
                if (mStandby || !last ||
                        track->presentationComplete(framesWritten, audioHALFrames)) {
                    if (track->isStopped()) {
                        track->reset();
                    }
                    tracksToRemove->add(track);
                }
            } else {
                if (--(track->mRetryCount) <= 0) {
                    ALOGV("BUFFER TIMEOUT: remove(%d) from active list", track->name());
                    tracksToRemove->add(track);
                    android_atomic_or(CBLK_DISABLED, &cblk->mFlags);
                } else if (last) {
                    mixerStatus = MIXER_TRACKS_ENABLED;
                }
            }
        }
    }
    removeTracks_l(*tracksToRemove);
    return mixerStatus;
}
void AudioFlinger::DirectOutputThread::threadLoop_mix()
{
    size_t frameCount = mFrameCount;
    int8_t *curBuf = (int8_t *)mMixBuffer;
    while (frameCount) {
        AudioBufferProvider::Buffer buffer;
        buffer.frameCount = frameCount;
        mActiveTrack->getNextBuffer(&buffer);
        if (buffer.raw == NULL) {
            memset(curBuf, 0, frameCount * mFrameSize);
            break;
        }
        memcpy(curBuf, buffer.raw, buffer.frameCount * mFrameSize);
        frameCount -= buffer.frameCount;
        curBuf += buffer.frameCount * mFrameSize;
        mActiveTrack->releaseBuffer(&buffer);
    }
    mCurrentWriteLength = curBuf - (int8_t *)mMixBuffer;
    sleepTime = 0;
    standbyTime = systemTime() + standbyDelay;
    mActiveTrack.clear();
}
void AudioFlinger::DirectOutputThread::threadLoop_sleepTime()
{
    if (sleepTime == 0) {
        if (mMixerStatus == MIXER_TRACKS_ENABLED) {
            sleepTime = activeSleepTime;
        } else {
            sleepTime = idleSleepTime;
        }
    } else if (mBytesWritten != 0 && audio_is_linear_pcm(mFormat)) {
        memset(mMixBuffer, 0, mFrameCount * mFrameSize);
        sleepTime = 0;
    }
}
int AudioFlinger::DirectOutputThread::getTrackName_l(audio_channel_mask_t channelMask __unused, int sessionId __unused)
{
    return 0;
}
void AudioFlinger::DirectOutputThread::deleteTrackName_l(int name __unused)
{
}
bool AudioFlinger::DirectOutputThread::checkForNewParameters_l()
{
    bool reconfig = false;
    while (!mNewParameters.isEmpty()) {
        status_t status = NO_ERROR;
        String8 keyValuePair = mNewParameters[0];
        AudioParameter param = AudioParameter(keyValuePair);
        int value;
        if (param.getInt(String8(AudioParameter::keyFrameCount), value) == NO_ERROR) {
            if (!mTracks.isEmpty()) {
                status = INVALID_OPERATION;
            } else {
                reconfig = true;
            }
        }
        if (status == NO_ERROR) {
            status = mOutput->stream->common.set_parameters(&mOutput->stream->common,
                                                    keyValuePair.string());
            if (!mStandby && status == INVALID_OPERATION) {
                mOutput->stream->common.standby(&mOutput->stream->common);
                mStandby = true;
                mBytesWritten = 0;
                status = mOutput->stream->common.set_parameters(&mOutput->stream->common,
                                                       keyValuePair.string());
            }
            if (status == NO_ERROR && reconfig) {
                readOutputParameters();
                sendIoConfigEvent_l(AudioSystem::OUTPUT_CONFIG_CHANGED);
            }
        }
        mNewParameters.removeAt(0);
        mParamStatus = status;
        mParamCond.signal();
        mWaitWorkCV.waitRelative(mLock, kSetParametersTimeoutNs);
    }
    return reconfig;
}
uint32_t AudioFlinger::DirectOutputThread::activeSleepTimeUs() const
{
    uint32_t time;
    if (audio_is_linear_pcm(mFormat)) {
        time = PlaybackThread::activeSleepTimeUs();
    } else {
        time = 10000;
    }
    return time;
}
uint32_t AudioFlinger::DirectOutputThread::idleSleepTimeUs() const
{
    uint32_t time;
    if (audio_is_linear_pcm(mFormat)) {
        time = (uint32_t)(((mFrameCount * 1000) / mSampleRate) * 1000) / 2;
    } else {
        time = 10000;
    }
    return time;
}
uint32_t AudioFlinger::DirectOutputThread::suspendSleepTimeUs() const
{
    uint32_t time;
    if (audio_is_linear_pcm(mFormat)) {
        time = (uint32_t)(((mFrameCount * 1000) / mSampleRate) * 1000);
    } else {
        time = 10000;
    }
    return time;
}
void AudioFlinger::DirectOutputThread::cacheParameters_l()
{
    PlaybackThread::cacheParameters_l();
    if (audio_is_linear_pcm(mFormat)) {
        standbyDelay = microseconds(activeSleepTime*2);
    } else {
        standbyDelay = kOffloadStandbyDelayNs;
    }
}
AudioFlinger::AsyncCallbackThread::AsyncCallbackThread(
        const wp<AudioFlinger::PlaybackThread>& playbackThread)
    : Thread(false ),
        mPlaybackThread(playbackThread),
        mWriteAckSequence(0),
        mDrainSequence(0)
{
}
AudioFlinger::AsyncCallbackThread::~AsyncCallbackThread()
{
}
void AudioFlinger::AsyncCallbackThread::onFirstRef()
{
    run("Offload Cbk", ANDROID_PRIORITY_URGENT_AUDIO);
}
bool AudioFlinger::AsyncCallbackThread::threadLoop()
{
    while (!exitPending()) {
        uint32_t writeAckSequence;
        uint32_t drainSequence;
        {
            Mutex::Autolock _l(mLock);
            while (!((mWriteAckSequence & 1) ||
                     (mDrainSequence & 1) ||
                     exitPending())) {
                mWaitWorkCV.wait(mLock);
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
        }
        {
            sp<AudioFlinger::PlaybackThread> playbackThread = mPlaybackThread.promote();
            if (playbackThread != 0) {
                if (writeAckSequence & 1) {
                    playbackThread->resetWriteBlocked(writeAckSequence >> 1);
                }
                if (drainSequence & 1) {
                    playbackThread->resetDraining(drainSequence >> 1);
                }
            }
        }
    }
    return false;
}
void AudioFlinger::AsyncCallbackThread::exit()
{
    ALOGV("AsyncCallbackThread::exit");
    Mutex::Autolock _l(mLock);
    requestExit();
    mWaitWorkCV.broadcast();
}
void AudioFlinger::AsyncCallbackThread::setWriteBlocked(uint32_t sequence)
{
    Mutex::Autolock _l(mLock);
    mWriteAckSequence = sequence << 1;
}
void AudioFlinger::AsyncCallbackThread::resetWriteBlocked()
{
    Mutex::Autolock _l(mLock);
    if (mWriteAckSequence & 2) {
        mWriteAckSequence |= 1;
        mWaitWorkCV.signal();
    }
}
void AudioFlinger::AsyncCallbackThread::setDraining(uint32_t sequence)
{
    Mutex::Autolock _l(mLock);
    mDrainSequence = sequence << 1;
}
void AudioFlinger::AsyncCallbackThread::resetDraining()
{
    Mutex::Autolock _l(mLock);
    if (mDrainSequence & 2) {
        mDrainSequence |= 1;
        mWaitWorkCV.signal();
    }
}
AudioFlinger::OffloadThread::OffloadThread(const sp<AudioFlinger>& audioFlinger,
        AudioStreamOut* output, audio_io_handle_t id, uint32_t device)
    : DirectOutputThread(audioFlinger, output, id, device, OFFLOAD),
        mHwPaused(false),
        mFlushPending(false),
        mPausedBytesRemaining(0)
{
    mStandby = true;
}
void AudioFlinger::OffloadThread::threadLoop_exit()
{
    if (mFlushPending || mHwPaused) {
        flushHw_l();
    } else {
        mMixerStatus = MIXER_DRAIN_ALL;
        threadLoop_drain();
    }
    mCallbackThread->exit();
    PlaybackThread::threadLoop_exit();
}
AudioFlinger::PlaybackThread::mixer_state AudioFlinger::OffloadThread::prepareTracks_l(
    Vector< sp<Track> > *tracksToRemove
)
{
    size_t count = mActiveTracks.size();
    mixer_state mixerStatus = MIXER_IDLE;
    bool doHwPause = false;
    bool doHwResume = false;
    ALOGV("OffloadThread::prepareTracks_l active tracks %d", count);
    for (size_t i = 0; i < count; i++) {
        sp<Track> t = mActiveTracks[i].promote();
        if (t == 0) {
            continue;
        }
        Track* const track = t.get();
        audio_track_cblk_t* cblk = track->cblk();
        sp<Track> l = mLatestActiveTrack.promote();
        bool last = l.get() == track;
        if (track->isInvalid()) {
            ALOGW("An invalidated track shouldn't be in active list");
            tracksToRemove->add(track);
            continue;
        }
        if (track->mState == TrackBase::IDLE) {
            ALOGW("An idle track shouldn't be in active list");
            continue;
        }
        if (track->isPausing()) {
            track->setPaused();
            if (last) {
                if (!mHwPaused) {
                    doHwPause = true;
                    mHwPaused = true;
                }
                mPausedWriteLength = mCurrentWriteLength;
                mPausedBytesRemaining = mBytesRemaining;
                mBytesRemaining = 0;
            }
            tracksToRemove->add(track);
        } else if (track->isFlushPending()) {
            track->flushAck();
            if (last) {
                mFlushPending = true;
            }
        } else if (track->framesReady() && track->isReady() &&
                !track->isPaused() && !track->isTerminated() && !track->isStopping_2()) {
            ALOGVV("OffloadThread: track %d s=%08x [OK]", track->name(), cblk->mServer);
            if (track->mFillingUpStatus == Track::FS_FILLED) {
                track->mFillingUpStatus = Track::FS_ACTIVE;
                mLeftVolFloat = mRightVolFloat = -1.0;
                if (track->mState == TrackBase::RESUMING) {
                    track->mState = TrackBase::ACTIVE;
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
                        sleepTime = 0;
                    }
                }
            }
            if (last) {
                sp<Track> previousTrack = mPreviousTrack.promote();
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
                track->mRetryCount = kMaxTrackRetriesOffload;
                mActiveTrack = t;
                mixerStatus = MIXER_TRACKS_READY;
            }
        } else {
            ALOGVV("OffloadThread: track %d s=%08x [NOT READY]", track->name(), cblk->mServer);
            if (track->isStopping_1()) {
                if (mBytesRemaining == 0) {
                    ALOGV("OffloadThread: underrun and STOPPING_1 -> draining, STOPPING_2");
                    track->mState = TrackBase::STOPPING_2;
                    if (last && !mStandby) {
                        if ((mDrainSequence & 1) == 0) {
                            sleepTime = 0;
                            standbyTime = systemTime() + standbyDelay;
                            mixerStatus = MIXER_DRAIN_TRACK;
                            mDrainSequence += 2;
                        }
                        if (mHwPaused) {
                            doHwResume = true;
                            mHwPaused = false;
                        }
                    }
                }
            } else if (track->isStopping_2()) {
                if (!(mDrainSequence & 1) || !last || mStandby) {
                    track->mState = TrackBase::STOPPED;
                    size_t audioHALFrames =
                            (mOutput->stream->get_latency(mOutput->stream)*mSampleRate) / 1000;
                    size_t framesWritten =
                            mBytesWritten / audio_stream_frame_size(&mOutput->stream->common);
                    track->presentationComplete(framesWritten, audioHALFrames);
                    track->reset();
                    tracksToRemove->add(track);
                }
            } else {
                if (--(track->mRetryCount) <= 0) {
                    ALOGV("OffloadThread: BUFFER TIMEOUT: remove(%d) from active list",
                          track->name());
                    tracksToRemove->add(track);
                    android_atomic_or(CBLK_DISABLED, &cblk->mFlags);
                } else if (last){
                    mixerStatus = MIXER_TRACKS_ENABLED;
                }
            }
        }
        processVolume_l(track, last);
    }
    if (!mStandby && (doHwPause || (mFlushPending && !mHwPaused && (count != 0)))) {
        mOutput->stream->pause(mOutput->stream);
    }
    if (mFlushPending) {
        flushHw_l();
        mFlushPending = false;
    }
    if (!mStandby && doHwResume) {
        mOutput->stream->resume(mOutput->stream);
    }
    removeTracks_l(*tracksToRemove);
    return mixerStatus;
}
bool AudioFlinger::OffloadThread::waitingAsyncCallback_l()
{
    ALOGVV("waitingAsyncCallback_l mWriteAckSequence %d mDrainSequence %d",
          mWriteAckSequence, mDrainSequence);
    if (mUseAsyncWrite && ((mWriteAckSequence & 1) || (mDrainSequence & 1))) {
        return true;
    }
    return false;
}
bool AudioFlinger::OffloadThread::shouldStandby_l()
{
    bool trackPaused = false;
    if (mTracks.size() > 0) {
        trackPaused = mTracks[mTracks.size() - 1]->isPaused();
    }
    return !mStandby && !trackPaused;
}
bool AudioFlinger::OffloadThread::waitingAsyncCallback()
{
    Mutex::Autolock _l(mLock);
    return waitingAsyncCallback_l();
}
void AudioFlinger::OffloadThread::flushHw_l()
{
    mOutput->stream->flush(mOutput->stream);
    mCurrentWriteLength = 0;
    mBytesRemaining = 0;
    mPausedWriteLength = 0;
    mPausedBytesRemaining = 0;
    mHwPaused = false;
    if (mUseAsyncWrite) {
        mWriteAckSequence = (mWriteAckSequence + 2) & ~1;
        mDrainSequence = (mDrainSequence + 2) & ~1;
        ALOG_ASSERT(mCallbackThread != 0);
        mCallbackThread->setWriteBlocked(mWriteAckSequence);
        mCallbackThread->setDraining(mDrainSequence);
    }
}
void AudioFlinger::OffloadThread::onAddNewTrack_l()
{
    sp<Track> previousTrack = mPreviousTrack.promote();
    sp<Track> latestTrack = mLatestActiveTrack.promote();
    if (previousTrack != 0 && latestTrack != 0 &&
        (previousTrack->sessionId() != latestTrack->sessionId())) {
        mFlushPending = true;
    }
    PlaybackThread::onAddNewTrack_l();
}
AudioFlinger::DuplicatingThread::DuplicatingThread(const sp<AudioFlinger>& audioFlinger,
        AudioFlinger::MixerThread* mainThread, audio_io_handle_t id)
    : MixerThread(audioFlinger, mainThread->getOutput(), id, mainThread->outDevice(),
                DUPLICATING),
        mWaitTimeMs(UINT_MAX)
{
    addOutputTrack(mainThread);
}
AudioFlinger::DuplicatingThread::~DuplicatingThread()
{
    for (size_t i = 0; i < mOutputTracks.size(); i++) {
        mOutputTracks[i]->destroy();
    }
}
void AudioFlinger::DuplicatingThread::threadLoop_mix()
{
    if (outputsReady(outputTracks)) {
        mAudioMixer->process(AudioBufferProvider::kInvalidPTS);
    } else {
        memset(mMixBuffer, 0, mixBufferSize);
    }
    sleepTime = 0;
    writeFrames = mNormalFrameCount;
    mCurrentWriteLength = mixBufferSize;
    standbyTime = systemTime() + standbyDelay;
}
void AudioFlinger::DuplicatingThread::threadLoop_sleepTime()
{
    if (sleepTime == 0) {
        if (mMixerStatus == MIXER_TRACKS_ENABLED) {
            sleepTime = activeSleepTime;
        } else {
            sleepTime = idleSleepTime;
        }
    } else if (mBytesWritten != 0) {
        if (mMixerStatus == MIXER_TRACKS_ENABLED) {
            writeFrames = mNormalFrameCount;
            memset(mMixBuffer, 0, mixBufferSize);
        } else {
            writeFrames = 0;
        }
        sleepTime = 0;
    }
}
ssize_t AudioFlinger::DuplicatingThread::threadLoop_write()
{
    for (size_t i = 0; i < outputTracks.size(); i++) {
        outputTracks[i]->write(mMixBuffer, writeFrames);
    }
    mStandby = false;
    return (ssize_t)mixBufferSize;
}
void AudioFlinger::DuplicatingThread::threadLoop_standby()
{
    for (size_t i = 0; i < outputTracks.size(); i++) {
        outputTracks[i]->stop();
    }
}
void AudioFlinger::DuplicatingThread::saveOutputTracks()
{
    outputTracks = mOutputTracks;
}
void AudioFlinger::DuplicatingThread::clearOutputTracks()
{
    outputTracks.clear();
}
void AudioFlinger::DuplicatingThread::addOutputTrack(MixerThread *thread)
{
    Mutex::Autolock _l(mLock);
    size_t frameCount = (3 * mNormalFrameCount * mSampleRate) / thread->sampleRate();
    OutputTrack *outputTrack = new OutputTrack(thread,
                                            this,
                                            mSampleRate,
                                            mFormat,
                                            mChannelMask,
                                            frameCount,
                                            IPCThreadState::self()->getCallingUid());
    if (outputTrack->cblk() != NULL) {
        thread->setStreamVolume(AUDIO_STREAM_CNT, 1.0f);
        mOutputTracks.add(outputTrack);
        ALOGV("addOutputTrack() track %p, on thread %p", outputTrack, thread);
        updateWaitTime_l();
    }
}
void AudioFlinger::DuplicatingThread::removeOutputTrack(MixerThread *thread)
{
    Mutex::Autolock _l(mLock);
    for (size_t i = 0; i < mOutputTracks.size(); i++) {
        if (mOutputTracks[i]->thread() == thread) {
            mOutputTracks[i]->destroy();
            mOutputTracks.removeAt(i);
            updateWaitTime_l();
            return;
        }
    }
    ALOGV("removeOutputTrack(): unkonwn thread: %p", thread);
}
void AudioFlinger::DuplicatingThread::updateWaitTime_l()
{
    mWaitTimeMs = UINT_MAX;
    for (size_t i = 0; i < mOutputTracks.size(); i++) {
        sp<ThreadBase> strong = mOutputTracks[i]->thread().promote();
        if (strong != 0) {
            uint32_t waitTimeMs = (strong->frameCount() * 2 * 1000) / strong->sampleRate();
            if (waitTimeMs < mWaitTimeMs) {
                mWaitTimeMs = waitTimeMs;
            }
        }
    }
}
bool AudioFlinger::DuplicatingThread::outputsReady(
        const SortedVector< sp<OutputTrack> > &outputTracks)
{
    for (size_t i = 0; i < outputTracks.size(); i++) {
        sp<ThreadBase> thread = outputTracks[i]->thread().promote();
        if (thread == 0) {
            ALOGW("DuplicatingThread::outputsReady() could not promote thread on output track %p",
                    outputTracks[i].get());
            return false;
        }
        PlaybackThread *playbackThread = (PlaybackThread *)thread.get();
        if (playbackThread->standby() && !playbackThread->isSuspended()) {
            ALOGV("DuplicatingThread output track %p on thread %p Not Ready", outputTracks[i].get(),
                    thread.get());
            return false;
        }
    }
    return true;
}
uint32_t AudioFlinger::DuplicatingThread::activeSleepTimeUs() const
{
    return (mWaitTimeMs * 1000) / 2;
}
void AudioFlinger::DuplicatingThread::cacheParameters_l()
{
    updateWaitTime_l();
    MixerThread::cacheParameters_l();
}
AudioFlinger::RecordThread::RecordThread(const sp<AudioFlinger>& audioFlinger,
                                         AudioStreamIn *input,
                                         uint32_t sampleRate,
                                         audio_channel_mask_t channelMask,
                                         audio_io_handle_t id,
                                         audio_devices_t outDevice,
                                         audio_devices_t inDevice
#ifdef TEE_SINK
                                         , const sp<NBAIO_Sink>& teeSink
#endif
                                         ) :
    ThreadBase(audioFlinger, id, outDevice, inDevice, RECORD),
    mInput(input), mActiveTracksGen(0), mResampler(NULL), mRsmpOutBuffer(NULL), mRsmpInBuffer(NULL),
    mReqChannelCount(popcount(channelMask)),
    mReqSampleRate(sampleRate)
#ifdef TEE_SINK
    , mTeeSink(teeSink)
#endif
{
    snprintf(mName, kNameLength, "AudioIn_%X", id);
    mNBLogWriter = audioFlinger->newWriter_l(kLogSize, mName);
    readInputParameters();
}
AudioFlinger::RecordThread::~RecordThread()
{
    mAudioFlinger->unregisterWriter(mNBLogWriter);
    delete[] mRsmpInBuffer;
    delete mResampler;
    delete[] mRsmpOutBuffer;
}{
    mAudioFlinger->unregisterWriter(mNBLogWriter);
    delete[] mRsmpInBuffer;
    delete mResampler;
    delete[] mRsmpOutBuffer;
}
void AudioFlinger::RecordThread::onFirstRef()
{
    run(mName, PRIORITY_URGENT_AUDIO);
}
bool AudioFlinger::RecordThread::threadLoop()
{
    nsecs_t lastWarning = 0;
    inputStandBy();
    bool readOnce = false;
    bool doSleep = false;
reacquire_wakelock:
    sp<RecordTrack> activeTrack;
    int activeTracksGen;
    {
        Mutex::Autolock _l(mLock);
        size_t size = mActiveTracks.size();
        activeTracksGen = mActiveTracksGen;
        if (size > 0) {
            activeTrack = mActiveTracks[0];
            acquireWakeLock_l(activeTrack->uid());
            if (size > 1) {
                SortedVector<int> tmp;
                for (size_t i = 0; i < size; i++) {
                    tmp.add(mActiveTracks[i]->uid());
                }
                updateWakeLockUids_l(tmp);
            }
        } else {
            acquireWakeLock_l(-1);
        }
    }
    for (;;) {
        TrackBase::track_state activeTrackState;
        Vector< sp<EffectChain> > effectChains;
        if (doSleep) {
            doSleep = false;
            usleep(kRecordThreadSleepUs);
        }
        {
            Mutex::Autolock _l(mLock);
            processConfigEvents_l();
            bool reconfig = checkForNewParameters_l();
            if (exitPending()) {
                break;
            }
            size_t size = mActiveTracks.size();
            if (size == 0) {
                standbyIfNotAlreadyInStandby();
                releaseWakeLock_l();
                ALOGV("RecordThread: loop stopping");
                mWaitWorkCV.wait(mLock);
                ALOGV("RecordThread: loop starting");
                goto reacquire_wakelock;
            }
            if (mActiveTracksGen != activeTracksGen) {
                activeTracksGen = mActiveTracksGen;
                SortedVector<int> tmp;
                for (size_t i = 0; i < size; i++) {
                    tmp.add(mActiveTracks[i]->uid());
                }
                updateWakeLockUids_l(tmp);
                activeTrack = mActiveTracks[0];
            }
            if (activeTrack->isTerminated()) {
                removeTrack_l(activeTrack);
                mActiveTracks.remove(activeTrack);
                mActiveTracksGen++;
                continue;
            }
            activeTrackState = activeTrack->mState;
            switch (activeTrackState) {
            case TrackBase::PAUSING:
                standbyIfNotAlreadyInStandby();
                mActiveTracks.remove(activeTrack);
                mActiveTracksGen++;
                mStartStopCond.broadcast();
                doSleep = true;
                continue;
            case TrackBase::RESUMING:
                mStandby = false;
                if (mReqChannelCount != activeTrack->channelCount()) {
                    mActiveTracks.remove(activeTrack);
                    mActiveTracksGen++;
                    mStartStopCond.broadcast();
                    continue;
                }
                if (readOnce) {
                    mStartStopCond.broadcast();
                    if (mBytesRead < 0) {
                        mActiveTracks.remove(activeTrack);
                        mActiveTracksGen++;
                        continue;
                    }
                    activeTrack->mState = TrackBase::ACTIVE;
                }
                break;
            case TrackBase::ACTIVE:
                break;
            case TrackBase::IDLE:
                doSleep = true;
                continue;
            default:
                LOG_FATAL("Unexpected activeTrackState %d", activeTrackState);
            }
            lockEffectChains_l(effectChains);
        }
        for (size_t i = 0; i < effectChains.size(); i ++) {
            effectChains[i]->process_l();
        }
        AudioBufferProvider::Buffer buffer;
        buffer.frameCount = mFrameCount;
        status_t status = activeTrack->getNextBuffer(&buffer);
        if (status == NO_ERROR) {
            readOnce = true;
            size_t framesOut = buffer.frameCount;
            if (mResampler == NULL) {
                while (framesOut) {
                    size_t framesIn = mFrameCount - mRsmpInIndex;
                    if (framesIn > 0) {
                        int8_t *src = (int8_t *)mRsmpInBuffer + mRsmpInIndex * mFrameSize;
                        int8_t *dst = buffer.i8 + (buffer.frameCount - framesOut) *
                                activeTrack->mFrameSize;
                        if (framesIn > framesOut) {
                            framesIn = framesOut;
                        }
                        mRsmpInIndex += framesIn;
                        framesOut -= framesIn;
                        if (mChannelCount == mReqChannelCount) {
                            memcpy(dst, src, framesIn * mFrameSize);
                        } else {
                            if (mChannelCount == 1) {
                                upmix_to_stereo_i16_from_mono_i16((int16_t *)dst,
                                        (int16_t *)src, framesIn);
                            } else {
                                downmix_to_mono_i16_from_stereo_i16((int16_t *)dst,
                                        (int16_t *)src, framesIn);
                            }
                        }
                    }
                    if (framesOut > 0 && mFrameCount == mRsmpInIndex) {
                        void *readInto;
                        if (framesOut == mFrameCount && mChannelCount == mReqChannelCount) {
                            readInto = buffer.raw;
                            framesOut = 0;
                        } else {
                            readInto = mRsmpInBuffer;
                            mRsmpInIndex = 0;
                        }
                        mBytesRead = mInput->stream->read(mInput->stream, readInto, mBufferSize);
                        if (mBytesRead <= 0) {
                            if ((mBytesRead < 0) && (activeTrackState == TrackBase::ACTIVE))
                            {
                                ALOGE("Error reading audio input");
                                inputStandBy();
                                doSleep = true;
                            }
                            mRsmpInIndex = mFrameCount;
                            framesOut = 0;
                            buffer.frameCount = 0;
                        }
#ifdef TEE_SINK
                        else if (mTeeSink != 0) {
                            (void) mTeeSink->write(readInto,
                                    mBytesRead >> Format_frameBitShift(mTeeSink->format()));
                        }
#endif
                    }
                }
            } else {
                bool madeProgress = false;
                for (;;) {
                    int32_t rear = mRsmpInRear;
                    ssize_t filled = rear - mRsmpInFront;
                    ALOG_ASSERT(0 <= filled && (size_t) filled <= mRsmpInFramesP2);
                    if ((size_t) filled >= mRsmpInFrames) {
                        break;
                    }
                    size_t avail = mRsmpInFramesP2 - filled;
                    if (avail < mFrameCount) {
                        ALOGE("insufficient space to read: avail %d < mFrameCount %d",
                                avail, mFrameCount);
                        break;
                    }
                    rear &= mRsmpInFramesP2 - 1;
                    mBytesRead = mInput->stream->read(mInput->stream,
                            &mRsmpInBuffer[rear * mChannelCount], mBufferSize);
                    if (mBytesRead <= 0) {
                        ALOGE("read failed: mBytesRead=%d < %u", mBytesRead, mBufferSize);
                        break;
                    }
                    ALOG_ASSERT((size_t) mBytesRead <= mBufferSize);
                    size_t framesRead = mBytesRead / mFrameSize;
                    ALOG_ASSERT(framesRead > 0);
                    madeProgress = true;
                    size_t part1 = mRsmpInFramesP2 - rear;
                    if (framesRead > part1) {
                        memcpy(mRsmpInBuffer, &mRsmpInBuffer[mRsmpInFramesP2 * mChannelCount],
                                (framesRead - part1) * mFrameSize);
                    }
                    mRsmpInRear += framesRead;
                }
                if (!madeProgress) {
                    ALOGV("Did not make progress");
                    usleep(((mFrameCount * 1000) / mSampleRate) * 1000);
                }
                memset(mRsmpOutBuffer, 0, framesOut * FCC_2 * sizeof(int32_t));
                mResampler->resample(mRsmpOutBuffer, framesOut,
                        this );
                if (mReqChannelCount == 1) {
                    ditherAndClamp(mRsmpOutBuffer, mRsmpOutBuffer, framesOut);
                    downmix_to_mono_i16_from_stereo_i16(buffer.i16, (int16_t *)mRsmpOutBuffer,
                            framesOut);
                } else {
                    ditherAndClamp((int32_t *)buffer.raw, mRsmpOutBuffer, framesOut);
                }
            }
            if (mFramestoDrop == 0) {
                activeTrack->releaseBuffer(&buffer);
            } else {
                if (mFramestoDrop > 0) {
                    mFramestoDrop -= buffer.frameCount;
                    if (mFramestoDrop <= 0) {
                        clearSyncStartEvent();
                    }
                } else {
                    mFramestoDrop += buffer.frameCount;
                    if (mFramestoDrop >= 0 || mSyncStartEvent == 0 ||
                            mSyncStartEvent->isCancelled()) {
                        ALOGW("Synced record %s, session %d, trigger session %d",
                              (mFramestoDrop >= 0) ? "timed out" : "cancelled",
                              activeTrack->sessionId(),
                              (mSyncStartEvent != 0) ? mSyncStartEvent->triggerSession() : 0);
                        clearSyncStartEvent();
                    }
                }
            }
            activeTrack->clearOverflow();
        }
        else {
            if (!activeTrack->setOverflow()) {
                nsecs_t now = systemTime();
                if ((now - lastWarning) > kWarningThrottleNs) {
                    ALOGW("RecordThread: buffer overflow");
                    lastWarning = now;
                }
            }
            doSleep = true;
        }
        unlockEffectChains(effectChains);
    }
    standbyIfNotAlreadyInStandby();
    {
        Mutex::Autolock _l(mLock);
        for (size_t i = 0; i < mTracks.size(); i++) {
            sp<RecordTrack> track = mTracks[i];
            track->invalidate();
        }
        mActiveTracks.clear();
        mActiveTracksGen++;
        mStartStopCond.broadcast();
    }
    releaseWakeLock();
    ALOGV("RecordThread %p exiting", this);
    return false;
}
void AudioFlinger::RecordThread::standbyIfNotAlreadyInStandby()
{
    if (!mStandby) {
        inputStandBy();
        mStandby = true;
    }
}
void AudioFlinger::RecordThread::inputStandBy()
{
    mInput->stream->common.standby(&mInput->stream->common);
}
sp<AudioFlinger::RecordThread::RecordTrack> AudioFlinger::RecordThread::createRecordTrack_l(const sp<AudioFlinger::Client>& client, uint32_t sampleRate, audio_format_t format, audio_channel_mask_t channelMask, size_t *pFrameCount, int sessionId, int uid, IAudioFlinger::track_flags_t *flags, pid_t tid, status_t *status)
{
    size_t frameCount = *pFrameCount;
    sp<RecordTrack> track;
    status_t lStatus;
    lStatus = initCheck();
    if (lStatus != NO_ERROR) {
        ALOGE("createRecordTrack_l() audio driver not initialized");
        goto Exit;
    }
    if (*flags & IAudioFlinger::TRACK_FAST) {
      if (
            (
                (tid != -1) &&
                ((frameCount == 0) ||
                (frameCount >= mFrameCount))
            ) &&
            ( (channelMask == AUDIO_CHANNEL_OUT_MONO) ||
              (channelMask == AUDIO_CHANNEL_OUT_STEREO) ) &&
            (sampleRate == mSampleRate) &&
            hasFastRecorder()
        ) {
        if (frameCount == 0) {
            frameCount = mFrameCount * kFastTrackMultiplier;
        }
        ALOGV("AUDIO_INPUT_FLAG_FAST accepted: frameCount=%d mFrameCount=%d",
                frameCount, mFrameCount);
      } else {
        ALOGV("AUDIO_INPUT_FLAG_FAST denied: frameCount=%d "
                "mFrameCount=%d format=%d isLinear=%d channelMask=%#x sampleRate=%u mSampleRate=%u "
                "hasFastRecorder=%d tid=%d",
                frameCount, mFrameCount, format,
                audio_is_linear_pcm(format),
                channelMask, sampleRate, mSampleRate, hasFastRecorder(), tid);
        *flags &= ~IAudioFlinger::TRACK_FAST;
        uint32_t latencyMs = 50;
        size_t mNormalFrameCount = 2048;
        uint32_t minBufCount = latencyMs / ((1000 * mNormalFrameCount) / mSampleRate);
        if (minBufCount < 2) {
            minBufCount = 2;
        }
        size_t minFrameCount = mNormalFrameCount * minBufCount;
        if (frameCount < minFrameCount) {
            frameCount = minFrameCount;
        }
      }
    }
    *pFrameCount = frameCount;
    {
        Mutex::Autolock _l(mLock);
        track = new RecordTrack(this, client, sampleRate,
                      format, channelMask, frameCount, sessionId, uid);
        lStatus = track->initCheck();
        if (lStatus != NO_ERROR) {
            ALOGE("createRecordTrack_l() initCheck failed %d; no control block?", lStatus);
            goto Exit;
        }
        mTracks.add(track);
        bool suspend = audio_is_bluetooth_sco_device(mInDevice) &&
                        mAudioFlinger->btNrecIsOff();
        setEffectSuspended_l(FX_IID_AEC, suspend, sessionId);
        setEffectSuspended_l(FX_IID_NS, suspend, sessionId);
        if ((*flags & IAudioFlinger::TRACK_FAST) && (tid != -1)) {
            pid_t callingPid = IPCThreadState::self()->getCallingPid();
            sendPrioConfigEvent_l(callingPid, tid, kPriorityAudioApp);
        }
    }
    lStatus = NO_ERROR;
Exit:
    *status = lStatus;
    return track;
}
status_t AudioFlinger::RecordThread::start(RecordThread::RecordTrack* recordTrack,
                                           AudioSystem::sync_event_t event,
                                           int triggerSession)
{
    ALOGV("RecordThread::start event %d, triggerSession %d", event, triggerSession);
    sp<ThreadBase> strongMe = this;
    status_t status = NO_ERROR;
    if (event == AudioSystem::SYNC_EVENT_NONE) {
        clearSyncStartEvent();
    } else if (event != AudioSystem::SYNC_EVENT_SAME) {
        mSyncStartEvent = mAudioFlinger->createSyncEvent(event,
                                       triggerSession,
                                       recordTrack->sessionId(),
                                       syncStartEventCallback,
                                       this);
        if (mSyncStartEvent->isCancelled()) {
            clearSyncStartEvent();
        } else {
            mFramestoDrop = - ((AudioSystem::kSyncRecordStartTimeOutMs * mReqSampleRate) / 1000);
        }
    }
    {
        AutoMutex lock(mLock);
        if (mActiveTracks.size() > 0) {
            if (mActiveTracks.indexOf(recordTrack) != 0) {
                status = -EBUSY;
            } else if (recordTrack->mState == TrackBase::PAUSING) {
                recordTrack->mState = TrackBase::ACTIVE;
            }
            return status;
        }
        recordTrack->mState = TrackBase::IDLE;
        mActiveTracks.add(recordTrack);
        mActiveTracksGen++;
        mLock.unlock();
        status_t status = AudioSystem::startInput(mId);
        mLock.lock();
        if (status != NO_ERROR) {
            mActiveTracks.remove(recordTrack);
            mActiveTracksGen++;
            clearSyncStartEvent();
            return status;
        }
        mRsmpInIndex = mFrameCount;
        mRsmpInFront = 0;
        mRsmpInRear = 0;
        mRsmpInUnrel = 0;
        mBytesRead = 0;
        if (mResampler != NULL) {
            mResampler->reset();
        }
        recordTrack->mState = TrackBase::RESUMING;
        ALOGV("Signal record thread");
        mWaitWorkCV.broadcast();
        if (exitPending()) {
            mActiveTracks.remove(recordTrack);
            mActiveTracksGen++;
            status = INVALID_OPERATION;
            goto startError;
        }
        mStartStopCond.wait(mLock);
        if (mActiveTracks.indexOf(recordTrack) < 0) {
            ALOGV("Record failed to start");
            status = BAD_VALUE;
            goto startError;
        }
        ALOGV("Record started OK");
        return status;
    }
startError:
    AudioSystem::stopInput(mId);
    clearSyncStartEvent();
    return status;
}
void AudioFlinger::RecordThread::clearSyncStartEvent()
{
    if (mSyncStartEvent != 0) {
        mSyncStartEvent->cancel();
    }
    mSyncStartEvent.clear();
    mFramestoDrop = 0;
}
void AudioFlinger::RecordThread::syncStartEventCallback(const wp<SyncEvent>& event)
{
    sp<SyncEvent> strongEvent = event.promote();
    if (strongEvent != 0) {
        RecordThread *me = (RecordThread *)strongEvent->cookie();
        me->handleSyncStartEvent(strongEvent);
    }
}
void AudioFlinger::RecordThread::handleSyncStartEvent(const sp<SyncEvent>& event)
{
    if (event == mSyncStartEvent) {
        mFramestoDrop = mFrameCount * 2;
    }
}
bool AudioFlinger::RecordThread::stop(RecordThread::RecordTrack* recordTrack) {
    ALOGV("RecordThread::stop");
    AutoMutex _l(mLock);
    if (mActiveTracks.indexOf(recordTrack) != 0 || recordTrack->mState == TrackBase::PAUSING) {
        return false;
    }
    recordTrack->mState = TrackBase::PAUSING;
    if (exitPending()) {
        return true;
    }
    mStartStopCond.wait(mLock);
    if (exitPending() || mActiveTracks.indexOf(recordTrack) != 0) {
        ALOGV("Record stopped OK");
        return true;
    }
    return false;
}
bool AudioFlinger::RecordThread::isValidSyncEvent(const sp<SyncEvent>& event __unused) const
{
    return false;
}
status_t AudioFlinger::RecordThread::setSyncEvent(const sp<SyncEvent>& event __unused)
{
#if 0
    if (!isValidSyncEvent(event)) {
        return BAD_VALUE;
    }
    int eventSession = event->triggerSession();
    status_t ret = NAME_NOT_FOUND;
    Mutex::Autolock _l(mLock);
    for (size_t i = 0; i < mTracks.size(); i++) {
        sp<RecordTrack> track = mTracks[i];
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
void AudioFlinger::RecordThread::destroyTrack_l(const sp<RecordTrack>& track)
{
    track->terminate();
    track->mState = TrackBase::STOPPED;
    if (mActiveTracks.indexOf(track) < 0) {
        removeTrack_l(track);
    }
}
void AudioFlinger::RecordThread::removeTrack_l(const sp<RecordTrack>& track)
{
    mTracks.remove(track);
}
void AudioFlinger::RecordThread::dump(int fd, const Vector<String16>& args)
{
    dumpInternals(fd, args);
    dumpTracks(fd, args);
    dumpEffectChains(fd, args);
}
void AudioFlinger::RecordThread::dumpInternals(int fd, const Vector<String16>& args)
{
    fdprintf(fd, "\nInput thread %p:\n", this);
<<<<<<< HEAD
    if (mActiveTracks.size() > 0) {
        fdprintf(fd, "  In index: %d\n", mRsmpInIndex);
        fdprintf(fd, "  Buffer size: %u bytes\n", mBufferSize);
        fdprintf(fd, "  Resampling: %d\n", (mResampler != NULL));
        fdprintf(fd, "  Out channel count: %u\n", mReqChannelCount);
        fdprintf(fd, "  Out sample rate: %u\n", mReqSampleRate);
||||||| 66e05682e4
    snprintf(buffer, SIZE, "\nInput thread %p internals\n", this);
    result.append(buffer);
    if (mActiveTrack != 0) {
        snprintf(buffer, SIZE, "In index: %d\n", mRsmpInIndex);
        result.append(buffer);
        snprintf(buffer, SIZE, "Buffer size: %u bytes\n", mBufferSize);
        result.append(buffer);
        snprintf(buffer, SIZE, "Resampling: %d\n", (mResampler != NULL));
        result.append(buffer);
        snprintf(buffer, SIZE, "Out channel count: %u\n", mReqChannelCount);
        result.append(buffer);
        snprintf(buffer, SIZE, "Out sample rate: %u\n", mReqSampleRate);
        result.append(buffer);
=======
    snprintf(buffer, SIZE, "\nInput thread %p internals\n", this);
    result.append(buffer);
    if (mActiveTrack != 0) {
        snprintf(buffer, SIZE, "In index: %zu\n", mRsmpInIndex);
        result.append(buffer);
        snprintf(buffer, SIZE, "Buffer size: %zu bytes\n", mBufferSize);
        result.append(buffer);
        snprintf(buffer, SIZE, "Resampling: %d\n", (mResampler != NULL));
        result.append(buffer);
        snprintf(buffer, SIZE, "Out channel count: %u\n", mReqChannelCount);
        result.append(buffer);
        snprintf(buffer, SIZE, "Out sample rate: %u\n", mReqSampleRate);
        result.append(buffer);
>>>>>>> 566be7c3
    } else {
        fdprintf(fd, "  No active record client\n");
    }
    dumpBase(fd, args);
}
void AudioFlinger::RecordThread::dumpTracks(int fd, const Vector<String16>& args __unused)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    size_t numtracks = mTracks.size();
    size_t numactive = mActiveTracks.size();
    size_t numactiveseen = 0;
    fdprintf(fd, "  %d Tracks", numtracks);
    if (numtracks) {
        fdprintf(fd, " of which %d are active\n", numactive);
        RecordTrack::appendDumpHeader(result);
        for (size_t i = 0; i < numtracks ; ++i) {
            sp<RecordTrack> track = mTracks[i];
            if (track != 0) {
                bool active = mActiveTracks.indexOf(track) >= 0;
                if (active) {
                    numactiveseen++;
                }
                track->dump(buffer, SIZE, active);
                result.append(buffer);
            }
        }
    } else {
        fdprintf(fd, "\n");
    }
    if (numactiveseen != numactive) {
        snprintf(buffer, SIZE, "  The following tracks are in the active list but"
                " not in the track list\n");
        result.append(buffer);
        RecordTrack::appendDumpHeader(result);
        for (size_t i = 0; i < numactive; ++i) {
            sp<RecordTrack> track = mActiveTracks[i];
            if (mTracks.indexOf(track) < 0) {
                track->dump(buffer, SIZE, true);
                result.append(buffer);
            }
        }
    }
    write(fd, result.string(), result.size());
}
status_t AudioFlinger::RecordThread::getNextBuffer(AudioBufferProvider::Buffer* buffer, int64_t pts __unused)
{
    int32_t rear = mRsmpInRear;
    int32_t front = mRsmpInFront;
    ssize_t filled = rear - front;
    ALOG_ASSERT(0 <= filled && (size_t) filled <= mRsmpInFramesP2);
    front &= mRsmpInFramesP2 - 1;
    size_t part1 = mRsmpInFramesP2 - front;
    if (part1 > (size_t) filled) {
        part1 = filled;
    }
    size_t ask = buffer->frameCount;
    ALOG_ASSERT(ask > 0);
    if (part1 > ask) {
        part1 = ask;
    }
    if (part1 == 0) {
        ALOGE("RecordThread::getNextBuffer() starved");
        buffer->raw = NULL;
        buffer->frameCount = 0;
        mRsmpInUnrel = 0;
        return NOT_ENOUGH_DATA;
    }
    buffer->raw = mRsmpInBuffer + front * mChannelCount;
    buffer->frameCount = part1;
    mRsmpInUnrel = part1;
    return NO_ERROR;
}
void AudioFlinger::RecordThread::releaseBuffer(AudioBufferProvider::Buffer* buffer)
{
    size_t stepCount = buffer->frameCount;
    if (stepCount == 0) {
        return;
    }
    ALOG_ASSERT(stepCount <= mRsmpInUnrel);
    mRsmpInUnrel -= stepCount;
    mRsmpInFront += stepCount;
    buffer->raw = NULL;
    buffer->frameCount = 0;
}
bool AudioFlinger::RecordThread::checkForNewParameters_l()
{
    bool reconfig = false;
    while (!mNewParameters.isEmpty()) {
        status_t status = NO_ERROR;
        String8 keyValuePair = mNewParameters[0];
        AudioParameter param = AudioParameter(keyValuePair);
        int value;
        audio_format_t reqFormat = mFormat;
        uint32_t reqSamplingRate = mReqSampleRate;
        audio_channel_mask_t reqChannelMask = audio_channel_in_mask_from_count(mReqChannelCount);
        if (param.getInt(String8(AudioParameter::keySamplingRate), value) == NO_ERROR) {
            reqSamplingRate = value;
            reconfig = true;
        }
        if (param.getInt(String8(AudioParameter::keyFormat), value) == NO_ERROR) {
            if ((audio_format_t) value != AUDIO_FORMAT_PCM_16_BIT) {
                status = BAD_VALUE;
            } else {
                reqFormat = (audio_format_t) value;
                reconfig = true;
            }
        }
        if (param.getInt(String8(AudioParameter::keyChannels), value) == NO_ERROR) {
            audio_channel_mask_t mask = (audio_channel_mask_t) value;
            if (mask != AUDIO_CHANNEL_IN_MONO && mask != AUDIO_CHANNEL_IN_STEREO) {
                status = BAD_VALUE;
            } else {
                reqChannelMask = mask;
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
            for (size_t i = 0; i < mEffectChains.size(); i++) {
                mEffectChains[i]->setDevice_l(value);
            }
            if (audio_is_output_devices(value)) {
                mOutDevice = value;
                status = BAD_VALUE;
            } else {
                mInDevice = value;
                if (mTracks.size() > 0) {
                    bool suspend = audio_is_bluetooth_sco_device(mInDevice) &&
                                        mAudioFlinger->btNrecIsOff();
                    for (size_t i = 0; i < mTracks.size(); i++) {
                        sp<RecordTrack> track = mTracks[i];
                        setEffectSuspended_l(FX_IID_AEC, suspend, track->sessionId());
                        setEffectSuspended_l(FX_IID_NS, suspend, track->sessionId());
                    }
                }
            }
        }
        if (param.getInt(String8(AudioParameter::keyInputSource), value) == NO_ERROR &&
                mAudioSource != (audio_source_t)value) {
            for (size_t i = 0; i < mEffectChains.size(); i++) {
                mEffectChains[i]->setAudioSource_l((audio_source_t)value);
            }
            mAudioSource = (audio_source_t)value;
        }
        if (status == NO_ERROR) {
            status = mInput->stream->common.set_parameters(&mInput->stream->common,
                    keyValuePair.string());
            if (status == INVALID_OPERATION) {
                inputStandBy();
                status = mInput->stream->common.set_parameters(&mInput->stream->common,
                        keyValuePair.string());
            }
            if (reconfig) {
                if (status == BAD_VALUE &&
                    reqFormat == mInput->stream->common.get_format(&mInput->stream->common) &&
                    reqFormat == AUDIO_FORMAT_PCM_16_BIT &&
                    (mInput->stream->common.get_sample_rate(&mInput->stream->common)
                            <= (2 * reqSamplingRate)) &&
                    popcount(mInput->stream->common.get_channels(&mInput->stream->common))
                            <= FCC_2 &&
                    (reqChannelMask == AUDIO_CHANNEL_IN_MONO ||
                            reqChannelMask == AUDIO_CHANNEL_IN_STEREO)) {
                    status = NO_ERROR;
                }
                if (status == NO_ERROR) {
                    readInputParameters();
                    sendIoConfigEvent_l(AudioSystem::INPUT_CONFIG_CHANGED);
                }
            }
        }
        mNewParameters.removeAt(0);
        mParamStatus = status;
        mParamCond.signal();
        mWaitWorkCV.waitRelative(mLock, kSetParametersTimeoutNs);
    }
    return reconfig;
}
String8 AudioFlinger::RecordThread::getParameters(const String8& keys)
{
    Mutex::Autolock _l(mLock);
    if (initCheck() != NO_ERROR) {
        return String8();
    }
    char *s = mInput->stream->common.get_parameters(&mInput->stream->common, keys.string());
    const String8 out_s8(s);
    free(s);
    return out_s8;
}
void AudioFlinger::RecordThread::audioConfigChanged_l(int event, int param __unused) {
    AudioSystem::OutputDescriptor desc;
    const void *param2 = NULL;
    switch (event) {
    case AudioSystem::INPUT_OPENED:
    case AudioSystem::INPUT_CONFIG_CHANGED:
        desc.channelMask = mChannelMask;
        desc.samplingRate = mSampleRate;
        desc.format = mFormat;
        desc.frameCount = mFrameCount;
        desc.latency = 0;
        param2 = &desc;
        break;
    case AudioSystem::INPUT_CLOSED:
    default:
        break;
    }
    mAudioFlinger->audioConfigChanged_l(event, mId, param2);
}
void AudioFlinger::RecordThread::readInputParameters()
{
    delete[] mRsmpInBuffer;
    delete[] mRsmpOutBuffer;
    mRsmpOutBuffer = NULL;
    delete mResampler;
    mResampler = NULL;
    mSampleRate = mInput->stream->common.get_sample_rate(&mInput->stream->common);
    mChannelMask = mInput->stream->common.get_channels(&mInput->stream->common);
    mChannelCount = popcount(mChannelMask);
    mFormat = mInput->stream->common.get_format(&mInput->stream->common);
    if (mFormat != AUDIO_FORMAT_PCM_16_BIT) {
        ALOGE("HAL format %#x not supported; must be AUDIO_FORMAT_PCM_16_BIT", mFormat);
    }
    mFrameSize = audio_stream_frame_size(&mInput->stream->common);
    mBufferSize = mInput->stream->common.get_buffer_size(&mInput->stream->common);
    mFrameCount = mBufferSize / mFrameSize;
    mRsmpInFrames = mFrameCount * 3;
    mRsmpInFramesP2 = roundup(mRsmpInFrames);
    mRsmpInBuffer = new int16_t[(mRsmpInFramesP2 + mFrameCount - 1) * mChannelCount];
    mRsmpInFront = 0;
    mRsmpInRear = 0;
    mRsmpInUnrel = 0;
    if (mSampleRate != mReqSampleRate && mChannelCount <= FCC_2 && mReqChannelCount <= FCC_2) {
        mResampler = AudioResampler::create(16, (int) mChannelCount, mReqSampleRate);
        mResampler->setSampleRate(mSampleRate);
        mResampler->setVolume(AudioMixer::UNITY_GAIN, AudioMixer::UNITY_GAIN);
        mRsmpOutBuffer = new int32_t[mFrameCount * FCC_2];
    }
    mRsmpInIndex = mFrameCount;
}
uint32_t AudioFlinger::RecordThread::getInputFramesLost()
{
    Mutex::Autolock _l(mLock);
    if (initCheck() != NO_ERROR) {
        return 0;
    }
    return mInput->stream->get_input_frames_lost(mInput->stream);
}
uint32_t AudioFlinger::RecordThread::hasAudioSession(int sessionId) const
{
    Mutex::Autolock _l(mLock);
    uint32_t result = 0;
    if (getEffectChain_l(sessionId) != 0) {
        result = EFFECT_SESSION;
    }
    for (size_t i = 0; i < mTracks.size(); ++i) {
        if (sessionId == mTracks[i]->sessionId()) {
            result |= TRACK_SESSION;
            break;
        }
    }
    return result;
}
KeyedVector<int, bool> AudioFlinger::RecordThread::sessionIds() const
{
    KeyedVector<int, bool> ids;
    Mutex::Autolock _l(mLock);
    for (size_t j = 0; j < mTracks.size(); ++j) {
        sp<RecordThread::RecordTrack> track = mTracks[j];
        int sessionId = track->sessionId();
        if (ids.indexOfKey(sessionId) < 0) {
            ids.add(sessionId, true);
        }
    }
    return ids;
}
AudioFlinger::AudioStreamIn* AudioFlinger::RecordThread::clearInput()
{
    Mutex::Autolock _l(mLock);
    AudioStreamIn *input = mInput;
    mInput = NULL;
    return input;
}
audio_stream_t* AudioFlinger::RecordThread::stream() const
{
    if (mInput == NULL) {
        return NULL;
    }
    return &mInput->stream->common;
}
status_t AudioFlinger::RecordThread::addEffectChain_l(const sp<EffectChain>& chain)
{
    if (mEffectChains.size() != 0) {
        return INVALID_OPERATION;
    }
    ALOGV("addEffectChain_l() %p on thread %p", chain.get(), this);
    chain->setInBuffer(NULL);
    chain->setOutBuffer(NULL);
    checkSuspendOnAddEffectChain_l(chain);
    mEffectChains.add(chain);
    return NO_ERROR;
}
size_t AudioFlinger::RecordThread::removeEffectChain_l(const sp<EffectChain>& chain)
{
    ALOGV("removeEffectChain_l() %p from thread %p", chain.get(), this);
    ALOGW_IF(mEffectChains.size() != 1,
            "removeEffectChain_l() %p invalid chain size %d on thread %p",
            chain.get(), mEffectChains.size(), this);
    if (mEffectChains.size() == 1) {
        mEffectChains.removeAt(0);
    }
    return 0;
}
}
