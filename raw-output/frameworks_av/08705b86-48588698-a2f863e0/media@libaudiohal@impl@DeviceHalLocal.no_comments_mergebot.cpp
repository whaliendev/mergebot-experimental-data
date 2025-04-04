#define LOG_TAG "DeviceHalLocal"
#include <media/AudioParameter.h>
#include <utils/Log.h>
#include "DeviceHalLocal.h"
#include "StreamHalLocal.h"
namespace android {
status_t DeviceHalLocal::getSupportedDevices(uint32_t *devices) {
    if (mDev->get_supported_devices == NULL) return INVALID_OPERATION;
    *devices = mDev->get_supported_devices(mDev);
    return OK;
}
status_t DeviceHalLocal::initCheck() {
    return mDev->init_check(mDev);
}
status_t DeviceHalLocal::setVoiceVolume(float volume) {
    return mDev->set_voice_volume(mDev, volume);
}
status_t DeviceHalLocal::setMasterVolume(float volume) {
    if (mDev->set_master_volume == NULL) return INVALID_OPERATION;
    return mDev->set_master_volume(mDev, volume);
}
status_t DeviceHalLocal::getMasterVolume(float *volume) {
    if (mDev->get_master_volume == NULL) return INVALID_OPERATION;
    return mDev->get_master_volume(mDev, volume);
}
status_t DeviceHalLocal::setMode(audio_mode_t mode) {
    return mDev->set_mode(mDev, mode);
}
status_t DeviceHalLocal::setMicMute(bool state) {
    return mDev->set_mic_mute(mDev, state);
}
status_t DeviceHalLocal::getMicMute(bool *state) {
    return mDev->get_mic_mute(mDev, state);
}
status_t DeviceHalLocal::setMasterMute(bool state) {
    if (mDev->set_master_mute == NULL) return INVALID_OPERATION;
    return mDev->set_master_mute(mDev, state);
}
status_t DeviceHalLocal::getMasterMute(bool *state) {
    if (mDev->get_master_mute == NULL) return INVALID_OPERATION;
    return mDev->get_master_mute(mDev, state);
}
status_t DeviceHalLocal::setParameters(const String8& kvPairs) {
    return mDev->set_parameters(mDev, kvPairs.string());
}
status_t DeviceHalLocal::getParameters(const String8& keys, String8 *values) {
    char *halValues = mDev->get_parameters(mDev, keys.string());
    if (halValues != NULL) {
        values->setTo(halValues);
        free(halValues);
    } else {
        values->clear();
    }
    return OK;
}
status_t DeviceHalLocal::getInputBufferSize(const struct audio_config *config, size_t *size) {
    *size = mDev->get_input_buffer_size(mDev, config);
    return OK;
}
status_t DeviceHalLocal::openOutputStream(audio_io_handle_t handle, audio_devices_t deviceType, audio_output_flags_t flags, struct audio_config *config, const char *address, sp<StreamOutHalInterface> *outStream) {
    audio_stream_out_t *halStream;
    ALOGV("open_output_stream handle: %d devices: %x flags: %#x"
            "srate: %d format %#x channels %x address %s",
            handle, deviceType, flags,
            config->sample_rate, config->format, config->channel_mask,
            address);
    int openResut = mDev->open_output_stream(
            mDev, handle, deviceType, flags, config, &halStream, address);
    if (openResut == OK) {
        *outStream = new StreamOutHalLocal(halStream, this);
    }
    ALOGV("open_output_stream status %d stream %p", openResut, halStream);
    return openResut;
}
status_t DeviceHalLocal::openInputStream(audio_io_handle_t handle, audio_devices_t devices, struct audio_config *config, audio_input_flags_t flags, const char *address, audio_source_t source, audio_devices_t , const char * , sp<StreamInHalInterface> *inStream) {
    audio_stream_in_t *halStream;
    ALOGV("open_input_stream handle: %d devices: %x flags: %#x "
            "srate: %d format %#x channels %x address %s source %d",
            handle, devices, flags,
            config->sample_rate, config->format, config->channel_mask,
            address, source);
    int openResult = mDev->open_input_stream(
            mDev, handle, devices, config, &halStream, flags, address, source);
    if (openResult == OK) {
        *inStream = new StreamInHalLocal(halStream, this);
    }
    ALOGV("open_input_stream status %d stream %p", openResult, inStream);
    return openResult;
}
status_t DeviceHalLocal::supportsAudioPatches(bool *supportsPatches) {
    *supportsPatches = version() >= AUDIO_DEVICE_API_VERSION_3_0;
    return OK;
}
status_t DeviceHalLocal::createAudioPatch(unsigned int num_sources, const struct audio_port_config *sources, unsigned int num_sinks, const struct audio_port_config *sinks, audio_patch_handle_t *patch) {
    if (version() >= AUDIO_DEVICE_API_VERSION_3_0) {
        return mDev->create_audio_patch(
                mDev, num_sources, sources, num_sinks, sinks, patch);
    } else {
        return INVALID_OPERATION;
    }
}
status_t DeviceHalLocal::releaseAudioPatch(audio_patch_handle_t patch) {
    if (version() >= AUDIO_DEVICE_API_VERSION_3_0) {
        return mDev->release_audio_patch(mDev, patch);
    } else {
        return INVALID_OPERATION;
    }
}
status_t DeviceHalLocal::getAudioPort(struct audio_port *port) {
    return mDev->get_audio_port(mDev, port);
}
status_t DeviceHalLocal::getAudioPort(struct audio_port_v7 *port) {
#if MAJOR_VERSION >= 7
    if (version() >= AUDIO_DEVICE_API_VERSION_3_2) {
        return mDev->get_audio_port_v7(mDev, port);
    }
#endif
    struct audio_port audioPort = {};
    audio_populate_audio_port(port, &audioPort);
    status_t status = getAudioPort(&audioPort);
    if (status == NO_ERROR) {
        audio_populate_audio_port_v7(&audioPort, port);
    }
    return status;
}
status_t DeviceHalLocal::setAudioPortConfig(const struct audio_port_config *config) {
    if (version() >= AUDIO_DEVICE_API_VERSION_3_0)
        return mDev->set_audio_port_config(mDev, config);
    else
        return INVALID_OPERATION;
}
}
