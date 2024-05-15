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
#include <media/IResourceManagerService.h>
#include <media/IResourceManagerClient.h>
#include <aidl/android/media/BnResourceManagerClient.h>
#include <aidl/android/media/BnResourceManagerService.h>
#include <mediadrm/DrmHal.h>
#include <mediadrm/DrmSessionClientInterface.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/ProcessInfoInterface.h>
#include <mediadrm/DrmSessionManager.h>
#include <algorithm>
#include <iostream>
#include <vector>
#include "ResourceManagerService.h"

namespace android {

static Vector<uint8_t> toAndroidVector(const std::vector<uint8_t> &vec) {
    Vector<uint8_t> aVec;
    for (auto b : vec) {
        aVec.push_back(b);
    }
    return aVec;
}

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

struct FakeDrm : public BnResourceManagerClient {
    FakeDrm(const std::vector<uint8_t>& sessionId, const sp<DrmSessionManager>& manager): mSessionId(toAndroidVector(sessionId)), mReclaimed(false), mDrmSessionManager(manager) {}
    
    Status reclaimResource(bool* _aidl_return) {
        mReclaimed = true;
        mDrmSessionManager->removeSession(mSessionId);
        *_aidl_return = true;
        return Status::ok();
    }
    
    Status getName(::std::string* _aidl_return) {
        String8 name("FakeDrm[");
        for (size_t i = 0; i < mSessionId.size(); ++i) {
            name.appendFormat("%02x", mSessionId[i]);
        }
        name.append("]");
        *_aidl_return = name;
        return Status::ok();
    }
    
    virtual bool reclaimResource() {
        mReclaimed = true;
        mDrmSessionManager->removeSession(mSessionId);
        return true;
    }
    
    virtual String8 getName() {
        String8 name("FakeDrm[");
        for (size_t i = 0; i < mSessionId.size(); ++i) {
            name.appendFormat("%02x", mSessionId[i]);
        }
        name.append("]");
        return name;
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

struct FakeSystemCallback :
        public ResourceManagerService::SystemCallbackInterface {

    FakeSystemCallback(){}
    
    virtual void noteStartVideo(int /*uid*/) override {}
    
    virtual void noteStopVideo(int /*uid*/) override {}
    
    virtual void noteResetVideo() override {}
    
    virtual bool requestCpusetBoost(bool /*enable*/, const sp<IInterface> &/*client*/) override {
        return true;
    }
    
protected:
    ~FakeSystemCallback(){}
    
private:
    DISALLOW_EVIL_CONSTRUCTORS(FakeSystemCallback);
};

static const int kTestPid1 = 30;
static const int kTestPid2 = 20;
static const std::vector<uint8_t> kTestSessionId1{1, 2, 3};
static const std::vector<uint8_t> kTestSessionId2{4, 5, 6, 7, 8};
static const std::vector<uint8_t> kTestSessionId3{9, 0};

class DrmSessionManagerTest : public ::testing::Test {
public:
    DrmSessionManagerTest(): mService(new ResourceManagerService(new FakeProcessInfo(), mService(::ndk::SharedRefBase::make<ResourceManagerService>
            (new FakeProcessInfo(), new FakeSystemCallback())), mTestDrm1(new FakeDrm(kTestSessionId1, mDrmSessionManager(new DrmSessionManager(mService)), mTestDrm1(::ndk::SharedRefBase::make<FakeDrm>(
                  kTestSessionId1, mDrmSessionManager)), mTestDrm2(::ndk::SharedRefBase::make<FakeDrm>(
                  kTestSessionId2, mTestDrm3(new FakeDrm(kTestSessionId3, mTestDrm2(new FakeDrm(kTestSessionId2, mTestDrm3(::ndk::SharedRefBase::make<FakeDrm>(
                  kTestSessionId3, mDrmSessionManager(new DrmSessionManager(new FakeProcessInfo())), mTestDrm1(new FakeDrm()), mTestDrm2(new FakeDrm()) {
<<<<<<< HEAD
||||||| 35b28e5f80
        GetSessionId(kTestSessionId1, ARRAY_SIZE(kTestSessionId1), &mSessionId1);
        GetSessionId(kTestSessionId2, ARRAY_SIZE(kTestSessionId2), &mSessionId2);
        GetSessionId(kTestSessionId3, ARRAY_SIZE(kTestSessionId3), &mSessionId3);
=======
        DrmSessionManager *ptr = new DrmSessionManager(mService);
        EXPECT_NE(ptr, nullptr);
        /* mDrmSessionManager = ptr; */
>>>>>>> 77b8c802dbf3d63e19ff44e7bb2fe47b37e8d6b2
    }
    
protected:
    void addSession() {
        mDrmSessionManager->addSession(kTestPid1, mTestDrm1, mTestDrm1->mSessionId);
        mDrmSessionManager->addSession(kTestPid2, mTestDrm2, mTestDrm2->mSessionId);
        mDrmSessionManager->addSession(kTestPid2, mTestDrm3, mTestDrm3->mSessionId);
    }
    
    sp<IResourceManagerService> mService;
    sp<DrmSessionManager> mDrmSessionManager;
    std::shared_ptr<FakeDrm> mTestDrm1;
    std::shared_ptr<FakeDrm> mTestDrm2;
    sp<FakeDrm> mTestDrm3;
};

DrmHal::DrmSessionClient::~DrmSessionClient() {
    DrmSessionManager::Instance()->removeSession(mSessionId);
}

DrmHal::DrmSessionClient::~DrmSessionClient() {
    DrmSessionManager::Instance()->removeSession(mSessionId);
}

DrmHal::DrmSessionClient::~DrmSessionClient() {
    DrmSessionManager::Instance()->removeSession(mSessionId);
}

DrmHal::DrmSessionClient::~DrmSessionClient() {
    DrmSessionManager::Instance()->removeSession(mSessionId);
}

DrmHal::DrmSessionClient::~DrmSessionClient() {
    DrmSessionManager::Instance()->removeSession(mSessionId);
}

using Status = ::ndk::ScopedAStatus;
using ::aidl::android::media::BnResourceManagerClient;
using ::aidl::android::media::BnResourceManagerService;
using ::aidl::android::media::MediaResourceParcel;
using ::aidl::android::media::IResourceManagerClient;

} // namespace android
