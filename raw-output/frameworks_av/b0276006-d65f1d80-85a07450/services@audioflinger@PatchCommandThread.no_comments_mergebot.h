       
#include "IAfPatchPanel.h"
#include <utils/RefBase.h>
#include <utils/Thread.h>
#include <deque>
#include <mutex>
namespace android {
class Command;
class PatchCommandThread : public Thread {
public:
    enum {
        CREATE_AUDIO_PATCH,
        RELEASE_AUDIO_PATCH,
        UPDATE_AUDIO_PATCH,
    };
    class PatchCommandListener : public virtual RefBase {
    public:
        virtual void onCreateAudioPatch(audio_patch_handle_t handle,
                                        const IAfPatchPanel::Patch& patch) = 0;
        virtual void onReleaseAudioPatch(audio_patch_handle_t handle) = 0;
        virtual void onUpdateAudioPatch(audio_patch_handle_t oldHandle,
                                        audio_patch_handle_t newHandle,
                                        const IAfPatchPanel::Patch& patch) = 0;
    };
    PatchCommandThread() : Thread(false ) {}
    ~PatchCommandThread() override;
    void addListener(const sp<PatchCommandListener>& listener)
    void createAudioPatch(audio_patch_handle_t handle, const IAfPatchPanel::Patch& patch)
    void releaseAudioPatch(audio_patch_handle_t handle)
    void onFirstRef() override;
    bool threadLoop() override;
    void exit();
    void createAudioPatchCommand(audio_patch_handle_t handle,
            const IAfPatchPanel::Patch& patch)
    void releaseAudioPatchCommand(audio_patch_handle_t handle)
private:
    class CommandData: public RefBase {};
    class Command: public RefBase {
    public:
        Command() = default;
        Command(int command, const sp<CommandData>& data)
            : mCommand(command), mData(data) {}
        const int mCommand = -1;
        const sp<CommandData> mData;
    };
    class CommandData: public RefBase {};
    class CreateAudioPatchData : public CommandData {
    public:
        CreateAudioPatchData(audio_patch_handle_t handle, const IAfPatchPanel::Patch& patch)
            : mHandle(handle), mPatch(patch) {}
        const audio_patch_handle_t mHandle;
        const IAfPatchPanel::Patch mPatch;
    };
    class ReleaseAudioPatchData : public CommandData {
    public:
        explicit ReleaseAudioPatchData(audio_patch_handle_t handle)
            : mHandle(handle) {}
        audio_patch_handle_t mHandle;
    };
    class UpdateAudioPatchData : public CommandData {
    public:
        UpdateAudioPatchData(audio_patch_handle_t oldHandle,
                             audio_patch_handle_t newHandle,
                             const IAfPatchPanel::Patch& patch)
            : mOldHandle(oldHandle), mNewHandle(newHandle), mPatch(patch) {}
        const audio_patch_handle_t mOldHandle;
        const audio_patch_handle_t mNewHandle;
        const IAfPatchPanel::Patch mPatch;
    };
    void sendCommand(const sp<Command>& command)
    audio_utils::mutex& mutex() const { return mMutex; }
    audio_utils::mutex& listenerMutex() const { return mListenerMutex; }
    mutable audio_utils::mutex mMutex;
    audio_utils::condition_variable mWaitWorkCV;
    std::vector<wp<PatchCommandListener>> mListeners GUARDED_BY(listenerMutex());
audio_utils::mutex& mutex() const RETURN_CAPABILITY(audio_utils::PatchCommandThread_ListenerMutex){
            return mListenerMutex;
            }
public:
void updateAudioPatchCommand(audio_patch_handle_t oldHandle, audio_patch_handle_t newHandle,
            const IAfPatchPanel::Patch& patch)void updateAudioPatch(audio_patch_handle_t oldHandle, audio_patch_handle_t newHandle,
            const IAfPatchPanel::Patch& patch)private:
    mutable audio_utils::mutex mListenerMutex;
    std::vector<wp<PatchCommandListener>> mListeners GUARDED_BY(listenerMutex());
};
}
