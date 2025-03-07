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
#define LOG_TAG "DrmSessionManager_test"
#include <android/binder_auto_utils.h>
#include <utils/Log.h>

#include <gtest/gtest.h>

<<<<<<< HEAD
#include <aidl/android/media/BnResourceManagerClient.h>
#include <aidl/android/media/BnResourceManagerService.h>

||||||| 35b28e5f80
=======
#include <media/IResourceManagerService.h>
#include <media/IResourceManagerClient.h>
>>>>>>> 77b8c802
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/ProcessInfoInterface.h>
#include <mediadrm/DrmSessionManager.h>

<<<<<<< HEAD
#include <algorithm>
#include <iostream>
#include <vector>

#include "ResourceManagerService.h"

||||||| 35b28e5f80
=======
#include <algorithm>
#include <vector>

#include "ResourceManagerService.h"

>>>>>>> 77b8c802
namespace android {

<<<<<<< HEAD
using Status = ::ndk::ScopedAStatus;
using ::aidl::android::media::BnResourceManagerClient;
using ::aidl::android::media::BnResourceManagerService;
using ::aidl::android::media::MediaResourceParcel;
using ::aidl::android::media::IResourceManagerClient;

static Vector<uint8_t> toAndroidVector(const std::vector<uint8_t> &vec) {
    Vector<uint8_t> aVec;
    for (auto b : vec) {
        aVec.push_back(b);
    }
    return aVec;
}

||||||| 35b28e5f80
=======
static Vector<uint8_t> toAndroidVector(const std::vector<uint8_t> &vec) {
    Vector<uint8_t> aVec;
    for (auto b : vec) {
        aVec.push_back(b);
    }
    return aVec;
}

>>>>>>> 77b8c802
struct FakeProcessInfo : public ProcessInfoInterface {
    FakeProcessInfo() {}
    virtual ~FakeProcessInfo() {}

    virtual bool getPriority(int pid, int* priority) {
        // For testing, use pid as priority.
        // Lower the value higher the priority.
        *priority = pid;
        return true;
    }

    virtual bool isValidPid(int /* pid */) {
        return true;
    }

private:
    DISALLOW_EVIL_CONSTRUCTORS(FakeProcessInfo);
};

<<<<<<< HEAD
struct FakeDrm : public BnResourceManagerClient {
    FakeDrm(const std::vector<uint8_t>& sessionId, const sp<DrmSessionManager>& manager)
        : mSessionId(toAndroidVector(sessionId)),
          mReclaimed(false),
          mDrmSessionManager(manager) {}
||||||| 35b28e5f80
struct FakeDrm : public DrmSessionClientInterface {
    FakeDrm() {}
    virtual ~FakeDrm() {}
=======
struct FakeDrm : public BnResourceManagerClient {
    FakeDrm(const std::vector<uint8_t>& sessionId, const sp<DrmSessionManager>& manager)
        : mSessionId(toAndroidVector(sessionId)),
          mReclaimed(false),
          mDrmSessionManager(manager) {}

    virtual ~FakeDrm() {}
>>>>>>> 77b8c802

<<<<<<< HEAD
    Status reclaimResource(bool* _aidl_return) {
        mReclaimed = true;
        mDrmSessionManager->removeSession(mSessionId);
        *_aidl_return = true;
        return Status::ok();
||||||| 35b28e5f80
    virtual bool reclaimSession(const Vector<uint8_t>& sessionId) {
        mReclaimedSessions.push_back(sessionId);
        return true;
=======
    virtual bool reclaimResource() {
        mReclaimed = true;
        mDrmSessionManager->removeSession(mSessionId);
        return true;
>>>>>>> 77b8c802
    }

<<<<<<< HEAD
    Status getName(::std::string* _aidl_return) {
        String8 name("FakeDrm[");
        for (size_t i = 0; i < mSessionId.size(); ++i) {
            name.appendFormat("%02x", mSessionId[i]);
        }
        name.append("]");
        *_aidl_return = name;
        return Status::ok();
||||||| 35b28e5f80
    const Vector<Vector<uint8_t> >& reclaimedSessions() const {
        return mReclaimedSessions;
=======
    virtual String8 getName() {
        String8 name("FakeDrm[");
        for (size_t i = 0; i < mSessionId.size(); ++i) {
            name.appendFormat("%02x", mSessionId[i]);
        }
        name.append("]");
        return name;
>>>>>>> 77b8c802
    }

    bool isReclaimed() const {
        return mReclaimed;
    }

    const Vector<uint8_t> mSessionId;

private:
    bool mReclaimed;
    const sp<DrmSessionManager> mDrmSessionManager;

    DISALLOW_EVIL_CONSTRUCTORS(FakeDrm);
};

<<<<<<< HEAD
struct FakeSystemCallback :
        public ResourceManagerService::SystemCallbackInterface {
    FakeSystemCallback() {}

    virtual void noteStartVideo(int /*uid*/) override {}

    virtual void noteStopVideo(int /*uid*/) override {}

    virtual void noteResetVideo() override {}

    virtual bool requestCpusetBoost(bool /*enable*/) override {
        return true;
    }

protected:
    virtual ~FakeSystemCallback() {}

private:

    DISALLOW_EVIL_CONSTRUCTORS(FakeSystemCallback);
};

||||||| 35b28e5f80
=======
struct FakeSystemCallback :
        public ResourceManagerService::SystemCallbackInterface {
    FakeSystemCallback() {}

    virtual void noteStartVideo(int /*uid*/) override {}

    virtual void noteStopVideo(int /*uid*/) override {}

    virtual void noteResetVideo() override {}

    virtual bool requestCpusetBoost(
            bool /*enable*/, const sp<IInterface> &/*client*/) override {
        return true;
    }

protected:
    virtual ~FakeSystemCallback() {}

private:

    DISALLOW_EVIL_CONSTRUCTORS(FakeSystemCallback);
};

>>>>>>> 77b8c802
static const int kTestPid1 = 30;
static const int kTestPid2 = 20;
static const std::vector<uint8_t> kTestSessionId1{1, 2, 3};
static const std::vector<uint8_t> kTestSessionId2{4, 5, 6, 7, 8};
static const std::vector<uint8_t> kTestSessionId3{9, 0};

class DrmSessionManagerTest : public ::testing::Test {
public:
    DrmSessionManagerTest()
<<<<<<< HEAD
        : mService(::ndk::SharedRefBase::make<ResourceManagerService>
            (new FakeProcessInfo(), new FakeSystemCallback())),
          mDrmSessionManager(new DrmSessionManager(mService)),
          mTestDrm1(::ndk::SharedRefBase::make<FakeDrm>(
                  kTestSessionId1, mDrmSessionManager)),
          mTestDrm2(::ndk::SharedRefBase::make<FakeDrm>(
                  kTestSessionId2, mDrmSessionManager)),
          mTestDrm3(::ndk::SharedRefBase::make<FakeDrm>(
                  kTestSessionId3, mDrmSessionManager)) {
||||||| 35b28e5f80
        : mDrmSessionManager(new DrmSessionManager(new FakeProcessInfo())),
          mTestDrm1(new FakeDrm()),
          mTestDrm2(new FakeDrm()) {
        GetSessionId(kTestSessionId1, ARRAY_SIZE(kTestSessionId1), &mSessionId1);
        GetSessionId(kTestSessionId2, ARRAY_SIZE(kTestSessionId2), &mSessionId2);
        GetSessionId(kTestSessionId3, ARRAY_SIZE(kTestSessionId3), &mSessionId3);
=======
        : mService(new ResourceManagerService(new FakeProcessInfo(), new FakeSystemCallback())),
          mDrmSessionManager(new DrmSessionManager(mService)),
          mTestDrm1(new FakeDrm(kTestSessionId1, mDrmSessionManager)),
          mTestDrm2(new FakeDrm(kTestSessionId2, mDrmSessionManager)),
          mTestDrm3(new FakeDrm(kTestSessionId3, mDrmSessionManager)) {
        DrmSessionManager *ptr = new DrmSessionManager(mService);
        EXPECT_NE(ptr, nullptr);
        /* mDrmSessionManager = ptr; */
>>>>>>> 77b8c802
    }

protected:
    void addSession() {
        mDrmSessionManager->addSession(kTestPid1, mTestDrm1, mTestDrm1->mSessionId);
        mDrmSessionManager->addSession(kTestPid2, mTestDrm2, mTestDrm2->mSessionId);
        mDrmSessionManager->addSession(kTestPid2, mTestDrm3, mTestDrm3->mSessionId);
    }

<<<<<<< HEAD
    std::shared_ptr<ResourceManagerService> mService;
||||||| 35b28e5f80
=======
    sp<IResourceManagerService> mService;
>>>>>>> 77b8c802
    sp<DrmSessionManager> mDrmSessionManager;
<<<<<<< HEAD
    std::shared_ptr<FakeDrm> mTestDrm1;
    std::shared_ptr<FakeDrm> mTestDrm2;
    std::shared_ptr<FakeDrm> mTestDrm3;
||||||| 35b28e5f80
    sp<FakeDrm> mTestDrm1;
    sp<FakeDrm> mTestDrm2;
    Vector<uint8_t> mSessionId1;
    Vector<uint8_t> mSessionId2;
    Vector<uint8_t> mSessionId3;
=======
    sp<FakeDrm> mTestDrm1;
    sp<FakeDrm> mTestDrm2;
    sp<FakeDrm> mTestDrm3;
>>>>>>> 77b8c802
};

TEST_F(DrmSessionManagerTest, addSession) {
    addSession();

    EXPECT_EQ(3u, mDrmSessionManager->getSessionCount());
    EXPECT_TRUE(mDrmSessionManager->containsSession(mTestDrm1->mSessionId));
    EXPECT_TRUE(mDrmSessionManager->containsSession(mTestDrm2->mSessionId));
    EXPECT_TRUE(mDrmSessionManager->containsSession(mTestDrm3->mSessionId));
}

TEST_F(DrmSessionManagerTest, useSession) {
    addSession();

    mDrmSessionManager->useSession(mTestDrm1->mSessionId);
    mDrmSessionManager->useSession(mTestDrm3->mSessionId);

    EXPECT_EQ(3u, mDrmSessionManager->getSessionCount());
    EXPECT_TRUE(mDrmSessionManager->containsSession(mTestDrm1->mSessionId));
    EXPECT_TRUE(mDrmSessionManager->containsSession(mTestDrm2->mSessionId));
    EXPECT_TRUE(mDrmSessionManager->containsSession(mTestDrm3->mSessionId));
}

TEST_F(DrmSessionManagerTest, removeSession) {
    addSession();

    mDrmSessionManager->removeSession(mTestDrm2->mSessionId);

    EXPECT_EQ(2u, mDrmSessionManager->getSessionCount());
    EXPECT_TRUE(mDrmSessionManager->containsSession(mTestDrm1->mSessionId));
    EXPECT_FALSE(mDrmSessionManager->containsSession(mTestDrm2->mSessionId));
    EXPECT_TRUE(mDrmSessionManager->containsSession(mTestDrm3->mSessionId));
}

TEST_F(DrmSessionManagerTest, reclaimSession) {
    EXPECT_FALSE(mDrmSessionManager->reclaimSession(kTestPid1));
    addSession();

    // calling pid priority is too low
    EXPECT_FALSE(mDrmSessionManager->reclaimSession(50));

    EXPECT_TRUE(mDrmSessionManager->reclaimSession(10));
    EXPECT_TRUE(mTestDrm1->isReclaimed());

    // add a session from a higher priority process.
<<<<<<< HEAD
    const std::vector<uint8_t> sid{1, 3, 5};
    std::shared_ptr<FakeDrm> drm =
            ::ndk::SharedRefBase::make<FakeDrm>(sid, mDrmSessionManager);
    mDrmSessionManager->addSession(15, drm, drm->mSessionId);
||||||| 35b28e5f80
    sp<FakeDrm> drm = new FakeDrm;
    const uint8_t ids[] = {1, 3, 5};
    Vector<uint8_t> sessionId;
    GetSessionId(ids, ARRAY_SIZE(ids), &sessionId);
    mDrmSessionManager->addSession(15, drm, sessionId);
=======
    const std::vector<uint8_t> sid{1, 3, 5};
    sp<FakeDrm> drm = new FakeDrm(sid, mDrmSessionManager);
    mDrmSessionManager->addSession(15, drm, drm->mSessionId);
>>>>>>> 77b8c802

    // make sure mTestDrm2 is reclaimed next instead of mTestDrm3
    mDrmSessionManager->useSession(mTestDrm3->mSessionId);
    EXPECT_TRUE(mDrmSessionManager->reclaimSession(18));
    EXPECT_TRUE(mTestDrm2->isReclaimed());

    EXPECT_EQ(2u, mDrmSessionManager->getSessionCount());
    EXPECT_FALSE(mDrmSessionManager->containsSession(mTestDrm1->mSessionId));
    EXPECT_FALSE(mDrmSessionManager->containsSession(mTestDrm2->mSessionId));
    EXPECT_TRUE(mDrmSessionManager->containsSession(mTestDrm3->mSessionId));
    EXPECT_TRUE(mDrmSessionManager->containsSession(drm->mSessionId));
}

TEST_F(DrmSessionManagerTest, reclaimAfterUse) {
    // nothing to reclaim yet
    EXPECT_FALSE(mDrmSessionManager->reclaimSession(kTestPid1));
    EXPECT_FALSE(mDrmSessionManager->reclaimSession(kTestPid2));

    // add sessions from same pid
    mDrmSessionManager->addSession(kTestPid2, mTestDrm1, mTestDrm1->mSessionId);
    mDrmSessionManager->addSession(kTestPid2, mTestDrm2, mTestDrm2->mSessionId);
    mDrmSessionManager->addSession(kTestPid2, mTestDrm3, mTestDrm3->mSessionId);

    // use some but not all sessions
    mDrmSessionManager->useSession(mTestDrm1->mSessionId);
    mDrmSessionManager->useSession(mTestDrm1->mSessionId);
    mDrmSessionManager->useSession(mTestDrm2->mSessionId);

    // calling pid priority is too low
    int lowPriorityPid = kTestPid2 + 1;
    EXPECT_FALSE(mDrmSessionManager->reclaimSession(lowPriorityPid));

    // unused session is reclaimed first
    int highPriorityPid = kTestPid2 - 1;
    EXPECT_TRUE(mDrmSessionManager->reclaimSession(highPriorityPid));
    EXPECT_FALSE(mTestDrm1->isReclaimed());
    EXPECT_FALSE(mTestDrm2->isReclaimed());
    EXPECT_TRUE(mTestDrm3->isReclaimed());
    mDrmSessionManager->removeSession(mTestDrm3->mSessionId);

    // less-used session is reclaimed next
    EXPECT_TRUE(mDrmSessionManager->reclaimSession(highPriorityPid));
    EXPECT_FALSE(mTestDrm1->isReclaimed());
    EXPECT_TRUE(mTestDrm2->isReclaimed());
    EXPECT_TRUE(mTestDrm3->isReclaimed());

    // most-used session still open
    EXPECT_EQ(1u, mDrmSessionManager->getSessionCount());
    EXPECT_TRUE(mDrmSessionManager->containsSession(mTestDrm1->mSessionId));
    EXPECT_FALSE(mDrmSessionManager->containsSession(mTestDrm2->mSessionId));
    EXPECT_FALSE(mDrmSessionManager->containsSession(mTestDrm3->mSessionId));
}

} // namespace android
