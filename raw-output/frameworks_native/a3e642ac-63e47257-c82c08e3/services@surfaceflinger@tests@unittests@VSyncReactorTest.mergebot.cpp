/*
 * Copyright 2019 The Android Open Source Project
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

#undef LOG_TAG
#define LOG_TAG "LibSurfaceFlingerUnittests"
#define LOG_NDEBUG 0
#include "Scheduler/TimeKeeper.h"
#include "Scheduler/VSyncDispatch.h"
#include "Scheduler/VSyncReactor.h"
#include "Scheduler/VSyncTracker.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <ui/Fence.h>
#include <ui/FenceTime.h>
#include <array>
using namespace testing;
using namespace std::literals;

namespace android::scheduler {

class MockVSyncTracker : public VSyncTracker {
public:
    MockVSyncTracker() { ON_CALL(*this, addVsyncTimestamp(_)).WillByDefault(Return(true)); }
    MOCK_METHOD1(addVsyncTimestamp, bool(nsecs_t));
    MOCK_CONST_METHOD1(dump, void(std::string&));
    MOCK_CONST_METHOD0(currentPeriod, nsecs_t());
    MOCK_METHOD1(setPeriod, void(nsecs_t));
    MOCK_METHOD0(resetModel, void());
    MOCK_CONST_METHOD0(needsMoreSamples, bool());
    MOCK_CONST_METHOD1(dump, void(std::string&));
};

class MockClock : public Clock {
public:
    MOCK_CONST_METHOD0(now, nsecs_t());
};

class ClockWrapper : public Clock {
public:
    ClockWrapper(std::shared_ptr<Clock> const& clock) : mClock(clock) {}
    
    nsecs_t now() const { return mClock->now(){ return mClock->now(); }
    
private:
    std::shared_ptr<Clock> const mClock;
};

class MockVSyncDispatch : public VSyncDispatch {
public:
    MOCK_METHOD2(registerCallback,
                 CallbackToken(std::function<void(nsecs_t, nsecs_t)> const&, std::string));
    MOCK_METHOD1(unregisterCallback, void(CallbackToken));
    MOCK_METHOD3(schedule, ScheduleResult(CallbackToken, nsecs_t, nsecs_t));
    MOCK_METHOD1(cancel, CancelResult(CallbackToken token));
    MOCK_CONST_METHOD1(dump, void(std::string&));
};

std::shared_ptr<FenceTime> generateInvalidFence() {
    sp<Fence> fence = new Fence();
    return std::make_shared<FenceTime>(fence);
}

std::shared_ptr<FenceTime> generatePendingFence() {
    sp<Fence> fence = new Fence(dup(fileno(tmpfile())));
    return std::make_shared<FenceTime>(fence);
}

void signalFenceWithTime(std::shared_ptr<FenceTime> const& fence, nsecs_t time) {
    FenceTime::Snapshot snap(time);
    fence->applyTrustedSnapshot(snap);
}

public:
    std::optional<nsecs_t> lastCallTime() const {
        std::lock_guard<std::mutex> lk(mMutex);
        return mLastCallTime;
    }
    
class StubCallback : public DispSync::Callback {
public:
    std::optional<nsecs_t> lastCallTime() const {
        std::lock_guard<std::mutex> lk(mMutex);
        return mLastCallTime;
    }
    
    std::optional<nsecs_t> lastCallTime() const {
        std::lock_guard<std::mutex> lk(mMutex);
        return mLastCallTime;
    }
    
private:
                                         GUARDED_BY(mMutex);
                                         GUARDED_BY(mMutex);
                                         GUARDED_BY(mMutex);
};

class VSyncReactorTest : public testing::Test {
protected:
    VSyncReactorTest()
          : mMockDispatch(std::make_shared<NiceMock<MockVSyncDispatch>>()),
            mMockTracker(std::make_shared<NiceMock<MockVSyncTracker>>()),
            mMockClock(std::make_shared<NiceMock<MockClock>>()),
            mReactor(std::make_unique<ClockWrapper>(mMockClock), *mMockDispatch, *mMockTracker,
                     kPendingLimit, false /* supportKernelIdleTimer */) {
        ON_CALL(*mMockClock, now()).WillByDefault(Return(mFakeNow));
        ON_CALL(*mMockTracker, currentPeriod()).WillByDefault(Return(period));
    }
    
    std::shared_ptr<MockVSyncDispatch> mMockDispatch;
    std::shared_ptr<MockVSyncTracker> mMockTracker;
    std::shared_ptr<MockClock> mMockClock;
    static constexpr size_t kPendingLimit = 3;
    static constexpr nsecs_t mDummyTime = 47;
    static constexpr nsecs_t mPhase = 3000;
    static constexpr nsecs_t mAnotherPhase = 5200;
    static constexpr nsecs_t period = 10000;
    static constexpr nsecs_t mFakeVSyncTime = 2093;
    static constexpr nsecs_t mFakeWakeupTime = 1892;
    static constexpr nsecs_t mFakeNow = 2214;
    static constexpr const char mName[] = "callbacky";
    VSyncDispatch::CallbackToken const mFakeToken{2398};
    
    nsecs_t lastCallbackTime = 0;
    StubCallback outerCb;
    std::function<void(nsecs_t, nsecs_t)> innerCb;
    
    VSyncReactor mReactor;
};

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

static nsecs_t computeWorkload(nsecs_t period, nsecs_t phase) {
    return period - phase;
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

using VSyncReactorDeathTest = VSyncReactorTest;
TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

TEST_F(VSyncReactorDeathTest, cannotScheduleOnCallback) {
    EXPECT_CALL(*mMockDispatch, registerCallback(_, std::string(mName)))
            .WillOnce(DoAll(SaveArg<0>(&innerCb), Return(mFakeToken)));
    EXPECT_CALL(*mMockDispatch, schedule(_, _, _)).WillOnce(Return(ScheduleResult::Scheduled));

    mReactor.addEventListener(mName, mPhase, &outerCb, lastCallbackTime);
    ASSERT_TRUE(innerCb);
    Mock::VerifyAndClearExpectations(mMockDispatch.get());

    ON_CALL(*mMockDispatch, schedule(_, _, _))
            .WillByDefault(Return(ScheduleResult::CannotSchedule));
    EXPECT_DEATH(innerCb(mFakeVSyncTime, mFakeWakeupTime), ".*");
}

} // namespace android::scheduler
