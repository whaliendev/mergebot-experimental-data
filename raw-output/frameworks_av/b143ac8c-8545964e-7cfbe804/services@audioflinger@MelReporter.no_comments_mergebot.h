       
#ifndef INCLUDING_FROM_AUDIOFLINGER_H
    #error This header file should only be included from AudioFlinger.h
#endif
#include "IAfPatchPanel.h"
#include "PatchCommandThread.h"
#include <sounddose/SoundDoseManager.h>
#include <mutex>
#include <unordered_map>
namespace android {
constexpr static int kMaxTimestampDeltaInSec = 120;
class MelReporter : public PatchCommandThread::PatchCommandListener {
public:
    explicitMelReporter(AudioFlinger& audioFlinger): mAudioFlinger(audioFlinger), mSoundDoseManager(sp<SoundDoseManager>::make()) {}
    void onFirstRef() override;
    bool activateHalSoundDoseComputation(const std::string& module,
                                         const sp<DeviceHalInterface>& device);
    void activateInternalSoundDoseComputation();
    sp<media::ISoundDose> getSoundDoseInterface(const sp<media::ISoundDoseCallback>& callback);
    std::string dump();
    void onCreateAudioPatch(audio_patch_handle_t handle,
        const IAfPatchPanel::Patch& patch) final;
    void onReleaseAudioPatch(audio_patch_handle_t handle) final;
    void updateMetadataForCsd(audio_io_handle_t streamHandle,
                              const std::vector<playback_track_metadata_v7_t>& metadataVec);
private:
    struct ActiveMelPatch {
        audio_io_handle_t streamHandle{AUDIO_IO_HANDLE_NONE};
        std::vector<audio_port_handle_t> deviceHandles;
        bool csdActive;
    };
    bool shouldComputeMelForDeviceType(audio_devices_t device);
    void stopInternalMelComputation();
void stopMelComputationForPatch_l(const ActiveMelPatch& patch) REQUIRES(mLock);
void startMelComputationForActivePatch_l(const ActiveMelPatch& patch) REQUIRES(mLock);
std::optional<audio_patch_handle_t>
    activePatchStreamHandle_l(audio_io_handle_t streamHandle) REQUIRES(mLock);
bool useHalSoundDoseInterface_l() REQUIRES(mLock);
AudioFlinger& mAudioFlinger;
    sp<SoundDoseManager> mSoundDoseManager;
    std::mutex mLock;
std::unordered_map<audio_patch_handle_t, ActiveMelPatch> mActiveMelPatches GUARDED_BY(mLock);
std::unordered_map<audio_port_handle_t, int> mActiveDevices GUARDED_BY(mLock);
bool mUseHalSoundDoseInterface GUARDED_BY(mLock) = false;
};
}
