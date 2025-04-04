       
#include "AudioPolicyInterface.h"
namespace android {
class AudioPolicyTestClient : public AudioPolicyClientInterface
{
public:
    virtual ~AudioPolicyTestClient() = default;
    audio_module_handle_t loadHwModule(const char* ) override {
        return AUDIO_MODULE_HANDLE_NONE;
    }
    status_t openOutput(audio_module_handle_t ,
                        audio_io_handle_t* ,
                        audio_config_t* ,
                        audio_config_base_t* ,
                        const sp<DeviceDescriptorBase>& ,
                        uint32_t* ,
                        audio_output_flags_t ) override { return NO_INIT; }
    audio_io_handle_t openDuplicateOutput(audio_io_handle_t ,
                                          audio_io_handle_t ) override {
        return AUDIO_IO_HANDLE_NONE;
    }
    status_t closeOutput(audio_io_handle_t ) override { return NO_INIT; }
    status_t suspendOutput(audio_io_handle_t ) override { return NO_INIT; }
    status_t restoreOutput(audio_io_handle_t ) override { return NO_INIT; }
    status_t openInput(audio_module_handle_t ,
                       audio_io_handle_t* ,
                       audio_config_t* ,
                       audio_devices_t* ,
                       const String8& ,
                       audio_source_t ,
                       audio_input_flags_t ) override { return NO_INIT; }
    status_t closeInput(audio_io_handle_t ) override { return NO_INIT; }
    status_t setStreamVolume(audio_stream_type_t ,
                             float ,
                             audio_io_handle_t ,
                             int ) override { return NO_INIT; }
    void setParameters(audio_io_handle_t ,
                       const String8& ,
                       int ) override { }
    String8 getParameters(audio_io_handle_t ,
                          const String8& ) override { return String8(); }
    status_t setVoiceVolume(float , int ) override { return NO_INIT; }
    status_t moveEffects(audio_session_t ,
                         audio_io_handle_t ,
                         audio_io_handle_t ) override { return NO_INIT; }
    status_t createAudioPatch(const struct audio_patch* ,
                              audio_patch_handle_t* ,
                              int ) override { return NO_INIT; }
    status_t releaseAudioPatch(audio_patch_handle_t ,
                               int ) override { return NO_INIT; }
    status_t setAudioPortConfig(const struct audio_port_config* ,
                                int ) override { return NO_INIT; }
    void onAudioPortListUpdate() override { }
    void onAudioPatchListUpdate() override { }
    void onAudioVolumeGroupChanged(volume_group_t , int ) override { }
    audio_unique_id_t newAudioUniqueId(audio_unique_id_use_t ) override { return 0; }
    void onDynamicPolicyMixStateUpdate(String8 , int32_t ) override { }
    void onRecordingConfigurationUpdate(int event __unused,
                                        const record_client_info_t *clientInfo __unused,
                                        const audio_config_base_t *clientConfig __unused,
                                        std::vector<effect_descriptor_t> clientEffects __unused,
                                        const audio_config_base_t *deviceConfig __unused,
                                        std::vector<effect_descriptor_t> effects __unused,
                                        audio_patch_handle_t patchHandle __unused,
                                        audio_source_t source __unused) override { }
    void onRoutingUpdated() override { }
    void onVolumeRangeInitRequest() override { }
    void setEffectSuspended(int effectId __unused,
                            audio_session_t sessionId __unused,
                            bool suspended __unused) {}
    void setSoundTriggerCaptureState(bool active __unused) override {};
    status_t getAudioPort(struct audio_port_v7 *port __unused) override {
        return INVALID_OPERATION;
    };
    status_t updateSecondaryOutputs(
            const TrackSecondaryOutputsMap& trackSecondaryOutputs __unused) override {
        return NO_INIT;
    }
    status_t setDeviceConnectedState(const struct audio_port_v7 *port __unused,
                                     media::DeviceConnectedState state __unused) override {
        return NO_INIT;
    }
    status_t invalidateTracks(const std::vector<audio_port_handle_t>& ) override {
        return NO_INIT;
    }
};
}
