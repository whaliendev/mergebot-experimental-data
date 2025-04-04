       
#include "TrackBase.h"
#include <android/os/BnExternalVibrationController.h>
#include <audio_utils/mutex.h>
#include <audio_utils/LinearMap.h>
#include <binder/AppOpsManager.h>
namespace android {
class OpPlayAudioMonitor : public RefBase {
    friend class sp<OpPlayAudioMonitor>;
public:
    ~OpPlayAudioMonitor() override;
    bool hasOpPlayAudio() const;
    static sp<OpPlayAudioMonitor> createIfNeeded(
            IAfThreadBase* thread,
            const AttributionSourceState& attributionSource,
            const audio_attributes_t& attr, int id,
            audio_stream_type_t streamType);
private:
    OpPlayAudioMonitor(IAfThreadBase* thread,
                       const AttributionSourceState& attributionSource,
                       audio_usage_t usage, int id, uid_t uid);
    void onFirstRef() override;
    static void getPackagesForUid(uid_t uid, Vector<String16>& packages);
    AppOpsManager mAppOpsManager;
    class PlayAudioOpCallback : public BnAppOpsCallback {
    public:
        explicit PlayAudioOpCallback(const wp<OpPlayAudioMonitor>& monitor);
        void opChanged(int32_t op, const String16& packageName) override;
    private:
        const wp<OpPlayAudioMonitor> mMonitor;
    };
    sp<PlayAudioOpCallback> mOpCallback;
    void checkPlayAudioForUsage(bool doBroadcast);
    wp<IAfThreadBase> mThread;
    std::atomic_bool mHasOpPlayAudio;
    const AttributionSourceState mAttributionSource;
    const int32_t mUsage;
    const int mId;
    const uid_t mUid;
    const String16 mPackageName;
};
class Track : public TrackBase, public virtual IAfTrack, public VolumeProvider {
public:
    Track(IAfPlaybackThread* thread,
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
                                audio_port_handle_t portId = AUDIO_PORT_HANDLE_NONE,
                                size_t frameCountToBeReady = SIZE_MAX,
                                float speed = 1.0f,
                                bool isSpatialized = false,
                                bool isBitPerfect = false);
    ~Track() override;
    status_t initCheck() const final;
    void appendDumpHeader(String8& result) const final;
    void appendDump(String8& result, bool active) const final;
    status_t start(AudioSystem::sync_event_t event = AudioSystem::SYNC_EVENT_NONE,
            audio_session_t triggerSession = AUDIO_SESSION_NONE) override;
    void stop() override;
    void pause() final;
    void flush() final;
    void destroy() final;
    uint32_t sampleRate() const final;
    audio_stream_type_t streamType() const final {
                return mStreamType;
            }
    bool isOffloaded() const final
                                { return (mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != 0; }
    bool isDirect() const final
                                { return (mFlags & AUDIO_OUTPUT_FLAG_DIRECT) != 0; }
    bool isOffloadedOrDirect() const final { return (mFlags
                            & (AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD
                                    | AUDIO_OUTPUT_FLAG_DIRECT)) != 0; }
    bool isStatic() const final { return mSharedBuffer.get() != nullptr; }
    status_t setParameters(const String8& keyValuePairs) final;
    status_t selectPresentation(int presentationId, int programId) final;
    status_t attachAuxEffect(int EffectId) final;
    void setAuxBuffer(int EffectId, int32_t* buffer) final;
    int32_t* auxBuffer() const final { return mAuxBuffer; }
    void setMainBuffer(float* buffer) final { mMainBuffer = buffer; }
    float* mainBuffer() const final { return mMainBuffer; }
    int auxEffectId() const final { return mAuxEffectId; }
    status_t getTimestamp(AudioTimestamp& timestamp) final;
    void signal() final;
    status_t getDualMonoMode(audio_dual_mono_mode_t* mode) const final;
    status_t setDualMonoMode(audio_dual_mono_mode_t mode) final;
    status_t getAudioDescriptionMixLevel(float* leveldB) const final;
    status_t setAudioDescriptionMixLevel(float leveldB) final;
    status_t getPlaybackRateParameters(audio_playback_rate_t* playbackRate) const final;
    status_t setPlaybackRateParameters(const audio_playback_rate_t& playbackRate) final;
    gain_minifloat_packed_t getVolumeLR() const final;
    status_t setSyncEvent(const sp<audioflinger::SyncEvent>& event) final;
    bool isFastTrack() const final { return (mFlags & AUDIO_OUTPUT_FLAG_FAST) != 0; }
    double bufferLatencyMs() const final {
                            return isStatic() ? 0. : TrackBase::bufferLatencyMs();
                        }
    media::VolumeShaper::Status applyVolumeShaper(
                                const sp<media::VolumeShaper::Configuration>& configuration,
                                const sp<media::VolumeShaper::Operation>& operation);
    sp<media::VolumeShaper::State> getVolumeShaperState(int id) const final;
    sp<media::VolumeHandler> getVolumeHandler() const final{ return mVolumeHandler; }
    void setFinalVolume(float volumeLeft, float volumeRight) final;
    float getFinalVolume() const final { return mFinalVolume; }
    void getFinalVolume(float* left, float* right) const final {
                            *left = mFinalVolumeLeft;
                            *right = mFinalVolumeRight;
    }
    using SourceMetadatas = std::vector<playback_track_metadata_v7_t>;
    using MetadataInserter = std::back_insert_iterator<SourceMetadatas>;
    void copyMetadataTo(MetadataInserter& backInserter) const override;
    bool getHapticPlaybackEnabled() const final { return mHapticPlaybackEnabled; }
    void setHapticPlaybackEnabled(bool hapticPlaybackEnabled) final {
                mHapticPlaybackEnabled = hapticPlaybackEnabled;
            }
    os::HapticScale getHapticIntensity() const final { return mHapticIntensity; }
    float getHapticMaxAmplitude() const final { return mHapticMaxAmplitude; }
    void setHapticIntensity(os::HapticScale hapticIntensity) final {
                if (os::isValidHapticScale(hapticIntensity)) {
                    mHapticIntensity = hapticIntensity;
                    setHapticPlaybackEnabled(mHapticIntensity != os::HapticScale::MUTE);
                }
            }
    void setHapticMaxAmplitude(float maxAmplitude) final {
                mHapticMaxAmplitude = maxAmplitude;
            }
    sp<os::ExternalVibration> getExternalVibration() const final { return mExternalVibration; }
    void updateTeePatches_l() final;
    void setTeePatchesToUpdate_l(TeePatches teePatchesToUpdate) final;
    void tallyUnderrunFrames(size_t frames) final {
       if (isOut()) {
           mAudioTrackServerProxy->tallyUnderrunFrames(frames);
           mTrackMetrics.logUnderruns(mAudioTrackServerProxy->getUnderrunCount(),
                   mAudioTrackServerProxy->getUnderrunFrames());
       }
    }
    audio_output_flags_t getOutputFlags() const final { return mFlags; }
    float getSpeed() const final { return mSpeed; }
    bool isSpatialized() const final { return mIsSpatialized; }
    bool isBitPerfect() const final { return mIsBitPerfect; }
    void processMuteEvent_l(const sp<IAudioManager>& audioManager, mute_state_t muteState) final;
protected:
    DISALLOW_COPY_AND_ASSIGN(Track);
    status_t getNextBuffer(AudioBufferProvider::Buffer* buffer) override;
    void releaseBuffer(AudioBufferProvider::Buffer* buffer) override;
    size_t framesReady() const override;
    int64_t framesReleased() const override;
    void onTimestamp(const ExtendedTimestamp &timestamp) override;
    bool isPausing() const final { return mState == PAUSING; }
    bool isPaused() const final { return mState == PAUSED; }
    bool isResuming() const final { return mState == RESUMING; }
    bool isReady() const final;
    void setPaused() final { mState = PAUSED; }
    void reset() final;
    bool isFlushPending() const final { return mFlushHwPending; }
    void flushAck() final;
    bool isResumePending() const final;
    void resumeAck() final;
    bool isPausePending() const final { return mPauseHwPending; }
    void pauseAck() final;
    void updateTrackFrameInfo(int64_t trackFramesReleased, int64_t sinkFramesWritten,
            uint32_t halSampleRate, const ExtendedTimestamp& timeStamp) final;
    sp<IMemory> sharedBuffer() const final { return mSharedBuffer; }
    bool presentationComplete(int64_t framesWritten, size_t audioHalFrames) final;
    bool presentationComplete(uint32_t latencyMs) final;
    void resetPresentationComplete() final {
        mPresentationCompleteFrames = 0;
        mPresentationCompleteTimeNs = 0;
    }
    void notifyPresentationComplete();
    void signalClientFlag(int32_t flag);
    void triggerEvents(AudioSystem::sync_event_t type) final;
    void invalidate() final;
    void disable() final;
    int& fastIndex() final { return mFastIndex; }
    bool isPlaybackRestricted() const final {
        return mOpPlayAudioMonitor ? !mOpPlayAudioMonitor->hasOpPlayAudio() : false; }
    const sp<AudioTrackServerProxy>& audioTrackServerProxy() const final {
        return mAudioTrackServerProxy;
    }
    bool hasVolumeController() const final { return mHasVolumeController; }
    void setHasVolumeController(bool hasVolumeController) final {
        mHasVolumeController = hasVolumeController;
    }
    void setCachedVolume(float volume) final {
        mCachedVolume = volume;
    }
    void setResetDone(bool resetDone) final {
        mResetDone = resetDone;
    }
    ExtendedAudioBufferProvider* asExtendedAudioBufferProvider() final {
        return this;
    }
    VolumeProvider* asVolumeProvider() final {
        return this;
    }
    FillingStatus& fillingStatus() final { return mFillingStatus; }
    int8_t& retryCount() final { return mRetryCount; }
    FastTrackUnderruns& fastTrackUnderruns() final { return mObservedUnderruns; }
protected:
    mutable FillingStatus mFillingStatus;
    int8_t mRetryCount;
    sp<IMemory> mSharedBuffer;
    bool mResetDone;
    const audio_stream_type_t mStreamType;
    float *mMainBuffer;
    int32_t *mAuxBuffer;
    int mAuxEffectId;
    bool mHasVolumeController;
    LinearMap<int64_t> mFrameMap;
    ExtendedTimestamp mSinkTimestamp;
    sp<media::VolumeHandler> mVolumeHandler;
    sp<OpPlayAudioMonitor> mOpPlayAudioMonitor;
    bool mHapticPlaybackEnabled = false;
    os::HapticScale mHapticIntensity = os::HapticScale::MUTE;
    float mHapticMaxAmplitude = NAN;
    class AudioVibrationController : public os::BnExternalVibrationController {
    public:
        explicit AudioVibrationController(Track* track) : mTrack(track) {}
        binder::Status mute( bool *ret) override;
        binder::Status unmute( bool *ret) override;
    private:
        Track* const mTrack;
        bool setMute(bool muted);
    };
    sp<AudioVibrationController> mAudioVibrationController;
    sp<os::ExternalVibration> mExternalVibration;
    audio_dual_mono_mode_t mDualMonoMode = AUDIO_DUAL_MONO_MODE_OFF;
    float mAudioDescriptionMixLevel = -std::numeric_limits<float>::infinity();
    audio_playback_rate_t mPlaybackRateParameters = AUDIO_PLAYBACK_RATE_INITIALIZER;
private:
    void interceptBuffer(const AudioBufferProvider::Buffer& buffer);
    template <class F>
    void forEachTeePatchTrack_l(F f) {
        for (auto& tp : mTeePatches) { f(tp.patchTrack); }
    };
    size_t mPresentationCompleteFrames = 0;
    int64_t mPresentationCompleteTimeNs = 0;
    int mFastIndex;
    FastTrackUnderruns mObservedUnderruns;
    volatile float mCachedVolume;
    float mFinalVolume;
    float mFinalVolumeLeft;
    float mFinalVolumeRight;
    sp<AudioTrackServerProxy> mAudioTrackServerProxy;
    bool mResumeToStopping;
    bool mFlushHwPending;
    bool mPauseHwPending = false;
    audio_output_flags_t mFlags;
    TeePatches mTeePatches;
    std::optional<TeePatches> mTeePatchesToUpdate;
    const float mSpeed;
    const bool mIsSpatialized;
    const bool mIsBitPerfect;
    std::unique_ptr<os::PersistableBundle> mMuteEventExtras;
    mute_state_t mMuteState;
};
class OutputTrack : public Track, public IAfOutputTrack {
public:
    class Buffer : public AudioBufferProvider::Buffer {
    public:
        void *mBuffer;
    };
    OutputTrack(IAfPlaybackThread* thread,
            IAfDuplicatingThread* sourceThread,
                                uint32_t sampleRate,
                                audio_format_t format,
                                audio_channel_mask_t channelMask,
                                size_t frameCount,
                                const AttributionSourceState& attributionSource);
    ~OutputTrack() override;
    status_t start(AudioSystem::sync_event_t event =
                                    AudioSystem::SYNC_EVENT_NONE,
                             audio_session_t triggerSession = AUDIO_SESSION_NONE) final;
    void stop() final;
    ssize_t write(void* data, uint32_t frames) final;
    bool bufferQueueEmpty() const final { return mBufferQueue.size() == 0; }
    bool isActive() const final { return mActive; }
    void copyMetadataTo(MetadataInserter& backInserter) const final;
    void setMetadatas(const SourceMetadatas& metadatas) final;
    ExtendedTimestamp getClientProxyTimestamp() const final {
                            ExtendedTimestamp timestamp;
                            (void) mClientProxy->getTimestamp(&timestamp);
                            return timestamp;
                        }
private:
    status_t obtainBuffer(AudioBufferProvider::Buffer* buffer,
                                     uint32_t waitTimeMs);
    void queueBuffer(Buffer& inBuffer);
    void clearBufferQueue();
    void restartIfDisabled();
    static const uint8_t kMaxOverFlowBuffers = 10;
    Vector < Buffer* > mBufferQueue;
    AudioBufferProvider::Buffer mOutBuffer;
    bool mActive;
    IAfDuplicatingThread* const mSourceThread;
    sp<AudioTrackClientProxy> mClientProxy;
    SourceMetadatas mTrackMetadatas;
<<<<<<< HEAD
    audio_utils::mutex& trackMetadataMutex() const { return mTrackMetadataMutex; }
    mutable audio_utils::mutex mTrackMetadataMutex;
||||||| 05101a67de
    mutable std::mutex mTrackMetadatasMutex;
=======
    mutable audio_utils::mutex mTrackMetadatasMutex;
>>>>>>> 9004695e
};
class PatchTrack : public Track, public PatchTrackBase, public IAfPatchTrack {
public:
    PatchTrack(IAfPlaybackThread* playbackThread,
                                   audio_stream_type_t streamType,
                                   uint32_t sampleRate,
                                   audio_channel_mask_t channelMask,
                                   audio_format_t format,
                                   size_t frameCount,
                                   void *buffer,
                                   size_t bufferSize,
                                   audio_output_flags_t flags,
                                   const Timeout& timeout = {},
                                   size_t frameCountToBeReady = 1 );
    ~PatchTrack() override;
    size_t framesReady() const final;
    status_t start(AudioSystem::sync_event_t event =
                                    AudioSystem::SYNC_EVENT_NONE,
                             audio_session_t triggerSession = AUDIO_SESSION_NONE) final;
    status_t getNextBuffer(AudioBufferProvider::Buffer* buffer) final;
    void releaseBuffer(AudioBufferProvider::Buffer* buffer) final;
    status_t obtainBuffer(Proxy::Buffer* buffer, const struct timespec* timeOut = nullptr) final;
    void releaseBuffer(Proxy::Buffer* buffer) final;
private:
            void restartIfDisabled();
};
}
