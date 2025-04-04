#define LOG_TAG "AudioFlinger"
#include "Configuration.h"
#include <math.h>
#include <utils/Log.h>
#include <private/media/AudioTrackShared.h>
#include <common_time/cc_helper.h>
#include <common_time/local_clock.h>
#include "AudioMixer.h"
#include "AudioFlinger.h"
#include "ServiceUtilities.h"
#include <media/nbaio/Pipe.h>
#include <media/nbaio/PipeReader.h>
#ifdef VERY_VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif
namespace android {
static volatile int32_t nextTrackId = 55;
AudioFlinger::ThreadBase::TrackBase::TrackBase(
            ThreadBase *thread,
            const sp<Client>& client,
            uint32_t sampleRate,
            audio_format_t format,
            audio_channel_mask_t channelMask,
            size_t frameCount,
            const sp<IMemory>& sharedBuffer,
            int sessionId,
            int clientUid,
            bool isOut)
    : RefBase(),
        mThread(thread),
        mClient(client),
        mCblk(NULL),
        mState(IDLE),
        mSampleRate(sampleRate),
        mFormat(format),
        mChannelMask(channelMask),
        mChannelCount(popcount(channelMask)),
        mFrameSize(audio_is_linear_pcm(format) ?
                mChannelCount * audio_bytes_per_sample(format) : sizeof(int8_t)),
        mFrameCount(frameCount),
        mSessionId(sessionId),
        mIsOut(isOut),
        mServerProxy(NULL),
        mId(android_atomic_inc(&nextTrackId)),
        mTerminated(false)
{
    if (IPCThreadState::self()->getCallingPid() != getpid_cached || clientUid == -1) {
        int newclientUid = IPCThreadState::self()->getCallingUid();
        if (clientUid != -1 && clientUid != newclientUid) {
            ALOGW("uid %d tried to pass itself off as %d", newclientUid, clientUid);
        }
        clientUid = newclientUid;
    }
    mUid = clientUid;
    ALOG_ASSERT(!(client == 0 && sharedBuffer != 0));
    ALOGV_IF(sharedBuffer != 0, "sharedBuffer: %p, size: %d", sharedBuffer->pointer(),
            sharedBuffer->size());
    size_t size = sizeof(audio_track_cblk_t);
    size_t bufferSize = (sharedBuffer == 0 ? roundup(frameCount) : frameCount) * mFrameSize;
    if (sharedBuffer == 0) {
        size += bufferSize;
    }
    if (client != 0) {
        mCblkMemory = client->heap()->allocate(size);
        if (mCblkMemory == 0 ||
                (mCblk = static_cast<audio_track_cblk_t *>(mCblkMemory->pointer())) == NULL) {
            ALOGE("not enough memory for AudioTrack size=%u", size);
            client->heap()->dump("AudioTrack");
            mCblkMemory.clear();
            return;
        }
    } else {
        mCblk = (audio_track_cblk_t *) new uint8_t[size];
    }
    if (mCblk != NULL) {
        new(mCblk) audio_track_cblk_t();
        if (sharedBuffer == 0) {
            mBuffer = (char*)mCblk + sizeof(audio_track_cblk_t);
            memset(mBuffer, 0, bufferSize);
        } else {
            mBuffer = sharedBuffer->pointer();
#if 0
            mCblk->mFlags = CBLK_FORCEREADY;
#endif
        }
#ifdef TEE_SINK
        if (mTeeSinkTrackEnabled) {
            NBAIO_Format pipeFormat = Format_from_SR_C(mSampleRate, mChannelCount);
            if (Format_isValid(pipeFormat)) {
                Pipe *pipe = new Pipe(mTeeSinkTrackFrames, pipeFormat);
                size_t numCounterOffers = 0;
                const NBAIO_Format offers[1] = {pipeFormat};
                ssize_t index = pipe->negotiate(offers, 1, NULL, numCounterOffers);
                ALOG_ASSERT(index == 0);
                PipeReader *pipeReader = new PipeReader(*pipe);
                numCounterOffers = 0;
                index = pipeReader->negotiate(offers, 1, NULL, numCounterOffers);
                ALOG_ASSERT(index == 0);
                mTeeSink = pipe;
                mTeeSource = pipeReader;
            }
        }
#endif
    }
}
AudioFlinger::ThreadBase::TrackBase::~TrackBase()
{
#ifdef TEE_SINK
    dumpTee(-1, mTeeSource, mId);
#endif
    delete mServerProxy;
    if (mCblk != NULL) {
        if (mClient == 0) {
            delete mCblk;
        } else {
            mCblk->~audio_track_cblk_t();
        }
    }
    mCblkMemory.clear();
    if (mClient != 0) {
        Mutex::Autolock _l(mClient->audioFlinger()->mLock);
        mClient.clear();
    }
}
void AudioFlinger::ThreadBase::TrackBase::releaseBuffer(AudioBufferProvider::Buffer* buffer)
{
#ifdef TEE_SINK
    if (mTeeSink != 0) {
        (void) mTeeSink->write(buffer->raw, buffer->frameCount);
    }
#endif
    ServerProxy::Buffer buf;
    buf.mFrameCount = buffer->frameCount;
    buf.mRaw = buffer->raw;
    buffer->frameCount = 0;
    buffer->raw = NULL;
    mServerProxy->releaseBuffer(&buf);
}
status_t AudioFlinger::ThreadBase::TrackBase::setSyncEvent(const sp<SyncEvent>& event)
{
    mSyncEvents.add(event);
    return NO_ERROR;
}
AudioFlinger::TrackHandle::TrackHandle(const sp<AudioFlinger::PlaybackThread::Track>& track)
    : BnAudioTrack(),
      mTrack(track)
{
}
AudioFlinger::TrackHandle::~TrackHandle() {
    mTrack->destroy();
}
sp<IMemory> AudioFlinger::TrackHandle::getCblk() const {
    return mTrack->getCblk();
}
status_t AudioFlinger::TrackHandle::start() {
    return mTrack->start();
}
void AudioFlinger::TrackHandle::stop() {
    mTrack->stop();
}
void AudioFlinger::TrackHandle::flush() {
    mTrack->flush();
}
void AudioFlinger::TrackHandle::pause() {
    mTrack->pause();
}
status_t AudioFlinger::TrackHandle::attachAuxEffect(int EffectId)
{
    return mTrack->attachAuxEffect(EffectId);
}
status_t AudioFlinger::TrackHandle::allocateTimedBuffer(size_t size,
                                                         sp<IMemory>* buffer) {
    if (!mTrack->isTimedTrack())
        return INVALID_OPERATION;
    PlaybackThread::TimedTrack* tt =
            reinterpret_cast<PlaybackThread::TimedTrack*>(mTrack.get());
    return tt->allocateTimedBuffer(size, buffer);
}
status_t AudioFlinger::TrackHandle::queueTimedBuffer(const sp<IMemory>& buffer,
                                                     int64_t pts) {
    if (!mTrack->isTimedTrack())
        return INVALID_OPERATION;
    if (buffer == 0 || buffer->pointer() == NULL) {
        ALOGE("queueTimedBuffer() buffer is 0 or has NULL pointer()");
        return BAD_VALUE;
    }
    PlaybackThread::TimedTrack* tt =
            reinterpret_cast<PlaybackThread::TimedTrack*>(mTrack.get());
    return tt->queueTimedBuffer(buffer, pts);
}
status_t AudioFlinger::TrackHandle::setMediaTimeTransform(
    const LinearTransform& xform, int target) {
    if (!mTrack->isTimedTrack())
        return INVALID_OPERATION;
    PlaybackThread::TimedTrack* tt =
            reinterpret_cast<PlaybackThread::TimedTrack*>(mTrack.get());
    return tt->setMediaTimeTransform(
        xform, static_cast<TimedAudioTrack::TargetTimeline>(target));
}
status_t AudioFlinger::TrackHandle::setParameters(const String8& keyValuePairs) {
    return mTrack->setParameters(keyValuePairs);
}
status_t AudioFlinger::TrackHandle::getTimestamp(AudioTimestamp& timestamp)
{
    return mTrack->getTimestamp(timestamp);
}
void AudioFlinger::TrackHandle::signal()
{
    return mTrack->signal();
}
status_t AudioFlinger::TrackHandle::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    return BnAudioTrack::onTransact(code, data, reply, flags);
}
AudioFlinger::PlaybackThread::Track::Track(
            PlaybackThread *thread,
            const sp<Client>& client,
            audio_stream_type_t streamType,
            uint32_t sampleRate,
            audio_format_t format,
            audio_channel_mask_t channelMask,
            size_t frameCount,
            const sp<IMemory>& sharedBuffer,
            int sessionId,
            int uid,
            IAudioFlinger::track_flags_t flags)
    : TrackBase(thread, client, sampleRate, format, channelMask, frameCount, sharedBuffer,
            sessionId, uid, true ),
    mFillingUpStatus(FS_INVALID),
    mSharedBuffer(sharedBuffer),
    mStreamType(streamType),
    mName(-1),
    mMainBuffer(thread->mixBuffer()),
    mAuxBuffer(NULL),
    mAuxEffectId(0), mHasVolumeController(false),
    mPresentationCompleteFrames(0),
    mFlags(flags),
    mFastIndex(-1),
    mCachedVolume(1.0),
    mIsInvalid(false),
    mAudioTrackServerProxy(NULL),
    mResumeToStopping(false),
    mFlushHwPending(false)
{
    if (mCblk != NULL) {
        if (sharedBuffer == 0) {
            mAudioTrackServerProxy = new AudioTrackServerProxy(mCblk, mBuffer, frameCount,
                    mFrameSize);
        } else {
            mAudioTrackServerProxy = new StaticAudioTrackServerProxy(mCblk, mBuffer, frameCount,
                    mFrameSize);
        }
        mServerProxy = mAudioTrackServerProxy;
        mName = thread->getTrackName_l(channelMask, sessionId);
        if (mName < 0) {
            ALOGE("no more track names available");
            return;
        }
        if (flags & IAudioFlinger::TRACK_FAST) {
            mAudioTrackServerProxy->framesReadyIsCalledByMultipleThreads();
            ALOG_ASSERT(thread->mFastTrackAvailMask != 0);
            int i = __builtin_ctz(thread->mFastTrackAvailMask);
            ALOG_ASSERT(0 < i && i < (int)FastMixerState::kMaxFastTracks);
            mFastIndex = i;
            mObservedUnderruns = thread->getFastTrackUnderruns(i);
            thread->mFastTrackAvailMask &= ~(1 << i);
        }
    }
    ALOGV("Track constructor name %d, calling pid %d", mName,
            IPCThreadState::self()->getCallingPid());
}
AudioFlinger::PlaybackThread::Track::~Track()
{
    ALOGV("PlaybackThread::Track destructor");
    if (mSharedBuffer != 0) {
        mSharedBuffer.clear();
        IPCThreadState::self()->flushCommands();
    }
}
status_t AudioFlinger::PlaybackThread::Track::initCheck() const
{
    status_t status = TrackBase::initCheck();
    if (status == NO_ERROR && mName < 0) {
        status = NO_MEMORY;
    }
    return status;
}
void AudioFlinger::PlaybackThread::Track::destroy()
{
    sp<Track> keep(this);
    {
        sp<ThreadBase> thread = mThread.promote();
        if (thread != 0) {
            Mutex::Autolock _l(thread->mLock);
            PlaybackThread *playbackThread = (PlaybackThread *)thread.get();
            bool wasActive = playbackThread->destroyTrack_l(this);
            if (!isOutputTrack() && !wasActive) {
                AudioSystem::releaseOutput(thread->id());
            }
        }
    }
}
           void AudioFlinger::PlaybackThread::Track::appendDumpHeader(String8& result)
{
    result.append("    Name Active Client Type      Fmt Chn mask Session fCount S F SRate  "
                  "L dB  R dB    Server Main buf  Aux Buf Flags UndFrmCnt\n");
}
void AudioFlinger::PlaybackThread::Track::dump(char* buffer, size_t size, bool active)
{
    uint32_t vlr = mAudioTrackServerProxy->getVolumeLR();
    if (isFastTrack()) {
        sprintf(buffer, "    F %2d", mFastIndex);
    } else if (mName >= AudioMixer::TRACK0) {
        sprintf(buffer, "    %4d", mName - AudioMixer::TRACK0);
    } else {
        sprintf(buffer, "    none");
    }
    track_state state = mState;
    char stateChar;
    if (isTerminated()) {
        stateChar = 'T';
    } else {
        switch (state) {
        case IDLE:
            stateChar = 'I';
            break;
        case STOPPING_1:
            stateChar = 's';
            break;
        case STOPPING_2:
            stateChar = '5';
            break;
        case STOPPED:
            stateChar = 'S';
            break;
        case RESUMING:
            stateChar = 'R';
            break;
        case ACTIVE:
            stateChar = 'A';
            break;
        case PAUSING:
            stateChar = 'p';
            break;
        case PAUSED:
            stateChar = 'P';
            break;
        case FLUSHED:
            stateChar = 'F';
            break;
        default:
            stateChar = '?';
            break;
        }
    }
    char nowInUnderrun;
    switch (mObservedUnderruns.mBitFields.mMostRecent) {
    case UNDERRUN_FULL:
        nowInUnderrun = ' ';
        break;
    case UNDERRUN_PARTIAL:
        nowInUnderrun = '<';
        break;
    case UNDERRUN_EMPTY:
        nowInUnderrun = '*';
        break;
    default:
        nowInUnderrun = '?';
        break;
    }
    snprintf(&buffer[8], size-8, " %6s %6u %4u %08X %08X %7u %6zu %1c %1d %5u %5.2g %5.2g  "
                                 "%08X %p %p 0x%03X %9u%c\n",
            active ? "yes" : "no",
            (mClient == 0) ? getpid_cached : mClient->pid(),
            mStreamType,
            mFormat,
            mChannelMask,
            mSessionId,
            mFrameCount,
            stateChar,
            mFillingUpStatus,
            mAudioTrackServerProxy->getSampleRate(),
            20.0 * log10((vlr & 0xFFFF) / 4096.0),
            20.0 * log10((vlr >> 16) / 4096.0),
            mCblk->mServer,
            mMainBuffer,
            mAuxBuffer,
            mCblk->mFlags,
            mAudioTrackServerProxy->getUnderrunFrames(),
            nowInUnderrun);
}
uint32_t AudioFlinger::PlaybackThread::Track::sampleRate() const {
    return mAudioTrackServerProxy->getSampleRate();
}
status_t AudioFlinger::PlaybackThread::Track::getNextBuffer(
        AudioBufferProvider::Buffer* buffer, int64_t pts __unused)
{
    ServerProxy::Buffer buf;
    size_t desiredFrames = buffer->frameCount;
    buf.mFrameCount = desiredFrames;
    status_t status = mServerProxy->obtainBuffer(&buf);
    buffer->frameCount = buf.mFrameCount;
    buffer->raw = buf.mRaw;
    if (buf.mFrameCount == 0) {
        mAudioTrackServerProxy->tallyUnderrunFrames(desiredFrames);
    }
    return status;
}
size_t AudioFlinger::PlaybackThread::Track::framesReady() const {
    return mAudioTrackServerProxy->framesReady();
}
size_t AudioFlinger::PlaybackThread::Track::framesReleased() const
{
    return mAudioTrackServerProxy->framesReleased();
}
bool AudioFlinger::PlaybackThread::Track::isReady() const {
    if (mFillingUpStatus != FS_FILLING || isStopped() || isPausing() || isStopping()) {
        return true;
    }
    if (framesReady() >= mFrameCount ||
            (mCblk->mFlags & CBLK_FORCEREADY)) {
        mFillingUpStatus = FS_FILLED;
        android_atomic_and(~CBLK_FORCEREADY, &mCblk->mFlags);
        return true;
    }
    return false;
}
status_t AudioFlinger::PlaybackThread::Track::start(AudioSystem::sync_event_t event __unused,
                                                    int triggerSession __unused)
{
    status_t status = NO_ERROR;
    ALOGV("start(%d), calling pid %d session %d",
            mName, IPCThreadState::self()->getCallingPid(), mSessionId);
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        if (isOffloaded()) {
            Mutex::Autolock _laf(thread->mAudioFlinger->mLock);
            Mutex::Autolock _lth(thread->mLock);
            sp<EffectChain> ec = thread->getEffectChain_l(mSessionId);
            if (thread->mAudioFlinger->isNonOffloadableGlobalEffectEnabled_l() ||
                    (ec != 0 && ec->isNonOffloadableEnabled())) {
                invalidate();
                return PERMISSION_DENIED;
            }
        }
        Mutex::Autolock _lth(thread->mLock);
        track_state state = mState;
        if (state == PAUSED) {
            if (mResumeToStopping) {
                mState = TrackBase::STOPPING_1;
                ALOGV("PAUSED => STOPPING_1 (%d) on thread %p", mName, this);
            } else {
                mState = TrackBase::RESUMING;
                ALOGV("PAUSED => RESUMING (%d) on thread %p", mName, this);
            }
        } else {
            mState = TrackBase::ACTIVE;
            ALOGV("? => ACTIVE (%d) on thread %p", mName, this);
        }
        PlaybackThread *playbackThread = (PlaybackThread *)thread.get();
        status = playbackThread->addTrack_l(this);
        if (status == INVALID_OPERATION || status == PERMISSION_DENIED) {
            triggerEvents(AudioSystem::SYNC_EVENT_PRESENTATION_COMPLETE);
            if (status == PERMISSION_DENIED) {
                mState = state;
            }
        }
        if (status == ALREADY_EXISTS) {
            status = NO_ERROR;
        } else {
            ServerProxy::Buffer buffer;
            buffer.mFrameCount = 1;
            (void) mAudioTrackServerProxy->obtainBuffer(&buffer, true );
        }
    } else {
        status = BAD_VALUE;
    }
    return status;
}
void AudioFlinger::PlaybackThread::Track::stop()
{
    ALOGV("stop(%d), calling pid %d", mName, IPCThreadState::self()->getCallingPid());
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        Mutex::Autolock _l(thread->mLock);
        track_state state = mState;
        if (state == RESUMING || state == ACTIVE || state == PAUSING || state == PAUSED) {
            PlaybackThread *playbackThread = (PlaybackThread *)thread.get();
            if (playbackThread->mActiveTracks.indexOf(this) < 0) {
                reset();
                mState = STOPPED;
            } else if (!isFastTrack() && !isOffloaded()) {
                mState = STOPPED;
            } else {
                mState = STOPPING_1;
            }
            ALOGV("not stopping/stopped => stopping/stopped (%d) on thread %p", mName,
                    playbackThread);
        }
    }
}
void AudioFlinger::PlaybackThread::Track::pause()
{
    ALOGV("pause(%d), calling pid %d", mName, IPCThreadState::self()->getCallingPid());
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        Mutex::Autolock _l(thread->mLock);
        PlaybackThread *playbackThread = (PlaybackThread *)thread.get();
        switch (mState) {
        case STOPPING_1:
        case STOPPING_2:
            if (!isOffloaded()) {
                break;
            }
            mResumeToStopping = true;
        case ACTIVE:
        case RESUMING:
            mState = PAUSING;
            ALOGV("ACTIVE/RESUMING => PAUSING (%d) on thread %p", mName, thread.get());
            playbackThread->broadcast_l();
            break;
        default:
            break;
        }
    }
}
void AudioFlinger::PlaybackThread::Track::flush()
{
    ALOGV("flush(%d)", mName);
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        Mutex::Autolock _l(thread->mLock);
        PlaybackThread *playbackThread = (PlaybackThread *)thread.get();
        if (isOffloaded()) {
            if (isTerminated()) {
                return;
            }
            ALOGV("flush: offload flush");
            reset();
            if (mState == STOPPING_1 || mState == STOPPING_2) {
                ALOGV("flushed in STOPPING_1 or 2 state, change state to ACTIVE");
                mState = ACTIVE;
            }
            if (mState == ACTIVE) {
                ALOGV("flush called in active state, resetting buffer time out retry count");
                mRetryCount = PlaybackThread::kMaxTrackRetriesOffload;
            }
            mFlushHwPending = true;
            mResumeToStopping = false;
        } else {
            if (mState != STOPPING_1 && mState != STOPPING_2 && mState != STOPPED &&
                    mState != PAUSED && mState != PAUSING && mState != IDLE && mState != FLUSHED) {
                return;
            }
            mState = FLUSHED;
            if (playbackThread->mActiveTracks.indexOf(this) < 0) {
                reset();
            }
        }
        playbackThread->broadcast_l();
    }
}
void AudioFlinger::PlaybackThread::Track::flushAck()
{
    if (!isOffloaded())
        return;
    mFlushHwPending = false;
}
void AudioFlinger::PlaybackThread::Track::reset()
{
    if (!mResetDone) {
        android_atomic_and(~CBLK_FORCEREADY, &mCblk->mFlags);
        mFillingUpStatus = FS_FILLING;
        mResetDone = true;
        if (mState == FLUSHED) {
            mState = IDLE;
        }
    }
}
status_t AudioFlinger::PlaybackThread::Track::setParameters(const String8& keyValuePairs)
{
    sp<ThreadBase> thread = mThread.promote();
    if (thread == 0) {
        ALOGE("thread is dead");
        return FAILED_TRANSACTION;
    } else if ((thread->type() == ThreadBase::DIRECT) ||
                    (thread->type() == ThreadBase::OFFLOAD)) {
        return thread->setParameters(keyValuePairs);
    } else {
        return PERMISSION_DENIED;
    }
}
status_t AudioFlinger::PlaybackThread::Track::getTimestamp(AudioTimestamp& timestamp)
{
    if (isFastTrack()) {
        return INVALID_OPERATION;
    }
    sp<ThreadBase> thread = mThread.promote();
    if (thread == 0) {
        return INVALID_OPERATION;
    }
    Mutex::Autolock _l(thread->mLock);
    PlaybackThread *playbackThread = (PlaybackThread *)thread.get();
    if (!isOffloaded()) {
        if (!playbackThread->mLatchQValid) {
            return INVALID_OPERATION;
        }
        uint32_t unpresentedFrames =
                ((int64_t) playbackThread->mLatchQ.mUnpresentedFrames * mSampleRate) /
                playbackThread->mSampleRate;
        uint32_t framesWritten = mAudioTrackServerProxy->framesReleased();
        if (framesWritten < unpresentedFrames) {
            return INVALID_OPERATION;
        }
        timestamp.mPosition = framesWritten - unpresentedFrames;
        timestamp.mTime = playbackThread->mLatchQ.mTimestamp.mTime;
        return NO_ERROR;
    }
    return playbackThread->getTimestamp_l(timestamp);
}
status_t AudioFlinger::PlaybackThread::Track::attachAuxEffect(int EffectId)
{
    status_t status = DEAD_OBJECT;
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        PlaybackThread *playbackThread = (PlaybackThread *)thread.get();
        sp<AudioFlinger> af = mClient->audioFlinger();
        Mutex::Autolock _l(af->mLock);
        sp<PlaybackThread> srcThread = af->getEffectThread_l(AUDIO_SESSION_OUTPUT_MIX, EffectId);
        if (EffectId != 0 && srcThread != 0 && playbackThread != srcThread.get()) {
            Mutex::Autolock _dl(playbackThread->mLock);
            Mutex::Autolock _sl(srcThread->mLock);
            sp<EffectChain> chain = srcThread->getEffectChain_l(AUDIO_SESSION_OUTPUT_MIX);
            if (chain == 0) {
                return INVALID_OPERATION;
            }
            sp<EffectModule> effect = chain->getEffectFromId_l(EffectId);
            if (effect == 0) {
                return INVALID_OPERATION;
            }
            srcThread->removeEffect_l(effect);
            status = playbackThread->addEffect_l(effect);
            if (status != NO_ERROR) {
                srcThread->addEffect_l(effect);
                return INVALID_OPERATION;
            }
            if (effect->state() == EffectModule::ACTIVE ||
                    effect->state() == EffectModule::STOPPING) {
                effect->start();
            }
            sp<EffectChain> dstChain = effect->chain().promote();
            if (dstChain == 0) {
                srcThread->addEffect_l(effect);
                return INVALID_OPERATION;
            }
            AudioSystem::unregisterEffect(effect->id());
            AudioSystem::registerEffect(&effect->desc(),
                                        srcThread->id(),
                                        dstChain->strategy(),
                                        AUDIO_SESSION_OUTPUT_MIX,
                                        effect->id());
            AudioSystem::setEffectEnabled(effect->id(), effect->isEnabled());
        }
        status = playbackThread->attachAuxEffect(this, EffectId);
    }
    return status;
}
void AudioFlinger::PlaybackThread::Track::setAuxBuffer(int EffectId, int32_t *buffer)
{
    mAuxEffectId = EffectId;
    mAuxBuffer = buffer;
}
bool AudioFlinger::PlaybackThread::Track::presentationComplete(size_t framesWritten,
                                                         size_t audioHalFrames)
{
    ALOGV("presentationComplete() mPresentationCompleteFrames %d framesWritten %d",
                      mPresentationCompleteFrames, framesWritten);
    if (mPresentationCompleteFrames == 0) {
        mPresentationCompleteFrames = framesWritten + audioHalFrames;
        ALOGV("presentationComplete() reset: mPresentationCompleteFrames %d audioHalFrames %d",
                  mPresentationCompleteFrames, audioHalFrames);
    }
    if (framesWritten >= mPresentationCompleteFrames || isOffloaded()) {
        ALOGV("presentationComplete() session %d complete: framesWritten %d",
                  mSessionId, framesWritten);
        triggerEvents(AudioSystem::SYNC_EVENT_PRESENTATION_COMPLETE);
        mAudioTrackServerProxy->setStreamEndDone();
        return true;
    }
    return false;
}
void AudioFlinger::PlaybackThread::Track::triggerEvents(AudioSystem::sync_event_t type)
{
    for (int i = 0; i < (int)mSyncEvents.size(); i++) {
        if (mSyncEvents[i]->type() == type) {
            mSyncEvents[i]->trigger();
            mSyncEvents.removeAt(i);
            i--;
        }
    }
}
uint32_t AudioFlinger::PlaybackThread::Track::getVolumeLR()
{
    ALOG_ASSERT(isFastTrack() && (mCblk != NULL));
    uint32_t vlr = mAudioTrackServerProxy->getVolumeLR();
    uint32_t vl = vlr & 0xFFFF;
    uint32_t vr = vlr >> 16;
    if (vl > MAX_GAIN_INT) {
        vl = MAX_GAIN_INT;
    }
    if (vr > MAX_GAIN_INT) {
        vr = MAX_GAIN_INT;
    }
    float v = mCachedVolume;
    vl *= v;
    vr *= v;
    vlr = (vr << 16) | (vl & 0xFFFF);
    return vlr;
}
status_t AudioFlinger::PlaybackThread::Track::setSyncEvent(const sp<SyncEvent>& event)
{
    if (isTerminated() || mState == PAUSED ||
            ((framesReady() == 0) && ((mSharedBuffer != 0) ||
                                      (mState == STOPPED)))) {
        ALOGW("Track::setSyncEvent() in invalid state %d on session %d %s mode, framesReady %d ",
              mState, mSessionId, (mSharedBuffer != 0) ? "static" : "stream", framesReady());
        event->cancel();
        return INVALID_OPERATION;
    }
    (void) TrackBase::setSyncEvent(event);
    return NO_ERROR;
}
void AudioFlinger::PlaybackThread::Track::invalidate()
{
    audio_track_cblk_t* cblk = mCblk;
    android_atomic_or(CBLK_INVALID, &cblk->mFlags);
    android_atomic_release_store(0x40000000, &cblk->mFutex);
    (void) __futex_syscall3(&cblk->mFutex, FUTEX_WAKE, INT_MAX);
    mIsInvalid = true;
}
void AudioFlinger::PlaybackThread::Track::signal()
{
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        PlaybackThread *t = (PlaybackThread *)thread.get();
        Mutex::Autolock _l(t->mLock);
        t->broadcast_l();
    }
}
sp<AudioFlinger::PlaybackThread::TimedTrack>
AudioFlinger::PlaybackThread::TimedTrack::create(
            PlaybackThread *thread,
            const sp<Client>& client,
            audio_stream_type_t streamType,
            uint32_t sampleRate,
            audio_format_t format,
            audio_channel_mask_t channelMask,
            size_t frameCount,
            const sp<IMemory>& sharedBuffer,
            int sessionId,
            int uid)
{
    if (!client->reserveTimedTrack())
        return 0;
    return new TimedTrack(
        thread, client, streamType, sampleRate, format, channelMask, frameCount,
        sharedBuffer, sessionId, uid);
}
AudioFlinger::PlaybackThread::TimedTrack::TimedTrack(
            PlaybackThread *thread,
            const sp<Client>& client,
            audio_stream_type_t streamType,
            uint32_t sampleRate,
            audio_format_t format,
            audio_channel_mask_t channelMask,
            size_t frameCount,
            const sp<IMemory>& sharedBuffer,
            int sessionId,
            int uid)
    : Track(thread, client, streamType, sampleRate, format, channelMask,
            frameCount, sharedBuffer, sessionId, uid, IAudioFlinger::TRACK_TIMED),
      mQueueHeadInFlight(false),
      mTrimQueueHeadOnRelease(false),
      mFramesPendingInQueue(0),
      mTimedSilenceBuffer(NULL),
      mTimedSilenceBufferSize(0),
      mTimedAudioOutputOnTime(false),
      mMediaTimeTransformValid(false)
{
    LocalClock lc;
    mLocalTimeFreq = lc.getLocalFreq();
    mLocalTimeToSampleTransform.a_zero = 0;
    mLocalTimeToSampleTransform.b_zero = 0;
    mLocalTimeToSampleTransform.a_to_b_numer = sampleRate;
    mLocalTimeToSampleTransform.a_to_b_denom = mLocalTimeFreq;
    LinearTransform::reduce(&mLocalTimeToSampleTransform.a_to_b_numer,
                            &mLocalTimeToSampleTransform.a_to_b_denom);
    mMediaTimeToSampleTransform.a_zero = 0;
    mMediaTimeToSampleTransform.b_zero = 0;
    mMediaTimeToSampleTransform.a_to_b_numer = sampleRate;
    mMediaTimeToSampleTransform.a_to_b_denom = 1000000;
    LinearTransform::reduce(&mMediaTimeToSampleTransform.a_to_b_numer,
                            &mMediaTimeToSampleTransform.a_to_b_denom);
}
AudioFlinger::PlaybackThread::TimedTrack::~TimedTrack() {
    mClient->releaseTimedTrack();
    delete [] mTimedSilenceBuffer;
}
status_t AudioFlinger::PlaybackThread::TimedTrack::allocateTimedBuffer(
    size_t size, sp<IMemory>* buffer) {
    Mutex::Autolock _l(mTimedBufferQueueLock);
    trimTimedBufferQueue_l();
    if (mTimedMemoryDealer == NULL) {
        const int kTimedBufferHeapSize = 512 << 10;
        mTimedMemoryDealer = new MemoryDealer(kTimedBufferHeapSize,
                                              "AudioFlingerTimed");
        if (mTimedMemoryDealer == NULL) {
            return NO_MEMORY;
        }
    }
    sp<IMemory> newBuffer = mTimedMemoryDealer->allocate(size);
    if (newBuffer == 0 || newBuffer->pointer() == NULL) {
        return NO_MEMORY;
    }
    *buffer = newBuffer;
    return NO_ERROR;
}
void AudioFlinger::PlaybackThread::TimedTrack::trimTimedBufferQueue_l() {
    int64_t mediaTimeNow;
    {
        Mutex::Autolock mttLock(mMediaTimeTransformLock);
        if (!mMediaTimeTransformValid)
            return;
        int64_t targetTimeNow;
        status_t res = (mMediaTimeTransformTarget == TimedAudioTrack::COMMON_TIME)
            ? mCCHelper.getCommonTime(&targetTimeNow)
            : mCCHelper.getLocalTime(&targetTimeNow);
        if (OK != res)
            return;
        if (!mMediaTimeTransform.doReverseTransform(targetTimeNow,
                                                    &mediaTimeNow)) {
            return;
        }
    }
    size_t trimEnd;
    for (trimEnd = 0; trimEnd < mTimedBufferQueue.size(); trimEnd++) {
        int64_t bufEnd;
        if ((trimEnd + 1) < mTimedBufferQueue.size()) {
            bufEnd = mTimedBufferQueue[trimEnd + 1].pts();
        } else {
            int64_t frameCount = mTimedBufferQueue[trimEnd].buffer()->size()
                               / mFrameSize;
            if (!mMediaTimeToSampleTransform.doReverseTransform(frameCount,
                                                                &bufEnd)) {
                ALOGE("Failed to convert frame count of %lld to media time"
                      " duration" " (scale factor %d/%u) in %s",
                      frameCount,
                      mMediaTimeToSampleTransform.a_to_b_numer,
                      mMediaTimeToSampleTransform.a_to_b_denom,
                      __PRETTY_FUNCTION__);
                break;
            }
            bufEnd += mTimedBufferQueue[trimEnd].pts();
        }
        if (bufEnd > mediaTimeNow)
            break;
        if (!trimEnd && mQueueHeadInFlight) {
            mTrimQueueHeadOnRelease = true;
        }
    }
    size_t trimStart = mTrimQueueHeadOnRelease ? 1 : 0;
    if (trimStart < trimEnd) {
        for (size_t i = trimStart; i < trimEnd; ++i) {
            updateFramesPendingAfterTrim_l(mTimedBufferQueue[i], "trim");
        }
        mTimedBufferQueue.removeItemsAt(trimStart, trimEnd);
    }
}
void AudioFlinger::PlaybackThread::TimedTrack::trimTimedBufferQueueHead_l(
        const char* logTag) {
    ALOG_ASSERT(mTimedBufferQueue.size() > 0,
                "%s called (reason \"%s\"), but timed buffer queue has no"
                " elements to trim.", __FUNCTION__, logTag);
    updateFramesPendingAfterTrim_l(mTimedBufferQueue[0], logTag);
    mTimedBufferQueue.removeAt(0);
}
void AudioFlinger::PlaybackThread::TimedTrack::updateFramesPendingAfterTrim_l(
        const TimedBuffer& buf,
        const char* logTag __unused) {
    uint32_t bufBytes = buf.buffer()->size();
    uint32_t consumedAlready = buf.position();
    ALOG_ASSERT(consumedAlready <= bufBytes,
                "Bad bookkeeping while updating frames pending.  Timed buffer is"
                " only %u bytes long, but claims to have consumed %u"
                " bytes.  (update reason: \"%s\")",
                bufBytes, consumedAlready, logTag);
    uint32_t bufFrames = (bufBytes - consumedAlready) / mFrameSize;
    ALOG_ASSERT(mFramesPendingInQueue >= bufFrames,
                "Bad bookkeeping while updating frames pending.  Should have at"
                " least %u queued frames, but we think we have only %u.  (update"
                " reason: \"%s\")",
                bufFrames, mFramesPendingInQueue, logTag);
    mFramesPendingInQueue -= bufFrames;
}
status_t AudioFlinger::PlaybackThread::TimedTrack::queueTimedBuffer(
    const sp<IMemory>& buffer, int64_t pts) {
    {
        Mutex::Autolock mttLock(mMediaTimeTransformLock);
        if (!mMediaTimeTransformValid)
            return INVALID_OPERATION;
    }
    Mutex::Autolock _l(mTimedBufferQueueLock);
    uint32_t bufFrames = buffer->size() / mFrameSize;
    mFramesPendingInQueue += bufFrames;
    mTimedBufferQueue.add(TimedBuffer(buffer, pts));
    return NO_ERROR;
}
status_t AudioFlinger::PlaybackThread::TimedTrack::setMediaTimeTransform(
    const LinearTransform& xform, TimedAudioTrack::TargetTimeline target) {
    ALOGVV("setMediaTimeTransform az=%lld bz=%lld n=%d d=%u tgt=%d",
           xform.a_zero, xform.b_zero, xform.a_to_b_numer, xform.a_to_b_denom,
           target);
    if (!(target == TimedAudioTrack::LOCAL_TIME ||
          target == TimedAudioTrack::COMMON_TIME)) {
        return BAD_VALUE;
    }
    Mutex::Autolock lock(mMediaTimeTransformLock);
    mMediaTimeTransform = xform;
    mMediaTimeTransformTarget = target;
    mMediaTimeTransformValid = true;
    return NO_ERROR;
}
#define min(a,b) ((a) < (b) ? (a) : (b))
status_t AudioFlinger::PlaybackThread::TimedTrack::getNextBuffer(
    AudioBufferProvider::Buffer* buffer, int64_t pts)
{
    if (pts == AudioBufferProvider::kInvalidPTS) {
        buffer->raw = NULL;
        buffer->frameCount = 0;
        mTimedAudioOutputOnTime = false;
        return INVALID_OPERATION;
    }
    Mutex::Autolock _l(mTimedBufferQueueLock);
    ALOG_ASSERT(!mQueueHeadInFlight,
                "getNextBuffer called without releaseBuffer!");
    while (true) {
        if (mTimedBufferQueue.isEmpty()) {
            buffer->raw = NULL;
            buffer->frameCount = 0;
            return NOT_ENOUGH_DATA;
        }
        TimedBuffer& head = mTimedBufferQueue.editItemAt(0);
        int64_t headLocalPTS;
        {
            Mutex::Autolock mttLock(mMediaTimeTransformLock);
            ALOG_ASSERT(mMediaTimeTransformValid, "media time transform invalid");
            if (mMediaTimeTransform.a_to_b_denom == 0) {
                timedYieldSilence_l(buffer->frameCount, buffer);
                return NO_ERROR;
            }
            int64_t transformedPTS;
            if (!mMediaTimeTransform.doForwardTransform(head.pts(),
                                                        &transformedPTS)) {
                ALOGW("timedGetNextBuffer transform failed");
                buffer->raw = NULL;
                buffer->frameCount = 0;
                trimTimedBufferQueueHead_l("getNextBuffer; no transform");
                return NO_ERROR;
            }
            if (mMediaTimeTransformTarget == TimedAudioTrack::COMMON_TIME) {
                if (OK != mCCHelper.commonTimeToLocalTime(transformedPTS,
                                                          &headLocalPTS)) {
                    buffer->raw = NULL;
                    buffer->frameCount = 0;
                    return INVALID_OPERATION;
                }
            } else {
                headLocalPTS = transformedPTS;
            }
        }
        uint32_t sr = sampleRate();
        int64_t effectivePTS = headLocalPTS +
                ((head.position() / mFrameSize) * mLocalTimeFreq / sr);
        int64_t sampleDelta;
        if (llabs(effectivePTS - pts) >= (static_cast<int64_t>(1) << 31)) {
            ALOGV("*** head buffer is too far from PTS: dropped buffer");
            trimTimedBufferQueueHead_l("getNextBuffer, buf pts too far from"
                                       " mix");
            continue;
        }
        if (!mLocalTimeToSampleTransform.doForwardTransform(
                (effectivePTS - pts) << 32, &sampleDelta)) {
            ALOGV("*** too late during sample rate transform: dropped buffer");
            trimTimedBufferQueueHead_l("getNextBuffer, bad local to sample");
            continue;
        }
        ALOGVV("*** getNextBuffer head.pts=%lld head.pos=%d pts=%lld"
               " sampleDelta=[%d.%08x]",
               head.pts(), head.position(), pts,
               static_cast<int32_t>((sampleDelta >= 0 ? 0 : 1)
                   + (sampleDelta >> 32)),
               static_cast<uint32_t>(sampleDelta & 0xFFFFFFFF));
        const int64_t kSampleContinuityThreshold =
                (static_cast<int64_t>(sr) << 32) / 250;
        const int64_t kSampleStartupThreshold = 1LL << 32;
        if ((mTimedAudioOutputOnTime && llabs(sampleDelta) <= kSampleContinuityThreshold) ||
           (!mTimedAudioOutputOnTime && llabs(sampleDelta) <= kSampleStartupThreshold)) {
            timedYieldSamples_l(buffer);
            ALOGVV("*** on time: head.pos=%d frameCount=%u",
                    head.position(), buffer->frameCount);
            return NO_ERROR;
        }
        mTimedAudioOutputOnTime = false;
        if (sampleDelta > 0) {
            uint32_t framesUntilNextInput = (sampleDelta + 0x80000000) >> 32;
            timedYieldSilence_l(framesUntilNextInput, buffer);
            ALOGV("*** silence: frameCount=%u", buffer->frameCount);
            return NO_ERROR;
        } else {
            uint32_t lateFrames = static_cast<uint32_t>(-((sampleDelta + 0x80000000) >> 32));
            size_t onTimeSamplePosition =
                    head.position() + lateFrames * mFrameSize;
            if (onTimeSamplePosition > head.buffer()->size()) {
                ALOGV("*** too late: dropped buffer");
                trimTimedBufferQueueHead_l("getNextBuffer, dropped late buffer");
                continue;
            } else {
                head.setPosition(onTimeSamplePosition);
                timedYieldSamples_l(buffer);
                ALOGV("*** late: head.pos=%d frameCount=%u", head.position(), buffer->frameCount);
                return NO_ERROR;
            }
        }
    }
}
void AudioFlinger::PlaybackThread::TimedTrack::timedYieldSamples_l(
    AudioBufferProvider::Buffer* buffer) {
    const TimedBuffer& head = mTimedBufferQueue[0];
    buffer->raw = (static_cast<uint8_t*>(head.buffer()->pointer()) +
                   head.position());
    uint32_t framesLeftInHead = ((head.buffer()->size() - head.position()) /
                                 mFrameSize);
    size_t framesRequested = buffer->frameCount;
    buffer->frameCount = min(framesLeftInHead, framesRequested);
    mQueueHeadInFlight = true;
    mTimedAudioOutputOnTime = true;
}
void AudioFlinger::PlaybackThread::TimedTrack::timedYieldSilence_l(
    uint32_t numFrames, AudioBufferProvider::Buffer* buffer) {
    if (mTimedSilenceBufferSize < numFrames * mFrameSize) {
        delete [] mTimedSilenceBuffer;
        mTimedSilenceBufferSize = numFrames * mFrameSize;
        mTimedSilenceBuffer = new uint8_t[mTimedSilenceBufferSize];
        memset(mTimedSilenceBuffer, 0, mTimedSilenceBufferSize);
    }
    buffer->raw = mTimedSilenceBuffer;
    size_t framesRequested = buffer->frameCount;
    buffer->frameCount = min(numFrames, framesRequested);
    mTimedAudioOutputOnTime = false;
}
void AudioFlinger::PlaybackThread::TimedTrack::releaseBuffer(
    AudioBufferProvider::Buffer* buffer) {
    Mutex::Autolock _l(mTimedBufferQueueLock);
    if (buffer->raw == mTimedSilenceBuffer) {
        ALOG_ASSERT(!mQueueHeadInFlight,
                    "Queue head in flight during release of silence buffer!");
        goto done;
    }
    ALOG_ASSERT(mQueueHeadInFlight,
                "TimedTrack::releaseBuffer of non-silence buffer, but no queue"
                " head in flight.");
    if (mTimedBufferQueue.size()) {
        TimedBuffer& head = mTimedBufferQueue.editItemAt(0);
        void* start = head.buffer()->pointer();
        void* end = reinterpret_cast<void*>(
                        reinterpret_cast<uint8_t*>(head.buffer()->pointer())
                        + head.buffer()->size());
        ALOG_ASSERT((buffer->raw >= start) && (buffer->raw < end),
                    "released buffer not within the head of the timed buffer"
                    " queue; qHead = [%p, %p], released buffer = %p",
                    start, end, buffer->raw);
        head.setPosition(head.position() +
                (buffer->frameCount * mFrameSize));
        mQueueHeadInFlight = false;
        ALOG_ASSERT(mFramesPendingInQueue >= buffer->frameCount,
                    "Bad bookkeeping during releaseBuffer!  Should have at"
                    " least %u queued frames, but we think we have only %u",
                    buffer->frameCount, mFramesPendingInQueue);
        mFramesPendingInQueue -= buffer->frameCount;
        if ((static_cast<size_t>(head.position()) >= head.buffer()->size())
            || mTrimQueueHeadOnRelease) {
            trimTimedBufferQueueHead_l("releaseBuffer");
            mTrimQueueHeadOnRelease = false;
        }
    } else {
        LOG_FATAL("TimedTrack::releaseBuffer of non-silence buffer with no"
                  " buffers in the timed buffer queue");
    }
done:
    buffer->raw = 0;
    buffer->frameCount = 0;
}
size_t AudioFlinger::PlaybackThread::TimedTrack::framesReady() const {
    Mutex::Autolock _l(mTimedBufferQueueLock);
    return mFramesPendingInQueue;
}
AudioFlinger::PlaybackThread::TimedTrack::TimedBuffer::TimedBuffer()
        : mPTS(0), mPosition(0) {}
AudioFlinger::PlaybackThread::TimedTrack::TimedBuffer::TimedBuffer(
    const sp<IMemory>& buffer, int64_t pts)
        : mBuffer(buffer), mPTS(pts), mPosition(0) {}
AudioFlinger::PlaybackThread::OutputTrack::OutputTrack(
            PlaybackThread *playbackThread,
            DuplicatingThread *sourceThread,
            uint32_t sampleRate,
            audio_format_t format,
            audio_channel_mask_t channelMask,
            size_t frameCount,
            int uid)
    : Track(playbackThread, NULL, AUDIO_STREAM_CNT, sampleRate, format, channelMask, frameCount,
                NULL, 0, uid, IAudioFlinger::TRACK_DEFAULT),
    mActive(false), mSourceThread(sourceThread), mClientProxy(NULL)
{
    if (mCblk != NULL) {
        mOutBuffer.frameCount = 0;
        playbackThread->mTracks.add(this);
        ALOGV("OutputTrack constructor mCblk %p, mBuffer %p, "
                "frameCount %u, mChannelMask 0x%08x",
                mCblk, mBuffer,
                frameCount, mChannelMask);
        mClientProxy = new AudioTrackClientProxy(mCblk, mBuffer, mFrameCount, mFrameSize);
        mClientProxy->setVolumeLR((uint32_t(uint16_t(0x1000)) << 16) | uint16_t(0x1000));
        mClientProxy->setSendLevel(0.0);
        mClientProxy->setSampleRate(sampleRate);
        mClientProxy = new AudioTrackClientProxy(mCblk, mBuffer, mFrameCount, mFrameSize,
                true );
    } else {
        ALOGW("Error creating output track on thread %p", playbackThread);
    }
}
AudioFlinger::PlaybackThread::OutputTrack::~OutputTrack()
{
    clearBufferQueue();
    delete mClientProxy;
}
status_t AudioFlinger::PlaybackThread::OutputTrack::start(AudioSystem::sync_event_t event,
                                                          int triggerSession)
{
    status_t status = Track::start(event, triggerSession);
    if (status != NO_ERROR) {
        return status;
    }
    mActive = true;
    mRetryCount = 127;
    return status;
}
void AudioFlinger::PlaybackThread::OutputTrack::stop()
{
    Track::stop();
    clearBufferQueue();
    mOutBuffer.frameCount = 0;
    mActive = false;
}
bool AudioFlinger::PlaybackThread::OutputTrack::write(int16_t* data, uint32_t frames)
{
    Buffer *pInBuffer;
    Buffer inBuffer;
    uint32_t channelCount = mChannelCount;
    bool outputBufferFull = false;
    inBuffer.frameCount = frames;
    inBuffer.i16 = data;
    uint32_t waitTimeLeftMs = mSourceThread->waitTimeMs();
    if (!mActive && frames != 0) {
        start();
        sp<ThreadBase> thread = mThread.promote();
        if (thread != 0) {
            MixerThread *mixerThread = (MixerThread *)thread.get();
            if (mFrameCount > frames) {
                if (mBufferQueue.size() < kMaxOverFlowBuffers) {
                    uint32_t startFrames = (mFrameCount - frames);
                    pInBuffer = new Buffer;
                    pInBuffer->mBuffer = new int16_t[startFrames * channelCount];
                    pInBuffer->frameCount = startFrames;
                    pInBuffer->i16 = pInBuffer->mBuffer;
                    memset(pInBuffer->raw, 0, startFrames * channelCount * sizeof(int16_t));
                    mBufferQueue.add(pInBuffer);
                } else {
                    ALOGW("OutputTrack::write() %p no more buffers in queue", this);
                }
            }
        }
    }
    while (waitTimeLeftMs) {
        if (mBufferQueue.size()) {
            pInBuffer = mBufferQueue.itemAt(0);
        } else {
            pInBuffer = &inBuffer;
        }
        if (pInBuffer->frameCount == 0) {
            break;
        }
        if (mOutBuffer.frameCount == 0) {
            mOutBuffer.frameCount = pInBuffer->frameCount;
            nsecs_t startTime = systemTime();
            status_t status = obtainBuffer(&mOutBuffer, waitTimeLeftMs);
            if (status != NO_ERROR) {
                ALOGV("OutputTrack::write() %p thread %p no more output buffers; status %d", this,
                        mThread.unsafe_get(), status);
                outputBufferFull = true;
                break;
            }
            uint32_t waitTimeMs = (uint32_t)ns2ms(systemTime() - startTime);
            if (waitTimeLeftMs >= waitTimeMs) {
                waitTimeLeftMs -= waitTimeMs;
            } else {
                waitTimeLeftMs = 0;
            }
        }
        uint32_t outFrames = pInBuffer->frameCount > mOutBuffer.frameCount ? mOutBuffer.frameCount :
                pInBuffer->frameCount;
        memcpy(mOutBuffer.raw, pInBuffer->raw, outFrames * channelCount * sizeof(int16_t));
        Proxy::Buffer buf;
        buf.mFrameCount = outFrames;
        buf.mRaw = NULL;
        mClientProxy->releaseBuffer(&buf);
        pInBuffer->frameCount -= outFrames;
        pInBuffer->i16 += outFrames * channelCount;
        mOutBuffer.frameCount -= outFrames;
        mOutBuffer.i16 += outFrames * channelCount;
        if (pInBuffer->frameCount == 0) {
            if (mBufferQueue.size()) {
                mBufferQueue.removeAt(0);
                delete [] pInBuffer->mBuffer;
                delete pInBuffer;
                ALOGV("OutputTrack::write() %p thread %p released overflow buffer %d", this,
                        mThread.unsafe_get(), mBufferQueue.size());
            } else {
                break;
            }
        }
    }
    if (inBuffer.frameCount) {
        sp<ThreadBase> thread = mThread.promote();
        if (thread != 0 && !thread->standby()) {
            if (mBufferQueue.size() < kMaxOverFlowBuffers) {
                pInBuffer = new Buffer;
                pInBuffer->mBuffer = new int16_t[inBuffer.frameCount * channelCount];
                pInBuffer->frameCount = inBuffer.frameCount;
                pInBuffer->i16 = pInBuffer->mBuffer;
                memcpy(pInBuffer->raw, inBuffer.raw, inBuffer.frameCount * channelCount *
                        sizeof(int16_t));
                mBufferQueue.add(pInBuffer);
                ALOGV("OutputTrack::write() %p thread %p adding overflow buffer %d", this,
                        mThread.unsafe_get(), mBufferQueue.size());
            } else {
                ALOGW("OutputTrack::write() %p thread %p no more overflow buffers",
                        mThread.unsafe_get(), this);
            }
        }
    }
    if (frames == 0 && mBufferQueue.size() == 0) {
        size_t user = 0;
        if (user < mFrameCount) {
            frames = mFrameCount - user;
            pInBuffer = new Buffer;
            pInBuffer->mBuffer = new int16_t[frames * channelCount];
            pInBuffer->frameCount = frames;
            pInBuffer->i16 = pInBuffer->mBuffer;
            memset(pInBuffer->raw, 0, frames * channelCount * sizeof(int16_t));
            mBufferQueue.add(pInBuffer);
        } else if (mActive) {
            stop();
        }
    }
    return outputBufferFull;
}
status_t AudioFlinger::PlaybackThread::OutputTrack::obtainBuffer(
        AudioBufferProvider::Buffer* buffer, uint32_t waitTimeMs)
{
    ClientProxy::Buffer buf;
    buf.mFrameCount = buffer->frameCount;
    struct timespec timeout;
    timeout.tv_sec = waitTimeMs / 1000;
    timeout.tv_nsec = (int) (waitTimeMs % 1000) * 1000000;
    status_t status = mClientProxy->obtainBuffer(&buf, &timeout);
    buffer->frameCount = buf.mFrameCount;
    buffer->raw = buf.mRaw;
    return status;
}
void AudioFlinger::PlaybackThread::OutputTrack::clearBufferQueue()
{
    size_t size = mBufferQueue.size();
    for (size_t i = 0; i < size; i++) {
        Buffer *pBuffer = mBufferQueue.itemAt(i);
        delete [] pBuffer->mBuffer;
        delete pBuffer;
    }
    mBufferQueue.clear();
}
AudioFlinger::RecordHandle::RecordHandle(
        const sp<AudioFlinger::RecordThread::RecordTrack>& recordTrack)
    : BnAudioRecord(),
    mRecordTrack(recordTrack)
{
}
AudioFlinger::RecordHandle::~RecordHandle() {
    stop_nonvirtual();
    mRecordTrack->destroy();
}
sp<IMemory> AudioFlinger::RecordHandle::getCblk() const {
    return mRecordTrack->getCblk();
}
status_t AudioFlinger::RecordHandle::start(int event,
        int triggerSession) {
    ALOGV("RecordHandle::start()");
    return mRecordTrack->start((AudioSystem::sync_event_t)event, triggerSession);
}
void AudioFlinger::RecordHandle::stop() {
    stop_nonvirtual();
}
void AudioFlinger::RecordHandle::stop_nonvirtual() {
    ALOGV("RecordHandle::stop()");
    mRecordTrack->stop();
}
status_t AudioFlinger::RecordHandle::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    return BnAudioRecord::onTransact(code, data, reply, flags);
}
AudioFlinger::RecordThread::RecordTrack::RecordTrack(
            RecordThread *thread,
            const sp<Client>& client,
            uint32_t sampleRate,
            audio_format_t format,
            audio_channel_mask_t channelMask,
            size_t frameCount,
            int sessionId,
            int uid)
    : TrackBase(thread, client, sampleRate, format,
                  channelMask, frameCount, 0 , sessionId, uid, false ),
        mOverflow(false)
{
    ALOGV("RecordTrack constructor");
    if (mCblk != NULL) {
        mServerProxy = new AudioRecordServerProxy(mCblk, mBuffer, frameCount, mFrameSize);
    }
}
AudioFlinger::RecordThread::RecordTrack::~RecordTrack()
{
    ALOGV("%s", __func__);
}
status_t AudioFlinger::RecordThread::RecordTrack::getNextBuffer(AudioBufferProvider::Buffer* buffer,
        int64_t pts __unused)
{
    ServerProxy::Buffer buf;
    buf.mFrameCount = buffer->frameCount;
    status_t status = mServerProxy->obtainBuffer(&buf);
    buffer->frameCount = buf.mFrameCount;
    buffer->raw = buf.mRaw;
    if (buf.mFrameCount == 0) {
        (void) android_atomic_or(CBLK_OVERRUN, &mCblk->mFlags);
    }
    return status;
}
status_t AudioFlinger::RecordThread::RecordTrack::start(AudioSystem::sync_event_t event,
                                                        int triggerSession)
{
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        RecordThread *recordThread = (RecordThread *)thread.get();
        return recordThread->start(this, event, triggerSession);
    } else {
        return BAD_VALUE;
    }
}
void AudioFlinger::RecordThread::RecordTrack::stop()
{
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        RecordThread *recordThread = (RecordThread *)thread.get();
        if (recordThread->stop(this)) {
            AudioSystem::stopInput(recordThread->id());
        }
    }
}
void AudioFlinger::RecordThread::RecordTrack::destroy()
{
    sp<RecordTrack> keep(this);
    {
        sp<ThreadBase> thread = mThread.promote();
        if (thread != 0) {
            if (mState == ACTIVE || mState == RESUMING) {
                AudioSystem::stopInput(thread->id());
            }
            AudioSystem::releaseInput(thread->id());
            Mutex::Autolock _l(thread->mLock);
            RecordThread *recordThread = (RecordThread *) thread.get();
            recordThread->destroyTrack_l(this);
        }
    }
}
void AudioFlinger::RecordThread::RecordTrack::invalidate()
{
    audio_track_cblk_t* cblk = mCblk;
    android_atomic_or(CBLK_INVALID, &cblk->mFlags);
    android_atomic_release_store(0x40000000, &cblk->mFutex);
    (void) __futex_syscall3(&cblk->mFutex, FUTEX_WAKE, INT_MAX);
}
           void AudioFlinger::RecordThread::RecordTrack::appendDumpHeader(String8& result)
{
    result.append("    Active Client Fmt Chn mask Session S   Server fCount\n");
}
void AudioFlinger::RecordThread::RecordTrack::dump(char* buffer, size_t size, bool active)
{
    snprintf(buffer, size, "    %6s %6u %3u %08X %7u %1d %08X %6zu\n",
            active ? "yes" : "no",
            (mClient == 0) ? getpid_cached : mClient->pid(),
            mFormat,
            mChannelMask,
            mSessionId,
            mState,
            mCblk->mServer,
            mFrameCount);
}
};
