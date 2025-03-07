#ifndef ANDROID_AUDIOSYSTEM_H_
#define ANDROID_AUDIOSYSTEM_H_ 
#include <sys/types.h>
#include <mutex>
#include <set>
#include <vector>
#include <android/content/AttributionSourceState.h>
#include <android/media/AudioPolicyConfig.h>
#include <android/media/AudioPortFw.h>
#include <android/media/AudioVibratorInfo.h>
#include <android/media/BnAudioFlingerClient.h>
#include <android/media/BnAudioPolicyServiceClient.h>
#include <android/media/EffectDescriptor.h>
#include <android/media/INativeSpatializerCallback.h>
#include <android/media/ISoundDose.h>
#include <android/media/ISoundDoseCallback.h>
#include <android/media/ISpatializer.h>
#include <android/media/MicrophoneInfoFw.h>
#include <android/media/RecordClientInfo.h>
#include <android/media/audio/common/AudioConfigBase.h>
#include <android/media/audio/common/AudioMMapPolicyInfo.h>
#include <android/media/audio/common/AudioMMapPolicyType.h>
#include <android/media/audio/common/AudioPort.h>
#include <media/AidlConversionUtil.h>
#include <media/AudioContainers.h>
#include <media/AudioDeviceTypeAddr.h>
#include <media/AudioPolicy.h>
#include <media/AudioProductStrategy.h>
#include <media/AudioVolumeGroup.h>
#include <media/AudioIoDescriptor.h>
#include <system/audio.h>
#include <system/audio_effect.h>
#include <system/audio_policy.h>
#include <utils/Errors.h>
#include <utils/Mutex.h>
using android::content::AttributionSourceState;
namespace android {
struct record_client_info {
    audio_unique_id_t riid;
    uid_t uid;
    audio_session_t session;
    audio_source_t source;
    audio_port_handle_t port_id;
    bool silenced;
};
typedef struct record_client_info record_client_info_t;
ConversionResult<record_client_info_t>
aidl2legacy_RecordClientInfo_record_client_info_t(const media::RecordClientInfo& aidl);
ConversionResult<media::RecordClientInfo>
legacy2aidl_record_client_info_t_RecordClientInfo(const record_client_info_t& legacy);
typedef void (*audio_error_callback)(status_t err);
typedef void (*dynamic_policy_callback)(int event, String8 regId, int val);
typedef void (*record_config_callback)(int event,
                                       const record_client_info_t *clientInfo,
                                       const audio_config_base_t *clientConfig,
                                       std::vector<effect_descriptor_t> clientEffects,
                                       const audio_config_base_t *deviceConfig,
                                       std::vector<effect_descriptor_t> effects,
                                       audio_patch_handle_t patchHandle,
                                       audio_source_t source);
typedef void (*routing_callback)();
typedef void (*vol_range_init_req_callback)();
class CaptureStateListenerImpl;
class IAudioFlinger;
class String8;
namespace media {
class IAudioPolicyService;
}
class AudioSystem
{
    friend class AudioFlingerClient;
    friend class AudioPolicyServiceClient;
    friend class CaptureStateListenerImpl;
    template <typename ServiceInterface, typename Client, typename AidlInterface,
            typename ServiceTraits>
    friend class ServiceHandler;
public:
    static status_t muteMicrophone(bool state);
    static status_t isMicrophoneMuted(bool *state);
    static status_t setMasterVolume(float value);
    static status_t getMasterVolume(float* volume);
    static status_t setMasterMute(bool mute);
    static status_t getMasterMute(bool* mute);
    static status_t setStreamVolume(audio_stream_type_t stream, float value,
                                    audio_io_handle_t output);
    static status_t getStreamVolume(audio_stream_type_t stream, float* volume,
                                    audio_io_handle_t output);
    static status_t setStreamMute(audio_stream_type_t stream, bool mute);
    static status_t getStreamMute(audio_stream_type_t stream, bool* mute);
    static status_t setMode(audio_mode_t mode);
    static status_t setSimulateDeviceConnections(bool enabled);
    static status_t isStreamActive(audio_stream_type_t stream, bool *state, uint32_t inPastMs);
    static status_t isStreamActiveRemotely(audio_stream_type_t stream, bool *state,
            uint32_t inPastMs);
    static status_t isSourceActive(audio_source_t source, bool *state);
    static status_t setParameters(audio_io_handle_t ioHandle, const String8& keyValuePairs);
    static String8 getParameters(audio_io_handle_t ioHandle, const String8& keys);
    static status_t setParameters(const String8& keyValuePairs);
    static String8 getParameters(const String8& keys);
    static uintptr_t addErrorCallback(audio_error_callback cb);
    static void removeErrorCallback(uintptr_t cb);
    static void setDynPolicyCallback(dynamic_policy_callback cb);
    static void setRecordConfigCallback(record_config_callback);
    static void setRoutingCallback(routing_callback cb);
    static void setVolInitReqCallback(vol_range_init_req_callback cb);
    static void setAudioFlingerBinder(const sp<IBinder>& audioFlinger);
    static status_t setLocalAudioFlinger(const sp<IAudioFlinger>& af);
static sp<IAudioFlinger> get_audio_flinger();
    static float linearToLog(int volume);
    static int logToLinear(float volume);
    static size_t calculateMinFrameCount(
            uint32_t afLatencyMs, uint32_t afFrameCount, uint32_t afSampleRate,
            uint32_t sampleRate, float speed );
    static status_t getOutputSamplingRate(uint32_t* samplingRate,
            audio_stream_type_t stream);
    static status_t getOutputFrameCount(size_t* frameCount,
            audio_stream_type_t stream);
    static status_t getOutputLatency(uint32_t* latency,
            audio_stream_type_t stream);
    static status_t getSamplingRate(audio_io_handle_t ioHandle,
                                          uint32_t* samplingRate);
    static status_t getFrameCount(audio_io_handle_t ioHandle,
                                  size_t* frameCount);
    static status_t getLatency(audio_io_handle_t output,
                               uint32_t* latency);
    static status_t getInputBufferSize(uint32_t sampleRate, audio_format_t format,
        audio_channel_mask_t channelMask, size_t* buffSize);
    static status_t setVoiceVolume(float volume);
    static status_t getRenderPosition(audio_io_handle_t output,
                                      uint32_t *halFrames,
                                      uint32_t *dspFrames);
    static uint32_t getInputFramesLost(audio_io_handle_t ioHandle);
    static audio_unique_id_t newAudioUniqueId(audio_unique_id_use_t use);
    static void acquireAudioSessionId(audio_session_t audioSession, pid_t pid, uid_t uid);
    static void releaseAudioSessionId(audio_session_t audioSession, pid_t pid);
    static audio_hw_sync_t getAudioHwSyncForSession(audio_session_t sessionId);
    static status_t systemReady();
    static status_t audioPolicyReady();
    static status_t getFrameCountHAL(audio_io_handle_t ioHandle,
                                     size_t* frameCount);
    enum sync_event_t {
        SYNC_EVENT_SAME = -1,
        SYNC_EVENT_NONE = 0,
        SYNC_EVENT_PRESENTATION_COMPLETE,
        SYNC_EVENT_CNT,
    };
    static const uint32_t kSyncRecordStartTimeOutMs = 30000;
    static void onNewAudioModulesAvailable();
    static status_t setDeviceConnectionState(audio_policy_dev_state_t state,
                                             const android::media::audio::common::AudioPort& port,
                                             audio_format_t encodedFormat);
    static audio_policy_dev_state_t getDeviceConnectionState(audio_devices_t device,
                                                                const char *device_address);
    static status_t handleDeviceConfigChange(audio_devices_t device,
                                             const char *device_address,
                                             const char *device_name,
                                             audio_format_t encodedFormat);
    static status_t setPhoneState(audio_mode_t state, uid_t uid);
    static status_t setForceUse(audio_policy_force_use_t usage, audio_policy_forced_cfg_t config);
    static audio_policy_forced_cfg_t getForceUse(audio_policy_force_use_t usage);
    static status_t getOutputForAttr(audio_attributes_t *attr,
                                     audio_io_handle_t *output,
                                     audio_session_t session,
                                     audio_stream_type_t *stream,
                                     const AttributionSourceState& attributionSource,
                                     audio_config_t *config,
                                     audio_output_flags_t flags,
                                     audio_port_handle_t *selectedDeviceId,
                                     audio_port_handle_t *portId,
                                     std::vector<audio_io_handle_t> *secondaryOutputs,
                                     bool *isSpatialized,
                                     bool *isBitPerfect);
    static status_t startOutput(audio_port_handle_t portId);
    static status_t stopOutput(audio_port_handle_t portId);
    static void releaseOutput(audio_port_handle_t portId);
    static status_t getInputForAttr(const audio_attributes_t *attr,
                                    audio_io_handle_t *input,
                                    audio_unique_id_t riid,
                                    audio_session_t session,
                                    const AttributionSourceState& attributionSource,
                                    audio_config_base_t *config,
                                    audio_input_flags_t flags,
                                    audio_port_handle_t *selectedDeviceId,
                                    audio_port_handle_t *portId);
    static status_t startInput(audio_port_handle_t portId);
    static status_t stopInput(audio_port_handle_t portId);
    static void releaseInput(audio_port_handle_t portId);
    static status_t initStreamVolume(audio_stream_type_t stream,
                                      int indexMin,
                                      int indexMax);
    static status_t setStreamVolumeIndex(audio_stream_type_t stream,
                                         int index,
                                         audio_devices_t device);
    static status_t getStreamVolumeIndex(audio_stream_type_t stream,
                                         int *index,
                                         audio_devices_t device);
    static status_t setVolumeIndexForAttributes(const audio_attributes_t &attr,
                                                int index,
                                                audio_devices_t device);
    static status_t getVolumeIndexForAttributes(const audio_attributes_t &attr,
                                                int &index,
                                                audio_devices_t device);
    static status_t getMaxVolumeIndexForAttributes(const audio_attributes_t &attr, int &index);
    static status_t getMinVolumeIndexForAttributes(const audio_attributes_t &attr, int &index);
    static product_strategy_t getStrategyForStream(audio_stream_type_t stream);
    static status_t getDevicesForAttributes(const audio_attributes_t &aa,
                                            AudioDeviceTypeAddrVector *devices,
                                            bool forVolume);
    static audio_io_handle_t getOutputForEffect(const effect_descriptor_t *desc);
    static status_t registerEffect(const effect_descriptor_t *desc,
                                    audio_io_handle_t io,
                                    product_strategy_t strategy,
                                    audio_session_t session,
                                    int id);
    static status_t unregisterEffect(int id);
    static status_t setEffectEnabled(int id, bool enabled);
    static status_t moveEffectsToIo(const std::vector<int>& ids, audio_io_handle_t io);
    static void clearAudioConfigCache();
    static status_t setLocalAudioPolicyService(const sp<media::IAudioPolicyService>& aps);
    static sp<media::IAudioPolicyService> get_audio_policy_service();
    static void clearAudioPolicyService();
    static uint32_t getPrimaryOutputSamplingRate();
    static size_t getPrimaryOutputFrameCount();
    static status_t setLowRamDevice(bool isLowRamDevice, int64_t totalMemory);
    static status_t setSupportedSystemUsages(const std::vector<audio_usage_t>& systemUsages);
    static status_t setAllowedCapturePolicy(uid_t uid, audio_flags_mask_t capturePolicy);
    static audio_offload_mode_t getOffloadSupport(const audio_offload_info_t& info);
    static status_t checkAudioFlinger();
    static status_t listAudioPorts(audio_port_role_t role,
                                   audio_port_type_t type,
                                   unsigned int *num_ports,
                                   struct audio_port_v7 *ports,
                                   unsigned int *generation);
    static status_t listDeclaredDevicePorts(media::AudioPortRole role,
                                            std::vector<media::AudioPortFw>* result);
    static status_t getAudioPort(struct audio_port_v7 *port);
    static status_t createAudioPatch(const struct audio_patch *patch,
                                       audio_patch_handle_t *handle);
    static status_t releaseAudioPatch(audio_patch_handle_t handle);
    static status_t listAudioPatches(unsigned int *num_patches,
                                      struct audio_patch *patches,
                                      unsigned int *generation);
    static status_t setAudioPortConfig(const struct audio_port_config *config);
    static status_t acquireSoundTriggerSession(audio_session_t *session,
                                           audio_io_handle_t *ioHandle,
                                           audio_devices_t *device);
    static status_t releaseSoundTriggerSession(audio_session_t session);
    static audio_mode_t getPhoneState();
    static status_t registerPolicyMixes(const Vector<AudioMix>& mixes, bool registration);
    static status_t getRegisteredPolicyMixes(std::vector<AudioMix>& mixes);
    static status_t updatePolicyMixes(
        const std::vector<
                std::pair<AudioMix, std::vector<AudioMixMatchCriterion>>>& mixesWithUpdates);
    static status_t setUidDeviceAffinities(uid_t uid, const AudioDeviceTypeAddrVector& devices);
    static status_t removeUidDeviceAffinities(uid_t uid);
    static status_t setUserIdDeviceAffinities(int userId, const AudioDeviceTypeAddrVector& devices);
    static status_t removeUserIdDeviceAffinities(int userId);
    static status_t startAudioSource(const struct audio_port_config *source,
                                     const audio_attributes_t *attributes,
                                     audio_port_handle_t *portId);
    static status_t stopAudioSource(audio_port_handle_t portId);
    static status_t setMasterMono(bool mono);
    static status_t getMasterMono(bool *mono);
    static status_t setMasterBalance(float balance);
    static status_t getMasterBalance(float *balance);
    static float getStreamVolumeDB(
            audio_stream_type_t stream, int index, audio_devices_t device);
    static status_t getMicrophones(std::vector<media::MicrophoneInfoFw> *microphones);
    static status_t getHwOffloadFormatsSupportedForBluetoothMedia(
                                    audio_devices_t device, std::vector<audio_format_t> *formats);
    static status_t getSurroundFormats(unsigned int *numSurroundFormats,
                                       audio_format_t *surroundFormats,
                                       bool *surroundFormatsEnabled);
    static status_t getReportedSurroundFormats(unsigned int *numSurroundFormats,
                                               audio_format_t *surroundFormats);
    static status_t setSurroundFormatEnabled(audio_format_t audioFormat, bool enabled);
    static status_t setAssistantServicesUids(const std::vector<uid_t>& uids);
    static status_t setActiveAssistantServicesUids(const std::vector<uid_t>& activeUids);
    static status_t setA11yServicesUids(const std::vector<uid_t>& uids);
    static status_t setCurrentImeUid(uid_t uid);
    static bool isHapticPlaybackSupported();
    static bool isUltrasoundSupported();
    static status_t listAudioProductStrategies(AudioProductStrategyVector &strategies);
    static status_t getProductStrategyFromAudioAttributes(
            const audio_attributes_t &aa, product_strategy_t &productStrategy,
            bool fallbackOnDefault = true);
    static audio_attributes_t streamTypeToAttributes(audio_stream_type_t stream);
    static audio_stream_type_t attributesToStreamType(const audio_attributes_t &attr);
    static status_t listAudioVolumeGroups(AudioVolumeGroupVector &groups);
    static status_t getVolumeGroupFromAudioAttributes(
            const audio_attributes_t &aa, volume_group_t &volumeGroup,
            bool fallbackOnDefault = true);
    static status_t setRttEnabled(bool enabled);
    static bool isCallScreenModeSupported();
    static status_t setAudioHalPids(const std::vector<pid_t>& pids);
    static status_t setDevicesRoleForStrategy(product_strategy_t strategy,
            device_role_t role, const AudioDeviceTypeAddrVector &devices);
    static status_t removeDevicesRoleForStrategy(product_strategy_t strategy,
            device_role_t role, const AudioDeviceTypeAddrVector &devices);
    static status_t clearDevicesRoleForStrategy(product_strategy_t strategy,
            device_role_t role);
    static status_t getDevicesForRoleAndStrategy(product_strategy_t strategy,
            device_role_t role, AudioDeviceTypeAddrVector &devices);
    static status_t setDevicesRoleForCapturePreset(audio_source_t audioSource,
            device_role_t role, const AudioDeviceTypeAddrVector &devices);
    static status_t addDevicesRoleForCapturePreset(audio_source_t audioSource,
            device_role_t role, const AudioDeviceTypeAddrVector &devices);
    static status_t removeDevicesRoleForCapturePreset(
            audio_source_t audioSource, device_role_t role,
            const AudioDeviceTypeAddrVector& devices);
    static status_t clearDevicesRoleForCapturePreset(
            audio_source_t audioSource, device_role_t role);
    static status_t getDevicesForRoleAndCapturePreset(audio_source_t audioSource,
            device_role_t role, AudioDeviceTypeAddrVector &devices);
    static status_t getDeviceForStrategy(product_strategy_t strategy,
            AudioDeviceTypeAddr &device);
    static status_t getSpatializer(const sp<media::INativeSpatializerCallback>& callback,
                                        sp<media::ISpatializer>* spatializer);
    static status_t canBeSpatialized(const audio_attributes_t *attr,
                                     const audio_config_t *config,
                                     const AudioDeviceTypeAddrVector &devices,
                                     bool *canBeSpatialized);
    static status_t getSoundDoseInterface(const sp<media::ISoundDoseCallback>& callback,
                                          sp<media::ISoundDose>* soundDose);
    static status_t getDirectPlaybackSupport(const audio_attributes_t *attr,
                                             const audio_config_t *config,
                                             audio_direct_mode_t *directMode);
    static status_t getDirectProfilesForAttributes(const audio_attributes_t* attr,
                                            std::vector<audio_profile>* audioProfiles);
    static status_t setRequestedLatencyMode(
            audio_io_handle_t output, audio_latency_mode_t mode);
    static status_t getSupportedLatencyModes(audio_io_handle_t output,
            std::vector<audio_latency_mode_t>* modes);
    static status_t setBluetoothVariableLatencyEnabled(bool enabled);
    static status_t isBluetoothVariableLatencyEnabled(bool *enabled);
    static status_t supportsBluetoothVariableLatency(bool *support);
    static status_t getSupportedMixerAttributes(audio_port_handle_t portId,
                                                std::vector<audio_mixer_attributes_t> *mixerAttrs);
    static status_t setPreferredMixerAttributes(const audio_attributes_t *attr,
                                                audio_port_handle_t portId,
                                                uid_t uid,
                                                const audio_mixer_attributes_t *mixerAttr);
    static status_t getPreferredMixerAttributes(const audio_attributes_t* attr,
                                                audio_port_handle_t portId,
                                                std::optional<audio_mixer_attributes_t>* mixerAttr);
    static status_t clearPreferredMixerAttributes(const audio_attributes_t* attr,
                                                  audio_port_handle_t portId,
                                                  uid_t uid);
    static status_t getAudioPolicyConfig(media::AudioPolicyConfig *config);
    class CaptureStateListener : public virtual RefBase {
    public:
        virtual void onStateChanged(bool active) = 0;
        virtual void onServiceDied() = 0;
        virtual ~CaptureStateListener() = default;
    };
    static status_t registerSoundTriggerCaptureStateListener(
            const sp<CaptureStateListener>& listener);
    class AudioVolumeGroupCallback : public virtual RefBase
    {
    public:
        AudioVolumeGroupCallback() {}
        virtual ~AudioVolumeGroupCallback() {}
        virtual void onAudioVolumeGroupChanged(volume_group_t group, int flags) = 0;
        virtual void onServiceDied() = 0;
    };
    static status_t addAudioVolumeGroupCallback(const sp<AudioVolumeGroupCallback>& callback);
    static status_t removeAudioVolumeGroupCallback(const sp<AudioVolumeGroupCallback>& callback);
    class AudioPortCallback : public virtual RefBase
    {
    public:
                AudioPortCallback() {}
        virtual ~AudioPortCallback() {}
        virtual void onAudioPortListUpdate() = 0;
        virtual void onAudioPatchListUpdate() = 0;
        virtual void onServiceDied() = 0;
    };
    static status_t addAudioPortCallback(const sp<AudioPortCallback>& callback);
    static status_t removeAudioPortCallback(const sp<AudioPortCallback>& callback);
    class AudioDeviceCallback : public virtual RefBase
    {
    public:
                AudioDeviceCallback() {}
        virtual ~AudioDeviceCallback() {}
        virtual void onAudioDeviceUpdate(audio_io_handle_t audioIo,
                                         audio_port_handle_t deviceId) = 0;
    };
    static status_t addAudioDeviceCallback(const wp<AudioDeviceCallback>& callback,
                                           audio_io_handle_t audioIo,
                                           audio_port_handle_t portId);
    static status_t removeAudioDeviceCallback(const wp<AudioDeviceCallback>& callback,
                                              audio_io_handle_t audioIo,
                                              audio_port_handle_t portId);
    class SupportedLatencyModesCallback : public virtual RefBase
    {
    public:
                SupportedLatencyModesCallback() = default;
        virtual ~SupportedLatencyModesCallback() = default;
        virtual void onSupportedLatencyModesChanged(
                audio_io_handle_t output, const std::vector<audio_latency_mode_t>& modes) = 0;
    };
    static status_t addSupportedLatencyModesCallback(
            const sp<SupportedLatencyModesCallback>& callback);
    static status_t removeSupportedLatencyModesCallback(
            const sp<SupportedLatencyModesCallback>& callback);
    static audio_port_handle_t getDeviceIdForIo(audio_io_handle_t audioIo);
    static status_t setVibratorInfos(const std::vector<media::AudioVibratorInfo>& vibratorInfos);
    static status_t getMmapPolicyInfo(
            media::audio::common::AudioMMapPolicyType policyType,
            std::vector<media::audio::common::AudioMMapPolicyInfo> *policyInfos);
    static int32_t getAAudioMixerBurstCount();
    static int32_t getAAudioHardwareBurstMinUsec();
    class AudioFlingerClient: public IBinder::DeathRecipient, public media::BnAudioFlingerClient
    {
    public:
        AudioFlingerClient() = default;
        void clearIoCache() EXCLUDES(mMutex);
        status_t getInputBufferSize(uint32_t sampleRate, audio_format_t format,
                audio_channel_mask_t channelMask, size_t* buffSize) EXCLUDES(mMutex);
        sp<AudioIoDescriptor> getIoDescriptor(audio_io_handle_t ioHandle) EXCLUDES(mMutex);
        void binderDied(const wp<IBinder>& who) final;
        binder::Status ioConfigChanged(
                media::AudioIoConfigEvent event,
                const media::AudioIoDescriptor& ioDesc) final EXCLUDES(mMutex);
        binder::Status onSupportedLatencyModesChanged(
                int output,
                const std::vector<media::audio::common::AudioLatencyMode>& latencyModes)
                final EXCLUDES(mMutex);
        status_t addAudioDeviceCallback(const wp<AudioDeviceCallback>& callback,
                audio_io_handle_t audioIo, audio_port_handle_t portId) EXCLUDES(mMutex);
        status_t removeAudioDeviceCallback(const wp<AudioDeviceCallback>& callback,
                audio_io_handle_t audioIo, audio_port_handle_t portId) EXCLUDES(mMutex);
        status_t addSupportedLatencyModesCallback(
                const sp<SupportedLatencyModesCallback>& callback) EXCLUDES(mMutex);
        status_t removeSupportedLatencyModesCallback(
                const sp<SupportedLatencyModesCallback>& callback) EXCLUDES(mMutex);
        audio_port_handle_t getDeviceIdForIo(audio_io_handle_t audioIo) EXCLUDES(mMutex);
    private:
        mutable std::mutex mMutex;
        std::map<audio_io_handle_t, sp<AudioIoDescriptor>> mIoDescriptors GUARDED_BY(mMutex);
        std::map<audio_io_handle_t, std::map<audio_port_handle_t, wp<AudioDeviceCallback>>>
                mAudioDeviceCallbacks GUARDED_BY(mMutex);
        std::vector<wp<SupportedLatencyModesCallback>>
                mSupportedLatencyModesCallbacks GUARDED_BY(mMutex);
        size_t mInBuffSize GUARDED_BY(mMutex) = 0;
        uint32_t mInSamplingRate GUARDED_BY(mMutex) = 0;
        audio_format_t mInFormat GUARDED_BY(mMutex) = AUDIO_FORMAT_DEFAULT;
        audio_channel_mask_t mInChannelMask GUARDED_BY(mMutex) = AUDIO_CHANNEL_NONE;
        sp<AudioIoDescriptor> getIoDescriptor_l(audio_io_handle_t ioHandle) REQUIRES(mMutex);
    };
    class AudioPolicyServiceClient: public IBinder::DeathRecipient,
                                    public media::BnAudioPolicyServiceClient {
    public:
        AudioPolicyServiceClient() = default;
        int addAudioPortCallback(const sp<AudioPortCallback>& callback) EXCLUDES(mMutex);
        int removeAudioPortCallback(const sp<AudioPortCallback>& callback) EXCLUDES(mMutex);
        bool isAudioPortCbEnabled() const EXCLUDES(mMutex) {
            std::lock_guard _l(mMutex);
            return !mAudioPortCallbacks.empty();
        }
        int addAudioVolumeGroupCallback(
                const sp<AudioVolumeGroupCallback>& callback) EXCLUDES(mMutex);
        int removeAudioVolumeGroupCallback(
                const sp<AudioVolumeGroupCallback>& callback) EXCLUDES(mMutex);
        bool isAudioVolumeGroupCbEnabled() const EXCLUDES(mMutex) {
            std::lock_guard _l(mMutex);
            return !mAudioVolumeGroupCallbacks.empty();
        }
        void binderDied(const wp<IBinder>& who) final;
        binder::Status onAudioVolumeGroupChanged(int32_t group, int32_t flags) override;
        binder::Status onAudioPortListUpdate() override;
        binder::Status onAudioPatchListUpdate() override;
        binder::Status onDynamicPolicyMixStateUpdate(const std::string& regId,
                                                     int32_t state) override;
        binder::Status onRecordingConfigurationUpdate(
                int32_t event,
                const media::RecordClientInfo& clientInfo,
                const media::audio::common::AudioConfigBase& clientConfig,
                const std::vector<media::EffectDescriptor>& clientEffects,
                const media::audio::common::AudioConfigBase& deviceConfig,
                const std::vector<media::EffectDescriptor>& effects,
                int32_t patchHandle,
                media::audio::common::AudioSource source) override;
        binder::Status onRoutingUpdated();
        binder::Status onVolumeRangeInitRequest();
    private:
        mutable std::mutex mMutex;
        std::set<sp<AudioPortCallback>> mAudioPortCallbacks GUARDED_BY(mMutex);
        std::set<sp<AudioVolumeGroupCallback>> mAudioVolumeGroupCallbacks GUARDED_BY(mMutex);
    };
    private:
    static audio_io_handle_t getOutput(audio_stream_type_t stream);
    static sp<AudioFlingerClient> getAudioFlingerClient();
    static sp<AudioIoDescriptor> getIoDescriptor(audio_io_handle_t ioHandle);
    static void reportError(status_t err);
    [[clang::no_destroy]] static std::mutex gMutex;
    static dynamic_policy_callback gDynPolicyCallback GUARDED_BY(gMutex);
    static record_config_callback gRecordConfigCallback GUARDED_BY(gMutex);
    static routing_callback gRoutingCallback GUARDED_BY(gMutex);
    static vol_range_init_req_callback gVolRangeInitReqCallback GUARDED_BY(gMutex);
    [[clang::no_destroy]] static std::mutex gApsCallbackMutex;
    [[clang::no_destroy]] static std::mutex gErrorCallbacksMutex;
    [[clang::no_destroy]] static std::set<audio_error_callback> gAudioErrorCallbacks
            GUARDED_BY(gErrorCallbacksMutex);
    [[clang::no_destroy]] static std::mutex gSoundTriggerMutex;
    [[clang::no_destroy]] static sp<CaptureStateListenerImpl> gSoundTriggerCaptureStateListener
            GUARDED_BY(gSoundTriggerMutex);
};
}
#endif
