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

#define ATRACE_TAG ATRACE_TAG_AUDIO
#include "MmapTracks.h"
#include "PlaybackTracks.h"
#include "RecordTracks.h"
#include "Client.h"
#include "IAfEffect.h"
#include "IAfThread.h"
#include "ResamplerBufferProvider.h"
#include <audio_utils/minifloat.h>
#include <media/AudioValidator.h>
#include <media/RecordBufferConverter.h>
#include <media/nbaio/Pipe.h>
#include <media/nbaio/PipeReader.h>
#include <mediautils/ServiceUtilities.h>
#include <mediautils/SharedMemoryAllocator.h>
#include <private/media/AudioTrackShared.h>
#include <utils/Log.h>
#include <utils/Trace.h>
#include <linux/futex.h>
#include <math.h>
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
// TODO: Remove when this is put into AidlConversionUtil.h

#define VALUE_OR_RETURN_BINDER_STATUS(x)    \
    ({                                      \
       auto _tmp = (x);                     \
       if (!_tmp.ok()) return ::android::aidl_utils::binderStatusFromStatusT(_tmp.error()); \
       std::move(_tmp.value());             \
     })

namespace android {

using ::android::aidl_utils::binderStatusFromStatusT;
using binder::Status;
using content::AttributionSourceState;
using media::VolumeShaper;
#define LOG_TAG "AF::TrackBase"

static volatile int32_t nextTrackId = 55;

// TrackBase constructor must be called with AudioFlinger::mLock held
TrackBase::TrackBase(
        IAfThreadBase *thread,
            const sp<Client>& client,
            const audio_attributes_t& attr,
            uint32_t sampleRate,
            audio_format_t format,
            audio_channel_mask_t channelMask,
            size_t frameCount,
            void *buffer,
            size_t bufferSize,
            audio_session_t sessionId,
            pid_t creatorPid,
            uid_t clientUid,
            bool isOut,
            const alloc_type alloc,
            track_type type,
            audio_port_handle_t portId,
            std::string metricsId)
    :
        mThread(thread),
        mAllocType(alloc),
        mClient(client),
        mCblk(NULL),
        // mBuffer, mBufferSize
        mState(IDLE),
        mAttr(attr),
        mSampleRate(sampleRate),
        mFormat(format),
        mChannelMask(channelMask),
        mChannelCount(isOut ?
                audio_channel_count_from_out_mask(channelMask) :
                audio_channel_count_from_in_mask(channelMask)),
        mFrameSize(audio_has_proportional_frames(format) ?
                mChannelCount * audio_bytes_per_sample(format) : sizeof(int8_t)),
        mFrameCount(frameCount),
        mSessionId(sessionId),
        mIsOut(isOut),
        mId(android_atomic_inc(&nextTrackId)),
        mTerminated(false),
        mType(type),
        mThreadIoHandle(thread ? thread->id() : AUDIO_IO_HANDLE_NONE),
        mPortId(portId),
        mIsInvalid(false),
        mTrackMetrics(std::move(metricsId), isOut, clientUid),
        mCreatorPid(creatorPid)
{
    const uid_t callingUid = IPCThreadState::self()->getCallingUid();
    if (!isAudioServerOrMediaServerUid(callingUid) || clientUid == AUDIO_UID_INVALID) {
        ALOGW_IF(clientUid != AUDIO_UID_INVALID && clientUid != callingUid,
                "%s(%d): uid %d tried to pass itself off as %d",
                 __func__, mId, callingUid, clientUid);
        clientUid = callingUid;
    }
    // clientUid contains the uid of the app that is responsible for this track, so we can blame
    // battery usage on it.
    mUid = clientUid;

    // ALOGD("Creating track with %d buffers @ %d bytes", bufferCount, bufferSize);

    size_t minBufferSize = buffer == NULL ? roundup(frameCount) : frameCount;
    // check overflow when computing bufferSize due to multiplication by mFrameSize.
    if (minBufferSize < frameCount  // roundup rounds down for values above UINT_MAX / 2
            || mFrameSize == 0   // format needs to be correct
            || minBufferSize > SIZE_MAX / mFrameSize) {
        android_errorWriteLog(0x534e4554, "34749571");
        return;
    }
    minBufferSize *= mFrameSize;

    if (buffer == nullptr) {
        bufferSize = minBufferSize; // allocated here.
    } else if (minBufferSize > bufferSize) {
        android_errorWriteLog(0x534e4554, "38340117");
        return;
    }

    size_t size = sizeof(audio_track_cblk_t);
    if (buffer == NULL && alloc == ALLOC_CBLK) {
        // check overflow when computing allocation size for streaming tracks.
        if (size > SIZE_MAX - bufferSize) {
            android_errorWriteLog(0x534e4554, "34749571");
            return;
        }
        size += bufferSize;
    }

    if (client != 0) {
        mCblkMemory = client->allocator().allocate(mediautils::NamedAllocRequest{{size},
                std::string("Track ID: ").append(std::to_string(mId))});
        if (mCblkMemory == 0 ||
                (mCblk = static_cast<audio_track_cblk_t *>(mCblkMemory->unsecurePointer())) == NULL) {
            ALOGE("%s(%d): not enough memory for AudioTrack size=%zu", __func__, mId, size);
            ALOGE("%s", client->allocator().dump().c_str());
            mCblkMemory.clear();
            return;
        }
    } else {
        mCblk = (audio_track_cblk_t *) malloc(size);
        if (mCblk == NULL) {
            ALOGE("%s(%d): not enough memory for AudioTrack size=%zu", __func__, mId, size);
            return;
        }
    }

    // construct the shared structure in-place.
    if (mCblk != NULL) {
        new(mCblk) audio_track_cblk_t();
        switch (alloc) {
        case ALLOC_READONLY: {
            const sp<MemoryDealer> roHeap(thread->readOnlyHeap());
            if (roHeap == 0 ||
                    (mBufferMemory = roHeap->allocate(bufferSize)) == 0 ||
                    (mBuffer = mBufferMemory->unsecurePointer()) == NULL) {
                ALOGE("%s(%d): not enough memory for read-only buffer size=%zu",
                        __func__, mId, bufferSize);
                if (roHeap != 0) {
                    roHeap->dump("buffer");
                }
                mCblkMemory.clear();
                mBufferMemory.clear();
                return;
            }
            memset(mBuffer, 0, bufferSize);
            } break;
        case ALLOC_PIPE:
            mBufferMemory = thread->pipeMemory();
            // mBuffer is the virtual address as seen from current process (mediaserver),
            // and should normally be coming from mBufferMemory->unsecurePointer().
            // However in this case the TrackBase does not reference the buffer directly.
            // It should references the buffer via the pipe.
            // Therefore, to detect incorrect usage of the buffer, we set mBuffer to NULL.
            mBuffer = NULL;
            bufferSize = 0;
            break;
        case ALLOC_CBLK:
            // clear all buffers
            if (buffer == NULL) {
                mBuffer = (char*)mCblk + sizeof(audio_track_cblk_t);
                memset(mBuffer, 0, bufferSize);
            } else {
                mBuffer = buffer;
#if 0
                mCblk->mFlags = CBLK_FORCEREADY;    // FIXME hack, need to fix the track ready logic
#endif
            }
            break;
        case ALLOC_LOCAL:
            mBuffer = calloc(1, bufferSize);
            break;
        case ALLOC_NONE:
            mBuffer = buffer;
            break;
        default:
            LOG_ALWAYS_FATAL("%s(%d): invalid allocation type: %d", __func__, mId, (int)alloc);
        }
        mBufferSize = bufferSize;

#ifdef TEE_SINK
        mTee.set(sampleRate, mChannelCount, format, NBAIO_Tee::TEE_FLAG_TRACK);
#endif
        // mState is mirrored for the client to read.
        mState.setMirror(&mCblk->mState);
        // ensure our state matches up until we consolidate the enumeration.
        static_assert(CBLK_STATE_IDLE == IDLE);
        static_assert(CBLK_STATE_PAUSING == PAUSING);
    }
}

// TODO b/182392769: use attribution source util
static AttributionSourceState audioServerAttributionSource(pid_t pid) {
   AttributionSourceState attributionSource{};
   attributionSource.uid = AID_AUDIOSERVER;
   attributionSource.pid = pid;
   attributionSource.token = sp<BBinder>::make();
   return attributionSource;
}

status_t TrackBase::initCheck() const
{
    status_t status;
    if (mType == TYPE_OUTPUT || mType == TYPE_PATCH) {
        status = cblk() != NULL ? NO_ERROR : NO_MEMORY;
    } else {
        status = getCblk() != 0 ? NO_ERROR : NO_MEMORY;
    }
    return status;
}

TrackBase::~TrackBase()
{
    // delete the proxy before deleting the shared memory it refers to, to avoid dangling reference
    mServerProxy.clear();
    releaseCblk();
    mCblkMemory.clear();    // free the shared memory before releasing the heap it belongs to
    if (mClient !{
    // delete the proxy before deleting the shared memory it refers to, to avoid dangling reference
    mServerProxy.clear();
    releaseCblk();
    mCblkMemory.clear();    // free the shared memory before releasing the heap it belongs to
    if (mClient != 0) {
        // Client destructor must run with AudioFlinger client mutex locked
        audio_utils::lock_guard _l(mClient->afClientCallback()->clientMutex());
        // If the client's reference count drops to zero, the associated destructor
        // must run with AudioFlinger lock held. Thus the explicit clear() rather than
        // relying on the automatic clear() at end of scope.
        mClient.clear();
    }
    if (mAllocType == ALLOC_LOCAL) {
        free(mBuffer);
        mBuffer = nullptr;
    }
    // flush the binder command buffer
    IPCThreadState::self()->flushCommands();
}

// AudioBufferProvider interface
// getNextBuffer() = 0;
// This implementation of releaseBuffer() is used by Track and RecordTrack
void TrackBase::releaseBuffer(AudioBufferProvider::Buffer* buffer)
{
#ifdef TEE_SINK
    mTee.write(buffer->raw, buffer->frameCount);
#endif

    ServerProxy::Buffer buf;
    buf.mFrameCount = buffer->frameCount;
    buf.mRaw = buffer->raw;
    buffer->frameCount = 0;
    buffer->raw = NULL;
    mServerProxy->releaseBuffer(&buf);
}

status_t TrackBase::setSyncEvent(
        const sp<audioflinger::SyncEvent>& event)
{
    mSyncEvents.emplace_back(event);
    return NO_ERROR;
}

PatchTrackBase::PatchTrackBase(const sp<ClientProxy>& proxy,
        IAfThreadBase* thread, const Timeout& timeout)
    : mProxy(proxy)
{
    if (timeout) {
        setPeerTimeout(*timeout);
    } else {
        // Double buffer mixer
        uint64_t mixBufferNs = ((uint64_t)2 * thread->frameCount() * 1000000000) /
                                              thread->sampleRate();
        setPeerTimeout(std::chrono::nanoseconds{mixBufferNs});
    }
}

void PatchTrackBase::setPeerTimeout(std::chrono::nanoseconds timeout) {
    mPeerTimeout.tv_sec = timeout.count() / std::nano::den;
    mPeerTimeout.tv_nsec = timeout.count() % std::nano::den;
}


#define LOG_TAG "AF::TrackHandle"

class TrackHandle : public android::media::BnAudioTrack {
public:
    explicit TrackHandle(const sp<IAfTrack>& track);
    ~TrackHandle() override;
    
    binder::Status getCblk(std::optional<media::SharedFileRegion>* _aidl_return) final;
    binder::Status start(int32_t* _aidl_return) final;
    binder::Status stop() final;
    binder::Status flush() final;
    binder::Status pause() final;
    binder::Status attachAuxEffect(int32_t effectId, int32_t* _aidl_return) final;
    binder::Status setParameters(const std::string& keyValuePairs,
                                 int32_t* _aidl_return) final;
    binder::Status selectPresentation(int32_t presentationId, int32_t programId,
                                      int32_t* _aidl_return) final;
    binder::Status getTimestamp(media::AudioTimestampInternal* timestamp,
                                int32_t* _aidl_return) final;
    binder::Status signal() final;
    binder::Status applyVolumeShaper(const media::VolumeShaperConfiguration& configuration,
                                     const media::VolumeShaperOperation& operation,
                                     int32_t* _aidl_return) final;
    binder::Status getVolumeShaperState(
            int32_t id,
            std::optional<media::VolumeShaperState>* _aidl_return) final;
    binder::Status getDualMonoMode(
            media::audio::common::AudioDualMonoMode* _aidl_return) final;
    binder::Status setDualMonoMode(
            media::audio::common::AudioDualMonoMode mode) final;
    binder::Status getAudioDescriptionMixLevel(float* _aidl_return) final;
    binder::Status setAudioDescriptionMixLevel(float leveldB) final;
    binder::Status getPlaybackRateParameters(
            media::audio::common::AudioPlaybackRate* _aidl_return) final;
    binder::Status setPlaybackRateParameters(
            const media::audio::common::AudioPlaybackRate& playbackRate) final;
    
private:
    const sp<IAfTrack> mTrack;
};

/* static */
sp<media::IAudioTrack> IAfTrack::createIAudioTrackAdapter(const sp<IAfTrack>& track) {
    return sp<TrackHandle>::make(track);
}

TrackHandle::TrackHandle(const sp<IAfTrack>& track)
    : BnAudioTrack(),
      mTrack(track)
{
    setMinSchedulerPolicy(SCHED_NORMAL, ANDROID_PRIORITY_AUDIO);
}

TrackHandle::~TrackHandle() {
    // just stop the track on deletion, associated resources
    // will be freed from the main thread once all pending buffers have
    // been played. Unless it's not in the active track list, in which
    // case we free everything now...
    mTrack->destroy();
}

Status TrackHandle::getCblk(
        std::optional<media::SharedFileRegion>* _aidl_return) {
    *_aidl_return = legacy2aidl_NullableIMemory_SharedFileRegion(mTrack->getCblk()).value();
    return Status::ok();
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

#define LOG_TAG "AF::Track"

/* static */
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

// implement VolumeBufferProvider interface

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

#define LOG_TAG "AF::OutputTrack"

/* static */
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

#define LOG_TAG "AF::PatchTrack"

/* static */
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

// ----------------------------------------------------------------------------
//      Record
// ----------------------------------------------------------------------------

#define LOG_TAG "AF::RecordHandle"

class RecordHandle : public android::media::BnAudioRecord {
private:
// for use from destructor
void stop_nonvirtual();
// for use from destructor
void stop_nonvirtual();
// for use from destructor
void stop_nonvirtual();
// for use from destructor
void stop_nonvirtual();
// for use from destructor
void stop_nonvirtual();
// for use from destructor
void stop_nonvirtual();
// for use from destructor
void stop_nonvirtual();
// for use from destructor
void stop_nonvirtual();
// for use from destructor
void stop_nonvirtual();
// for use from destructor
void stop_nonvirtual();
};

/* static */
sp<media::IAudioRecord> IAfRecordTrack::createIAudioRecordAdapter(
        const sp<IAfRecordTrack>& recordTrack) {
    return sp<RecordHandle>::make(recordTrack);
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

#define LOG_TAG "AF::RecordTrack"


/* static */
sp<IAfRecordTrack> IAfRecordTrack::create(IAfRecordThread* thread,
        const sp<Client>& client,
        const audio_attributes_t& attr,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        size_t frameCount,
        void* buffer,
        size_t bufferSize,
        audio_session_t sessionId,
        pid_t creatorPid,
        const AttributionSourceState& attributionSource,
        audio_input_flags_t flags,
        track_type type,
        audio_port_handle_t portId,
        int32_t startFrames)
{
    return sp<RecordTrack>::make(
        thread,
        client,
        attr,
        sampleRate,
        format,
        channelMask,
        frameCount,
        buffer,
        bufferSize,
        sessionId,
        creatorPid,
        attributionSource,
        flags,
        type,
        portId,
        startFrames);
}

// RecordTrack constructor must be called with AudioFlinger::mLock and ThreadBase::mLock held
RecordTrack::RecordTrack(
            IAfRecordThread* thread,
            const sp<Client>& client,
            const audio_attributes_t& attr,
            uint32_t sampleRate,
            audio_format_t format,
            audio_channel_mask_t channelMask,
            size_t frameCount,
            void *buffer,
            size_t bufferSize,
            audio_session_t sessionId,
            pid_t creatorPid,
            const AttributionSourceState& attributionSource,
            audio_input_flags_t flags,
            track_type type,
            audio_port_handle_t portId,
            int32_t startFrames)
    :   TrackBase(thread, client, attr, sampleRate, format,
                  channelMask, frameCount, buffer, bufferSize, sessionId,
                  creatorPid,
                  VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid)),
                  false /*isOut*/,
                  (type == TYPE_DEFAULT) ?
                          ((flags & AUDIO_INPUT_FLAG_FAST) ? ALLOC_PIPE : ALLOC_CBLK) :
                          ((buffer == NULL) ? ALLOC_LOCAL : ALLOC_NONE),
                  type, portId,
                  std::string(AMEDIAMETRICS_KEY_PREFIX_AUDIO_RECORD) + std::to_string(portId)),
        mOverflow(false),
        mResamplerBufferProvider(NULL), // initialize in case of early constructor exit
        mRecordBufferConverter(NULL),
        mFlags(flags),
        mSilenced(false),
        mStartFrames(startFrames)
{
    if (mCblk == NULL) {
        return;
    }

    if (!isDirect()) {
        mRecordBufferConverter = new RecordBufferConverter(
                thread->channelMask(), thread->format(), thread->sampleRate(),
                channelMask, format, sampleRate);
        // Check if the RecordBufferConverter construction was successful.
        // If not, don't continue with construction.
        //
        // NOTE: It would be extremely rare that the record track cannot be created
        // for the current device, but a pending or future device change would make
        // the record track configuration valid.
        if (mRecordBufferConverter->initCheck() != NO_ERROR) {
            ALOGE("%s(%d): RecordTrack unable to create record buffer converter", __func__, mId);
            return;
        }
    }

    mServerProxy = new AudioRecordServerProxy(mCblk, mBuffer, frameCount,
            mFrameSize, !isExternalTrack());

    mResamplerBufferProvider = new ResamplerBufferProvider(this);

    if (flags & AUDIO_INPUT_FLAG_FAST) {
        ALOG_ASSERT(thread->fastTrackAvailable());
        thread->setFastTrackAvailable(false);
    } else {
        // TODO: only Normal Record has timestamps (Fast Record does not).
        mServerLatencySupported = checkServerLatencySupported(mFormat, flags);
    }
#ifdef TEE_SINK
    mTee.setId(std::string("_") + std::to_string(mThreadIoHandle)
            + "_" + std::to_string(mId)
            + "_R");
#endif

    // Once this item is logged by the server, the client can add properties.
    mTrackMetrics.logConstructor(creatorPid, uid(), id());
}

RecordTrack::~RecordTrack()
{
    ALOGV("%s()", __func__);
    delete mRecordBufferConverter;
    delete mResamplerBufferProvider;
}{
    ALOGV("%s()", __func__);
    delete mRecordBufferConverter;
    delete mResamplerBufferProvider;
}

status_t RecordTrack::initCheck() const
{
    status_t status = TrackBase::initCheck();
    if (status == NO_ERROR && mServerProxy == 0) {
        status = BAD_VALUE;
    }
    return status;
}

// AudioBufferProvider interface
status_t RecordTrack::getNextBuffer(AudioBufferProvider::Buffer* buffer)
{
    ServerProxy::Buffer buf;
    buf.mFrameCount = buffer->frameCount;
    status_t status = mServerProxy->obtainBuffer(&buf);
    buffer->frameCount = buf.mFrameCount;
    buffer->raw = buf.mRaw;
    if (buf.mFrameCount == 0) {
        // FIXME also wake futex so that overrun is noticed more quickly
        (void) android_atomic_or(CBLK_OVERRUN, &mCblk->mFlags);
    }
    return status;
}

status_t RecordTrack::start(AudioSystem::sync_event_t event,
                                                        audio_session_t triggerSession)
{
    const sp<IAfThreadBase> thread = mThread.promote();
    if (thread != 0) {
        auto* const recordThread = thread->asIAfRecordThread().get();
        return recordThread->start(this, event, triggerSession);
    } else {
        ALOGW("%s track %d: thread was destroyed", __func__, portId());
        return DEAD_OBJECT;
    }
}

void RecordTrack::stop()
{
    const sp<IAfThreadBase> thread = mThread.promote();
    if (thread != 0) {
        auto* const recordThread = thread->asIAfRecordThread().get();
        if (recordThread->stop(this) && isExternalTrack()) {
            AudioSystem::stopInput(mPortId);
        }
    }
}

void RecordTrack::destroy()
{
    // see comments at Track::destroy()
    sp<RecordTrack> keep(this);
    {
        track_state priorState = mState;
        const sp<IAfThreadBase> thread = mThread.promote();
        if (thread != 0) {
            audio_utils::lock_guard _l(thread->mutex());
            auto* const recordThread = thread->asIAfRecordThread().get();
            priorState = mState;
            if (!mSharedAudioPackageName.empty()) {
                recordThread->resetAudioHistory_l();
            }
            recordThread->destroyTrack_l(this); // move mState to STOPPED, terminate
        }
        // APM portid/client management done outside of lock.
        // NOTE: if thread doesn't exist, the input descriptor probably doesn't either.
        if (isExternalTrack()) {
            switch (priorState) {
            case ACTIVE:     // invalidated while still active
            case STARTING_2: // invalidated/start-aborted after startInput successfully called
            case PAUSING:    // invalidated while in the middle of stop() pausing (still active)
                AudioSystem::stopInput(mPortId);
                break;

            case STARTING_1: // invalidated/start-aborted and startInput not successful
            case PAUSED:     // OK, not active
            case IDLE:       // OK, not active
                break;

            case STOPPED:    // unexpected (destroyed)
            default:
                LOG_ALWAYS_FATAL("%s(%d): invalid prior state: %d", __func__, mId, priorState);
            }
            AudioSystem::releaseInput(mPortId);
        }
    }
}

void RecordTrack::invalidate()
{
    TrackBase::invalidate();
    // FIXME should use proxy, and needs work
    audio_track_cblk_t* cblk = mCblk;
    android_atomic_or(CBLK_INVALID, &cblk->mFlags);
    android_atomic_release_store(0x40000000, &cblk->mFutex);
    // client is not in server, so FUTEX_WAKE is needed instead of FUTEX_WAKE_PRIVATE
    (void) syscall(__NR_futex, &cblk->mFutex, FUTEX_WAKE, INT_MAX);
}


void RecordTrack::appendDumpHeader(String8& result) const
{
    result.appendFormat("Active     Id Client Session Port Id  S  Flags  "
                        " Format Chn mask  SRate Source  "
                        " Server FrmCnt FrmRdy Sil%s\n",
                        isServerLatencySupported() ? "   Latency" : "");
}

void RecordTrack::appendDump(String8& result, bool active) const
{
    result.appendFormat("%c%5s %6d %6u %7u %7u  %2s 0x%03X "
            "%08X %08X %6u %6X "
            "%08X %6zu %6zu %3c",
            isFastTrack() ? 'F' : ' ',
            active ? "yes" : "no",
            mId,
            (mClient == 0) ? getpid() : mClient->pid(),
            mSessionId,
            mPortId,
            getTrackStateAsCodedString(),
            mCblk->mFlags,

            mFormat,
            mChannelMask,
            mSampleRate,
            mAttr.source,

            mCblk->mServer,
            mFrameCount,
            mServerProxy->framesReadySafe(),
            isSilenced() ? 's' : 'n'
            );
    if (isServerLatencySupported()) {
        double latencyMs;
        bool fromTrack;
        if (getTrackLatencyMs(&latencyMs, &fromTrack) == OK) {
            // Show latency in msec, followed by 't' if from track timestamp (the most accurate)
            // or 'k' if estimated from kernel (usually for debugging).
            result.appendFormat(" %7.2lf %c", latencyMs, fromTrack ? 't' : 'k');
        } else {
            result.appendFormat("%10s", mCblk->mServer != 0 ? "unavail" : "new");
        }
    }
    result.append("\n");
}

// This is invoked by SyncEvent callback.
void RecordTrack::handleSyncStartEvent(
        const sp<audioflinger::SyncEvent>& event)
{
    size_t framesToDrop = 0;
    const sp<IAfThreadBase> threadBase = mThread.promote();
    if (threadBase != 0) {
        // TODO: use actual buffer filling status instead of 2 buffers when info is available
        // from audio HAL
        framesToDrop = threadBase->frameCount() * 2;
    }

    mSynchronizedRecordState.onPlaybackFinished(event, framesToDrop);
}

void RecordTrack::clearSyncStartEvent()
{
    mSynchronizedRecordState.clear();
}

void RecordTrack::updateTrackFrameInfo(
        int64_t trackFramesReleased, int64_t sourceFramesRead,
        uint32_t halSampleRate, const ExtendedTimestamp &timestamp)
{
   // Make the kernel frametime available.
    const FrameTime ft{
            timestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL],
            timestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL]};
    // ALOGD("FrameTime: %lld %lld", (long long)ft.frames, (long long)ft.timeNs);
    mKernelFrameTime.store(ft);
    if (!audio_is_linear_pcm(mFormat)) {
        // Stream is direct, return provided timestamp with no conversion
        mServerProxy->setTimestamp(timestamp);
        return;
    }

    ExtendedTimestamp local = timestamp;

    // Convert HAL frames to server-side track frames at track sample rate.
    // We use trackFramesReleased and sourceFramesRead as an anchor point.
    for (int i = ExtendedTimestamp::LOCATION_SERVER; i < ExtendedTimestamp::LOCATION_MAX; ++i) {
        if (local.mTimeNs[i] != 0) {
            const int64_t relativeServerFrames = local.mPosition[i] - sourceFramesRead;
            const int64_t relativeTrackFrames = relativeServerFrames
                    * mSampleRate / halSampleRate; // TODO: potential computation overflow
            local.mPosition[i] = relativeTrackFrames + trackFramesReleased;
        }
    }
    mServerProxy->setTimestamp(local);

    // Compute latency info.
    const bool useTrackTimestamp = true; // use track unless debugging.
    const double latencyMs = - (useTrackTimestamp
            ? local.getOutputServerLatencyMs(sampleRate())
            : timestamp.getOutputServerLatencyMs(halSampleRate));

    mServerLatencyFromTrack.store(useTrackTimestamp);
    mServerLatencyMs.store(latencyMs);
}

status_t RecordTrack::getActiveMicrophones(
        std::vector<media::MicrophoneInfoFw>* activeMicrophones) const
{
    const sp<IAfThreadBase> thread = mThread.promote();
    if (thread != 0) {
        auto* const recordThread = thread->asIAfRecordThread().get();
        return recordThread->getActiveMicrophones(activeMicrophones);
    } else {
        return BAD_VALUE;
    }
}

status_t RecordTrack::setPreferredMicrophoneDirection(
        audio_microphone_direction_t direction) {
    const sp<IAfThreadBase> thread = mThread.promote();
    if (thread != 0) {
        auto* const recordThread = thread->asIAfRecordThread().get();
        return recordThread->setPreferredMicrophoneDirection(direction);
    } else {
        return BAD_VALUE;
    }
}

status_t RecordTrack::setPreferredMicrophoneFieldDimension(float zoom) {
    const sp<IAfThreadBase> thread = mThread.promote();
    if (thread != 0) {
        auto* const recordThread = thread->asIAfRecordThread().get();
        return recordThread->setPreferredMicrophoneFieldDimension(zoom);
    } else {
        return BAD_VALUE;
    }
}

status_t RecordTrack::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {

    const uid_t callingUid = IPCThreadState::self()->getCallingUid();
    const pid_t callingPid = IPCThreadState::self()->getCallingPid();
    if (callingUid != mUid || callingPid != mCreatorPid) {
        return PERMISSION_DENIED;
    }

    AttributionSourceState attributionSource{};
    attributionSource.uid = VALUE_OR_RETURN_STATUS(legacy2aidl_uid_t_int32_t(callingUid));
    attributionSource.pid = VALUE_OR_RETURN_STATUS(legacy2aidl_uid_t_int32_t(callingPid));
    attributionSource.token = sp<BBinder>::make();
    if (!captureHotwordAllowed(attributionSource)) {
        return PERMISSION_DENIED;
    }

    const sp<IAfThreadBase> thread = mThread.promote();
    if (thread != 0) {
        auto* const recordThread = thread->asIAfRecordThread().get();
        status_t status = recordThread->shareAudioHistory(
                sharedAudioPackageName, mSessionId, sharedAudioStartMs);
        if (status == NO_ERROR) {
            mSharedAudioPackageName = sharedAudioPackageName;
        }
        return status;
    } else {
        return BAD_VALUE;
    }
}

void RecordTrack::copyMetadataTo(MetadataInserter& backInserter) const
{

    // Do not forward PatchRecord metadata with unspecified audio source
    if (mAttr.source == AUDIO_SOURCE_DEFAULT) {
        return;
    }

    // No track is invalid as this is called after prepareTrack_l in the same critical section
    record_track_metadata_v7_t metadata;
    metadata.base = {
            .source = mAttr.source,
            .gain = 1, // capture tracks do not have volumes
    };
    metadata.channel_mask = mChannelMask;
    strncpy(metadata.tags, mAttr.tags, AUDIO_ATTRIBUTES_TAGS_MAX_SIZE);

    *backInserter++ = metadata;
}

#define LOG_TAG "AF::PatchRecord"

/* static */
sp<IAfPatchRecord> IAfPatchRecord::create(
        IAfRecordThread* recordThread,
        uint32_t sampleRate,
        audio_channel_mask_t channelMask,
        audio_format_t format,
        size_t frameCount,
        void *buffer,
        size_t bufferSize,
        audio_input_flags_t flags,
        const Timeout& timeout,
        audio_source_t source)
{
    return sp<PatchRecord>::make(
            recordThread,
            sampleRate,
            channelMask,
            format,
            frameCount,
            buffer,
            bufferSize,
            flags,
            timeout,
            source);
}

PatchRecord::PatchRecord(IAfRecordThread* recordThread,
                                                     uint32_t sampleRate,
                                                     audio_channel_mask_t channelMask,
                                                     audio_format_t format,
                                                     size_t frameCount,
                                                     void *buffer,
                                                     size_t bufferSize,
                                                     audio_input_flags_t flags,
                                                     const Timeout& timeout,
                                                     audio_source_t source)
    :   RecordTrack(recordThread, NULL,
                audio_attributes_t{
    ALOGV("%s(%d): sampleRate %d mPeerTimeout %d.%03d sec",
                                      __func__, mId, sampleRate,
                                      (int)mPeerTimeout.tv_sec,
                                      (int)(mPeerTimeout.tv_nsec / 1000000));
}

PatchRecord::~PatchRecord()
{
    ALOGV("%s(%d)", __func__, mId);
}

static size_t writeFramesHelper(
        AudioBufferProvider* dest, const void* src, size_t frameCount, size_t frameSize)
{
    AudioBufferProvider::Buffer patchBuffer;
    patchBuffer.frameCount = frameCount;
    auto status = dest->getNextBuffer(&patchBuffer);
    if (status != NO_ERROR) {
       ALOGW("%s PathRecord getNextBuffer failed with error %d: %s",
             __func__, status, strerror(-status));
       return 0;
    }
    ALOG_ASSERT(patchBuffer.frameCount <= frameCount);
    memcpy(patchBuffer.raw, src, patchBuffer.frameCount * frameSize);
    size_t framesWritten = patchBuffer.frameCount;
    dest->releaseBuffer(&patchBuffer);
    return framesWritten;
}

// static
size_t PatchRecord::writeFrames(
        AudioBufferProvider* dest, const void* src, size_t frameCount, size_t frameSize)
{
    size_t framesWritten = writeFramesHelper(dest, src, frameCount, frameSize);
    // On buffer wrap, the buffer frame count will be less than requested,
    // when this happens a second buffer needs to be used to write the leftover audio
    const size_t framesLeft = frameCount - framesWritten;
    if (framesWritten != 0 && framesLeft != 0) {
        framesWritten += writeFramesHelper(dest, (const char*)src + framesWritten * frameSize,
                        framesLeft, frameSize);
    }
    return framesWritten;
}

// AudioBufferProvider interface
status_t PatchRecord::getNextBuffer(
                                                  AudioBufferProvider::Buffer* buffer)
{
    ALOG_ASSERT(mPeerProxy != 0, "%s(%d): called without peer proxy", __func__, mId);
    Proxy::Buffer buf;
    buf.mFrameCount = buffer->frameCount;
    status_t status = mPeerProxy->obtainBuffer(&buf, &mPeerTimeout);
    ALOGV_IF(status != NO_ERROR,
             "%s(%d): mPeerProxy->obtainBuffer status %d", __func__, mId, status);
    buffer->frameCount = buf.mFrameCount;
    if (ATRACE_ENABLED()) {
        std::string traceName("PRnObt");
        traceName += std::to_string(id());
        ATRACE_INT(traceName.c_str(), buf.mFrameCount);
    }
    if (buf.mFrameCount == 0) {
        return WOULD_BLOCK;
    }
    status = RecordTrack::getNextBuffer(buffer);
    return status;
}

void PatchRecord::releaseBuffer(Proxy::Buffer* buffer)
{
    mProxy->releaseBuffer(buffer);
}

status_t PatchRecord::obtainBuffer(Proxy::Buffer* buffer,
                                                               const struct timespec *timeOut)
{
    return mProxy->obtainBuffer(buffer, timeOut);
}

void PatchRecord::releaseBuffer(Proxy::Buffer* buffer)
{
    mProxy->releaseBuffer(buffer);
}

#define LOG_TAG "AF::PthrPatchRecord"

static std::unique_ptr<void, decltype(free)*> allocAligned(size_t alignment, size_t size)
{
    void *ptr = nullptr;
    (void)posix_memalign(&ptr, alignment, size);
    return {ptr, free};
}

/* static */
sp<IAfPatchRecord> IAfPatchRecord::createPassThru(
        IAfRecordThread* recordThread,
        uint32_t sampleRate,
        audio_channel_mask_t channelMask,
        audio_format_t format,
        size_t frameCount,
        audio_input_flags_t flags,
        audio_source_t source)
{
    return sp<PassthruPatchRecord>::make(
            recordThread,
            sampleRate,
            channelMask,
            format,
            frameCount,
            flags,
            source);
}

PassthruPatchRecord::PassthruPatchRecord(
        IAfRecordThread* recordThread,
        uint32_t sampleRate,
        audio_channel_mask_t channelMask,
        audio_format_t format,
        size_t frameCount,
        audio_input_flags_t flags,
        audio_source_t source)
        : PatchRecord(recordThread, sampleRate, channelMask, format, frameCount,
                nullptr /*buffer*/, 0 /*bufferSize*/, flags, {
    memset(mStubBuffer.get(), 0, mFrameCount * mFrameSize);
}

sp<StreamInHalInterface> PassthruPatchRecord::obtainStream(
        sp<IAfThreadBase>* thread)
{
    *thread = mThread.promote();
    if (!*thread) return nullptr;
    auto* const recordThread = (*thread)->asIAfRecordThread().get();
    audio_utils::lock_guard _l(recordThread->mutex());
    return recordThread->getInput() ? recordThread->getInput()->stream : nullptr;
}

// PatchProxyBufferProvider methods are called on DirectOutputThread
status_t PassthruPatchRecord::obtainBuffer(
        Proxy::Buffer* buffer, const struct timespec* timeOut)
{
    if (mUnconsumedFrames) {
        buffer->mFrameCount = std::min(buffer->mFrameCount, mUnconsumedFrames);
        // mUnconsumedFrames is decreased in releaseBuffer to use actual frame consumption figure.
        return PatchRecord::obtainBuffer(buffer, timeOut);
    }

    // Otherwise, execute a read from HAL and write into the buffer.
    nsecs_t startTimeNs = 0;
    if (timeOut && (timeOut->tv_sec != 0 || timeOut->tv_nsec != 0) && timeOut->tv_sec != INT_MAX) {
        // Will need to correct timeOut by elapsed time.
        startTimeNs = systemTime();
    }
    const size_t framesToRead = std::min(buffer->mFrameCount, mFrameCount);
    buffer->mFrameCount = 0;
    buffer->mRaw = nullptr;
    sp<IAfThreadBase> thread;
    sp<StreamInHalInterface> stream = obtainStream(&thread);
    if (!stream) return NO_INIT;  // If there is no stream, RecordThread is not reading.

    status_t result = NO_ERROR;
    size_t bytesRead = 0;
    {
        ATRACE_NAME("read");
        result = stream->read(mSinkBuffer.get(), framesToRead * mFrameSize, &bytesRead);
        if (result != NO_ERROR) goto stream_error;
        if (bytesRead == 0) return NO_ERROR;
    }

    {
        audio_utils::lock_guard lock(readMutex());
        mReadBytes += bytesRead;
        mReadError = NO_ERROR;
    }
    mReadCV.notify_one();
    // writeFrames handles wraparound and should write all the provided frames.
    // If it couldn't, there is something wrong with the client/server buffer of the software patch.
    buffer->mFrameCount = writeFrames(
            &mPatchRecordAudioBufferProvider,
            mSinkBuffer.get(), bytesRead / mFrameSize, mFrameSize);
    ALOGW_IF(buffer->mFrameCount < bytesRead / mFrameSize,
            "Lost %zu frames obtained from HAL", bytesRead / mFrameSize - buffer->mFrameCount);
    mUnconsumedFrames = buffer->mFrameCount;
    struct timespec newTimeOut;
    if (startTimeNs) {
        // Correct the timeout by elapsed time.
        nsecs_t newTimeOutNs = audio_utils_ns_from_timespec(timeOut) - (systemTime() - startTimeNs);
        if (newTimeOutNs < 0) newTimeOutNs = 0;
        newTimeOut.tv_sec = newTimeOutNs / NANOS_PER_SECOND;
        newTimeOut.tv_nsec = newTimeOutNs - newTimeOut.tv_sec * NANOS_PER_SECOND;
        timeOut = &newTimeOut;
    }
    return PatchRecord::obtainBuffer(buffer, timeOut);

stream_error:
    stream->standby();
    {
        audio_utils::lock_guard lock(readMutex());
        mReadError = result;
    }
    mReadCV.notify_one();
    return result;
}

void PassthruPatchRecord::releaseBuffer(
        AudioBufferProvider::Buffer* buffer)
{
    buffer->frameCount = 0;
    buffer->raw = nullptr;
}

// AudioBufferProvider and Source methods are called on RecordThread
// 'read' emulates actual audio data with 0's. This is OK as 'getNextBuffer'
// and 'releaseBuffer' are stubbed out and ignore their input.
// It's not possible to retrieve actual data here w/o blocking 'obtainBuffer'
// until we copy it.
status_t PassthruPatchRecord::read(
        void* buffer, size_t bytes, size_t* read)
{
    bytes = std::min(bytes, mFrameCount * mFrameSize);
    {
        audio_utils::unique_lock lock(readMutex());
        mReadCV.wait(lock, [&]{ return mReadError != NO_ERROR || mReadBytes != 0; });
        if (mReadError != NO_ERROR) {
            mLastReadFrames = 0;
            return mReadError;
        }
        *read = std::min(bytes, mReadBytes);
        mReadBytes -= *read;
    }
    mLastReadFrames = *read / mFrameSize;
    memset(buffer, 0, *read);
    return 0;
}

status_t PassthruPatchRecord::getCapturePosition(
        int64_t* frames, int64_t* time)
{
    sp<IAfThreadBase> thread;
    sp<StreamInHalInterface> stream = obtainStream(&thread);
    return stream ? stream->getCapturePosition(frames, time) : NO_INIT;
}

status_t PassthruPatchRecord::standby()
{
    // RecordThread issues 'standby' command in two major cases:
    // 1. Error on read--this case is handled in 'obtainBuffer'.
    // 2. Track is stopping--as PassthruPatchRecord assumes continuous
    //    output, this can only happen when the software patch
    //    is being torn down. In this case, the RecordThread
    //    will terminate and close the HAL stream.
    return 0;
}

// As the buffer gets filled in obtainBuffer, here we only simulate data consumption.
status_t PassthruPatchRecord::getNextBuffer(
        AudioBufferProvider::Buffer* buffer)
{
    buffer->frameCount = mLastReadFrames;
    buffer->raw = buffer->frameCount != 0 ? mStubBuffer.get() : nullptr;
    return NO_ERROR;
}

void PassthruPatchRecord::releaseBuffer(
        AudioBufferProvider::Buffer* buffer)
{
    buffer->frameCount = 0;
    buffer->raw = nullptr;
}

#define LOG_TAG "AF::MmapTrack"

/* static */
sp<IAfMmapTrack> IAfMmapTrack::create(IAfThreadBase* thread,
          const audio_attributes_t& attr,
          uint32_t sampleRate,
          audio_format_t format,
          audio_channel_mask_t channelMask,
          audio_session_t sessionId,
          bool isOut,
          const android::content::AttributionSourceState& attributionSource,
          pid_t creatorPid,
          audio_port_handle_t portId)
{
    return sp<MmapTrack>::make(
            thread,
            attr,
            sampleRate,
            format,
            channelMask,
            sessionId,
            isOut,
            attributionSource,
            creatorPid,
            portId);
}

MmapTrack::MmapTrack(IAfThreadBase* thread,
        const audio_attributes_t& attr,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        audio_session_t sessionId,
        bool isOut,
        const AttributionSourceState& attributionSource,
        pid_t creatorPid,
        audio_port_handle_t portId)
    :   TrackBase(thread, NULL, attr, sampleRate, format,
                  channelMask, (size_t)0 /* frameCount */,
                  nullptr /* buffer */, (size_t)0 /* bufferSize */,
                  sessionId, creatorPid,
                  VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid)),
                  isOut,
                  ALLOC_NONE,
                  TYPE_DEFAULT, portId,
                  std::string(AMEDIAMETRICS_KEY_PREFIX_AUDIO_MMAP) + std::to_string(portId)),
        mPid(VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.pid))),
            mSilenced(false), mSilencedNotified(false)
{
    // Once this item is logged by the server, the client can add properties.
    mTrackMetrics.logConstructor(creatorPid, uid(), id());
}

MmapTrack::~MmapTrack()
{
}

status_t MmapTrack::initCheck() const
{
    return NO_ERROR;
}

status_t MmapTrack::start(AudioSystem::sync_event_t event __unused,
                                                    audio_session_t triggerSession __unused)
{
    return NO_ERROR;
}

void MmapTrack::stop()
{
}

// AudioBufferProvider interface
status_t MmapTrack::getNextBuffer(AudioBufferProvider::Buffer* buffer)
{
    buffer->frameCount = 0;
    buffer->raw = nullptr;
    return INVALID_OPERATION;
}

// ExtendedAudioBufferProvider interface
size_t MmapTrack::framesReady() const {
    return 0;
}

int64_t MmapTrack::framesReleased() const
{
    return 0;
}

void MmapTrack::onTimestamp(const ExtendedTimestamp& timestamp __unused)
{
}

void MmapTrack::processMuteEvent_l(const sp<IAudioManager>& audioManager, mute_state_t muteState)
{
    if (mMuteState == muteState) {
        // mute state did not change, do nothing
        return;
    }

    status_t result = UNKNOWN_ERROR;
    if (audioManager && mPortId != AUDIO_PORT_HANDLE_NONE) {
        if (mMuteEventExtras == nullptr) {
            mMuteEventExtras = std::make_unique<os::PersistableBundle>();
        }
        mMuteEventExtras->putInt(String16(kExtraPlayerEventMuteKey),
                                 static_cast<int>(muteState));

        result = audioManager->portEvent(mPortId,
                                         PLAYER_UPDATE_MUTED,
                                         mMuteEventExtras);
    }

    if (result == OK) {
        mMuteState = muteState;
    } else {
        ALOGW("%s(%d): cannot process mute state for port ID %d, status error %d",
              __func__,
              id(),
              mPortId,
              result);
    }
}

void MmapTrack::appendDumpHeader(String8& result) const
{
    result.appendFormat("Client Session Port Id  Format Chn mask  SRate Flags %s\n",
                        isOut() ? "Usg CT": "Source");
}

void MmapTrack::appendDump(String8& result, bool active __unused) const
{
    result.appendFormat("%6u %7u %7u %08X %08X %6u 0x%03X ",
            mPid,
            mSessionId,
            mPortId,
            mFormat,
            mChannelMask,
            mSampleRate,
            mAttr.flags);
    if (isOut()) {
        result.appendFormat("%3x %2x", mAttr.usage, mAttr.content_type);
    } else {
        result.appendFormat("%6x", mAttr.source);
    }
    result.append("\n");
}

} // namespace android
