       
#include "Client.h"
#include "DeviceEffectManager.h"
#include "IAfEffect.h"
#include "IAfPatchPanel.h"
#include "IAfThread.h"
#include "IAfTrack.h"
#include "MelReporter.h"
#include "PatchCommandThread.h"
#include <audio_utils/FdToString.h>
#include <audio_utils/SimpleLog.h>
#include <media/IAudioFlinger.h>
#include <media/MediaMetricsItem.h>
#include <media/audiohal/DevicesFactoryHalInterface.h>
#include <mediautils/ServiceUtilities.h>
#include <mediautils/Synchronization.h>
#include <utils/KeyedVector.h>
#include <utils/String16.h>
#include <atomic>
#include <functional>
#include <map>
#include <optional>
#include <set>
namespace android {
class AudioFlinger
    : public AudioFlingerServerAdapter::Delegate
    , public IAfClientCallback
<<<<<<< HEAD
    , public IAfDeviceEffectManagerCallback
    , public IAfMelReporterCallback
    , public IAfPatchPanelCallback
    , public IAfThreadCallback
||||||| 95a74a0beb
=======
    , public IAfDeviceEffectManagerCallback
>>>>>>> 2a665bc8
{
    friend class sp<AudioFlinger>;
<<<<<<< HEAD
||||||| 95a74a0beb
    friend class DeviceEffectManager;
    friend class DeviceEffectManagerCallback;
    friend class MelReporter;
    friend class PatchPanel;
    friend class DirectOutputThread;
    friend class MixerThread;
    friend class MmapPlaybackThread;
    friend class MmapThread;
    friend class PlaybackThread;
    friend class RecordThread;
    friend class ThreadBase;
=======
    friend class MelReporter;
    friend class PatchPanel;
    friend class DirectOutputThread;
    friend class MixerThread;
    friend class MmapPlaybackThread;
    friend class MmapThread;
    friend class PlaybackThread;
    friend class RecordThread;
    friend class ThreadBase;
>>>>>>> 2a665bc8
public:
    static void instantiate() ANDROID_API;
private:
    status_t dump(int fd, const Vector<String16>& args) final;
    status_t createTrack(const media::CreateTrackRequest& input,
                         media::CreateTrackResponse& output) final;
    status_t createRecord(const media::CreateRecordRequest& input,
                          media::CreateRecordResponse& output) final;
    uint32_t sampleRate(audio_io_handle_t ioHandle) const final;
    audio_format_t format(audio_io_handle_t output) const final;
    size_t frameCount(audio_io_handle_t ioHandle) const final;
    size_t frameCountHAL(audio_io_handle_t ioHandle) const final;
    uint32_t latency(audio_io_handle_t output) const final;
    status_t setMasterVolume(float value) final;
    status_t setMasterMute(bool muted) final;
    float masterVolume() const final;
    bool masterMute() const final;
    status_t setMasterBalance(float balance) final;
    status_t getMasterBalance(float* balance) const final;
    status_t setStreamVolume(audio_stream_type_t stream, float value,
            audio_io_handle_t output) final;
    status_t setStreamMute(audio_stream_type_t stream, bool muted) final;
    float streamVolume(audio_stream_type_t stream,
            audio_io_handle_t output) const final;
    bool streamMute(audio_stream_type_t stream) const final;
    status_t setMode(audio_mode_t mode) final;
    status_t setMicMute(bool state) final;
    bool getMicMute() const final;
    void setRecordSilenced(audio_port_handle_t portId, bool silenced) final;
    status_t setParameters(audio_io_handle_t ioHandle, const String8& keyValuePairs) final;
    String8 getParameters(audio_io_handle_t ioHandle, const String8& keys) const final;
    void registerClient(const sp<media::IAudioFlingerClient>& client) final;
    size_t getInputBufferSize(uint32_t sampleRate, audio_format_t format,
            audio_channel_mask_t channelMask) const final;
    status_t openOutput(const media::OpenOutputRequest& request,
            media::OpenOutputResponse* response) final;
    audio_io_handle_t openDuplicateOutput(audio_io_handle_t output1,
            audio_io_handle_t output2) final;
    status_t closeOutput(audio_io_handle_t output) final;
    status_t suspendOutput(audio_io_handle_t output) final;
    status_t restoreOutput(audio_io_handle_t output) final;
    status_t openInput(const media::OpenInputRequest& request,
            media::OpenInputResponse* response) final;
    status_t closeInput(audio_io_handle_t input) final;
    status_t setVoiceVolume(float volume) final;
    status_t getRenderPosition(uint32_t* halFrames, uint32_t* dspFrames,
            audio_io_handle_t output) const final;
    uint32_t getInputFramesLost(audio_io_handle_t ioHandle) const final;
    audio_unique_id_t newAudioUniqueId(audio_unique_id_use_t use) final;
    void acquireAudioSessionId(audio_session_t audioSession, pid_t pid, uid_t uid) final;
    void releaseAudioSessionId(audio_session_t audioSession, pid_t pid) final;
    status_t queryNumberEffects(uint32_t* numEffects) const final;
    status_t queryEffect(uint32_t index, effect_descriptor_t* descriptor) const final;
    status_t getEffectDescriptor(const effect_uuid_t* pUuid,
            const effect_uuid_t* pTypeUuid,
            uint32_t preferredTypeFlag,
            effect_descriptor_t* descriptor) const final;
    status_t createEffect(const media::CreateEffectRequest& request,
            media::CreateEffectResponse* response) final;
    status_t moveEffects(audio_session_t sessionId, audio_io_handle_t srcOutput,
            audio_io_handle_t dstOutput) final;
    void setEffectSuspended(int effectId,
            audio_session_t sessionId,
            bool suspended) final;
    audio_module_handle_t loadHwModule(const char* name) final;
    uint32_t getPrimaryOutputSamplingRate() const final;
    size_t getPrimaryOutputFrameCount() const final;
    status_t setLowRamDevice(bool isLowRamDevice, int64_t totalMemory) final;
    status_t getAudioPort(struct audio_port_v7* port) const final;
    status_t createAudioPatch(const struct audio_patch *patch,
            audio_patch_handle_t* handle) final;
    status_t releaseAudioPatch(audio_patch_handle_t handle) final;
    status_t listAudioPatches(unsigned int* num_patches,
            struct audio_patch* patches) const final;
    status_t setAudioPortConfig(const struct audio_port_config* config) final;
    audio_hw_sync_t getAudioHwSyncForSession(audio_session_t sessionId) final;
    status_t systemReady() final;
    status_t audioPolicyReady() final { mAudioPolicyReady.store(true); return NO_ERROR; }
    status_t getMicrophones(std::vector<media::MicrophoneInfoFw>* microphones) const final;
    status_t setAudioHalPids(const std::vector<pid_t>& pids) final;
    status_t setVibratorInfos(const std::vector<media::AudioVibratorInfo>& vibratorInfos) final;
    status_t updateSecondaryOutputs(
            const TrackSecondaryOutputsMap& trackSecondaryOutputs) final;
    status_t getMmapPolicyInfos(
            media::audio::common::AudioMMapPolicyType policyType,
            std::vector<media::audio::common::AudioMMapPolicyInfo>* policyInfos) final;
    int32_t getAAudioMixerBurstCount() const final;
    int32_t getAAudioHardwareBurstMinUsec() const final;
    status_t setDeviceConnectedState(const struct audio_port_v7* port,
            media::DeviceConnectedState state) final;
    status_t setSimulateDeviceConnections(bool enabled) final;
    status_t setRequestedLatencyMode(
            audio_io_handle_t output, audio_latency_mode_t mode) final;
    status_t getSupportedLatencyModes(audio_io_handle_t output,
            std::vector<audio_latency_mode_t>* modes) const final;
    status_t setBluetoothVariableLatencyEnabled(bool enabled) final;
    status_t isBluetoothVariableLatencyEnabled(bool* enabled) const final;
    status_t supportsBluetoothVariableLatency(bool* support) const final;
    status_t getSoundDoseInterface(const sp<media::ISoundDoseCallback>& callback,
            sp<media::ISoundDose>* soundDose) const final;
    status_t invalidateTracks(const std::vector<audio_port_handle_t>& portIds) final;
    status_t getAudioPolicyConfig(media::AudioPolicyConfig* config) final;
    status_t onTransactWrapper(TransactionCode code, const Parcel& data, uint32_t flags,
            const std::function<status_t()>& delegate) final;
    Mutex& clientMutex() const final { return mClientLock; }
    void removeClient_l(pid_t pid) final;
    void removeNotificationClient(pid_t pid) final;
    status_t moveAuxEffectToIo(
            int effectId,
            const sp<IAfPlaybackThread>& dstThread,
            sp<IAfPlaybackThread>* srcThread) final;
<<<<<<< HEAD
    bool isAudioPolicyReady() const final { return mAudioPolicyReady.load(); }
    const sp<PatchCommandThread>& getPatchCommandThread() final { return mPatchCommandThread; }
    status_t addEffectToHal(
            const struct audio_port_config* device, const sp<EffectHalInterface>& effect) final;
    status_t removeEffectFromHal(
            const struct audio_port_config* device, const sp<EffectHalInterface>& effect) final;
    Mutex& mutex() const final { return mLock; }
    sp<IAfThreadBase> checkOutputThread_l(audio_io_handle_t ioHandle) const final REQUIRES(mLock);
    void closeThreadInternal_l(const sp<IAfPlaybackThread>& thread) final;
    void closeThreadInternal_l(const sp<IAfRecordThread>& thread) final;
    IAfPlaybackThread* primaryPlaybackThread_l() const final;
    IAfPlaybackThread* checkPlaybackThread_l(audio_io_handle_t output) const final;
    IAfRecordThread* checkRecordThread_l(audio_io_handle_t input) const final;
    IAfMmapThread* checkMmapThread_l(audio_io_handle_t io) const final;
    void lock() const final ACQUIRE(mLock) { mLock.lock(); }
    void unlock() const final RELEASE(mLock) { mLock.unlock(); }
    sp<IAfThreadBase> openInput_l(audio_module_handle_t module,
            audio_io_handle_t* input,
            audio_config_t* config,
            audio_devices_t device,
            const char* address,
            audio_source_t source,
            audio_input_flags_t flags,
            audio_devices_t outputDevice,
            const String8& outputDeviceAddress) final;
    sp<IAfThreadBase> openOutput_l(audio_module_handle_t module,
            audio_io_handle_t* output,
            audio_config_t* halConfig,
            audio_config_base_t* mixerConfig,
            audio_devices_t deviceType,
            const String8& address,
            audio_output_flags_t flags) final;
    const DefaultKeyedVector<audio_module_handle_t, AudioHwDevice*>&
            getAudioHwDevs_l() const final { return mAudioHwDevs; }
    void updateDownStreamPatches_l(const struct audio_patch* patch,
            const std::set<audio_io_handle_t>& streams) final;
    void updateOutDevicesForRecordThreads_l(const DeviceDescriptorBaseVector& devices) final;
    bool isNonOffloadableGlobalEffectEnabled_l() const final;
    bool btNrecIsOff() const final { return mBtNrecIsOff.load(); }
    float masterVolume_l() const final;
    bool masterMute_l() const final;
    float getMasterBalance_l() const;
    bool streamMute_l(audio_stream_type_t stream) const final { return mStreamTypes[stream].mute; }
    audio_mode_t getMode() const final { return mMode; }
    bool isLowRamDevice() const final { return mIsLowRamDevice; }
    uint32_t getScreenState() const final { return mScreenState; }
    std::optional<media::AudioVibratorInfo> getDefaultVibratorInfo_l() const final;
    const sp<IAfPatchPanel>& getPatchPanel() const final { return mPatchPanel; }
    const sp<MelReporter>& getMelReporter() const final { return mMelReporter; }
    const sp<EffectsFactoryHalInterface>& getEffectsFactoryHal() const final {
        return mEffectsFactoryHal;
    }
    sp<IAudioManager> getOrCreateAudioManager() final;
    bool updateOrphanEffectChains(const sp<IAfEffectModule>& effect) final;
    status_t moveEffectChain_l(audio_session_t sessionId,
            IAfPlaybackThread* srcThread, IAfPlaybackThread* dstThread) final;
    void requestLogMerge() final;
    sp<NBLog::Writer> newWriter_l(size_t size, const char *name) final;
    void unregisterWriter(const sp<NBLog::Writer>& writer) final;
    sp<audioflinger::SyncEvent> createSyncEvent(AudioSystem::sync_event_t type,
            audio_session_t triggerSession,
            audio_session_t listenerSession,
            const audioflinger::SyncEventCallback& callBack,
            const wp<IAfTrackBase>& cookie) final;
    void ioConfigChanged(audio_io_config_event_t event,
            const sp<AudioIoDescriptor>& ioDesc,
            pid_t pid = 0) final;
    void onNonOffloadableGlobalEffectEnable() final;
    void onSupportedLatencyModesChanged(
            audio_io_handle_t output, const std::vector<audio_latency_mode_t>& modes) final;
||||||| 95a74a0beb
    bool isAudioPolicyReady() const { return mAudioPolicyReady.load(); }
=======
    bool isAudioPolicyReady() const final { return mAudioPolicyReady.load(); }
    const sp<PatchCommandThread>& getPatchCommandThread() final { return mPatchCommandThread; }
    status_t addEffectToHal(
            const struct audio_port_config* device, const sp<EffectHalInterface>& effect) final;
    status_t removeEffectFromHal(
            const struct audio_port_config* device, const sp<EffectHalInterface>& effect) final;
>>>>>>> 2a665bc8
    status_t listAudioPorts(unsigned int* num_ports, struct audio_port* ports) const;
    sp<EffectsFactoryHalInterface> getEffectsFactory();
public:
    static inline std::atomic<AudioFlinger*> gAudioFlinger = nullptr;
    status_t openMmapStream(MmapStreamInterface::stream_direction_t direction,
                            const audio_attributes_t *attr,
                            audio_config_base_t *config,
                            const AudioClient& client,
                            audio_port_handle_t *deviceId,
                            audio_session_t *sessionId,
                            const sp<MmapStreamCallback>& callback,
                            sp<MmapStreamInterface>& interface,
                            audio_port_handle_t *handle);
<<<<<<< HEAD
||||||| 95a74a0beb
    static os::HapticScale onExternalVibrationStart(
        const sp<os::ExternalVibration>& externalVibration);
    static void onExternalVibrationStop(const sp<os::ExternalVibration>& externalVibration);
    status_t addEffectToHal(
            const struct audio_port_config *device, const sp<EffectHalInterface>& effect);
    status_t removeEffectFromHal(
            const struct audio_port_config *device, const sp<EffectHalInterface>& effect);
    void updateDownStreamPatches_l(const struct audio_patch *patch,
                                   const std::set<audio_io_handle_t>& streams);
    std::optional<media::AudioVibratorInfo> getDefaultVibratorInfo_l();
=======
    static os::HapticScale onExternalVibrationStart(
        const sp<os::ExternalVibration>& externalVibration);
    static void onExternalVibrationStop(const sp<os::ExternalVibration>& externalVibration);
    void updateDownStreamPatches_l(const struct audio_patch *patch,
                                   const std::set<audio_io_handle_t>& streams);
    std::optional<media::AudioVibratorInfo> getDefaultVibratorInfo_l();
>>>>>>> 2a665bc8
private:
    static const size_t kLogMemorySize = 400 * 1024;
    sp<MemoryDealer> mLogMemoryDealer;
    Vector< sp<NBLog::Writer> > mUnregisteredWriters;
    Mutex mUnregisteredWritersLock;
                            AudioFlinger() ANDROID_API;
    ~AudioFlinger() override;
    status_t initCheck() const { return mPrimaryHardwareDev == NULL ?
                                                        NO_INIT : NO_ERROR; }
    void onFirstRef() override;
    AudioHwDevice* findSuitableHwDev_l(audio_module_handle_t module,
                                                audio_devices_t deviceType);
    std::atomic_uint32_t mScreenState{};
    void dumpPermissionDenial(int fd, const Vector<String16>& args);
    void dumpClients(int fd, const Vector<String16>& args);
    void dumpInternals(int fd, const Vector<String16>& args);
    SimpleLog mThreadLog{16};
    void dumpToThreadLog_l(const sp<IAfThreadBase>& thread);
    class NotificationClient : public IBinder::DeathRecipient {
    public:
                            NotificationClient(const sp<AudioFlinger>& audioFlinger,
                                                const sp<media::IAudioFlingerClient>& client,
                                                pid_t pid,
                                                uid_t uid);
        virtual ~NotificationClient();
                sp<media::IAudioFlingerClient> audioFlingerClient() const { return mAudioFlingerClient; }
                pid_t getPid() const { return mPid; }
                uid_t getUid() const { return mUid; }
                virtual void binderDied(const wp<IBinder>& who);
    private:
        DISALLOW_COPY_AND_ASSIGN(NotificationClient);
        const sp<AudioFlinger> mAudioFlinger;
        const pid_t mPid;
        const uid_t mUid;
        const sp<media::IAudioFlingerClient> mAudioFlingerClient;
    };
    class MediaLogNotifier : public Thread {
    public:
        MediaLogNotifier();
        void requestMerge();
    private:
        virtual bool threadLoop() override;
        bool mPendingRequests;
        Mutex mMutex;
        Condition mCond;
        static const int kPostTriggerSleepPeriod = 1000000;
    };
    const sp<MediaLogNotifier> mMediaLogNotifier;
    template <typename T>
    static audio_io_handle_t findIoHandleBySessionId_l(
            audio_session_t sessionId, const T& threads) {
        audio_io_handle_t io = AUDIO_IO_HANDLE_NONE;
        for (size_t i = 0; i < threads.size(); i++) {
            const uint32_t sessionType = threads.valueAt(i)->hasAudioSession(sessionId);
            if (sessionType != 0) {
                io = threads.keyAt(i);
                if ((sessionType & IAfThreadBase::EFFECT_SESSION) != 0) {
                    break;
                }
            }
        }
        return io;
    }
    IAfThreadBase* checkThread_l(audio_io_handle_t ioHandle) const;
    IAfPlaybackThread* checkMixerThread_l(audio_io_handle_t output) const;
              sp<VolumeInterface> getVolumeInterface_l(audio_io_handle_t output) const;
              std::vector<sp<VolumeInterface>> getAllVolumeInterfaces_l() const;
    void closeOutputFinish(const sp<IAfPlaybackThread>& thread);
    void closeInputFinish(const sp<IAfRecordThread>& thread);
<<<<<<< HEAD
    audio_unique_id_t nextUniqueId(audio_unique_id_use_t use) final;
||||||| 95a74a0beb
              audio_unique_id_t nextUniqueId(audio_unique_id_use_t use);
              status_t moveEffectChain_l(audio_session_t sessionId,
            IAfPlaybackThread* srcThread, IAfPlaybackThread* dstThread);
=======
    audio_unique_id_t nextUniqueId(audio_unique_id_use_t use) final;
              status_t moveEffectChain_l(audio_session_t sessionId,
            IAfPlaybackThread* srcThread, IAfPlaybackThread* dstThread);
>>>>>>> 2a665bc8
              DeviceTypeSet primaryOutputDevice_l() const;
              IAfPlaybackThread* fastPlaybackThread_l() const;
              sp<IAfThreadBase> getEffectThread_l(audio_session_t sessionId, int effectId);
              IAfThreadBase* hapticPlaybackThread_l() const;
              void updateSecondaryOutputsForTrack_l(
                      IAfTrack* track,
                      IAfPlaybackThread* thread,
                      const std::vector<audio_io_handle_t>& secondaryOutputs) const;
                bool isSessionAcquired_l(audio_session_t audioSession);
                status_t putOrphanEffectChain_l(const sp<IAfEffectChain>& chain);
                sp<IAfEffectChain> getOrphanEffectChain_l(audio_session_t session);
                std::vector< sp<IAfEffectModule> > purgeStaleEffects_l();
                void broadcastParametersToRecordThreads_l(const String8& keyValuePairs);
                void forwardParametersToDownstreamPatches_l(
                        audio_io_handle_t upStream, const String8& keyValuePairs,
            const std::function<bool(const sp<IAfPlaybackThread>&)>& useThread = nullptr);
    struct AudioSessionRef {
        AudioSessionRef(audio_session_t sessionid, pid_t pid, uid_t uid) :
            mSessionid(sessionid), mPid(pid), mUid(uid), mCnt(1) {}
        const audio_session_t mSessionid;
        const pid_t mPid;
        const uid_t mUid;
        int mCnt;
    };
    mutable Mutex mLock;
    mutable Mutex mClientLock;
                DefaultKeyedVector< pid_t, wp<Client> > mClients;
                mutable Mutex mHardwareLock;
                AudioHwDevice* mPrimaryHardwareDev;
                DefaultKeyedVector<audio_module_handle_t, AudioHwDevice*> mAudioHwDevs;
                sp<DevicesFactoryHalInterface> mDevicesFactoryHal;
                sp<DevicesFactoryHalCallback> mDevicesFactoryHalCallback;
    enum hardware_call_state {
        AUDIO_HW_IDLE = 0,
        AUDIO_HW_INIT,
        AUDIO_HW_OUTPUT_OPEN,
        AUDIO_HW_OUTPUT_CLOSE,
        AUDIO_HW_INPUT_OPEN,
        AUDIO_HW_INPUT_CLOSE,
        AUDIO_HW_STANDBY,
        AUDIO_HW_SET_MASTER_VOLUME,
        AUDIO_HW_GET_ROUTING,
        AUDIO_HW_SET_ROUTING,
        AUDIO_HW_GET_MODE,
        AUDIO_HW_SET_MODE,
        AUDIO_HW_GET_MIC_MUTE,
        AUDIO_HW_SET_MIC_MUTE,
        AUDIO_HW_SET_VOICE_VOLUME,
        AUDIO_HW_SET_PARAMETER,
        AUDIO_HW_GET_INPUT_BUFFER_SIZE,
        AUDIO_HW_GET_MASTER_VOLUME,
        AUDIO_HW_GET_PARAMETER,
        AUDIO_HW_SET_MASTER_MUTE,
        AUDIO_HW_GET_MASTER_MUTE,
        AUDIO_HW_GET_MICROPHONES,
        AUDIO_HW_SET_CONNECTED_STATE,
        AUDIO_HW_SET_SIMULATE_CONNECTIONS,
    };
    mutable hardware_call_state mHardwareStatus;
    DefaultKeyedVector<audio_io_handle_t, sp<IAfPlaybackThread>> mPlaybackThreads;
                stream_type_t mStreamTypes[AUDIO_STREAM_CNT];
                float mMasterVolume;
                bool mMasterMute;
                float mMasterBalance = 0.f;
    DefaultKeyedVector<audio_io_handle_t, sp<IAfRecordThread>> mRecordThreads;
                DefaultKeyedVector< pid_t, sp<NotificationClient> > mNotificationClients;
                volatile atomic_uint_fast32_t mNextUniqueIds[AUDIO_UNIQUE_ID_USE_MAX];
                audio_mode_t mMode;
                std::atomic_bool mBtNrecIsOff;
                Vector<AudioSessionRef*> mAudioSessionRefs;
                AudioHwDevice* loadHwModule_l(const char *name);
                std::list<sp<audioflinger::SyncEvent>> mPendingSyncEvents;
                DefaultKeyedVector<audio_session_t, sp<IAfEffectChain>> mOrphanEffectChains;
                DefaultKeyedVector< audio_session_t , audio_hw_sync_t >mHwAvSyncIds;
    DefaultKeyedVector<audio_io_handle_t, sp<IAfMmapThread>> mMmapThreads;
    sp<Client> registerPid(pid_t pid);
    status_t closeOutput_nonvirtual(audio_io_handle_t output);
    status_t closeInput_nonvirtual(audio_io_handle_t input);
    void setAudioHwSyncForSession_l(IAfPlaybackThread* thread, audio_session_t sessionId);
    status_t checkStreamType(audio_stream_type_t stream) const;
    void filterReservedParameters(String8& keyValuePairs, uid_t callingUid);
    void logFilteredParameters(size_t originalKVPSize, const String8& originalKVPs,
                                      size_t rejectedKVPSize, const String8& rejectedKVPs,
                                      uid_t callingUid);
    size_t getClientSharedHeapSize() const;
    std::atomic<bool> mIsLowRamDevice;
    bool mIsDeviceTypeKnown;
    int64_t mTotalMemory;
    std::atomic<size_t> mClientSharedHeapSize;
    static constexpr size_t kMinimumClientSharedHeapSizeBytes = 1024 * 1024;
    nsecs_t mGlobalEffectEnableTime;
                sp<IAfPatchPanel> mPatchPanel;
    sp<EffectsFactoryHalInterface> mEffectsFactoryHal;
    const sp<PatchCommandThread> mPatchCommandThread;
<<<<<<< HEAD
                sp<DeviceEffectManager> mDeviceEffectManager;
                sp<MelReporter> mMelReporter;
||||||| 95a74a0beb
    sp<DeviceEffectManager> mDeviceEffectManager;
    sp<MelReporter> mMelReporter;
=======
                sp<DeviceEffectManager> mDeviceEffectManager;
    sp<MelReporter> mMelReporter;
>>>>>>> 2a665bc8
    bool mSystemReady;
    std::atomic_bool mAudioPolicyReady{};
    mediautils::UidInfo mUidInfo;
    SimpleLog mRejectedSetParameterLog;
    SimpleLog mAppSetParameterLog;
    SimpleLog mSystemSetParameterLog;
    std::vector<media::AudioVibratorInfo> mAudioVibratorInfos;
    static inline constexpr const char *mMetricsId = AMEDIAMETRICS_KEY_AUDIO_FLINGER;
    std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos;
    int32_t mAAudioBurstsPerBuffer = 0;
    int32_t mAAudioHwBurstMinMicros = 0;
    mediautils::atomic_sp<IAudioManager> mAudioManager;
    std::atomic_bool mBluetoothLatencyModesEnabled;
};
}
