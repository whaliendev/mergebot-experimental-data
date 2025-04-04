#ifndef ANDROID_HARDWARE_DEVICE_HAL_HIDL_H
#define ANDROID_HARDWARE_DEVICE_HAL_HIDL_H 
#include PATH(android/hardware/audio/FILE_VERSION/IDevice.h)
#include PATH(android/hardware/audio/FILE_VERSION/IPrimaryDevice.h)
#include <media/audiohal/DeviceHalInterface.h>
#include <media/audiohal/EffectHalInterface.h>
#include "ConversionHelperHidl.h"
namespace android {
class DeviceHalHidl : public DeviceHalInterface, public ConversionHelperHidl
{
  public:
    virtual status_t getSupportedDevices(uint32_t *devices);
    virtual status_t initCheck();
    virtual status_t setVoiceVolume(float volume);
    virtual status_t setMasterVolume(float volume);
    virtual status_t getMasterVolume(float *volume);
    virtual status_t setMode(audio_mode_t mode);
    virtual status_t setMicMute(bool state);
    virtual status_t getMicMute(bool *state);
    virtual status_t setMasterMute(bool state);
    virtual status_t getMasterMute(bool *state);
    virtual status_t setParameters(const String8& kvPairs);
    virtual status_t getParameters(const String8& keys, String8 *values);
    virtual status_t getInputBufferSize(const struct audio_config *config,
            size_t *size);
    virtual status_t openOutputStream(
            audio_io_handle_t handle,
            audio_devices_t devices,
            audio_output_flags_t flags,
            struct audio_config *config,
            const char *address,
            sp<StreamOutHalInterface> *outStream);
    virtual status_t openInputStream(
            audio_io_handle_t handle,
            audio_devices_t devices,
            struct audio_config *config,
            audio_input_flags_t flags,
            const char *address,
            audio_source_t source,
            audio_devices_t outputDevice,
            const char *outputDeviceAddress,
            sp<StreamInHalInterface> *inStream);
    virtual status_t supportsAudioPatches(bool *supportsPatches);
    virtual status_t createAudioPatch(
            unsigned int num_sources,
            const struct audio_port_config *sources,
            unsigned int num_sinks,
            const struct audio_port_config *sinks,
            audio_patch_handle_t *patch);
    virtual status_t releaseAudioPatch(audio_patch_handle_t patch);
    virtual status_t getAudioPort(struct audio_port *port);
    virtual status_t getAudioPort(struct audio_port_v7 *port);
    virtual status_t setAudioPortConfig(const struct audio_port_config *config);
    virtual status_t getMicrophones(std::vector<media::MicrophoneInfo> *microphones);
    status_t addDeviceEffect(audio_port_handle_t device, sp<EffectHalInterface> effect) override;
    status_t removeDeviceEffect(audio_port_handle_t device, sp<EffectHalInterface> effect) override;
<<<<<<< HEAD
    status_t setConnectedState(const struct audio_port_v7 *port, bool connected) override;
    status_t dump(int fd, const Vector<String16>& args) override;
||||||| a2f863e050
    virtual status_t dump(int fd);
=======
    status_t dump(int fd, const Vector<String16>& args) override;
>>>>>>> 48588698
  private:
    friend class DevicesFactoryHalHidl;
    sp<::android::hardware::audio::CPP_VERSION::IDevice> mDevice;
    sp<::android::hardware::audio::CPP_VERSION::IPrimaryDevice> mPrimaryDevice;
    bool supportsSetConnectedState7_1 = true;
    explicit DeviceHalHidl(const sp<::android::hardware::audio::CPP_VERSION::IDevice>& device);
    explicit DeviceHalHidl(
            const sp<::android::hardware::audio::CPP_VERSION::IPrimaryDevice>& device);
    virtual ~DeviceHalHidl();
    template <typename HalPort> status_t getAudioPortImpl(HalPort *port);
};
}
#endif
