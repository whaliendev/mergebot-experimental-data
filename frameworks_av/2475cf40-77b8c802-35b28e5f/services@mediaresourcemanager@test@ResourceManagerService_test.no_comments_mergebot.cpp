#define LOG_TAG "ResourceManagerService_test"
#include <utils/Log.h>
#include <gtest/gtest.h>
#include "ResourceManagerService.h"
#include <media/IResourceManagerService.h>
#include <aidl/android/media/BnResourceManagerClient.h>
#include <media/MediaResource.h>
#include <media/MediaResourcePolicy.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/ProcessInfoInterface.h>
namespace aidl {
namespace android {
namespace media {
bool operator== (const MediaResourceParcel& lhs, const MediaResourceParcel& rhs) {
    return lhs.type == rhs.type && lhs.subType == rhs.subType &&
            lhs.id == rhs.id && lhs.value == rhs.value;
}}
}
}
namespace android {
static int64_t getId(const std::shared_ptr<IResourceManagerClient>& client) {
    return (int64_t) client.get();
}
struct TestProcessInfo : public ProcessInfoInterface {
    TestProcessInfo() {}
    virtual ~TestProcessInfo() {}
    virtual bool getPriority(int pid, int *priority) {
        *priority = pid;
        return true;
    }
    virtual bool isValidPid(int ) {
        return true;
    }
private:
    DISALLOW_EVIL_CONSTRUCTORS(TestProcessInfo);
};
struct TestSystemCallback :
        public ResourceManagerService::SystemCallbackInterface {
    TestSystemCallback() :
        mLastEvent({}
    enum EventType {
        INVALID = -1,
        VIDEO_ON = 0,
        VIDEO_OFF = 1,
        VIDEO_RESET = 2,
        CPUSET_ENABLE = 3,
        CPUSET_DISABLE = 4,
    };
    struct EventEntry {
        EventType type;
        int arg;
    };
    virtual void noteStartVideo(int uid) override {
        mLastEvent = {EventType::VIDEO_ON, uid};
        mEventCount++;
    }
    virtual void noteStopVideo(int uid) override {
        mLastEvent = {EventType::VIDEO_OFF, uid};
        mEventCount++;
    }
    virtual void noteResetVideo() override {
        mLastEvent = {EventType::VIDEO_RESET, 0};
        mEventCount++;
    }
    virtual bool requestCpusetBoost(bool enable) override {
        mLastEvent = {enable ? EventType::CPUSET_ENABLE : EventType::CPUSET_DISABLE, 0};
        mEventCount++;
        return true;
    }
    size_t eventCount() { return mEventCount; }
    EventType lastEventType() { return mLastEvent.type; }
    EventEntry lastEvent() { return mLastEvent; }
protected:
    virtual ~TestSystemCallback() {}
private:
    EventEntry mLastEvent;
    size_t mEventCount;
    DISALLOW_EVIL_CONSTRUCTORS(TestSystemCallback);
};
struct TestClient : public BnResourceManagerClient {
    TestClient(int pid, const std::shared_ptr<ResourceManagerService> &service)
        : mReclaimed(false), mPid(pid), mService(service) {}
    bool reclaimed() const {
        return mReclaimed;
    }
    void reset() {
        mReclaimed = false;
    }
protected:
    virtual ~TestClient() {}
private:
    bool mReclaimed;
    int mPid;
    std::shared_ptr<ResourceManagerService> mService;
    DISALLOW_EVIL_CONSTRUCTORS(TestClient);
    Status reclaimResource(bool* _aidl_return) override {
        mService->removeClient(mPid, getId(ref<TestClient>()));
        mReclaimed = true;
        *_aidl_return = true;
        return Status::ok();
    }
    Status getName(::std::string* _aidl_return) override {
        *_aidl_return = "test_client";
        return Status::ok();
    }
};
static const int kTestPid1 = 30;
static const int kTestUid1 = 1010;
static const int kTestPid2 = 20;
static const int kTestUid2 = 1011;
static const int kLowPriorityPid = 40;
static const int kMidPriorityPid = 25;
static const int kHighPriorityPid = 10;
using EventType = TestSystemCallback::EventType;
using EventEntry = TestSystemCallback::EventEntry;
bool operator== (const EventEntry& lhs, const EventEntry& rhs) {
    return lhs.type == rhs.type && lhs.arg == rhs.arg;
}
class ResourceManagerServiceTest : public ::testing::Test {
public:
    ResourceManagerServiceTest()
        : mSystemCB(new TestSystemCallback()),
          mService(::ndk::SharedRefBase::make<ResourceManagerService>(
                  new TestProcessInfo, mSystemCB)),
          mTestClient1(::ndk::SharedRefBase::make<TestClient>(kTestPid1, mService)),
          mTestClient2(::ndk::SharedRefBase::make<TestClient>(kTestPid2, mService)),
          mTestClient3(::ndk::SharedRefBase::make<TestClient>(kTestPid2, mService)) {
    }
protected:
    static bool isEqualResources(const std::vector<MediaResourceParcel> &resources1,
            const ResourceList &resources2) {
        ResourceList r1;
        for (size_t i = 0; i < resources1.size(); ++i) {
<<<<<<< HEAD
            const auto &res = resources1[i];
            const auto resType = std::tuple(res.type, res.subType, res.id);
            r1[resType] = res;
||||||| 35b28e5f80
            const auto resType = std::make_pair(resources1[i].mType, resources1[i].mSubType);
            r1[resType] = resources1[i];
=======
            const auto &res = resources1[i];
            const auto resType = std::tuple(res.mType, res.mSubType, res.mId);
            r1[resType] = res;
>>>>>>> 77b8c802dbf3d63e19ff44e7bb2fe47b37e8d6b2
        }
        return r1 == resources2;
    }
    static void expectEqResourceInfo(const ResourceInfo &info,
            int uid,
            std::shared_ptr<IResourceManagerClient> client,
            const std::vector<MediaResourceParcel> &resources) {
        EXPECT_EQ(uid, info.uid);
        EXPECT_EQ(client, info.client);
        EXPECT_TRUE(isEqualResources(resources, info.resources));
    }
    void verifyClients(bool c1, bool c2, bool c3) {
        TestClient *client1 = static_cast<TestClient*>(mTestClient1.get());
        TestClient *client2 = static_cast<TestClient*>(mTestClient2.get());
        TestClient *client3 = static_cast<TestClient*>(mTestClient3.get());
        EXPECT_EQ(c1, client1->reclaimed());
        EXPECT_EQ(c2, client2->reclaimed());
        EXPECT_EQ(c3, client3->reclaimed());
        client1->reset();
        client2->reset();
        client3->reset();
    }
    void addResource() {
        std::vector<MediaResourceParcel> resources1;
        resources1.push_back(MediaResource(MediaResource::Type::kSecureCodec, 1));
        mService->addResource(kTestPid1, kTestUid1, getId(mTestClient1), mTestClient1, resources1);
        resources1.push_back(MediaResource(MediaResource::Type::kGraphicMemory, 200));
        std::vector<MediaResourceParcel> resources11;
        resources11.push_back(MediaResource(MediaResource::Type::kGraphicMemory, 200));
        mService->addResource(kTestPid1, kTestUid1, getId(mTestClient1), mTestClient1, resources11);
        std::vector<MediaResourceParcel> resources2;
        resources2.push_back(MediaResource(MediaResource::Type::kNonSecureCodec, 1));
        resources2.push_back(MediaResource(MediaResource::Type::kGraphicMemory, 300));
        mService->addResource(kTestPid2, kTestUid2, getId(mTestClient2), mTestClient2, resources2);
        std::vector<MediaResourceParcel> resources3;
        mService->addResource(kTestPid2, kTestUid2, getId(mTestClient3), mTestClient3, resources3);
        resources3.push_back(MediaResource(MediaResource::Type::kSecureCodec, 1));
        resources3.push_back(MediaResource(MediaResource::Type::kGraphicMemory, 100));
        mService->addResource(kTestPid2, kTestUid2, getId(mTestClient3), mTestClient3, resources3);
        const PidResourceInfosMap &map = mService->mMap;
        EXPECT_EQ(2u, map.size());
        ssize_t index1 = map.indexOfKey(kTestPid1);
        ASSERT_GE(index1, 0);
        const ResourceInfos &infos1 = map[index1];
        EXPECT_EQ(1u, infos1.size());
        expectEqResourceInfo(infos1.valueFor(getId(mTestClient1)), kTestUid1, mTestClient1, resources1);
        ssize_t index2 = map.indexOfKey(kTestPid2);
        ASSERT_GE(index2, 0);
        const ResourceInfos &infos2 = map[index2];
        EXPECT_EQ(2u, infos2.size());
        expectEqResourceInfo(infos2.valueFor(getId(mTestClient2)), kTestUid2, mTestClient2, resources2);
        expectEqResourceInfo(infos2.valueFor(getId(mTestClient3)), kTestUid2, mTestClient3, resources3);
    }
    void testCombineResourceWithNegativeValues() {
        std::vector<MediaResourceParcel> resources1;
        resources1.push_back(MediaResource(MediaResource::Type::kDrmSession, -100));
        resources1.push_back(MediaResource(MediaResource::Type::kNonSecureCodec, -100));
        mService->addResource(kTestPid1, kTestUid1, getId(mTestClient1), mTestClient1, resources1);
        const PidResourceInfosMap &map = mService->mMap;
        EXPECT_EQ(1u, map.size());
        ssize_t index1 = map.indexOfKey(kTestPid1);
        ASSERT_GE(index1, 0);
        const ResourceInfos &infos1 = map[index1];
        EXPECT_EQ(1u, infos1.size());
        std::vector<MediaResourceParcel> expected;
        expectEqResourceInfo(infos1.valueFor(getId(mTestClient1)), kTestUid1, mTestClient1, expected);
        resources1.clear();
        resources1.push_back(MediaResource(MediaResource::Type::kDrmSession, INT64_MAX));
        resources1.push_back(MediaResource(MediaResource::Type::kNonSecureCodec, INT64_MAX));
        mService->addResource(kTestPid1, kTestUid1, getId(mTestClient1), mTestClient1, resources1);
        resources1.clear();
        resources1.push_back(MediaResource(MediaResource::Type::kDrmSession, 10));
        resources1.push_back(MediaResource(MediaResource::Type::kNonSecureCodec, 10));
        mService->addResource(kTestPid1, kTestUid1, getId(mTestClient1), mTestClient1, resources1);
        expected.push_back(MediaResource(MediaResource::Type::kDrmSession, INT64_MAX));
        expected.push_back(MediaResource(MediaResource::Type::kNonSecureCodec, INT64_MAX));
        expectEqResourceInfo(infos1.valueFor(getId(mTestClient1)), kTestUid1, mTestClient1, expected);
        resources1.clear();
        resources1.push_back(MediaResource(MediaResource::Type::kDrmSession, -10));
        resources1.push_back(MediaResource(MediaResource::Type::kNonSecureCodec, -10));
        mService->addResource(kTestPid1, kTestUid1, getId(mTestClient1), mTestClient1, resources1);
        expected.push_back(MediaResource(MediaResource::Type::kDrmSession, INT64_MAX - 10));
        expected.push_back(MediaResource(MediaResource::Type::kNonSecureCodec, INT64_MAX));
        expectEqResourceInfo(infos1.valueFor(getId(mTestClient1)), kTestUid1, mTestClient1, expected);
        resources1.clear();
        resources1.push_back(MediaResource(MediaResource::Type::kDrmSession, INT64_MIN));
        expected.push_back(MediaResource(MediaResource::Type::kNonSecureCodec, INT64_MIN));
        mService->addResource(kTestPid1, kTestUid1, getId(mTestClient1), mTestClient1, resources1);
        expected.clear();
        expected.push_back(MediaResource(MediaResource::Type::kDrmSession, 0));
        expected.push_back(MediaResource(MediaResource::Type::kNonSecureCodec, INT64_MAX));
        expectEqResourceInfo(infos1.valueFor(getId(mTestClient1)), kTestUid1, mTestClient1, expected);
    }
    void testConfig() {
        EXPECT_TRUE(mService->mSupportsMultipleSecureCodecs);
        EXPECT_TRUE(mService->mSupportsSecureWithNonSecureCodec);
        std::vector<MediaResourcePolicyParcel> policies1;
        policies1.push_back(
                MediaResourcePolicy(
                        IResourceManagerService::kPolicySupportsMultipleSecureCodecs,
                        "true"));
        policies1.push_back(
                MediaResourcePolicy(
                        IResourceManagerService::kPolicySupportsSecureWithNonSecureCodec,
                        "false"));
        mService->config(policies1);
        EXPECT_TRUE(mService->mSupportsMultipleSecureCodecs);
        EXPECT_FALSE(mService->mSupportsSecureWithNonSecureCodec);
        std::vector<MediaResourcePolicyParcel> policies2;
        policies2.push_back(
                MediaResourcePolicy(
                        IResourceManagerService::kPolicySupportsMultipleSecureCodecs,
                        "false"));
        policies2.push_back(
                MediaResourcePolicy(
                        IResourceManagerService::kPolicySupportsSecureWithNonSecureCodec,
                        "true"));
        mService->config(policies2);
        EXPECT_FALSE(mService->mSupportsMultipleSecureCodecs);
        EXPECT_TRUE(mService->mSupportsSecureWithNonSecureCodec);
    }
    void testCombineResource() {
        std::vector<MediaResourceParcel> resources1;
        resources1.push_back(MediaResource(MediaResource::Type::kSecureCodec, 1));
        mService->addResource(kTestPid1, kTestUid1, getId(mTestClient1), mTestClient1, resources1);
        std::vector<MediaResourceParcel> resources11;
        resources11.push_back(MediaResource(MediaResource::Type::kGraphicMemory, 200));
        mService->addResource(kTestPid1, kTestUid1, getId(mTestClient1), mTestClient1, resources11);
        const PidResourceInfosMap &map = mService->mMap;
        EXPECT_EQ(1u, map.size());
        ssize_t index1 = map.indexOfKey(kTestPid1);
        ASSERT_GE(index1, 0);
        const ResourceInfos &infos1 = map[index1];
        EXPECT_EQ(1u, infos1.size());
        resources1.push_back(MediaResource(MediaResource::Type::kGraphicMemory, 100));
        mService->addResource(kTestPid1, kTestUid1, getId(mTestClient1), mTestClient1, resources1);
        std::vector<MediaResourceParcel> expected;
        expected.push_back(MediaResource(MediaResource::Type::kSecureCodec, 2));
        expected.push_back(MediaResource(MediaResource::Type::kGraphicMemory, 300));
        expectEqResourceInfo(infos1.valueFor(getId(mTestClient1)), kTestUid1, mTestClient1, expected);
        resources11.push_back(MediaResource(MediaResource::Type::kNonSecureCodec, 1));
        resources11.push_back(MediaResource(MediaResource::Type::kSecureCodec, MediaResource::SubType::kVideoCodec, 1));
        mService->addResource(kTestPid1, kTestUid1, getId(mTestClient1), mTestClient1, resources11);
        expected.clear();
        expected.push_back(MediaResource(MediaResource::Type::kSecureCodec, 2));
        expected.push_back(MediaResource(MediaResource::Type::kNonSecureCodec, 1));
        expected.push_back(MediaResource(MediaResource::Type::kSecureCodec, MediaResource::SubType::kVideoCodec, 1));
        expected.push_back(MediaResource(MediaResource::Type::kGraphicMemory, 500));
        expectEqResourceInfo(infos1.valueFor(getId(mTestClient1)), kTestUid1, mTestClient1, expected);
    }
    void testRemoveResource() {
        std::vector<MediaResourceParcel> resources1;
        resources1.push_back(MediaResource(MediaResource::Type::kSecureCodec, 1));
        mService->addResource(kTestPid1, kTestUid1, getId(mTestClient1), mTestClient1, resources1);
        std::vector<MediaResourceParcel> resources11;
        resources11.push_back(MediaResource(MediaResource::Type::kGraphicMemory, 200));
        mService->addResource(kTestPid1, kTestUid1, getId(mTestClient1), mTestClient1, resources11);
        const PidResourceInfosMap &map = mService->mMap;
        EXPECT_EQ(1u, map.size());
        ssize_t index1 = map.indexOfKey(kTestPid1);
        ASSERT_GE(index1, 0);
        const ResourceInfos &infos1 = map[index1];
        EXPECT_EQ(1u, infos1.size());
        resources11[0].value = 100;
        mService->removeResource(kTestPid1, getId(mTestClient1), resources11);
        std::vector<MediaResourceParcel> expected;
        expected.push_back(MediaResource(MediaResource::Type::kSecureCodec, 1));
        expected.push_back(MediaResource(MediaResource::Type::kGraphicMemory, 100));
        expectEqResourceInfo(infos1.valueFor(getId(mTestClient1)), kTestUid1, mTestClient1, expected);
        resources11[0].value = -10000;
        mService->removeResource(kTestPid1, getId(mTestClient1), resources11);
        expectEqResourceInfo(infos1.valueFor(getId(mTestClient1)), kTestUid1, mTestClient1, expected);
        resources11[0].value = 1000;
        mService->removeResource(kTestPid1, getId(mTestClient1), resources11);
        expected.clear();
        expected.push_back(MediaResource(MediaResource::Type::kSecureCodec, 1));
        expectEqResourceInfo(infos1.valueFor(getId(mTestClient1)), kTestUid1, mTestClient1, expected);
    }
    void testRemoveClient() {
        addResource();
        mService->removeClient(kTestPid2, getId(mTestClient2));
        const PidResourceInfosMap &map = mService->mMap;
        EXPECT_EQ(2u, map.size());
        const ResourceInfos &infos1 = map.valueFor(kTestPid1);
        const ResourceInfos &infos2 = map.valueFor(kTestPid2);
        EXPECT_EQ(1u, infos1.size());
        EXPECT_EQ(1u, infos2.size());
        EXPECT_EQ(mTestClient3, infos2[0].client);
    }
    void testGetAllClients() {
        addResource();
        MediaResource::Type type = MediaResource::Type::kSecureCodec;
        Vector<std::shared_ptr<IResourceManagerClient> > clients;
        EXPECT_FALSE(mService->getAllClients_l(kLowPriorityPid, type, &clients));
        EXPECT_FALSE(mService->getAllClients_l(kMidPriorityPid, type, &clients));
        EXPECT_TRUE(mService->getAllClients_l(kHighPriorityPid, type, &clients));
        EXPECT_EQ(2u, clients.size());
        EXPECT_EQ(mTestClient3, clients[0]);
        EXPECT_EQ(mTestClient1, clients[1]);
    }
    void testReclaimResourceSecure() {
        bool result;
        std::vector<MediaResourceParcel> resources;
        resources.push_back(MediaResource(MediaResource::Type::kSecureCodec, 1));
        resources.push_back(MediaResource(MediaResource::Type::kGraphicMemory, 150));
        {
            addResource();
            mService->mSupportsMultipleSecureCodecs = false;
            mService->mSupportsSecureWithNonSecureCodec = true;
            CHECK_STATUS_FALSE(mService->reclaimResource(kLowPriorityPid, resources, &result));
            CHECK_STATUS_FALSE(mService->reclaimResource(kMidPriorityPid, resources, &result));
            CHECK_STATUS_TRUE(mService->reclaimResource(kHighPriorityPid, resources, &result));
            verifyClients(true , false , true );
            CHECK_STATUS_TRUE(mService->reclaimResource(kHighPriorityPid, resources, &result));
            verifyClients(false , true , false );
            CHECK_STATUS_FALSE(mService->reclaimResource(kHighPriorityPid, resources, &result));
        }
        {
            addResource();
            mService->mSupportsMultipleSecureCodecs = false;
            mService->mSupportsSecureWithNonSecureCodec = false;
            CHECK_STATUS_FALSE(mService->reclaimResource(kLowPriorityPid, resources, &result));
            CHECK_STATUS_FALSE(mService->reclaimResource(kMidPriorityPid, resources, &result));
            CHECK_STATUS_TRUE(mService->reclaimResource(kHighPriorityPid, resources, &result));
            verifyClients(true , true , true );
            CHECK_STATUS_FALSE(mService->reclaimResource(kHighPriorityPid, resources, &result));
        }
        {
            addResource();
            mService->mSupportsMultipleSecureCodecs = true;
            mService->mSupportsSecureWithNonSecureCodec = false;
            CHECK_STATUS_FALSE(mService->reclaimResource(kLowPriorityPid, resources, &result));
            CHECK_STATUS_FALSE(mService->reclaimResource(kMidPriorityPid, resources, &result));
            CHECK_STATUS_TRUE(mService->reclaimResource(kHighPriorityPid, resources, &result));
            verifyClients(false , true , false );
            CHECK_STATUS_TRUE(mService->reclaimResource(kHighPriorityPid, resources, &result));
            verifyClients(true , false , false );
            CHECK_STATUS_TRUE(mService->reclaimResource(kHighPriorityPid, resources, &result));
            verifyClients(false , false , true );
            CHECK_STATUS_FALSE(mService->reclaimResource(kHighPriorityPid, resources, &result));
        }
        {
            addResource();
            mService->mSupportsMultipleSecureCodecs = true;
            mService->mSupportsSecureWithNonSecureCodec = true;
            CHECK_STATUS_FALSE(mService->reclaimResource(kLowPriorityPid, resources, &result));
            CHECK_STATUS_TRUE(mService->reclaimResource(kHighPriorityPid, resources, &result));
            verifyClients(true , false , false );
            CHECK_STATUS_TRUE(mService->reclaimResource(kHighPriorityPid, resources, &result));
            verifyClients(false , true , false );
            CHECK_STATUS_TRUE(mService->reclaimResource(kHighPriorityPid, resources, &result));
            verifyClients(false , false , true );
            CHECK_STATUS_FALSE(mService->reclaimResource(kHighPriorityPid, resources, &result));
        }
        {
            addResource();
            mService->mSupportsMultipleSecureCodecs = true;
            mService->mSupportsSecureWithNonSecureCodec = true;
            std::vector<MediaResourceParcel> resources;
            resources.push_back(MediaResource(MediaResource::Type::kSecureCodec, 1));
            CHECK_STATUS_TRUE(mService->reclaimResource(kHighPriorityPid, resources, &result));
            verifyClients(true , false , false );
            CHECK_STATUS_TRUE(mService->reclaimResource(kHighPriorityPid, resources, &result));
            verifyClients(false , false , true );
            CHECK_STATUS_TRUE(mService->reclaimResource(kHighPriorityPid, resources, &result));
            verifyClients(false , true , false );
        }
    }
    void testReclaimResourceNonSecure() {
        bool result;
        std::vector<MediaResourceParcel> resources;
        resources.push_back(MediaResource(MediaResource::Type::kNonSecureCodec, 1));
        resources.push_back(MediaResource(MediaResource::Type::kGraphicMemory, 150));
        {
            addResource();
            mService->mSupportsSecureWithNonSecureCodec = false;
            CHECK_STATUS_FALSE(mService->reclaimResource(kLowPriorityPid, resources, &result));
            CHECK_STATUS_FALSE(mService->reclaimResource(kMidPriorityPid, resources, &result));
            CHECK_STATUS_TRUE(mService->reclaimResource(kHighPriorityPid, resources, &result));
            verifyClients(true , false , true );
            CHECK_STATUS_TRUE(mService->reclaimResource(kHighPriorityPid, resources, &result));
            verifyClients(false , true , false );
            CHECK_STATUS_FALSE(mService->reclaimResource(kHighPriorityPid, resources, &result));
        }
        {
            addResource();
            mService->mSupportsSecureWithNonSecureCodec = true;
            CHECK_STATUS_FALSE(mService->reclaimResource(kLowPriorityPid, resources, &result));
            CHECK_STATUS_TRUE(mService->reclaimResource(kHighPriorityPid, resources, &result));
            verifyClients(true , false , false );
            CHECK_STATUS_TRUE(mService->reclaimResource(kHighPriorityPid, resources, &result));
            verifyClients(false , true , false );
            CHECK_STATUS_TRUE(mService->reclaimResource(kHighPriorityPid, resources, &result));
            verifyClients(false , false , true );
            CHECK_STATUS_FALSE(mService->reclaimResource(kHighPriorityPid, resources, &result));
        }
        {
            addResource();
            mService->mSupportsSecureWithNonSecureCodec = true;
            std::vector<MediaResourceParcel> resources;
            resources.push_back(MediaResource(MediaResource::Type::kNonSecureCodec, 1));
            CHECK_STATUS_TRUE(mService->reclaimResource(kHighPriorityPid, resources, &result));
            verifyClients(false , true , false );
            CHECK_STATUS_TRUE(mService->reclaimResource(kHighPriorityPid, resources, &result));
            verifyClients(true , false , false );
            mService->removeClient(kTestPid2, getId(mTestClient3));
        }
    }
    void testGetLowestPriorityBiggestClient() {
        MediaResource::Type type = MediaResource::Type::kGraphicMemory;
        std::shared_ptr<IResourceManagerClient> client;
        EXPECT_FALSE(mService->getLowestPriorityBiggestClient_l(kHighPriorityPid, type, &client));
        addResource();
        EXPECT_FALSE(mService->getLowestPriorityBiggestClient_l(kLowPriorityPid, type, &client));
        EXPECT_TRUE(mService->getLowestPriorityBiggestClient_l(kHighPriorityPid, type, &client));
        EXPECT_EQ(mTestClient1, client);
    }
    void testGetLowestPriorityPid() {
        int pid;
        int priority;
        TestProcessInfo processInfo;
        MediaResource::Type type = MediaResource::Type::kGraphicMemory;
        EXPECT_FALSE(mService->getLowestPriorityPid_l(type, &pid, &priority));
        addResource();
        EXPECT_TRUE(mService->getLowestPriorityPid_l(type, &pid, &priority));
        EXPECT_EQ(kTestPid1, pid);
        int priority1;
        processInfo.getPriority(kTestPid1, &priority1);
        EXPECT_EQ(priority1, priority);
        type = MediaResource::Type::kNonSecureCodec;
        EXPECT_TRUE(mService->getLowestPriorityPid_l(type, &pid, &priority));
        EXPECT_EQ(kTestPid2, pid);
        int priority2;
        processInfo.getPriority(kTestPid2, &priority2);
        EXPECT_EQ(priority2, priority);
    }
    void testGetBiggestClient() {
        MediaResource::Type type = MediaResource::Type::kGraphicMemory;
        std::shared_ptr<IResourceManagerClient> client;
        EXPECT_FALSE(mService->getBiggestClient_l(kTestPid2, type, &client));
        addResource();
        EXPECT_TRUE(mService->getBiggestClient_l(kTestPid2, type, &client));
        EXPECT_EQ(mTestClient2, client);
    }
    void testIsCallingPriorityHigher() {
        EXPECT_FALSE(mService->isCallingPriorityHigher_l(101, 100));
        EXPECT_FALSE(mService->isCallingPriorityHigher_l(100, 100));
        EXPECT_TRUE(mService->isCallingPriorityHigher_l(99, 100));
    }
    void testBatteryStats() {
        EXPECT_EQ(1u, mSystemCB->eventCount());
        EXPECT_EQ(EventType::VIDEO_RESET, mSystemCB->lastEventType());
        std::vector<MediaResourceParcel> resources1;
        resources1.push_back(MediaResource(MediaResource::Type::kBattery, MediaResource::SubType::kVideoCodec, 1));
        mService->addResource(kTestPid1, kTestUid1, getId(mTestClient1), mTestClient1, resources1);
        EXPECT_EQ(2u, mSystemCB->eventCount());
        EXPECT_EQ(EventEntry({EventType::VIDEO_ON, kTestUid1}), mSystemCB->lastEvent());
        mService->addResource(kTestPid1, kTestUid1, getId(mTestClient1), mTestClient1, resources1);
        EXPECT_EQ(2u, mSystemCB->eventCount());
        std::vector<MediaResourceParcel> resources2;
        resources2.push_back(MediaResource(MediaResource::Type::kBattery, MediaResource::SubType::kVideoCodec, 2));
        mService->addResource(kTestPid2, kTestUid2, getId(mTestClient2), mTestClient2, resources2);
        EXPECT_EQ(3u, mSystemCB->eventCount());
        EXPECT_EQ(EventEntry({EventType::VIDEO_ON, kTestUid2}), mSystemCB->lastEvent());
        mService->removeResource(kTestPid1, getId(mTestClient1), resources1);
        EXPECT_EQ(3u, mSystemCB->eventCount());
        mService->removeResource(kTestPid1, getId(mTestClient1), resources2);
        EXPECT_EQ(4u, mSystemCB->eventCount());
        EXPECT_EQ(EventEntry({EventType::VIDEO_OFF, kTestUid1}), mSystemCB->lastEvent());
        mService->removeClient(kTestPid2, getId(mTestClient2));
        EXPECT_EQ(5u, mSystemCB->eventCount());
        EXPECT_EQ(EventEntry({EventType::VIDEO_OFF, kTestUid2}), mSystemCB->lastEvent());
    }
    void testCpusetBoost() {
        EXPECT_EQ(1u, mSystemCB->eventCount());
        EXPECT_EQ(EventType::VIDEO_RESET, mSystemCB->lastEventType());
        std::vector<MediaResourceParcel> resources1;
        resources1.push_back(MediaResource(MediaResource::Type::kCpuBoost, 1));
        mService->addResource(kTestPid1, kTestUid1, getId(mTestClient1), mTestClient1, resources1);
        EXPECT_EQ(2u, mSystemCB->eventCount());
        EXPECT_EQ(EventType::CPUSET_ENABLE, mSystemCB->lastEventType());
        mService->addResource(kTestPid1, kTestUid1, getId(mTestClient1), mTestClient1, resources1);
        EXPECT_EQ(2u, mSystemCB->eventCount());
        std::vector<MediaResourceParcel> resources2;
        resources2.push_back(MediaResource(MediaResource::Type::kCpuBoost, 2));
        mService->addResource(kTestPid2, kTestUid2, getId(mTestClient2), mTestClient2, resources2);
        EXPECT_EQ(3u, mSystemCB->eventCount());
        EXPECT_EQ(EventType::CPUSET_ENABLE, mSystemCB->lastEventType());
        mService->removeClient(kTestPid2, getId(mTestClient2));
        EXPECT_EQ(3u, mSystemCB->eventCount());
        mService->removeResource(kTestPid1, getId(mTestClient1), resources1);
        EXPECT_EQ(3u, mSystemCB->eventCount());
        mService->removeResource(kTestPid1, getId(mTestClient1), resources2);
        EXPECT_EQ(4u, mSystemCB->eventCount());
        EXPECT_EQ(EventType::CPUSET_DISABLE, mSystemCB->lastEventType());
    }
    sp<TestSystemCallback> mSystemCB;
    std::shared_ptr<ResourceManagerService> mService;
    std::shared_ptr<IResourceManagerClient> mTestClient1;
    std::shared_ptr<IResourceManagerClient> mTestClient2;
    std::shared_ptr<IResourceManagerClient> mTestClient3;
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
DrmHal::DrmSessionClient::~DrmSessionClient() {
    DrmSessionManager::Instance()->removeSession(mSessionId);
}
DrmHal::DrmSessionClient::~DrmSessionClient() {
    DrmSessionManager::Instance()->removeSession(mSessionId);
}
DrmHal::DrmSessionClient::~DrmSessionClient() {
    DrmSessionManager::Instance()->removeSession(mSessionId);
}
using ::aidl::android::media::IResourceManagerService;
}
