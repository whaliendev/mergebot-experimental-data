#ifndef ANDROID_HARDWARE_DEVICE_HAL_HIDL_H
#define ANDROID_HARDWARE_DEVICE_HAL_HIDL_H 
#include PATH(android/hardware/audio/FILE_VERSION/IDevice.h)
#include PATH(android/hardware/audio/FILE_VERSION/IPrimaryDevice.h)
#include <media/audiohal/DeviceHalInterface.h>
#include <media/audiohal/EffectHalInterface.h>
#include "CoreConversionHelperHidl.h"
namespace android {
class DeviceHalHidl : public DeviceHalInterface, public CoreConversionHelperHidl
{
public:
    status_t getAudioPorts(std::vector<media::audio::common::AudioPort> *ports) override;
    status_t getAudioRoutes(std::vector<media::AudioRoute> *routes) override;
    status_t getSupportedDevices(uint32_t *devices) override;
    status_t initCheck() override;
    status_t setVoiceVolume(float volume) override;
    status_t setMasterVolume(float volume) override;
    status_t getMasterVolume(float *volume) override;
    status_t setMode(audio_mode_t mode) override;
    status_t setMicMute(bool state) override;
    status_t getMicMute(bool *state) override;
    status_t setMasterMute(bool state) override;
    status_t getMasterMute(bool *state) override;
    status_t setParameters(const String8& kvPairs) override;
    status_t getParameters(const String8& keys, String8 *values) override;
    status_t getInputBufferSize(const struct audio_config* config, size_t* size) override;
    status_t openOutputStream(audio_io_handle_t handle, audio_devices_t devices,
                              audio_output_flags_t flags, struct audio_config* config,
                              const char* address, sp<StreamOutHalInterface>* outStream) override;
    status_t openInputStream(audio_io_handle_t handle, audio_devices_t devices,
                             struct audio_config* config, audio_input_flags_t flags,
                             const char* address, audio_source_t source,
                             audio_devices_t outputDevice, const char* outputDeviceAddress,
                             sp<StreamInHalInterface>* inStream) override;
    status_t supportsAudioPatches(bool* supportsPatches) override;
    status_t createAudioPatch(unsigned int num_sources, const struct audio_port_config* sources,
                              unsigned int num_sinks, const struct audio_port_config* sinks,
                              audio_patch_handle_t* patch) override;
    status_t releaseAudioPatch(audio_patch_handle_t patch) override;
    status_t getAudioPort(struct audio_port *port) override;
    status_t getAudioPort(struct audio_port_v7 *port) override;
    status_t setAudioPortConfig(const struct audio_port_config *config) override;
    status_t getMicrophones(std::vector<audio_microphone_characteristic_t>* microphones) override;
    status_t addDeviceEffect(audio_port_handle_t device, sp<EffectHalInterface> effect) override;
    status_t removeDeviceEffect(audio_port_handle_t device, sp<EffectHalInterface> effect) override;
    status_t getMmapPolicyInfos(
            media::audio::common::AudioMMapPolicyType policyType __unused,
            std::vector<media::audio::common::AudioMMapPolicyInfo> *policyInfos __unused) override {
        return INVALID_OPERATION;
    }
    int32_t getAAudioMixerBurstCount() override {
        return INVALID_OPERATION;
    }
    int32_t getAAudioHardwareBurstMinUsec() override {
        return INVALID_OPERATION;
    }
    int32_t supportsBluetoothVariableLatency(bool* supports __unused) override {
        return INVALID_OPERATION;
    }
    status_t setConnectedState(const struct audio_port_v7 *port, bool connected) override;
    status_t setSimulateDeviceConnections(bool enabled __unused) override {
        return INVALID_OPERATION;
    }
    error::Result<audio_hw_sync_t> getHwAvSync() override;
    status_t dump(int fd, const Vector<String16>& args) override;
    status_t getSoundDoseInterface(const std::string& module,
                                   ::ndk::SpAIBinder* soundDoseBinder) override;
    status_t prepareToDisconnectExternalDevice(const struct audio_port_v7* port) override;
private:
    friend class DevicesFactoryHalHidl;
    sp<::android::hardware::audio::CPP_VERSION::IDevice> mDevice;
    sp<::android::hardware::audio::CPP_VERSION::IPrimaryDevice> mPrimaryDevice;
    bool supportsSetConnectedState7_1 = true;
    class SoundDoseWrapper;
    const std::unique_ptr<SoundDoseWrapper> mSoundDoseWrapper;
    std::set<audio_port_handle_t> mDeviceDisconnectionNotified;
    explicit DeviceHalHidl(
            const sp<::android::hardware::audio::CPP_VERSION::IPrimaryDevice>& device);
    explicit DeviceHalHidl(
            const sp<::android::hardware::audio::CPP_VERSION::IPrimaryDevice>& device);
    virtual ~DeviceHalHidl();
};
}
#endif
