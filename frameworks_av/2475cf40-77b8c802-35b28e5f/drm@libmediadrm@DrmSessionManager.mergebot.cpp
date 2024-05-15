/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
//#define LOG_NDEBUG 0

#define LOG_TAG "DrmSessionManager"
#include <utils/Log.h>
#include <binder/IPCThreadState.h>
#include <binder/IProcessInfoService.h>
#include <binder/IServiceManager.h>
#include <media/IResourceManagerClient.h>
#include <media/MediaResource.h>
#include <media/stagefright/ProcessInfo.h>
#include <mediadrm/DrmSessionClientInterface.h>
#include <aidl/android/media/IResourceManagerClient.h>
#include <aidl/android/media/IResourceManagerService.h>
#include <aidl/android/media/MediaResourceParcel.h>
#include <android/binder_ibinder.h>
#include <android/binder_manager.h>
#include <cutils/properties.h>
#include <mediadrm/DrmUtils.h>
#include <mediadrm/DrmSessionManager.h>
#include <unistd.h>
#include <utils/String8.h>
#include <vector>
#include "ResourceManagerService.h"

namespace android {

static String8 GetSessionIdString(const Vector<uint8_t> &sessionId) {
  String8 sessionIdStr;
  for (size_t i = 0; i < sessionId.size(); ++i) {
    sessionIdStr.appendFormat("%u ", sessionId[i]);
  }
  return sessionIdStr;
}

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
  MediaResourceParcel resource{Type::kDrmSession, SubType::kUnspecifiedSubType,
                               toStdVec<int8_t>(sessionId), value};
  resources.push_back(resource);
  return resources;
}

static std::vector<uint8_t> toStdVec(const Vector<uint8_t> &vector) {
  const uint8_t *v = vector.array();
  std::vector<uint8_t> vec(v, v + vector.size());
  return vec;
}

static uint64_t toClientId(const sp<IResourceManagerClient> &drm) {
  return reinterpret_cast<int64_t>(drm.get());
}

static Vector<MediaResource> toResourceVec(const Vector<uint8_t> &sessionId) {
  Vector<MediaResource> resources;
  // use UINT64_MAX to decrement through addition overflow
  resources.push_back(MediaResource(MediaResource::kDrmSession,
                                    toStdVec(sessionId), UINT64_MAX));
  return resources;
}

static sp<IResourceManagerService> getResourceManagerService() {
  if (property_get_bool("persist.device_config.media_native.mediadrmserver",
                        1)) {
    return new ResourceManagerService();
  }
  sp<IServiceManager> sm = defaultServiceManager();
  if (sm == NULL) {
    return NULL;
  }
  sp<IBinder> binder = sm->getService(String16("media.resource_manager"));
  return interface_cast<IResourceManagerService>(binder);
}

bool isEqualSessionId(const Vector<uint8_t> &sessionId1,
                      const Vector<uint8_t> &sessionId2) {
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
>>>>>>> 77b8c802dbf3d63e19ff44e7bb2fe47b37e8d6b2
  return drmSessionManager;
}

DrmSessionManager::DrmSessionManager()
    : DrmSessionManager(getResourceManagerService()),
      mProcessInfo(new ProcessInfo()),
      mTime(0) {}

DrmSessionManager::DrmSessionManager(const sp<IResourceManagerService> &service)
    : mService(service), mInitialized(false) {
  if (mService == NULL) {
    ALOGE("Failed to init ResourceManagerService");
  }
}

DrmSessionManager::~DrmSessionManager() < < < < < < < HEAD {
  if (mService != NULL) {
    AIBinder_unlinkToDeath(mService->asBinder().get(), mDeathRecipient.get(),
                           this);
  }
}
||||||| 35b28e5f80
{}
=======
{
  if (mService != NULL) {
    IInterface::asBinder(mService)->unlinkToDeath(this);
  }
}
>>>>>>> 77b8c802dbf3d63e19ff44e7bb2fe47b37e8d6b2

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
                                   const sp<IResourceManagerClient> &drm,
                                   const Vector<uint8_t> &sessionId) {
  uid_t uid = IPCThreadState::self()->getCallingUid();
  ALOGV("addSession(pid %d, uid %d, drm %p, sessionId %s)", pid, uid, drm.get(),
        GetSessionIdString(sessionId).string());

  Mutex::Autolock lock(mLock);
  if (mService == NULL) {
    return;
  }

  int64_t clientId = toClientId(drm);
  mSessionMap[toStdVec(sessionId)] = (SessionInfo){pid, uid, clientId};
  mService->addResource(pid, uid, clientId, drm, toResourceVec(sessionId));
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
  mService->addResource(info.pid, info.uid, info.clientId, NULL,
                        toResourceVec(sessionId, -1));
||||||| 35b28e5f80
}
}
=======

  auto info = it->second;
  mService->addResource(info.pid, info.uid, info.clientId, NULL,
                        toResourceVec(sessionId));
>>>>>>> 77b8c802dbf3d63e19ff44e7bb2fe47b37e8d6b2
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
  mService->removeResource(info.pid, info.clientId,
                           toResourceVec(sessionId, INT64_MAX));
  mSessionMap.erase(it);
||||||| 35b28e5f80
}
}
=======

  auto info = it->second;
  mService->removeResource(info.pid, info.clientId, toResourceVec(sessionId));
  mSessionMap.erase(it);
>>>>>>> 77b8c802dbf3d63e19ff44e7bb2fe47b37e8d6b2
}

bool DrmSessionManager::reclaimSession(int callingPid) {
  ALOGV("reclaimSession(%d)", callingPid);

<<<<<<< HEAD
  // unlock early because reclaimResource might callback into removeSession
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
  // unlock early because reclaimResource might callback into removeSession
  mLock.lock();
  sp<IResourceManagerService> service(mService);
  mLock.unlock();
>>>>>>> 77b8c802dbf3d63e19ff44e7bb2fe47b37e8d6b2

  if (service == NULL) {
    return false;
  }

<<<<<<< HEAD
  // cannot update mSessionMap because we do not know which sessionId is reclaimed;
  // we rely on IResourceManagerClient to removeSession in reclaimResource
  Vector<uint8_t> dummy;
  bool success;
  ScopedAStatus status = service->reclaimResource(
      callingPid, toResourceVec(dummy, INT64_MAX), &success);
  return status.isOk() && success;
||||||| 35b28e5f80
    return drm->reclaimSession(sessionId);
=======
  // cannot update mSessionMap because we do not know which sessionId is reclaimed;
  // we rely on IResourceManagerClient to removeSession in reclaimResource
  Vector<uint8_t> dummy;
  return service->reclaimResource(callingPid, toResourceVec(dummy));
>>>>>>> 77b8c802dbf3d63e19ff44e7bb2fe47b37e8d6b2
}

size_t DrmSessionManager::getSessionCount() const {
  Mutex::Autolock lock(mLock);
  return mSessionMap.size();
}

bool DrmSessionManager::containsSession(
    const Vector<uint8_t> &sessionId) const {
  Mutex::Autolock lock(mLock);
  return mSessionMap.count(toStdVec(sessionId));
}

void DrmSessionManager::binderDied() {
  ALOGW("ResourceManagerService died.");
  Mutex::Autolock lock(mLock);
  mService.reset();
}

void DrmSessionManager::binderDied(const wp<IBinder> & /*who*/) {
  ALOGW("ResourceManagerService died.");
  Mutex::Autolock lock(mLock);
  mService.clear();
}

using aidl::android::media::MediaResourceParcel;

namespace {
void ResourceManagerServiceDied(void *cookie) {
  auto thiz = static_cast<DrmSessionManager *>(cookie);
  thiz->binderDied();
}
}

using ::ndk::ScopedAStatus;

}  // namespace android