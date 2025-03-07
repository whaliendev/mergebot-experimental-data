#ifndef DRM_SESSION_MANAGER_H_
#define DRM_SESSION_MANAGER_H_ 
#include <aidl/android/media/IResourceManagerClient.h> #include <aidl/android/media/IResourceManagerService.h> #include <android/binder/IBinder.h> #include <media/IResourceManagerService.h>
#include <media/stagefright/foundation/ABase.h>
#include <utils/RefBase.h>
#include <utils/KeyedVector.h>
#include <utils/threads.h>
#include <utils/Vector.h>
#include <map> #include <memory> #include <utility> #include <vector>
namespace android {
class DrmSessionManagerTest;
class IResourceManagerClient; using aidl::android::media::IResourceManagerService;
bool isEqualSessionId(const Vector<uint8_t> &sessionId1, const Vector<uint8_t> &sessionId2);
struct SessionInfo {
    pid_t pid;
    uid_t uid;
    int64_t clientId;
};
typedef std::map<std::vector<uint8_t>, SessionInfo> SessionInfoMap;
struct DrmSessionManager : public IBinder::DeathRecipient {
    static sp<DrmSessionManager> Instance();
    DrmSessionManager();
explicit DrmSessionManager(const std::shared_ptr<IResourceManagerService> &service);
void addSession(int pid, const std::shared_ptr<IResourceManagerClient>& drm, const Vector<uint8_t>& sessionId);
    void useSession(const Vector<uint8_t>& sessionId);
    void removeSession(const Vector<uint8_t>& sessionId);
    bool reclaimSession(int callingPid);
protected:
    virtual ~DrmSessionManager();
private:
    void init();
std::shared_ptr<IResourceManagerService> mService;
    mutable Mutex mLock;
bool mInitialized; ::ndk::ScopedAIBinder_DeathRecipient mDeathRecipient;
    DISALLOW_EVIL_CONSTRUCTORS(DrmSessionManager);
};
}
#endif
