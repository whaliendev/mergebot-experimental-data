#define LOG_TAG "ResourceManagerService"
#include <utils/Log.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <binder/IMediaResourceMonitor.h>
#include <binder/IServiceManager.h>
#include <cutils/sched_policy.h>
#include <dirent.h>
#include <media/MediaResourcePolicy.h>
#include <media/stagefright/ProcessInfo.h>
#include <mediautils/BatteryNotifier.h>
#include <mediautils/SchedulingPolicyService.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include "ResourceManagerService.h"
#include "ServiceLog.h"
namespace android {
template <typename T>
static String8 getString(const std::vector<T> &items) {
  String8 itemsStr;
  for (size_t i = 0; i < items.size(); ++i) {
    itemsStr.appendFormat("%s ", toString(items[i]).string());
  }
  return itemsStr;
}
static bool hasResourceType(MediaResource::Type type,
                            const ResourceInfos &infos) {
  for (size_t i = 0; i < infos.size(); ++i) {
    if (hasResourceType(type, infos[i].resources)) {
      return true;
    }
  }
  return false;
}
static bool hasResourceType(MediaResource::Type type,
                            const ResourceInfos &infos) {
  for (size_t i = 0; i < infos.size(); ++i) {
    if (hasResourceType(type, infos[i].resources)) {
      return true;
    }
  }
  return false;
}
static ResourceInfos &getResourceInfosForEdit(int pid,
                                              PidResourceInfosMap &map) {
  ssize_t index = map.indexOfKey(pid);
  if (index < 0) {
    ResourceInfos infosForPid;
    map.add(pid, infosForPid);
  }
  return map.editValueFor(pid);
}
static ResourceInfo &getResourceInfoForEdit(
    uid_t uid, int64_t clientId,
    const std::shared_ptr<IResourceManagerClient> &client,
    ResourceInfos &infos) {
  ssize_t index = infos.indexOfKey(clientId);
  if (index < 0) {
    ResourceInfo info;
    info.uid = uid;
    info.clientId = clientId;
    info.client = client;
    index = infos.add(clientId, info);
  }
  return infos.editValueAt(index);
}
static void notifyResourceGranted(
    int pid, const std::vector<MediaResourceParcel> &resources) {
  static const char *const kServiceName = "media_resource_monitor";
  sp<IBinder> binder =
      defaultServiceManager()->checkService(String16(kServiceName));
  if (binder != NULL) {
    sp<IMediaResourceMonitor> service =
        interface_cast<IMediaResourceMonitor>(binder);
    for (size_t i = 0; i < resources.size(); ++i) {
      if (resources[i].subType == MediaResource::SubType::kAudioCodec) {
        service->notifyResourceGranted(pid,
                                       IMediaResourceMonitor::TYPE_AUDIO_CODEC);
      } else if (resources[i].subType == MediaResource::SubType::kVideoCodec) {
        service->notifyResourceGranted(pid,
                                       IMediaResourceMonitor::TYPE_VIDEO_CODEC);
      }
    }
  }
}
binder_status_t ResourceManagerService::dump(int fd, const char ** ,
                                             uint32_t ) {
  String8 result;
  if (checkCallingPermission(String16("android.permission.DUMP")) == false) {
    result.format(
        "Permission Denial: "
        "can't dump ResourceManagerService from pid=%d, uid=%d\n",
        AIBinder_getCallingPid(), AIBinder_getCallingUid());
    write(fd, result.string(), result.size());
    return PERMISSION_DENIED;
  }
  PidResourceInfosMap mapCopy;
  bool supportsMultipleSecureCodecs;
  bool supportsSecureWithNonSecureCodec;
  String8 serviceLog;
  {
    Mutex::Autolock lock(mLock);
    mapCopy = mMap;
    supportsMultipleSecureCodecs = mSupportsMultipleSecureCodecs;
    supportsSecureWithNonSecureCodec = mSupportsSecureWithNonSecureCodec;
    serviceLog = mServiceLog->toString("    " );
  }
  const size_t SIZE = 256;
  char buffer[SIZE];
  snprintf(buffer, SIZE, "ResourceManagerService: %p\n", this);
  result.append(buffer);
  result.append("  Policies:\n");
  snprintf(buffer, SIZE, "    SupportsMultipleSecureCodecs: %d\n",
           supportsMultipleSecureCodecs);
  result.append(buffer);
  snprintf(buffer, SIZE, "    SupportsSecureWithNonSecureCodec: %d\n",
           supportsSecureWithNonSecureCodec);
  result.append(buffer);
  result.append("  Processes:\n");
  for (size_t i = 0; i < mapCopy.size(); ++i) {
    snprintf(buffer, SIZE, "    Pid: %d\n", mapCopy.keyAt(i));
    result.append(buffer);
    const ResourceInfos &infos = mapCopy.valueAt(i);
    for (size_t j = 0; j < infos.size(); ++j) {
      result.append("      Client:\n");
      snprintf(buffer, SIZE, "        Id: %lld\n",
               (long long)infos[j].clientId);
      result.append(buffer);
      std::string clientName;
      Status status = infos[j].client->getName(&clientName);
      if (!status.isOk()) {
        clientName = "<unknown client>";
      }
      snprintf(buffer, SIZE, "        Name: %s\n", clientName.c_str());
      result.append(buffer);
      const ResourceList &resources = infos[j].resources;
      result.append("        Resources:\n");
      for (auto it = resources.begin(); it != resources.end(); it++) {
        snprintf(buffer, SIZE, "          %s\n", toString(it->second).string());
        result.append(buffer);
      }
    }
  }
  result.append("  Events logs (most recent at top):\n");
  result.append(serviceLog);
  write(fd, result.string(), result.size());
  return OK;
}
struct SystemCallbackImpl
    : public ResourceManagerService::SystemCallbackInterface {
  SystemCallbackImpl() : mClientToken(new BBinder()) {}
  virtual void noteStartVideo(int uid) override {
    BatteryNotifier::getInstance().noteStartVideo(uid);
  }
  virtual void noteStopVideo(int uid) override {
    BatteryNotifier::getInstance().noteStopVideo(uid);
  }
  virtual void noteResetVideo() override {
    BatteryNotifier::getInstance().noteResetVideo();
  }
  virtual bool requestCpusetBoost(bool enable) override {
    return android::requestCpusetBoost(enable, mClientToken);
  }
 protected:
  virtual ~SystemCallbackImpl() {}
 private:
  DISALLOW_EVIL_CONSTRUCTORS(SystemCallbackImpl);
  sp<IBinder> mClientToken;
};
ResourceManagerService::ResourceManagerService()
    : ResourceManagerService(new ProcessInfo(), new SystemCallbackImpl()) {}
ResourceManagerService::ResourceManagerService(
    const sp<ProcessInfoInterface> &processInfo,
    const sp<SystemCallbackInterface> &systemResource)
    : mProcessInfo(processInfo),
      mSystemCB(systemResource),
      mServiceLog(new ServiceLog()),
      mSupportsMultipleSecureCodecs(true),
      mSupportsSecureWithNonSecureCodec(true),
      mCpuBoostCount(0),
      mDeathRecipient(
          AIBinder_DeathRecipient_new(DeathNotifier::BinderDiedCallback)) {
  mSystemCB->noteResetVideo();
}
void ResourceManagerService::instantiate() {
  std::shared_ptr<ResourceManagerService> service =
      ::ndk::SharedRefBase::make<ResourceManagerService>();
  binder_status_t status =
      AServiceManager_addService(service->asBinder().get(), getServiceName());
  if (status != STATUS_OK) {
    return;
  }
}
ResourceManagerService::~ResourceManagerService() {}
Status ResourceManagerService::config(
    const std::vector<MediaResourcePolicyParcel> &policies) {
  String8 log = String8::format("config(%s)", getString(policies).string());
  mServiceLog->add(log);
  Mutex::Autolock lock(mLock);
  for (size_t i = 0; i < policies.size(); ++i) {
    const std::string &type = policies[i].type;
    const std::string &value = policies[i].value;
    if (type == MediaResourcePolicy::kPolicySupportsMultipleSecureCodecs()) {
      mSupportsMultipleSecureCodecs = (value == "true");
    } else if (type ==
               MediaResourcePolicy::kPolicySupportsSecureWithNonSecureCodec()) {
      mSupportsSecureWithNonSecureCodec = (value == "true");
    }
  }
  return Status::ok();
}
void ResourceManagerService::onFirstAdded(const MediaResourceParcel &resource,
                                          const ResourceInfo &clientInfo) {
  if (resource.type == MediaResource::Type::kCpuBoost &&
      resource.subType == MediaResource::SubType::kUnspecifiedSubType) {
    if (mSystemCB->requestCpusetBoost(true) != OK) {
      ALOGW("couldn't request cpuset boost");
    }
    mCpuBoostCount++;
  } else if (resource.type == MediaResource::Type::kBattery &&
             resource.subType == MediaResource::SubType::kVideoCodec) {
    mSystemCB->noteStartVideo(clientInfo.uid);
  }
}
void ResourceManagerService::onLastRemoved(const MediaResourceParcel &resource,
                                           const ResourceInfo &clientInfo) {
  if (resource.type == MediaResource::Type::kCpuBoost &&
      resource.subType == MediaResource::SubType::kUnspecifiedSubType &&
      mCpuBoostCount > 0) {
    if (--mCpuBoostCount == 0) {
      mSystemCB->requestCpusetBoost(false);
    }
  } else if (resource.type == MediaResource::Type::kBattery &&
             resource.subType == MediaResource::SubType::kVideoCodec) {
    mSystemCB->noteStopVideo(clientInfo.uid);
  }
}
void ResourceManagerService::mergeResources(MediaResource &r1,
                                            const MediaResource &r2) {
  if (r1.mType == MediaResource::kDrmSession) {
    r1.mValue -= (r1.mValue == 0 ? 0 : 1);
  } else {
    r1.mValue += r2.mValue;
  }
}
Status ResourceManagerService::addResource(
    int32_t pid, int32_t uid, int64_t clientId,
    const std::shared_ptr<IResourceManagerClient> &client,
    const std::vector<MediaResourceParcel> &resources) {
  String8 log =
      String8::format("addResource(pid %d, clientId %lld, resources %s)", pid,
                      (long long)clientId, getString(resources).string());
  mServiceLog->add(log);
  Mutex::Autolock lock(mLock);
  if (!mProcessInfo->isValidPid(pid)) {
    ALOGE("Rejected addResource call with invalid pid.");
    return Status::fromServiceSpecificError(BAD_VALUE);
  }
  ResourceInfos &infos = getResourceInfosForEdit(pid, mMap);
  ResourceInfo &info = getResourceInfoForEdit(uid, clientId, client, infos);
  for (size_t i = 0; i < resources.size(); ++i) {
<<<<<<< HEAD
    const auto &res = resources[i];
    const auto resType = std::tuple(res.type, res.subType, res.id);
    if (res.value < 0 && res.type != MediaResource::Type::kDrmSession) {
      ALOGW("Ignoring request to remove negative value of non-drm resource");
      continue;
    }
||||||| 35b28e5f80
    const auto resType =
        std::make_pair(resources[i].mType, resources[i].mSubType);
=======
    const auto &res = resources[i];
    const auto resType = std::tuple(res.mType, res.mSubType, res.mId);
>>>>>>> 77b8c802dbf3d63e19ff44e7bb2fe47b37e8d6b2
    if (info.resources.find(resType) == info.resources.end()) {
<<<<<<< HEAD
      if (res.value <= 0) {
        ALOGW("Ignoring request to add new resource entry with value <= 0");
        continue;
      }
      onFirstAdded(res, info);
      info.resources[resType] = res;
||||||| 35b28e5f80
      onFirstAdded(resources[i], info);
      info.resources[resType] = resources[i];
=======
      onFirstAdded(res, info);
      info.resources[resType] = res;
>>>>>>> 77b8c802dbf3d63e19ff44e7bb2fe47b37e8d6b2
    } else {
      mergeResources(info.resources[resType], res);
    }
  }
<<<<<<< HEAD
  if (info.deathNotifier == nullptr && client != nullptr) {
    info.deathNotifier =
        new DeathNotifier(ref<ResourceManagerService>(), pid, clientId);
    AIBinder_linkToDeath(client->asBinder().get(), mDeathRecipient.get(),
                         info.deathNotifier.get());
||||||| 35b28e5f80
  if (info.deathNotifier == nullptr) {
    info.deathNotifier = new DeathNotifier(this, pid, clientId);
    IInterface::asBinder(client)->linkToDeath(info.deathNotifier);
=======
  if (info.deathNotifier == nullptr && client != nullptr) {
    info.deathNotifier = new DeathNotifier(this, pid, clientId);
    IInterface::asBinder(client)->linkToDeath(info.deathNotifier);
>>>>>>> 77b8c802dbf3d63e19ff44e7bb2fe47b37e8d6b2
  }
  notifyResourceGranted(pid, resources);
  return Status::ok();
}
Status ResourceManagerService::removeResource(
    int32_t pid, int64_t clientId,
    const std::vector<MediaResourceParcel> &resources) {
  String8 log =
      String8::format("removeResource(pid %d, clientId %lld, resources %s)",
                      pid, (long long)clientId, getString(resources).string());
  mServiceLog->add(log);
  Mutex::Autolock lock(mLock);
  if (!mProcessInfo->isValidPid(pid)) {
    ALOGE("Rejected removeResource call with invalid pid.");
    return Status::fromServiceSpecificError(BAD_VALUE);
  }
  ssize_t index = mMap.indexOfKey(pid);
  if (index < 0) {
    ALOGV("removeResource: didn't find pid %d for clientId %lld", pid,
          (long long)clientId);
    return Status::ok();
  }
  ResourceInfos &infos = mMap.editValueAt(index);
  index = infos.indexOfKey(clientId);
  if (index < 0) {
    ALOGV("removeResource: didn't find clientId %lld", (long long)clientId);
    return Status::ok();
  }
  ResourceInfo &info = infos.editValueAt(index);
  for (size_t i = 0; i < resources.size(); ++i) {
<<<<<<< HEAD
    const auto &res = resources[i];
    const auto resType = std::tuple(res.type, res.subType, res.id);
    if (res.value < 0) {
      ALOGW("Ignoring request to remove negative value of resource");
      continue;
    }
||||||| 35b28e5f80
    const auto resType =
        std::make_pair(resources[i].mType, resources[i].mSubType);
=======
    const auto &res = resources[i];
    const auto resType = std::tuple(res.mType, res.mSubType, res.mId);
>>>>>>> 77b8c802dbf3d63e19ff44e7bb2fe47b37e8d6b2
    if (info.resources.find(resType) != info.resources.end()) {
<<<<<<< HEAD
      MediaResourceParcel &resource = info.resources[resType];
      if (resource.value > res.value) {
        resource.value -= res.value;
||||||| 35b28e5f80
      MediaResource &resource = info.resources[resType];
      if (resource.mValue > resources[i].mValue) {
        resource.mValue -= resources[i].mValue;
=======
      MediaResource &resource = info.resources[resType];
      if (resource.mValue > res.mValue) {
        resource.mValue -= res.mValue;
>>>>>>> 77b8c802dbf3d63e19ff44e7bb2fe47b37e8d6b2
      } else {
<<<<<<< HEAD
        onLastRemoved(res, info);
||||||| 35b28e5f80
        onLastRemoved(resources[i], info);
=======
        onLastRemoved(res, info);
>>>>>>> 77b8c802dbf3d63e19ff44e7bb2fe47b37e8d6b2
        info.resources.erase(resType);
      }
    }
  }
  return Status::ok();
}
Status ResourceManagerService::removeClient(int32_t pid, int64_t clientId) {
  removeResource(pid, clientId, true);
  return Status::ok();
}
Status ResourceManagerService::removeResource(int pid, int64_t clientId,
                                              bool checkValid) {
  String8 log = String8::format("removeResource(pid %d, clientId %lld)", pid,
                                (long long)clientId);
  mServiceLog->add(log);
  Mutex::Autolock lock(mLock);
  if (checkValid && !mProcessInfo->isValidPid(pid)) {
    ALOGE("Rejected removeResource call with invalid pid.");
    return Status::fromServiceSpecificError(BAD_VALUE);
  }
  ssize_t index = mMap.indexOfKey(pid);
  if (index < 0) {
    ALOGV("removeResource: didn't find pid %d for clientId %lld", pid,
          (long long)clientId);
    return Status::ok();
  }
  ResourceInfos &infos = mMap.editValueAt(index);
  index = infos.indexOfKey(clientId);
  if (index < 0) {
    ALOGV("removeResource: didn't find clientId %lld", (long long)clientId);
    return Status::ok();
  }
  const ResourceInfo &info = infos[index];
  for (auto it = info.resources.begin(); it != info.resources.end(); it++) {
    onLastRemoved(it->second, info);
  }
  AIBinder_unlinkToDeath(info.client->asBinder().get(), mDeathRecipient.get(),
                         info.deathNotifier.get());
  infos.removeItemsAt(index);
  return Status::ok();
}
void ResourceManagerService::getClientForResource_l(
    int callingPid, const MediaResourceParcel *res,
    Vector<std::shared_ptr<IResourceManagerClient>> *clients) {
  if (res == NULL) {
    return;
  }
  std::shared_ptr<IResourceManagerClient> client;
  if (getLowestPriorityBiggestClient_l(callingPid, res->type, &client)) {
    clients->push_back(client);
  }
}
Status ResourceManagerService::reclaimResource(
    int32_t callingPid, const std::vector<MediaResourceParcel> &resources,
    bool *_aidl_return) {
  String8 log = String8::format("reclaimResource(callingPid %d, resources %s)",
                                callingPid, getString(resources).string());
  mServiceLog->add(log);
  *_aidl_return = false;
  Vector<std::shared_ptr<IResourceManagerClient>> clients;
  {
    Mutex::Autolock lock(mLock);
    if (!mProcessInfo->isValidPid(callingPid)) {
      ALOGE("Rejected reclaimResource call with invalid callingPid.");
      return Status::fromServiceSpecificError(BAD_VALUE);
    }
<<<<<<< HEAD
    const MediaResourceParcel *secureCodec = NULL;
    const MediaResourceParcel *nonSecureCodec = NULL;
    const MediaResourceParcel *graphicMemory = NULL;
    const MediaResourceParcel *drmSession = NULL;
||||||| 35b28e5f80
    const MediaResource *secureCodec = NULL;
    const MediaResource *nonSecureCodec = NULL;
    const MediaResource *graphicMemory = NULL;
=======
    const MediaResource *secureCodec = NULL;
    const MediaResource *nonSecureCodec = NULL;
    const MediaResource *graphicMemory = NULL;
    const MediaResource *drmSession = NULL;
>>>>>>> 77b8c802dbf3d63e19ff44e7bb2fe47b37e8d6b2
    for (size_t i = 0; i < resources.size(); ++i) {
      MediaResource::Type type = resources[i].type;
      if (resources[i].type == MediaResource::Type::kSecureCodec) {
        secureCodec = &resources[i];
      } else if (type == MediaResource::Type::kNonSecureCodec) {
        nonSecureCodec = &resources[i];
      } else if (type == MediaResource::Type::kGraphicMemory) {
        graphicMemory = &resources[i];
<<<<<<< HEAD
      } else if (type == MediaResource::Type::kDrmSession) {
        drmSession = &resources[i];
||||||| 35b28e5f80
=======
      } else if (type == MediaResource::kDrmSession) {
        drmSession = &resources[i];
>>>>>>> 77b8c802dbf3d63e19ff44e7bb2fe47b37e8d6b2
      }
    }
    if (secureCodec != NULL) {
      if (!mSupportsMultipleSecureCodecs) {
        if (!getAllClients_l(callingPid, MediaResource::Type::kSecureCodec,
                             &clients)) {
          return Status::ok();
        }
      }
      if (!mSupportsSecureWithNonSecureCodec) {
        if (!getAllClients_l(callingPid, MediaResource::Type::kNonSecureCodec,
                             &clients)) {
          return Status::ok();
        }
      }
    }
    if (nonSecureCodec != NULL) {
      if (!mSupportsSecureWithNonSecureCodec) {
        if (!getAllClients_l(callingPid, MediaResource::Type::kSecureCodec,
                             &clients)) {
          return Status::ok();
        }
      }
    }
<<<<<<< HEAD
    if (drmSession != NULL) {
      getClientForResource_l(callingPid, drmSession, &clients);
      if (clients.size() == 0) {
        return Status::ok();
      }
    }
||||||| 35b28e5f80
=======
    if (drmSession != NULL) {
      getClientForResource_l(callingPid, drmSession, &clients);
      if (clients.size() == 0) {
        return false;
      }
    }
>>>>>>> 77b8c802dbf3d63e19ff44e7bb2fe47b37e8d6b2
    if (clients.size() == 0) {
      getClientForResource_l(callingPid, graphicMemory, &clients);
    }
    if (clients.size() == 0) {
      getClientForResource_l(callingPid, secureCodec, &clients);
      getClientForResource_l(callingPid, nonSecureCodec, &clients);
    }
    if (clients.size() == 0) {
      if (secureCodec != NULL) {
        MediaResource temp(MediaResource::Type::kNonSecureCodec, 1);
        getClientForResource_l(callingPid, &temp, &clients);
      }
      if (nonSecureCodec != NULL) {
        MediaResource temp(MediaResource::Type::kSecureCodec, 1);
        getClientForResource_l(callingPid, &temp, &clients);
      }
    }
  }
  if (clients.size() == 0) {
    return Status::ok();
  }
  std::shared_ptr<IResourceManagerClient> failedClient;
  for (size_t i = 0; i < clients.size(); ++i) {
    log = String8::format("reclaimResource from client %p", clients[i].get());
    mServiceLog->add(log);
    bool success;
    Status status = clients[i]->reclaimResource(&success);
    if (!status.isOk() || !success) {
      failedClient = clients[i];
      break;
    }
  }
  if (failedClient == NULL) {
    *_aidl_return = true;
    return Status::ok();
  }
  {
    Mutex::Autolock lock(mLock);
    bool found = false;
    for (size_t i = 0; i < mMap.size(); ++i) {
      ResourceInfos &infos = mMap.editValueAt(i);
      for (size_t j = 0; j < infos.size();) {
        if (infos[j].client == failedClient) {
          j = infos.removeItemsAt(j);
          found = true;
        } else {
          ++j;
        }
      }
      if (found) {
        break;
      }
    }
    if (!found) {
      ALOGV("didn't find failed client");
    }
  }
  return Status::ok();
}
bool ResourceManagerService::getAllClients_l(
    int callingPid, MediaResource::Type type,
    Vector<std::shared_ptr<IResourceManagerClient>> *clients) {
  Vector<std::shared_ptr<IResourceManagerClient>> temp;
  for (size_t i = 0; i < mMap.size(); ++i) {
    ResourceInfos &infos = mMap.editValueAt(i);
    for (size_t j = 0; j < infos.size(); ++j) {
      if (hasResourceType(type, infos[j].resources)) {
        if (!isCallingPriorityHigher_l(callingPid, mMap.keyAt(i))) {
          ALOGE("getAllClients_l: can't reclaim resource %s from pid %d",
                asString(type), mMap.keyAt(i));
          return false;
        }
        temp.push_back(infos[j].client);
      }
    }
  }
  if (temp.size() == 0) {
    ALOGV("getAllClients_l: didn't find any resource %s", asString(type));
    return true;
  }
  clients->appendVector(temp);
  return true;
}
bool ResourceManagerService::getLowestPriorityBiggestClient_l(
    int callingPid, MediaResource::Type type,
    std::shared_ptr<IResourceManagerClient> *client) {
  int lowestPriorityPid;
  int lowestPriority;
  int callingPriority;
  if (!mProcessInfo->getPriority(callingPid, &callingPriority)) {
    ALOGE(
        "getLowestPriorityBiggestClient_l: can't get process priority for pid "
        "%d",
        callingPid);
    return false;
  }
  if (!getLowestPriorityPid_l(type, &lowestPriorityPid, &lowestPriority)) {
    return false;
  }
  if (lowestPriority <= callingPriority) {
    ALOGE(
        "getLowestPriorityBiggestClient_l: lowest priority %d vs caller "
        "priority %d",
        lowestPriority, callingPriority);
    return false;
  }
  if (!getBiggestClient_l(lowestPriorityPid, type, client)) {
    return false;
  }
  return true;
}
bool ResourceManagerService::getLowestPriorityPid_l(MediaResource::Type type,
                                                    int *lowestPriorityPid,
                                                    int *lowestPriority) {
  int pid = -1;
  int priority = -1;
  for (size_t i = 0; i < mMap.size(); ++i) {
    if (mMap.valueAt(i).size() == 0) {
      continue;
    }
    if (!hasResourceType(type, mMap.valueAt(i))) {
      continue;
    }
    int tempPid = mMap.keyAt(i);
    int tempPriority;
    if (!mProcessInfo->getPriority(tempPid, &tempPriority)) {
      ALOGV("getLowestPriorityPid_l: can't get priority of pid %d, skipped",
            tempPid);
      continue;
    }
    if (pid == -1 || tempPriority > priority) {
      pid = tempPid;
      priority = tempPriority;
    }
  }
  if (pid != -1) {
    *lowestPriorityPid = pid;
    *lowestPriority = priority;
  }
  return (pid != -1);
}
bool ResourceManagerService::isCallingPriorityHigher_l(int callingPid,
                                                       int pid) {
  int callingPidPriority;
  if (!mProcessInfo->getPriority(callingPid, &callingPidPriority)) {
    return false;
  }
  int priority;
  if (!mProcessInfo->getPriority(pid, &priority)) {
    return false;
  }
  return (callingPidPriority < priority);
}
bool ResourceManagerService::getBiggestClient_l(
    int pid, MediaResource::Type type,
    std::shared_ptr<IResourceManagerClient> *client) {
  ssize_t index = mMap.indexOfKey(pid);
  if (index < 0) {
    ALOGE("getBiggestClient_l: can't find resource info for pid %d", pid);
    return false;
  }
  std::shared_ptr<IResourceManagerClient> clientTemp;
  uint64_t largestValue = 0;
  const ResourceInfos &infos = mMap.valueAt(index);
  for (size_t i = 0; i < infos.size(); ++i) {
    const ResourceList &resources = infos[i].resources;
    for (auto it = resources.begin(); it != resources.end(); it++) {
      const MediaResourceParcel &resource = it->second;
      if (resource.type == type) {
        if (resource.value > largestValue) {
          largestValue = resource.value;
          clientTemp = infos[i].client;
        }
      }
    }
  }
  if (clientTemp == NULL) {
    ALOGE("getBiggestClient_l: can't find resource type %s for pid %d",
          asString(type), pid);
    return false;
  }
  *client = clientTemp;
  return true;
}
DeathNotifier::DeathNotifier(
    const std::shared_ptr<ResourceManagerService> &service, int pid,
    int64_t clientId)
    : mService(service), mPid(pid), mClientId(clientId) {}
void DeathNotifier::BinderDiedCallback(void *cookie) {
  auto thiz = static_cast<DeathNotifier *>(cookie);
  thiz->binderDied();
}
void DeathNotifier::binderDied() {
  std::shared_ptr<ResourceManagerService> service = mService.lock();
  if (service == nullptr) {
    ALOGW("ResourceManagerService is dead as well.");
    return;
  }
  service->removeResource(mPid, mClientId, false);
}
}
