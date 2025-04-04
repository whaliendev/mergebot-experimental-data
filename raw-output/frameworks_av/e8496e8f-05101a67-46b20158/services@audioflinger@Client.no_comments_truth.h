       
#include <afutils/AllocatorFactory.h>
#include <audio_utils/mutex.h>
#include <android-base/macros.h>
#include <utils/RefBase.h>
namespace android {
class IAfPlaybackThread;
class IAfClientCallback : public virtual RefBase {
public:
    virtual audio_utils::mutex& clientMutex() const
            RETURN_CAPABILITY(audio_utils::AudioFlinger_ClientMutex) = 0;
    virtual void removeClient_l(pid_t pid) REQUIRES(clientMutex()) = 0;
    virtual void removeNotificationClient(pid_t pid) EXCLUDES_AudioFlinger_Mutex = 0;
    virtual status_t moveAuxEffectToIo(
            int effectId,
            const sp<IAfPlaybackThread>& dstThread,
            sp<IAfPlaybackThread>* srcThread) EXCLUDES_AudioFlinger_Mutex = 0;
};
class Client : public RefBase {
public:
    Client(const sp<IAfClientCallback>& audioFlinger, pid_t pid);
    ~Client() override;
    AllocatorFactory::ClientAllocator& allocator();
    pid_t pid() const { return mPid; }
    const auto& afClientCallback() const { return mAfClientCallback; }
private:
    DISALLOW_COPY_AND_ASSIGN(Client);
    const sp<IAfClientCallback> mAfClientCallback;
    const pid_t mPid;
    AllocatorFactory::ClientAllocator mClientAllocator;
};
}
