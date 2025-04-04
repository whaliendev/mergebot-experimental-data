       
#include "IAfPatchPanel.h"
#include "PatchCommandThread.h"
#include <audio_utils/mutex.h>
#include <sounddose/SoundDoseManager.h>
#include <unordered_map>
namespace android {
constexpr static int kMaxTimestampDeltaInSec = 120;
class IAfMelReporterCallback : public virtual RefBase {
public:
    virtual audio_utils::mutex& mutex() const
            RETURN_CAPABILITY(audio_utils::AudioFlinger_Mutex) = 0;
    virtual const sp<PatchCommandThread>& getPatchCommandThread() = 0;
    virtual sp<IAfThreadBase> checkOutputThread_l(audio_io_handle_t ioHandle) const
            REQUIRES(mutex()) = 0;
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
    audio_utils::mutex& mutex() const { return mMutex; }
    void stopMelComputationForPatch_l(const ActiveMelPatch& patch) REQUIRES(mutex());
    void startMelComputationForActivePatch_l(const ActiveMelPatch& patch) REQUIRES(mutex());
    std::optional<audio_patch_handle_t>
    activePatchStreamHandle_l(audio_io_handle_t streamHandle) REQUIRES(mutex());
    bool useHalSoundDoseInterface_l() REQUIRES(mutex());
    const sp<IAfMelReporterCallback> mAfMelReporterCallback;
                sp<SoundDoseManager> mSoundDoseManager;
    mutable audio_utils::mutex mMutex;
    std::unordered_map<audio_patch_handle_t, ActiveMelPatch> mActiveMelPatches
            GUARDED_BY(mutex());
    std::unordered_map<audio_port_handle_t, int> mActiveDevices GUARDED_BY(mutex());
    bool mUseHalSoundDoseInterface GUARDED_BY(mutex()) = false;
};
}
