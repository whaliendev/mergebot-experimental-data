       
#include "Configuration.h"
#include "ResamplerBufferProvider.h"
#include "IAfPatchPanel.h"
#include "Client.h"
#include "DeviceEffectManager.h"
#include "IAfEffect.h"
#include "IAfPatchPanel.h"
#include <audio_utils/LinearMap.h>
#include <audio_utils/TimestampVerifier.h>
#include <sounddose/SoundDoseManager.h>
#include <timing/MonotonicFrameCounter.h>
#include <timing/SyncEvent.h>
#include <timing/SynchronizedRecordState.h>
#include <datapath/AudioHwDevice.h>
#include <datapath/AudioStreamIn.h>
#include <datapath/AudioStreamOut.h>
#include <datapath/SpdifStreamOut.h>
#include <datapath/ThreadMetrics.h>
#include <datapath/TrackMetrics.h>
#include <datapath/VolumeInterface.h>
#include <fastpath/FastCapture.h>
#include <fastpath/FastMixer.h>
#include <media/nbaio/NBAIO.h>
#include <android/os/IPowerManager.h>
#include <media/nblog/NBLog.h>
#include <private/media/AudioEffectShared.h>
#include <private/media/AudioTrackShared.h>
#include <vibrator/ExternalVibration.h>
#include <vibrator/ExternalVibrationUtils.h>
#include "android/media/BnAudioRecord.h"
#include "android/media/BnEffect.h"
#include <audio_utils/MelAggregator.h>
#include <audio_utils/MelProcessor.h>
#include "IAfThread.h"
#include "IAfTrack.h"
#include <media/AudioSystem.h>
#include <media/AudioTrack.h>
#include <media/MmapStreamInterface.h>
#include <mediautils/SharedMemoryAllocator.h>
#include <mediautils/ThreadSnapshot.h>
#include <afutils/AllocatorFactory.h>
#include <afutils/AudioWatchdog.h>
#include <afutils/NBAIO_Tee.h>
#include <audio_utils/clock.h>
#include <media/MmapStreamCallback.h>
#include <utils/Errors.h>
#include <utils/threads.h>
#include <utils/SortedVector.h>
#include <utils/TypeHelpers.h>
#include <utils/Vector.h>
#include <binder/AppOpsManager.h>
#include <binder/BinderService.h>
#include <binder/IAppOpsCallback.h>
#include <binder/MemoryDealer.h>
#include <system/audio.h>
#include <system/audio_policy.h>
#include <media/audiohal/EffectBufferHalInterface.h>
#include <media/audiohal/StreamHalInterface.h>
#include <media/AudioBufferProvider.h>
#include <media/AudioContainers.h>
#include <media/AudioDeviceTypeAddr.h>
#include <media/AudioMixer.h>
#include <media/DeviceDescriptorBase.h>
#include <media/ExtendedAudioBufferProvider.h>
#include <media/VolumeShaper.h>
#include "MelReporter.h"
#include "PatchCommandThread.h"
#include <mutex>
#include <chrono>
#include <numeric>
#include <deque>
#include <string>
#include <vector>
#include <stdint.h>
#include <sys/types.h>
#include <limits.h>
#include <android/media/BnAudioTrack.h>
#include <android/media/IAudioFlingerClient.h>
#include <android/media/IAudioTrackCallback.h>
#include <android/os/BnExternalVibrationController.h>
#include <android/content/AttributionSourceState.h>
#include <android-base/macros.h>
#include <cutils/atomic.h>
#include <cutils/compiler.h>
#include <cutils/properties.h>
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
<<<<<<< HEAD
class AudioFlinger
    : public AudioFlingerServerAdapter::Delegate
    , public IAfClientCallback
    , public IAfDeviceEffectManagerCallback
    , public IAfMelReporterCallback
    , public IAfPatchPanelCallback
    , public IAfThreadCallback
||||||| d6bfd2c02d
class AudioFlinger : public AudioFlingerServerAdapter::Delegate
=======
class AudioFlinger
    : public AudioFlingerServerAdapter::Delegate
>>>>>>> 817baca4
{
    friend class sp<AudioFlinger>;
public:
static void instantiate()private:
    std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos;
public:
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
public:
    size_t getPrimaryOutputFrameCount() const final;
private:
    status_t setLowRamDevice(bool isLowRamDevice, int64_t totalMemory) final;
public:
    status_t getAudioPort(struct audio_port_v7* port) const final;
private:
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
<<<<<<< HEAD
            std::vector<media::audio::common::AudioMMapPolicyInfo>* policyInfos) final;
||||||| d6bfd2c02d
            std::vector<media::audio::common::AudioMMapPolicyInfo> *policyInfos);
=======
            std::vector<media::audio::common::AudioMMapPolicyInfo>* policyInfos) override;
>>>>>>> 817baca4
public:
    int32_t getAAudioMixerBurstCount() const final;
private:
    int32_t getAAudioHardwareBurstMinUsec() const final;
    status_t setDeviceConnectedState(const struct audio_port_v7* port,
            media::DeviceConnectedState state) final;
    status_t setSimulateDeviceConnections(bool enabled) final;
    status_t setRequestedLatencyMode(
            audio_io_handle_t output, audio_latency_mode_t mode) final;
    status_t getSupportedLatencyModes(audio_io_handle_t output,
            std::vector<audio_latency_mode_t>* modes) const final;
    status_t setBluetoothVariableLatencyEnabled(bool enabled) final;
public:
    status_t isBluetoothVariableLatencyEnabled(bool* enabled) const final;
private:
    status_t supportsBluetoothVariableLatency(bool* support) const final;
public:
    status_t getSoundDoseInterface(const sp<media::ISoundDoseCallback>& callback,
            sp<media::ISoundDose>* soundDose) const final;
private:
    status_t invalidateTracks(const std::vector<audio_port_handle_t>& portIds) final;
    status_t getAudioPolicyConfig(media::AudioPolicyConfig* config) final;
    status_t onTransactWrapper(TransactionCode code, const Parcel& data, uint32_t flags,
            const std::function<status_t()>& delegate) final;
    Mutex& clientMutex() const final { return mClientLock; }
public:
private:
<<<<<<< HEAD
||||||| d6bfd2c02d
=======
>>>>>>> 817baca4
            bool isAudioPolicyReady() const final { return mAudioPolicyReady.load(); }
    const sp<PatchCommandThread>& getPatchCommandThread() final { return mPatchCommandThread; }
public:
    status_t listAudioPorts(unsigned int* num_ports, struct audio_port* ports) const;
private:
    sp<NBLog::Writer> newWriter_l(size_t size, const char *name) final;
    void unregisterWriter(const sp<NBLog::Writer>& writer) final;
    sp<EffectsFactoryHalInterface> getEffectsFactory();
public:
    status_t openMmapStream(MmapStreamInterface::stream_direction_t direction,
                            const audio_attributes_t *attr,
                            audio_config_base_t *config,
                            const AudioClient& client,
                            audio_port_handle_t *deviceId,
                            audio_session_t *sessionId,
                            const sp<MmapStreamCallback>& callback,
                            sp<MmapStreamInterface>& interface,
                            audio_port_handle_t *handle);
private:
    status_t addEffectToHal(
            const struct audio_port_config* device, const sp<EffectHalInterface>& effect) final;
    status_t removeEffectFromHal(
            const struct audio_port_config* device, const sp<EffectHalInterface>& effect) final;
    Mutex& mutex() const final { return mLock; }
    void updateDownStreamPatches_l(const struct audio_patch* patch,
            const std::set<audio_io_handle_t>& streams) final;
    static const size_t kLogMemorySize = 400 * 1024;
sp<MemoryDealer> mLogMemoryDealer;
    Vector< sp<NBLog::Writer> > mUnregisteredWriters;
    Mutex mUnregisteredWritersLock;
              AudioFlinger()public:
    static inline std::atomic<AudioFlinger*> gAudioFlinger = nullptr;
private:
    sp<audioflinger::SyncEvent> createSyncEvent(AudioSystem::sync_event_t type,
            audio_session_t triggerSession,
            audio_session_t listenerSession,
            const audioflinger::SyncEventCallback& callBack,
            const wp<IAfTrackBase>& cookie) final;
    bool btNrecIsOff() const final { return mBtNrecIsOff.load(); }
               audio_mode_t getMode() const final { return mMode; }
               audio_mode_t getMode() const final { return mMode; }
               audio_mode_t getMode() const final { return mMode; }
    std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos;
    std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos;
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
    void requestLogMerge() final;
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
sp<IAfThreadBase> checkOutputThread_l(audio_io_handle_t ioHandle) const final std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos;
    IAfPlaybackThread* checkPlaybackThread_l(audio_io_handle_t output) const final;
    IAfPlaybackThread* checkMixerThread_l(audio_io_handle_t output) const;
    IAfRecordThread* checkRecordThread_l(audio_io_handle_t input) const final;
    IAfMmapThread* checkMmapThread_l(audio_io_handle_t io) const final;
void lock() const final RELEASE(mLock){ mLock.unlock(); }
              sp<VolumeInterface> getVolumeInterface_l(audio_io_handle_t output) const;
              std::vector<sp<VolumeInterface>> getAllVolumeInterfaces_l() const;
    sp<IAfThreadBase> openOutput_l(audio_module_handle_t module,
            audio_io_handle_t* output,
            audio_config_t* halConfig,
            audio_config_base_t* mixerConfig,
            audio_devices_t deviceType,
            const String8& address,
            audio_output_flags_t flags) final;
    const DefaultKeyedVector<audio_module_handle_t, AudioHwDevice*>&getAudioHwDevs_l() const final { return mAudioHwDevs; }
    void closeOutputFinish(const sp<IAfPlaybackThread>& thread);
    void closeInputFinish(const sp<IAfRecordThread>& thread);
              bool streamMute_l(audio_stream_type_t stream) const final { return mStreamTypes[stream].mute; }
              void ioConfigChanged(audio_io_config_event_t event,
              const sp<AudioIoDescriptor>& ioDesc,
              pid_t pid = 0) final;
              void onSupportedLatencyModesChanged(
              audio_io_handle_t output, const std::vector<audio_latency_mode_t>& modes) final;
              audio_unique_id_t nextUniqueId(audio_unique_id_use_t use) final;
              status_t moveEffectChain_l(audio_session_t sessionId,
              IAfPlaybackThread* srcThread, IAfPlaybackThread* dstThread) final;
              status_t moveAuxEffectToIo(
              int effectId,
              const sp<IAfPlaybackThread>& dstThread,
              sp<IAfPlaybackThread>* srcThread) final;
              IAfPlaybackThread* primaryPlaybackThread_l() const final;
              DeviceTypeSet primaryOutputDevice_l() const;
              IAfPlaybackThread* fastPlaybackThread_l() const;
              sp<IAfThreadBase> getEffectThread_l(audio_session_t sessionId, int effectId);
              IAfThreadBase* hapticPlaybackThread_l() const;
              void updateSecondaryOutputsForTrack_l(
                      IAfTrack* track,
                      IAfPlaybackThread* thread,
                      const std::vector<audio_io_handle_t>& secondaryOutputs) const;
                void removeClient_l(pid_t pid) final;
                void removeNotificationClient(pid_t pid) final;
                void onNonOffloadableGlobalEffectEnable() final;
                bool isSessionAcquired_l(audio_session_t audioSession);
                status_t putOrphanEffectChain_l(const sp<IAfEffectChain>& chain);
                sp<IAfEffectChain> getOrphanEffectChain_l(audio_session_t session);
                bool updateOrphanEffectChains(const sp<IAfEffectModule>& effect) final;
                std::vector< sp<IAfEffectModule> > purgeStaleEffects_l();
                void broadcastParametersToRecordThreads_l(const String8& keyValuePairs);
                void updateOutDevicesForRecordThreads_l(const DeviceDescriptorBaseVector& devices) final;
    bool isNonOffloadableGlobalEffectEnabled_l() const final;
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
    std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos;
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
    std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos;
                stream_type_t mStreamTypes[AUDIO_STREAM_CNT];
                float mMasterVolume;
                bool mMasterMute;
                float mMasterBalance = 0.f;
    std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos;
    std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos;
                volatile atomic_uint_fast32_t mNextUniqueIds[AUDIO_UNIQUE_ID_USE_MAX];
                audio_mode_t mMode;
                std::atomic_bool mBtNrecIsOff;
                Vector<AudioSessionRef*> mAudioSessionRefs;
                float masterVolume_l() const final;
                float getMasterBalance_l() const;
                bool masterMute_l() const final;
                AudioHwDevice* loadHwModule_l(const char *name);
                std::list<sp<audioflinger::SyncEvent>> mPendingSyncEvents;
    std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos;
                DefaultKeyedVector< audio_session_t , audio_hw_sync_t >mHwAvSyncIds;
    std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos;
sp<Client> registerPid(pid_t pid);
    status_t closeOutput_nonvirtual(audio_io_handle_t output);
    void closeThreadInternal_l(const sp<IAfRecordThread>& thread);
    status_t closeInput_nonvirtual(audio_io_handle_t input);
    void closeThreadInternal_l(const sp<IAfRecordThread>& thread);
    void setAudioHwSyncForSession_l(IAfPlaybackThread* thread, audio_session_t sessionId);
    status_t checkStreamType(audio_stream_type_t stream) const;
    void filterReservedParameters(String8& keyValuePairs, uid_t callingUid);
    void logFilteredParameters(size_t originalKVPSize, const String8& originalKVPs,
                                      size_t rejectedKVPSize, const String8& rejectedKVPs,
                                      uid_t callingUid);
    sp<IAudioManager> getOrCreateAudioManager() final;
    bool isLowRamDevice() const final { return mIsLowRamDevice; }
    uint32_t getScreenState() const final { return mScreenState; }
    std::optional<media::AudioVibratorInfo> getDefaultVibratorInfo_l() const final;
    const sp<IAfPatchPanel>& getPatchPanel() const final { return mPatchPanel; }
    const sp<MelReporter>& getMelReporter() const final { return mMelReporter; }
    const sp<EffectsFactoryHalInterface>& getEffectsFactoryHal() const final {
        return mEffectsFactoryHal;
    }
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
    sp<DeviceEffectManager> mDeviceEffectManager;
    sp<MelReporter> mMelReporter;
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
