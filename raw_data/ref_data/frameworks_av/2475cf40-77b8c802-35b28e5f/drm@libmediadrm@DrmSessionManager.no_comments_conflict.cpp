#define LOG_TAG "DrmSessionManager"
#include <utils/Log.h>
<<<<<<< HEAD
#include <aidl/android/media/IResourceManagerClient.h>
#include <aidl/android/media/IResourceManagerService.h>
#include <aidl/android/media/MediaResourceParcel.h>
#include <android/binder_ibinder.h>
#include <android/binder_manager.h>
#include <cutils/properties.h>
#include <mediadrm/DrmUtils.h>
||||||| 35b28e5f80
#include <binder/IPCThreadState.h>
#include <binder/IProcessInfoService.h>
#include <binder/IServiceManager.h>
#include <media/stagefright/ProcessInfo.h>
#include <mediadrm/DrmSessionClientInterface.h>
=======
#include <binder/IPCThreadState.h>
#include <binder/IProcessInfoService.h>
#include <binder/IServiceManager.h>
#include <cutils/properties.h>
#include <media/IResourceManagerClient.h>
#include <media/MediaResource.h>
>>>>>>> 77b8c802
#include <mediadrm/DrmSessionManager.h>
#include <unistd.h>
#include <utils/String8.h>
<<<<<<< HEAD
#include <vector>
||||||| 35b28e5f80
=======
#include <vector>
#include "ResourceManagerService.h"
>>>>>>> 77b8c802
namespace android {
using aidl::android::media::MediaResourceParcel;
namespace {
void ResourceManagerServiceDied(void* cookie) {
    auto thiz = static_cast<DrmSessionManager*>(cookie);
    thiz->binderDied();
}
}
using ::ndk::ScopedAStatus;
static String8 GetSessionIdString(const Vector<uint8_t> &sessionId) {
    String8 sessionIdStr;
    for (size_t i = 0; i < sessionId.size(); ++i) {
        sessionIdStr.appendFormat("%u ", sessionId[i]);
    }
    return sessionIdStr;
}
<<<<<<< HEAD
template <typename Byte = uint8_t>
static std::vector<Byte> toStdVec(const Vector<uint8_t> &vector) {
    auto v = reinterpret_cast<const Byte *>(vector.array());
    std::vector<Byte> vec(v, v + vector.size());
    return vec;
}
static std::vector<MediaResourceParcel> toResourceVec(
        const Vector<uint8_t> &sessionId, int64_t value) {
    using Type = aidl::android::media::MediaResourceType;
    using SubType = aidl::android::media::MediaResourceSubType;
    std::vector<MediaResourceParcel> resources;
    MediaResourceParcel resource{
            Type::kDrmSession, SubType::kUnspecifiedSubType,
            toStdVec<int8_t>(sessionId), value};
    resources.push_back(resource);
    return resources;
}
static std::shared_ptr<IResourceManagerService> getResourceManagerService() {
    ::ndk::SpAIBinder binder(AServiceManager_getService("media.resource_manager"));
    return IResourceManagerService::fromBinder(binder);
}
||||||| 35b28e5f80
=======
static std::vector<uint8_t> toStdVec(const Vector<uint8_t> &vector) {
    const uint8_t *v = vector.array();
    std::vector<uint8_t> vec(v, v + vector.size());
    return vec;
}
static uint64_t toClientId(const sp<IResourceManagerClient>& drm) {
    return reinterpret_cast<int64_t>(drm.get());
}
static Vector<MediaResource> toResourceVec(const Vector<uint8_t> &sessionId) {
    Vector<MediaResource> resources;
    resources.push_back(MediaResource(MediaResource::kDrmSession, toStdVec(sessionId), UINT64_MAX));
    return resources;
}
static sp<IResourceManagerService> getResourceManagerService() {
    if (property_get_bool("persist.device_config.media_native.mediadrmserver", 1)) {
        return new ResourceManagerService();
    }
    sp<IServiceManager> sm = defaultServiceManager();
    if (sm == NULL) {
        return NULL;
    }
    sp<IBinder> binder = sm->getService(String16("media.resource_manager"));
    return interface_cast<IResourceManagerService>(binder);
}
>>>>>>> 77b8c802
bool isEqualSessionId(const Vector<uint8_t> &sessionId1, const Vector<uint8_t> &sessionId2) {
    if (sessionId1.size() != sessionId2.size()) {
        return false;
    }
    for (size_t i = 0; i < sessionId1.size(); ++i) {
        if (sessionId1[i] != sessionId2[i]) {
            return false;
        }
    }
    return true;
}
sp<DrmSessionManager> DrmSessionManager::Instance() {
<<<<<<< HEAD
    auto drmSessionManager = new DrmSessionManager();
    drmSessionManager->init();
||||||| 35b28e5f80
    static sp<DrmSessionManager> drmSessionManager = new DrmSessionManager();
=======
    static sp<DrmSessionManager> drmSessionManager = new DrmSessionManager();
    drmSessionManager->init();
>>>>>>> 77b8c802
    return drmSessionManager;
}
DrmSessionManager::DrmSessionManager()
    : DrmSessionManager(getResourceManagerService()) {
}
<<<<<<< HEAD
DrmSessionManager::DrmSessionManager(const std::shared_ptr<IResourceManagerService> &service)
    : mService(service),
      mInitialized(false),
      mDeathRecipient(AIBinder_DeathRecipient_new(ResourceManagerServiceDied)) {
    if (mService == NULL) {
        ALOGE("Failed to init ResourceManagerService");
    }
}
||||||| 35b28e5f80
DrmSessionManager::DrmSessionManager(sp<ProcessInfoInterface> processInfo)
    : mProcessInfo(processInfo),
      mTime(0) {}
=======
DrmSessionManager::DrmSessionManager(const sp<IResourceManagerService> &service)
    : mService(service),
      mInitialized(false) {
    if (mService == NULL) {
        ALOGE("Failed to init ResourceManagerService");
    }
}
>>>>>>> 77b8c802
<<<<<<< HEAD
DrmSessionManager::~DrmSessionManager() {
    if (mService != NULL) {
        AIBinder_unlinkToDeath(mService->asBinder().get(), mDeathRecipient.get(), this);
    }
}
||||||| 35b28e5f80
DrmSessionManager::~DrmSessionManager() {}
=======
DrmSessionManager::~DrmSessionManager() {
    if (mService != NULL) {
        IInterface::asBinder(mService)->unlinkToDeath(this);
    }
}
>>>>>>> 77b8c802
<<<<<<< HEAD
void DrmSessionManager::init() {
    Mutex::Autolock lock(mLock);
    if (mInitialized) {
        return;
    }
    mInitialized = true;
    if (mService != NULL) {
        AIBinder_linkToDeath(mService->asBinder().get(), mDeathRecipient.get(), this);
    }
}
void DrmSessionManager::addSession(int pid,
        const std::shared_ptr<IResourceManagerClient>& drm, const Vector<uint8_t> &sessionId) {
    uid_t uid = AIBinder_getCallingUid();
    ALOGV("addSession(pid %d, uid %d, drm %p, sessionId %s)", pid, uid, drm.get(),
||||||| 35b28e5f80
void DrmSessionManager::addSession(
        int pid, const sp<DrmSessionClientInterface>& drm, const Vector<uint8_t> &sessionId) {
    ALOGV("addSession(pid %d, drm %p, sessionId %s)", pid, drm.get(),
=======
void DrmSessionManager::init() {
    Mutex::Autolock lock(mLock);
    if (mInitialized) {
        return;
    }
    mInitialized = true;
    if (mService != NULL) {
        IInterface::asBinder(mService)->linkToDeath(this);
    }
}
void DrmSessionManager::addSession(int pid,
        const sp<IResourceManagerClient>& drm, const Vector<uint8_t> &sessionId) {
    uid_t uid = IPCThreadState::self()->getCallingUid();
    ALOGV("addSession(pid %d, uid %d, drm %p, sessionId %s)", pid, uid, drm.get(),
>>>>>>> 77b8c802
            GetSessionIdString(sessionId).string());
    Mutex::Autolock lock(mLock);
    if (mService == NULL) {
        return;
    }
<<<<<<< HEAD
    static int64_t clientId = 0;
    mSessionMap[toStdVec(sessionId)] = (SessionInfo){pid, uid, clientId};
    mService->addResource(pid, uid, clientId++, drm, toResourceVec(sessionId, INT64_MAX));
||||||| 35b28e5f80
=======
    int64_t clientId = toClientId(drm);
    mSessionMap[toStdVec(sessionId)] = (SessionInfo){pid, uid, clientId};
    mService->addResource(pid, uid, clientId, drm, toResourceVec(sessionId));
>>>>>>> 77b8c802
}
void DrmSessionManager::useSession(const Vector<uint8_t> &sessionId) {
    ALOGV("useSession(%s)", GetSessionIdString(sessionId).string());
    Mutex::Autolock lock(mLock);
    auto it = mSessionMap.find(toStdVec(sessionId));
    if (mService == NULL || it == mSessionMap.end()) {
        return;
    }
<<<<<<< HEAD
    auto info = it->second;
    mService->addResource(info.pid, info.uid, info.clientId, NULL, toResourceVec(sessionId, -1));
||||||| 35b28e5f80
=======
    auto info = it->second;
    mService->addResource(info.pid, info.uid, info.clientId, NULL, toResourceVec(sessionId));
>>>>>>> 77b8c802
}
void DrmSessionManager::removeSession(const Vector<uint8_t> &sessionId) {
    ALOGV("removeSession(%s)", GetSessionIdString(sessionId).string());
    Mutex::Autolock lock(mLock);
    auto it = mSessionMap.find(toStdVec(sessionId));
    if (mService == NULL || it == mSessionMap.end()) {
        return;
    }
<<<<<<< HEAD
    auto info = it->second;
    mService->removeResource(info.pid, info.clientId, toResourceVec(sessionId, INT64_MAX));
    mSessionMap.erase(it);
||||||| 35b28e5f80
void DrmSessionManager::removeDrm(const sp<DrmSessionClientInterface>& drm) {
    ALOGV("removeDrm(%p)", drm.get());
    Mutex::Autolock lock(mLock);
    bool found = false;
    for (size_t i = 0; i < mSessionMap.size(); ++i) {
        SessionInfos& infos = mSessionMap.editValueAt(i);
        for (size_t j = 0; j < infos.size();) {
            if (infos[j].drm == drm) {
                ALOGV("removed session (%s)", GetSessionIdString(infos[j].sessionId).string());
                j = infos.removeAt(j);
                found = true;
            } else {
                ++j;
            }
        }
        if (found) {
            break;
        }
    }
=======
    auto info = it->second;
    mService->removeResource(info.pid, info.clientId, toResourceVec(sessionId));
    mSessionMap.erase(it);
>>>>>>> 77b8c802
}
bool DrmSessionManager::reclaimSession(int callingPid) {
    ALOGV("reclaimSession(%d)", callingPid);
<<<<<<< HEAD
    mLock.lock();
    std::shared_ptr<IResourceManagerService> service(mService);
    mLock.unlock();
||||||| 35b28e5f80
    sp<DrmSessionClientInterface> drm;
    Vector<uint8_t> sessionId;
    int lowestPriorityPid;
    int lowestPriority;
    {
        Mutex::Autolock lock(mLock);
        int callingPriority;
        if (!mProcessInfo->getPriority(callingPid, &callingPriority)) {
            return false;
        }
        if (!getLowestPriority_l(&lowestPriorityPid, &lowestPriority)) {
            return false;
        }
        if (lowestPriority <= callingPriority) {
            return false;
        }
=======
    mLock.lock();
    sp<IResourceManagerService> service(mService);
    mLock.unlock();
>>>>>>> 77b8c802
    if (service == NULL) {
        return false;
    }
<<<<<<< HEAD
    Vector<uint8_t> dummy;
    bool success;
    ScopedAStatus status = service->reclaimResource(callingPid, toResourceVec(dummy, INT64_MAX), &success);
    return status.isOk() && success;
||||||| 35b28e5f80
    ALOGV("reclaim session(%s) opened by pid %d",
            GetSessionIdString(sessionId).string(), lowestPriorityPid);
    return drm->reclaimSession(sessionId);
=======
    Vector<uint8_t> dummy;
    return service->reclaimResource(callingPid, toResourceVec(dummy));
>>>>>>> 77b8c802
}
size_t DrmSessionManager::getSessionCount() const {
    Mutex::Autolock lock(mLock);
    return mSessionMap.size();
}
bool DrmSessionManager::containsSession(const Vector<uint8_t>& sessionId) const {
    Mutex::Autolock lock(mLock);
    return mSessionMap.count(toStdVec(sessionId));
}
<<<<<<< HEAD
void DrmSessionManager::binderDied() {
    ALOGW("ResourceManagerService died.");
    Mutex::Autolock lock(mLock);
    mService.reset();
||||||| 35b28e5f80
bool DrmSessionManager::getLeastUsedSession_l(
        int pid, sp<DrmSessionClientInterface>* drm, Vector<uint8_t>* sessionId) {
    ssize_t index = mSessionMap.indexOfKey(pid);
    if (index < 0) {
        return false;
    }
    int leastUsedIndex = -1;
    int64_t minTs = LLONG_MAX;
    const SessionInfos& infos = mSessionMap.valueAt(index);
    for (size_t j = 0; j < infos.size(); ++j) {
        if (leastUsedIndex == -1) {
            leastUsedIndex = j;
            minTs = infos[j].timeStamp;
        } else {
            if (infos[j].timeStamp < minTs) {
                leastUsedIndex = j;
                minTs = infos[j].timeStamp;
            }
        }
    }
    if (leastUsedIndex != -1) {
        *drm = infos[leastUsedIndex].drm;
        *sessionId = infos[leastUsedIndex].sessionId;
    }
    return (leastUsedIndex != -1);
=======
void DrmSessionManager::binderDied(const wp<IBinder>& ) {
    ALOGW("ResourceManagerService died.");
    Mutex::Autolock lock(mLock);
    mService.clear();
>>>>>>> 77b8c802
}
}
