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

#ifndef DRM_SESSION_MANAGER_H_

#define DRM_SESSION_MANAGER_H_

<<<<<<< HEAD
#include <aidl/android/media/IResourceManagerClient.h>
#include <aidl/android/media/IResourceManagerService.h>
#include <android/binder_auto_utils.h>
||||||| 35b28e5f80
=======
#include <binder/IBinder.h>
#include <media/IResourceManagerService.h>
>>>>>>> 77b8c802
#include <media/stagefright/foundation/ABase.h>
#include <utils/RefBase.h>
#include <utils/KeyedVector.h>
#include <utils/threads.h>
#include <utils/Vector.h>

<<<<<<< HEAD
#include <map>
#include <memory>
#include <utility>
#include <vector>

||||||| 35b28e5f80
=======
#include <map>
#include <utility>
#include <vector>

>>>>>>> 77b8c802
namespace android {

class DrmSessionManagerTest;
<<<<<<< HEAD

using aidl::android::media::IResourceManagerClient;
using aidl::android::media::IResourceManagerService;
||||||| 35b28e5f80
struct DrmSessionClientInterface;
struct ProcessInfoInterface;
=======
class IResourceManagerClient;
>>>>>>> 77b8c802

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
<<<<<<< HEAD
    explicit DrmSessionManager(const std::shared_ptr<IResourceManagerService> &service);
||||||| 35b28e5f80
    explicit DrmSessionManager(sp<ProcessInfoInterface> processInfo);
=======
    explicit DrmSessionManager(const sp<IResourceManagerService> &service);
>>>>>>> 77b8c802

<<<<<<< HEAD
    void addSession(int pid,
            const std::shared_ptr<IResourceManagerClient>& drm,
            const Vector<uint8_t>& sessionId);
||||||| 35b28e5f80
    void addSession(int pid, const sp<DrmSessionClientInterface>& drm, const Vector<uint8_t>& sessionId);
=======
    void addSession(int pid, const sp<IResourceManagerClient>& drm, const Vector<uint8_t>& sessionId);
>>>>>>> 77b8c802
    void useSession(const Vector<uint8_t>& sessionId);
    void removeSession(const Vector<uint8_t>& sessionId);
    bool reclaimSession(int callingPid);

<<<<<<< HEAD
    // sanity check APIs
    size_t getSessionCount() const;
    bool containsSession(const Vector<uint8_t>& sessionId) const;

    // implements DeathRecipient
    void binderDied();

||||||| 35b28e5f80
=======
    // sanity check APIs
    size_t getSessionCount() const;
    bool containsSession(const Vector<uint8_t>& sessionId) const;

    // implements DeathRecipient
    virtual void binderDied(const wp<IBinder>& /*who*/);

>>>>>>> 77b8c802
protected:
    virtual ~DrmSessionManager();

private:
    void init();

<<<<<<< HEAD
    std::shared_ptr<IResourceManagerService> mService;
||||||| 35b28e5f80
    int64_t getTime_l();
    bool getLowestPriority_l(int* lowestPriorityPid, int* lowestPriority);
    bool getLeastUsedSession_l(
            int pid, sp<DrmSessionClientInterface>* drm, Vector<uint8_t>* sessionId);

    sp<ProcessInfoInterface> mProcessInfo;
=======
    sp<IResourceManagerService> mService;
>>>>>>> 77b8c802
    mutable Mutex mLock;
<<<<<<< HEAD
    SessionInfoMap mSessionMap;
    bool mInitialized;
    ::ndk::ScopedAIBinder_DeathRecipient mDeathRecipient;
||||||| 35b28e5f80
    PidSessionInfosMap mSessionMap;
    int64_t mTime;
=======
    SessionInfoMap mSessionMap;
    bool mInitialized;
>>>>>>> 77b8c802

    DISALLOW_EVIL_CONSTRUCTORS(DrmSessionManager);
};

}  // namespace android

#endif  // DRM_SESSION_MANAGER_H_
