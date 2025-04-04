#define LOG_TAG "AudioFlinger"
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
#ifdef VERY_VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif
#define VALUE_OR_RETURN_BINDER_STATUS(x) \
    ({ \
       auto _tmp = (x); \
       if (!_tmp.ok()) return ::android::aidl_utils::binderStatusFromStatusT(_tmp.error()); \
       std::move(_tmp.value()); \
     })
namespace android {
using ::android::aidl_utils::binderStatusFromStatusT;
using binder::Status;
using content::AttributionSourceState;
using media::VolumeShaper;
#undef LOG_TAG
#define LOG_TAG "AF::TrackBase"
static volatile int32_t nextTrackId = 55;
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
    mUid = clientUid;
    size_t minBufferSize = buffer == NULL ? roundup(frameCount) : frameCount;
    if (minBufferSize < frameCount
            || mFrameSize == 0
            || minBufferSize > SIZE_MAX / mFrameSize) {
        android_errorWriteLog(0x534e4554, "34749571");
        return;
    }
    minBufferSize *= mFrameSize;
    if (buffer == nullptr) {
        bufferSize = minBufferSize;
    } else if (minBufferSize > bufferSize) {
        android_errorWriteLog(0x534e4554, "38340117");
        return;
    }
    size_t size = sizeof(audio_track_cblk_t);
    if (buffer == NULL && alloc == ALLOC_CBLK) {
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
            mBuffer = NULL;
            bufferSize = 0;
            break;
        case ALLOC_CBLK:
            if (buffer == NULL) {
                mBuffer = (char*)mCblk + sizeof(audio_track_cblk_t);
                memset(mBuffer, 0, bufferSize);
            } else {
                mBuffer = buffer;
#if 0
                mCblk->mFlags = CBLK_FORCEREADY;
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
        mState.setMirror(&mCblk->mState);
        static_assert(CBLK_STATE_IDLE == IDLE);
        static_assert(CBLK_STATE_PAUSING == PAUSING);
    }
}
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
    mServerProxy.clear();
    releaseCblk();
    mCblkMemory.clear();
    if (mClient != 0) {
        audio_utils::lock_guard _l(mClient->afClientCallback()->clientMutex());
        mClient.clear();
    }
    if (mAllocType == ALLOC_LOCAL) {
        free(mBuffer);
        mBuffer = nullptr;
    }
    IPCThreadState::self()->flushCommands();
}
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
        uint64_t mixBufferNs = ((uint64_t)2 * thread->frameCount() * 1000000000) /
                                              thread->sampleRate();
        setPeerTimeout(std::chrono::nanoseconds{mixBufferNs});
    }
}
void PatchTrackBase::setPeerTimeout(std::chrono::nanoseconds timeout) {
    mPeerTimeout.tv_sec = timeout.count() / std::nano::den;
    mPeerTimeout.tv_nsec = timeout.count() % std::nano::den;
}
#undef LOG_TAG
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
    mTrack->destroy();
}
Status TrackHandle::getCblk(
        std::optional<media::SharedFileRegion>* _aidl_return) {
    *_aidl_return = legacy2aidl_NullableIMemory_SharedFileRegion(mTrack->getCblk()).value();
    return Status::ok();
}
Status TrackHandle::start(int32_t* _aidl_return) {
    *_aidl_return = mTrack->start();
    return Status::ok();
}
Status TrackHandle::stop() {
    mTrack->stop();
    return Status::ok();
}
Status TrackHandle::flush() {
    mTrack->flush();
    return Status::ok();
}
Status TrackHandle::pause() {
    mTrack->pause();
    return Status::ok();
}
Status TrackHandle::attachAuxEffect(int32_t effectId,
                                                  int32_t* _aidl_return) {
    *_aidl_return = mTrack->attachAuxEffect(effectId);
    return Status::ok();
}
Status TrackHandle::setParameters(const std::string& keyValuePairs,
                                                int32_t* _aidl_return) {
    *_aidl_return = mTrack->setParameters(String8(keyValuePairs.c_str()));
    return Status::ok();
}
Status TrackHandle::selectPresentation(int32_t presentationId, int32_t programId,
                                                     int32_t* _aidl_return) {
    *_aidl_return = mTrack->selectPresentation(presentationId, programId);
    return Status::ok();
}
Status TrackHandle::getTimestamp(media::AudioTimestampInternal* timestamp,
                                               int32_t* _aidl_return) {
    AudioTimestamp legacy;
    *_aidl_return = mTrack->getTimestamp(legacy);
    if (*_aidl_return != OK) {
        return Status::ok();
    }
    *timestamp = legacy2aidl_AudioTimestamp_AudioTimestampInternal(legacy).value();
    return Status::ok();
}
Status TrackHandle::signal() {
    mTrack->signal();
    return Status::ok();
}
Status TrackHandle::applyVolumeShaper(
        const media::VolumeShaperConfiguration& configuration,
        const media::VolumeShaperOperation& operation,
        int32_t* _aidl_return) {
    sp<VolumeShaper::Configuration> conf = new VolumeShaper::Configuration();
    *_aidl_return = conf->readFromParcelable(configuration);
    if (*_aidl_return != OK) {
        return Status::ok();
    }
    sp<VolumeShaper::Operation> op = new VolumeShaper::Operation();
    *_aidl_return = op->readFromParcelable(operation);
    if (*_aidl_return != OK) {
        return Status::ok();
    }
    *_aidl_return = mTrack->applyVolumeShaper(conf, op);
    return Status::ok();
}
Status TrackHandle::getVolumeShaperState(
        int32_t id,
        std::optional<media::VolumeShaperState>* _aidl_return) {
    sp<VolumeShaper::State> legacy = mTrack->getVolumeShaperState(id);
    if (legacy == nullptr) {
        _aidl_return->reset();
        return Status::ok();
    }
    media::VolumeShaperState aidl;
    legacy->writeToParcelable(&aidl);
    *_aidl_return = aidl;
    return Status::ok();
}
Status TrackHandle::getDualMonoMode(
        media::audio::common::AudioDualMonoMode* _aidl_return)
{
    audio_dual_mono_mode_t mode = AUDIO_DUAL_MONO_MODE_OFF;
    const status_t status = mTrack->getDualMonoMode(&mode)
            ?: AudioValidator::validateDualMonoMode(mode);
    if (status == OK) {
        *_aidl_return = VALUE_OR_RETURN_BINDER_STATUS(
                legacy2aidl_audio_dual_mono_mode_t_AudioDualMonoMode(mode));
    }
    return binderStatusFromStatusT(status);
}
Status TrackHandle::setDualMonoMode(
        media::audio::common::AudioDualMonoMode mode)
{
    const auto localMonoMode = VALUE_OR_RETURN_BINDER_STATUS(
            aidl2legacy_AudioDualMonoMode_audio_dual_mono_mode_t(mode));
    return binderStatusFromStatusT(AudioValidator::validateDualMonoMode(localMonoMode)
            ?: mTrack->setDualMonoMode(localMonoMode));
}
Status TrackHandle::getAudioDescriptionMixLevel(float* _aidl_return)
{
    float leveldB = -std::numeric_limits<float>::infinity();
    const status_t status = mTrack->getAudioDescriptionMixLevel(&leveldB)
            ?: AudioValidator::validateAudioDescriptionMixLevel(leveldB);
    if (status == OK) *_aidl_return = leveldB;
    return binderStatusFromStatusT(status);
}
Status TrackHandle::setAudioDescriptionMixLevel(float leveldB)
{
    return binderStatusFromStatusT(AudioValidator::validateAudioDescriptionMixLevel(leveldB)
             ?: mTrack->setAudioDescriptionMixLevel(leveldB));
}
Status TrackHandle::getPlaybackRateParameters(
        media::audio::common::AudioPlaybackRate* _aidl_return)
{
    audio_playback_rate_t localPlaybackRate{};
    status_t status = mTrack->getPlaybackRateParameters(&localPlaybackRate)
            ?: AudioValidator::validatePlaybackRate(localPlaybackRate);
    if (status == NO_ERROR) {
        *_aidl_return = VALUE_OR_RETURN_BINDER_STATUS(
                legacy2aidl_audio_playback_rate_t_AudioPlaybackRate(localPlaybackRate));
    }
    return binderStatusFromStatusT(status);
}
Status TrackHandle::setPlaybackRateParameters(
        const media::audio::common::AudioPlaybackRate& playbackRate)
{
    const audio_playback_rate_t localPlaybackRate = VALUE_OR_RETURN_BINDER_STATUS(
            aidl2legacy_AudioPlaybackRate_audio_playback_rate_t(playbackRate));
    return binderStatusFromStatusT(AudioValidator::validatePlaybackRate(localPlaybackRate)
            ?: mTrack->setPlaybackRateParameters(localPlaybackRate));
}
sp<OpPlayAudioMonitor> OpPlayAudioMonitor::createIfNeeded(
            IAfThreadBase* thread,
            const AttributionSourceState& attributionSource, const audio_attributes_t& attr, int id,
            audio_stream_type_t streamType)
{
    Vector<String16> packages;
    const uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    getPackagesForUid(uid, packages);
    if (isServiceUid(uid)) {
        if (packages.isEmpty()) {
            ALOGW("OpPlayAudio: not muting track:%d usage:%d for service UID %d", id, attr.usage,
                  uid);
            return nullptr;
        }
    }
    if (streamType == AUDIO_STREAM_ENFORCED_AUDIBLE) {
        ALOGD("OpPlayAudio: not muting track:%d usage:%d ENFORCED_AUDIBLE", id, attr.usage);
        return nullptr;
    }
    if ((attr.flags & AUDIO_FLAG_BYPASS_INTERRUPTION_POLICY)
            == AUDIO_FLAG_BYPASS_INTERRUPTION_POLICY) {
        ALOGD("OpPlayAudio: not muting track:%d flags %#x have FLAG_BYPASS_INTERRUPTION_POLICY",
            id, attr.flags);
        return nullptr;
    }
    return sp<OpPlayAudioMonitor>::make(thread, attributionSource, attr.usage, id, uid);
}
OpPlayAudioMonitor::OpPlayAudioMonitor(IAfThreadBase* thread,
                                       const AttributionSourceState& attributionSource,
                                       audio_usage_t usage, int id, uid_t uid)
    : mThread(wp<IAfThreadBase>::fromExisting(thread)),
      mHasOpPlayAudio(true),
      mUsage((int32_t)usage),
      mId(id),
      mUid(uid),
      mPackageName(VALUE_OR_FATAL(aidl2legacy_string_view_String16(
                  attributionSource.packageName.value_or("")))) {}
OpPlayAudioMonitor::~OpPlayAudioMonitor()
{
    if (mOpCallback != 0) {
        mAppOpsManager.stopWatchingMode(mOpCallback);
    }
    mOpCallback.clear();
}
void OpPlayAudioMonitor::onFirstRef()
{
    checkPlayAudioForUsage( false);
    if (mPackageName.size()) {
        mOpCallback = new PlayAudioOpCallback(this);
        mAppOpsManager.startWatchingMode(AppOpsManager::OP_PLAY_AUDIO, mPackageName, mOpCallback);
    } else {
        ALOGW("Skipping OpPlayAudioMonitor due to null package name");
    }
}
bool OpPlayAudioMonitor::hasOpPlayAudio() const {
    return mHasOpPlayAudio.load();
}
void OpPlayAudioMonitor::checkPlayAudioForUsage(bool doBroadcast) {
    const bool hasAppOps =
            mPackageName.size() &&
            mAppOpsManager.checkAudioOpNoThrow(AppOpsManager::OP_PLAY_AUDIO, mUsage, mUid,
                                               mPackageName) == AppOpsManager::MODE_ALLOWED;
    bool shouldChange = !hasAppOps;
    if (mHasOpPlayAudio.compare_exchange_strong(shouldChange, hasAppOps)) {
        ALOGI("OpPlayAudio: track:%d package:%s usage:%d %smuted", mId,
              String8(mPackageName).c_str(), mUsage, hasAppOps ? "not " : "");
        if (doBroadcast) {
            auto thread = mThread.promote();
            if (thread != nullptr && thread->type() == IAfThreadBase::OFFLOAD) {
                audio_utils::lock_guard _l(thread->mutex());
                thread->broadcast_l();
            }
        }
    }
}
OpPlayAudioMonitor::PlayAudioOpCallback::PlayAudioOpCallback(
        const wp<OpPlayAudioMonitor>& monitor) : mMonitor(monitor)
{ }
void OpPlayAudioMonitor::PlayAudioOpCallback::opChanged(int32_t op,
            const String16& packageName) {
    if (op != AppOpsManager::OP_PLAY_AUDIO) {
        return;
    }
    ALOGI("%s OP_PLAY_AUDIO callback received for %s", __func__, String8(packageName).c_str());
    sp<OpPlayAudioMonitor> monitor = mMonitor.promote();
    if (monitor != NULL) {
        monitor->checkPlayAudioForUsage( true);
    }
}
void OpPlayAudioMonitor::getPackagesForUid(
    uid_t uid, Vector<String16>& packages)
{
    PermissionController permissionController;
    permissionController.getPackagesForUid(uid, packages);
}
#undef LOG_TAG
#define LOG_TAG "AF::Track"
sp<IAfTrack> IAfTrack::create(
        IAfPlaybackThread* thread,
        const sp<Client>& client,
        audio_stream_type_t streamType,
        const audio_attributes_t& attr,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        size_t frameCount,
        void *buffer,
        size_t bufferSize,
        const sp<IMemory>& sharedBuffer,
        audio_session_t sessionId,
        pid_t creatorPid,
        const AttributionSourceState& attributionSource,
        audio_output_flags_t flags,
        track_type type,
        audio_port_handle_t portId,
        size_t frameCountToBeReady,
        float speed,
        bool isSpatialized,
        bool isBitPerfect) {
    return sp<Track>::make(thread,
            client,
            streamType,
            attr,
            sampleRate,
            format,
            channelMask,
            frameCount,
            buffer,
            bufferSize,
            sharedBuffer,
            sessionId,
            creatorPid,
            attributionSource,
            flags,
            type,
            portId,
            frameCountToBeReady,
            speed,
            isSpatialized,
            isBitPerfect);
}
Track::Track(
        IAfPlaybackThread* thread,
            const sp<Client>& client,
            audio_stream_type_t streamType,
            const audio_attributes_t& attr,
            uint32_t sampleRate,
            audio_format_t format,
            audio_channel_mask_t channelMask,
            size_t frameCount,
            void *buffer,
            size_t bufferSize,
            const sp<IMemory>& sharedBuffer,
            audio_session_t sessionId,
            pid_t creatorPid,
            const AttributionSourceState& attributionSource,
            audio_output_flags_t flags,
            track_type type,
            audio_port_handle_t portId,
            size_t frameCountToBeReady,
            float speed,
            bool isSpatialized,
            bool isBitPerfect)
    : TrackBase(thread, client, attr, sampleRate, format, channelMask, frameCount,
                  (sharedBuffer != 0) ? sharedBuffer->unsecurePointer() : buffer,
                  (sharedBuffer != 0) ? sharedBuffer->size() : bufferSize,
                  sessionId, creatorPid,
                  VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid)), true ,
                  (type == TYPE_PATCH) ? ( buffer == NULL ? ALLOC_LOCAL : ALLOC_NONE) : ALLOC_CBLK,
                  type,
                  portId,
                  std::string(AMEDIAMETRICS_KEY_PREFIX_AUDIO_TRACK) + std::to_string(portId)),
    mFillingStatus(FS_INVALID),
    mSharedBuffer(sharedBuffer),
    mStreamType(streamType),
    mMainBuffer(thread->sinkBuffer()),
    mAuxBuffer(NULL),
    mAuxEffectId(0), mHasVolumeController(false),
    mFrameMap(16 ),
    mVolumeHandler(new media::VolumeHandler(sampleRate)),
    mOpPlayAudioMonitor(OpPlayAudioMonitor::createIfNeeded(thread, attributionSource, attr, id(),
        streamType)),
    mFastIndex(-1),
    mCachedVolume(1.0),
    mFinalVolume(0.f),
    mResumeToStopping(false),
    mFlushHwPending(false),
    mFlags(flags),
    mSpeed(speed),
    mIsSpatialized(isSpatialized),
    mIsBitPerfect(isBitPerfect)
{
    ALOG_ASSERT(!(client == 0 && sharedBuffer != 0));
    ALOGV_IF(sharedBuffer != 0, "%s(%d): sharedBuffer: %p, size: %zu",
            __func__, mId, sharedBuffer->unsecurePointer(), sharedBuffer->size());
    if (mCblk == NULL) {
        return;
    }
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    if (!thread->isTrackAllowed_l(channelMask, format, sessionId, uid)) {
        ALOGE("%s(%d): no more tracks available", __func__, mId);
        releaseCblk();
        return;
    }
    if (sharedBuffer == 0) {
        mAudioTrackServerProxy = new AudioTrackServerProxy(mCblk, mBuffer, frameCount,
                mFrameSize, !isExternalTrack(), sampleRate);
    } else {
        mAudioTrackServerProxy = new StaticAudioTrackServerProxy(mCblk, mBuffer, frameCount,
                mFrameSize, sampleRate);
    }
    mServerProxy = mAudioTrackServerProxy;
    mServerProxy->setStartThresholdInFrames(frameCountToBeReady);
    if (flags & AUDIO_OUTPUT_FLAG_FAST) {
        ALOG_ASSERT(thread->fastTrackAvailMask_l() != 0);
        const int i = __builtin_ctz(thread->fastTrackAvailMask_l());
        ALOG_ASSERT(0 < i && i < (int)FastMixerState::sMaxFastTracks);
        mFastIndex = i;
        thread->fastTrackAvailMask_l() &= ~(1 << i);
    }
    mServerLatencySupported = checkServerLatencySupported(format, flags);
#ifdef TEE_SINK
    mTee.setId(std::string("_") + std::to_string(mThreadIoHandle)
            + "_" + std::to_string(mId) + "_T");
#endif
    if (thread->supportsHapticPlayback()) {
        mAudioVibrationController = new AudioVibrationController(this);
        std::string packageName = attributionSource.packageName.has_value() ?
            attributionSource.packageName.value() : "";
        mExternalVibration = new os::ExternalVibration(
                mUid, packageName, mAttr, mAudioVibrationController);
    }
    const char * const traits = sharedBuffer == 0 ? "" : "static";
    mTrackMetrics.logConstructor(creatorPid, uid, id(), traits, streamType);
}
Track::~Track()
{
    ALOGV("%s(%d)", __func__, mId);
    if (mSharedBuffer != 0) {
        mSharedBuffer.clear();
    }
}
status_t Track::initCheck() const
{
    status_t status = TrackBase::initCheck();
    if (status == NO_ERROR && mCblk == nullptr) {
        status = NO_MEMORY;
    }
    return status;
}
void Track::destroy()
{
    sp<Track> keep(this);
    {
        bool wasActive = false;
        const sp<IAfThreadBase> thread = mThread.promote();
        if (thread != 0) {
            audio_utils::lock_guard _l(thread->mutex());
            auto* const playbackThread = thread->asIAfPlaybackThread().get();
            wasActive = playbackThread->destroyTrack_l(this);
            forEachTeePatchTrack_l([](const auto& patchTrack) { patchTrack->destroy(); });
        }
        if (isExternalTrack() && !wasActive) {
            AudioSystem::releaseOutput(mPortId);
        }
    }
}
void Track::appendDumpHeader(String8& result) const
{
    result.appendFormat("Type     Id Active Client Session Port Id S  Flags "
                        "  Format Chn mask  SRate "
                        "ST Usg CT "
                        " G db  L dB  R dB  VS dB "
                        "  Server FrmCnt  FrmRdy F Underruns  Flushed BitPerfect"
                        "%s\n",
                        isServerLatencySupported() ? "   Latency" : "");
}
void Track::appendDump(String8& result, bool active) const
{
    char trackType;
    switch (mType) {
    case TYPE_DEFAULT:
    case TYPE_OUTPUT:
        if (isStatic()) {
            trackType = 'S';
        } else {
            trackType = ' ';
        }
        break;
    case TYPE_PATCH:
        trackType = 'P';
        break;
    default:
        trackType = '?';
    }
    if (isFastTrack()) {
        result.appendFormat("F%d %c %6d", mFastIndex, trackType, mId);
    } else {
        result.appendFormat("   %c %6d", trackType, mId);
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
    char fillingStatus;
    switch (mFillingStatus) {
    case FS_INVALID:
        fillingStatus = 'I';
        break;
    case FS_FILLING:
        fillingStatus = 'f';
        break;
    case FS_FILLED:
        fillingStatus = 'F';
        break;
    case FS_ACTIVE:
        fillingStatus = 'A';
        break;
    default:
        fillingStatus = '?';
        break;
    }
    const size_t framesReadySafe =
            std::min(mAudioTrackServerProxy->framesReadySafe(), (size_t)99999999);
    const gain_minifloat_packed_t vlr = mAudioTrackServerProxy->getVolumeLR();
    const std::pair<float , bool > vsVolume =
            mVolumeHandler->getLastVolume();
    const size_t bufferSizeInFrames = (size_t)mAudioTrackServerProxy->getBufferSizeInFrames();
    const char modifiedBufferChar = bufferSizeInFrames < mFrameCount
            ? 'r' : bufferSizeInFrames > mFrameCount
                    ? 'e' : ' ' ;
    result.appendFormat("%7s %6u %7u %7u %2s 0x%03X "
                        "%08X %08X %6u "
                        "%2u %3x %2x "
                        "%5.2g %5.2g %5.2g %5.2g%c "
                        "%08X %6zu%c %6zu %c %9u%c %7u %10s",
            active ? "yes" : "no",
            (mClient == 0) ? getpid() : mClient->pid(),
            mSessionId,
            mPortId,
            getTrackStateAsCodedString(),
            mCblk->mFlags,
            mFormat,
            mChannelMask,
            sampleRate(),
            mStreamType,
            mAttr.usage,
            mAttr.content_type,
            20.0 * log10(mFinalVolume),
            20.0 * log10(float_from_gain(gain_minifloat_unpack_left(vlr))),
            20.0 * log10(float_from_gain(gain_minifloat_unpack_right(vlr))),
            20.0 * log10(vsVolume.first),
            vsVolume.second ? 'A' : ' ',
            mCblk->mServer,
            bufferSizeInFrames,
            modifiedBufferChar,
            framesReadySafe,
            fillingStatus,
            mAudioTrackServerProxy->getUnderrunFrames(),
            nowInUnderrun,
            (unsigned)mAudioTrackServerProxy->framesFlushed() % 10000000,
            isBitPerfect() ? "true" : "false"
            );
    if (isServerLatencySupported()) {
        double latencyMs;
        bool fromTrack;
        if (getTrackLatencyMs(&latencyMs, &fromTrack) == OK) {
            result.appendFormat(" %7.2lf %c", latencyMs, fromTrack ? 't' : 'k');
        } else {
            result.appendFormat("%10s", mCblk->mServer != 0 ? "unavail" : "new");
        }
    }
    result.append("\n");
}
uint32_t Track::sampleRate() const {
    return mAudioTrackServerProxy->getSampleRate();
}
status_t Track::getNextBuffer(AudioBufferProvider::Buffer* buffer)
{
    ServerProxy::Buffer buf;
    size_t desiredFrames = buffer->frameCount;
    buf.mFrameCount = desiredFrames;
    status_t status = mServerProxy->obtainBuffer(&buf);
    buffer->frameCount = buf.mFrameCount;
    buffer->raw = buf.mRaw;
    if (buf.mFrameCount == 0 && !isStopping() && !isStopped() && !isPaused() && !isOffloaded()) {
        ALOGV("%s(%d): underrun,  framesReady(%zu) < framesDesired(%zd), state: %d",
                __func__, mId, buf.mFrameCount, desiredFrames, (int)mState);
        mAudioTrackServerProxy->tallyUnderrunFrames(desiredFrames);
    } else {
        mAudioTrackServerProxy->tallyUnderrunFrames(0);
    }
    return status;
}
void Track::releaseBuffer(AudioBufferProvider::Buffer* buffer)
{
    interceptBuffer(*buffer);
    TrackBase::releaseBuffer(buffer);
}
void Track::interceptBuffer(
        const AudioBufferProvider::Buffer& sourceBuffer) {
    auto start = std::chrono::steady_clock::now();
    const size_t frameCount = sourceBuffer.frameCount;
    if (frameCount == 0) {
        return;
    }
    for (auto& teePatch : mTeePatches) {
        IAfPatchRecord* patchRecord = teePatch.patchRecord.get();
        const size_t framesWritten = patchRecord->writeFrames(
                sourceBuffer.i8, frameCount, mFrameSize);
        const size_t framesLeft = frameCount - framesWritten;
        ALOGW_IF(framesLeft != 0, "%s(%d) PatchRecord %d can not provide big enough "
                 "buffer %zu/%zu, dropping %zu frames", __func__, mId, patchRecord->id(),
                 framesWritten, frameCount, framesLeft);
    }
    auto spent = ceil<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
    using namespace std::chrono_literals;
    ALOGD_IF(spent > 500us, "%s: took %lldus to intercept %zu tracks", __func__,
             spent.count(), mTeePatches.size());
}
size_t Track::framesReady() const {
    if (mSharedBuffer != 0 && (isStopped() || isStopping())) {
        return 0;
    }
    return mAudioTrackServerProxy->framesReady();
}
int64_t Track::framesReleased() const
{
    return mAudioTrackServerProxy->framesReleased();
}
void Track::onTimestamp(const ExtendedTimestamp &timestamp)
{
    mAudioTrackServerProxy->setTimestamp(timestamp);
    const double latencyMs = timestamp.getOutputServerLatencyMs(sampleRate());
    mServerLatencyFromTrack.store(true);
    mServerLatencyMs.store(latencyMs);
}
bool Track::isReady() const {
    if (mFillingStatus != FS_FILLING || isStopped() || isPausing()) {
        return true;
    }
    if (isStopping()) {
        if (framesReady() > 0) {
            mFillingStatus = FS_FILLED;
        }
        return true;
    }
    size_t bufferSizeInFrames = mServerProxy->getBufferSizeInFrames();
    const size_t startThresholdInFrames = mServerProxy->getStartThresholdInFrames();
    const size_t framesToBeReady = std::clamp(
            std::min(startThresholdInFrames, bufferSizeInFrames), size_t(1), mFrameCount);
    if (framesReady() >= framesToBeReady || (mCblk->mFlags & CBLK_FORCEREADY)) {
        ALOGV("%s(%d): consider track ready with %zu/%zu, target was %zu)",
              __func__, mId, framesReady(), bufferSizeInFrames, framesToBeReady);
        mFillingStatus = FS_FILLED;
        android_atomic_and(~CBLK_FORCEREADY, &mCblk->mFlags);
        return true;
    }
    return false;
}
status_t Track::start(AudioSystem::sync_event_t event __unused,
                                                    audio_session_t triggerSession __unused)
{
    status_t status = NO_ERROR;
    ALOGV("%s(%d): calling pid %d session %d",
            __func__, mId, IPCThreadState::self()->getCallingPid(), mSessionId);
    const sp<IAfThreadBase> thread = mThread.promote();
    if (thread != 0) {
        if (isOffloaded()) {
            audio_utils::lock_guard _laf(thread->afThreadCallback()->mutex());
            const bool nonOffloadableGlobalEffectEnabled =
                    thread->afThreadCallback()->isNonOffloadableGlobalEffectEnabled_l();
            audio_utils::lock_guard _lth(thread->mutex());
            sp<IAfEffectChain> ec = thread->getEffectChain_l(mSessionId);
            if (nonOffloadableGlobalEffectEnabled ||
                    (ec != 0 && ec->isNonOffloadableEnabled())) {
                invalidate();
                return PERMISSION_DENIED;
            }
        }
        audio_utils::lock_guard _lth(thread->mutex());
        track_state state = mState;
        if (state == FLUSHED) {
            reset();
        }
        mPauseHwPending = false;
        if (state == PAUSED || state == PAUSING) {
            if (mResumeToStopping) {
                mState = TrackBase::STOPPING_1;
                ALOGV("%s(%d): PAUSED => STOPPING_1 on thread %d",
                        __func__, mId, (int)mThreadIoHandle);
            } else {
                mState = TrackBase::RESUMING;
                ALOGV("%s(%d): PAUSED => RESUMING on thread %d",
                        __func__, mId, (int)mThreadIoHandle);
            }
        } else {
            mState = TrackBase::ACTIVE;
            ALOGV("%s(%d): ? => ACTIVE on thread %d",
                    __func__, mId, (int)mThreadIoHandle);
        }
        auto* const playbackThread = thread->asIAfPlaybackThread().get();
        if (audio_is_linear_pcm(mFormat)
                && (state == IDLE || state == STOPPED || state == FLUSHED)) {
            mFrameMap.reset();
            if (!isFastTrack() && (isDirect() || isOffloaded())) {
                mFrameMap.push(mAudioTrackServerProxy->framesReleased(),
                               playbackThread->framesWritten());
            }
        }
        if (isFastTrack()) {
            mObservedUnderruns = playbackThread->getFastTrackUnderruns(mFastIndex);
        }
        status = playbackThread->addTrack_l(this);
        if (status == INVALID_OPERATION || status == PERMISSION_DENIED || status == DEAD_OBJECT) {
            triggerEvents(AudioSystem::SYNC_EVENT_PRESENTATION_COMPLETE);
            if (status == PERMISSION_DENIED || status == DEAD_OBJECT) {
                mState = state;
            }
        }
        {
            mLogStartCountdown = LOG_START_COUNTDOWN;
            mLogStartTimeNs = systemTime();
            mLogStartFrames = mAudioTrackServerProxy->getTimestamp()
                    .mPosition[ExtendedTimestamp::LOCATION_KERNEL];
            mLogLatencyMs = 0.;
        }
        mLogForceVolumeUpdate = true;
        if (status == NO_ERROR || status == ALREADY_EXISTS) {
            mAudioTrackServerProxy->start();
        }
        if (status == ALREADY_EXISTS) {
            status = NO_ERROR;
        } else {
            ServerProxy::Buffer buffer;
            buffer.mFrameCount = 1;
            (void) mAudioTrackServerProxy->obtainBuffer(&buffer, true );
        }
        if (status == NO_ERROR) {
            forEachTeePatchTrack_l([](const auto& patchTrack) { patchTrack->start(); });
        }
    } else {
        status = BAD_VALUE;
    }
    if (status == NO_ERROR) {
        const sp<IAudioManager> audioManager =
                thread->afThreadCallback()->getOrCreateAudioManager();
        if (audioManager && mPortId != AUDIO_PORT_HANDLE_NONE) {
            std::unique_ptr<os::PersistableBundle> bundle =
                    std::make_unique<os::PersistableBundle>();
        bundle->putBoolean(String16(kExtraPlayerEventSpatializedKey),
                           isSpatialized());
        bundle->putInt(String16(kExtraPlayerEventSampleRateKey), mSampleRate);
        bundle->putInt(String16(kExtraPlayerEventChannelMaskKey), mChannelMask);
        status_t result = audioManager->portEvent(mPortId,
                                                  PLAYER_UPDATE_FORMAT, bundle);
        if (result != OK) {
            ALOGE("%s: unable to send playback format for port ID %d, status error %d",
                  __func__, mPortId, result);
        }
      }
    }
    return status;
}
void Track::stop()
{
    ALOGV("%s(%d): calling pid %d", __func__, mId, IPCThreadState::self()->getCallingPid());
    const sp<IAfThreadBase> thread = mThread.promote();
    if (thread != 0) {
        audio_utils::lock_guard _l(thread->mutex());
        track_state state = mState;
        if (state == RESUMING || state == ACTIVE || state == PAUSING || state == PAUSED) {
            auto* const playbackThread = thread->asIAfPlaybackThread().get();
            if (!playbackThread->isTrackActive(this)) {
                reset();
                mState = STOPPED;
            } else if (!isFastTrack() && !isOffloaded() && !isDirect()) {
                mState = STOPPED;
            } else {
                mState = STOPPING_1;
                if (isOffloaded()) {
                    mRetryCount = IAfPlaybackThread::kMaxTrackStopRetriesOffload;
                }
            }
            playbackThread->broadcast_l();
            ALOGV("%s(%d): not stopping/stopped => stopping/stopped on thread %d",
                    __func__, mId, (int)mThreadIoHandle);
        }
        forEachTeePatchTrack_l([](const auto& patchTrack) { patchTrack->stop(); });
    }
}
void Track::pause()
{
    ALOGV("%s(%d): calling pid %d", __func__, mId, IPCThreadState::self()->getCallingPid());
    const sp<IAfThreadBase> thread = mThread.promote();
    if (thread != 0) {
        audio_utils::lock_guard _l(thread->mutex());
        auto* const playbackThread = thread->asIAfPlaybackThread().get();
        switch (mState) {
        case STOPPING_1:
        case STOPPING_2:
            if (!isOffloaded()) {
                break;
            }
            mResumeToStopping = true;
            FALLTHROUGH_INTENDED;
        case ACTIVE:
        case RESUMING:
            mState = PAUSING;
            ALOGV("%s(%d): ACTIVE/RESUMING => PAUSING on thread %d",
                    __func__, mId, (int)mThreadIoHandle);
            if (isOffloadedOrDirect()) {
                mPauseHwPending = true;
            }
            playbackThread->broadcast_l();
            break;
        default:
            break;
        }
        forEachTeePatchTrack_l([](const auto& patchTrack) { patchTrack->pause(); });
    }
}
void Track::flush()
{
    ALOGV("%s(%d)", __func__, mId);
    const sp<IAfThreadBase> thread = mThread.promote();
    if (thread != 0) {
        audio_utils::lock_guard _l(thread->mutex());
        auto* const playbackThread = thread->asIAfPlaybackThread().get();
        if (!playbackThread->isTrackActive(this)) {
            (void)mServerProxy->flushBufferIfNeeded();
        }
        if (isOffloaded()) {
            if (isTerminated()) {
                return;
            }
            ALOGV("%s(%d): offload flush", __func__, mId);
            reset();
            if (mState == STOPPING_1 || mState == STOPPING_2) {
                ALOGV("%s(%d): flushed in STOPPING_1 or 2 state, change state to ACTIVE",
                        __func__, mId);
                mState = ACTIVE;
            }
            mFlushHwPending = true;
            mResumeToStopping = false;
        } else {
            if (mState != STOPPING_1 && mState != STOPPING_2 && mState != STOPPED &&
                    mState != PAUSED && mState != PAUSING && mState != IDLE && mState != FLUSHED) {
                return;
            }
            mState = FLUSHED;
            if (isDirect()) {
                mFlushHwPending = true;
            }
            if (!playbackThread->isTrackActive(this)) {
                reset();
            }
        }
        playbackThread->broadcast_l();
        forEachTeePatchTrack_l([](const auto& patchTrack) { patchTrack->flush(); });
    }
}
void Track::flushAck()
{
    if (!isOffloaded() && !isDirect()) {
        return;
    }
    mServerProxy->flushBufferIfNeeded();
    mFlushHwPending = false;
}
void Track::pauseAck()
{
    mPauseHwPending = false;
}
void Track::reset()
{
    if (!mResetDone) {
        android_atomic_and(~CBLK_FORCEREADY, &mCblk->mFlags);
        mFillingStatus = FS_FILLING;
        mResetDone = true;
        if (mState == FLUSHED) {
            mState = IDLE;
        }
    }
}
status_t Track::setParameters(const String8& keyValuePairs)
{
    const sp<IAfThreadBase> thread = mThread.promote();
    if (thread == 0) {
        ALOGE("%s(%d): thread is dead", __func__, mId);
        return FAILED_TRANSACTION;
    } else if (thread->type() == IAfThreadBase::DIRECT
            || thread->type() == IAfThreadBase::OFFLOAD) {
        return thread->setParameters(keyValuePairs);
    } else {
        return PERMISSION_DENIED;
    }
}
status_t Track::selectPresentation(int presentationId,
        int programId) {
    const sp<IAfThreadBase> thread = mThread.promote();
    if (thread == 0) {
        ALOGE("thread is dead");
        return FAILED_TRANSACTION;
    } else if (thread->type() == IAfThreadBase::DIRECT
            || thread->type() == IAfThreadBase::OFFLOAD) {
        auto directOutputThread = thread->asIAfDirectOutputThread().get();
        return directOutputThread->selectPresentation(presentationId, programId);
    }
    return INVALID_OPERATION;
}
VolumeShaper::Status Track::applyVolumeShaper(
        const sp<VolumeShaper::Configuration>& configuration,
        const sp<VolumeShaper::Operation>& operation)
{
    VolumeShaper::Status status = mVolumeHandler->applyVolumeShaper(configuration, operation);
    if (isOffloadedOrDirect()) {
        const sp<IAfThreadBase> thread = mThread.promote();
        if (thread != 0) {
            audio_utils::lock_guard _l(thread->mutex());
            thread->broadcast_l();
        }
    }
    return status;
}
sp<VolumeShaper::State> Track::getVolumeShaperState(int id) const
{
    return mVolumeHandler->getVolumeShaperState(id);
}
void Track::setFinalVolume(float volumeLeft, float volumeRight)
{
    mFinalVolumeLeft = volumeLeft;
    mFinalVolumeRight = volumeRight;
    const float volume = (volumeLeft + volumeRight) * 0.5f;
    if (mFinalVolume != volume) {
        mFinalVolume = volume;
        setMetadataHasChanged();
        mLogForceVolumeUpdate = true;
    }
    if (mLogForceVolumeUpdate) {
        mLogForceVolumeUpdate = false;
        mTrackMetrics.logVolume(mFinalVolume);
    }
}
void Track::copyMetadataTo(MetadataInserter& backInserter) const
{
    if (mStreamType == AUDIO_STREAM_PATCH) {
        return;
    }
    playback_track_metadata_v7_t metadata;
    metadata.base = {
            .usage = mAttr.usage,
            .content_type = mAttr.content_type,
            .gain = mFinalVolume,
    };
    if (mAttr.usage == AUDIO_USAGE_UNKNOWN) {
        switch (mStreamType) {
        case AUDIO_STREAM_VOICE_CALL:
            metadata.base.usage = AUDIO_USAGE_VOICE_COMMUNICATION;
            metadata.base.content_type = AUDIO_CONTENT_TYPE_SPEECH;
            break;
        case AUDIO_STREAM_SYSTEM:
            metadata.base.usage = AUDIO_USAGE_ASSISTANCE_SONIFICATION;
            metadata.base.content_type = AUDIO_CONTENT_TYPE_SONIFICATION;
            break;
        case AUDIO_STREAM_RING:
            metadata.base.usage = AUDIO_USAGE_NOTIFICATION_TELEPHONY_RINGTONE;
            metadata.base.content_type = AUDIO_CONTENT_TYPE_SONIFICATION;
            break;
        case AUDIO_STREAM_MUSIC:
            metadata.base.usage = AUDIO_USAGE_MEDIA;
            metadata.base.content_type = AUDIO_CONTENT_TYPE_MUSIC;
            break;
        case AUDIO_STREAM_ALARM:
            metadata.base.usage = AUDIO_USAGE_ALARM;
            metadata.base.content_type = AUDIO_CONTENT_TYPE_SONIFICATION;
            break;
        case AUDIO_STREAM_NOTIFICATION:
            metadata.base.usage = AUDIO_USAGE_NOTIFICATION;
            metadata.base.content_type = AUDIO_CONTENT_TYPE_SONIFICATION;
            break;
        case AUDIO_STREAM_DTMF:
            metadata.base.usage = AUDIO_USAGE_VOICE_COMMUNICATION_SIGNALLING;
            metadata.base.content_type = AUDIO_CONTENT_TYPE_SONIFICATION;
            break;
        case AUDIO_STREAM_ACCESSIBILITY:
            metadata.base.usage = AUDIO_USAGE_ASSISTANCE_ACCESSIBILITY;
            metadata.base.content_type = AUDIO_CONTENT_TYPE_SPEECH;
            break;
        case AUDIO_STREAM_ASSISTANT:
            metadata.base.usage = AUDIO_USAGE_ASSISTANT;
            metadata.base.content_type = AUDIO_CONTENT_TYPE_SPEECH;
            break;
        case AUDIO_STREAM_REROUTING:
            metadata.base.usage = AUDIO_USAGE_VIRTUAL_SOURCE;
            break;
        case AUDIO_STREAM_CALL_ASSISTANT:
            metadata.base.usage = AUDIO_USAGE_CALL_ASSISTANT;
            metadata.base.content_type = AUDIO_CONTENT_TYPE_SPEECH;
            break;
        default:
            break;
        }
    }
    metadata.channel_mask = mChannelMask;
    strncpy(metadata.tags, mAttr.tags, AUDIO_ATTRIBUTES_TAGS_MAX_SIZE);
    *backInserter++ = metadata;
}
void Track::updateTeePatches_l() {
    if (mTeePatchesToUpdate.has_value()) {
        forEachTeePatchTrack_l([](const auto& patchTrack) { patchTrack->destroy(); });
        mTeePatches = mTeePatchesToUpdate.value();
        if (mState == TrackBase::ACTIVE || mState == TrackBase::RESUMING ||
                mState == TrackBase::STOPPING_1) {
            forEachTeePatchTrack_l([](const auto& patchTrack) { patchTrack->start(); });
        }
        mTeePatchesToUpdate.reset();
    }
}
void Track::setTeePatchesToUpdate_l(TeePatches teePatchesToUpdate) {
    ALOGW_IF(mTeePatchesToUpdate.has_value(),
             "%s, existing tee patches to update will be ignored", __func__);
    mTeePatchesToUpdate = std::move(teePatchesToUpdate);
}
void Track::processMuteEvent_l(const sp<
    IAudioManager>& audioManager, mute_state_t muteState)
{
    if (mMuteState == muteState) {
        return;
    }
    status_t result = UNKNOWN_ERROR;
    if (audioManager && mPortId != AUDIO_PORT_HANDLE_NONE) {
        if (mMuteEventExtras == nullptr) {
            mMuteEventExtras = std::make_unique<os::PersistableBundle>();
        }
        mMuteEventExtras->putInt(String16(kExtraPlayerEventMuteKey), static_cast<int>(muteState));
        result = audioManager->portEvent(mPortId, PLAYER_UPDATE_MUTED, mMuteEventExtras);
    }
    if (result == OK) {
        ALOGI("%s(%d): processed mute state for port ID %d from %d to %d", __func__, id(), mPortId,
              int(muteState), int(mMuteState));
        mMuteState = muteState;
    } else {
        ALOGW("%s(%d): cannot process mute state for port ID %d, status error %d", __func__, id(),
              mPortId, result);
    }
}
status_t Track::getTimestamp(AudioTimestamp& timestamp)
{
    if (!isOffloaded() && !isDirect()) {
        return INVALID_OPERATION;
    }
    const sp<IAfThreadBase> thread = mThread.promote();
    if (thread == 0) {
        return INVALID_OPERATION;
    }
    audio_utils::lock_guard _l(thread->mutex());
    auto* const playbackThread = thread->asIAfPlaybackThread().get();
    return playbackThread->getTimestamp_l(timestamp);
}
status_t Track::attachAuxEffect(int EffectId)
{
    const sp<IAfThreadBase> thread = mThread.promote();
    if (thread == nullptr) {
        return DEAD_OBJECT;
    }
    auto dstThread = thread->asIAfPlaybackThread();
    sp<IAfPlaybackThread> srcThread;
    const auto& af = mClient->afClientCallback();
    status_t status = af->moveAuxEffectToIo(EffectId, dstThread, &srcThread);
    if (EffectId != 0 && status == NO_ERROR) {
        status = dstThread->attachAuxEffect(this, EffectId);
        if (status == NO_ERROR) {
            AudioSystem::moveEffectsToIo(std::vector<int>(EffectId), dstThread->id());
        }
    }
    if (status != NO_ERROR && srcThread != nullptr) {
        af->moveAuxEffectToIo(EffectId, srcThread, &dstThread);
    }
    return status;
}
void Track::setAuxBuffer(int EffectId, int32_t *buffer)
{
    mAuxEffectId = EffectId;
    mAuxBuffer = buffer;
}
bool Track::presentationComplete(
        int64_t framesWritten, size_t audioHalFrames)
{
    ALOGV("%s(%d): presentationComplete() mPresentationCompleteFrames %lld framesWritten %lld",
            __func__, mId,
            (long long)mPresentationCompleteFrames, (long long)framesWritten);
    if (mPresentationCompleteFrames == 0) {
        mPresentationCompleteFrames = framesWritten + audioHalFrames;
        ALOGV("%s(%d): set:"
                " mPresentationCompleteFrames %lld audioHalFrames %zu",
                __func__, mId,
                (long long)mPresentationCompleteFrames, audioHalFrames);
    }
    bool complete;
    if (isFastTrack()) {
        complete = framesWritten >= (int64_t) mPresentationCompleteFrames;
        ALOGV("%s(%d): %s framesWritten:%lld  mPresentationCompleteFrames:%lld",
                __func__, mId, (complete ? "complete" : "waiting"),
                (long long) framesWritten, (long long) mPresentationCompleteFrames);
    } else {
        complete = framesWritten >= (int64_t) mPresentationCompleteFrames
                && mAudioTrackServerProxy->isDrained();
    }
    if (complete) {
        notifyPresentationComplete();
        return true;
    }
    return false;
}
bool Track::presentationComplete(uint32_t latencyMs)
{
    constexpr float MIN_SPEED = 0.125f;
    if (mPresentationCompleteTimeNs == 0) {
        mPresentationCompleteTimeNs = systemTime() + latencyMs * 1e6 / fmax(mSpeed, MIN_SPEED);
        ALOGV("%s(%d): set: latencyMs %u  mPresentationCompleteTimeNs:%lld",
                __func__, mId, latencyMs, (long long) mPresentationCompleteTimeNs);
    }
    bool complete;
    if (isOffloaded()) {
        complete = true;
    } else {
        complete = systemTime() >= mPresentationCompleteTimeNs;
        ALOGV("%s(%d): %s", __func__, mId, (complete ? "complete" : "waiting"));
    }
    if (complete) {
        notifyPresentationComplete();
        return true;
    }
    return false;
}
void Track::notifyPresentationComplete()
{
    triggerEvents(AudioSystem::SYNC_EVENT_PRESENTATION_COMPLETE);
    mAudioTrackServerProxy->setStreamEndDone();
}
void Track::triggerEvents(AudioSystem::sync_event_t type)
{
    for (auto it = mSyncEvents.begin(); it != mSyncEvents.end();) {
        if ((*it)->type() == type) {
            ALOGV("%s: triggering SyncEvent type %d", __func__, type);
            (*it)->trigger();
            it = mSyncEvents.erase(it);
        } else {
            ++it;
        }
    }
}
gain_minifloat_packed_t Track::getVolumeLR() const
{
    ALOG_ASSERT(isFastTrack() && (mCblk != NULL));
    gain_minifloat_packed_t vlr = mAudioTrackServerProxy->getVolumeLR();
    float vl = float_from_gain(gain_minifloat_unpack_left(vlr));
    float vr = float_from_gain(gain_minifloat_unpack_right(vlr));
    if (vl > GAIN_FLOAT_UNITY) {
        vl = GAIN_FLOAT_UNITY;
    }
    if (vr > GAIN_FLOAT_UNITY) {
        vr = GAIN_FLOAT_UNITY;
    }
    float v = mCachedVolume;
    vl *= v;
    vr *= v;
    vlr = gain_minifloat_pack(gain_from_float(vl), gain_from_float(vr));
    return vlr;
}
status_t Track::setSyncEvent(
        const sp<audioflinger::SyncEvent>& event)
{
    if (isTerminated() || mState == PAUSED ||
            ((framesReady() == 0) && ((mSharedBuffer != 0) ||
                                      (mState == STOPPED)))) {
        ALOGW("%s(%d): in invalid state %d on session %d %s mode, framesReady %zu",
              __func__, mId,
              (int)mState, mSessionId, (mSharedBuffer != 0) ? "static" : "stream", framesReady());
        event->cancel();
        return INVALID_OPERATION;
    }
    (void) TrackBase::setSyncEvent(event);
    return NO_ERROR;
}
void Track::invalidate()
{
    TrackBase::invalidate();
    signalClientFlag(CBLK_INVALID);
}
void Track::disable()
{
    signalClientFlag(CBLK_DISABLED);
}
void Track::signalClientFlag(int32_t flag)
{
    audio_track_cblk_t* cblk = mCblk;
    android_atomic_or(flag, &cblk->mFlags);
    android_atomic_release_store(0x40000000, &cblk->mFutex);
    (void) syscall(__NR_futex, &cblk->mFutex, FUTEX_WAKE, INT_MAX);
}
void Track::signal()
{
    const sp<IAfThreadBase> thread = mThread.promote();
    if (thread != 0) {
        auto* const t = thread->asIAfPlaybackThread().get();
        audio_utils::lock_guard _l(t->mutex());
        t->broadcast_l();
    }
}
status_t Track::getDualMonoMode(audio_dual_mono_mode_t* mode) const
{
    status_t status = INVALID_OPERATION;
    if (isOffloadedOrDirect()) {
        const sp<IAfThreadBase> thread = mThread.promote();
        if (thread != nullptr) {
            auto* const t = thread->asIAfPlaybackThread().get();
            audio_utils::lock_guard _l(t->mutex());
            status = t->getOutput_l()->stream->getDualMonoMode(mode);
            ALOGD_IF((status == NO_ERROR) && (mDualMonoMode != *mode),
                    "%s: mode %d inconsistent", __func__, mDualMonoMode);
        }
    }
    return status;
}
status_t Track::setDualMonoMode(audio_dual_mono_mode_t mode)
{
    status_t status = INVALID_OPERATION;
    if (isOffloadedOrDirect()) {
        const sp<IAfThreadBase> thread = mThread.promote();
        if (thread != nullptr) {
            auto* const t = thread->asIAfPlaybackThread().get();
            audio_utils::lock_guard lock(t->mutex());
            status = t->getOutput_l()->stream->setDualMonoMode(mode);
            if (status == NO_ERROR) {
                mDualMonoMode = mode;
            }
        }
    }
    return status;
}
status_t Track::getAudioDescriptionMixLevel(float* leveldB) const
{
    status_t status = INVALID_OPERATION;
    if (isOffloadedOrDirect()) {
        sp<IAfThreadBase> thread = mThread.promote();
        if (thread != nullptr) {
            auto* const t = thread->asIAfPlaybackThread().get();
            audio_utils::lock_guard lock(t->mutex());
            status = t->getOutput_l()->stream->getAudioDescriptionMixLevel(leveldB);
            ALOGD_IF((status == NO_ERROR) && (mAudioDescriptionMixLevel != *leveldB),
                    "%s: level %.3f inconsistent", __func__, mAudioDescriptionMixLevel);
        }
    }
    return status;
}
status_t Track::setAudioDescriptionMixLevel(float leveldB)
{
    status_t status = INVALID_OPERATION;
    if (isOffloadedOrDirect()) {
        const sp<IAfThreadBase> thread = mThread.promote();
        if (thread != nullptr) {
            auto* const t = thread->asIAfPlaybackThread().get();
            audio_utils::lock_guard lock(t->mutex());
            status = t->getOutput_l()->stream->setAudioDescriptionMixLevel(leveldB);
            if (status == NO_ERROR) {
                mAudioDescriptionMixLevel = leveldB;
            }
        }
    }
    return status;
}
status_t Track::getPlaybackRateParameters(
        audio_playback_rate_t* playbackRate) const
{
    status_t status = INVALID_OPERATION;
    if (isOffloadedOrDirect()) {
        const sp<IAfThreadBase> thread = mThread.promote();
        if (thread != nullptr) {
            auto* const t = thread->asIAfPlaybackThread().get();
            audio_utils::lock_guard lock(t->mutex());
            status = t->getOutput_l()->stream->getPlaybackRateParameters(playbackRate);
            ALOGD_IF((status == NO_ERROR) &&
                    !isAudioPlaybackRateEqual(mPlaybackRateParameters, *playbackRate),
                    "%s: playbackRate inconsistent", __func__);
        }
    }
    return status;
}
status_t Track::setPlaybackRateParameters(
        const audio_playback_rate_t& playbackRate)
{
    status_t status = INVALID_OPERATION;
    if (isOffloadedOrDirect()) {
        const sp<IAfThreadBase> thread = mThread.promote();
        if (thread != nullptr) {
            auto* const t = thread->asIAfPlaybackThread().get();
            audio_utils::lock_guard lock(t->mutex());
            status = t->getOutput_l()->stream->setPlaybackRateParameters(playbackRate);
            if (status == NO_ERROR) {
                mPlaybackRateParameters = playbackRate;
            }
        }
    }
    return status;
}
bool Track::isResumePending() const {
    if (mState == RESUMING) {
        return true;
    }
    if (mState == STOPPING_1 &&
        mResumeToStopping) {
        return true;
    }
    return false;
}
void Track::resumeAck() {
    if (mState == RESUMING) {
        mState = ACTIVE;
    }
    if (mState == STOPPING_1) {
        mResumeToStopping = false;
    }
}
void Track::updateTrackFrameInfo(
        int64_t trackFramesReleased, int64_t sinkFramesWritten,
        uint32_t halSampleRate, const ExtendedTimestamp &timeStamp) {
    const FrameTime ft{
            timeStamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL],
            timeStamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL]};
    mKernelFrameTime.store(ft);
    if (!audio_is_linear_pcm(mFormat)) {
        return;
    }
    mFrameMap.push(trackFramesReleased, sinkFramesWritten);
    ExtendedTimestamp local = timeStamp;
    bool drained = true;
    bool checked = false;
    for (int i = ExtendedTimestamp::LOCATION_MAX - 1;
            i >= ExtendedTimestamp::LOCATION_SERVER; --i) {
        if (local.mTimeNs[i] > 0) {
            local.mPosition[i] = mFrameMap.findX(local.mPosition[i]);
            if (!checked && i <= ExtendedTimestamp::LOCATION_KERNEL) {
                drained = local.mPosition[i] >= mAudioTrackServerProxy->framesReleased();
                checked = true;
            }
        }
    }
    ALOGV("%s: trackFramesReleased:%lld  sinkFramesWritten:%lld  setDrained: %d",
        __func__, (long long)trackFramesReleased, (long long)sinkFramesWritten, drained);
    mAudioTrackServerProxy->setDrained(drained);
    local.mFlushed = mAudioTrackServerProxy->framesFlushed();
    mServerProxy->setTimestamp(local);
    const bool useTrackTimestamp = !drained;
    const double latencyMs = useTrackTimestamp
            ? local.getOutputServerLatencyMs(sampleRate())
            : timeStamp.getOutputServerLatencyMs(halSampleRate);
    mServerLatencyFromTrack.store(useTrackTimestamp);
    mServerLatencyMs.store(latencyMs);
    if (mLogStartCountdown > 0
            && local.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL] > 0
            && local.mPosition[ExtendedTimestamp::LOCATION_KERNEL] > 0)
    {
        if (mLogStartCountdown > 1) {
            --mLogStartCountdown;
        } else if (latencyMs < mLogLatencyMs) {
            mLogStartCountdown = 0;
            double startUpMs =
                    (local.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL] - mLogStartTimeNs) * 1e-6;
            startUpMs -= (local.mPosition[ExtendedTimestamp::LOCATION_KERNEL] - mLogStartFrames)
                    * 1e3 / mSampleRate;
            ALOGV("%s: latencyMs:%lf startUpMs:%lf"
                    " localTime:%lld startTime:%lld"
                    " localPosition:%lld startPosition:%lld",
                    __func__, latencyMs, startUpMs,
                    (long long)local.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL],
                    (long long)mLogStartTimeNs,
                    (long long)local.mPosition[ExtendedTimestamp::LOCATION_KERNEL],
                    (long long)mLogStartFrames);
            mTrackMetrics.logLatencyAndStartup(latencyMs, startUpMs);
        }
        mLogLatencyMs = latencyMs;
    }
}
bool Track::AudioVibrationController::setMute(bool muted) {
    const sp<IAfThreadBase> thread = mTrack->mThread.promote();
    if (thread != 0) {
        audio_utils::lock_guard _l(thread->mutex());
        auto* const playbackThread = thread->asIAfPlaybackThread().get();
        if ((mTrack->channelMask() & AUDIO_CHANNEL_HAPTIC_ALL) != AUDIO_CHANNEL_NONE
                && playbackThread->hapticChannelCount() > 0) {
            ALOGD("%s, haptic playback was %s for track %d",
                    __func__, muted ? "muted" : "unmuted", mTrack->id());
            mTrack->setHapticPlaybackEnabled(!muted);
            return true;
        }
    }
    return false;
}
binder::Status Track::AudioVibrationController::mute(
                bool *ret) {
    *ret = setMute(true);
    return binder::Status::ok();
}
binder::Status Track::AudioVibrationController::unmute(
                bool *ret) {
    *ret = setMute(false);
    return binder::Status::ok();
}
#undef LOG_TAG
#define LOG_TAG "AF::OutputTrack"
sp<IAfOutputTrack> IAfOutputTrack::create(
        IAfPlaybackThread* playbackThread,
        IAfDuplicatingThread* sourceThread,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        size_t frameCount,
        const AttributionSourceState& attributionSource) {
    return sp<OutputTrack>::make(
            playbackThread,
            sourceThread,
            sampleRate,
            format,
            channelMask,
            frameCount,
            attributionSource);
}
OutputTrack::OutputTrack(
            IAfPlaybackThread* playbackThread,
            IAfDuplicatingThread* sourceThread,
            uint32_t sampleRate,
            audio_format_t format,
            audio_channel_mask_t channelMask,
            size_t frameCount,
            const AttributionSourceState& attributionSource)
    : Track(playbackThread, NULL, AUDIO_STREAM_PATCH,
              audio_attributes_t{} ,
              sampleRate, format, channelMask, frameCount,
              nullptr , (size_t)0 , nullptr ,
              AUDIO_SESSION_NONE, getpid(), attributionSource, AUDIO_OUTPUT_FLAG_NONE,
              TYPE_OUTPUT),
    mActive(false), mSourceThread(sourceThread)
{
    if (mCblk != NULL) {
        mOutBuffer.frameCount = 0;
        playbackThread->addOutputTrack_l(this);
        ALOGV("%s(): mCblk %p, mBuffer %p, "
                "frameCount %zu, mChannelMask 0x%08x",
                __func__, mCblk, mBuffer,
                frameCount, mChannelMask);
        mClientProxy = new AudioTrackClientProxy(mCblk, mBuffer, mFrameCount, mFrameSize,
                true );
        mClientProxy->setVolumeLR(GAIN_MINIFLOAT_PACKED_UNITY);
        mClientProxy->setSendLevel(0.0);
        mClientProxy->setSampleRate(sampleRate);
    } else {
        ALOGW("%s(%d): Error creating output track on thread %d",
                __func__, mId, (int)mThreadIoHandle);
    }
}
OutputTrack::~OutputTrack()
{
    clearBufferQueue();
}
status_t OutputTrack::start(AudioSystem::sync_event_t event,
                                                          audio_session_t triggerSession)
{
    status_t status = Track::start(event, triggerSession);
    if (status != NO_ERROR) {
        return status;
    }
    mActive = true;
    mRetryCount = 127;
    return status;
}
void OutputTrack::stop()
{
    Track::stop();
    clearBufferQueue();
    mOutBuffer.frameCount = 0;
    mActive = false;
}
ssize_t OutputTrack::write(void* data, uint32_t frames)
{
    if (!mActive && frames != 0) {
        const sp<IAfThreadBase> thread = mThread.promote();
        if (thread != nullptr && thread->inStandby()) {
            ClientProxy::Buffer buf { .mFrameCount = mClientProxy->getStartThresholdInFrames() };
            status_t status = mClientProxy->obtainBuffer(&buf);
            if (status != NO_ERROR && status != NOT_ENOUGH_DATA && status != WOULD_BLOCK) {
                ALOGE("%s(%d): could not obtain buffer on start", __func__, mId);
                return 0;
            }
            memset(buf.mRaw, 0, buf.mFrameCount * mFrameSize);
            mClientProxy->releaseBuffer(&buf);
            (void) start();
            auto* const pt = thread->asIAfPlaybackThread().get();
            if (!pt->waitForHalStart()) {
                ALOGW("%s(%d): timeout waiting for thread to exit standby", __func__, mId);
                stop();
                return 0;
            }
            Buffer firstBuffer;
            firstBuffer.frameCount = frames;
            firstBuffer.raw = data;
            queueBuffer(firstBuffer);
            return frames;
        } else {
            (void) start();
        }
    }
    Buffer *pInBuffer;
    Buffer inBuffer;
    inBuffer.frameCount = frames;
    inBuffer.raw = data;
    uint32_t waitTimeLeftMs = mSourceThread->waitTimeMs();
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
            if (status != NO_ERROR && status != NOT_ENOUGH_DATA) {
                ALOGV("%s(%d): thread %d no more output buffers; status %d",
                        __func__, mId,
                        (int)mThreadIoHandle, status);
                break;
            }
            uint32_t waitTimeMs = (uint32_t)ns2ms(systemTime() - startTime);
            if (waitTimeLeftMs >= waitTimeMs) {
                waitTimeLeftMs -= waitTimeMs;
            } else {
                waitTimeLeftMs = 0;
            }
            if (status == NOT_ENOUGH_DATA) {
                restartIfDisabled();
                continue;
            }
        }
        uint32_t outFrames = pInBuffer->frameCount > mOutBuffer.frameCount ? mOutBuffer.frameCount :
                pInBuffer->frameCount;
        memcpy(mOutBuffer.raw, pInBuffer->raw, outFrames * mFrameSize);
        Proxy::Buffer buf;
        buf.mFrameCount = outFrames;
        buf.mRaw = NULL;
        mClientProxy->releaseBuffer(&buf);
        restartIfDisabled();
        pInBuffer->frameCount -= outFrames;
        pInBuffer->raw = (int8_t *)pInBuffer->raw + outFrames * mFrameSize;
        mOutBuffer.frameCount -= outFrames;
        mOutBuffer.raw = (int8_t *)mOutBuffer.raw + outFrames * mFrameSize;
        if (pInBuffer->frameCount == 0) {
            if (mBufferQueue.size()) {
                mBufferQueue.removeAt(0);
                free(pInBuffer->mBuffer);
                if (pInBuffer != &inBuffer) {
                    delete pInBuffer;
                }
                ALOGV("%s(%d): thread %d released overflow buffer %zu",
                        __func__, mId,
                        (int)mThreadIoHandle, mBufferQueue.size());
            } else {
                break;
            }
        }
    }
    if (inBuffer.frameCount) {
        const sp<IAfThreadBase> thread = mThread.promote();
        if (thread != nullptr && !thread->inStandby()) {
            queueBuffer(inBuffer);
        }
    }
    if (frames == 0 && mBufferQueue.size() == 0 && mActive) {
        stop();
    }
    return frames - inBuffer.frameCount;
}
void OutputTrack::queueBuffer(Buffer& inBuffer) {
    if (mBufferQueue.size() < kMaxOverFlowBuffers) {
        Buffer *pInBuffer = new Buffer;
        const size_t bufferSize = inBuffer.frameCount * mFrameSize;
        pInBuffer->mBuffer = malloc(bufferSize);
        LOG_ALWAYS_FATAL_IF(pInBuffer->mBuffer == nullptr,
                "%s: Unable to malloc size %zu", __func__, bufferSize);
        pInBuffer->frameCount = inBuffer.frameCount;
        pInBuffer->raw = pInBuffer->mBuffer;
        memcpy(pInBuffer->raw, inBuffer.raw, inBuffer.frameCount * mFrameSize);
        mBufferQueue.add(pInBuffer);
        ALOGV("%s(%d): thread %d adding overflow buffer %zu", __func__, mId,
                (int)mThreadIoHandle, mBufferQueue.size());
        inBuffer.frameCount = 0;
    } else {
        ALOGW("%s(%d): thread %d no more overflow buffers",
                __func__, mId, (int)mThreadIoHandle);
    }
}
void OutputTrack::copyMetadataTo(MetadataInserter& backInserter) const
{
<<<<<<< HEAD
    audio_utils::lock_guard lock(trackMetadataMutex());
||||||| 85a074502c
    std::lock_guard<std::mutex> lock(mTrackMetadatasMutex);
=======
    audio_utils::lock_guard lock(mTrackMetadatasMutex);
>>>>>>> d65f1d80
    backInserter = std::copy(mTrackMetadatas.begin(), mTrackMetadatas.end(), backInserter);
}
void OutputTrack::setMetadatas(const SourceMetadatas& metadatas) {
    {
<<<<<<< HEAD
        audio_utils::lock_guard lock(trackMetadataMutex());
||||||| 85a074502c
        std::lock_guard<std::mutex> lock(mTrackMetadatasMutex);
=======
        audio_utils::lock_guard lock(mTrackMetadatasMutex);
>>>>>>> d65f1d80
        mTrackMetadatas = metadatas;
    }
    setMetadataHasChanged();
}
status_t OutputTrack::obtainBuffer(
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
void OutputTrack::clearBufferQueue()
{
    size_t size = mBufferQueue.size();
    for (size_t i = 0; i < size; i++) {
        Buffer *pBuffer = mBufferQueue.itemAt(i);
        free(pBuffer->mBuffer);
        delete pBuffer;
    }
    mBufferQueue.clear();
}
void OutputTrack::restartIfDisabled()
{
    int32_t flags = android_atomic_and(~CBLK_DISABLED, &mCblk->mFlags);
    if (mActive && (flags & CBLK_DISABLED)) {
        start();
    }
}
#undef LOG_TAG
#define LOG_TAG "AF::PatchTrack"
sp<IAfPatchTrack> IAfPatchTrack::create(
        IAfPlaybackThread* playbackThread,
        audio_stream_type_t streamType,
        uint32_t sampleRate,
        audio_channel_mask_t channelMask,
        audio_format_t format,
        size_t frameCount,
        void* buffer,
        size_t bufferSize,
        audio_output_flags_t flags,
        const Timeout& timeout,
        size_t frameCountToBeReady )
{
    return sp<PatchTrack>::make(
            playbackThread,
            streamType,
            sampleRate,
            channelMask,
            format,
            frameCount,
            buffer,
            bufferSize,
            flags,
            timeout,
            frameCountToBeReady);
}
PatchTrack::PatchTrack(IAfPlaybackThread* playbackThread,
                                                     audio_stream_type_t streamType,
                                                     uint32_t sampleRate,
                                                     audio_channel_mask_t channelMask,
                                                     audio_format_t format,
                                                     size_t frameCount,
                                                     void *buffer,
                                                     size_t bufferSize,
                                                     audio_output_flags_t flags,
                                                     const Timeout& timeout,
                                                     size_t frameCountToBeReady)
    : Track(playbackThread, NULL, streamType,
              audio_attributes_t{} ,
              sampleRate, format, channelMask, frameCount,
              buffer, bufferSize, nullptr ,
              AUDIO_SESSION_NONE, getpid(), audioServerAttributionSource(getpid()), flags,
              TYPE_PATCH, AUDIO_PORT_HANDLE_NONE, frameCountToBeReady),
        PatchTrackBase(mCblk ? new ClientProxy(mCblk, mBuffer, frameCount, mFrameSize, true, true)
                        : nullptr,
                       playbackThread, timeout)
{
    ALOGV("%s(%d): sampleRate %d mPeerTimeout %d.%03d sec",
                                      __func__, mId, sampleRate,
                                      (int)mPeerTimeout.tv_sec,
                                      (int)(mPeerTimeout.tv_nsec / 1000000));
}
PatchTrack::~PatchTrack()
{
    ALOGV("%s(%d)", __func__, mId);
}
size_t PatchTrack::framesReady() const
{
    if (mPeerProxy && mPeerProxy->producesBufferOnDemand()) {
        return std::numeric_limits<size_t>::max();
    } else {
        return Track::framesReady();
    }
}
status_t PatchTrack::start(AudioSystem::sync_event_t event,
                                                         audio_session_t triggerSession)
{
    status_t status = Track::start(event, triggerSession);
    if (status != NO_ERROR) {
        return status;
    }
    android_atomic_and(~CBLK_DISABLED, &mCblk->mFlags);
    return status;
}
status_t PatchTrack::getNextBuffer(
        AudioBufferProvider::Buffer* buffer)
{
    ALOG_ASSERT(mPeerProxy != 0, "%s(%d): called without peer proxy", __func__, mId);
    Proxy::Buffer buf;
    buf.mFrameCount = buffer->frameCount;
    if (ATRACE_ENABLED()) {
        std::string traceName("PTnReq");
        traceName += std::to_string(id());
        ATRACE_INT(traceName.c_str(), buf.mFrameCount);
    }
    status_t status = mPeerProxy->obtainBuffer(&buf, &mPeerTimeout);
    ALOGV_IF(status != NO_ERROR, "%s(%d): getNextBuffer status %d", __func__, mId, status);
    buffer->frameCount = buf.mFrameCount;
    if (ATRACE_ENABLED()) {
        std::string traceName("PTnObt");
        traceName += std::to_string(id());
        ATRACE_INT(traceName.c_str(), buf.mFrameCount);
    }
    if (buf.mFrameCount == 0) {
        return WOULD_BLOCK;
    }
    status = Track::getNextBuffer(buffer);
    return status;
}
void PatchTrack::releaseBuffer(AudioBufferProvider::Buffer* buffer)
{
    ALOG_ASSERT(mPeerProxy != 0, "%s(%d): called without peer proxy", __func__, mId);
    Proxy::Buffer buf;
    buf.mFrameCount = buffer->frameCount;
    buf.mRaw = buffer->raw;
    mPeerProxy->releaseBuffer(&buf);
    TrackBase::releaseBuffer(buffer);
}
status_t PatchTrack::obtainBuffer(Proxy::Buffer* buffer,
                                                                const struct timespec *timeOut)
{
    status_t status = NO_ERROR;
    static const int32_t kMaxTries = 5;
    int32_t tryCounter = kMaxTries;
    const size_t originalFrameCount = buffer->mFrameCount;
    do {
        if (status == NOT_ENOUGH_DATA) {
            restartIfDisabled();
            buffer->mFrameCount = originalFrameCount;
        }
        status = mProxy->obtainBuffer(buffer, timeOut);
    } while ((status == NOT_ENOUGH_DATA) && (tryCounter-- > 0));
    return status;
}
void PatchTrack::releaseBuffer(Proxy::Buffer* buffer)
{
    mProxy->releaseBuffer(buffer);
    restartIfDisabled();
    if (mFillingStatus == FS_ACTIVE
            && audio_is_linear_pcm(mFormat)
            && !isOffloadedOrDirect()) {
        if (const sp<IAfThreadBase> thread = mThread.promote();
            thread != 0) {
            auto* const playbackThread = thread->asIAfPlaybackThread().get();
            const size_t frameCount = playbackThread->frameCount() * sampleRate()
                    / playbackThread->sampleRate();
            if (framesReady() < frameCount) {
                ALOGD("%s(%d) Not enough data, wait for buffer to fill", __func__, mId);
                mFillingStatus = FS_FILLING;
            }
        }
    }
}
void PatchTrack::restartIfDisabled()
{
    if (android_atomic_and(~CBLK_DISABLED, &mCblk->mFlags) & CBLK_DISABLED) {
        ALOGW("%s(%d): disabled due to previous underrun, restarting", __func__, mId);
        start();
    }
}
#undef LOG_TAG
#define LOG_TAG "AF::RecordHandle"
class RecordHandle : public android::media::BnAudioRecord {
public:
    explicit RecordHandle(const sp<IAfRecordTrack>& recordTrack);
    ~RecordHandle() override;
    binder::Status start(int event,
            int triggerSession) final;
    binder::Status stop() final;
    binder::Status getActiveMicrophones(
            std::vector<media::MicrophoneInfoFw>* activeMicrophones) final;
    binder::Status setPreferredMicrophoneDirection(
            int direction) final;
    binder::Status setPreferredMicrophoneFieldDimension(float zoom) final;
    binder::Status shareAudioHistory(
            const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) final;
private:
    const sp<IAfRecordTrack> mRecordTrack;
    void stop_nonvirtual();
};
sp<media::IAudioRecord> IAfRecordTrack::createIAudioRecordAdapter(
        const sp<IAfRecordTrack>& recordTrack) {
    return sp<RecordHandle>::make(recordTrack);
}
RecordHandle::RecordHandle(
        const sp<IAfRecordTrack>& recordTrack)
    : BnAudioRecord(),
    mRecordTrack(recordTrack)
{
    setMinSchedulerPolicy(SCHED_NORMAL, ANDROID_PRIORITY_AUDIO);
}
RecordHandle::~RecordHandle() {
    stop_nonvirtual();
    mRecordTrack->destroy();
}
binder::Status RecordHandle::start(int event,
        int triggerSession) {
    ALOGV("%s()", __func__);
    return binderStatusFromStatusT(
        mRecordTrack->start((AudioSystem::sync_event_t)event, (audio_session_t) triggerSession));
}
binder::Status RecordHandle::stop() {
    stop_nonvirtual();
    return binder::Status::ok();
}
void RecordHandle::stop_nonvirtual() {
    ALOGV("%s()", __func__);
    mRecordTrack->stop();
}
binder::Status RecordHandle::getActiveMicrophones(
        std::vector<media::MicrophoneInfoFw>* activeMicrophones) {
    ALOGV("%s()", __func__);
    return binderStatusFromStatusT(mRecordTrack->getActiveMicrophones(activeMicrophones));
}
binder::Status RecordHandle::setPreferredMicrophoneDirection(
        int direction) {
    ALOGV("%s()", __func__);
    return binderStatusFromStatusT(mRecordTrack->setPreferredMicrophoneDirection(
            static_cast<audio_microphone_direction_t>(direction)));
}
binder::Status RecordHandle::setPreferredMicrophoneFieldDimension(float zoom) {
    ALOGV("%s()", __func__);
    return binderStatusFromStatusT(mRecordTrack->setPreferredMicrophoneFieldDimension(zoom));
}
binder::Status RecordHandle::shareAudioHistory(
        const std::string& sharedAudioPackageName, int64_t sharedAudioStartMs) {
    return binderStatusFromStatusT(
            mRecordTrack->shareAudioHistory(sharedAudioPackageName, sharedAudioStartMs));
}
#undef LOG_TAG
#define LOG_TAG "AF::RecordTrack"
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
    : TrackBase(thread, client, attr, sampleRate, format,
                  channelMask, frameCount, buffer, bufferSize, sessionId,
                  creatorPid,
                  VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid)),
                  false ,
                  (type == TYPE_DEFAULT) ?
                          ((flags & AUDIO_INPUT_FLAG_FAST) ? ALLOC_PIPE : ALLOC_CBLK) :
                          ((buffer == NULL) ? ALLOC_LOCAL : ALLOC_NONE),
                  type, portId,
                  std::string(AMEDIAMETRICS_KEY_PREFIX_AUDIO_RECORD) + std::to_string(portId)),
        mOverflow(false),
        mResamplerBufferProvider(NULL),
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
        mServerLatencySupported = checkServerLatencySupported(mFormat, flags);
    }
#ifdef TEE_SINK
    mTee.setId(std::string("_") + std::to_string(mThreadIoHandle)
            + "_" + std::to_string(mId)
            + "_R");
#endif
    mTrackMetrics.logConstructor(creatorPid, uid(), id());
}
RecordTrack::~RecordTrack()
{
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
status_t RecordTrack::getNextBuffer(AudioBufferProvider::Buffer* buffer)
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
            recordThread->destroyTrack_l(this);
        }
        if (isExternalTrack()) {
            switch (priorState) {
            case ACTIVE:
            case STARTING_2:
            case PAUSING:
                AudioSystem::stopInput(mPortId);
                break;
            case STARTING_1:
            case PAUSED:
            case IDLE:
                break;
            case STOPPED:
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
    audio_track_cblk_t* cblk = mCblk;
    android_atomic_or(CBLK_INVALID, &cblk->mFlags);
    android_atomic_release_store(0x40000000, &cblk->mFutex);
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
            result.appendFormat(" %7.2lf %c", latencyMs, fromTrack ? 't' : 'k');
        } else {
            result.appendFormat("%10s", mCblk->mServer != 0 ? "unavail" : "new");
        }
    }
    result.append("\n");
}
void RecordTrack::handleSyncStartEvent(
        const sp<audioflinger::SyncEvent>& event)
{
    size_t framesToDrop = 0;
    const sp<IAfThreadBase> threadBase = mThread.promote();
    if (threadBase != 0) {
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
    const FrameTime ft{
            timestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL],
            timestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL]};
    mKernelFrameTime.store(ft);
    if (!audio_is_linear_pcm(mFormat)) {
        mServerProxy->setTimestamp(timestamp);
        return;
    }
    ExtendedTimestamp local = timestamp;
    for (int i = ExtendedTimestamp::LOCATION_SERVER; i < ExtendedTimestamp::LOCATION_MAX; ++i) {
        if (local.mTimeNs[i] != 0) {
            const int64_t relativeServerFrames = local.mPosition[i] - sourceFramesRead;
            const int64_t relativeTrackFrames = relativeServerFrames
                    * mSampleRate / halSampleRate;
            local.mPosition[i] = relativeTrackFrames + trackFramesReleased;
        }
    }
    mServerProxy->setTimestamp(local);
    const bool useTrackTimestamp = true;
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
    if (mAttr.source == AUDIO_SOURCE_DEFAULT) {
        return;
    }
    record_track_metadata_v7_t metadata;
    metadata.base = {
            .source = mAttr.source,
            .gain = 1,
    };
    metadata.channel_mask = mChannelMask;
    strncpy(metadata.tags, mAttr.tags, AUDIO_ATTRIBUTES_TAGS_MAX_SIZE);
    *backInserter++ = metadata;
}
#undef LOG_TAG
#define LOG_TAG "AF::PatchRecord"
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
    : RecordTrack(recordThread, NULL,
                audio_attributes_t{ .source = source } ,
                sampleRate, format, channelMask, frameCount,
                buffer, bufferSize, AUDIO_SESSION_NONE, getpid(),
                audioServerAttributionSource(getpid()), flags, TYPE_PATCH),
        PatchTrackBase(mCblk ? new ClientProxy(mCblk, mBuffer, frameCount, mFrameSize, false, true)
                        : nullptr,
                       recordThread, timeout)
{
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
size_t PatchRecord::writeFrames(
        AudioBufferProvider* dest, const void* src, size_t frameCount, size_t frameSize)
{
    size_t framesWritten = writeFramesHelper(dest, src, frameCount, frameSize);
    const size_t framesLeft = frameCount - framesWritten;
    if (framesWritten != 0 && framesLeft != 0) {
        framesWritten += writeFramesHelper(dest, (const char*)src + framesWritten * frameSize,
                        framesLeft, frameSize);
    }
    return framesWritten;
}
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
void PatchRecord::releaseBuffer(AudioBufferProvider::Buffer* buffer)
{
    ALOG_ASSERT(mPeerProxy != 0, "%s(%d): called without peer proxy", __func__, mId);
    Proxy::Buffer buf;
    buf.mFrameCount = buffer->frameCount;
    buf.mRaw = buffer->raw;
    mPeerProxy->releaseBuffer(&buf);
    TrackBase::releaseBuffer(buffer);
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
#undef LOG_TAG
#define LOG_TAG "AF::PthrPatchRecord"
static std::unique_ptr<void, decltype(free)*> allocAligned(size_t alignment, size_t size)
{
    void *ptr = nullptr;
    (void)posix_memalign(&ptr, alignment, size);
    return {ptr, free};
}
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
                nullptr , 0 , flags, {} , source),
          mPatchRecordAudioBufferProvider(*this),
          mSinkBuffer(allocAligned(32, mFrameCount * mFrameSize)),
          mStubBuffer(allocAligned(32, mFrameCount * mFrameSize))
{
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
status_t PassthruPatchRecord::obtainBuffer(
        Proxy::Buffer* buffer, const struct timespec* timeOut)
{
    if (mUnconsumedFrames) {
        buffer->mFrameCount = std::min(buffer->mFrameCount, mUnconsumedFrames);
        return PatchRecord::obtainBuffer(buffer, timeOut);
    }
    nsecs_t startTimeNs = 0;
    if (timeOut && (timeOut->tv_sec != 0 || timeOut->tv_nsec != 0) && timeOut->tv_sec != INT_MAX) {
        startTimeNs = systemTime();
    }
    const size_t framesToRead = std::min(buffer->mFrameCount, mFrameCount);
    buffer->mFrameCount = 0;
    buffer->mRaw = nullptr;
    sp<IAfThreadBase> thread;
    sp<StreamInHalInterface> stream = obtainStream(&thread);
    if (!stream) return NO_INIT;
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
    buffer->mFrameCount = writeFrames(
            &mPatchRecordAudioBufferProvider,
            mSinkBuffer.get(), bytesRead / mFrameSize, mFrameSize);
    ALOGW_IF(buffer->mFrameCount < bytesRead / mFrameSize,
            "Lost %zu frames obtained from HAL", bytesRead / mFrameSize - buffer->mFrameCount);
    mUnconsumedFrames = buffer->mFrameCount;
    struct timespec newTimeOut;
    if (startTimeNs) {
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
void PassthruPatchRecord::releaseBuffer(Proxy::Buffer* buffer)
{
    if (buffer->mFrameCount <= mUnconsumedFrames) {
        mUnconsumedFrames -= buffer->mFrameCount;
    } else {
        ALOGW("Write side has consumed more frames than we had: %zu > %zu",
                buffer->mFrameCount, mUnconsumedFrames);
        mUnconsumedFrames = 0;
    }
    PatchRecord::releaseBuffer(buffer);
}
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
    return 0;
}
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
#undef LOG_TAG
#define LOG_TAG "AF::MmapTrack"
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
    : TrackBase(thread, NULL, attr, sampleRate, format,
                  channelMask, (size_t)0 ,
                  nullptr , (size_t)0 ,
                  sessionId, creatorPid,
                  VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid)),
                  isOut,
                  ALLOC_NONE,
                  TYPE_DEFAULT, portId,
                  std::string(AMEDIAMETRICS_KEY_PREFIX_AUDIO_MMAP) + std::to_string(portId)),
        mPid(VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.pid))),
            mSilenced(false), mSilencedNotified(false)
{
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
status_t MmapTrack::getNextBuffer(AudioBufferProvider::Buffer* buffer)
{
    buffer->frameCount = 0;
    buffer->raw = nullptr;
    return INVALID_OPERATION;
}
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
}
