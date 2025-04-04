       
#include "IAfThread.h"
#include "IAfTrack.h"
#include <datapath/AudioHwDevice.h>
#include <media/DeviceDescriptorBase.h>
#include <utils/Log.h>
#include <utils/RefBase.h>
#include <utils/Thread.h>
namespace android {
class IAfPatchPanel;
class PatchCommandThread;
class SoftwarePatch {
public:
    SoftwarePatch(
            const sp<const IAfPatchPanel>& patchPanel,
            audio_patch_handle_t patchHandle,
            audio_io_handle_t playbackThreadHandle,
            audio_io_handle_t recordThreadHandle)
        : mPatchPanel(patchPanel),
          mPatchHandle(patchHandle),
          mPlaybackThreadHandle(playbackThreadHandle),
          mRecordThreadHandle(recordThreadHandle) {}
    SoftwarePatch(const SoftwarePatch&) = default;
    status_t getLatencyMs_l(double* latencyMs) const REQUIRES(audio_utils::AudioFlinger_Mutex);
    audio_patch_handle_t getPatchHandle() const { return mPatchHandle; };
    audio_io_handle_t getPlaybackThreadHandle() const { return mPlaybackThreadHandle; };
    audio_io_handle_t getRecordThreadHandle() const { return mRecordThreadHandle; };
private:
    const sp<const IAfPatchPanel> mPatchPanel;
    const audio_patch_handle_t mPatchHandle;
    const audio_io_handle_t mPlaybackThreadHandle;
    const audio_io_handle_t mRecordThreadHandle;
};
class IAfPatchPanelCallback : public virtual RefBase {
public:
    virtual void closeThreadInternal_l(const sp<IAfPlaybackThread>& thread) REQUIRES(mutex()) = 0;
    virtual void closeThreadInternal_l(const sp<IAfRecordThread>& thread) REQUIRES(mutex()) = 0;
    virtual IAfPlaybackThread* primaryPlaybackThread_l() const REQUIRES(mutex()) = 0;
    virtual IAfPlaybackThread* checkPlaybackThread_l(audio_io_handle_t output) const
            REQUIRES(mutex()) = 0;
    virtual IAfRecordThread* checkRecordThread_l(audio_io_handle_t input) const
            REQUIRES(mutex()) = 0;
    virtual IAfMmapThread* checkMmapThread_l(audio_io_handle_t io) const REQUIRES(mutex()) = 0;
    virtual sp<IAfThreadBase> openInput_l(audio_module_handle_t module,
            audio_io_handle_t* input,
            audio_config_t* config,
            audio_devices_t device,
            const char* address,
            audio_source_t source,
            audio_input_flags_t flags,
            audio_devices_t outputDevice,
            const String8& outputDeviceAddress) REQUIRES(mutex()) = 0;
    virtual sp<IAfThreadBase> openOutput_l(audio_module_handle_t module,
            audio_io_handle_t* output,
            audio_config_t* halConfig,
            audio_config_base_t* mixerConfig,
            audio_devices_t deviceType,
            const String8& address,
            audio_output_flags_t flags) REQUIRES(mutex()) = 0;
    virtual audio_utils::mutex& mutex() const
            RETURN_CAPABILITY(audio_utils::AudioFlinger_Mutex) = 0;
    virtual const DefaultKeyedVector<audio_module_handle_t, AudioHwDevice*>&
            getAudioHwDevs_l() const REQUIRES(mutex()) = 0;
    virtual audio_unique_id_t nextUniqueId(audio_unique_id_use_t use) = 0;
    virtual const sp<PatchCommandThread>& getPatchCommandThread() = 0;
    virtual void updateDownStreamPatches_l(
            const struct audio_patch* patch, const std::set<audio_io_handle_t>& streams)
            REQUIRES(mutex()) = 0;
    virtual void updateOutDevicesForRecordThreads_l(const DeviceDescriptorBaseVector& devices)
            REQUIRES(mutex()) = 0;
};
class IAfPatchPanel : public virtual RefBase {
public:
    static sp<IAfPatchPanel> create(const sp<IAfPatchPanelCallback>& afPatchPanelCallback);
    template <typename ThreadType, typename TrackType>
    class Endpoint final {
    public:
        Endpoint() = default;
        Endpoint(const Endpoint&) = delete;
        Endpoint& operator=(const Endpoint& other) noexcept {
            mThread = other.mThread;
            mCloseThread = other.mCloseThread;
            mHandle = other.mHandle;
            mTrack = other.mTrack;
            return *this;
        }
        Endpoint(Endpoint&& other) noexcept { swap(other); }
        Endpoint& operator=(Endpoint&& other) noexcept {
            swap(other);
            return *this;
        }
        ~Endpoint() {
            ALOGE_IF(
                    mHandle != AUDIO_PATCH_HANDLE_NONE,
                    "A non empty Patch Endpoint leaked, handle %d", mHandle);
        }
        status_t checkTrack(TrackType* trackOrNull) const {
            if (trackOrNull == nullptr) return NO_MEMORY;
            return trackOrNull->initCheck();
        }
        audio_patch_handle_t handle() const { return mHandle; }
        sp<ThreadType> thread() const { return mThread; }
        sp<TrackType> track() const { return mTrack; }
        sp<const ThreadType> const_thread() const { return mThread; }
        sp<const TrackType> const_track() const { return mTrack; }
        void closeConnections_l(const sp<IAfPatchPanel>& panel)
                REQUIRES(audio_utils::AudioFlinger_Mutex)
                NO_THREAD_SAFETY_ANALYSIS
        {
            if (mHandle != AUDIO_PATCH_HANDLE_NONE) {
                panel->releaseAudioPatch_l(mHandle);
                mHandle = AUDIO_PATCH_HANDLE_NONE;
            }
            if (mThread != nullptr) {
                if (mTrack != nullptr) {
                    mThread->deletePatchTrack(mTrack);
                }
                if (mCloseThread) {
                    panel->closeThreadInternal_l(mThread);
                }
            }
        }
        audio_patch_handle_t* handlePtr() { return &mHandle; }
        void setThread(const sp<ThreadType>& thread, bool closeThread = true) {
            mThread = thread;
            mCloseThread = closeThread;
        }
        template <typename T>
        void setTrackAndPeer(const sp<TrackType>& track, const sp<T>& peer, bool holdReference) {
            mTrack = track;
            mThread->addPatchTrack(mTrack);
            mTrack->setPeerProxy(peer, holdReference);
            mClearPeerProxy = holdReference;
        }
        void clearTrackPeer() {
            if (mClearPeerProxy && mTrack) mTrack->clearPeerProxy();
        }
        void stopTrack() {
            if (mTrack) mTrack->stop();
        }
        void swap(Endpoint& other) noexcept {
            using std::swap;
            swap(mThread, other.mThread);
            swap(mCloseThread, other.mCloseThread);
            swap(mClearPeerProxy, other.mClearPeerProxy);
            swap(mHandle, other.mHandle);
            swap(mTrack, other.mTrack);
        }
        friend void swap(Endpoint& a, Endpoint& b) noexcept { a.swap(b); }
    private:
        sp<ThreadType> mThread;
        bool mCloseThread = true;
        bool mClearPeerProxy = true;
        audio_patch_handle_t mHandle = AUDIO_PATCH_HANDLE_NONE;
        sp<TrackType> mTrack;
    };
    class Patch final {
    public:
        Patch(const struct audio_patch& patch, bool endpointPatch)
            : mAudioPatch(patch), mIsEndpointPatch(endpointPatch) {}
        Patch() = default;
        ~Patch();
        Patch(const Patch& other) noexcept {
            mAudioPatch = other.mAudioPatch;
            mHalHandle = other.mHalHandle;
            mPlayback = other.mPlayback;
            mRecord = other.mRecord;
            mThread = other.mThread;
            mIsEndpointPatch = other.mIsEndpointPatch;
        }
        Patch(Patch&& other) noexcept { swap(other); }
        Patch& operator=(Patch&& other) noexcept {
            swap(other);
            return *this;
        }
        void swap(Patch& other) noexcept {
            using std::swap;
            swap(mAudioPatch, other.mAudioPatch);
            swap(mHalHandle, other.mHalHandle);
            swap(mPlayback, other.mPlayback);
            swap(mRecord, other.mRecord);
            swap(mThread, other.mThread);
            swap(mIsEndpointPatch, other.mIsEndpointPatch);
        }
        friend void swap(Patch& a, Patch& b) noexcept { a.swap(b); }
        status_t createConnections_l(const sp<IAfPatchPanel>& panel)
                REQUIRES(audio_utils::AudioFlinger_Mutex);
        void clearConnections_l(const sp<IAfPatchPanel>& panel)
                REQUIRES(audio_utils::AudioFlinger_Mutex);
        bool isSoftware() const {
            return mRecord.handle() != AUDIO_PATCH_HANDLE_NONE ||
                   mPlayback.handle() != AUDIO_PATCH_HANDLE_NONE;
        }
        void setThread(const sp<IAfThreadBase>& thread) { mThread = thread; }
        wp<IAfThreadBase> thread() const { return mThread; }
        status_t getLatencyMs(double* latencyMs) const;
        String8 dump(audio_patch_handle_t myHandle) const;
        struct audio_patch mAudioPatch;
        audio_patch_handle_t mHalHandle = AUDIO_PATCH_HANDLE_NONE;
        Endpoint<IAfPlaybackThread, IAfPatchTrack> mPlayback;
        Endpoint<IAfRecordThread, IAfPatchRecord> mRecord;
        wp<IAfThreadBase> mThread;
        bool mIsEndpointPatch;
    };
    virtual status_t listAudioPorts_l(unsigned int* num_ports, struct audio_port* ports)
            REQUIRES(audio_utils::AudioFlinger_Mutex) = 0;
    virtual status_t getAudioPort_l(struct audio_port_v7* port)
            REQUIRES(audio_utils::AudioFlinger_Mutex) = 0;
    virtual status_t createAudioPatch_l(
            const struct audio_patch* patch,
            audio_patch_handle_t* handle,
            bool endpointPatch = false)
            REQUIRES(audio_utils::AudioFlinger_Mutex) = 0;
    virtual status_t releaseAudioPatch_l(audio_patch_handle_t handle)
            REQUIRES(audio_utils::AudioFlinger_Mutex) = 0;
    virtual status_t listAudioPatches_l(unsigned int* num_patches, struct audio_patch* patches)
            REQUIRES(audio_utils::AudioFlinger_Mutex) = 0;
    virtual status_t getDownstreamSoftwarePatches(
            audio_io_handle_t stream, std::vector<SoftwarePatch>* patches) const = 0;
    virtual void notifyStreamOpened(
            AudioHwDevice* audioHwDevice, audio_io_handle_t stream, struct audio_patch* patch) = 0;
    virtual void notifyStreamClosed(audio_io_handle_t stream) = 0;
    virtual void dump(int fd) const = 0;
    virtual const std::map<audio_patch_handle_t, Patch>& patches_l() const
            REQUIRES(audio_utils::AudioFlinger_Mutex) = 0;
    virtual status_t getLatencyMs_l(audio_patch_handle_t patchHandle, double* latencyMs) const
            REQUIRES(audio_utils::AudioFlinger_Mutex) = 0;
    virtual void closeThreadInternal_l(const sp<IAfThreadBase>& thread) const
            REQUIRES(audio_utils::AudioFlinger_Mutex) = 0;
};
}
