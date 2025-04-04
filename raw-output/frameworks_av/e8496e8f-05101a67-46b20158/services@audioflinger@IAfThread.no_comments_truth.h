       
#include <android/media/IAudioTrackCallback.h>
#include <android/media/IEffectClient.h>
#include <audiomanager/IAudioManager.h>
#include <audio_utils/mutex.h>
#include <audio_utils/MelProcessor.h>
#include <binder/MemoryDealer.h>
#include <datapath/AudioStreamIn.h>
#include <datapath/AudioStreamOut.h>
#include <datapath/VolumeInterface.h>
#include <fastpath/FastMixerDumpState.h>
#include <media/DeviceDescriptorBase.h>
#include <media/MmapStreamInterface.h>
#include <media/audiohal/StreamHalInterface.h>
#include <media/nblog/NBLog.h>
#include <timing/SyncEvent.h>
#include <utils/RefBase.h>
#include <vibrator/ExternalVibration.h>
#include <optional>
namespace android {
class IAfDirectOutputThread;
class IAfDuplicatingThread;
class IAfMmapCaptureThread;
class IAfMmapPlaybackThread;
class IAfPlaybackThread;
class IAfRecordThread;
class IAfEffectChain;
class IAfEffectHandle;
class IAfEffectModule;
class IAfPatchPanel;
class IAfPatchRecord;
class IAfPatchTrack;
class IAfRecordTrack;
class IAfTrack;
class IAfTrackBase;
class Client;
class MelReporter;
struct stream_type_t {
    float volume = 1.f;
    bool mute = false;
};
class IAfThreadCallback : public virtual RefBase {
public:
    virtual audio_utils::mutex& mutex() const
            RETURN_CAPABILITY(audio_utils::AudioFlinger_Mutex) = 0;
    virtual bool isNonOffloadableGlobalEffectEnabled_l() const
            REQUIRES(mutex()) = 0;
    virtual audio_unique_id_t nextUniqueId(audio_unique_id_use_t use) = 0;
    virtual bool btNrecIsOff() const = 0;
    virtual float masterVolume_l() const
            REQUIRES(mutex()) = 0;
    virtual bool masterMute_l() const
            REQUIRES(mutex()) = 0;
    virtual float getMasterBalance_l() const
            REQUIRES(mutex()) = 0;
    virtual bool streamMute_l(audio_stream_type_t stream) const
            REQUIRES(mutex()) = 0;
    virtual audio_mode_t getMode() const = 0;
    virtual bool isLowRamDevice() const = 0;
    virtual bool isAudioPolicyReady() const = 0;
    virtual uint32_t getScreenState() const = 0;
    virtual std::optional<media::AudioVibratorInfo> getDefaultVibratorInfo_l() const
            REQUIRES(mutex()) = 0;
    virtual const sp<IAfPatchPanel>& getPatchPanel() const = 0;
    virtual const sp<MelReporter>& getMelReporter() const = 0;
    virtual const sp<EffectsFactoryHalInterface>& getEffectsFactoryHal() const = 0;
    virtual sp<IAudioManager> getOrCreateAudioManager() = 0;
    virtual bool updateOrphanEffectChains(const sp<IAfEffectModule>& effect)
            EXCLUDES_AudioFlinger_Mutex = 0;
    virtual status_t moveEffectChain_ll(audio_session_t sessionId,
            IAfPlaybackThread* srcThread, IAfPlaybackThread* dstThread)
            REQUIRES(mutex(), audio_utils::ThreadBase_Mutex) = 0;
    virtual void requestLogMerge() = 0;
    virtual sp<NBLog::Writer> newWriter_l(size_t size, const char *name)
            REQUIRES(mutex()) = 0;
    virtual void unregisterWriter(const sp<NBLog::Writer>& writer) = 0;
    virtual sp<audioflinger::SyncEvent> createSyncEvent(AudioSystem::sync_event_t type,
            audio_session_t triggerSession,
            audio_session_t listenerSession,
            const audioflinger::SyncEventCallback& callBack,
            const wp<IAfTrackBase>& cookie)
            EXCLUDES_AudioFlinger_Mutex = 0;
    virtual void ioConfigChanged(audio_io_config_event_t event,
            const sp<AudioIoDescriptor>& ioDesc,
            pid_t pid = 0) EXCLUDES_AudioFlinger_ClientMutex = 0;
    virtual void onNonOffloadableGlobalEffectEnable() EXCLUDES_AudioFlinger_Mutex = 0;
    virtual void onSupportedLatencyModesChanged(
            audio_io_handle_t output, const std::vector<audio_latency_mode_t>& modes)
            EXCLUDES_AudioFlinger_ClientMutex = 0;
};
class IAfThreadBase : public virtual RefBase {
public:
    enum type_t {
        MIXER,
        DIRECT,
        DUPLICATING,
        RECORD,
        OFFLOAD,
        MMAP_PLAYBACK,
        MMAP_CAPTURE,
        SPATIALIZER,
        BIT_PERFECT,
    };
    static const char* threadTypeToString(type_t type);
    static std::string formatToString(audio_format_t format);
    static bool isValidPcmSinkChannelMask(audio_channel_mask_t channelMask);
    static bool isValidPcmSinkFormat(audio_format_t format);
    virtual status_t readyToRun() = 0;
    virtual void clearPowerManager() = 0;
    virtual status_t initCheck() const = 0;
    virtual type_t type() const = 0;
    virtual bool isDuplicating() const = 0;
    virtual audio_io_handle_t id() const = 0;
    virtual uint32_t sampleRate() const = 0;
    virtual audio_channel_mask_t channelMask() const = 0;
    virtual audio_channel_mask_t mixerChannelMask() const = 0;
    virtual audio_format_t format() const = 0;
    virtual uint32_t channelCount() const = 0;
    virtual size_t frameCount() const = 0;
    virtual audio_channel_mask_t hapticChannelMask() const = 0;
    virtual uint32_t hapticChannelCount() const = 0;
    virtual uint32_t latency_l() const = 0;
    virtual void setVolumeForOutput_l(float left, float right) const = 0;
    virtual size_t frameCountHAL() const = 0;
    virtual size_t frameSize() const = 0;
    virtual void exit() = 0;
    virtual bool checkForNewParameter_l(const String8& keyValuePair, status_t& status) = 0;
    virtual status_t setParameters(const String8& keyValuePairs) = 0;
    virtual String8 getParameters(const String8& keys) = 0;
    virtual void ioConfigChanged(
            audio_io_config_event_t event, pid_t pid = 0,
            audio_port_handle_t portId = AUDIO_PORT_HANDLE_NONE) = 0;
    virtual void sendIoConfigEvent(
            audio_io_config_event_t event, pid_t pid = 0,
            audio_port_handle_t portId = AUDIO_PORT_HANDLE_NONE) = 0;
    virtual void sendIoConfigEvent_l(
            audio_io_config_event_t event, pid_t pid = 0,
            audio_port_handle_t portId = AUDIO_PORT_HANDLE_NONE) = 0;
    virtual void sendPrioConfigEvent(pid_t pid, pid_t tid, int32_t prio, bool forApp) = 0;
    virtual void sendPrioConfigEvent_l(pid_t pid, pid_t tid, int32_t prio, bool forApp) = 0;
    virtual status_t sendSetParameterConfigEvent_l(const String8& keyValuePair) = 0;
    virtual status_t sendCreateAudioPatchConfigEvent(
            const struct audio_patch* patch, audio_patch_handle_t* handle) = 0;
    virtual status_t sendReleaseAudioPatchConfigEvent(audio_patch_handle_t handle) = 0;
    virtual status_t sendUpdateOutDeviceConfigEvent(
            const DeviceDescriptorBaseVector& outDevices) = 0;
    virtual void sendResizeBufferConfigEvent_l(int32_t maxSharedAudioHistoryMs) = 0;
    virtual void sendCheckOutputStageEffectsEvent() = 0;
    virtual void sendCheckOutputStageEffectsEvent_l() = 0;
    virtual void sendHalLatencyModesChangedEvent_l() = 0;
    virtual void processConfigEvents_l() = 0;
    virtual void setCheckOutputStageEffects() = 0;
    virtual void cacheParameters_l() = 0;
    virtual status_t createAudioPatch_l(
            const struct audio_patch* patch, audio_patch_handle_t* handle) = 0;
    virtual status_t releaseAudioPatch_l(const audio_patch_handle_t handle) = 0;
    virtual void updateOutDevices(const DeviceDescriptorBaseVector& outDevices) = 0;
    virtual void toAudioPortConfig(struct audio_port_config* config) = 0;
    virtual void resizeInputBuffer_l(int32_t maxSharedAudioHistoryMs) = 0;
    virtual bool inStandby() const = 0;
    virtual const DeviceTypeSet outDeviceTypes() const = 0;
    virtual audio_devices_t inDeviceType() const = 0;
    virtual DeviceTypeSet getDeviceTypes() const = 0;
    virtual const AudioDeviceTypeAddrVector& outDeviceTypeAddrs() const = 0;
    virtual const AudioDeviceTypeAddr& inDeviceTypeAddr() const = 0;
    virtual bool isOutput() const = 0;
    virtual bool isOffloadOrMmap() const = 0;
    virtual sp<StreamHalInterface> stream() const = 0;
    virtual sp<IAfEffectHandle> createEffect_l(
            const sp<Client>& client,
            const sp<media::IEffectClient>& effectClient,
            int32_t priority,
            audio_session_t sessionId,
            effect_descriptor_t* desc,
            int* enabled,
            status_t* status ,
            bool pinned,
            bool probe,
            bool notifyFramesProcessed)
            REQUIRES(audio_utils::AudioFlinger_Mutex) = 0;
    enum effect_state {
        EFFECT_SESSION = 0x1,
        TRACK_SESSION = 0x2,
        FAST_SESSION = 0x4,
        SPATIALIZED_SESSION = 0x8,
        BIT_PERFECT_SESSION = 0x10
    };
    virtual sp<IAfEffectChain> getEffectChain(audio_session_t sessionId) const = 0;
    virtual sp<IAfEffectChain> getEffectChain_l(audio_session_t sessionId) const = 0;
    virtual std::vector<int> getEffectIds_l(audio_session_t sessionId) const = 0;
    virtual status_t addEffectChain_l(const sp<IAfEffectChain>& chain) = 0;
    virtual size_t removeEffectChain_l(const sp<IAfEffectChain>& chain) = 0;
    virtual void lockEffectChains_l(Vector<sp<IAfEffectChain>>& effectChains) = 0;
    virtual void unlockEffectChains(const Vector<sp<IAfEffectChain>>& effectChains) = 0;
    virtual Vector<sp<IAfEffectChain>> getEffectChains_l() const = 0;
    virtual void setMode(audio_mode_t mode) = 0;
    virtual sp<IAfEffectModule> getEffect(audio_session_t sessionId, int effectId) const = 0;
    virtual sp<IAfEffectModule> getEffect_l(audio_session_t sessionId, int effectId) const = 0;
    virtual status_t addEffect_ll(const sp<IAfEffectModule>& effect)
            REQUIRES(audio_utils::AudioFlinger_Mutex, mutex()) = 0;
    virtual void removeEffect_l(const sp<IAfEffectModule>& effect, bool release = false) = 0;
    virtual void disconnectEffectHandle(IAfEffectHandle* handle, bool unpinIfLast) = 0;
    virtual void detachAuxEffect_l(int effectId) = 0;
    virtual uint32_t hasAudioSession_l(audio_session_t sessionId) const = 0;
    virtual uint32_t hasAudioSession(audio_session_t sessionId) const = 0;
    virtual product_strategy_t getStrategyForSession_l(audio_session_t sessionId) const = 0;
    virtual void checkSuspendOnEffectEnabled(
            bool enabled, audio_session_t sessionId, bool threadLocked) = 0;
    virtual status_t setSyncEvent(const sp<audioflinger::SyncEvent>& event) = 0;
    virtual bool isValidSyncEvent(const sp<audioflinger::SyncEvent>& event) const = 0;
    virtual sp<MemoryDealer> readOnlyHeap() const = 0;
    virtual sp<IMemory> pipeMemory() const = 0;
    virtual void systemReady() = 0;
    virtual status_t checkEffectCompatibility_l(
            const effect_descriptor_t* desc, audio_session_t sessionId) = 0;
    virtual void broadcast_l() = 0;
    virtual bool isTimestampCorrectionEnabled() const = 0;
    virtual bool isMsdDevice() const = 0;
    virtual void dump(int fd, const Vector<String16>& args) = 0;
    virtual void sendStatistics(bool force) = 0;
    virtual audio_utils::mutex& mutex() const
            RETURN_CAPABILITY(audio_utils::ThreadBase_Mutex) = 0;
    virtual void onEffectEnable(const sp<IAfEffectModule>& effect) = 0;
    virtual void onEffectDisable() = 0;
    virtual void invalidateTracksForAudioSession_l(audio_session_t sessionId) const = 0;
    virtual void invalidateTracksForAudioSession(audio_session_t sessionId) const = 0;
    virtual bool isStreamInitialized() const = 0;
    virtual void startMelComputation_l(const sp<audio_utils::MelProcessor>& processor)
            REQUIRES(audio_utils::AudioFlinger_Mutex) = 0;
    virtual void stopMelComputation_l()
            REQUIRES(audio_utils::AudioFlinger_Mutex) = 0;
    virtual product_strategy_t getStrategyForStream(audio_stream_type_t stream) const = 0;
    virtual void setEffectSuspended_l(
            const effect_uuid_t* type, bool suspend, audio_session_t sessionId) = 0;
    virtual sp<IAfDirectOutputThread> asIAfDirectOutputThread() { return nullptr; }
    virtual sp<IAfDuplicatingThread> asIAfDuplicatingThread() { return nullptr; }
    virtual sp<IAfPlaybackThread> asIAfPlaybackThread() { return nullptr; }
    virtual sp<IAfRecordThread> asIAfRecordThread() { return nullptr; }
    virtual IAfThreadCallback* afThreadCallback() const = 0;
};
class IAfPlaybackThread : public virtual IAfThreadBase, public virtual VolumeInterface {
public:
    static sp<IAfPlaybackThread> createBitPerfectThread(
            const sp<IAfThreadCallback>& afThreadCallback, AudioStreamOut* output,
            audio_io_handle_t id, bool systemReady);
    static sp<IAfPlaybackThread> createDirectOutputThread(
            const sp<IAfThreadCallback>& afThreadCallback, AudioStreamOut* output,
            audio_io_handle_t id, bool systemReady, const audio_offload_info_t& offloadInfo);
    static sp<IAfPlaybackThread> createMixerThread(
            const sp<IAfThreadCallback>& afThreadCallback, AudioStreamOut* output,
            audio_io_handle_t id, bool systemReady, type_t type = MIXER,
            audio_config_base_t* mixerConfig = nullptr);
    static sp<IAfPlaybackThread> createOffloadThread(
            const sp<IAfThreadCallback>& afThreadCallback, AudioStreamOut* output,
            audio_io_handle_t id, bool systemReady, const audio_offload_info_t& offloadInfo);
    static sp<IAfPlaybackThread> createSpatializerThread(
            const sp<IAfThreadCallback>& afThreadCallback, AudioStreamOut* output,
            audio_io_handle_t id, bool systemReady, audio_config_base_t* mixerConfig);
    static constexpr int8_t kMaxTrackStopRetriesOffload = 2;
    enum mixer_state {
        MIXER_IDLE,
        MIXER_TRACKS_ENABLED,
        MIXER_TRACKS_READY,
        MIXER_DRAIN_TRACK,
        MIXER_DRAIN_ALL,
    };
    virtual uint32_t latency() const = 0;
    virtual uint32_t& fastTrackAvailMask_l() = 0;
    virtual sp<IAfTrack> createTrack_l(
            const sp<Client>& client,
            audio_stream_type_t streamType,
            const audio_attributes_t& attr,
            uint32_t* sampleRate,
            audio_format_t format,
            audio_channel_mask_t channelMask,
            size_t* pFrameCount,
            size_t* pNotificationFrameCount,
            uint32_t notificationsPerBuffer,
            float speed,
            const sp<IMemory>& sharedBuffer,
            audio_session_t sessionId,
            audio_output_flags_t* flags,
            pid_t creatorPid,
            const AttributionSourceState& attributionSource,
            pid_t tid,
            status_t* status ,
            audio_port_handle_t portId,
            const sp<media::IAudioTrackCallback>& callback,
            bool isSpatialized,
            bool isBitPerfect)
            REQUIRES(audio_utils::AudioFlinger_Mutex) = 0;
    virtual status_t addTrack_l(const sp<IAfTrack>& track) = 0;
    virtual bool destroyTrack_l(const sp<IAfTrack>& track) = 0;
    virtual bool isTrackActive(const sp<IAfTrack>& track) const = 0;
    virtual void addOutputTrack_l(const sp<IAfTrack>& track) = 0;
    virtual AudioStreamOut* getOutput_l() const = 0;
    virtual AudioStreamOut* getOutput() const = 0;
    virtual AudioStreamOut* clearOutput() = 0;
    virtual void suspend() = 0;
    virtual void restore() = 0;
    virtual bool isSuspended() const = 0;
    virtual status_t getRenderPosition(uint32_t* halFrames, uint32_t* dspFrames) const = 0;
    virtual float* sinkBuffer() const = 0;
    virtual status_t attachAuxEffect(const sp<IAfTrack>& track, int EffectId) = 0;
    virtual status_t attachAuxEffect_l(const sp<IAfTrack>& track, int EffectId) = 0;
    virtual bool invalidateTracks_l(audio_stream_type_t streamType) = 0;
    virtual bool invalidateTracks_l(std::set<audio_port_handle_t>& portIds) = 0;
    virtual void invalidateTracks(audio_stream_type_t streamType) = 0;
    virtual void invalidateTracks(std::set<audio_port_handle_t>& portIds) = 0;
    virtual status_t getTimestamp_l(AudioTimestamp& timestamp) = 0;
    virtual void addPatchTrack(const sp<IAfPatchTrack>& track) = 0;
    virtual void deletePatchTrack(const sp<IAfPatchTrack>& track) = 0;
    virtual int64_t computeWaitTimeNs_l() const = 0;
    virtual bool isTrackAllowed_l(
            audio_channel_mask_t channelMask, audio_format_t format, audio_session_t sessionId,
            uid_t uid) const = 0;
    virtual bool supportsHapticPlayback() const = 0;
    virtual void setDownStreamPatch(const struct audio_patch* patch) = 0;
    virtual IAfTrack* getTrackById_l(audio_port_handle_t trackId) = 0;
    virtual bool hasMixer() const = 0;
    virtual status_t setRequestedLatencyMode(audio_latency_mode_t mode) = 0;
    virtual status_t getSupportedLatencyModes(std::vector<audio_latency_mode_t>* modes) = 0;
    virtual status_t setBluetoothVariableLatencyEnabled(bool enabled) = 0;
    virtual void setStandby() = 0;
    virtual void setStandby_l() = 0;
    virtual bool waitForHalStart() = 0;
    virtual bool hasFastMixer() const = 0;
    virtual FastTrackUnderruns getFastTrackUnderruns(size_t fastIndex) const = 0;
    virtual const std::atomic<int64_t>& framesWritten() const = 0;
    virtual bool usesHwAvSync() const = 0;
};
class IAfDirectOutputThread : public virtual IAfPlaybackThread {
public:
    virtual status_t selectPresentation(int presentationId, int programId) = 0;
};
class IAfDuplicatingThread : public virtual IAfPlaybackThread {
public:
    static sp<IAfDuplicatingThread> create(
            const sp<IAfThreadCallback>& afThreadCallback, IAfPlaybackThread* mainThread,
            audio_io_handle_t id, bool systemReady);
    virtual void addOutputTrack(IAfPlaybackThread* thread) = 0;
    virtual uint32_t waitTimeMs() const = 0;
    virtual void removeOutputTrack(IAfPlaybackThread* thread) = 0;
};
class IAfRecordThread : public virtual IAfThreadBase {
public:
    static sp<IAfRecordThread> create(
            const sp<IAfThreadCallback>& afThreadCallback, AudioStreamIn* input,
            audio_io_handle_t id, bool systemReady);
    virtual sp<IAfRecordTrack> createRecordTrack_l(
            const sp<Client>& client,
            const audio_attributes_t& attr,
            uint32_t* pSampleRate,
            audio_format_t format,
            audio_channel_mask_t channelMask,
            size_t* pFrameCount,
            audio_session_t sessionId,
            size_t* pNotificationFrameCount,
            pid_t creatorPid,
            const AttributionSourceState& attributionSource,
            audio_input_flags_t* flags,
            pid_t tid,
            status_t* status ,
            audio_port_handle_t portId,
            int32_t maxSharedAudioHistoryMs)
            REQUIRES(audio_utils::AudioFlinger_Mutex) = 0;
    virtual void destroyTrack_l(const sp<IAfRecordTrack>& track) = 0;
    virtual void removeTrack_l(const sp<IAfRecordTrack>& track) = 0;
    virtual status_t start(
            IAfRecordTrack* recordTrack, AudioSystem::sync_event_t event,
            audio_session_t triggerSession) = 0;
    virtual bool stop(IAfRecordTrack* recordTrack) = 0;
    virtual AudioStreamIn* getInput() const = 0;
    virtual AudioStreamIn* clearInput() = 0;
    virtual status_t getActiveMicrophones(
            std::vector<media::MicrophoneInfoFw>* activeMicrophones) const = 0;
    virtual status_t setPreferredMicrophoneDirection(audio_microphone_direction_t direction) = 0;
    virtual status_t setPreferredMicrophoneFieldDimension(float zoom) = 0;
    virtual void addPatchTrack(const sp<IAfPatchRecord>& record) = 0;
    virtual void deletePatchTrack(const sp<IAfPatchRecord>& record) = 0;
    virtual bool fastTrackAvailable() const = 0;
    virtual void setFastTrackAvailable(bool available) = 0;
    virtual void setRecordSilenced(audio_port_handle_t portId, bool silenced) = 0;
    virtual bool hasFastCapture() const = 0;
    virtual void checkBtNrec() = 0;
    virtual uint32_t getInputFramesLost() const = 0;
    virtual status_t shareAudioHistory(
            const std::string& sharedAudioPackageName,
            audio_session_t sharedSessionId = AUDIO_SESSION_NONE,
            int64_t sharedAudioStartMs = -1) = 0;
    virtual void resetAudioHistory_l() = 0;
};
class IAfMmapThread : public virtual IAfThreadBase {
public:
    static sp<MmapStreamInterface> createMmapStreamInterfaceAdapter(
            const sp<IAfMmapThread>& mmapThread);
    virtual void configure(
            const audio_attributes_t* attr,
            audio_stream_type_t streamType,
            audio_session_t sessionId,
            const sp<MmapStreamCallback>& callback,
            audio_port_handle_t deviceId,
            audio_port_handle_t portId) = 0;
    virtual void disconnect() = 0;
    virtual status_t createMmapBuffer(
            int32_t minSizeFrames, struct audio_mmap_buffer_info* info) = 0;
    virtual status_t getMmapPosition(struct audio_mmap_position* position) const = 0;
    virtual status_t start(
            const AudioClient& client, const audio_attributes_t* attr,
            audio_port_handle_t* handle) = 0;
    virtual status_t stop(audio_port_handle_t handle) = 0;
    virtual status_t standby() = 0;
    virtual status_t getExternalPosition(uint64_t* position, int64_t* timeNanos) const = 0;
    virtual status_t reportData(const void* buffer, size_t frameCount) = 0;
    virtual void invalidateTracks(std::set<audio_port_handle_t>& portIds) = 0;
    virtual void setRecordSilenced(audio_port_handle_t portId, bool silenced) = 0;
    virtual sp<IAfMmapPlaybackThread> asIAfMmapPlaybackThread() { return nullptr; }
    virtual sp<IAfMmapCaptureThread> asIAfMmapCaptureThread() { return nullptr; }
};
class IAfMmapPlaybackThread : public virtual IAfMmapThread, public virtual VolumeInterface {
public:
    static sp<IAfMmapPlaybackThread> create(
            const sp<IAfThreadCallback>& afThreadCallback, audio_io_handle_t id,
            AudioHwDevice* hwDev, AudioStreamOut* output, bool systemReady);
    virtual AudioStreamOut* clearOutput() = 0;
};
class IAfMmapCaptureThread : public virtual IAfMmapThread {
public:
    static sp<IAfMmapCaptureThread> create(
            const sp<IAfThreadCallback>& afThreadCallback, audio_io_handle_t id,
            AudioHwDevice* hwDev, AudioStreamIn* input, bool systemReady);
    virtual AudioStreamIn* clearInput() = 0;
};
}
