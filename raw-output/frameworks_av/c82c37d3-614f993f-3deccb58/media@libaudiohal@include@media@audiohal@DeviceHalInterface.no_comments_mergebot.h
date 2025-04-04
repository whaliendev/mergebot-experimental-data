#ifndef ANDROID_HARDWARE_DEVICE_HAL_INTERFACE_H
#define ANDROID_HARDWARE_DEVICE_HAL_INTERFACE_H 
#include <android/media/audio/common/AudioMMapPolicyInfo.h>
#include <android/media/audio/common/AudioMMapPolicyType.h>
#include <android/media/audio/common/AudioPort.h>
#include <android/media/AudioRoute.h>
#include <error/Result.h>
#include <media/audiohal/EffectHalInterface.h>
#include <system/audio.h>
#include <utils/Errors.h>
#include <utils/RefBase.h>
#include <utils/String8.h>
namespace ndk {
class SpAIBinder;
}
namespace android {
class StreamInHalInterface;
class StreamOutHalInterface;
class DeviceHalInterface : public virtual RefBase
{
public:
    virtual status_t getAudioPorts(std::vector<media::audio::common::AudioPort> *ports) = 0;
    virtual status_t getAudioRoutes(std::vector<media::AudioRoute> *routes) = 0;
    virtual status_t getSupportedDevices(uint32_t *devices) = 0;
    virtual status_t initCheck() = 0;
    virtual status_t setVoiceVolume(float volume) = 0;
    virtual status_t setMasterVolume(float volume) = 0;
    virtual status_t getMasterVolume(float *volume) = 0;
    virtual status_t setMode(audio_mode_t mode) = 0;
    virtual status_t setMicMute(bool state) = 0;
    virtual status_t getMicMute(bool *state) = 0;
    virtual status_t setMasterMute(bool state) = 0;
    virtual status_t getMasterMute(bool *state) = 0;
    virtual status_t setParameters(const String8& kvPairs) = 0;
    virtual status_t getParameters(const String8& keys, String8 *values) = 0;
    virtual status_t getInputBufferSize(const struct audio_config *config,
            size_t *size) = 0;
    virtual status_t openOutputStream(
            audio_io_handle_t handle,
            audio_devices_t deviceType,
            audio_output_flags_t flags,
            struct audio_config *config,
            const char *address,
            sp<StreamOutHalInterface> *outStream) = 0;
    virtual status_t openInputStream(
            audio_io_handle_t handle,
            audio_devices_t devices,
            struct audio_config *config,
            audio_input_flags_t flags,
            const char *address,
            audio_source_t source,
            audio_devices_t outputDevice,
            const char *outputDeviceAddress,
            sp<StreamInHalInterface> *inStream) = 0;
    virtual status_t supportsAudioPatches(bool *supportsPatches) = 0;
    virtual status_t createAudioPatch(
            unsigned int num_sources,
            const struct audio_port_config *sources,
            unsigned int num_sinks,
            const struct audio_port_config *sinks,
            audio_patch_handle_t *patch) = 0;
    virtual status_t releaseAudioPatch(audio_patch_handle_t patch) = 0;
    virtual status_t getAudioPort(struct audio_port* port) = 0;
    virtual status_t getAudioPort(struct audio_port_v7 *port) = 0;
    virtual status_t setAudioPortConfig(const struct audio_port_config *config) = 0;
    virtual status_t getMicrophones(
            std::vector<audio_microphone_characteristic_t>* microphones) = 0;
    virtual status_t addDeviceEffect(
            audio_port_handle_t device, sp<EffectHalInterface> effect) = 0;
    virtual status_t removeDeviceEffect(
            audio_port_handle_t device, sp<EffectHalInterface> effect) = 0;
    virtual status_t getMmapPolicyInfos(
            media::audio::common::AudioMMapPolicyType policyType,
            std::vector<media::audio::common::AudioMMapPolicyInfo> *policyInfos) = 0;
    virtual int32_t getAAudioMixerBurstCount() = 0;
    virtual int32_t getAAudioHardwareBurstMinUsec() = 0;
    virtual int32_t supportsBluetoothVariableLatency(bool* supports) = 0;
    virtual status_t setConnectedState(const struct audio_port_v7* port, bool connected) = 0;
    virtual status_t setSimulateDeviceConnections(bool enabled) = 0;
    virtual error::Result<audio_hw_sync_t> getHwAvSync() = 0;
    virtual status_t dump(int fd, const Vector<String16>& args) = 0;
    virtual status_t getSoundDoseInterface(const std::string& module,
                                           ::ndk::SpAIBinder* soundDoseBinder) = 0;
    virtual status_t prepareToDisconnectExternalDevice(const struct audio_port_v7* port) = 0;
protected:
    DeviceHalInterface() {}
    virtual ~DeviceHalInterface() {}
};
}
#endif
