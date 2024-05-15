#ifndef ANDROID_AUDIOPOLICY_INTERFACE_H
#define ANDROID_AUDIOPOLICY_INTERFACE_H 
#include <media/AudioDeviceTypeAddr.h>
#include <media/AudioSystem.h>
#include <media/AudioPolicy.h>
#include <media/DeviceDescriptorBase.h>
#include <utils/String8.h>
namespace android {
class AudioPolicyInterface
{
public:
    typedef enum {
        API_INPUT_INVALID = -1,
        API_INPUT_LEGACY = 0,
        API_INPUT_MIX_CAPTURE,
        API_INPUT_MIX_EXT_POLICY_REROUTE,
        API_INPUT_MIX_PUBLIC_CAPTURE_PLAYBACK,
        API_INPUT_TELEPHONY_RX,
    } input_type_t;
    typedef enum {
        API_OUTPUT_INVALID = -1,
        API_OUTPUT_LEGACY = 0,
        API_OUT_MIX_PLAYBACK,
        API_OUTPUT_TELEPHONY_TX,
    } output_type_t;
public:
    virtual ~AudioPolicyInterface() {}
    virtual status_t setDeviceConnectionState(audio_devices_t device,
                                              audio_policy_dev_state_t state,
                                              const char *device_address,
                                              const char *device_name,
                                              audio_format_t encodedFormat) = 0;
    virtual audio_policy_dev_state_t getDeviceConnectionState(audio_devices_t device,
                                                                          const char *device_address) = 0;
    virtual status_t handleDeviceConfigChange(audio_devices_t device,
                                              const char *device_address,
                                              const char *device_name,
                                              audio_format_t encodedFormat) = 0;
    virtual void setPhoneState(audio_mode_t state) = 0;
    virtual void setForceUse(audio_policy_force_use_t usage, audio_policy_forced_cfg_t config) = 0;
    virtual audio_policy_forced_cfg_t getForceUse(audio_policy_force_use_t usage) = 0;
    virtual void setSystemProperty(const char* property, const char* value) = 0;
    virtual status_t initCheck() = 0;
    virtual audio_io_handle_t getOutput(audio_stream_type_t stream) = 0;
    virtual status_t getOutputForAttr(const audio_attributes_t *attr,
                                        audio_io_handle_t *output,
                                        audio_session_t session,
                                        audio_stream_type_t *stream,
                                        uid_t uid,
                                        const audio_config_t *config,
                                        audio_output_flags_t *flags,
                                        audio_port_handle_t *selectedDeviceId,
                                        audio_port_handle_t *portId,
                                        std::vector<audio_io_handle_t> *secondaryOutputs,
                                        output_type_t *outputType) = 0;
    virtual status_t startOutput(audio_port_handle_t portId) = 0;
    virtual status_t stopOutput(audio_port_handle_t portId) = 0;
    virtual void releaseOutput(audio_port_handle_t portId) = 0;
    virtual status_t getInputForAttr(const audio_attributes_t *attr,
                                     audio_io_handle_t *input,
                                     audio_unique_id_t riid,
                                     audio_session_t session,
                                     uid_t uid,
                                     const audio_config_base_t *config,
                                     audio_input_flags_t flags,
                                     audio_port_handle_t *selectedDeviceId,
                                     input_type_t *inputType,
                                     audio_port_handle_t *portId) = 0;
    virtual status_t startInput(audio_port_handle_t portId) = 0;
    virtual status_t stopInput(audio_port_handle_t portId) = 0;
    virtual void releaseInput(audio_port_handle_t portId) = 0;
    virtual void initStreamVolume(audio_stream_type_t stream,
                                      int indexMin,
                                      int indexMax) = 0;
    virtual status_t setStreamVolumeIndex(audio_stream_type_t stream,
                                          int index,
                                          audio_devices_t device) = 0;
    virtual status_t getStreamVolumeIndex(audio_stream_type_t stream,
                                          int *index,
                                          audio_devices_t device) = 0;
    virtual status_t setVolumeIndexForAttributes(const audio_attributes_t &attr,
                                                 int index,
                                                 audio_devices_t device) = 0;
    virtual status_t getVolumeIndexForAttributes(const audio_attributes_t &attr,
                                                 int &index,
                                                 audio_devices_t device) = 0;
    virtual status_t getMaxVolumeIndexForAttributes(const audio_attributes_t &attr,
                                                    int &index) = 0;
    virtual status_t getMinVolumeIndexForAttributes(const audio_attributes_t &attr,
                                                    int &index) = 0;
    virtual uint32_t getStrategyForStream(audio_stream_type_t stream) = 0;
    virtual audio_devices_t getDevicesForStream(audio_stream_type_t stream) = 0;
    virtual audio_io_handle_t getOutputForEffect(const effect_descriptor_t *desc) = 0;
    virtual status_t registerEffect(const effect_descriptor_t *desc,
                                    audio_io_handle_t io,
                                    uint32_t strategy,
                                    int session,
                                    int id) = 0;
    virtual status_t unregisterEffect(int id) = 0;
    virtual status_t setEffectEnabled(int id, bool enabled) = 0;
    virtual status_t moveEffectsToIo(const std::vector<int>& ids, audio_io_handle_t io) = 0;
    virtual bool isStreamActive(audio_stream_type_t stream, uint32_t inPastMs = 0) const = 0;
    virtual bool isStreamActiveRemotely(audio_stream_type_t stream,
                                        uint32_t inPastMs = 0) const = 0;
    virtual bool isSourceActive(audio_source_t source) const = 0;
    virtual status_t dump(int fd) = 0;
    virtual status_t setAllowedCapturePolicy(uid_t uid, audio_flags_mask_t flags) = 0;
    virtual bool isOffloadSupported(const audio_offload_info_t& offloadInfo) = 0;
    virtual bool isDirectOutputSupported(const audio_config_base_t& config,
                                         const audio_attributes_t& attributes) = 0;
    virtual status_t listAudioPorts(audio_port_role_t role,
                                    audio_port_type_t type,
                                    unsigned int *num_ports,
                                    struct audio_port *ports,
                                    unsigned int *generation) = 0;
    virtual status_t getAudioPort(struct audio_port *port) = 0;
    virtual status_t createAudioPatch(const struct audio_patch *patch,
                                       audio_patch_handle_t *handle,
                                       uid_t uid) = 0;
    virtual status_t releaseAudioPatch(audio_patch_handle_t handle,
                                          uid_t uid) = 0;
    virtual status_t listAudioPatches(unsigned int *num_patches,
                                      struct audio_patch *patches,
                                      unsigned int *generation) = 0;
    virtual status_t setAudioPortConfig(const struct audio_port_config *config) = 0;
    virtual void releaseResourcesForUid(uid_t uid) = 0;
    virtual status_t acquireSoundTriggerSession(audio_session_t *session,
                                           audio_io_handle_t *ioHandle,
                                           audio_devices_t *device) = 0;
    virtual status_t releaseSoundTriggerSession(audio_session_t session) = 0;
    virtual status_t registerPolicyMixes(const Vector<AudioMix>& mixes) = 0;
    virtual status_t unregisterPolicyMixes(Vector<AudioMix> mixes) = 0;
    virtual status_t setUidDeviceAffinities(uid_t uid, const Vector<AudioDeviceTypeAddr>& devices)
            = 0;
    virtual status_t removeUidDeviceAffinities(uid_t uid) = 0;
    virtual status_t startAudioSource(const struct audio_port_config *source,
                                      const audio_attributes_t *attributes,
                                      audio_port_handle_t *portId,
                                      uid_t uid) = 0;
    virtual status_t stopAudioSource(audio_port_handle_t portId) = 0;
    virtual status_t setMasterMono(bool mono) = 0;
    virtual status_t getMasterMono(bool *mono) = 0;
    virtual float getStreamVolumeDB(
                audio_stream_type_t stream, int index, audio_devices_t device) = 0;
    virtual status_t getSurroundFormats(unsigned int *numSurroundFormats,
                                        audio_format_t *surroundFormats,
                                        bool *surroundFormatsEnabled,
                                        bool reported) = 0;
    virtual status_t setSurroundFormatEnabled(audio_format_t audioFormat, bool enabled) = 0;
    virtual bool isHapticPlaybackSupported() = 0;
    virtual status_t getHwOffloadEncodingFormatsSupportedForA2DP(
                std::vector<audio_format_t> *formats) = 0;
    virtual void setAppState(audio_port_handle_t portId, app_state_t state) = 0;
    virtual status_t listAudioProductStrategies(AudioProductStrategyVector &strategies) = 0;
    virtual status_t getProductStrategyFromAudioAttributes(const AudioAttributes &aa,
                                                           product_strategy_t &productStrategy) = 0;
    virtual status_t listAudioVolumeGroups(AudioVolumeGroupVector &groups) = 0;
    virtual status_t getVolumeGroupFromAudioAttributes(const AudioAttributes &aa,
                                                       volume_group_t &volumeGroup) = 0;
    virtual bool isCallScreenModeSupported() = 0;
    virtual status_t setPreferredDeviceForStrategy(product_strategy_t strategy,
                                                   const AudioDeviceTypeAddr &device) = 0;
    virtual status_t removePreferredDeviceForStrategy(product_strategy_t strategy) = 0;
    virtual status_t getPreferredDeviceForStrategy(product_strategy_t strategy,
                                                   AudioDeviceTypeAddr &device) = 0;
};
class AudioPolicyClientInterface
{
public:
    virtual ~AudioPolicyClientInterface() {}
    virtual audio_module_handle_t loadHwModule(const char *name) = 0;
    virtual status_t openOutput(audio_module_handle_t module,
                                audio_io_handle_t *output,
                                audio_config_t *config,
                                const sp<DeviceDescriptorBase>& device,
                                uint32_t *latencyMs,
                                audio_output_flags_t flags) = 0;
    virtual audio_io_handle_t openDuplicateOutput(audio_io_handle_t output1, audio_io_handle_t output2) = 0;
    virtual status_t closeOutput(audio_io_handle_t output) = 0;
    virtual status_t suspendOutput(audio_io_handle_t output) = 0;
    virtual status_t restoreOutput(audio_io_handle_t output) = 0;
    virtual status_t openInput(audio_module_handle_t module,
                               audio_io_handle_t *input,
                               audio_config_t *config,
                               audio_devices_t *device,
                               const String8& address,
                               audio_source_t source,
                               audio_input_flags_t flags) = 0;
    virtual status_t closeInput(audio_io_handle_t input) = 0;
    virtual status_t setStreamVolume(audio_stream_type_t stream, float volume, audio_io_handle_t output, int delayMs = 0) = 0;
    virtual status_t invalidateStream(audio_stream_type_t stream) = 0;
    virtual void setParameters(audio_io_handle_t ioHandle, const String8& keyValuePairs, int delayMs = 0) = 0;
    virtual String8 getParameters(audio_io_handle_t ioHandle, const String8& keys) = 0;
    virtual status_t setVoiceVolume(float volume, int delayMs = 0) = 0;
    virtual status_t moveEffects(audio_session_t session,
                                     audio_io_handle_t srcOutput,
                                     audio_io_handle_t dstOutput) = 0;
    virtual void setEffectSuspended(int effectId,
                                    audio_session_t sessionId,
                                    bool suspended) = 0;
    virtual status_t createAudioPatch(const struct audio_patch *patch,
                                       audio_patch_handle_t *handle,
                                       int delayMs) = 0;
    virtual status_t releaseAudioPatch(audio_patch_handle_t handle,
                                       int delayMs) = 0;
    virtual status_t setAudioPortConfig(const struct audio_port_config *config, int delayMs) = 0;
    virtual void onAudioPortListUpdate() = 0;
    virtual void onAudioPatchListUpdate() = 0;
    virtual void onAudioVolumeGroupChanged(volume_group_t group, int flags) = 0;
    virtual audio_unique_id_t newAudioUniqueId(audio_unique_id_use_t use) = 0;
    virtual void onDynamicPolicyMixStateUpdate(String8 regId, int32_t state) = 0;
    virtual void onRecordingConfigurationUpdate(int event,
                                                const record_client_info_t *clientInfo,
                                                const audio_config_base_t *clientConfig,
                                                std::vector<effect_descriptor_t> clientEffects,
                                                const audio_config_base_t *deviceConfig,
                                                std::vector<effect_descriptor_t> effects,
                                                audio_patch_handle_t patchHandle,
                                                audio_source_t source) = 0;
};
extern "C" AudioPolicyInterface* createAudioPolicyManager(AudioPolicyClientInterface *clientInterface);
extern "C" void destroyAudioPolicyManager(AudioPolicyInterface *interface);
}
#endif
