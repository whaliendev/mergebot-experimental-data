#define LOG_TAG "AudioTrack"
#include <sys/resource.h>
#include <audio_utils/primitives.h>
#include <binder/IPCThreadState.h>
#include <media/AudioTrack.h>
#include <utils/Log.h>
#include <private/media/AudioTrackShared.h>
#include <media/IAudioFlinger.h>
#define WAIT_PERIOD_MS 10
#define WAIT_STREAM_END_TIMEOUT_SEC 120
namespace android {
status_t AudioTrack::getMinFrameCount(
        size_t* frameCount,
        audio_stream_type_t streamType,
        uint32_t sampleRate)
{
    if (frameCount == NULL) {
        return BAD_VALUE;
    }
    uint32_t afSampleRate;
    status_t status;
    status = AudioSystem::getOutputSamplingRate(&afSampleRate, streamType);
    if (status != NO_ERROR) {
        ALOGE("Unable to query output sample rate for stream type %d; status %d",
                streamType, status);
        return status;
    }
    size_t afFrameCount;
    status = AudioSystem::getOutputFrameCount(&afFrameCount, streamType);
    if (status != NO_ERROR) {
        ALOGE("Unable to query output frame count for stream type %d; status %d",
                streamType, status);
        return status;
    }
    uint32_t afLatency;
    status = AudioSystem::getOutputLatency(&afLatency, streamType);
    if (status != NO_ERROR) {
        ALOGE("Unable to query output latency for stream type %d; status %d",
                streamType, status);
        return status;
    }
    uint32_t minBufCount = afLatency / ((1000 * afFrameCount) / afSampleRate);
    if (minBufCount < 2) {
        minBufCount = 2;
    }
    *frameCount = (sampleRate == 0) ? afFrameCount * minBufCount :
            afFrameCount * minBufCount * sampleRate / afSampleRate;
    if (*frameCount == 0) {
        ALOGE("AudioTrack::getMinFrameCount failed for streamType %d, sampleRate %d",
                streamType, sampleRate);
        return BAD_VALUE;
    }
    ALOGV("getMinFrameCount=%d: afFrameCount=%d, minBufCount=%d, afSampleRate=%d, afLatency=%d",
            *frameCount, afFrameCount, minBufCount, afSampleRate, afLatency);
    return NO_ERROR;
}
AudioTrack::AudioTrack()
    : mStatus(NO_INIT),
      mIsTimed(false),
      mPreviousPriority(ANDROID_PRIORITY_NORMAL),
      mPreviousSchedulingGroup(SP_DEFAULT)
{
}
AudioTrack::AudioTrack(
        audio_stream_type_t streamType,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        int frameCount,
        audio_output_flags_t flags,
        callback_t cbf,
        void* user,
        int notificationFrames,
        int sessionId,
        transfer_type transferType,
        const audio_offload_info_t *offloadInfo,
        int uid)
    : mStatus(NO_INIT),
      mIsTimed(false),
      mPreviousPriority(ANDROID_PRIORITY_NORMAL),
      mPreviousSchedulingGroup(SP_DEFAULT)
{
    mStatus = set(streamType, sampleRate, format, channelMask,
            frameCount, flags, cbf, user, notificationFrames,
            0 , false , sessionId, transferType,
            offloadInfo, uid);
}
AudioTrack::AudioTrack(
        audio_stream_type_t streamType,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        const sp<IMemory>& sharedBuffer,
        audio_output_flags_t flags,
        callback_t cbf,
        void* user,
        int notificationFrames,
        int sessionId,
        transfer_type transferType,
        const audio_offload_info_t *offloadInfo,
        int uid)
    : mStatus(NO_INIT),
      mIsTimed(false),
      mPreviousPriority(ANDROID_PRIORITY_NORMAL),
      mPreviousSchedulingGroup(SP_DEFAULT)
{
    mStatus = set(streamType, sampleRate, format, channelMask,
            0 , flags, cbf, user, notificationFrames,
            sharedBuffer, false , sessionId, transferType, offloadInfo, uid);
}
AudioTrack::~AudioTrack()
{
    if (mStatus == NO_ERROR) {
        stop();
        if (mAudioTrackThread != 0) {
            mProxy->interrupt();
            mAudioTrackThread->requestExit();
            mAudioTrackThread->requestExitAndWait();
            mAudioTrackThread.clear();
        }
        mAudioTrack->asBinder()->unlinkToDeath(mDeathNotifier, this);
        mAudioTrack.clear();
        IPCThreadState::self()->flushCommands();
        AudioSystem::releaseAudioSessionId(mSessionId);
    }
}
status_t AudioTrack::set(
        audio_stream_type_t streamType,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        int frameCountInt,
        audio_output_flags_t flags,
        callback_t cbf,
        void* user,
        int notificationFrames,
        const sp<IMemory>& sharedBuffer,
        bool threadCanCallJava,
        int sessionId,
        transfer_type transferType,
        const audio_offload_info_t *offloadInfo,
        int uid)
{
    switch (transferType) {
    case TRANSFER_DEFAULT:
        if (sharedBuffer != 0) {
            transferType = TRANSFER_SHARED;
        } else if (cbf == NULL || threadCanCallJava) {
            transferType = TRANSFER_SYNC;
        } else {
            transferType = TRANSFER_CALLBACK;
        }
        break;
    case TRANSFER_CALLBACK:
        if (cbf == NULL || sharedBuffer != 0) {
            ALOGE("Transfer type TRANSFER_CALLBACK but cbf == NULL || sharedBuffer != 0");
            return BAD_VALUE;
        }
        break;
    case TRANSFER_OBTAIN:
    case TRANSFER_SYNC:
        if (sharedBuffer != 0) {
            ALOGE("Transfer type TRANSFER_OBTAIN but sharedBuffer != 0");
            return BAD_VALUE;
        }
        break;
    case TRANSFER_SHARED:
        if (sharedBuffer == 0) {
            ALOGE("Transfer type TRANSFER_SHARED but sharedBuffer == 0");
            return BAD_VALUE;
        }
        break;
    default:
        ALOGE("Invalid transfer type %d", transferType);
        return BAD_VALUE;
    }
    mSharedBuffer = sharedBuffer;
    mTransfer = transferType;
    if (frameCountInt < 0) {
        ALOGE("Invalid frame count %d", frameCountInt);
        return BAD_VALUE;
    }
    size_t frameCount = frameCountInt;
    ALOGV_IF(sharedBuffer != 0, "sharedBuffer: %p, size: %d", sharedBuffer->pointer(),
            sharedBuffer->size());
    ALOGV("set() streamType %d frameCount %u flags %04x", streamType, frameCount, flags);
    AutoMutex lock(mLock);
    if (mAudioTrack != 0) {
        ALOGE("Track already in use");
        return INVALID_OPERATION;
    }
    if (streamType == AUDIO_STREAM_DEFAULT) {
        streamType = AUDIO_STREAM_MUSIC;
    }
    if (uint32_t(streamType) >= AUDIO_STREAM_CNT) {
        ALOGE("Invalid stream type %d", streamType);
        return BAD_VALUE;
    }
    mStreamType = streamType;
    status_t status;
    if (sampleRate == 0) {
        status = AudioSystem::getOutputSamplingRate(&sampleRate, streamType);
        if (status != NO_ERROR) {
            ALOGE("Could not get output sample rate for stream type %d; status %d",
                    streamType, status);
            return status;
        }
    }
    mSampleRate = sampleRate;
    if (format == AUDIO_FORMAT_DEFAULT) {
        format = AUDIO_FORMAT_PCM_16_BIT;
    }
    if (!audio_is_valid_format(format)) {
        ALOGE("Invalid format %#x", format);
        return BAD_VALUE;
    }
    mFormat = format;
    if (!audio_is_output_channel(channelMask)) {
        ALOGE("Invalid channel mask %#x", channelMask);
        return BAD_VALUE;
    }
    if (format == AUDIO_FORMAT_PCM_8_BIT && sharedBuffer != 0) {
        ALOGE("8-bit data in shared memory is not supported");
        return BAD_VALUE;
    }
    if ((flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
            || !audio_is_linear_pcm(format)) {
        ALOGV( (flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
                    ? "Offload request, forcing to Direct Output"
                    : "Not linear PCM, forcing to Direct Output");
        flags = (audio_output_flags_t)
                ((flags | AUDIO_OUTPUT_FLAG_DIRECT) & ~AUDIO_OUTPUT_FLAG_FAST);
    }
    if (streamType != AUDIO_STREAM_MUSIC) {
        flags = (audio_output_flags_t)(flags &~AUDIO_OUTPUT_FLAG_DEEP_BUFFER);
    }
    mChannelMask = channelMask;
    uint32_t channelCount = popcount(channelMask);
    mChannelCount = channelCount;
    if (audio_is_linear_pcm(format)) {
        mFrameSize = channelCount * audio_bytes_per_sample(format);
        mFrameSizeAF = channelCount * sizeof(int16_t);
    } else {
        mFrameSize = sizeof(uint8_t);
        mFrameSizeAF = sizeof(uint8_t);
    }
    if (offloadInfo != NULL) {
        mOffloadInfoCopy = *offloadInfo;
        mOffloadInfo = &mOffloadInfoCopy;
    } else {
        mOffloadInfo = NULL;
    }
    mVolume[LEFT] = 1.0f;
    mVolume[RIGHT] = 1.0f;
    mSendLevel = 0.0f;
    mReqFrameCount = frameCount;
    mNotificationFramesReq = notificationFrames;
    mNotificationFramesAct = 0;
    mSessionId = sessionId;
    if (uid == -1 || (IPCThreadState::self()->getCallingPid() != getpid())) {
        mClientUid = IPCThreadState::self()->getCallingUid();
    } else {
        mClientUid = uid;
    }
    mAuxEffectId = 0;
    mFlags = flags;
    mCbf = cbf;
    if (cbf != NULL) {
        mAudioTrackThread = new AudioTrackThread(*this, threadCanCallJava);
        mAudioTrackThread->run("AudioTrack", ANDROID_PRIORITY_AUDIO, 0 );
    }
    status = createTrack_l(0 );
    if (status != NO_ERROR) {
        if (mAudioTrackThread != 0) {
            mAudioTrackThread->requestExit();
            mAudioTrackThread->requestExitAndWait();
            mAudioTrackThread.clear();
        }
#if 0
        if (mOutput != 0) {
            AudioSystem::releaseOutput(mOutput);
            mOutput = 0;
        }
#endif
        return status;
    }
    mStatus = NO_ERROR;
    mState = STATE_STOPPED;
    mUserData = user;
    mLoopPeriod = 0;
    mMarkerPosition = 0;
    mMarkerReached = false;
    mNewPosition = 0;
    mUpdatePeriod = 0;
    AudioSystem::acquireAudioSessionId(mSessionId);
    mSequence = 1;
    mObservedSequence = mSequence;
    mInUnderrun = false;
    return NO_ERROR;
}
status_t AudioTrack::start()
{
    AutoMutex lock(mLock);
    if (mState == STATE_ACTIVE) {
        return INVALID_OPERATION;
    }
    mInUnderrun = true;
    State previousState = mState;
    if (previousState == STATE_PAUSED_STOPPING) {
        mState = STATE_STOPPING;
    } else {
        mState = STATE_ACTIVE;
    }
    if (previousState == STATE_STOPPED || previousState == STATE_FLUSHED) {
        mProxy->setEpoch(mProxy->getEpoch() - mProxy->getPosition());
        mRefreshRemaining = true;
    }
    mNewPosition = mProxy->getPosition() + mUpdatePeriod;
    int32_t flags = android_atomic_and(~CBLK_DISABLED, &mCblk->mFlags);
    sp<AudioTrackThread> t = mAudioTrackThread;
    if (t != 0) {
        if (previousState == STATE_STOPPING) {
            mProxy->interrupt();
        } else {
            t->resume();
        }
    } else {
        mPreviousPriority = getpriority(PRIO_PROCESS, 0);
        get_sched_policy(0, &mPreviousSchedulingGroup);
        androidSetThreadPriority(0, ANDROID_PRIORITY_AUDIO);
    }
    status_t status = NO_ERROR;
    if (!(flags & CBLK_INVALID)) {
        status = mAudioTrack->start();
        if (status == DEAD_OBJECT) {
            flags |= CBLK_INVALID;
        }
    }
    if (flags & CBLK_INVALID) {
        status = restoreTrack_l("start");
    }
    if (status != NO_ERROR) {
        ALOGE("start() status %d", status);
        mState = previousState;
        if (t != 0) {
            if (previousState != STATE_STOPPING) {
                t->pause();
            }
        } else {
            setpriority(PRIO_PROCESS, 0, mPreviousPriority);
            set_sched_policy(0, mPreviousSchedulingGroup);
        }
    }
    return status;
}
void AudioTrack::stop()
{
    AutoMutex lock(mLock);
    if (mState != STATE_ACTIVE && mState != STATE_PAUSED) {
        return;
    }
    if (isOffloaded_l()) {
        mState = STATE_STOPPING;
    } else {
        mState = STATE_STOPPED;
    }
    mProxy->interrupt();
    mAudioTrack->stop();
    mMarkerReached = false;
#if 0
    if (mSharedBuffer != 0) {
        flush_l();
    }
#endif
    sp<AudioTrackThread> t = mAudioTrackThread;
    if (t != 0) {
        if (!isOffloaded_l()) {
            t->pause();
        }
    } else {
        setpriority(PRIO_PROCESS, 0, mPreviousPriority);
        set_sched_policy(0, mPreviousSchedulingGroup);
    }
}
bool AudioTrack::stopped() const
{
    AutoMutex lock(mLock);
    return mState != STATE_ACTIVE;
}
void AudioTrack::flush()
{
    if (mSharedBuffer != 0) {
        return;
    }
    AutoMutex lock(mLock);
    if (mState == STATE_ACTIVE || mState == STATE_FLUSHED) {
        return;
    }
    flush_l();
}
void AudioTrack::flush_l()
{
    ALOG_ASSERT(mState != STATE_ACTIVE);
    mMarkerPosition = 0;
    mMarkerReached = false;
    mUpdatePeriod = 0;
    mRefreshRemaining = true;
    mState = STATE_FLUSHED;
    if (isOffloaded_l()) {
        mProxy->interrupt();
    }
    mProxy->flush();
    mAudioTrack->flush();
}
void AudioTrack::pause()
{
    AutoMutex lock(mLock);
    if (mState == STATE_ACTIVE) {
        mState = STATE_PAUSED;
    } else if (mState == STATE_STOPPING) {
        mState = STATE_PAUSED_STOPPING;
    } else {
        return;
    }
    mProxy->interrupt();
    mAudioTrack->pause();
}
status_t AudioTrack::setVolume(float left, float right)
{
    if (left < 0.0f || left > 1.0f || right < 0.0f || right > 1.0f) {
        return BAD_VALUE;
    }
    AutoMutex lock(mLock);
    mVolume[LEFT] = left;
    mVolume[RIGHT] = right;
    mProxy->setVolumeLR((uint32_t(uint16_t(right * 0x1000)) << 16) | uint16_t(left * 0x1000));
    if (isOffloaded_l()) {
        mAudioTrack->signal();
    }
    return NO_ERROR;
}
status_t AudioTrack::setVolume(float volume)
{
    return setVolume(volume, volume);
}
status_t AudioTrack::setAuxEffectSendLevel(float level)
{
    if (level < 0.0f || level > 1.0f) {
        return BAD_VALUE;
    }
    AutoMutex lock(mLock);
    mSendLevel = level;
    mProxy->setSendLevel(level);
    return NO_ERROR;
}
void AudioTrack::getAuxEffectSendLevel(float* level) const
{
    if (level != NULL) {
        *level = mSendLevel;
    }
}
status_t AudioTrack::setSampleRate(uint32_t rate)
{
    if (mIsTimed || isOffloaded()) {
        return INVALID_OPERATION;
    }
    uint32_t afSamplingRate;
    if (AudioSystem::getOutputSamplingRate(&afSamplingRate, mStreamType) != NO_ERROR) {
        return NO_INIT;
    }
    if (rate == 0 || rate > afSamplingRate*2 ) {
        return BAD_VALUE;
    }
    AutoMutex lock(mLock);
    mSampleRate = rate;
    mProxy->setSampleRate(rate);
    return NO_ERROR;
}
uint32_t AudioTrack::getSampleRate() const
{
    if (mIsTimed) {
        return 0;
    }
    AutoMutex lock(mLock);
    if (isOffloaded_l()) {
        if (mOutput != 0) {
            uint32_t sampleRate = 0;
            status_t status = AudioSystem::getSamplingRate(mOutput, mStreamType, &sampleRate);
            if (status == NO_ERROR) {
                mSampleRate = sampleRate;
            }
        }
    }
    return mSampleRate;
}
status_t AudioTrack::setLoop(uint32_t loopStart, uint32_t loopEnd, int loopCount)
{
    if (mSharedBuffer == 0 || mIsTimed || isOffloaded()) {
        return INVALID_OPERATION;
    }
    if (loopCount == 0) {
        ;
    } else if (loopCount >= -1 && loopStart < loopEnd && loopEnd <= mFrameCount &&
            loopEnd - loopStart >= MIN_LOOP) {
        ;
    } else {
        return BAD_VALUE;
    }
    AutoMutex lock(mLock);
    if (mState == STATE_ACTIVE) {
        return INVALID_OPERATION;
    }
    setLoop_l(loopStart, loopEnd, loopCount);
    return NO_ERROR;
}
void AudioTrack::setLoop_l(uint32_t loopStart, uint32_t loopEnd, int loopCount)
{
    mNewPosition = mProxy->getPosition() + mUpdatePeriod;
    mLoopPeriod = loopCount != 0 ? loopEnd - loopStart : 0;
    mStaticProxy->setLoop(loopStart, loopEnd, loopCount);
}
status_t AudioTrack::setMarkerPosition(uint32_t marker)
{
    if (mCbf == NULL || isOffloaded()) {
        return INVALID_OPERATION;
    }
    AutoMutex lock(mLock);
    mMarkerPosition = marker;
    mMarkerReached = false;
    return NO_ERROR;
}
status_t AudioTrack::getMarkerPosition(uint32_t *marker) const
{
    if (isOffloaded()) {
        return INVALID_OPERATION;
    }
    if (marker == NULL) {
        return BAD_VALUE;
    }
    AutoMutex lock(mLock);
    *marker = mMarkerPosition;
    return NO_ERROR;
}
status_t AudioTrack::setPositionUpdatePeriod(uint32_t updatePeriod)
{
    if (mCbf == NULL || isOffloaded()) {
        return INVALID_OPERATION;
    }
    AutoMutex lock(mLock);
    mNewPosition = mProxy->getPosition() + updatePeriod;
    mUpdatePeriod = updatePeriod;
    return NO_ERROR;
}
status_t AudioTrack::getPositionUpdatePeriod(uint32_t *updatePeriod) const
{
    if (isOffloaded()) {
        return INVALID_OPERATION;
    }
    if (updatePeriod == NULL) {
        return BAD_VALUE;
    }
    AutoMutex lock(mLock);
    *updatePeriod = mUpdatePeriod;
    return NO_ERROR;
}
status_t AudioTrack::setPosition(uint32_t position)
{
    if (mSharedBuffer == 0 || mIsTimed || isOffloaded()) {
        return INVALID_OPERATION;
    }
    if (position > mFrameCount) {
        return BAD_VALUE;
    }
    AutoMutex lock(mLock);
    if (mState == STATE_ACTIVE) {
        return INVALID_OPERATION;
    }
    mNewPosition = mProxy->getPosition() + mUpdatePeriod;
    mLoopPeriod = 0;
    mStaticProxy->setLoop(position, mFrameCount, 0);
    return NO_ERROR;
}
status_t AudioTrack::getPosition(uint32_t *position) const
{
    if (position == NULL) {
        return BAD_VALUE;
    }
    AutoMutex lock(mLock);
    if (isOffloaded_l()) {
        uint32_t dspFrames = 0;
        if (mOutput != 0) {
            uint32_t halFrames;
            AudioSystem::getRenderPosition(mOutput, &halFrames, &dspFrames);
        }
        *position = dspFrames;
    } else {
        *position = (mState == STATE_STOPPED || mState == STATE_FLUSHED) ? 0 :
                mProxy->getPosition();
    }
    return NO_ERROR;
}
status_t AudioTrack::getBufferPosition(uint32_t *position)
{
    if (mSharedBuffer == 0 || mIsTimed) {
        return INVALID_OPERATION;
    }
    if (position == NULL) {
        return BAD_VALUE;
    }
    AutoMutex lock(mLock);
    *position = mStaticProxy->getBufferPosition();
    return NO_ERROR;
}
status_t AudioTrack::reload()
{
    if (mSharedBuffer == 0 || mIsTimed || isOffloaded()) {
        return INVALID_OPERATION;
    }
    AutoMutex lock(mLock);
    if (mState == STATE_ACTIVE) {
        return INVALID_OPERATION;
    }
    mNewPosition = mUpdatePeriod;
    mLoopPeriod = 0;
    mStaticProxy->setLoop(0, mFrameCount, 0);
    return NO_ERROR;
}
audio_io_handle_t AudioTrack::getOutput() const
{
    AutoMutex lock(mLock);
    return mOutput;
}
status_t AudioTrack::attachAuxEffect(int effectId)
{
    AutoMutex lock(mLock);
    status_t status = mAudioTrack->attachAuxEffect(effectId);
    if (status == NO_ERROR) {
        mAuxEffectId = effectId;
    }
    return status;
}
status_t AudioTrack::createTrack_l(size_t epoch)
{
    status_t status;
    const sp<IAudioFlinger>& audioFlinger = AudioSystem::get_audio_flinger();
    if (audioFlinger == 0) {
        ALOGE("Could not get audioflinger");
        return NO_INIT;
    }
    audio_io_handle_t output = AudioSystem::getOutput(mStreamType, mSampleRate, mFormat,
            mChannelMask, mFlags, mOffloadInfo);
    if (output == 0) {
        ALOGE("Could not get audio output for stream type %d, sample rate %u, format %#x, "
              "channel mask %#x, flags %#x",
              mStreamType, mSampleRate, mFormat, mChannelMask, mFlags);
        return BAD_VALUE;
    }
    {
    uint32_t afLatency;
    status = AudioSystem::getLatency(output, mStreamType, &afLatency);
    if (status != NO_ERROR) {
        ALOGE("getLatency(%d) failed status %d", output, status);
        goto release;
    }
    size_t afFrameCount;
    status = AudioSystem::getFrameCount(output, mStreamType, &afFrameCount);
    if (status != NO_ERROR) {
        ALOGE("getFrameCount(output=%d, streamType=%d) status %d", output, mStreamType, status);
        goto release;
    }
    uint32_t afSampleRate;
    status = AudioSystem::getSamplingRate(output, mStreamType, &afSampleRate);
    if (status != NO_ERROR) {
        ALOGE("getSamplingRate(output=%d, streamType=%d) status %d", output, mStreamType, status);
        goto release;
    }
    if ((mFlags & AUDIO_OUTPUT_FLAG_FAST) && !((
            (mSharedBuffer != 0) ||
            (mCbf != NULL)) &&
            (mSampleRate == afSampleRate))) {
        ALOGW("AUDIO_OUTPUT_FLAG_FAST denied by client");
        mFlags = (audio_output_flags_t) (mFlags & ~AUDIO_OUTPUT_FLAG_FAST);
    }
    ALOGV("createTrack_l() output %d afLatency %d", output, afLatency);
    const uint32_t nBuffering = (mSampleRate == afSampleRate) ? 2 : 3;
    mNotificationFramesAct = mNotificationFramesReq;
    size_t frameCount = mReqFrameCount;
    if (!audio_is_linear_pcm(mFormat)) {
        if (mSharedBuffer != 0) {
            frameCount = mSharedBuffer->size();
        } else if (frameCount == 0) {
            frameCount = afFrameCount;
        }
        if (mNotificationFramesAct != frameCount) {
            mNotificationFramesAct = frameCount;
        }
    } else if (mSharedBuffer != 0) {
        size_t alignment = 2;
        if (mChannelCount > 1) {
            alignment <<= 1;
        }
<<<<<<< HEAD
        if (((size_t)mSharedBuffer->pointer() & (alignment - 1)) != 0) {
||||||| 66e05682e4
        if (((size_t)sharedBuffer->pointer() & (alignment - 1)) != 0) {
=======
        if (((uintptr_t)sharedBuffer->pointer() & (alignment - 1)) != 0) {
>>>>>>> 566be7c3
            ALOGE("Invalid buffer alignment: address %p, channel count %u",
                    mSharedBuffer->pointer(), mChannelCount);
            status = BAD_VALUE;
            goto release;
        }
        frameCount = mSharedBuffer->size()/mChannelCount/sizeof(int16_t);
    } else if (!(mFlags & AUDIO_OUTPUT_FLAG_FAST)) {
        uint32_t minBufCount = afLatency / ((1000 * afFrameCount)/afSampleRate);
        ALOGV("afFrameCount=%d, minBufCount=%d, afSampleRate=%u, afLatency=%d",
                afFrameCount, minBufCount, afSampleRate, afLatency);
        if (minBufCount <= nBuffering) {
            minBufCount = nBuffering;
        }
        size_t minFrameCount = (afFrameCount*mSampleRate*minBufCount)/afSampleRate;
        ALOGV("minFrameCount: %u, afFrameCount=%d, minBufCount=%d, sampleRate=%u, afSampleRate=%u"
                ", afLatency=%d",
                minFrameCount, afFrameCount, minBufCount, mSampleRate, afSampleRate, afLatency);
        if (frameCount == 0) {
            frameCount = minFrameCount;
        } else if (frameCount < minFrameCount) {
            ALOGV("Minimum buffer size corrected from %d to %d",
                     frameCount, minFrameCount);
            frameCount = minFrameCount;
        }
        if (mNotificationFramesAct == 0 || mNotificationFramesAct > frameCount/nBuffering) {
            mNotificationFramesAct = frameCount/nBuffering;
        }
    } else {
    }
    IAudioFlinger::track_flags_t trackFlags = IAudioFlinger::TRACK_DEFAULT;
    if (mIsTimed) {
        trackFlags |= IAudioFlinger::TRACK_TIMED;
    }
    pid_t tid = -1;
    if (mFlags & AUDIO_OUTPUT_FLAG_FAST) {
        trackFlags |= IAudioFlinger::TRACK_FAST;
        if (mAudioTrackThread != 0) {
            tid = mAudioTrackThread->getTid();
        }
    }
    if (mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) {
        trackFlags |= IAudioFlinger::TRACK_OFFLOAD;
    }
    size_t temp = frameCount;
    sp<IAudioTrack> track = audioFlinger->createTrack(mStreamType,
                                                      mSampleRate,
                                                      mFormat == AUDIO_FORMAT_PCM_8_BIT ?
                                                              AUDIO_FORMAT_PCM_16_BIT : mFormat,
                                                      mChannelMask,
                                                      &temp,
                                                      &trackFlags,
                                                      mSharedBuffer,
                                                      output,
                                                      tid,
                                                      &mSessionId,
                                                      mName,
                                                      mClientUid,
                                                      &status);
    if (track == 0) {
        ALOGE("AudioFlinger could not create track, status: %d", status);
        goto release;
    }
    sp<IMemory> iMem = track->getCblk();
    if (iMem == 0) {
        ALOGE("Could not get control block");
        return NO_INIT;
    }
    void *iMemPointer = iMem->pointer();
    if (iMemPointer == NULL) {
        ALOGE("Could not get control block pointer");
        return NO_INIT;
    }
    if (mAudioTrack != 0) {
        mAudioTrack->asBinder()->unlinkToDeath(mDeathNotifier, this);
        mDeathNotifier.clear();
    }
    mAudioTrack = track;
    mCblkMemory = iMem;
    audio_track_cblk_t* cblk = static_cast<audio_track_cblk_t*>(iMemPointer);
    mCblk = cblk;
    if (temp < frameCount || (frameCount == 0 && temp == 0)) {
        ALOGW("Requested frameCount %u but received frameCount %u", frameCount, temp);
    }
    frameCount = temp;
    mAwaitBoost = false;
    if (mFlags & AUDIO_OUTPUT_FLAG_FAST) {
        if (trackFlags & IAudioFlinger::TRACK_FAST) {
            ALOGV("AUDIO_OUTPUT_FLAG_FAST successful; frameCount %u", frameCount);
            mAwaitBoost = true;
            if (mSharedBuffer == 0) {
                if (mNotificationFramesAct == 0 || mNotificationFramesAct > frameCount/nBuffering) {
                    mNotificationFramesAct = frameCount/nBuffering;
                }
            }
        } else {
            ALOGV("AUDIO_OUTPUT_FLAG_FAST denied by server; frameCount %u", frameCount);
            mFlags = (audio_output_flags_t) (mFlags & ~AUDIO_OUTPUT_FLAG_FAST);
            if (mSharedBuffer == 0) {
                if (mNotificationFramesAct == 0 || mNotificationFramesAct > frameCount/nBuffering) {
                    mNotificationFramesAct = frameCount/nBuffering;
                }
            }
        }
    }
    if (mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) {
        if (trackFlags & IAudioFlinger::TRACK_OFFLOAD) {
            ALOGV("AUDIO_OUTPUT_FLAG_OFFLOAD successful");
        } else {
            ALOGW("AUDIO_OUTPUT_FLAG_OFFLOAD denied by server");
            mFlags = (audio_output_flags_t) (mFlags & ~AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD);
        }
    }
    mOutput = output;
    mRefreshRemaining = true;
    void* buffers;
    if (mSharedBuffer == 0) {
        buffers = (char*)cblk + sizeof(audio_track_cblk_t);
    } else {
        buffers = mSharedBuffer->pointer();
    }
    mAudioTrack->attachAuxEffect(mAuxEffectId);
    mLatency = afLatency + (1000*frameCount) / mSampleRate;
    mFrameCount = frameCount;
    if (frameCount > mReqFrameCount) {
        mReqFrameCount = frameCount;
    }
    if (mSharedBuffer == 0) {
        mStaticProxy.clear();
        mProxy = new AudioTrackClientProxy(cblk, buffers, frameCount, mFrameSizeAF);
    } else {
        mStaticProxy = new StaticAudioTrackClientProxy(cblk, buffers, frameCount, mFrameSizeAF);
        mProxy = mStaticProxy;
    }
    mProxy->setVolumeLR((uint32_t(uint16_t(mVolume[RIGHT] * 0x1000)) << 16) |
            uint16_t(mVolume[LEFT] * 0x1000));
    mProxy->setSendLevel(mSendLevel);
    mProxy->setSampleRate(mSampleRate);
    mProxy->setEpoch(epoch);
    mProxy->setMinimum(mNotificationFramesAct);
    mDeathNotifier = new DeathNotifier(this);
    mAudioTrack->asBinder()->linkToDeath(mDeathNotifier, this);
    return NO_ERROR;
    }
release:
    AudioSystem::releaseOutput(output);
    if (status == NO_ERROR) {
        status = NO_INIT;
    }
    return status;
}
status_t AudioTrack::obtainBuffer(Buffer* audioBuffer, int32_t waitCount)
{
    if (audioBuffer == NULL) {
        return BAD_VALUE;
    }
    if (mTransfer != TRANSFER_OBTAIN) {
        audioBuffer->frameCount = 0;
        audioBuffer->size = 0;
        audioBuffer->raw = NULL;
        return INVALID_OPERATION;
    }
    const struct timespec *requested;
    struct timespec timeout;
    if (waitCount == -1) {
        requested = &ClientProxy::kForever;
    } else if (waitCount == 0) {
        requested = &ClientProxy::kNonBlocking;
    } else if (waitCount > 0) {
        long long ms = WAIT_PERIOD_MS * (long long) waitCount;
        timeout.tv_sec = ms / 1000;
        timeout.tv_nsec = (int) (ms % 1000) * 1000000;
        requested = &timeout;
    } else {
        ALOGE("%s invalid waitCount %d", __func__, waitCount);
        requested = NULL;
    }
    return obtainBuffer(audioBuffer, requested);
}
status_t AudioTrack::obtainBuffer(Buffer* audioBuffer, const struct timespec *requested,
        struct timespec *elapsed, size_t *nonContig)
{
    uint32_t oldSequence = 0;
    uint32_t newSequence;
    Proxy::Buffer buffer;
    status_t status = NO_ERROR;
    static const int32_t kMaxTries = 5;
    int32_t tryCounter = kMaxTries;
    do {
        sp<AudioTrackClientProxy> proxy;
        sp<IMemory> iMem;
        {
            AutoMutex lock(mLock);
            newSequence = mSequence;
            if (status == DEAD_OBJECT) {
                if (newSequence == oldSequence) {
                    status = restoreTrack_l("obtainBuffer");
                    if (status != NO_ERROR) {
                        buffer.mFrameCount = 0;
                        buffer.mRaw = NULL;
                        buffer.mNonContig = 0;
                        break;
                    }
                }
            }
            oldSequence = newSequence;
            proxy = mProxy;
            iMem = mCblkMemory;
            if (mState == STATE_STOPPING) {
                status = -EINTR;
                buffer.mFrameCount = 0;
                buffer.mRaw = NULL;
                buffer.mNonContig = 0;
                break;
            }
            if (mState != STATE_ACTIVE) {
                requested = &ClientProxy::kNonBlocking;
            }
        }
        buffer.mFrameCount = audioBuffer->frameCount;
        status = proxy->obtainBuffer(&buffer, requested, elapsed);
    } while ((status == DEAD_OBJECT) && (tryCounter-- > 0));
    audioBuffer->frameCount = buffer.mFrameCount;
    audioBuffer->size = buffer.mFrameCount * mFrameSizeAF;
    audioBuffer->raw = buffer.mRaw;
    if (nonContig != NULL) {
        *nonContig = buffer.mNonContig;
    }
    return status;
}
void AudioTrack::releaseBuffer(Buffer* audioBuffer)
{
    if (mTransfer == TRANSFER_SHARED) {
        return;
    }
    size_t stepCount = audioBuffer->size / mFrameSizeAF;
    if (stepCount == 0) {
        return;
    }
    Proxy::Buffer buffer;
    buffer.mFrameCount = stepCount;
    buffer.mRaw = audioBuffer->raw;
    AutoMutex lock(mLock);
    mInUnderrun = false;
    mProxy->releaseBuffer(&buffer);
    if (mState == STATE_ACTIVE) {
        audio_track_cblk_t* cblk = mCblk;
        if (android_atomic_and(~CBLK_DISABLED, &cblk->mFlags) & CBLK_DISABLED) {
            ALOGW("releaseBuffer() track %p name=%s disabled due to previous underrun, restarting",
                    this, mName.string());
            mAudioTrack->start();
        }
    }
}
ssize_t AudioTrack::write(const void* buffer, size_t userSize)
{
    if (mTransfer != TRANSFER_SYNC || mIsTimed) {
        return INVALID_OPERATION;
    }
    if (ssize_t(userSize) < 0 || (buffer == NULL && userSize != 0)) {
        ALOGE("AudioTrack::write(buffer=%p, size=%zu (%zd)", buffer, userSize, userSize);
        return BAD_VALUE;
    }
    size_t written = 0;
    Buffer audioBuffer;
    while (userSize >= mFrameSize) {
        audioBuffer.frameCount = userSize / mFrameSize;
        status_t err = obtainBuffer(&audioBuffer, &ClientProxy::kForever);
        if (err < 0) {
            if (written > 0) {
                break;
            }
            return ssize_t(err);
        }
        size_t toWrite;
        if (mFormat == AUDIO_FORMAT_PCM_8_BIT && !(mFlags & AUDIO_OUTPUT_FLAG_DIRECT)) {
            toWrite = audioBuffer.size >> 1;
            memcpy_to_i16_from_u8(audioBuffer.i16, (const uint8_t *) buffer, toWrite);
        } else {
            toWrite = audioBuffer.size;
            memcpy(audioBuffer.i8, buffer, toWrite);
        }
        buffer = ((const char *) buffer) + toWrite;
        userSize -= toWrite;
        written += toWrite;
        releaseBuffer(&audioBuffer);
    }
    return written;
}
TimedAudioTrack::TimedAudioTrack() {
    mIsTimed = true;
}
status_t TimedAudioTrack::allocateTimedBuffer(size_t size, sp<IMemory>* buffer)
{
    AutoMutex lock(mLock);
    status_t result = UNKNOWN_ERROR;
#if 1
    sp<IAudioTrack> audioTrack = mAudioTrack;
    sp<IMemory> iMem = mCblkMemory;
#endif
    audio_track_cblk_t* cblk = mCblk;
    if (!(cblk->mFlags & CBLK_INVALID)) {
        result = mAudioTrack->allocateTimedBuffer(size, buffer);
        if (result == DEAD_OBJECT) {
            android_atomic_or(CBLK_INVALID, &cblk->mFlags);
        }
    }
    if (cblk->mFlags & CBLK_INVALID) {
        result = restoreTrack_l("allocateTimedBuffer");
        if (result == NO_ERROR) {
            result = mAudioTrack->allocateTimedBuffer(size, buffer);
        }
    }
    return result;
}
status_t TimedAudioTrack::queueTimedBuffer(const sp<IMemory>& buffer,
                                           int64_t pts)
{
    status_t status = mAudioTrack->queueTimedBuffer(buffer, pts);
    {
        AutoMutex lock(mLock);
        audio_track_cblk_t* cblk = mCblk;
        if (buffer->size() != 0 && status == NO_ERROR &&
                (mState == STATE_ACTIVE) && (cblk->mFlags & CBLK_DISABLED)) {
            android_atomic_and(~CBLK_DISABLED, &cblk->mFlags);
            ALOGW("queueTimedBuffer() track %p disabled, restarting", this);
            mAudioTrack->start();
        }
    }
    return status;
}
status_t TimedAudioTrack::setMediaTimeTransform(const LinearTransform& xform,
                                                TargetTimeline target)
{
    return mAudioTrack->setMediaTimeTransform(xform, target);
}
nsecs_t AudioTrack::processAudioBuffer()
{
    LOG_ALWAYS_FATAL_IF(mCblk == NULL);
    mLock.lock();
    if (mAwaitBoost) {
        mAwaitBoost = false;
        mLock.unlock();
        static const int32_t kMaxTries = 5;
        int32_t tryCounter = kMaxTries;
        uint32_t pollUs = 10000;
        do {
            int policy = sched_getscheduler(0);
            if (policy == SCHED_FIFO || policy == SCHED_RR) {
                break;
            }
            usleep(pollUs);
            pollUs <<= 1;
        } while (tryCounter-- > 0);
        if (tryCounter < 0) {
            ALOGE("did not receive expected priority boost on time");
        }
        return 0;
    }
    int32_t flags = android_atomic_and(
        ~(CBLK_UNDERRUN | CBLK_LOOP_CYCLE | CBLK_LOOP_FINAL | CBLK_BUFFER_END), &mCblk->mFlags);
    if (flags & CBLK_INVALID) {
        if (!isOffloaded_l() || (mSequence == mObservedSequence)) {
            status_t status = restoreTrack_l("processAudioBuffer");
            mLock.unlock();
            return 0;
        }
    }
    bool waitStreamEnd = mState == STATE_STOPPING;
    bool active = mState == STATE_ACTIVE;
    bool newUnderrun = false;
    if (flags & CBLK_UNDERRUN) {
#if 0
        if (mTransfer == TRANSFER_SHARED) {
            mState = STATE_PAUSED;
            active = false;
        }
#endif
        if (!mInUnderrun) {
            mInUnderrun = true;
            newUnderrun = true;
        }
    }
    size_t position = mProxy->getPosition();
    bool markerReached = false;
    size_t markerPosition = mMarkerPosition;
    if (!mMarkerReached && (markerPosition > 0) && (position >= markerPosition)) {
        mMarkerReached = markerReached = true;
    }
    size_t newPosCount = 0;
    size_t newPosition = mNewPosition;
    size_t updatePeriod = mUpdatePeriod;
    if (updatePeriod > 0 && position >= newPosition) {
        newPosCount = ((position - newPosition) / updatePeriod) + 1;
        mNewPosition += updatePeriod * newPosCount;
    }
    uint32_t loopPeriod = mLoopPeriod;
    uint32_t sampleRate = mSampleRate;
    size_t notificationFrames = mNotificationFramesAct;
    if (mRefreshRemaining) {
        mRefreshRemaining = false;
        mRemainingFrames = notificationFrames;
        mRetryOnPartialBuffer = false;
    }
    size_t misalignment = mProxy->getMisalignment();
    uint32_t sequence = mSequence;
    mLock.unlock();
    if (waitStreamEnd) {
        AutoMutex lock(mLock);
        sp<AudioTrackClientProxy> proxy = mProxy;
        sp<IMemory> iMem = mCblkMemory;
        struct timespec timeout;
        timeout.tv_sec = WAIT_STREAM_END_TIMEOUT_SEC;
        timeout.tv_nsec = 0;
        mLock.unlock();
        status_t status = mProxy->waitStreamEndDone(&timeout);
        mLock.lock();
        switch (status) {
        case NO_ERROR:
        case DEAD_OBJECT:
        case TIMED_OUT:
            mLock.unlock();
            mCbf(EVENT_STREAM_END, mUserData, NULL);
            mLock.lock();
            if (mState == STATE_STOPPING) {
                mState = STATE_STOPPED;
                if (status != DEAD_OBJECT) {
                   return NS_INACTIVE;
                }
            }
            return 0;
        default:
            return 0;
        }
    }
    if (newUnderrun) {
        mCbf(EVENT_UNDERRUN, mUserData, NULL);
    }
    if (flags & (CBLK_LOOP_CYCLE | CBLK_LOOP_FINAL)) {
        mCbf(EVENT_LOOP_END, mUserData, NULL);
    }
    if (flags & CBLK_BUFFER_END) {
        mCbf(EVENT_BUFFER_END, mUserData, NULL);
    }
    if (markerReached) {
        mCbf(EVENT_MARKER, mUserData, &markerPosition);
    }
    while (newPosCount > 0) {
        size_t temp = newPosition;
        mCbf(EVENT_NEW_POS, mUserData, &temp);
        newPosition += updatePeriod;
        newPosCount--;
    }
    if (mObservedSequence != sequence) {
        mObservedSequence = sequence;
        mCbf(EVENT_NEW_IAUDIOTRACK, mUserData, NULL);
        if (isOffloaded()) {
            return NS_INACTIVE;
        }
    }
    if (!active) {
        return NS_INACTIVE;
    }
    uint32_t minFrames = ~0;
    if (!markerReached && position < markerPosition) {
        minFrames = markerPosition - position;
    }
    if (loopPeriod > 0 && loopPeriod < minFrames) {
        minFrames = loopPeriod;
    }
    if (updatePeriod > 0 && updatePeriod < minFrames) {
        minFrames = updatePeriod;
    }
    static const uint32_t kPoll = 0;
    if (kPoll > 0 && mTransfer == TRANSFER_CALLBACK && kPoll * notificationFrames < minFrames) {
        minFrames = kPoll * notificationFrames;
    }
    nsecs_t ns = NS_WHENEVER;
    if (minFrames != (uint32_t) ~0) {
        static const nsecs_t kFudgeNs = 10000000LL;
        ns = ((minFrames * 1000000000LL) / sampleRate) + kFudgeNs;
    }
    if (mTransfer != TRANSFER_CALLBACK) {
        return ns;
    }
    struct timespec timeout;
    const struct timespec *requested = &ClientProxy::kForever;
    if (ns != NS_WHENEVER) {
        timeout.tv_sec = ns / 1000000000LL;
        timeout.tv_nsec = ns % 1000000000LL;
        ALOGV("timeout %ld.%03d", timeout.tv_sec, (int) timeout.tv_nsec / 1000000);
        requested = &timeout;
    }
    while (mRemainingFrames > 0) {
        Buffer audioBuffer;
        audioBuffer.frameCount = mRemainingFrames;
        size_t nonContig;
        status_t err = obtainBuffer(&audioBuffer, requested, NULL, &nonContig);
        LOG_ALWAYS_FATAL_IF((err != NO_ERROR) != (audioBuffer.frameCount == 0),
                "obtainBuffer() err=%d frameCount=%u", err, audioBuffer.frameCount);
        requested = &ClientProxy::kNonBlocking;
        size_t avail = audioBuffer.frameCount + nonContig;
        ALOGV("obtainBuffer(%u) returned %u = %u + %u err %d",
                mRemainingFrames, avail, audioBuffer.frameCount, nonContig, err);
        if (err != NO_ERROR) {
            if (err == TIMED_OUT || err == WOULD_BLOCK || err == -EINTR ||
                    (isOffloaded() && (err == DEAD_OBJECT))) {
                return 0;
            }
            ALOGE("Error %d obtaining an audio buffer, giving up.", err);
            return NS_NEVER;
        }
        if (mRetryOnPartialBuffer && !isOffloaded()) {
            mRetryOnPartialBuffer = false;
            if (avail < mRemainingFrames) {
                int64_t myns = ((mRemainingFrames - avail) * 1100000000LL) / sampleRate;
                if (ns < 0 || myns < ns) {
                    ns = myns;
                }
                return ns;
            }
        }
        if (mFormat == AUDIO_FORMAT_PCM_8_BIT && !(mFlags & AUDIO_OUTPUT_FLAG_DIRECT)) {
            audioBuffer.size >>= 1;
        }
        size_t reqSize = audioBuffer.size;
        mCbf(EVENT_MORE_DATA, mUserData, &audioBuffer);
        size_t writtenSize = audioBuffer.size;
        if (ssize_t(writtenSize) < 0 || writtenSize > reqSize) {
            ALOGE("EVENT_MORE_DATA requested %u bytes but callback returned %d bytes",
                    reqSize, (int) writtenSize);
            return NS_NEVER;
        }
        if (writtenSize == 0) {
            return WAIT_PERIOD_MS * 1000000LL;
        }
        if (mFormat == AUDIO_FORMAT_PCM_8_BIT && !(mFlags & AUDIO_OUTPUT_FLAG_DIRECT)) {
            memcpy_to_i16_from_u8(audioBuffer.i16, (const uint8_t *) audioBuffer.i8, writtenSize);
            audioBuffer.size <<= 1;
        }
        size_t releasedFrames = audioBuffer.size / mFrameSizeAF;
        audioBuffer.frameCount = releasedFrames;
        mRemainingFrames -= releasedFrames;
        if (misalignment >= releasedFrames) {
            misalignment -= releasedFrames;
        } else {
            misalignment = 0;
        }
        releaseBuffer(&audioBuffer);
        if (writtenSize < reqSize) {
            continue;
        }
        if (mRemainingFrames <= nonContig) {
            continue;
        }
#if 0
        if (0 < misalignment && misalignment <= mRemainingFrames) {
            mRemainingFrames = misalignment;
            return (mRemainingFrames * 1100000000LL) / sampleRate;
        }
#endif
    }
    mRemainingFrames = notificationFrames;
    mRetryOnPartialBuffer = true;
    return 0;
}
status_t AudioTrack::restoreTrack_l(const char *from)
{
    ALOGW("dead IAudioTrack, %s, creating a new one from %s()",
          isOffloaded_l() ? "Offloaded" : "PCM", from);
    ++mSequence;
    status_t result;
    AudioSystem::clearAudioConfigCache();
    if (isOffloaded_l()) {
        return DEAD_OBJECT;
    }
    size_t position = mProxy->getPosition() + mProxy->getFramesFilled();
    size_t bufferPosition = mStaticProxy != NULL ? mStaticProxy->getBufferPosition() : 0;
    result = createTrack_l(position );
    if (result == NO_ERROR) {
        if (mStaticProxy != NULL) {
            mLoopPeriod = 0;
            mStaticProxy->setLoop(bufferPosition, mFrameCount, 0);
        }
#if 0
        if (!strcmp(from, "start")) {
            if (mSharedBuffer == 0) {
                android_atomic_or(CBLK_FORCEREADY, &mCblk->mFlags);
            }
        }
#endif
        if (mState == STATE_ACTIVE) {
            result = mAudioTrack->start();
        }
    }
    if (result != NO_ERROR) {
#if 0
        if (mOutput != 0) {
            AudioSystem::releaseOutput(mOutput);
            mOutput = 0;
        }
#endif
        ALOGW("restoreTrack_l() failed status %d", result);
        mState = STATE_STOPPED;
    }
    return result;
}
status_t AudioTrack::setParameters(const String8& keyValuePairs)
{
    AutoMutex lock(mLock);
    return mAudioTrack->setParameters(keyValuePairs);
}
status_t AudioTrack::getTimestamp(AudioTimestamp& timestamp)
{
    AutoMutex lock(mLock);
    if (mFlags & AUDIO_OUTPUT_FLAG_FAST) {
        return INVALID_OPERATION;
    }
    if (mState != STATE_ACTIVE && mState != STATE_PAUSED) {
        return INVALID_OPERATION;
    }
    status_t status = mAudioTrack->getTimestamp(timestamp);
    if (status == NO_ERROR) {
        timestamp.mPosition += mProxy->getEpoch();
    }
    return status;
}
String8 AudioTrack::getParameters(const String8& keys)
{
    audio_io_handle_t output = getOutput();
    if (output != 0) {
        return AudioSystem::getParameters(output, keys);
    } else {
        return String8::empty();
    }
}
bool AudioTrack::isOffloaded() const
{
    AutoMutex lock(mLock);
    return isOffloaded_l();
}
status_t AudioTrack::dump(int fd, const Vector<String16>& args __unused) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    result.append(" AudioTrack::dump\n");
    snprintf(buffer, 255, "  stream type(%d), left - right volume(%f, %f)\n", mStreamType,
            mVolume[0], mVolume[1]);
    result.append(buffer);
    snprintf(buffer, 255, "  format(%d), channel count(%d), frame count(%zu)\n", mFormat,
            mChannelCount, mFrameCount);
    result.append(buffer);
    snprintf(buffer, 255, "  sample rate(%u), status(%d)\n", mSampleRate, mStatus);
    result.append(buffer);
    snprintf(buffer, 255, "  state(%d), latency (%d)\n", mState, mLatency);
    result.append(buffer);
    ::write(fd, result.string(), result.size());
    return NO_ERROR;
}
uint32_t AudioTrack::getUnderrunFrames() const
{
    AutoMutex lock(mLock);
    return mProxy->getUnderrunFrames();
}
void AudioTrack::DeathNotifier::binderDied(const wp<IBinder>& who __unused)
{
    sp<AudioTrack> audioTrack = mAudioTrack.promote();
    if (audioTrack != 0) {
        AutoMutex lock(audioTrack->mLock);
        audioTrack->mProxy->binderDied();
    }
}
AudioTrack::AudioTrackThread::AudioTrackThread(AudioTrack& receiver, bool bCanCallJava)
    : Thread(bCanCallJava), mReceiver(receiver), mPaused(true), mPausedInt(false), mPausedNs(0LL),
      mIgnoreNextPausedInt(false)
{
}
AudioTrack::AudioTrackThread::~AudioTrackThread()
{
}
bool AudioTrack::AudioTrackThread::threadLoop()
{
    {
        AutoMutex _l(mMyLock);
        if (mPaused) {
            mMyCond.wait(mMyLock);
            return true;
        }
        if (mIgnoreNextPausedInt) {
            mIgnoreNextPausedInt = false;
            mPausedInt = false;
        }
        if (mPausedInt) {
            if (mPausedNs > 0) {
                (void) mMyCond.waitRelative(mMyLock, mPausedNs);
            } else {
                mMyCond.wait(mMyLock);
            }
            mPausedInt = false;
            return true;
        }
    }
    nsecs_t ns = mReceiver.processAudioBuffer();
    switch (ns) {
    case 0:
        return true;
    case NS_INACTIVE:
        pauseInternal();
        return true;
    case NS_NEVER:
        return false;
    case NS_WHENEVER:
        ns = 1000000000LL;
    default:
        LOG_ALWAYS_FATAL_IF(ns < 0, "processAudioBuffer() returned %lld", ns);
        pauseInternal(ns);
        return true;
    }
}
void AudioTrack::AudioTrackThread::requestExit()
{
    Thread::requestExit();
    resume();
}
void AudioTrack::AudioTrackThread::pause()
{
    AutoMutex _l(mMyLock);
    mPaused = true;
}
void AudioTrack::AudioTrackThread::resume()
{
    AutoMutex _l(mMyLock);
    mIgnoreNextPausedInt = true;
    if (mPaused || mPausedInt) {
        mPaused = false;
        mPausedInt = false;
        mMyCond.signal();
    }
}
void AudioTrack::AudioTrackThread::pauseInternal(nsecs_t ns)
{
    AutoMutex _l(mMyLock);
    mPausedInt = true;
    mPausedNs = ns;
}
};
