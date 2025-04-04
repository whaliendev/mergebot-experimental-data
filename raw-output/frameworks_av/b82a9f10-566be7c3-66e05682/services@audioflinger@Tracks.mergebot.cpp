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
//#define LOG_NDEBUG 0

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

namespace android {

// ----------------------------------------------------------------------------
//      TrackBase
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
//      TrackBase
// ----------------------------------------------------------------------------
static volatile int32_t nextTrackId = 55;

// TrackBase constructor must be called with AudioFlinger::mLock held
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
    :   RefBase(),
        mThread(thread),
        mClient(client),
        mCblk(NULL),
        // mBuffer
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
    // if the caller is us, trust the specified uid
    if (IPCThreadState::self()->getCallingPid() != getpid_cached || clientUid == -1) {
        int newclientUid = IPCThreadState::self()->getCallingUid();
        if (clientUid != -1 && clientUid != newclientUid) {
            ALOGW("uid %d tried to pass itself off as %d", newclientUid, clientUid);
        }
        clientUid = newclientUid;
    }
    // clientUid contains the uid of the app that is responsible for this track, so we can blame
    // battery usage on it.
    mUid = clientUid;

    // client == 0 implies sharedBuffer == 0
    ALOG_ASSERT(!(client == 0 && sharedBuffer != 0));

    ALOGV_IF(sharedBuffer != 0, "sharedBuffer: %p, size: %d", sharedBuffer->pointer(),
            sharedBuffer->size());

    // ALOGD("Creating track with %d buffers @ %d bytes", bufferCount, bufferSize);
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
        // this syntax avoids calling the audio_track_cblk_t constructor twice
        mCblk = (audio_track_cblk_t *) new uint8_t[size];
        // assume mCblk != NULL
    }

    // construct the shared structure in-place.
    if (mCblk != NULL) {
        new(mCblk) audio_track_cblk_t();
        // clear all buffers
        if (sharedBuffer == 0) {
            mBuffer = (char*)mCblk + sizeof(audio_track_cblk_t);
            memset(mBuffer, 0, bufferSize);
        } else {
            mBuffer = sharedBuffer->pointer();
#if 0
            mCblk->mFlags = CBLK_FORCEREADY;    // FIXME hack, need to fix the track ready logic
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
    // delete the proxy before deleting the shared memory it refers to, to avoid dangling reference
    delete mServerProxy;
    if (mCblk !{
#ifdef TEE_SINK
    dumpTee(-1, mTeeSource, mId);
#endif
    // delete the proxy before deleting the shared memory it refers to, to avoid dangling reference
    delete mServerProxy;
    if (mCblk != NULL) {
        if (mClient == 0) {
            delete mCblk;
        } else {
            mCblk->~audio_track_cblk_t();   // destroy our shared-structure.
        }
    }
    mCblkMemory.clear();    // free the shared memory before releasing the heap it belongs to
    if (mClient != 0) {
        // Client destructor must run with AudioFlinger mutex locked
        Mutex::Autolock _l(mClient->audioFlinger()->mLock);
        // If the client's reference count drops to zero, the associated destructor
        // must run with AudioFlinger lock held. Thus the explicit clear() rather than
        // relying on the automatic clear() at end of scope.
        mClient.clear();
    }
}

// AudioBufferProvider interface
// getNextBuffer() = 0;
// This implementation of releaseBuffer() is used by Track and RecordTrack, but not TimedTrack
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

// ----------------------------------------------------------------------------
//      Playback
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
//      Playback
// ----------------------------------------------------------------------------
AudioFlinger::TrackHandle::TrackHandle(const sp<AudioFlinger::PlaybackThread::Track>& track)
    : BnAudioTrack(),
      mTrack(track)
{
}

AudioFlinger::TrackHandle::~TrackHandle() {
    // just stop the track on deletion, associated resources
    // will be freed from the main thread once all pending buffers have
    // been played. Unless it's not in the active track list, in which
    // case we free everything now...
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

// ----------------------------------------------------------------------------
// Track constructor must be called with AudioFlinger::mLock and ThreadBase::mLock held
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
    :   TrackBase(thread, client, sampleRate, format, channelMask, frameCount, sharedBuffer,
            sessionId, uid, true /*isOut*/),
    mFillingUpStatus(FS_INVALID),
    // mRetryCount initialized later when needed
    mSharedBuffer(sharedBuffer),
    mStreamType(streamType),
    mName(-1),  // see note below
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
        // to avoid leaking a track name, do not allocate one unless there is an mCblk
        mName = thread->getTrackName_l(channelMask, sessionId);
        if (mName < 0) {
            ALOGE("no more track names available");
            return;
        }
        // only allocate a fast track index if we were able to allocate a normal track name
        if (flags & IAudioFlinger::TRACK_FAST) {
            mAudioTrackServerProxy->framesReadyIsCalledByMultipleThreads();
            ALOG_ASSERT(thread->mFastTrackAvailMask != 0);
            int i = __builtin_ctz(thread->mFastTrackAvailMask);
            ALOG_ASSERT(0 < i && i < (int)FastMixerState::kMaxFastTracks);
            // FIXME This is too eager.  We allocate a fast track index before the
            //       fast track becomes active.  Since fast tracks are a scarce resource,
            //       this means we are potentially denying other more important fast tracks from
            //       being created.  It would be better to allocate the index dynamically.
            mFastIndex = i;
            // Read the initial underruns because this field is never cleared by the fast mixer
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

    // The destructor would clear mSharedBuffer,
    // but it will not push the decremented reference count,
    // leaving the client's IMemory dangling indefinitely.
    // This prevents that leak.
    if (mSharedBuffer != 0) {
        mSharedBuffer.clear();
        // flush the binder command buffer
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
    // NOTE: destroyTrack_l() can remove a strong reference to this Track
    // by removing it from mTracks vector, so there is a risk that this Tracks's
    // destructor is called. As the destructor needs to lock mLock,
    // we must acquire a strong reference on this Track before locking mLock
    // here so that the destructor is called only when exiting this function.
    // On the other hand, as long as Track::destroy() is only called by
    // TrackHandle destructor, the TrackHandle still holds a strong ref on
    // this Track with its member mTrack.
    sp<Track> keep(this);
    { // scope for mLock
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

           /*static*/
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
<<<<<<< HEAD
    snprintf(&buffer[8], size-8, " %6s %6u %4u %08X %08X %7u %6u %1c %1d %5u %5.2g %5.2g  "
                                 "%08X %08X %08X 0x%03X %9u%c\n",
            active ? "yes" : "no",
||||||| 66e05682e4
    snprintf(&buffer[7], size-7, " %6u %4u %08X %08X %7u %6u %1c %1d %5u %5.2g %5.2g  "
                                 "%08X %08X %08X 0x%03X %9u%c\n",
=======
    snprintf(&buffer[7], size-7, " %6u %4u %08X %08X %7u %6zu %1c %1d %5u %5.2g %5.2g  "
                                 "%08X %p %p 0x%03X %9u%c\n",
>>>>>>> 566be7c3
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

// AudioBufferProvider interface
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

// releaseBuffer() is not overridden
// ExtendedAudioBufferProvider interface
// Note that framesReady() takes a mutex on the control block using tryLock().
// This could result in priority inversion if framesReady() is called by the normal mixer,
// as the normal mixer thread runs at lower
// priority than the client's callback thread:  there is a short window within framesReady()
// during which the normal mixer could be preempted, and the client callback would block.
// Another problem can occur if framesReady() is called by the fast mixer:
// the tryLock() could block for up to 1 ms, and a sequence of these could delay fast mixer.
// FIXME Replace AudioTrackShared control block implementation by a non-blocking FIFO queue.
size_t AudioFlinger::PlaybackThread::Track::framesReady() const {
    return mAudioTrackServerProxy->framesReady();
}

size_t AudioFlinger::PlaybackThread::Track::framesReleased() const
{
    return mAudioTrackServerProxy->framesReleased();
}

// Don't call for fast tracks; the framesReady() could result in priority inversion
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

status_t AudioFlinger::PlaybackThread::Track::start(AudioSystem::sync_event_t event __unused, int triggerSession __unused)
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
        // here the track could be either new, or restarted
        // in both cases "unstop" the track

        if (state == PAUSED) {
            if (mResumeToStopping) {
                // happened we need to resume to STOPPING_1
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
            //  restore previous state if start was rejected by policy manager
            if (status == PERMISSION_DENIED) {
                mState = state;
            }
        }
        // track was already in the active list, not a problem
        if (status == ALREADY_EXISTS) {
            status = NO_ERROR;
        } else {
            // Acknowledge any pending flush(), so that subsequent new data isn't discarded.
            // It is usually unsafe to access the server proxy from a binder thread.
            // But in this case we know the mixer thread (whether normal mixer or fast mixer)
            // isn't looking at this track yet:  we still hold the normal mixer thread lock,
            // and for fast tracks the track is not yet in the fast mixer thread's active set.
            ServerProxy::Buffer buffer;
            buffer.mFrameCount = 1;
            (void) mAudioTrackServerProxy->obtainBuffer(&buffer, true /*ackFlush*/);
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
            // If the track is not active (PAUSED and buffers full), flush buffers
            PlaybackThread *playbackThread = (PlaybackThread *)thread.get();
            if (playbackThread->mActiveTracks.indexOf(this) < 0) {
                reset();
                mState = STOPPED;
            } else if (!isFastTrack() && !isOffloaded()) {
                mState = STOPPED;
            } else {
                // For fast tracks prepareTracks_l() will set state to STOPPING_2
                // presentation is complete
                // For an offloaded track this starts a drain and state will
                // move to STOPPING_2 when drain completes and then STOPPED
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
                /* nothing to do if track is not offloaded */
                break;
            }

            // Offloaded track was draining, we need to carry on draining when resumed
            mResumeToStopping = true;
            // fall through...
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
            // If offloaded we allow flush during any state except terminated
            // and keep the track active to avoid problems if user is seeking
            // rapidly and underlying hardware has a significant delay handling
            // a pause
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
            // No point remaining in PAUSED state after a flush => go to
            // FLUSHED state
            mState = FLUSHED;
            // do not reset the track if it is still in the process of being stopped or paused.
            // this will be done by prepareTracks_l() when the track is stopped.
            // prepareTracks_l() will see mState == FLUSHED, then
            // remove from active track list, reset(), and trigger presentation complete
            if (playbackThread->mActiveTracks.indexOf(this) < 0) {
                reset();
            }
        }
        // Prevent flush being lost if the track is flushed and then resumed
        // before mixer thread can run. This is important when offloading
        // because the hardware buffer could hold a large amount of audio
        playbackThread->broadcast_l();
    }
}

// must be called with thread lock held
void AudioFlinger::PlaybackThread::Track::flushAck()
{
    if (!isOffloaded())
        return;

    mFlushHwPending = false;
}

void AudioFlinger::PlaybackThread::Track::reset()
{
    // Do not reset twice to avoid discarding data written just after a flush and before
    // the audioflinger thread detects the track is stopped.
    if (!mResetDone) {
        // Force underrun condition to avoid false underrun callback until first data is
        // written to buffer
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
    // Client should implement this using SSQ; the unpresented frame count in latch is irrelevant
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
            // removeEffect_l() has stopped the effect if it was active so it must be restarted
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
    // a track is considered presented when the total number of frames written to audio HAL
    // corresponds to the number of frames written when presentationComplete() is called for the
    // first time (mPresentationCompleteFrames == 0) plus the buffer filling status at that time.
    // For an offloaded track the HAL+h/w delay is variable so a HAL drain() is used
    // to detect when all frames have been played. In this case framesWritten isn't
    // useful because it doesn't always reflect whether there is data in the h/w
    // buffers, particularly if a track has been paused and resumed during draining
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

// implement VolumeBufferProvider interface

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

// ----------------------------------------------------------------------------

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

MPEG4Writer::Track::~Track() {
    stop();

    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;

    mStszTableEntries {
    stop();

    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;

    mStszTableEntries = NULL;
    mStcoTableEntries = NULL;
    mCo64TableEntries = NULL;
    mStscTableEntries = NULL;
    mSttsTableEntries = NULL;
    mStssTableEntries = NULL;
    mCttsTableEntries = NULL;

    if (mCodecSpecificData != NULL) {
        free(mCodecSpecificData);
        mCodecSpecificData = NULL;
    }
}

MPEG4Writer::Track::~Track() {
    stop();

    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;

    mStszTableEntries {
    stop();

    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;

    mStszTableEntries = NULL;
    mStcoTableEntries = NULL;
    mCo64TableEntries = NULL;
    mStscTableEntries = NULL;
    mSttsTableEntries = NULL;
    mStssTableEntries = NULL;
    mCttsTableEntries = NULL;

    if (mCodecSpecificData != NULL) {
        free(mCodecSpecificData);
        mCodecSpecificData = NULL;
    }
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

// implementation of getNextBuffer for tracks whose buffers have timestamps
/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

MPEG4Writer::Track::~Track() {
    stop();

    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;

    mStszTableEntries {
    stop();

    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;

    mStszTableEntries = NULL;
    mStcoTableEntries = NULL;
    mCo64TableEntries = NULL;
    mStscTableEntries = NULL;
    mSttsTableEntries = NULL;
    mStssTableEntries = NULL;
    mCttsTableEntries = NULL;

    if (mCodecSpecificData != NULL) {
        free(mCodecSpecificData);
        mCodecSpecificData = NULL;
    }
}

MPEG4Writer::Track::~Track() {
    stop();

    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;

    mStszTableEntries {
    stop();

    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;

    mStszTableEntries = NULL;
    mStcoTableEntries = NULL;
    mCo64TableEntries = NULL;
    mStscTableEntries = NULL;
    mSttsTableEntries = NULL;
    mStssTableEntries = NULL;
    mCttsTableEntries = NULL;

    if (mCodecSpecificData != NULL) {
        free(mCodecSpecificData);
        mCodecSpecificData = NULL;
    }
}

// ----------------------------------------------------------------------------

MPEG4Writer::Track::~Track() {
    stop();

    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;

    mStszTableEntries {
    stop();

    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;

    mStszTableEntries = NULL;
    mStcoTableEntries = NULL;
    mCo64TableEntries = NULL;
    mStscTableEntries = NULL;
    mSttsTableEntries = NULL;
    mStssTableEntries = NULL;
    mCttsTableEntries = NULL;

    if (mCodecSpecificData != NULL) {
        free(mCodecSpecificData);
        mCodecSpecificData = NULL;
    }
}

MPEG4Writer::Track::~Track() {
    stop();

    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;

    mStszTableEntries {
    stop();

    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;

    mStszTableEntries = NULL;
    mStcoTableEntries = NULL;
    mCo64TableEntries = NULL;
    mStscTableEntries = NULL;
    mSttsTableEntries = NULL;
    mStssTableEntries = NULL;
    mCttsTableEntries = NULL;

    if (mCodecSpecificData != NULL) {
        free(mCodecSpecificData);
        mCodecSpecificData = NULL;
    }
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

// ----------------------------------------------------------------------------
//      Record
// ----------------------------------------------------------------------------

MPEG4Writer::Track::~Track() {
    stop();

    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;

    mStszTableEntries {
    stop();

    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;

    mStszTableEntries = NULL;
    mStcoTableEntries = NULL;
    mCo64TableEntries = NULL;
    mStscTableEntries = NULL;
    mSttsTableEntries = NULL;
    mStssTableEntries = NULL;
    mCttsTableEntries = NULL;

    if (mCodecSpecificData != NULL) {
        free(mCodecSpecificData);
        mCodecSpecificData = NULL;
    }
}

MPEG4Writer::Track::~Track() {
    stop();

    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;

    mStszTableEntries {
    stop();

    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;

    mStszTableEntries = NULL;
    mStcoTableEntries = NULL;
    mCo64TableEntries = NULL;
    mStscTableEntries = NULL;
    mSttsTableEntries = NULL;
    mStssTableEntries = NULL;
    mCttsTableEntries = NULL;

    if (mCodecSpecificData != NULL) {
        free(mCodecSpecificData);
        mCodecSpecificData = NULL;
    }
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

MPEG4Writer::Track::~Track() {
    stop();

    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;

    mStszTableEntries {
    stop();

    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;

    mStszTableEntries = NULL;
    mStcoTableEntries = NULL;
    mCo64TableEntries = NULL;
    mStscTableEntries = NULL;
    mSttsTableEntries = NULL;
    mStssTableEntries = NULL;
    mCttsTableEntries = NULL;

    if (mCodecSpecificData != NULL) {
        free(mCodecSpecificData);
        mCodecSpecificData = NULL;
    }
}

MPEG4Writer::Track::~Track() {
    stop();

    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;

    mStszTableEntries {
    stop();

    delete mStszTableEntries;
    delete mStcoTableEntries;
    delete mCo64TableEntries;
    delete mStscTableEntries;
    delete mSttsTableEntries;
    delete mStssTableEntries;
    delete mCttsTableEntries;

    mStszTableEntries = NULL;
    mStcoTableEntries = NULL;
    mCo64TableEntries = NULL;
    mStscTableEntries = NULL;
    mSttsTableEntries = NULL;
    mStssTableEntries = NULL;
    mCttsTableEntries = NULL;

    if (mCodecSpecificData != NULL) {
        free(mCodecSpecificData);
        mCodecSpecificData = NULL;
    }
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

/*
 * Geodata is stored according to ISO-6709 standard.
 */
void MPEG4Writer::writeGeoDataBox() {
    beginBox("\xA9xyz");
    /*
     * For historical reasons, any user data start
     * with "\0xA9", must be followed by its assoicated
     * language code.
     * 0x0012: text string length
     * 0x15c7: lang (locale) code: en
     */
    writeInt32(0x001215c7);
    writeLatitude(mLatitudex10000);
    writeLongitude(mLongitudex10000);
    writeInt8(0x2F);
    endBox();
}

} 

   // namespace android