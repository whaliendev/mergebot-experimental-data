       
#include "Client.h"
<<<<<<< HEAD
#include "DeviceEffectManager.h"
#include "EffectConfiguration.h"
||||||| 8c9114940a
#include "ResamplerBufferProvider.h"
namespace android { class AudioFlinger; }
#include "IAfPatchPanel.h"
=======
#include "ResamplerBufferProvider.h"
#include "IAfPatchPanel.h"
>>>>>>> 4ee25d84
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
    static void instantiate() ANDROID_API;
private:
private:
    status_t dump(int fd, const Vector<String16>& args) final EXCLUDES_AudioFlinger_Mutex;
    status_t createTrack(const media::CreateTrackRequest& input,
            media::CreateTrackResponse& output) final EXCLUDES_AudioFlinger_Mutex;
    status_t createRecord(const media::CreateRecordRequest& input,
            media::CreateRecordResponse& output) final EXCLUDES_AudioFlinger_Mutex;
    uint32_t sampleRate(audio_io_handle_t ioHandle) const final EXCLUDES_AudioFlinger_Mutex;
    audio_format_t format(audio_io_handle_t output) const final EXCLUDES_AudioFlinger_Mutex;
    size_t frameCount(audio_io_handle_t ioHandle) const final EXCLUDES_AudioFlinger_Mutex;
    size_t frameCountHAL(audio_io_handle_t ioHandle) const final EXCLUDES_AudioFlinger_Mutex;
    uint32_t latency(audio_io_handle_t output) const final EXCLUDES_AudioFlinger_Mutex;
    status_t setMasterVolume(float value) final EXCLUDES_AudioFlinger_Mutex;
    status_t setMasterMute(bool muted) final EXCLUDES_AudioFlinger_Mutex;
    float masterVolume() const final EXCLUDES_AudioFlinger_Mutex;
    bool masterMute() const final EXCLUDES_AudioFlinger_Mutex;
    status_t setMasterBalance(float balance) final EXCLUDES_AudioFlinger_Mutex;
    status_t getMasterBalance(float* balance) const final EXCLUDES_AudioFlinger_Mutex;
    status_t setStreamVolume(audio_stream_type_t stream, float value,
            audio_io_handle_t output) final EXCLUDES_AudioFlinger_Mutex;
    status_t setStreamMute(audio_stream_type_t stream, bool muted) final
            EXCLUDES_AudioFlinger_Mutex;
    float streamVolume(audio_stream_type_t stream,
            audio_io_handle_t output) const final EXCLUDES_AudioFlinger_Mutex;
    bool streamMute(audio_stream_type_t stream) const final EXCLUDES_AudioFlinger_Mutex;
    status_t setMode(audio_mode_t mode) final EXCLUDES_AudioFlinger_Mutex;
    status_t setMicMute(bool state) final EXCLUDES_AudioFlinger_Mutex;
    bool getMicMute() const final EXCLUDES_AudioFlinger_Mutex;
    void setRecordSilenced(audio_port_handle_t portId, bool silenced) final
            EXCLUDES_AudioFlinger_Mutex;
    status_t setParameters(audio_io_handle_t ioHandle, const String8& keyValuePairs) final
            EXCLUDES_AudioFlinger_Mutex;
    String8 getParameters(audio_io_handle_t ioHandle, const String8& keys) const final
            EXCLUDES_AudioFlinger_Mutex;
    void registerClient(const sp<media::IAudioFlingerClient>& client) final
            EXCLUDES_AudioFlinger_Mutex;
    size_t getInputBufferSize(uint32_t sampleRate, audio_format_t format,
            audio_channel_mask_t channelMask) const final EXCLUDES_AudioFlinger_Mutex;
    status_t openOutput(const media::OpenOutputRequest& request,
            media::OpenOutputResponse* response) final EXCLUDES_AudioFlinger_Mutex;
    audio_io_handle_t openDuplicateOutput(audio_io_handle_t output1,
            audio_io_handle_t output2) final EXCLUDES_AudioFlinger_Mutex;
    status_t closeOutput(audio_io_handle_t output) final EXCLUDES_AudioFlinger_Mutex;
    status_t suspendOutput(audio_io_handle_t output) final EXCLUDES_AudioFlinger_Mutex;
    status_t restoreOutput(audio_io_handle_t output) final EXCLUDES_AudioFlinger_Mutex;
    status_t openInput(const media::OpenInputRequest& request,
            media::OpenInputResponse* response) final EXCLUDES_AudioFlinger_Mutex;
    status_t closeInput(audio_io_handle_t input) final EXCLUDES_AudioFlinger_Mutex;
    status_t setVoiceVolume(float volume) final EXCLUDES_AudioFlinger_Mutex;
    status_t getRenderPosition(uint32_t* halFrames, uint32_t* dspFrames,
            audio_io_handle_t output) const final EXCLUDES_AudioFlinger_Mutex;
    uint32_t getInputFramesLost(audio_io_handle_t ioHandle) const final
            EXCLUDES_AudioFlinger_Mutex;
    audio_unique_id_t newAudioUniqueId(audio_unique_id_use_t use) final
            EXCLUDES_AudioFlinger_Mutex;
    void acquireAudioSessionId(audio_session_t audioSession, pid_t pid, uid_t uid) final
            EXCLUDES_AudioFlinger_Mutex;
    void releaseAudioSessionId(audio_session_t audioSession, pid_t pid) final
            EXCLUDES_AudioFlinger_Mutex;
    status_t queryNumberEffects(uint32_t* numEffects) const final EXCLUDES_AudioFlinger_Mutex;
    status_t queryEffect(uint32_t index, effect_descriptor_t* descriptor) const final
            EXCLUDES_AudioFlinger_Mutex;
    status_t getEffectDescriptor(const effect_uuid_t* pUuid,
            const effect_uuid_t* pTypeUuid,
            uint32_t preferredTypeFlag,
            effect_descriptor_t* descriptor) const final EXCLUDES_AudioFlinger_Mutex;
    status_t createEffect(const media::CreateEffectRequest& request,
            media::CreateEffectResponse* response) final EXCLUDES_AudioFlinger_Mutex;
    status_t moveEffects(audio_session_t sessionId, audio_io_handle_t srcOutput,
            audio_io_handle_t dstOutput) final EXCLUDES_AudioFlinger_Mutex;
    void setEffectSuspended(int effectId,
            audio_session_t sessionId,
            bool suspended) final EXCLUDES_AudioFlinger_Mutex;
    audio_module_handle_t loadHwModule(const char* name) final EXCLUDES_AudioFlinger_Mutex;
    uint32_t getPrimaryOutputSamplingRate() const final EXCLUDES_AudioFlinger_Mutex;
    size_t getPrimaryOutputFrameCount() const final EXCLUDES_AudioFlinger_Mutex;
    status_t setLowRamDevice(bool isLowRamDevice, int64_t totalMemory) final
            EXCLUDES_AudioFlinger_Mutex;
    status_t getAudioPort(struct audio_port_v7* port) const final EXCLUDES_AudioFlinger_Mutex;
    status_t createAudioPatch(const struct audio_patch *patch,
            audio_patch_handle_t* handle) final EXCLUDES_AudioFlinger_Mutex;
    status_t releaseAudioPatch(audio_patch_handle_t handle) final EXCLUDES_AudioFlinger_Mutex;
    status_t listAudioPatches(unsigned int* num_patches,
            struct audio_patch* patches) const final EXCLUDES_AudioFlinger_Mutex;
    status_t setAudioPortConfig(const struct audio_port_config* config) final
            EXCLUDES_AudioFlinger_Mutex;
    audio_hw_sync_t getAudioHwSyncForSession(audio_session_t sessionId) final
            EXCLUDES_AudioFlinger_Mutex;
    status_t systemReady() final EXCLUDES_AudioFlinger_Mutex;
    status_t audioPolicyReady() final { mAudioPolicyReady.store(true); return NO_ERROR; }
    status_t getMicrophones(std::vector<media::MicrophoneInfoFw>* microphones) const final
            EXCLUDES_AudioFlinger_Mutex;
    status_t setAudioHalPids(const std::vector<pid_t>& pids) final
            EXCLUDES_AudioFlinger_Mutex;
    status_t setVibratorInfos(const std::vector<media::AudioVibratorInfo>& vibratorInfos) final
            EXCLUDES_AudioFlinger_Mutex;
    status_t updateSecondaryOutputs(
            const TrackSecondaryOutputsMap& trackSecondaryOutputs) final
            EXCLUDES_AudioFlinger_Mutex;
    status_t getMmapPolicyInfos(
            media::audio::common::AudioMMapPolicyType policyType,
<<<<<<< HEAD
            std::vector<media::audio::common::AudioMMapPolicyInfo>* policyInfos) final
            EXCLUDES_AudioFlinger_Mutex;
||||||| 8c9114940a
            std::vector<media::audio::common::AudioMMapPolicyInfo>* policyInfos) override;
=======
            std::vector<media::audio::common::AudioMMapPolicyInfo>* policyInfos) final;
>>>>>>> 4ee25d84
    int32_t getAAudioMixerBurstCount() const final EXCLUDES_AudioFlinger_Mutex;
    int32_t getAAudioHardwareBurstMinUsec() const final EXCLUDES_AudioFlinger_Mutex;
    status_t setDeviceConnectedState(const struct audio_port_v7* port,
            media::DeviceConnectedState state) final EXCLUDES_AudioFlinger_Mutex;
    status_t setSimulateDeviceConnections(bool enabled) final EXCLUDES_AudioFlinger_Mutex;
    status_t setRequestedLatencyMode(
            audio_io_handle_t output, audio_latency_mode_t mode) final
            EXCLUDES_AudioFlinger_Mutex;
    status_t getSupportedLatencyModes(audio_io_handle_t output,
            std::vector<audio_latency_mode_t>* modes) const final EXCLUDES_AudioFlinger_Mutex;
    status_t setBluetoothVariableLatencyEnabled(bool enabled) final EXCLUDES_AudioFlinger_Mutex;
    status_t isBluetoothVariableLatencyEnabled(bool* enabled) const final
            EXCLUDES_AudioFlinger_Mutex;
    status_t supportsBluetoothVariableLatency(bool* support) const final
            EXCLUDES_AudioFlinger_Mutex;
    status_t getSoundDoseInterface(const sp<media::ISoundDoseCallback>& callback,
            sp<media::ISoundDose>* soundDose) const final EXCLUDES_AudioFlinger_Mutex;
    status_t invalidateTracks(const std::vector<audio_port_handle_t>& portIds) final
            EXCLUDES_AudioFlinger_Mutex;
    status_t getAudioPolicyConfig(media::AudioPolicyConfig* config) final
            EXCLUDES_AudioFlinger_Mutex;
    status_t onTransactWrapper(TransactionCode code, const Parcel& data, uint32_t flags,
            const std::function<status_t()>& delegate) final EXCLUDES_AudioFlinger_Mutex;
    audio_utils::mutex& clientMutex() const final
            RETURN_CAPABILITY(audio_utils::AudioFlinger_ClientMutex) {
        return mClientMutex;
    }
    void removeClient_l(pid_t pid) REQUIRES(clientMutex()) final;
    void removeNotificationClient(pid_t pid) final EXCLUDES_AudioFlinger_Mutex;
    status_t moveAuxEffectToIo(
            int effectId,
            const sp<IAfPlaybackThread>& dstThread,
            sp<IAfPlaybackThread>* srcThread) final EXCLUDES_AudioFlinger_Mutex;
    bool isAudioPolicyReady() const final { return mAudioPolicyReady.load(); }
    const sp<PatchCommandThread>& getPatchCommandThread() final { return mPatchCommandThread; }
    status_t addEffectToHal(
            const struct audio_port_config* device, const sp<EffectHalInterface>& effect) final
            EXCLUDES_AudioFlinger_HardwareMutex;
    status_t removeEffectFromHal(
            const struct audio_port_config* device, const sp<EffectHalInterface>& effect) final
            EXCLUDES_AudioFlinger_HardwareMutex;
<<<<<<< HEAD
    audio_utils::mutex& mutex() const final
            RETURN_CAPABILITY(audio_utils::AudioFlinger_Mutex)
            EXCLUDES_BELOW_AudioFlinger_Mutex { return mMutex; }
    sp<IAfThreadBase> checkOutputThread_l(audio_io_handle_t ioHandle) const final
            REQUIRES(mutex());
||||||| 8c9114940a
    Mutex& mutex() const final { return mLock; }
    sp<IAfThreadBase> checkOutputThread_l(audio_io_handle_t ioHandle) const final REQUIRES(mLock);
=======
    Mutex& mutex() const final { return mLock; }
    sp<IAfThreadBase> checkOutputThread_l(audio_io_handle_t ioHandle) const final REQUIRES(mLock);
>>>>>>> 4ee25d84
    void closeThreadInternal_l(const sp<IAfPlaybackThread>& thread) final REQUIRES(mutex());
    void closeThreadInternal_l(const sp<IAfRecordThread>& thread) final REQUIRES(mutex());
    IAfPlaybackThread* primaryPlaybackThread_l() const final REQUIRES(mutex());
    IAfPlaybackThread* checkPlaybackThread_l(audio_io_handle_t output) const final
            REQUIRES(mutex());
    IAfRecordThread* checkRecordThread_l(audio_io_handle_t input) const final REQUIRES(mutex());
    IAfMmapThread* checkMmapThread_l(audio_io_handle_t io) const final REQUIRES(mutex());
    sp<IAfThreadBase> openInput_l(audio_module_handle_t module,
            audio_io_handle_t* input,
            audio_config_t* config,
            audio_devices_t device,
            const char* address,
            audio_source_t source,
            audio_input_flags_t flags,
            audio_devices_t outputDevice,
            const String8& outputDeviceAddress) final REQUIRES(mutex());
    sp<IAfThreadBase> openOutput_l(audio_module_handle_t module,
            audio_io_handle_t* output,
            audio_config_t* halConfig,
            audio_config_base_t* mixerConfig,
            audio_devices_t deviceType,
            const String8& address,
            audio_output_flags_t flags) final REQUIRES(mutex());
    const DefaultKeyedVector<audio_module_handle_t, AudioHwDevice*>&
            getAudioHwDevs_l() const final REQUIRES(mutex()) { return mAudioHwDevs; }
    void updateDownStreamPatches_l(const struct audio_patch* patch,
            const std::set<audio_io_handle_t>& streams) final REQUIRES(mutex());
    void updateOutDevicesForRecordThreads_l(const DeviceDescriptorBaseVector& devices) final
            REQUIRES(mutex());
<<<<<<< HEAD
    bool isNonOffloadableGlobalEffectEnabled_l() const final REQUIRES(mutex());
    bool btNrecIsOff() const final { return mBtNrecIsOff.load(); }
    float masterVolume_l() const final REQUIRES(mutex());
    bool masterMute_l() const final REQUIRES(mutex());
    float getMasterBalance_l() const REQUIRES(mutex());
    bool streamMute_l(audio_stream_type_t stream) const final REQUIRES(mutex()) {
        return mStreamTypes[stream].mute;
    }
    audio_mode_t getMode() const final { return mMode; }
    bool isLowRamDevice() const final { return mIsLowRamDevice; }
    uint32_t getScreenState() const final { return mScreenState; }
    std::optional<media::AudioVibratorInfo> getDefaultVibratorInfo_l() const final
            REQUIRES(mutex());
    const sp<IAfPatchPanel>& getPatchPanel() const final { return mPatchPanel; }
    const sp<MelReporter>& getMelReporter() const final { return mMelReporter; }
    const sp<EffectsFactoryHalInterface>& getEffectsFactoryHal() const final {
        return mEffectsFactoryHal;
    }
    sp<IAudioManager> getOrCreateAudioManager() final;
    bool updateOrphanEffectChains(const sp<IAfEffectModule>& effect) final
            EXCLUDES_AudioFlinger_Mutex;
    status_t moveEffectChain_ll(audio_session_t sessionId,
            IAfPlaybackThread* srcThread, IAfPlaybackThread* dstThread) final
            REQUIRES(mutex(), audio_utils::ThreadBase_Mutex);
    void requestLogMerge() final;
    sp<NBLog::Writer> newWriter_l(size_t size, const char *name) final REQUIRES(mutex());
    void unregisterWriter(const sp<NBLog::Writer>& writer) final;
    sp<audioflinger::SyncEvent> createSyncEvent(AudioSystem::sync_event_t type,
            audio_session_t triggerSession,
            audio_session_t listenerSession,
            const audioflinger::SyncEventCallback& callBack,
            const wp<IAfTrackBase>& cookie) final EXCLUDES_AudioFlinger_Mutex;
    void ioConfigChanged(audio_io_config_event_t event,
            const sp<AudioIoDescriptor>& ioDesc,
            pid_t pid = 0) final EXCLUDES_AudioFlinger_ClientMutex;
    void onNonOffloadableGlobalEffectEnable() final EXCLUDES_AudioFlinger_Mutex;
    void onSupportedLatencyModesChanged(
            audio_io_handle_t output, const std::vector<audio_latency_mode_t>& modes) final
            EXCLUDES_AudioFlinger_ClientMutex;
    status_t listAudioPorts(unsigned int* num_ports, struct audio_port* ports) const
            EXCLUDES_AudioFlinger_Mutex;
||||||| 8c9114940a
    status_t listAudioPorts(unsigned int* num_ports, struct audio_port* ports) const;
=======
    bool isNonOffloadableGlobalEffectEnabled_l() const final;
    bool btNrecIsOff() const final { return mBtNrecIsOff.load(); }
    float masterVolume_l() const final;
    bool masterMute_l() const final;
    float getMasterBalance_l() const;
    bool streamMute_l(audio_stream_type_t stream) const final { return mStreamTypes[stream].mute; }
    audio_mode_t getMode() const final { return mMode; }
    bool isLowRamDevice() const final { return mIsLowRamDevice; }
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
    status_t listAudioPorts(unsigned int* num_ports, struct audio_port* ports) const;
>>>>>>> 4ee25d84
    sp<EffectsFactoryHalInterface> getEffectsFactory();
<<<<<<< HEAD
public:
    static inline std::atomic<AudioFlinger*> gAudioFlinger = nullptr;
||||||| 8c9114940a
=======
public:
>>>>>>> 4ee25d84
    status_t openMmapStream(MmapStreamInterface::stream_direction_t direction,
                            const audio_attributes_t *attr,
                            audio_config_base_t *config,
                            const AudioClient& client,
                            audio_port_handle_t *deviceId,
                            audio_session_t *sessionId,
                            const sp<MmapStreamCallback>& callback,
                            sp<MmapStreamInterface>& interface,
<<<<<<< HEAD
            audio_port_handle_t *handle) EXCLUDES_AudioFlinger_Mutex;
||||||| 8c9114940a
                            audio_port_handle_t *handle);
    static os::HapticScale onExternalVibrationStart(
        const sp<os::ExternalVibration>& externalVibration);
    static void onExternalVibrationStop(const sp<os::ExternalVibration>& externalVibration);
    std::optional<media::AudioVibratorInfo> getDefaultVibratorInfo_l();
=======
                            audio_port_handle_t *handle);
    static os::HapticScale onExternalVibrationStart(
        const sp<os::ExternalVibration>& externalVibration);
    static void onExternalVibrationStop(const sp<os::ExternalVibration>& externalVibration);
>>>>>>> 4ee25d84
private:
    static const size_t kLogMemorySize = 400 * 1024;
    sp<MemoryDealer> mLogMemoryDealer;
    Vector< sp<NBLog::Writer> > mUnregisteredWriters;
<<<<<<< HEAD
    audio_utils::mutex& unregisteredWritersMutex() const { return mUnregisteredWritersMutex; }
    mutable audio_utils::mutex mUnregisteredWritersMutex;
||||||| 8c9114940a
    Mutex mUnregisteredWritersLock;
public:
    static inline std::atomic<AudioFlinger *> gAudioFlinger = nullptr;
    sp<audioflinger::SyncEvent> createSyncEvent(AudioSystem::sync_event_t type,
                                        audio_session_t triggerSession,
                                        audio_session_t listenerSession,
                                        const audioflinger::SyncEventCallback& callBack,
                                        const wp<IAfTrackBase>& cookie);
    bool btNrecIsOff() const { return mBtNrecIsOff.load(); }
private:
               audio_mode_t getMode() const { return mMode; }
=======
    Mutex mUnregisteredWritersLock;
>>>>>>> 4ee25d84
                            AudioFlinger() ANDROID_API;
    ~AudioFlinger() override;
    status_t initCheck() const { return mPrimaryHardwareDev == NULL ?
                                                        NO_INIT : NO_ERROR; }
    void onFirstRef() override;
    AudioHwDevice* findSuitableHwDev_l(audio_module_handle_t module,
<<<<<<< HEAD
            audio_devices_t deviceType) REQUIRES(mutex());
||||||| 8c9114940a
                                                audio_devices_t deviceType);
    static const bool kEnableExtendedChannels = true;
    static inline bool isValidPcmSinkChannelMask(audio_channel_mask_t channelMask) {
        switch (audio_channel_mask_get_representation(channelMask)) {
        case AUDIO_CHANNEL_REPRESENTATION_POSITION: {
            const uint32_t channelCount = audio_channel_count_from_out_mask(
                    static_cast<audio_channel_mask_t>(channelMask & ~AUDIO_CHANNEL_HAPTIC_ALL));
            const uint32_t maxChannelCount = kEnableExtendedChannels
                    ? AudioMixer::MAX_NUM_CHANNELS : FCC_2;
            if (channelCount < FCC_2
                    || channelCount > maxChannelCount) {
                return false;
            }
            return audio_channel_position_mask_is_out_canonical(channelMask);
            }
        case AUDIO_CHANNEL_REPRESENTATION_INDEX:
            if (kEnableExtendedChannels) {
                const uint32_t channelCount = audio_channel_count_from_out_mask(channelMask);
                if (channelCount >= FCC_2
                        && channelCount <= AudioMixer::MAX_NUM_CHANNELS) {
                    return true;
                }
            }
            return false;
        default:
            return false;
        }
    }
    static const bool kEnableExtendedPrecision = true;
    static inline bool isValidPcmSinkFormat(audio_format_t format) {
        switch (format) {
        case AUDIO_FORMAT_PCM_16_BIT:
            return true;
        case AUDIO_FORMAT_PCM_FLOAT:
        case AUDIO_FORMAT_PCM_24_BIT_PACKED:
        case AUDIO_FORMAT_PCM_32_BIT:
        case AUDIO_FORMAT_PCM_8_24_BIT:
            return kEnableExtendedPrecision;
        default:
            return false;
        }
    }
    static nsecs_t mStandbyTimeInNsecs;
=======
                                                audio_devices_t deviceType);
    static const bool kEnableExtendedChannels = true;
public:
    static inline std::atomic<AudioFlinger*> gAudioFlinger = nullptr;
    static inline bool isValidPcmSinkChannelMask(audio_channel_mask_t channelMask) {
        switch (audio_channel_mask_get_representation(channelMask)) {
        case AUDIO_CHANNEL_REPRESENTATION_POSITION: {
            const uint32_t channelCount = audio_channel_count_from_out_mask(
                    static_cast<audio_channel_mask_t>(channelMask & ~AUDIO_CHANNEL_HAPTIC_ALL));
            const uint32_t maxChannelCount = kEnableExtendedChannels
                    ? AudioMixer::MAX_NUM_CHANNELS : FCC_2;
            if (channelCount < FCC_2
                    || channelCount > maxChannelCount) {
                return false;
            }
            return audio_channel_position_mask_is_out_canonical(channelMask);
            }
        case AUDIO_CHANNEL_REPRESENTATION_INDEX:
            if (kEnableExtendedChannels) {
                const uint32_t channelCount = audio_channel_count_from_out_mask(channelMask);
                if (channelCount >= FCC_2
                        && channelCount <= AudioMixer::MAX_NUM_CHANNELS) {
                    return true;
                }
            }
            return false;
        default:
            return false;
        }
    }
    static const bool kEnableExtendedPrecision = true;
    static inline bool isValidPcmSinkFormat(audio_format_t format) {
        switch (format) {
        case AUDIO_FORMAT_PCM_16_BIT:
            return true;
        case AUDIO_FORMAT_PCM_FLOAT:
        case AUDIO_FORMAT_PCM_24_BIT_PACKED:
        case AUDIO_FORMAT_PCM_32_BIT:
        case AUDIO_FORMAT_PCM_8_24_BIT:
            return kEnableExtendedPrecision;
        default:
            return false;
        }
    }
    static nsecs_t mStandbyTimeInNsecs;
>>>>>>> 4ee25d84
    std::atomic_uint32_t mScreenState{};
<<<<<<< HEAD
||||||| 8c9114940a
    static const int kDumpLockTimeoutNs = 1 * NANOS_PER_SECOND;
public:
    static bool dumpTryLock(Mutex& mutex);
private:
=======
    static const int kDumpLockTimeoutNs = 1 * NANOS_PER_SECOND;
    static bool dumpTryLock(Mutex& mutex);
private:
>>>>>>> 4ee25d84
    void dumpPermissionDenial(int fd, const Vector<String16>& args);
    void dumpClients_ll(int fd, const Vector<String16>& args) REQUIRES(mutex(), clientMutex());
    void dumpInternals_l(int fd, const Vector<String16>& args) REQUIRES(mutex());
    SimpleLog mThreadLog{16};
    void dumpToThreadLog_l(const sp<IAfThreadBase>& thread) REQUIRES(mutex());
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
<<<<<<< HEAD
    const sp<MediaLogNotifier> mMediaLogNotifier = sp<MediaLogNotifier>::make();
||||||| 8c9114940a
    const sp<MediaLogNotifier> mMediaLogNotifier;
    void requestLogMerge();
=======
    const sp<MediaLogNotifier> mMediaLogNotifier;
>>>>>>> 4ee25d84
    template <typename T>
    static audio_io_handle_t findIoHandleBySessionId_l(
            audio_session_t sessionId, const T& threads)
            REQUIRES(audio_utils::AudioFlinger_Mutex) {
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
    IAfThreadBase* checkThread_l(audio_io_handle_t ioHandle) const REQUIRES(mutex());
    IAfPlaybackThread* checkMixerThread_l(audio_io_handle_t output) const REQUIRES(mutex());
    sp<VolumeInterface> getVolumeInterface_l(audio_io_handle_t output) const REQUIRES(mutex());
    std::vector<sp<VolumeInterface>> getAllVolumeInterfaces_l() const REQUIRES(mutex());
    static void closeOutputFinish(const sp<IAfPlaybackThread>& thread);
    void closeInputFinish(const sp<IAfRecordThread>& thread);
    audio_unique_id_t nextUniqueId(audio_unique_id_use_t use) final;
    DeviceTypeSet primaryOutputDevice_l() const REQUIRES(mutex());
    IAfPlaybackThread* fastPlaybackThread_l() const REQUIRES(mutex());
    sp<IAfThreadBase> getEffectThread_l(audio_session_t sessionId, int effectId)
            REQUIRES(mutex());
    IAfThreadBase* hapticPlaybackThread_l() const REQUIRES(mutex());
              void updateSecondaryOutputsForTrack_l(
                      IAfTrack* track,
                      IAfPlaybackThread* thread,
            const std::vector<audio_io_handle_t>& secondaryOutputs) const REQUIRES(mutex());
<<<<<<< HEAD
    bool isSessionAcquired_l(audio_session_t audioSession) REQUIRES(mutex());
||||||| 8c9114940a
public:
                bool isNonOffloadableGlobalEffectEnabled_l();
private:
                void onNonOffloadableGlobalEffectEnable();
                bool isSessionAcquired_l(audio_session_t audioSession);
=======
                bool isSessionAcquired_l(audio_session_t audioSession);
>>>>>>> 4ee25d84
    status_t putOrphanEffectChain_l(const sp<IAfEffectChain>& chain) REQUIRES(mutex());
<<<<<<< HEAD
    sp<IAfEffectChain> getOrphanEffectChain_l(audio_session_t session) REQUIRES(mutex());
||||||| 8c9114940a
                sp<IAfEffectChain> getOrphanEffectChain_l(audio_session_t session);
public:
                bool updateOrphanEffectChains(const sp<IAfEffectModule>& effect);
private:
                std::vector< sp<IAfEffectModule> > purgeStaleEffects_l();
=======
                sp<IAfEffectChain> getOrphanEffectChain_l(audio_session_t session);
                std::vector< sp<IAfEffectModule> > purgeStaleEffects_l();
>>>>>>> 4ee25d84
    std::vector< sp<IAfEffectModule> > purgeStaleEffects_l() REQUIRES(mutex());
    void broadcastParametersToRecordThreads_l(const String8& keyValuePairs) REQUIRES(mutex());
    void forwardParametersToDownstreamPatches_l(
                        audio_io_handle_t upStream, const String8& keyValuePairs,
            const std::function<bool(const sp<IAfPlaybackThread>&)>& useThread = nullptr)
            REQUIRES(mutex());
    struct AudioSessionRef {
        AudioSessionRef(audio_session_t sessionid, pid_t pid, uid_t uid) :
            mSessionid(sessionid), mPid(pid), mUid(uid), mCnt(1) {}
        const audio_session_t mSessionid;
        const pid_t mPid;
        const uid_t mUid;
        int mCnt;
    };
<<<<<<< HEAD
    mutable audio_utils::mutex mMutex;
||||||| 8c9114940a
public:
    mutable Mutex mLock;
=======
    mutable Mutex mLock;
>>>>>>> 4ee25d84
<<<<<<< HEAD
    mutable audio_utils::mutex mClientMutex;
||||||| 8c9114940a
private:
    mutable Mutex mClientLock;
=======
    mutable Mutex mClientLock;
>>>>>>> 4ee25d84
    DefaultKeyedVector<pid_t, wp<Client>> mClients GUARDED_BY(clientMutex());
    audio_utils::mutex& hardwareMutex() const { return mHardwareMutex; }
    mutable audio_utils::mutex mHardwareMutex;
    std::atomic<AudioHwDevice*> mPrimaryHardwareDev = nullptr;
    DefaultKeyedVector<audio_module_handle_t, AudioHwDevice*> mAudioHwDevs
            GUARDED_BY(hardwareMutex()) {nullptr };
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
    DefaultKeyedVector<audio_io_handle_t, sp<IAfPlaybackThread>> mPlaybackThreads
            GUARDED_BY(mutex());
    stream_type_t mStreamTypes[AUDIO_STREAM_CNT] GUARDED_BY(mutex());
    float mMasterVolume GUARDED_BY(mutex()) = 1.f;
    bool mMasterMute GUARDED_BY(mutex()) = false;
    float mMasterBalance GUARDED_BY(mutex()) = 0.f;
    DefaultKeyedVector<audio_io_handle_t, sp<IAfRecordThread>> mRecordThreads GUARDED_BY(mutex());
    DefaultKeyedVector<pid_t, sp<NotificationClient>> mNotificationClients
            GUARDED_BY(clientMutex());
    volatile atomic_uint_fast32_t mNextUniqueIds[AUDIO_UNIQUE_ID_USE_MAX];
    std::atomic<audio_mode_t> mMode = AUDIO_MODE_INVALID;
    std::atomic<bool> mBtNrecIsOff = false;
    Vector<AudioSessionRef*> mAudioSessionRefs GUARDED_BY(mutex());
<<<<<<< HEAD
    AudioHwDevice* loadHwModule_ll(const char *name) REQUIRES(mutex(), hardwareMutex());
||||||| 8c9114940a
                float masterVolume_l() const;
                float getMasterBalance_l() const;
                bool masterMute_l() const;
                AudioHwDevice* loadHwModule_l(const char *name);
=======
                AudioHwDevice* loadHwModule_l(const char *name);
>>>>>>> 4ee25d84
    std::list<sp<audioflinger::SyncEvent>> mPendingSyncEvents GUARDED_BY(mutex());
    DefaultKeyedVector<audio_session_t, sp<IAfEffectChain>> mOrphanEffectChains
            GUARDED_BY(mutex());
    DefaultKeyedVector<audio_session_t, audio_hw_sync_t> mHwAvSyncIds GUARDED_BY(mutex());
    DefaultKeyedVector<audio_io_handle_t, sp<IAfMmapThread>> mMmapThreads GUARDED_BY(mutex());
<<<<<<< HEAD
    sp<Client> registerPid(pid_t pid) EXCLUDES_AudioFlinger_ClientMutex;
||||||| 8c9114940a
private:
    sp<Client> registerPid(pid_t pid);
=======
    sp<Client> registerPid(pid_t pid);
>>>>>>> 4ee25d84
    status_t closeOutput_nonvirtual(audio_io_handle_t output) EXCLUDES_AudioFlinger_Mutex;
    status_t closeInput_nonvirtual(audio_io_handle_t input) EXCLUDES_AudioFlinger_Mutex;
    void setAudioHwSyncForSession_l(IAfPlaybackThread* thread, audio_session_t sessionId)
            REQUIRES(mutex());
    static status_t checkStreamType(audio_stream_type_t stream);
    void filterReservedParameters(String8& keyValuePairs, uid_t callingUid);
    void logFilteredParameters(size_t originalKVPSize, const String8& originalKVPs,
                                      size_t rejectedKVPSize, const String8& rejectedKVPs,
                                      uid_t callingUid);
    size_t getClientSharedHeapSize() const;
<<<<<<< HEAD
    std::atomic<bool> mIsLowRamDevice = true;
    bool mIsDeviceTypeKnown GUARDED_BY(mutex()) = false;
    int64_t mTotalMemory GUARDED_BY(mutex()) = 0;
    std::atomic<size_t> mClientSharedHeapSize = kMinimumClientSharedHeapSizeBytes;
||||||| 8c9114940a
private:
    std::atomic<bool> mIsLowRamDevice;
    bool mIsDeviceTypeKnown;
    int64_t mTotalMemory;
    std::atomic<size_t> mClientSharedHeapSize;
=======
    std::atomic<bool> mIsLowRamDevice;
    bool mIsDeviceTypeKnown;
    int64_t mTotalMemory;
    std::atomic<size_t> mClientSharedHeapSize;
>>>>>>> 4ee25d84
    static constexpr size_t kMinimumClientSharedHeapSizeBytes = 1024 * 1024;
    nsecs_t mGlobalEffectEnableTime GUARDED_BY(mutex()) = 0;
                sp<IAfPatchPanel> mPatchPanel;
<<<<<<< HEAD
    const sp<EffectsFactoryHalInterface> mEffectsFactoryHal =
            audioflinger::EffectConfiguration::getEffectsFactoryHal();
||||||| 8c9114940a
public:
    sp<EffectsFactoryHalInterface> mEffectsFactoryHal;
private:
=======
    sp<EffectsFactoryHalInterface> mEffectsFactoryHal;
>>>>>>> 4ee25d84
    const sp<PatchCommandThread> mPatchCommandThread = sp<PatchCommandThread>::make();
                sp<DeviceEffectManager> mDeviceEffectManager;
                sp<MelReporter> mMelReporter;
    bool mSystemReady GUARDED_BY(mutex()) = false;
    std::atomic<bool> mAudioPolicyReady = false;
    mediautils::UidInfo mUidInfo GUARDED_BY(mutex());
    SimpleLog mRejectedSetParameterLog;
    SimpleLog mAppSetParameterLog;
    SimpleLog mSystemSetParameterLog;
    std::vector<media::AudioVibratorInfo> mAudioVibratorInfos GUARDED_BY(mutex());
    static inline constexpr const char *mMetricsId = AMEDIAMETRICS_KEY_AUDIO_FLINGER;
<<<<<<< HEAD
||||||| 8c9114940a
    static constexpr int32_t kMaxSharedAudioHistoryMs = 5000;
=======
public:
    static constexpr int32_t kMaxSharedAudioHistoryMs = 5000;
private:
>>>>>>> 4ee25d84
    std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos
             GUARDED_BY(mutex());
    int32_t mAAudioBurstsPerBuffer GUARDED_BY(mutex()) = 0;
    int32_t mAAudioHwBurstMinMicros GUARDED_BY(mutex()) = 0;
    mediautils::atomic_sp<IAudioManager> mAudioManager;
    std::atomic<bool> mBluetoothLatencyModesEnabled = true;
};
}
