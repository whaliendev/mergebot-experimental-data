       
#include "Client.h"
#include "DeviceEffectManager.h"
#include "EffectConfiguration.h"
#include "IAfEffect.h"
#include "IAfPatchPanel.h"
#include "IAfThread.h"
#include "IAfTrack.h"
#include "MelReporter.h"
#include "PatchCommandThread.h"
#include <audio_utils/mutex.h>
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
    , public IAfDeviceEffectManagerCallback
    , public IAfMelReporterCallback
    , public IAfPatchPanelCallback
    , public IAfThreadCallback
{
    friend class sp<AudioFlinger>;
public:
static void instantiate()private:
    std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos;
void dumpInternals_l(int fd, const Vector<String16>& args) status_t moveEffectChain_ll(audio_session_t sessionId,
            IAfPlaybackThread* srcThread, IAfPlaybackThread* dstThread) final
bool streamMute_l(audio_stream_type_t stream) const finalconst DefaultKeyedVector<audio_module_handle_t, AudioHwDevice*>&
            getAudioHwDevs_l() const final
    audio_utils::mutex& mutex() const final
                                                              () = delete;{
                                                              return mStreamTypes[stream].mute;
                                                              }
    audio_utils::mutex& clientMutex() const final
    status_t dump(int fd, const Vector<String16>& args) final
    status_t createTrack(const media::CreateTrackRequest& input,
            media::CreateTrackResponse& output) final
    status_t createRecord(const media::CreateRecordRequest& input,
            media::CreateRecordResponse& output) final
    uint32_t sampleRate(audio_io_handle_t ioHandle) const final
    audio_format_t format(audio_io_handle_t output) const final
    size_t frameCount(audio_io_handle_t ioHandle) const final
    size_t frameCountHAL(audio_io_handle_t ioHandle) const final
    uint32_t latency(audio_io_handle_t output) const final
    status_t setMasterVolume(float value) final
    status_t setMasterMute(bool muted) final
    float masterVolume() const final
    bool masterMute() const final
    status_t setMasterBalance(float balance) final
    status_t getMasterBalance(float* balance) const final
    status_t setStreamVolume(audio_stream_type_t stream, float value,
            audio_io_handle_t output) final
    status_t setStreamMute(audio_stream_type_t stream, bool muted) final
    float streamVolume(audio_stream_type_t stream,
            audio_io_handle_t output) const final
    bool streamMute(audio_stream_type_t stream) const final
    status_t setMode(audio_mode_t mode) final
    status_t setMicMute(bool state) final
    bool getMicMute() const final
    void setRecordSilenced(audio_port_handle_t portId, bool silenced) final
    status_t setParameters(audio_io_handle_t ioHandle, const String8& keyValuePairs) final
    String8 getParameters(audio_io_handle_t ioHandle, const String8& keys) const final
    void registerClient(const sp<media::IAudioFlingerClient>& client) final
    size_t getInputBufferSize(uint32_t sampleRate, audio_format_t format,
            audio_channel_mask_t channelMask) const final
    status_t openOutput(const media::OpenOutputRequest& request,
            media::OpenOutputResponse* response) final
    audio_io_handle_t openDuplicateOutput(audio_io_handle_t output1,
            audio_io_handle_t output2) final
    status_t closeOutput(audio_io_handle_t output) final
    status_t suspendOutput(audio_io_handle_t output) final
    status_t restoreOutput(audio_io_handle_t output) final
    status_t openInput(const media::OpenInputRequest& request,
            media::OpenInputResponse* response) final
    status_t closeInput(audio_io_handle_t input) final
    status_t setVoiceVolume(float volume) final
    status_t getRenderPosition(uint32_t* halFrames, uint32_t* dspFrames,
            audio_io_handle_t output) const final
    uint32_t getInputFramesLost(audio_io_handle_t ioHandle) const final
    audio_unique_id_t newAudioUniqueId(audio_unique_id_use_t use) final
    void acquireAudioSessionId(audio_session_t audioSession, pid_t pid, uid_t uid) final
    void releaseAudioSessionId(audio_session_t audioSession, pid_t pid) final
    status_t queryNumberEffects(uint32_t* numEffects) const final
    status_t queryEffect(uint32_t index, effect_descriptor_t* descriptor) const final
    status_t getEffectDescriptor(const effect_uuid_t* pUuid,
            const effect_uuid_t* pTypeUuid,
            uint32_t preferredTypeFlag,
            effect_descriptor_t* descriptor) const final
    status_t createEffect(const media::CreateEffectRequest& request,
            media::CreateEffectResponse* response) final
    status_t moveEffects(audio_session_t sessionId, audio_io_handle_t srcOutput,
            audio_io_handle_t dstOutput) final
    void setEffectSuspended(int effectId,
            audio_session_t sessionId,
            bool suspended) final
    audio_module_handle_t loadHwModule(const char* name) final
    uint32_t getPrimaryOutputSamplingRate() const final
    size_t getPrimaryOutputFrameCount() const final
    status_t setLowRamDevice(bool isLowRamDevice, int64_t totalMemory) final
    status_t getAudioPort(struct audio_port_v7* port) const final
    status_t createAudioPatch(const struct audio_patch *patch,
            audio_patch_handle_t* handle) final
    status_t releaseAudioPatch(audio_patch_handle_t handle) final
    status_t listAudioPatches(unsigned int* num_patches,
            struct audio_patch* patches) const final
    status_t setAudioPortConfig(const struct audio_port_config* config) final
    audio_hw_sync_t getAudioHwSyncForSession(audio_session_t sessionId) final
    status_t systemReady() final
    status_t audioPolicyReady() final { mAudioPolicyReady.store(true); return NO_ERROR; }
    status_t getMicrophones(std::vector<media::MicrophoneInfoFw>* microphones) const final
    status_t setAudioHalPids(const std::vector<pid_t>& pids) final
    status_t setVibratorInfos(const std::vector<media::AudioVibratorInfo>& vibratorInfos) final
    status_t updateSecondaryOutputs(
            const TrackSecondaryOutputsMap& trackSecondaryOutputs) final
    status_t getMmapPolicyInfos(
            media::audio::common::AudioMMapPolicyType policyType,
            std::vector<media::audio::common::AudioMMapPolicyInfo>* policyInfos) final
    int32_t getAAudioMixerBurstCount() const final
    int32_t getAAudioHardwareBurstMinUsec() const final
    status_t setDeviceConnectedState(const struct audio_port_v7* port,
            media::DeviceConnectedState state) final
    status_t setSimulateDeviceConnections(bool enabled) final
    status_t setRequestedLatencyMode(
            audio_io_handle_t output, audio_latency_mode_t mode) final
    status_t getSupportedLatencyModes(audio_io_handle_t output,
            std::vector<audio_latency_mode_t>* modes) const final
    status_t setBluetoothVariableLatencyEnabled(bool enabled) final
    status_t isBluetoothVariableLatencyEnabled(bool* enabled) const final
    status_t supportsBluetoothVariableLatency(bool* support) const final
    status_t getSoundDoseInterface(const sp<media::ISoundDoseCallback>& callback,
            sp<media::ISoundDose>* soundDose) const final
    status_t invalidateTracks(const std::vector<audio_port_handle_t>& portIds) final
    status_t getAudioPolicyConfig(media::AudioPolicyConfig* config) final
    status_t onTransactWrapper(TransactionCode code, const Parcel& data, uint32_t flags,
            const std::function<status_t()>& delegate) final
    void removeNotificationClient(pid_t pid) final
    status_t moveAuxEffectToIo(
            int effectId,
            const sp<IAfPlaybackThread>& dstThread,
            sp<IAfPlaybackThread>* srcThread) final
    bool isAudioPolicyReady() const final { return mAudioPolicyReady.load(); }
    const sp<PatchCommandThread>& getPatchCommandThread() final { return mPatchCommandThread; }
    status_t addEffectToHal(
            const struct audio_port_config* device, const sp<EffectHalInterface>& effect) final
    status_t removeEffectFromHal(
            const struct audio_port_config* device, const sp<EffectHalInterface>& effect) final
    std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos;
    void closeThreadInternal_l(const sp<IAfRecordThread>& thread) final;
    void closeThreadInternal_l(const sp<IAfRecordThread>& thread) final;
    IAfPlaybackThread* primaryPlaybackThread_l() const final
    IAfPlaybackThread* checkPlaybackThread_l(audio_io_handle_t output) const final
    IAfRecordThread* checkRecordThread_l(audio_io_handle_t input) const final
    IAfMmapThread* checkMmapThread_l(audio_io_handle_t io) const final
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
            audio_output_flags_t flags) final
    void updateOutDevicesForRecordThreads_l(const DeviceDescriptorBaseVector& devices) final
    bool isNonOffloadableGlobalEffectEnabled_l() const final
    bool btNrecIsOff() const final { return mBtNrecIsOff.load(); }
    float masterVolume_l() const final
    bool masterMute_l() const final
    float getMasterBalance_l() const
    uint32_t getScreenState() const final { return mScreenState; }
    const sp<IAfPatchPanel>& getPatchPanel() const final { return mPatchPanel; }
    const sp<MelReporter>& getMelReporter() const final { return mMelReporter; }
    const sp<EffectsFactoryHalInterface>& getEffectsFactoryHal() const final {
        return mEffectsFactoryHal;
    }
    sp<IAudioManager> getOrCreateAudioManager() final;
    bool updateOrphanEffectChains(const sp<IAfEffectModule>& effect) final
    void requestLogMerge() final;
    sp<NBLog::Writer> newWriter_l(size_t size, const char *name) final
    void unregisterWriter(const sp<NBLog::Writer>& writer) final;
    sp<audioflinger::SyncEvent> createSyncEvent(AudioSystem::sync_event_t type,
            audio_session_t triggerSession,
            audio_session_t listenerSession,
            const audioflinger::SyncEventCallback& callBack,
            const wp<IAfTrackBase>& cookie) final
    void ioConfigChanged(audio_io_config_event_t event,
            const sp<AudioIoDescriptor>& ioDesc,
            pid_t pid = 0) final
    void onNonOffloadableGlobalEffectEnable() final
    void onSupportedLatencyModesChanged(
            audio_io_handle_t output, const std::vector<audio_latency_mode_t>& modes) final
    status_t listAudioPorts(unsigned int* num_ports, struct audio_port* ports) const
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
            audio_port_handle_t *handle)
private:
    static const size_t kLogMemorySize = 400 * 1024;
sp<MemoryDealer> mLogMemoryDealer;
    Vector< sp<NBLog::Writer> > mUnregisteredWriters;
    audio_utils::mutex& unregisteredWritersMutex() const { return mUnregisteredWritersMutex; }
    mutable audio_utils::mutex mUnregisteredWritersMutex;
              AudioFlinger() std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos;
    ~AudioFlinger() override;
    status_t initCheck() const { return mPrimaryHardwareDev == NULL ?
                                                        NO_INIT : NO_ERROR; }
    void onFirstRef() override;
    AudioHwDevice* findSuitableHwDev_l(audio_module_handle_t module,
            audio_devices_t deviceType)
    std::atomic_uint32_t mScreenState{};
    void dumpPermissionDenial(int fd, const Vector<String16>& args);
void dumpClients_ll(int fd, const Vector<String16>& args)SimpleLog mThreadLog{16};
    void dumpToThreadLog_l(const sp<IAfThreadBase>& thread)
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
        audio_utils::mutex mMutex;
        audio_utils::condition_variable mCondition;
        static const int kPostTriggerSleepPeriod = 1000000;
    };
    const sp<MediaLogNotifier> mMediaLogNotifier = sp<MediaLogNotifier>::make();
    IAfPlaybackThread* checkMixerThread_l(audio_io_handle_t output) const
              sp<VolumeInterface> getVolumeInterface_l(audio_io_handle_t output) const
              std::vector<sp<VolumeInterface>> getAllVolumeInterfaces_l() const
    static void closeOutputFinish(const sp<IAfPlaybackThread>& thread);
    void closeInputFinish(const sp<IAfRecordThread>& thread);
    audio_unique_id_t nextUniqueId(audio_unique_id_use_t use) final;
              DeviceTypeSet primaryOutputDevice_l() const
              IAfPlaybackThread* fastPlaybackThread_l() const
              sp<IAfThreadBase> getEffectThread_l(audio_session_t sessionId, int effectId)
              IAfThreadBase* hapticPlaybackThread_l() const
              void updateSecondaryOutputsForTrack_l(
                      IAfTrack* track,
                      IAfPlaybackThread* thread,
              const std::vector<audio_io_handle_t>& secondaryOutputs) const
                bool isSessionAcquired_l(audio_session_t audioSession)
                status_t putOrphanEffectChain_l(const sp<IAfEffectChain>& chain)
                sp<IAfEffectChain> getOrphanEffectChain_l(audio_session_t session)
                std::vector< sp<IAfEffectModule> > purgeStaleEffects_l()
                void broadcastParametersToRecordThreads_l(const String8& keyValuePairs)
                void forwardParametersToDownstreamPatches_l(
                        audio_io_handle_t upStream, const String8& keyValuePairs,
                const std::function<bool(const sp<IAfPlaybackThread>&)>& useThread = nullptr)
    struct AudioSessionRef {
        AudioSessionRef(audio_session_t sessionid, pid_t pid, uid_t uid) :
            mSessionid(sessionid), mPid(pid), mUid(uid), mCnt(1) {}
        const audio_session_t mSessionid;
        const pid_t mPid;
        const uid_t mUid;
        int mCnt;
    };
    mutable audio_utils::mutex mMutex;
    mutable audio_utils::mutex mClientMutex;
    std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos;
    audio_utils::mutex& hardwareMutex() const { return mHardwareMutex; }
    mutable audio_utils::mutex mHardwareMutex;
    std::atomic<AudioHwDevice*> mPrimaryHardwareDev = nullptr;
    DefaultKeyedVector<audio_module_handle_t, AudioHwDevice*> mAudioHwDevs
     const sp<DevicesFactoryHalInterface> mDevicesFactoryHal =
             DevicesFactoryHalInterface::create();
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
mutable hardware_call_state mHardwareStatus = AUDIO_HW_IDLE;
    std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos;
                stream_type_t mStreamTypes[AUDIO_STREAM_CNT]
<<<<<<< HEAD
||||||| 46b20158b2
=======
>>>>>>> 05101a67
    float mMasterVolume
    bool mMasterMute
    float mMasterBalance
    std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos;
    std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos;
volatile atomic_uint_fast32_t mNextUniqueIds[AUDIO_UNIQUE_ID_USE_MAX];
    std::atomic<audio_mode_t> mMode = AUDIO_MODE_INVALID;
    std::atomic<bool> mBtNrecIsOff = false;
<<<<<<< HEAD
||||||| 46b20158b2
=======
>>>>>>> 05101a67
                Vector<AudioSessionRef*> mAudioSessionRefs
                AudioHwDevice* loadHwModule_ll(const char *name)
                std::list<sp<audioflinger::SyncEvent>> mPendingSyncEvents
    std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos;
                DefaultKeyedVector<audio_session_t, audio_hw_sync_t> mHwAvSyncIds
    std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos;
sp<Client> registerPid(pid_t pid)
    status_t closeOutput_nonvirtual(audio_io_handle_t output)
    status_t closeInput_nonvirtual(audio_io_handle_t input)
    void setAudioHwSyncForSession_l(IAfPlaybackThread* thread, audio_session_t sessionId)
    static status_t checkStreamType(audio_stream_type_t stream);
    void filterReservedParameters(String8& keyValuePairs, uid_t callingUid);
    void logFilteredParameters(size_t originalKVPSize, const String8& originalKVPs,
                                      size_t rejectedKVPSize, const String8& rejectedKVPs,
                                      uid_t callingUid);
    size_t getClientSharedHeapSize() const;
    std::atomic<bool> mIsLowRamDevice = true;
    bool mIsDeviceTypeKnown
    int64_t mTotalMemory
    std::atomic<size_t> mClientSharedHeapSize = kMinimumClientSharedHeapSizeBytes;
static constexpr size_t kMinimumClientSharedHeapSizeBytes = 1024 * 1024;
nsecs_t mGlobalEffectEnableTime
                sp<IAfPatchPanel> mPatchPanel;
    const sp<EffectsFactoryHalInterface> mEffectsFactoryHal =
            audioflinger::EffectConfiguration::getEffectsFactoryHal();
    const sp<PatchCommandThread> mPatchCommandThread = sp<PatchCommandThread>::make();
sp<DeviceEffectManager> mDeviceEffectManager;
sp<MelReporter> mMelReporter;
    bool mSystemReady
    std::atomic<bool> mAudioPolicyReady = false;
    SimpleLog mRejectedSetParameterLog;
    SimpleLog mAppSetParameterLog;
    SimpleLog mSystemSetParameterLog;
    std::vector<media::AudioVibratorInfo> mAudioVibratorInfos
    static inline constexpr const char *mMetricsId = AMEDIAMETRICS_KEY_AUDIO_FLINGER;
    std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos;
    int32_t mAAudioBurstsPerBuffer
    int32_t mAAudioHwBurstMinMicros
    mediautils::atomic_sp<IAudioManager> mAudioManager;
    std::atomic<bool> mBluetoothLatencyModesEnabled = true;
};
}
