       
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
            EXCLUDES_PatchCommandThread_ListenerMutex;
    void createAudioPatch(audio_patch_handle_t handle, const IAfPatchPanel::Patch& patch)
            EXCLUDES_PatchCommandThread_Mutex;
    void releaseAudioPatch(audio_patch_handle_t handle) EXCLUDES_PatchCommandThread_Mutex;
    void updateAudioPatch(audio_patch_handle_t oldHandle, audio_patch_handle_t newHandle,
            const IAfPatchPanel::Patch& patch) EXCLUDES_PatchCommandThread_Mutex;
    void onFirstRef() override;
    bool threadLoop() override;
    void exit();
    void createAudioPatchCommand(audio_patch_handle_t handle,
            const IAfPatchPanel::Patch& patch) EXCLUDES_PatchCommandThread_Mutex;
    void releaseAudioPatchCommand(audio_patch_handle_t handle) EXCLUDES_PatchCommandThread_Mutex;
    void updateAudioPatchCommand(audio_patch_handle_t oldHandle, audio_patch_handle_t newHandle,
            const IAfPatchPanel::Patch& patch) EXCLUDES_PatchCommandThread_Mutex;
private:
    class CommandData;
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
<<<<<<< HEAD
        const audio_patch_handle_t mOldHandle;
        const audio_patch_handle_t mNewHandle;
        const IAfPatchPanel::Patch mPatch;
    };
||||||| 85a074502c
    std::string mThreadName;
    std::mutex mLock;
    std::condition_variable mWaitWorkCV;
    std::deque<sp<Command>> mCommands GUARDED_BY(mLock);
=======
    audio_utils::mutex& mutex() const { return mMutex; }
    audio_utils::mutex& listenerMutex() const { return mListenerMutex; }
>>>>>>> d65f1d80
<<<<<<< HEAD
    void sendCommand(const sp<Command>& command) EXCLUDES_PatchCommandThread_Mutex;
    audio_utils::mutex& mutex() const RETURN_CAPABILITY(audio_utils::PatchCommandThread_Mutex) {
        return mMutex;
    }
    audio_utils::mutex& listenerMutex() const
            RETURN_CAPABILITY(audio_utils::PatchCommandThread_ListenerMutex) {
        return mListenerMutex;
    }
    mutable audio_utils::mutex mMutex;
    audio_utils::condition_variable mWaitWorkCV;
    std::deque<sp<Command>> mCommands GUARDED_BY(mutex());
    mutable audio_utils::mutex mListenerMutex;
    std::vector<wp<PatchCommandListener>> mListeners GUARDED_BY(listenerMutex());
||||||| 85a074502c
    std::mutex mListenerLock;
    std::vector<wp<PatchCommandListener>> mListeners GUARDED_BY(mListenerLock);
=======
    std::string mThreadName;
    mutable audio_utils::mutex mMutex;
    audio_utils::condition_variable mWaitWorkCV;
    std::deque<sp<Command>> mCommands GUARDED_BY(mutex());
    mutable audio_utils::mutex mListenerMutex;
    std::vector<wp<PatchCommandListener>> mListeners GUARDED_BY(listenerMutex());
>>>>>>> d65f1d80
};
}
