<<<<<<< HEAD
       
#include "IAfPatchPanel.h"
#include "PatchCommandThread.h"
#include <sounddose/SoundDoseManager.h>
||||||| 7cfbe80423
#ifndef INCLUDING_FROM_AUDIOFLINGER_H
    #error This header file should only be included from AudioFlinger.h
#endif
=======
       
>>>>>>> 8545964e
#include <mutex>
#include <unordered_map>
namespace android {
constexpr static int kMaxTimestampDeltaInSec = 120;
class IAfMelReporterCallback : public virtual RefBase {
public:
    virtual Mutex& mutex() const = 0;
    virtual const sp<PatchCommandThread>& getPatchCommandThread() = 0;
    virtual sp<IAfThreadBase> checkOutputThread_l(audio_io_handle_t ioHandle) const = 0;
};
class MelReporter : public PatchCommandThread::PatchCommandListener,
                    public IMelReporterCallback {
public:
    explicit MelReporter(const sp<IAfMelReporterCallback>& afMelReporterCallback)
        : mAfMelReporterCallback(afMelReporterCallback) {}
    void onFirstRef() override;
    bool activateHalSoundDoseComputation(const std::string& module,
                                         const sp<DeviceHalInterface>& device);
    void activateInternalSoundDoseComputation();
    sp<media::ISoundDose> getSoundDoseInterface(const sp<media::ISoundDoseCallback>& callback);
    std::string dump();
    void stopMelComputationForDeviceId(audio_port_handle_t deviceId) override;
    void startMelComputationForDeviceId(audio_port_handle_t deviceId) override;
    void onCreateAudioPatch(audio_patch_handle_t handle,
        const IAfPatchPanel::Patch& patch) final;
    void onReleaseAudioPatch(audio_patch_handle_t handle) final;
    void updateMetadataForCsd(audio_io_handle_t streamHandle,
                              const std::vector<playback_track_metadata_v7_t>& metadataVec);
private:
    struct ActiveMelPatch {
        audio_io_handle_t streamHandle{AUDIO_IO_HANDLE_NONE};
        std::vector<std::pair<audio_port_handle_t,bool>> deviceStates;
        bool csdActive;
    };
    void stopInternalMelComputation();
    void stopMelComputationForPatch_l(const ActiveMelPatch& patch) REQUIRES(mLock);
    void startMelComputationForActivePatch_l(const ActiveMelPatch& patch) REQUIRES(mLock);
    std::optional<audio_patch_handle_t>
    activePatchStreamHandle_l(audio_io_handle_t streamHandle) REQUIRES(mLock);
    bool useHalSoundDoseInterface_l() REQUIRES(mLock);
    const sp<IAfMelReporterCallback> mAfMelReporterCallback;
    sp<SoundDoseManager> mSoundDoseManager;
    std::mutex mLock;
    std::unordered_map<audio_patch_handle_t, ActiveMelPatch> mActiveMelPatches GUARDED_BY(mLock);
    std::unordered_map<audio_port_handle_t, int> mActiveDevices GUARDED_BY(mLock);
    bool mUseHalSoundDoseInterface GUARDED_BY(mLock) = false;
};
}
