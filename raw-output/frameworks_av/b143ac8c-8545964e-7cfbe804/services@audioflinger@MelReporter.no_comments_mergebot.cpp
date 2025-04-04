#define LOG_TAG "AudioFlinger::MelReporter"
#include "AudioFlinger.h"
#define LOG_TAG "MelReporter"
#include "MelReporter.h"
#include <android/media/ISoundDoseCallback.h>
#include <audio_utils/power.h>
#include <utils/Log.h>
using aidl::android::hardware::audio::core::sounddose::ISoundDose;
namespace android {
bool MelReporter::activateHalSoundDoseComputation(const std::string& module, const sp<DeviceHalInterface>& device) {
    if (mSoundDoseManager->forceUseFrameworkMel()) {
        ALOGD("%s: Forcing use of internal MEL computation.", __func__);
        activateInternalSoundDoseComputation();
        return false;
    }
    ndk::SpAIBinder soundDoseBinder;
    if (device->getSoundDoseInterface(module, &soundDoseBinder) != OK) {
        ALOGW("%s: HAL cannot provide sound dose interface for module %s, use internal MEL",
              __func__, module.c_str());
        activateInternalSoundDoseComputation();
        return false;
    }
    if (soundDoseBinder == nullptr) {
         ALOGW("%s: HAL doesn't implement a sound dose interface for module %s, use internal MEL",
              __func__, module.c_str());
        activateInternalSoundDoseComputation();
        return false;
    }
    std::shared_ptr<ISoundDose> soundDoseInterface = ISoundDose::fromBinder(soundDoseBinder);
    if (!mSoundDoseManager->setHalSoundDoseInterface(soundDoseInterface)) {
        ALOGW("%s: cannot activate HAL MEL reporting for module %s", __func__, module.c_str());
        activateInternalSoundDoseComputation();
        return false;
    }
    stopInternalMelComputation();
    return true;
}
void AudioFlinger::MelReporter::activateInternalSoundDoseComputation() {
    {
        std::lock_guard _l(mLock);
        if (!mUseHalSoundDoseInterface) {
            return;
        }
        mUseHalSoundDoseInterface = false;
    }
    mSoundDoseManager->setHalSoundDoseInterface(nullptr);
}
bool MelReporter::shouldComputeMelForDeviceType(audio_devices_t device) {
    if (!mSoundDoseManager->isCsdEnabled()) {
        ALOGV("%s csd is disabled", __func__);
        return false;
    }
    if (mSoundDoseManager->forceComputeCsdOnAllDevices()) {
        return true;
    }
    switch (device) {
        case AUDIO_DEVICE_OUT_WIRED_HEADSET:
        case AUDIO_DEVICE_OUT_WIRED_HEADPHONE:
        case AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES:
        case AUDIO_DEVICE_OUT_USB_HEADSET:
        case AUDIO_DEVICE_OUT_BLE_HEADSET:
        case AUDIO_DEVICE_OUT_BLE_BROADCAST:
            return true;
        default:
            return false;
    }
}
void AudioFlinger::MelReporter::updateMetadataForCsd(audio_io_handle_t streamHandle, const std::vector<playback_track_metadata_v7_t>& metadataVec) {
    if (!mSoundDoseManager->isCsdEnabled()) {
        ALOGV("%s csd is disabled", __func__);
        return;
    }
    std::lock_guard _laf(mAfMelReporterCallback->mutex());
    std::lock_guard _l(mLock);
    auto activeMelPatchId = activePatchStreamHandle_l(streamHandle);
    if (!activeMelPatchId) {
        ALOGV("%s stream handle %d does not have an active patch", __func__, streamHandle);
        return;
    }
    bool shouldActivateCsd = false;
    for (const auto& metadata : metadataVec) {
        if (metadata.base.usage == AUDIO_USAGE_GAME || metadata.base.usage == AUDIO_USAGE_MEDIA) {
            shouldActivateCsd = true;
        }
    }
    auto activeMelPatchIt = mActiveMelPatches.find(activeMelPatchId.value());
    if (activeMelPatchIt != mActiveMelPatches.end()) {
        if (shouldActivateCsd != activeMelPatchIt->second.csdActive) {
            if (activeMelPatchIt->second.csdActive) {
                ALOGV("%s should not compute CSD for stream handle %d", __func__, streamHandle);
                stopMelComputationForPatch_l(activeMelPatchIt->second);
            } else {
                ALOGV("%s should compute CSD for stream handle %d", __func__, streamHandle);
                startMelComputationForActivePatch_l(activeMelPatchIt->second);
            }
            activeMelPatchIt->second.csdActive = shouldActivateCsd;
        }
    }
}
void MelReporter::onCreateAudioPatch(audio_patch_handle_t handle, const IAfPatchPanel::Patch& patch) {
    if (!mSoundDoseManager->isCsdEnabled()) {
        ALOGV("%s csd is disabled", __func__);
        return;
    }
    ALOGV("%s: handle %d mHalHandle %d device sink %08x",
            __func__, handle, patch.mHalHandle,
            patch.mAudioPatch.num_sinks > 0 ? patch.mAudioPatch.sinks[0].ext.device.type : 0);
    if (patch.mAudioPatch.num_sources == 0
        || patch.mAudioPatch.sources[0].type != AUDIO_PORT_TYPE_MIX) {
        ALOGV("%s: patch does not contain any mix sources", __func__);
        return;
    }
    audio_io_handle_t streamHandle = patch.mAudioPatch.sources[0].ext.mix.handle;
    ActiveMelPatch newPatch;
    newPatch.streamHandle = streamHandle;
    for (size_t i = 0; i < patch.mAudioPatch.num_sinks; ++i) {
        if (patch.mAudioPatch.sinks[i].type == AUDIO_PORT_TYPE_DEVICE
            && shouldComputeMelForDeviceType(patch.mAudioPatch.sinks[i].ext.device.type)) {
            audio_port_handle_t deviceId = patch.mAudioPatch.sinks[i].id;
            newPatch.deviceHandles.push_back(deviceId);
            AudioDeviceTypeAddr adt{patch.mAudioPatch.sinks[i].ext.device.type,
                                    patch.mAudioPatch.sinks[i].ext.device.address};
            mSoundDoseManager->mapAddressToDeviceId(adt, deviceId);
        }
    }
    if (!newPatch.deviceHandles.empty()) {
        std::lock_guard _afl(mAudioFlinger.mLock);
        std::lock_guard _l(mLock);
        ALOGV("%s add patch handle %d to active devices", __func__, handle);
        startMelComputationForActivePatch_l(newPatch);
        newPatch.csdActive = true;
        mActiveMelPatches[handle] = newPatch;
    }
}
std::string MelReporter::dump() {
    std::lock_guard _l(mLock);
    std::string output("\nSound Dose:\n");
    output.append(mSoundDoseManager->dump());
    return output;
}
std::string MelReporter::dump() {
    std::lock_guard _l(mLock);
    std::string output("\nSound Dose:\n");
    output.append(mSoundDoseManager->dump());
    return output;
}
std::string MelReporter::dump() {
    std::lock_guard _l(mLock);
    std::string output("\nSound Dose:\n");
    output.append(mSoundDoseManager->dump());
    return output;
}
std::string MelReporter::dump() {
    std::lock_guard _l(mLock);
    std::string output("\nSound Dose:\n");
    output.append(mSoundDoseManager->dump());
    return output;
}
std::string MelReporter::dump() {
    std::lock_guard _l(mLock);
    std::string output("\nSound Dose:\n");
    output.append(mSoundDoseManager->dump());
    return output;
}
std::string MelReporter::dump() {
    std::lock_guard _l(mLock);
    std::string output("\nSound Dose:\n");
    output.append(mSoundDoseManager->dump());
    return output;
}
std::string MelReporter::dump() {
    std::lock_guard _l(mLock);
    std::string output("\nSound Dose:\n");
    output.append(mSoundDoseManager->dump());
    return output;
}
std::string MelReporter::dump() {
    std::lock_guard _l(mLock);
    std::string output("\nSound Dose:\n");
    output.append(mSoundDoseManager->dump());
    return output;
}
void MelReporter::onFirstRef() {
    mAfMelReporterCallback->getPatchCommandThread()->addListener(this);
    mSoundDoseManager = sp<SoundDoseManager>::make(sp<IMelReporterCallback>::fromExisting(this));
}
}
