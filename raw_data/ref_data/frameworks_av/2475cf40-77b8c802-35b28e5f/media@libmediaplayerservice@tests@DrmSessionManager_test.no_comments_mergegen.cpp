#define LOG_TAG "DrmSessionManager_test"
#include <android/binder_auto_utils.h>
#include <utils/Log.h>
#include <gtest/gtest.h>
#include <media/IResourceManagerService.h> #include <media/IResourceManagerClient.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/ProcessInfoInterface.h>
#include <mediadrm/DrmSessionManager.h>
#include <algorithm> #include <iostream> #include <vector> #include "ResourceManagerService.h"
namespace android {
using Status = ::ndk::ScopedAStatus; using ::aidl::android::media::BnResourceManagerClient; using ::aidl::android::media::BnResourceManagerService; using ::aidl::android::media::MediaResourceParcel; using ::aidl::android::media::IResourceManagerClient;
struct FakeProcessInfo : public ProcessInfoInterface {
    FakeProcessInfo() {}
    virtual ~FakeProcessInfo() {}
    virtual bool getPriority(int pid, int* priority) {
        *priority = pid;
        return true;
    }
    virtual bool isValidPid(int ) {
        return true;
    }
private:
    DISALLOW_EVIL_CONSTRUCTORS(FakeProcessInfo);
};
struct FakeDrm : public BnResourceManagerClient { FakeDrm(const std::vector<uint8_t>& sessionId, const sp<DrmSessionManager>& manager) : mSessionId(toAndroidVector(sessionId)), mReclaimed(false), mDrmSessionManager(manager) {}
Status reclaimResource(bool* _aidl_return) { mReclaimed = true; mDrmSessionManager->removeSession(mSessionId); *_aidl_return = true;
    }
Status getName(::std::string* _aidl_return) { String8 name("FakeDrm["); for (size_t i = 0; i < mSessionId.size(); ++i) { name.appendFormat("%02x", mSessionId[i]); } name.append("]"); *_aidl_return = name;
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
struct FakeSystemCallback : public ResourceManagerService::SystemCallbackInterface { FakeSystemCallback() {} virtual void noteStartVideo(int ) override {} virtual void noteStopVideo(int ) override {} virtual void noteResetVideo() override {} virtual bool requestCpusetBoost(bool ) override { return true; } protected: virtual ~FakeSystemCallback() {} private: DISALLOW_EVIL_CONSTRUCTORS(FakeSystemCallback); };
static const int kTestPid1 = 30;
static const int kTestPid2 = 20;
static const std::vector<uint8_t> kTestSessionId1{1, 2, 3};
static const std::vector<uint8_t> kTestSessionId2{4, 5, 6, 7, 8};
static const std::vector<uint8_t> kTestSessionId3{9, 0};
class DrmSessionManagerTest : public ::testing::Test {
public:
    DrmSessionManagerTest()
DrmSessionManagerTest() : mService(::ndk::SharedRefBase::make<ResourceManagerService> (new FakeProcessInfo(), new FakeSystemCallback())), mDrmSessionManager(new DrmSessionManager(mService)), mTestDrm1(::ndk::SharedRefBase::make<FakeDrm>( kTestSessionId1, mDrmSessionManager)), mTestDrm2(::ndk::SharedRefBase::make<FakeDrm>( kTestSessionId3, mDrmSessionManager)), mTestDrm3(::ndk::SharedRefBase::make<FakeDrm>( kTestSessionId3, mDrmSessionManager)), mTestDrm3(::ndk::SharedRefBase::make<FakeDrm>( kTestSessionId3, mDrmSessionManager)) { DrmSessionManager *ptr = new DrmSessionManager(mService); EXPECT_NE(ptr, nullptr);
    }
protected:
    void addSession() {
        mDrmSessionManager->addSession(kTestPid1, mTestDrm1, mTestDrm1->mSessionId);
        mDrmSessionManager->addSession(kTestPid2, mTestDrm2, mTestDrm2->mSessionId);
        mDrmSessionManager->addSession(kTestPid2, mTestDrm3, mTestDrm3->mSessionId);
    }
std::shared_ptr<ResourceManagerService> mService; sp<IResourceManagerService> mService;
    sp<DrmSessionManager> mDrmSessionManager;
std::shared_ptr<FakeDrm> mTestDrm1; std::shared_ptr<FakeDrm> mTestDrm2; std::shared_ptr<FakeDrm> mTestDrm3;
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
    EXPECT_FALSE(mDrmSessionManager->reclaimSession(50));
    EXPECT_TRUE(mDrmSessionManager->reclaimSession(10));
    EXPECT_TRUE(mTestDrm1->isReclaimed());
std::shared_ptr<FakeDrm> drm = ::ndk::SharedRefBase::make<FakeDrm>(sid, mDrmSessionManager); mDrmSessionManager->addSession(15, drm, drm->mSessionId);
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
    EXPECT_FALSE(mDrmSessionManager->reclaimSession(kTestPid1));
    EXPECT_FALSE(mDrmSessionManager->reclaimSession(kTestPid2));
    mDrmSessionManager->addSession(kTestPid2, mTestDrm1, mTestDrm1->mSessionId);
    mDrmSessionManager->addSession(kTestPid2, mTestDrm2, mTestDrm2->mSessionId);
    mDrmSessionManager->addSession(kTestPid2, mTestDrm3, mTestDrm3->mSessionId);
    mDrmSessionManager->useSession(mTestDrm1->mSessionId);
    mDrmSessionManager->useSession(mTestDrm1->mSessionId);
    mDrmSessionManager->useSession(mTestDrm2->mSessionId);
    int lowPriorityPid = kTestPid2 + 1;
    EXPECT_FALSE(mDrmSessionManager->reclaimSession(lowPriorityPid));
    int highPriorityPid = kTestPid2 - 1;
    EXPECT_TRUE(mDrmSessionManager->reclaimSession(highPriorityPid));
    EXPECT_FALSE(mTestDrm1->isReclaimed());
    EXPECT_FALSE(mTestDrm2->isReclaimed());
    EXPECT_TRUE(mTestDrm3->isReclaimed());
    mDrmSessionManager->removeSession(mTestDrm3->mSessionId);
    EXPECT_TRUE(mDrmSessionManager->reclaimSession(highPriorityPid));
    EXPECT_FALSE(mTestDrm1->isReclaimed());
    EXPECT_TRUE(mTestDrm2->isReclaimed());
    EXPECT_TRUE(mTestDrm3->isReclaimed());
    EXPECT_EQ(1u, mDrmSessionManager->getSessionCount());
    EXPECT_TRUE(mDrmSessionManager->containsSession(mTestDrm1->mSessionId));
    EXPECT_FALSE(mDrmSessionManager->containsSession(mTestDrm2->mSessionId));
    EXPECT_FALSE(mDrmSessionManager->containsSession(mTestDrm3->mSessionId));
}
}
