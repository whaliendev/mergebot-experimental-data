#ifndef ANDROID_MEDIA_RESOURCEMANAGERSERVICE_H
#define ANDROID_MEDIA_RESOURCEMANAGERSERVICE_H 
#include <aidl/android/media/BnResourceManagerService.h>
#include <arpa/inet.h>
#include <media/MediaResource.h>
#include <utils/Errors.h>
#include <utils/KeyedVector.h>
#include <utils/String8.h>
#include <utils/threads.h>
#include <utils/Vector.h>
namespace android {
class DeathNotifier;
class ResourceManagerService;
class ServiceLog;
struct ProcessInfoInterface;
using Status = ::ndk::ScopedAStatus;
using ::aidl::android::media::IResourceManagerClient;
using ::aidl::android::media::BnResourceManagerService;
using ::aidl::android::media::MediaResourceParcel;
using ::aidl::android::media::MediaResourcePolicyParcel;
typedef std::map<std::tuple<
        MediaResource::Type, MediaResource::SubType, std::vector<int8_t>>,
        MediaResourceParcel> ResourceList;
struct ResourceInfo {
    int64_t clientId;
    uid_t uid;
    std::shared_ptr<IResourceManagerClient> client;
    sp<DeathNotifier> deathNotifier;
    ResourceList resources;
};
typedef KeyedVector<int64_t, ResourceInfo> ResourceInfos;
typedef KeyedVector<int, ResourceInfos> PidResourceInfosMap;
class DeathNotifier : public RefBase {
public:
    DeathNotifier(const std::shared_ptr<ResourceManagerService> &service,
            int pid, int64_t clientId);
    ~DeathNotifier() {}
    static void BinderDiedCallback(void* cookie);
    void binderDied();
private:
    std::weak_ptr<ResourceManagerService> mService;
    int mPid;
    int64_t mClientId;
};
class ResourceManagerService : public BnResourceManagerService {
public:
    struct SystemCallbackInterface : public RefBase {
        virtual void noteStartVideo(int uid) = 0;
        virtual void noteStopVideo(int uid) = 0;
        virtual void noteResetVideo() = 0;
        virtual bool requestCpusetBoost(bool enable) = 0;
    };
    static char const *getServiceName() { return "media.resource_manager"; }
    static void instantiate();
    virtual inline binder_status_t dump(
            int , const char** , uint32_t );
    ResourceManagerService();
    explicit ResourceManagerService(
            const sp<ProcessInfoInterface> &processInfo,
            const sp<SystemCallbackInterface> &systemResource);
    virtual ~ResourceManagerService();
    Status config(const std::vector<MediaResourcePolicyParcel>& policies) override;
    Status addResource(
            int32_t pid,
            int32_t uid,
            int64_t clientId,
            const std::shared_ptr<IResourceManagerClient>& client,
            const std::vector<MediaResourceParcel>& resources) override;
    Status removeResource(
            int32_t pid,
            int64_t clientId,
            const std::vector<MediaResourceParcel>& resources) override;
    Status removeClient(int32_t pid, int64_t clientId) override;
    Status reclaimResource(
            int32_t callingPid,
            const std::vector<MediaResourceParcel>& resources,
            bool* _aidl_return) override;
    Status removeResource(int pid, int64_t clientId, bool checkValid);
private:
    friend class ResourceManagerServiceTest;
    bool getAllClients_l(int callingPid, MediaResource::Type type,
            Vector<std::shared_ptr<IResourceManagerClient>> *clients);
    bool getLowestPriorityBiggestClient_l(int callingPid, MediaResource::Type type,
            std::shared_ptr<IResourceManagerClient> *client);
    bool getLowestPriorityPid_l(MediaResource::Type type, int *pid, int *priority);
    bool getBiggestClient_l(int pid, MediaResource::Type type,
            std::shared_ptr<IResourceManagerClient> *client);
    bool isCallingPriorityHigher_l(int callingPid, int pid);
    void getClientForResource_l(int callingPid, const MediaResourceParcel *res,
            Vector<std::shared_ptr<IResourceManagerClient>> *clients);
    void onFirstAdded(const MediaResourceParcel& res, const ResourceInfo& clientInfo);
    void onLastRemoved(const MediaResourceParcel& res, const ResourceInfo& clientInfo);
    void mergeResources(MediaResourceParcel& r1, const MediaResourceParcel& r2);
    mutable Mutex mLock;
    sp<ProcessInfoInterface> mProcessInfo;
    sp<SystemCallbackInterface> mSystemCB;
    sp<ServiceLog> mServiceLog;
    PidResourceInfosMap mMap;
    bool mSupportsMultipleSecureCodecs;
    bool mSupportsSecureWithNonSecureCodec;
    int32_t mCpuBoostCount;
    ::ndk::ScopedAIBinder_DeathRecipient mDeathRecipient;
};
}
#endif
