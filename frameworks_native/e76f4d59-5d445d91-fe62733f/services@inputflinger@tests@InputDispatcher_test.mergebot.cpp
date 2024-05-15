/*
 * Copyright (C) 2010 The Android Open Source Project
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

#include "../dispatcher/InputDispatcher.h"
#include <android-base/properties.h>
#include <android-base/silent_death_test.h>
#include <android-base/stringprintf.h>
#include <android-base/thread_annotations.h>
#include <binder/Binder.h>
#include <fcntl.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <input/Input.h>
#include <linux/input.h>
#include <sys/epoll.h>
#include <cinttypes>
#include <compare>
#include <thread>
#include <unordered_set>
#include <vector>
using android::base::StringPrintf;
using android::gui::FocusRequest;
using android::gui::TouchOcclusionMode;
using android::gui::WindowInfo;
using android::gui::WindowInfoHandle;
using android::os::InputEventInjectionResult;
using android::os::InputEventInjectionSync;

namespace android::inputdispatcher {

using namespace ftl::flag_operators;

using testing::AllOf;

// An arbitrary time value.
static constexpr nsecs_t ARBITRARY_TIME = 1234;

// An arbitrary device id.
static constexpr int32_t DEVICE_ID = 1;

// An arbitrary display id.
static constexpr int32_t DISPLAY_ID = ADISPLAY_ID_DEFAULT;
static constexpr int32_t SECOND_DISPLAY_ID = 1;

static constexpr int32_t ACTION_OUTSIDE = AMOTION_EVENT_ACTION_OUTSIDE;
static constexpr int32_t POINTER_1_DOWN =
        AMOTION_EVENT_ACTION_POINTER_DOWN | (1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
static constexpr int32_t POINTER_2_DOWN =
        AMOTION_EVENT_ACTION_POINTER_DOWN | (2 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
static constexpr int32_t POINTER_3_DOWN =
        AMOTION_EVENT_ACTION_POINTER_DOWN | (3 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
static constexpr int32_t POINTER_0_UP =
        AMOTION_EVENT_ACTION_POINTER_UP | (0 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
static constexpr int32_t POINTER_1_UP =
        AMOTION_EVENT_ACTION_POINTER_UP | (1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);

// The default pid and uid for windows created on the primary display by the test.
static constexpr int32_t WINDOW_PID = 999;
static constexpr int32_t WINDOW_UID = 1001;

// The default pid and uid for the windows created on the secondary display by the test.
static constexpr int32_t SECONDARY_WINDOW_PID = 1010;
static constexpr int32_t SECONDARY_WINDOW_UID = 1012;

// The default policy flags to use for event injection by tests.
static constexpr uint32_t DEFAULT_POLICY_FLAGS = POLICY_FLAG_FILTERED | POLICY_FLAG_PASS_TO_USER;

// An arbitrary pid of the gesture monitor window
static constexpr int32_t MONITOR_PID = 2001;

static constexpr std::chrono::duration STALE_EVENT_TIMEOUT = 1000ms;

static constexpr int expectedWallpaperFlags =
        AMOTION_EVENT_FLAG_WINDOW_IS_OBSCURED | AMOTION_EVENT_FLAG_WINDOW_IS_PARTIALLY_OBSCURED;

struct PointF {
    float x;
    float y;
                                    const = default;
};

/**
 * Return a DOWN key event with KEYCODE_A.
 */
static KeyEvent getTestKeyEvent() {
    KeyEvent event;

    event.initialize(InputEvent::nextId(), DEVICE_ID, AINPUT_SOURCE_KEYBOARD, ADISPLAY_ID_NONE,
                     INVALID_HMAC, AKEY_EVENT_ACTION_DOWN, 0, AKEYCODE_A, KEY_A, AMETA_NONE, 0,
                     ARBITRARY_TIME, ARBITRARY_TIME);
    return event;
}

static void assertMotionAction(int32_t expectedAction, int32_t receivedAction) {
    ASSERT_EQ(expectedAction, receivedAction)
            << "expected " << MotionEvent::actionToString(expectedAction) << ", got "
            << MotionEvent::actionToString(receivedAction);
}

// --- FakeInputDispatcherPolicy ---

// --- FakeInputDispatcherPolicy ---
class FakeInputDispatcherPolicy : public InputDispatcherPolicyInterface {
    InputDispatcherConfiguration mConfig;
    
    using AnrResult = std::pair<sp<IBinder>, int32_t /*pid*/>;
    
protected:
    virtual ~FakeInputDispatcherPolicy() {}
    
public:
    FakeInputDispatcherPolicy() {}
    
    void assertFilterInputEventWasCalled(const NotifyKeyArgs& args) {
        assertFilterInputEventWasCalledInternal([&args](const InputEvent& event) {
            ASSERT_EQ(event.getType(), AINPUT_EVENT_TYPE_KEY);
            EXPECT_EQ(event.getDisplayId(), args.displayId);
    
            const auto& keyEvent = static_cast<const KeyEvent&>(event);
            EXPECT_EQ(keyEvent.getEventTime(), args.eventTime);
            EXPECT_EQ(keyEvent.getAction(), args.action);
        });
    }
    
    void assertFilterInputEventWasCalled(const NotifyMotionArgs& args, vec2 point) {
        assertFilterInputEventWasCalledInternal([&](const InputEvent& event) {
            ASSERT_EQ(event.getType(), AINPUT_EVENT_TYPE_MOTION);
            EXPECT_EQ(event.getDisplayId(), args.displayId);
    
            const auto& motionEvent = static_cast<const MotionEvent&>(event);
            EXPECT_EQ(motionEvent.getEventTime(), args.eventTime);
            EXPECT_EQ(motionEvent.getAction(), args.action);
            EXPECT_EQ(motionEvent.getX(0), point.x);
            EXPECT_EQ(motionEvent.getY(0), point.y);
            EXPECT_EQ(motionEvent.getRawX(0), point.x);
            EXPECT_EQ(motionEvent.getRawY(0), point.y);
        });
    }
    
    void assertFilterInputEventWasNotCalled() {
        std::scoped_lock lock(mLock);
        ASSERT_EQ(nullptr, mFilteredEvent);
    }
    
    void assertNotifyConfigurationChangedWasCalled(nsecs_t when) {
        std::scoped_lock lock(mLock);
        ASSERT_TRUE(mConfigurationChangedTime)
                << "Timed out waiting for configuration changed call";
        ASSERT_EQ(*mConfigurationChangedTime, when);
        mConfigurationChangedTime = std::nullopt;
    }
    
    void assertNotifySwitchWasCalled(const NotifySwitchArgs& args) {
        std::scoped_lock lock(mLock);
        ASSERT_TRUE(mLastNotifySwitch);
        // We do not check id because it is not exposed to the policy
        EXPECT_EQ(args.eventTime, mLastNotifySwitch->eventTime);
        EXPECT_EQ(args.policyFlags, mLastNotifySwitch->policyFlags);
        EXPECT_EQ(args.switchValues, mLastNotifySwitch->switchValues);
        EXPECT_EQ(args.switchMask, mLastNotifySwitch->switchMask);
        mLastNotifySwitch = std::nullopt;
    }
    
    void assertOnPointerDownEquals(const sp<IBinder>& touchedToken) {
        std::scoped_lock lock(mLock);
        ASSERT_EQ(touchedToken, mOnPointerDownToken);
        mOnPointerDownToken.clear();
    }
    
    void assertOnPointerDownWasNotCalled() {
        std::scoped_lock lock(mLock);
        ASSERT_TRUE(mOnPointerDownToken == nullptr)
                << "Expected onPointerDownOutsideFocus to not have been called";
    }
    
    // This function must be called soon after the expected ANR timer starts,
    // because we are also checking how much time has passed.
    void assertNotifyNoFocusedWindowAnrWasCalled(
            std::chrono::nanoseconds timeout,
            const std::shared_ptr<InputApplicationHandle>& expectedApplication) {
        std::unique_lock lock(mLock);
        android::base::ScopedLockAssertion assumeLocked(mLock);
        std::shared_ptr<InputApplicationHandle> application;
        ASSERT_NO_FATAL_FAILURE(
                application = getAnrTokenLockedInterruptible(timeout, mAnrApplications, lock));
        ASSERT_EQ(expectedApplication, application);
    }
    
    void assertNotifyWindowUnresponsiveWasCalled(std::chrono::nanoseconds timeout,
                                                 const sp<WindowInfoHandle>& window) {
        LOG_ALWAYS_FATAL_IF(window == nullptr, "window should not be null");
        assertNotifyWindowUnresponsiveWasCalled(timeout, window->getToken(),
                                                window->getInfo()->ownerPid);
    }
    
    void assertNotifyWindowUnresponsiveWasCalled(std::chrono::nanoseconds timeout,
                                                 const sp<IBinder>& expectedToken,
                                                 int32_t expectedPid) {
        std::unique_lock lock(mLock);
        android::base::ScopedLockAssertion assumeLocked(mLock);
        AnrResult result;
        ASSERT_NO_FATAL_FAILURE(result =
                                        getAnrTokenLockedInterruptible(timeout, mAnrWindows, lock));
        const auto& [token, pid] = result;
        ASSERT_EQ(expectedToken, token);
        ASSERT_EQ(expectedPid, pid);
    }
    
    /** Wrap call with ASSERT_NO_FATAL_FAILURE() to ensure the return value is valid. */
    sp<IBinder> getUnresponsiveWindowToken(std::chrono::nanoseconds timeout) {
        std::unique_lock lock(mLock);
        android::base::ScopedLockAssertion assumeLocked(mLock);
        AnrResult result = getAnrTokenLockedInterruptible(timeout, mAnrWindows, lock);
        const auto& [token, _] = result;
        return token;
    }
    
    void assertNotifyWindowResponsiveWasCalled(const sp<IBinder>& expectedToken,
                                               int32_t expectedPid) {
        std::unique_lock lock(mLock);
        android::base::ScopedLockAssertion assumeLocked(mLock);
        AnrResult result;
        ASSERT_NO_FATAL_FAILURE(
                result = getAnrTokenLockedInterruptible(0s, mResponsiveWindows, lock));
        const auto& [token, pid] = result;
        ASSERT_EQ(expectedToken, token);
        ASSERT_EQ(expectedPid, pid);
    }
    
    /** Wrap call with ASSERT_NO_FATAL_FAILURE() to ensure the return value is valid. */
    sp<IBinder> getResponsiveWindowToken() {
        std::unique_lock lock(mLock);
        android::base::ScopedLockAssertion assumeLocked(mLock);
        AnrResult result = getAnrTokenLockedInterruptible(0s, mResponsiveWindows, lock);
        const auto& [token, _] = result;
        return token;
    }
    
    void assertNotifyAnrWasNotCalled() {
        std::scoped_lock lock(mLock);
        ASSERT_TRUE(mAnrApplications.empty());
        ASSERT_TRUE(mAnrWindows.empty());
        ASSERT_TRUE(mResponsiveWindows.empty())
                << "ANR was not called, but please also consume the 'connection is responsive' "
                   "signal";
    }
    
    void setKeyRepeatConfiguration(nsecs_t timeout, nsecs_t delay) {
        mConfig.keyRepeatTimeout = timeout;
        mConfig.keyRepeatDelay = delay;
    }
    
    PointerCaptureRequest assertSetPointerCaptureCalled(bool enabled) {
        std::unique_lock lock(mLock);
        base::ScopedLockAssertion assumeLocked(mLock);
    
        if (!mPointerCaptureChangedCondition.wait_for(lock, 100ms,
                                                      [this, enabled]() REQUIRES(mLock) {
                                                          return mPointerCaptureRequest->enable ==
                                                                  enabled;
                                                      })) {
            ADD_FAILURE() << "Timed out waiting for setPointerCapture(" << enabled
                          << ") to be called.";
            return {};
        }
        auto request = *mPointerCaptureRequest;
        mPointerCaptureRequest.reset();
        return request;
    }
    
    void assertSetPointerCaptureNotCalled() {
        std::unique_lock lock(mLock);
        base::ScopedLockAssertion assumeLocked(mLock);
    
        if (mPointerCaptureChangedCondition.wait_for(lock, 100ms) != std::cv_status::timeout) {
            FAIL() << "Expected setPointerCapture(request) to not be called, but was called. "
                      "enabled = "
                   << std::to_string(mPointerCaptureRequest->enable);
        }
        mPointerCaptureRequest.reset();
    }
    
    void assertDropTargetEquals(const sp<IBinder>& targetToken) {
        std::scoped_lock lock(mLock);
        ASSERT_TRUE(mNotifyDropWindowWasCalled);
        ASSERT_EQ(targetToken, mDropTargetWindowToken);
        mNotifyDropWindowWasCalled = false;
    }
    
    void assertNotifyInputChannelBrokenWasCalled(const sp<IBinder>& token) {
        std::unique_lock lock(mLock);
        base::ScopedLockAssertion assumeLocked(mLock);
        std::optional<sp<IBinder>> receivedToken =
                getItemFromStorageLockedInterruptible(100ms, mBrokenInputChannels, lock,
                                                      mNotifyInputChannelBroken);
        ASSERT_TRUE(receivedToken.has_value());
        ASSERT_EQ(token, *receivedToken);
    }
    
    /**
     * Set policy timeout. A value of zero means next key will not be intercepted.
     */
    void setInterceptKeyTimeout(std::chrono::milliseconds timeout) {
        mInterceptKeyTimeout = timeout;
    }
    
private:
                                    GUARDED_BY(mLock) = false;
                                    
std::unique_ptr<InputEvent> mFilteredEvent                                       GUARDED_BY(mLock);
std::optional<nsecs_t> mConfigurationChangedTime                                       GUARDED_BY(mLock);
sp<IBinder> mOnPointerDownToken                                       GUARDED_BY(mLock);
std::optional<NotifySwitchArgs> mLastNotifySwitch                                       GUARDED_BY(mLock);
    std::condition_variable mPointerCaptureChangedCondition;
    
std::optional<PointerCaptureRequest> mPointerCaptureRequest                                       GUARDED_BY(mLock);
// ANR handling
std::queue<std::shared_ptr<InputApplicationHandle>> mAnrApplications                                       GUARDED_BY(mLock);
std::queue<AnrResult> mAnrWindows                                       GUARDED_BY(mLock);
std::queue<AnrResult> mResponsiveWindows                                       GUARDED_BY(mLock);
    std::condition_variable mNotifyAnr;
std::queue<sp<IBinder>> mBrokenInputChannels                                       GUARDED_BY(mLock);
    std::condition_variable mNotifyInputChannelBroken;
    
sp<IBinder> mDropTargetWindowToken                                       GUARDED_BY(mLock);
bool mNotifyDropWindowWasCalled                                    GUARDED_BY(mLock) = false;
                                    
    std::chrono::milliseconds mInterceptKeyTimeout = 0ms;
    
                                                                         REQUIRES(mLock) {
                                                                         // If there is an ANR, Dispatcher won't be idle because there are still events
                                                                         // in the waitQueue that we need to check on. So we can't wait for dispatcher to be idle
                                                                         // before checking if ANR was called.
                                                                         // Since dispatcher is not guaranteed to call notifyNoFocusedWindowAnr right away, we need
                                                                         // to provide it some time to act. 100ms seems reasonable.
                                                                         std::chrono::duration timeToWait = timeout + 100ms; // provide some slack
                                                                         const std::chrono::time_point start = std::chrono::steady_clock::now();
                                                                         std::optional<T> token =
                                                                         getItemFromStorageLockedInterruptible(timeToWait, storage, lock, mNotifyAnr);
                                                                         if (!token.has_value()) {
                                                                         ADD_FAILURE() << "Did not receive the ANR callback";
                                                                         return {};
                                                                         }
                                                                         
                                                                         const std::chrono::duration waited = std::chrono::steady_clock::now() - start;
                                                                         // Ensure that the ANR didn't get raised too early. We can't be too strict here because
                                                                         // the dispatcher started counting before this function was called
                                                                         if (std::chrono::abs(timeout - waited) > 100ms) {
                                                                         ADD_FAILURE() << "ANR was raised too early or too late. Expected "
                                                                         << std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count()
                                                                         << "ms, but waited "
                                                                         << std::chrono::duration_cast<std::chrono::milliseconds>(waited).count()
                                                                         << "ms instead";
                                                                         }
                                                                         return *token;
                                                                         }
                                                                         
            REQUIRES(mLock) {
            condition.wait_for(lock, timeout,
                           [&storage]() REQUIRES(mLock) { return !storage.empty(); });
            if (storage.empty()) {
            ADD_FAILURE() << "Did not receive the expected callback";
            return std::nullopt;
            }
            T item = storage.front();
            storage.pop();
            return std::make_optional(item);
            }
            
    void notifyConfigurationChanged(nsecs_t when) override {
        std::scoped_lock lock(mLock);
        mConfigurationChangedTime = when;
    }
    
    void notifyWindowUnresponsive(const sp<IBinder>& connectionToken, std::optional<int32_t> pid,
                                  const std::string&) override {
        std::scoped_lock lock(mLock);
        ASSERT_TRUE(pid.has_value());
        mAnrWindows.push({connectionToken, *pid});
        mNotifyAnr.notify_all();
    }
    
    void notifyWindowResponsive(const sp<IBinder>& connectionToken,
                                std::optional<int32_t> pid) override {
        std::scoped_lock lock(mLock);
        ASSERT_TRUE(pid.has_value());
        mResponsiveWindows.push({connectionToken, *pid});
        mNotifyAnr.notify_all();
    }
    
    void notifyNoFocusedWindowAnr(
            const std::shared_ptr<InputApplicationHandle>& applicationHandle) override {
        std::scoped_lock lock(mLock);
        mAnrApplications.push(applicationHandle);
        mNotifyAnr.notify_all();
    }
    
    void notifyInputChannelBroken(const sp<IBinder>& connectionToken) override {
        std::scoped_lock lock(mLock);
        mBrokenInputChannels.push(connectionToken);
        mNotifyInputChannelBroken.notify_all();
    }
    
    void notifyFocusChanged(const sp<IBinder>&, const sp<IBinder>&) override {}
    
    void notifySensorEvent(int32_t deviceId, InputDeviceSensorType sensorType,
                           InputDeviceSensorAccuracy accuracy, nsecs_t timestamp,
                           const std::vector<float>& values) override {}
    
    void notifySensorAccuracy(int deviceId, InputDeviceSensorType sensorType,
                              InputDeviceSensorAccuracy accuracy) override {}
    
    void notifyVibratorState(int32_t deviceId, bool isOn) override {}
    
    void getDispatcherConfiguration(InputDispatcherConfiguration* outConfig) override {
        *outConfig = mConfig;
    }
    
    bool filterInputEvent(const InputEvent* inputEvent, uint32_t policyFlags) override {
        std::scoped_lock lock(mLock);
        switch (inputEvent->getType()) {
            case AINPUT_EVENT_TYPE_KEY: {
                const KeyEvent* keyEvent = static_cast<const KeyEvent*>(inputEvent);
                mFilteredEvent = std::make_unique<KeyEvent>(*keyEvent);
                break;
            }
    
            case AINPUT_EVENT_TYPE_MOTION: {
                const MotionEvent* motionEvent = static_cast<const MotionEvent*>(inputEvent);
                mFilteredEvent = std::make_unique<MotionEvent>(*motionEvent);
                break;
            }
        }
        return true;
    }
    
    void interceptKeyBeforeQueueing(const KeyEvent* inputEvent, uint32_t&) override {
        if (inputEvent->getAction() == AKEY_EVENT_ACTION_UP) {
            // Clear intercept state when we handled the event.
            mInterceptKeyTimeout = 0ms;
        }
    }
    
    void interceptMotionBeforeQueueing(int32_t, nsecs_t, uint32_t&) override {}
    
    nsecs_t interceptKeyBeforeDispatching(const sp<IBinder>&, const KeyEvent*, uint32_t) override {
        nsecs_t delay = std::chrono::nanoseconds(mInterceptKeyTimeout).count();
        // Clear intercept state so we could dispatch the event in next wake.
        mInterceptKeyTimeout = 0ms;
        return delay;
    }
    
    bool dispatchUnhandledKey(const sp<IBinder>&, const KeyEvent*, uint32_t, KeyEvent*) override {
        return false;
    }
    
    void notifySwitch(nsecs_t when, uint32_t switchValues, uint32_t switchMask,
                      uint32_t policyFlags) override {
        std::scoped_lock lock(mLock);
        /** We simply reconstruct NotifySwitchArgs in policy because InputDispatcher is
         * essentially a passthrough for notifySwitch.
         */
        mLastNotifySwitch = NotifySwitchArgs(1 /*id*/, when, policyFlags, switchValues, switchMask);
    }
    
    void pokeUserActivity(nsecs_t, int32_t, int32_t) override {}
    
    void onPointerDownOutsideFocus(const sp<IBinder>& newToken) override {
        std::scoped_lock lock(mLock);
        mOnPointerDownToken = newToken;
    }
    
    void setPointerCapture(const PointerCaptureRequest& request) override {
        std::scoped_lock lock(mLock);
        mPointerCaptureRequest = {request};
        mPointerCaptureChangedCondition.notify_all();
    }
    
    void notifyDropWindow(const sp<IBinder>& token, float x, float y) override {
        std::scoped_lock lock(mLock);
        mNotifyDropWindowWasCalled = true;
        mDropTargetWindowToken = token;
    }
    
    void assertFilterInputEventWasCalledInternal(
            const std::function<void(const InputEvent&)>& verify) {
        std::scoped_lock lock(mLock);
        ASSERT_NE(nullptr, mFilteredEvent) << "Expected filterInputEvent() to have been called.";
        verify(*mFilteredEvent);
        mFilteredEvent = nullptr;
    }
};

// --- InputDispatcherTest ---

// --- InputDispatcherTest ---
class InputDispatcherTest : public testing::Test {
protected:
    sp<FakeInputDispatcherPolicy> mFakePolicy;
    std::unique_ptr<InputDispatcher> mDispatcher;
    
    void SetUp() override {
        mFakePolicy = sp<FakeInputDispatcherPolicy>::make();
        mDispatcher = std::make_unique<InputDispatcher>(mFakePolicy, STALE_EVENT_TIMEOUT);
        mDispatcher->setInputDispatchMode(/*enabled*/ true, /*frozen*/ false);
        // Start InputDispatcher thread
        ASSERT_EQ(OK, mDispatcher->start());
    }
    
    void TearDown() override {
        ASSERT_EQ(OK, mDispatcher->stop());
        mFakePolicy.clear();
        mDispatcher.reset();
    }
    
    /**
     * Used for debugging when writing the test
     */
    void dumpDispatcherState() {
        std::string dump;
        mDispatcher->dump(dump);
        std::stringstream ss(dump);
        std::string to;
    
        while (std::getline(ss, to, '\n')) {
            ALOGE("%s", to.c_str());
        }
    }
    
    void setFocusedWindow(const sp<WindowInfoHandle>& window,
                          const sp<WindowInfoHandle>& focusedWindow = nullptr) {
        FocusRequest request;
        request.token = window->getToken();
        request.windowName = window->getName();
        if (focusedWindow) {
            request.focusedToken = focusedWindow->getToken();
        }
        request.timestamp = systemTime(SYSTEM_TIME_MONOTONIC);
        request.displayId = window->getInfo()->displayId;
        mDispatcher->setFocusedWindow(request);
    }
};

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

/* Test InputDispatcher for notifyConfigurationChanged and notifySwitch events */

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

// --- InputDispatcherTest SetInputWindowTest ---
static constexpr std::chrono::duration INJECT_EVENT_TIMEOUT = 500ms;
// Default input dispatching timeout if there is no focused application or paused window
// from which to determine an appropriate dispatching timeout.
static const std::chrono::duration DISPATCHING_TIMEOUT = std::chrono::milliseconds(
        android::os::IInputConstants::UNMULTIPLIED_DEFAULT_DISPATCHING_TIMEOUT_MILLIS *
        android::base::HwTimeoutMultiplier());

class FakeApplicationHandle : public InputApplicationHandle {
public:
    FakeApplicationHandle() {
        mInfo.name = "Fake Application";
        mInfo.token = sp<BBinder>::make();
        mInfo.dispatchingTimeoutMillis =
                std::chrono::duration_cast<std::chrono::milliseconds>(DISPATCHING_TIMEOUT).count();
    }
    virtual ~FakeApplicationHandle() {}
    
    virtual bool updateInfo() override { return true; }
    
    void setDispatchingTimeout(std::chrono::milliseconds timeout) {
        mInfo.dispatchingTimeoutMillis = timeout.count();
    }
};

class FakeInputReceiver {
public:
    explicit FakeInputReceiver(std::unique_ptr<InputChannel> clientChannel, const std::string name)
          : mName(name) {
        mConsumer = std::make_unique<InputConsumer>(std::move(clientChannel));
    }
    
    InputEvent* consume() {
        InputEvent* event;
        std::optional<uint32_t> consumeSeq = receiveEvent(&event);
        if (!consumeSeq) {
            return nullptr;
        }
        finishEvent(*consumeSeq);
        return event;
    }
    
    /**
     * Receive an event without acknowledging it.
     * Return the sequence number that could later be used to send finished signal.
     */
    std::optional<uint32_t> receiveEvent(InputEvent** outEvent = nullptr) {
        uint32_t consumeSeq;
        InputEvent* event;
    
        std::chrono::time_point start = std::chrono::steady_clock::now();
        status_t status = WOULD_BLOCK;
        while (status == WOULD_BLOCK) {
            status = mConsumer->consume(&mEventFactory, true /*consumeBatches*/, -1, &consumeSeq,
                                        &event);
            std::chrono::duration elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > 100ms) {
                break;
            }
        }
    
        if (status == WOULD_BLOCK) {
            // Just means there's no event available.
            return std::nullopt;
        }
    
        if (status != OK) {
            ADD_FAILURE() << mName.c_str() << ": consumer consume should return OK.";
            return std::nullopt;
        }
        if (event == nullptr) {
            ADD_FAILURE() << "Consumed correctly, but received NULL event from consumer";
            return std::nullopt;
        }
        if (outEvent != nullptr) {
            *outEvent = event;
        }
        return consumeSeq;
    }
    
    /**
     * To be used together with "receiveEvent" to complete the consumption of an event.
     */
    void finishEvent(uint32_t consumeSeq) {
        const status_t status = mConsumer->sendFinishedSignal(consumeSeq, true);
        ASSERT_EQ(OK, status) << mName.c_str() << ": consumer sendFinishedSignal should return OK.";
    }
    
    void sendTimeline(int32_t inputEventId, std::array<nsecs_t, GraphicsTimeline::SIZE> timeline) {
        const status_t status = mConsumer->sendTimeline(inputEventId, timeline);
        ASSERT_EQ(OK, status);
    }
    
    void consumeEvent(int32_t expectedEventType, int32_t expectedAction,
                      std::optional<int32_t> expectedDisplayId,
                      std::optional<int32_t> expectedFlags) {
        InputEvent* event = consume();
    
        ASSERT_NE(nullptr, event) << mName.c_str()
                                  << ": consumer should have returned non-NULL event.";
        ASSERT_EQ(expectedEventType, event->getType())
                << mName.c_str() << " expected " << inputEventTypeToString(expectedEventType)
                << " event, got " << inputEventTypeToString(event->getType()) << " event";
    
        if (expectedDisplayId.has_value()) {
            EXPECT_EQ(expectedDisplayId, event->getDisplayId());
        }
    
        switch (expectedEventType) {
            case AINPUT_EVENT_TYPE_KEY: {
                const KeyEvent& keyEvent = static_cast<const KeyEvent&>(*event);
                EXPECT_EQ(expectedAction, keyEvent.getAction());
                if (expectedFlags.has_value()) {
                    EXPECT_EQ(expectedFlags.value(), keyEvent.getFlags());
                }
                break;
            }
            case AINPUT_EVENT_TYPE_MOTION: {
                const MotionEvent& motionEvent = static_cast<const MotionEvent&>(*event);
                assertMotionAction(expectedAction, motionEvent.getAction());
    
                if (expectedFlags.has_value()) {
                    EXPECT_EQ(expectedFlags.value(), motionEvent.getFlags());
                }
                break;
            }
            case AINPUT_EVENT_TYPE_FOCUS: {
                FAIL() << "Use 'consumeFocusEvent' for FOCUS events";
            }
            case AINPUT_EVENT_TYPE_CAPTURE: {
                FAIL() << "Use 'consumeCaptureEvent' for CAPTURE events";
            }
            case AINPUT_EVENT_TYPE_TOUCH_MODE: {
                FAIL() << "Use 'consumeTouchModeEvent' for TOUCH_MODE events";
            }
            case AINPUT_EVENT_TYPE_DRAG: {
                FAIL() << "Use 'consumeDragEvent' for DRAG events";
            }
            default: {
                FAIL() << mName.c_str() << ": invalid event type: " << expectedEventType;
            }
        }
    }
    
    void consumeFocusEvent(bool hasFocus, bool inTouchMode) {
        InputEvent* event = consume();
        ASSERT_NE(nullptr, event) << mName.c_str()
                                  << ": consumer should have returned non-NULL event.";
        ASSERT_EQ(AINPUT_EVENT_TYPE_FOCUS, event->getType())
                << "Got " << inputEventTypeToString(event->getType())
                << " event instead of FOCUS event";
    
        ASSERT_EQ(ADISPLAY_ID_NONE, event->getDisplayId())
                << mName.c_str() << ": event displayId should always be NONE.";
    
        FocusEvent* focusEvent = static_cast<FocusEvent*>(event);
        EXPECT_EQ(hasFocus, focusEvent->getHasFocus());
    }
    
    void consumeCaptureEvent(bool hasCapture) {
        const InputEvent* event = consume();
        ASSERT_NE(nullptr, event) << mName.c_str()
                                  << ": consumer should have returned non-NULL event.";
        ASSERT_EQ(AINPUT_EVENT_TYPE_CAPTURE, event->getType())
                << "Got " << inputEventTypeToString(event->getType())
                << " event instead of CAPTURE event";
    
        ASSERT_EQ(ADISPLAY_ID_NONE, event->getDisplayId())
                << mName.c_str() << ": event displayId should always be NONE.";
    
        const auto& captureEvent = static_cast<const CaptureEvent&>(*event);
        EXPECT_EQ(hasCapture, captureEvent.getPointerCaptureEnabled());
    }
    
    void consumeDragEvent(bool isExiting, float x, float y) {
        const InputEvent* event = consume();
        ASSERT_NE(nullptr, event) << mName.c_str()
                                  << ": consumer should have returned non-NULL event.";
        ASSERT_EQ(AINPUT_EVENT_TYPE_DRAG, event->getType())
                << "Got " << inputEventTypeToString(event->getType())
                << " event instead of DRAG event";
    
        EXPECT_EQ(ADISPLAY_ID_NONE, event->getDisplayId())
                << mName.c_str() << ": event displayId should always be NONE.";
    
        const auto& dragEvent = static_cast<const DragEvent&>(*event);
        EXPECT_EQ(isExiting, dragEvent.isExiting());
        EXPECT_EQ(x, dragEvent.getX());
        EXPECT_EQ(y, dragEvent.getY());
    }
    
    void consumeTouchModeEvent(bool inTouchMode) {
        const InputEvent* event = consume();
        ASSERT_NE(nullptr, event) << mName.c_str()
                                  << ": consumer should have returned non-NULL event.";
        ASSERT_EQ(AINPUT_EVENT_TYPE_TOUCH_MODE, event->getType())
                << "Got " << inputEventTypeToString(event->getType())
                << " event instead of TOUCH_MODE event";
    
        ASSERT_EQ(ADISPLAY_ID_NONE, event->getDisplayId())
                << mName.c_str() << ": event displayId should always be NONE.";
        const auto& touchModeEvent = static_cast<const TouchModeEvent&>(*event);
        EXPECT_EQ(inTouchMode, touchModeEvent.isInTouchMode());
    }
    
    void assertNoEvents() {
        InputEvent* event = consume();
        if (event == nullptr) {
            return;
        }
        if (event->getType() == AINPUT_EVENT_TYPE_KEY) {
            KeyEvent& keyEvent = static_cast<KeyEvent&>(*event);
            ADD_FAILURE() << "Received key event "
                          << KeyEvent::actionToString(keyEvent.getAction());
        } else if (event->getType() == AINPUT_EVENT_TYPE_MOTION) {
            MotionEvent& motionEvent = static_cast<MotionEvent&>(*event);
            ADD_FAILURE() << "Received motion event "
                          << MotionEvent::actionToString(motionEvent.getAction());
        } else if (event->getType() == AINPUT_EVENT_TYPE_FOCUS) {
            FocusEvent& focusEvent = static_cast<FocusEvent&>(*event);
            ADD_FAILURE() << "Received focus event, hasFocus = "
                          << (focusEvent.getHasFocus() ? "true" : "false");
        } else if (event->getType() == AINPUT_EVENT_TYPE_CAPTURE) {
            const auto& captureEvent = static_cast<CaptureEvent&>(*event);
            ADD_FAILURE() << "Received capture event, pointerCaptureEnabled = "
                          << (captureEvent.getPointerCaptureEnabled() ? "true" : "false");
        } else if (event->getType() == AINPUT_EVENT_TYPE_TOUCH_MODE) {
            const auto& touchModeEvent = static_cast<TouchModeEvent&>(*event);
            ADD_FAILURE() << "Received touch mode event, inTouchMode = "
                          << (touchModeEvent.isInTouchMode() ? "true" : "false");
        }
        FAIL() << mName.c_str()
               << ": should not have received any events, so consume() should return NULL";
    }
    
    sp<IBinder> getToken() { return mConsumer->getChannel()->getConnectionToken(); }
    
    int getChannelFd() { return mConsumer->getChannel()->getFd().get(); }
    
protected:
    std::unique_ptr<InputConsumer> mConsumer;
    PreallocatedInputEventFactory mEventFactory;
    
    std::string mName;
};

class FakeWindowHandle : public WindowInfoHandle {
public:
    static const int32_t WIDTH = 600;
    static const int32_t HEIGHT = 800;
    
    FakeWindowHandle(const std::shared_ptr<InputApplicationHandle>& inputApplicationHandle,
                     const std::unique_ptr<InputDispatcher>& dispatcher, const std::string name,
                     int32_t displayId, std::optional<sp<IBinder>> token = std::nullopt)
          : mName(name) {
        if (token == std::nullopt) {
            base::Result<std::unique_ptr<InputChannel>> channel =
                    dispatcher->createInputChannel(name);
            token = (*channel)->getConnectionToken();
            mInputReceiver = std::make_unique<FakeInputReceiver>(std::move(*channel), name);
        }
    
        inputApplicationHandle->updateInfo();
        mInfo.applicationInfo = *inputApplicationHandle->getInfo();
    
        mInfo.token = *token;
        mInfo.id = sId++;
        mInfo.name = name;
        mInfo.dispatchingTimeout = DISPATCHING_TIMEOUT;
        mInfo.alpha = 1.0;
        mInfo.frameLeft = 0;
        mInfo.frameTop = 0;
        mInfo.frameRight = WIDTH;
        mInfo.frameBottom = HEIGHT;
        mInfo.transform.set(0, 0);
        mInfo.globalScaleFactor = 1.0;
        mInfo.touchableRegion.clear();
        mInfo.addTouchableRegion(Rect(0, 0, WIDTH, HEIGHT));
        mInfo.ownerPid = WINDOW_PID;
        mInfo.ownerUid = WINDOW_UID;
        mInfo.displayId = displayId;
        mInfo.inputConfig = WindowInfo::InputConfig::DEFAULT;
    }
    
    sp<FakeWindowHandle> clone(
            const std::shared_ptr<InputApplicationHandle>& inputApplicationHandle,
            const std::unique_ptr<InputDispatcher>& dispatcher, int32_t displayId) {
        sp<FakeWindowHandle> handle =
                sp<FakeWindowHandle>::make(inputApplicationHandle, dispatcher,
                                           mInfo.name + "(Mirror)", displayId, mInfo.token);
        return handle;
    }
    
    void setTouchable(bool touchable) {
        mInfo.setInputConfig(WindowInfo::InputConfig::NOT_TOUCHABLE, !touchable);
    }
    
    void setFocusable(bool focusable) {
        mInfo.setInputConfig(WindowInfo::InputConfig::NOT_FOCUSABLE, !focusable);
    }
    
    void setVisible(bool visible) {
        mInfo.setInputConfig(WindowInfo::InputConfig::NOT_VISIBLE, !visible);
    }
    
    void setDispatchingTimeout(std::chrono::nanoseconds timeout) {
        mInfo.dispatchingTimeout = timeout;
    }
    
    void setPaused(bool paused) {
        mInfo.setInputConfig(WindowInfo::InputConfig::PAUSE_DISPATCHING, paused);
    }
    
    void setPreventSplitting(bool preventSplitting) {
        mInfo.setInputConfig(WindowInfo::InputConfig::PREVENT_SPLITTING, preventSplitting);
    }
    
    void setSlippery(bool slippery) {
        mInfo.setInputConfig(WindowInfo::InputConfig::SLIPPERY, slippery);
    }
    
    void setWatchOutsideTouch(bool watchOutside) {
        mInfo.setInputConfig(WindowInfo::InputConfig::WATCH_OUTSIDE_TOUCH, watchOutside);
    }
    
    void setSpy(bool spy) { mInfo.setInputConfig(WindowInfo::InputConfig::SPY, spy); }
    
    void setInterceptsStylus(bool interceptsStylus) {
        mInfo.setInputConfig(WindowInfo::InputConfig::INTERCEPTS_STYLUS, interceptsStylus);
    }
    
    void setDropInput(bool dropInput) {
        mInfo.setInputConfig(WindowInfo::InputConfig::DROP_INPUT, dropInput);
    }
    
    void setDropInputIfObscured(bool dropInputIfObscured) {
        mInfo.setInputConfig(WindowInfo::InputConfig::DROP_INPUT_IF_OBSCURED, dropInputIfObscured);
    }
    
    void setNoInputChannel(bool noInputChannel) {
        mInfo.setInputConfig(WindowInfo::InputConfig::NO_INPUT_CHANNEL, noInputChannel);
    }
    
    void setAlpha(float alpha) { mInfo.alpha = alpha; }
    
    void setTouchOcclusionMode(TouchOcclusionMode mode) { mInfo.touchOcclusionMode = mode; }
    
    void setApplicationToken(sp<IBinder> token) { mInfo.applicationInfo.token = token; }
    
    void setFrame(const Rect& frame, const ui::Transform& displayTransform = ui::Transform()) {
        mInfo.frameLeft = frame.left;
        mInfo.frameTop = frame.top;
        mInfo.frameRight = frame.right;
        mInfo.frameBottom = frame.bottom;
        mInfo.touchableRegion.clear();
        mInfo.addTouchableRegion(frame);
    
        const Rect logicalDisplayFrame = displayTransform.transform(frame);
        ui::Transform translate;
        translate.set(-logicalDisplayFrame.left, -logicalDisplayFrame.top);
        mInfo.transform = translate * displayTransform;
    }
    
    void setTouchableRegion(const Region& region) { mInfo.touchableRegion = region; }
    
    void setIsWallpaper(bool isWallpaper) {
        mInfo.setInputConfig(WindowInfo::InputConfig::IS_WALLPAPER, isWallpaper);
    }
    
    void setDupTouchToWallpaper(bool hasWallpaper) {
        mInfo.setInputConfig(WindowInfo::InputConfig::DUPLICATE_TOUCH_TO_WALLPAPER, hasWallpaper);
    }
    
    void setTrustedOverlay(bool trustedOverlay) {
        mInfo.setInputConfig(WindowInfo::InputConfig::TRUSTED_OVERLAY, trustedOverlay);
    }
    
    void setWindowTransform(float dsdx, float dtdx, float dtdy, float dsdy) {
        mInfo.transform.set(dsdx, dtdx, dtdy, dsdy);
    }
    
    void setWindowScale(float xScale, float yScale) { setWindowTransform(xScale, 0, 0, yScale); }
    
    void setWindowOffset(float offsetX, float offsetY) { mInfo.transform.set(offsetX, offsetY); }
    
    void consumeKeyDown(int32_t expectedDisplayId, int32_t expectedFlags = 0) {
        consumeEvent(AINPUT_EVENT_TYPE_KEY, AKEY_EVENT_ACTION_DOWN, expectedDisplayId,
                     expectedFlags);
    }
    
    void consumeKeyUp(int32_t expectedDisplayId, int32_t expectedFlags = 0) {
        consumeEvent(AINPUT_EVENT_TYPE_KEY, AKEY_EVENT_ACTION_UP, expectedDisplayId, expectedFlags);
    }
    
    void consumeMotionCancel(int32_t expectedDisplayId = ADISPLAY_ID_DEFAULT,
                             int32_t expectedFlags = 0) {
        consumeEvent(AINPUT_EVENT_TYPE_MOTION, AMOTION_EVENT_ACTION_CANCEL, expectedDisplayId,
                     expectedFlags);
    }
    
    void consumeMotionMove(int32_t expectedDisplayId = ADISPLAY_ID_DEFAULT,
                           int32_t expectedFlags = 0) {
        consumeEvent(AINPUT_EVENT_TYPE_MOTION, AMOTION_EVENT_ACTION_MOVE, expectedDisplayId,
                     expectedFlags);
    }
    
    void consumeMotionDown(int32_t expectedDisplayId = ADISPLAY_ID_DEFAULT,
                           int32_t expectedFlags = 0) {
        consumeAnyMotionDown(expectedDisplayId, expectedFlags);
    }
    
    void consumeAnyMotionDown(std::optional<int32_t> expectedDisplayId = std::nullopt,
                              std::optional<int32_t> expectedFlags = std::nullopt) {
        consumeEvent(AINPUT_EVENT_TYPE_MOTION, AMOTION_EVENT_ACTION_DOWN, expectedDisplayId,
                     expectedFlags);
    }
    
    void consumeMotionPointerDown(int32_t pointerIdx,
                                  int32_t expectedDisplayId = ADISPLAY_ID_DEFAULT,
                                  int32_t expectedFlags = 0) {
        int32_t action = AMOTION_EVENT_ACTION_POINTER_DOWN |
                (pointerIdx << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        consumeEvent(AINPUT_EVENT_TYPE_MOTION, action, expectedDisplayId, expectedFlags);
    }
    
    void consumeMotionPointerUp(int32_t pointerIdx, int32_t expectedDisplayId = ADISPLAY_ID_DEFAULT,
                                int32_t expectedFlags = 0) {
        int32_t action = AMOTION_EVENT_ACTION_POINTER_UP |
                (pointerIdx << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        consumeEvent(AINPUT_EVENT_TYPE_MOTION, action, expectedDisplayId, expectedFlags);
    }
    
    void consumeMotionUp(int32_t expectedDisplayId = ADISPLAY_ID_DEFAULT,
                         int32_t expectedFlags = 0) {
        consumeEvent(AINPUT_EVENT_TYPE_MOTION, AMOTION_EVENT_ACTION_UP, expectedDisplayId,
                     expectedFlags);
    }
    
    void consumeMotionOutside(int32_t expectedDisplayId = ADISPLAY_ID_DEFAULT,
                              int32_t expectedFlags = 0) {
        consumeEvent(AINPUT_EVENT_TYPE_MOTION, AMOTION_EVENT_ACTION_OUTSIDE, expectedDisplayId,
                     expectedFlags);
    }
    
    void consumeMotionOutsideWithZeroedCoords(int32_t expectedDisplayId = ADISPLAY_ID_DEFAULT,
                                              int32_t expectedFlags = 0) {
        InputEvent* event = consume();
        ASSERT_NE(nullptr, event);
        ASSERT_EQ(AINPUT_EVENT_TYPE_MOTION, event->getType());
        const MotionEvent& motionEvent = static_cast<MotionEvent&>(*event);
        EXPECT_EQ(AMOTION_EVENT_ACTION_OUTSIDE, motionEvent.getActionMasked());
        EXPECT_EQ(0.f, motionEvent.getRawPointerCoords(0)->getX());
        EXPECT_EQ(0.f, motionEvent.getRawPointerCoords(0)->getY());
    }
    
    void consumeFocusEvent(bool hasFocus, bool inTouchMode = true) {
        ASSERT_NE(mInputReceiver, nullptr)
                << "Cannot consume events from a window with no receiver";
        mInputReceiver->consumeFocusEvent(hasFocus, inTouchMode);
    }
    
    void consumeCaptureEvent(bool hasCapture) {
        ASSERT_NE(mInputReceiver, nullptr)
                << "Cannot consume events from a window with no receiver";
        mInputReceiver->consumeCaptureEvent(hasCapture);
    }
    
    void consumeMotionEvent(const ::testing::Matcher<MotionEvent>& matcher) {
        MotionEvent* motionEvent = consumeMotion();
        ASSERT_NE(nullptr, motionEvent) << "Did not get a motion event, but expected " << matcher;
        ASSERT_THAT(*motionEvent, matcher);
    }
    
    void consumeEvent(int32_t expectedEventType, int32_t expectedAction,
                      std::optional<int32_t> expectedDisplayId,
                      std::optional<int32_t> expectedFlags) {
        ASSERT_NE(mInputReceiver, nullptr) << "Invalid consume event on window with no receiver";
        mInputReceiver->consumeEvent(expectedEventType, expectedAction, expectedDisplayId,
                                     expectedFlags);
    }
    
    void consumeDragEvent(bool isExiting, float x, float y) {
        mInputReceiver->consumeDragEvent(isExiting, x, y);
    }
    
    void consumeTouchModeEvent(bool inTouchMode) {
        ASSERT_NE(mInputReceiver, nullptr)
                << "Cannot consume events from a window with no receiver";
        mInputReceiver->consumeTouchModeEvent(inTouchMode);
    }
    
    std::optional<uint32_t> receiveEvent(InputEvent** outEvent = nullptr) {
        if (mInputReceiver == nullptr) {
            ADD_FAILURE() << "Invalid receive event on window with no receiver";
            return std::nullopt;
        }
        return mInputReceiver->receiveEvent(outEvent);
    }
    
    void finishEvent(uint32_t sequenceNum) {
        ASSERT_NE(mInputReceiver, nullptr) << "Invalid receive event on window with no receiver";
        mInputReceiver->finishEvent(sequenceNum);
    }
    
    void sendTimeline(int32_t inputEventId, std::array<nsecs_t, GraphicsTimeline::SIZE> timeline) {
        ASSERT_NE(mInputReceiver, nullptr) << "Invalid receive event on window with no receiver";
        mInputReceiver->sendTimeline(inputEventId, timeline);
    }
    
    InputEvent* consume() {
        if (mInputReceiver == nullptr) {
            return nullptr;
        }
        return mInputReceiver->consume();
    }
    
    MotionEvent* consumeMotion() {
        InputEvent* event = consume();
        if (event == nullptr) {
            ADD_FAILURE() << "Consume failed : no event";
            return nullptr;
        }
        if (event->getType() != AINPUT_EVENT_TYPE_MOTION) {
            ADD_FAILURE() << "Instead of motion event, got "
                          << inputEventTypeToString(event->getType());
            return nullptr;
        }
        return static_cast<MotionEvent*>(event);
    }
    
    void assertNoEvents() {
        if (mInputReceiver == nullptr &&
            mInfo.inputConfig.test(WindowInfo::InputConfig::NO_INPUT_CHANNEL)) {
            return; // Can't receive events if the window does not have input channel
        }
        ASSERT_NE(nullptr, mInputReceiver)
                << "Window without InputReceiver must specify feature NO_INPUT_CHANNEL";
        mInputReceiver->assertNoEvents();
    }
    
    sp<IBinder> getToken() { return mInfo.token; }
    
    const std::string& getName() { return mName; }
    
    void setOwnerInfo(int32_t ownerPid, int32_t ownerUid) {
        mInfo.ownerPid = ownerPid;
        mInfo.ownerUid = ownerUid;
    }
    
    int32_t getPid() const { return mInfo.ownerPid; }
    
    void destroyReceiver() { mInputReceiver = nullptr; }
    
    int getChannelFd() { return mInputReceiver->getChannelFd(){ return mInputReceiver->getChannelFd(); }
    
private:
    const std::string mName;
    std::unique_ptr<FakeInputReceiver> mInputReceiver;
static std::atomic<int32_t> sId;                                     // each window gets a unique id, like in surfaceflinger
};

std::atomic<int32_t> FakeWindowHandle::sId{1};

static InputEventInjectionResult injectKey(
        const std::unique_ptr<InputDispatcher>& dispatcher, int32_t action, int32_t repeatCount,
        int32_t displayId = ADISPLAY_ID_NONE,
        InputEventInjectionSync syncMode = InputEventInjectionSync::WAIT_FOR_RESULT,
        std::chrono::milliseconds injectionTimeout = INJECT_EVENT_TIMEOUT,
        bool allowKeyRepeat = true, std::optional<int32_t> targetUid = {},
        uint32_t policyFlags = DEFAULT_POLICY_FLAGS) {
    KeyEvent event;
    nsecs_t currentTime = systemTime(SYSTEM_TIME_MONOTONIC);

    // Define a valid key down event.
    event.initialize(InputEvent::nextId(), DEVICE_ID, AINPUT_SOURCE_KEYBOARD, displayId,
                     INVALID_HMAC, action, /* flags */ 0, AKEYCODE_A, KEY_A, AMETA_NONE,
                     repeatCount, currentTime, currentTime);

    if (!allowKeyRepeat) {
        policyFlags |= POLICY_FLAG_DISABLE_KEY_REPEAT;
    }
    // Inject event until dispatch out.
    return dispatcher->injectInputEvent(&event, targetUid, syncMode, injectionTimeout, policyFlags);
}

static InputEventInjectionResult injectKeyDown(const std::unique_ptr<InputDispatcher>& dispatcher,
                                               int32_t displayId = ADISPLAY_ID_NONE) {
    return injectKey(dispatcher, AKEY_EVENT_ACTION_DOWN, /* repeatCount */ 0, displayId);
}

// Inject a down event that has key repeat disabled. This allows InputDispatcher to idle without
// sending a subsequent key up. When key repeat is enabled, the dispatcher cannot idle because it
// has to be woken up to process the repeating key.
static InputEventInjectionResult injectKeyDownNoRepeat(
        const std::unique_ptr<InputDispatcher>& dispatcher, int32_t displayId = ADISPLAY_ID_NONE) {
    return injectKey(dispatcher, AKEY_EVENT_ACTION_DOWN, /* repeatCount */ 0, displayId,
                     InputEventInjectionSync::WAIT_FOR_RESULT, INJECT_EVENT_TIMEOUT,
                     /* allowKeyRepeat */ false);
}

static InputEventInjectionResult injectKeyUp(const std::unique_ptr<InputDispatcher>& dispatcher,
                                             int32_t displayId = ADISPLAY_ID_NONE) {
    return injectKey(dispatcher, AKEY_EVENT_ACTION_UP, /* repeatCount */ 0, displayId);
}

class PointerBuilder {
public:
    PointerBuilder(int32_t id, int32_t toolType) {
        mProperties.clear();
        mProperties.id = id;
        mProperties.toolType = toolType;
        mCoords.clear();
    }
    
    PointerBuilder& x(float x) { return axis(AMOTION_EVENT_AXIS_X, x); }
    
    PointerBuilder& y(float y) { return axis(AMOTION_EVENT_AXIS_Y, y); }
    
    PointerBuilder& axis(int32_t axis, float value) {
        mCoords.setAxisValue(axis, value);
        return *this;
    }
    
    PointerProperties buildProperties() const { return mProperties; }
    
    PointerCoords buildCoords() const { return mCoords; }
    
private:
    PointerProperties mProperties;
    PointerCoords mCoords;
};

class MotionEventBuilder {
public:
    MotionEventBuilder(int32_t action, int32_t source) {
        mAction = action;
        mSource = source;
        mEventTime = systemTime(SYSTEM_TIME_MONOTONIC);
    }
    
    MotionEventBuilder& eventTime(nsecs_t eventTime) {
        mEventTime = eventTime;
        return *this;
    }
    
    MotionEventBuilder& displayId(int32_t displayId) {
        mDisplayId = displayId;
        return *this;
    }
    
    MotionEventBuilder& actionButton(int32_t actionButton) {
        mActionButton = actionButton;
        return *this;
    }
    
    MotionEventBuilder& buttonState(int32_t buttonState) {
        mButtonState = buttonState;
        return *this;
    }
    
    MotionEventBuilder& rawXCursorPosition(float rawXCursorPosition) {
        mRawXCursorPosition = rawXCursorPosition;
        return *this;
    }
    
    MotionEventBuilder& rawYCursorPosition(float rawYCursorPosition) {
        mRawYCursorPosition = rawYCursorPosition;
        return *this;
    }
    
    MotionEventBuilder& pointer(PointerBuilder pointer) {
        mPointers.push_back(pointer);
        return *this;
    }
    
    MotionEventBuilder& addFlag(uint32_t flags) {
        mFlags |= flags;
        return *this;
    }
    
    MotionEvent build() {
        std::vector<PointerProperties> pointerProperties;
        std::vector<PointerCoords> pointerCoords;
        for (const PointerBuilder& pointer : mPointers) {
            pointerProperties.push_back(pointer.buildProperties());
            pointerCoords.push_back(pointer.buildCoords());
        }
    
        // Set mouse cursor position for the most common cases to avoid boilerplate.
        if (mSource == AINPUT_SOURCE_MOUSE &&
            !MotionEvent::isValidCursorPosition(mRawXCursorPosition, mRawYCursorPosition) &&
            mPointers.size() == 1) {
            mRawXCursorPosition = pointerCoords[0].getX();
            mRawYCursorPosition = pointerCoords[0].getY();
        }
    
        MotionEvent event;
        ui::Transform identityTransform;
        event.initialize(InputEvent::nextId(), DEVICE_ID, mSource, mDisplayId, INVALID_HMAC,
                         mAction, mActionButton, mFlags, /* edgeFlags */ 0, AMETA_NONE,
                         mButtonState, MotionClassification::NONE, identityTransform,
                         /* xPrecision */ 0, /* yPrecision */ 0, mRawXCursorPosition,
                         mRawYCursorPosition, identityTransform, mEventTime, mEventTime,
                         mPointers.size(), pointerProperties.data(), pointerCoords.data());
    
        return event;
    }
    
private:
    int32_t mAction;
    int32_t mSource;
    nsecs_t mEventTime;
    int32_t mDisplayId{ADISPLAY_ID_DEFAULT};
    int32_t mActionButton{0};
    int32_t mButtonState{0};
    int32_t mFlags{0};
    float mRawXCursorPosition{AMOTION_EVENT_INVALID_CURSOR_POSITION};
    float mRawYCursorPosition{AMOTION_EVENT_INVALID_CURSOR_POSITION};
    
    std::vector<PointerBuilder> mPointers;
};

static InputEventInjectionResult injectMotionEvent(
        const std::unique_ptr<InputDispatcher>& dispatcher, const MotionEvent& event,
        std::chrono::milliseconds injectionTimeout = INJECT_EVENT_TIMEOUT,
        InputEventInjectionSync injectionMode = InputEventInjectionSync::WAIT_FOR_RESULT,
        std::optional<int32_t> targetUid = {}, uint32_t policyFlags = DEFAULT_POLICY_FLAGS) {
    return dispatcher->injectInputEvent(&event, targetUid, injectionMode, injectionTimeout,
                                        policyFlags);
}

static InputEventInjectionResult injectMotionEvent(
        const std::unique_ptr<InputDispatcher>& dispatcher, int32_t action, int32_t source,
        int32_t displayId, const PointF& position = {100, 200},
        const PointF& cursorPosition = {AMOTION_EVENT_INVALID_CURSOR_POSITION,
                                        AMOTION_EVENT_INVALID_CURSOR_POSITION},
        std::chrono::milliseconds injectionTimeout = INJECT_EVENT_TIMEOUT,
        InputEventInjectionSync injectionMode = InputEventInjectionSync::WAIT_FOR_RESULT,
        nsecs_t eventTime = systemTime(SYSTEM_TIME_MONOTONIC),
        std::optional<int32_t> targetUid = {
    MotionEvent event = MotionEventBuilder(action, source)
                                .displayId(displayId)
                                .eventTime(eventTime)
                                .rawXCursorPosition(cursorPosition.x)
                                .rawYCursorPosition(cursorPosition.y)
                                .pointer(PointerBuilder(/* id */ 0, AMOTION_EVENT_TOOL_TYPE_FINGER)
                                                 .x(position.x)
                                                 .y(position.y))
                                .build();

    // Inject event until dispatch out.
    return injectMotionEvent(dispatcher, event, injectionTimeout, injectionMode, targetUid,
                             policyFlags);
}

static InputEventInjectionResult injectMotionDown(
        const std::unique_ptr<InputDispatcher>& dispatcher, int32_t source, int32_t displayId,
        const PointF& location = {100, 200}) {
    return injectMotionEvent(dispatcher, AMOTION_EVENT_ACTION_DOWN, source, displayId, location);
}

static InputEventInjectionResult injectMotionUp(const std::unique_ptr<InputDispatcher>& dispatcher,
                                                int32_t source, int32_t displayId,
                                                const PointF& location = {100, 200}) {
    return injectMotionEvent(dispatcher, AMOTION_EVENT_ACTION_UP, source, displayId, location);
}

static NotifyKeyArgs generateKeyArgs(int32_t action, int32_t displayId = ADISPLAY_ID_NONE) {
    nsecs_t currentTime = systemTime(SYSTEM_TIME_MONOTONIC);
    // Define a valid key event.
    NotifyKeyArgs args(/* id */ 0, currentTime, 0 /*readTime*/, DEVICE_ID, AINPUT_SOURCE_KEYBOARD,
                       displayId, POLICY_FLAG_PASS_TO_USER, action, /* flags */ 0, AKEYCODE_A,
                       KEY_A, AMETA_NONE, currentTime);

    return args;
}

static NotifyMotionArgs generateMotionArgs(int32_t action, int32_t source, int32_t displayId,
                                           const std::vector<PointF>& points) {
    size_t pointerCount = points.size();
    if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_UP) {
        EXPECT_EQ(1U, pointerCount) << "Actions DOWN and UP can only contain a single pointer";
    }

    PointerProperties pointerProperties[pointerCount];
    PointerCoords pointerCoords[pointerCount];

    for (size_t i = 0; i < pointerCount; i++) {
        pointerProperties[i].clear();
        pointerProperties[i].id = i;
        pointerProperties[i].toolType = AMOTION_EVENT_TOOL_TYPE_FINGER;

        pointerCoords[i].clear();
        pointerCoords[i].setAxisValue(AMOTION_EVENT_AXIS_X, points[i].x);
        pointerCoords[i].setAxisValue(AMOTION_EVENT_AXIS_Y, points[i].y);
    }

    nsecs_t currentTime = systemTime(SYSTEM_TIME_MONOTONIC);
    // Define a valid motion event.
    NotifyMotionArgs args(/* id */ 0, currentTime, 0 /*readTime*/, DEVICE_ID, source, displayId,
                          POLICY_FLAG_PASS_TO_USER, action, /* actionButton */ 0, /* flags */ 0,
                          AMETA_NONE, /* buttonState */ 0, MotionClassification::NONE,
                          AMOTION_EVENT_EDGE_FLAG_NONE, pointerCount, pointerProperties,
                          pointerCoords, /* xPrecision */ 0, /* yPrecision */ 0,
                          AMOTION_EVENT_INVALID_CURSOR_POSITION,
                          AMOTION_EVENT_INVALID_CURSOR_POSITION, currentTime, /* videoFrames */ {});

    return args;
}

static NotifyMotionArgs generateTouchArgs(int32_t action, const std::vector<PointF>& points) {
    return generateMotionArgs(action, AINPUT_SOURCE_TOUCHSCREEN, DISPLAY_ID, points);
}

static NotifyMotionArgs generateMotionArgs(int32_t action, int32_t source, int32_t displayId) {
    return generateMotionArgs(action, source, displayId, {PointF{100, 200}});
}

static NotifyPointerCaptureChangedArgs generatePointerCaptureChangedArgs(
        const PointerCaptureRequest& request) {
    return NotifyPointerCaptureChangedArgs(/* id */ 0, systemTime(SYSTEM_TIME_MONOTONIC), request);
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

class ShouldSplitTouchFixture : public InputDispatcherTest,
                                public ::testing::WithParamInterface<bool> {};

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

/**
 * Ensure the correct coordinate spaces are used by InputDispatcher.
 *
 * InputDispatcher works in the display space, so its coordinate system is relative to the display
 * panel. Windows get events in the window space, and get raw coordinates in the logical display
 * space.
 */
class InputDispatcherDisplayProjectionTest : public InputDispatcherTest {
public:
    void SetUp() override {
        InputDispatcherTest::SetUp();
        mDisplayInfos.clear();
        mWindowInfos.clear();
    }
    
    void addDisplayInfo(int displayId, const ui::Transform& transform) {
        gui::DisplayInfo info;
        info.displayId = displayId;
        info.transform = transform;
        mDisplayInfos.push_back(std::move(info));
        mDispatcher->onWindowInfosChanged(mWindowInfos, mDisplayInfos);
    }
    
    void addWindow(const sp<WindowInfoHandle>& windowHandle) {
        mWindowInfos.push_back(*windowHandle->getInfo());
        mDispatcher->onWindowInfosChanged(mWindowInfos, mDisplayInfos);
    }
    
    // Set up a test scenario where the display has a scaled projection and there are two windows
    // on the display.
    std::pair<sp<FakeWindowHandle>, sp<FakeWindowHandle>> setupScaledDisplayScenario() {
        // The display has a projection that has a scale factor of 2 and 4 in the x and y directions
        // respectively.
        ui::Transform displayTransform;
        displayTransform.set(2, 0, 0, 4);
        addDisplayInfo(ADISPLAY_ID_DEFAULT, displayTransform);
    
        std::shared_ptr<FakeApplicationHandle> application =
                std::make_shared<FakeApplicationHandle>();
    
        // Add two windows to the display. Their frames are represented in the display space.
        sp<FakeWindowHandle> firstWindow =
                sp<FakeWindowHandle>::make(application, mDispatcher, "First Window",
                                           ADISPLAY_ID_DEFAULT);
        firstWindow->setFrame(Rect(0, 0, 100, 200), displayTransform);
        addWindow(firstWindow);
    
        sp<FakeWindowHandle> secondWindow =
                sp<FakeWindowHandle>::make(application, mDispatcher, "Second Window",
                                           ADISPLAY_ID_DEFAULT);
        secondWindow->setFrame(Rect(100, 200, 200, 400), displayTransform);
        addWindow(secondWindow);
        return {std::move(firstWindow), std::move(secondWindow)};
    }
    
private:
    std::vector<gui::DisplayInfo> mDisplayInfos;
    std::vector<gui::WindowInfo> mWindowInfos;
};

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

using TransferFunction = std::function<bool(const std::unique_ptr<InputDispatcher>& dispatcher,
                                            sp<IBinder>, sp<IBinder>)>;

class TransferTouchFixture : public InputDispatcherTest,
                             public ::testing::WithParamInterface<TransferFunction> {};

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

class FakeMonitorReceiver {
public:
    FakeMonitorReceiver(const std::unique_ptr<InputDispatcher>& dispatcher, const std::string name,
                        int32_t displayId) {
        base::Result<std::unique_ptr<InputChannel>> channel =
                dispatcher->createInputMonitor(displayId, name, MONITOR_PID);
        mInputReceiver = std::make_unique<FakeInputReceiver>(std::move(*channel), name);
    }
    
    sp<IBinder> getToken() { return mInputReceiver->getToken(){ return mInputReceiver->getToken(); }
    
    void consumeKeyDown(int32_t expectedDisplayId, int32_t expectedFlags = 0) {
        mInputReceiver->consumeEvent(AINPUT_EVENT_TYPE_KEY, AKEY_EVENT_ACTION_DOWN,
                                     expectedDisplayId, expectedFlags);
    }
    
    std::optional<int32_t> receiveEvent() { return mInputReceiver->receiveEvent(){ return mInputReceiver->receiveEvent(); }
    
    void finishEvent(uint32_t consumeSeq) { return mInputReceiver->finishEvent(consumeSeq){ return mInputReceiver->finishEvent(consumeSeq); }
    
    void consumeMotionDown(int32_t expectedDisplayId, int32_t expectedFlags = 0) {
        mInputReceiver->consumeEvent(AINPUT_EVENT_TYPE_MOTION, AMOTION_EVENT_ACTION_DOWN,
                                     expectedDisplayId, expectedFlags);
    }
    
    void consumeMotionMove(int32_t expectedDisplayId, int32_t expectedFlags = 0) {
        mInputReceiver->consumeEvent(AINPUT_EVENT_TYPE_MOTION, AMOTION_EVENT_ACTION_MOVE,
                                     expectedDisplayId, expectedFlags);
    }
    
    void consumeMotionUp(int32_t expectedDisplayId, int32_t expectedFlags = 0) {
        mInputReceiver->consumeEvent(AINPUT_EVENT_TYPE_MOTION, AMOTION_EVENT_ACTION_UP,
                                     expectedDisplayId, expectedFlags);
    }
    
    void consumeMotionCancel(int32_t expectedDisplayId, int32_t expectedFlags = 0) {
        mInputReceiver->consumeEvent(AINPUT_EVENT_TYPE_MOTION, AMOTION_EVENT_ACTION_CANCEL,
                                     expectedDisplayId, expectedFlags);
    }
    
    void consumeMotionPointerDown(int32_t pointerIdx) {
        int32_t action = AMOTION_EVENT_ACTION_POINTER_DOWN |
                (pointerIdx << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        mInputReceiver->consumeEvent(AINPUT_EVENT_TYPE_MOTION, action, ADISPLAY_ID_DEFAULT,
                                     0 /*expectedFlags*/);
    }
    
    MotionEvent* consumeMotion() {
        InputEvent* event = mInputReceiver->consume();
        if (!event) {
            ADD_FAILURE() << "No event was produced";
            return nullptr;
        }
        if (event->getType() != AINPUT_EVENT_TYPE_MOTION) {
            ADD_FAILURE() << "Received event of type " << event->getType() << " instead of motion";
            return nullptr;
        }
        return static_cast<MotionEvent*>(event);
    }
    
    void assertNoEvents() { mInputReceiver->assertNoEvents(){ mInputReceiver->assertNoEvents(); }
    
private:
    std::unique_ptr<FakeInputReceiver> mInputReceiver;
};

using InputDispatcherMonitorTest = InputDispatcherTest;

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

class InputDispatcherKeyRepeatTest : public InputDispatcherTest {
protected:
static constexpr nsecs_t KEY_REPEAT_TIMEOUT = 40 * 1000000;                                                                // 40 ms
static constexpr nsecs_t KEY_REPEAT_DELAY = 40 * 1000000;                                                                // 40 ms
    std::shared_ptr<FakeApplicationHandle> mApp;
    sp<FakeWindowHandle> mWindow;
    
    virtual void SetUp() override {
        mFakePolicy = sp<FakeInputDispatcherPolicy>::make();
        mFakePolicy->setKeyRepeatConfiguration(KEY_REPEAT_TIMEOUT, KEY_REPEAT_DELAY);
        mDispatcher = std::make_unique<InputDispatcher>(mFakePolicy);
        mDispatcher->setInputDispatchMode(/*enabled*/ true, /*frozen*/ false);
        ASSERT_EQ(OK, mDispatcher->start());
    
        setUpWindow();
    }
    
    void setUpWindow() {
        mApp = std::make_shared<FakeApplicationHandle>();
        mWindow = sp<FakeWindowHandle>::make(mApp, mDispatcher, "Fake Window", ADISPLAY_ID_DEFAULT);
    
        mWindow->setFocusable(true);
        mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mWindow}}});
        setFocusedWindow(mWindow);
        mWindow->consumeFocusEvent(true);
    }
    
    void sendAndConsumeKeyDown(int32_t deviceId) {
        NotifyKeyArgs keyArgs = generateKeyArgs(AKEY_EVENT_ACTION_DOWN, ADISPLAY_ID_DEFAULT);
        keyArgs.deviceId = deviceId;
        keyArgs.policyFlags |= POLICY_FLAG_TRUSTED; // Otherwise it won't generate repeat event
        mDispatcher->notifyKey(&keyArgs);
    
        // Window should receive key down event.
        mWindow->consumeKeyDown(ADISPLAY_ID_DEFAULT);
    }
    
    void expectKeyRepeatOnce(int32_t repeatCount) {
        SCOPED_TRACE(StringPrintf("Checking event with repeat count %" PRId32, repeatCount));
        InputEvent* repeatEvent = mWindow->consume();
        ASSERT_NE(nullptr, repeatEvent);
    
        uint32_t eventType = repeatEvent->getType();
        ASSERT_EQ(AINPUT_EVENT_TYPE_KEY, eventType);
    
        KeyEvent* repeatKeyEvent = static_cast<KeyEvent*>(repeatEvent);
        uint32_t eventAction = repeatKeyEvent->getAction();
        EXPECT_EQ(AKEY_EVENT_ACTION_DOWN, eventAction);
        EXPECT_EQ(repeatCount, repeatKeyEvent->getRepeatCount());
    }
    
    void sendAndConsumeKeyUp(int32_t deviceId) {
        NotifyKeyArgs keyArgs = generateKeyArgs(AKEY_EVENT_ACTION_UP, ADISPLAY_ID_DEFAULT);
        keyArgs.deviceId = deviceId;
        keyArgs.policyFlags |= POLICY_FLAG_TRUSTED; // Unless it won't generate repeat event
        mDispatcher->notifyKey(&keyArgs);
    
        // Window should receive key down event.
        mWindow->consumeEvent(AINPUT_EVENT_TYPE_KEY, AKEY_EVENT_ACTION_UP, ADISPLAY_ID_DEFAULT,
                              0 /*expectedFlags*/);
    }
};

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

/* Test InputDispatcher for MultiDisplay */
class InputDispatcherFocusOnTwoDisplaysTest : public InputDispatcherTest {
public:
    virtual void SetUp() override {
        InputDispatcherTest::SetUp();
    
        application1 = std::make_shared<FakeApplicationHandle>();
        windowInPrimary =
                sp<FakeWindowHandle>::make(application1, mDispatcher, "D_1", ADISPLAY_ID_DEFAULT);
    
        // Set focus window for primary display, but focused display would be second one.
        mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application1);
        windowInPrimary->setFocusable(true);
        mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {windowInPrimary}}});
        setFocusedWindow(windowInPrimary);
        windowInPrimary->consumeFocusEvent(true);
    
        application2 = std::make_shared<FakeApplicationHandle>();
        windowInSecondary =
                sp<FakeWindowHandle>::make(application2, mDispatcher, "D_2", SECOND_DISPLAY_ID);
        // Set focus to second display window.
        // Set focus display to second one.
        mDispatcher->setFocusedDisplay(SECOND_DISPLAY_ID);
        // Set focus window for second display.
        mDispatcher->setFocusedApplication(SECOND_DISPLAY_ID, application2);
        windowInSecondary->setFocusable(true);
        mDispatcher->setInputWindows({{SECOND_DISPLAY_ID, {windowInSecondary}}});
        setFocusedWindow(windowInSecondary);
        windowInSecondary->consumeFocusEvent(true);
    }
    
    virtual void TearDown() override {
        InputDispatcherTest::TearDown();
    
        application1.reset();
        windowInPrimary.clear();
        application2.reset();
        windowInSecondary.clear();
    }
    
protected:
    std::shared_ptr<FakeApplicationHandle> application1;
    sp<FakeWindowHandle> windowInPrimary;
    std::shared_ptr<FakeApplicationHandle> application2;
    sp<FakeWindowHandle> windowInSecondary;
};

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

class InputFilterTest : public InputDispatcherTest {
protected:
    void testNotifyMotion(int32_t displayId, bool expectToBeFiltered,
                          const ui::Transform& transform = ui::Transform()) {
        NotifyMotionArgs motionArgs;
    
        motionArgs =
                generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN, displayId);
        mDispatcher->notifyMotion(&motionArgs);
        motionArgs =
                generateMotionArgs(AMOTION_EVENT_ACTION_UP, AINPUT_SOURCE_TOUCHSCREEN, displayId);
        mDispatcher->notifyMotion(&motionArgs);
        ASSERT_TRUE(mDispatcher->waitForIdle());
        if (expectToBeFiltered) {
            const auto xy = transform.transform(motionArgs.pointerCoords->getXYValue());
            mFakePolicy->assertFilterInputEventWasCalled(motionArgs, xy);
        } else {
            mFakePolicy->assertFilterInputEventWasNotCalled();
        }
    }
    
    void testNotifyKey(bool expectToBeFiltered) {
        NotifyKeyArgs keyArgs;
    
        keyArgs = generateKeyArgs(AKEY_EVENT_ACTION_DOWN);
        mDispatcher->notifyKey(&keyArgs);
        keyArgs = generateKeyArgs(AKEY_EVENT_ACTION_UP);
        mDispatcher->notifyKey(&keyArgs);
        ASSERT_TRUE(mDispatcher->waitForIdle());
    
        if (expectToBeFiltered) {
            mFakePolicy->assertFilterInputEventWasCalled(keyArgs);
        } else {
            mFakePolicy->assertFilterInputEventWasNotCalled();
        }
    }
};

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

class InputFilterInjectionPolicyTest : public InputDispatcherTest {
protected:
    virtual void SetUp() override {
        InputDispatcherTest::SetUp();
    
        /**
         * We don't need to enable input filter to test the injected event policy, but we enabled it
         * here to make the tests more realistic, since this policy only matters when inputfilter is
         * on.
         */
        mDispatcher->setInputFilterEnabled(true);
    
        std::shared_ptr<InputApplicationHandle> application =
                std::make_shared<FakeApplicationHandle>();
        mWindow = sp<FakeWindowHandle>::make(application, mDispatcher, "Test Window",
                                             ADISPLAY_ID_DEFAULT);
    
        mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
        mWindow->setFocusable(true);
        mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mWindow}}});
        setFocusedWindow(mWindow);
        mWindow->consumeFocusEvent(true);
    }
    
    void testInjectedKey(int32_t policyFlags, int32_t injectedDeviceId, int32_t resolvedDeviceId,
                         int32_t flags) {
        KeyEvent event;
    
        const nsecs_t eventTime = systemTime(SYSTEM_TIME_MONOTONIC);
        event.initialize(InputEvent::nextId(), injectedDeviceId, AINPUT_SOURCE_KEYBOARD,
                         ADISPLAY_ID_NONE, INVALID_HMAC, AKEY_EVENT_ACTION_DOWN, 0, AKEYCODE_A,
                         KEY_A, AMETA_NONE, 0 /*repeatCount*/, eventTime, eventTime);
        const int32_t additionalPolicyFlags =
                POLICY_FLAG_PASS_TO_USER | POLICY_FLAG_DISABLE_KEY_REPEAT;
        ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
                  mDispatcher->injectInputEvent(&event, {} /*targetUid*/,
                                                InputEventInjectionSync::WAIT_FOR_RESULT, 10ms,
                                                policyFlags | additionalPolicyFlags));
    
        InputEvent* received = mWindow->consume();
        ASSERT_NE(nullptr, received);
        ASSERT_EQ(resolvedDeviceId, received->getDeviceId());
        ASSERT_EQ(received->getType(), AINPUT_EVENT_TYPE_KEY);
        KeyEvent& keyEvent = static_cast<KeyEvent&>(*received);
        ASSERT_EQ(flags, keyEvent.getFlags());
    }
    
    void testInjectedMotion(int32_t policyFlags, int32_t injectedDeviceId, int32_t resolvedDeviceId,
                            int32_t flags) {
        MotionEvent event;
        PointerProperties pointerProperties[1];
        PointerCoords pointerCoords[1];
        pointerProperties[0].clear();
        pointerProperties[0].id = 0;
        pointerCoords[0].clear();
        pointerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_X, 300);
        pointerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_Y, 400);
    
        ui::Transform identityTransform;
        const nsecs_t eventTime = systemTime(SYSTEM_TIME_MONOTONIC);
        event.initialize(InputEvent::nextId(), injectedDeviceId, AINPUT_SOURCE_TOUCHSCREEN,
                         DISPLAY_ID, INVALID_HMAC, AMOTION_EVENT_ACTION_DOWN, 0, 0,
                         AMOTION_EVENT_EDGE_FLAG_NONE, AMETA_NONE, 0, MotionClassification::NONE,
                         identityTransform, 0, 0, AMOTION_EVENT_INVALID_CURSOR_POSITION,
                         AMOTION_EVENT_INVALID_CURSOR_POSITION, identityTransform, eventTime,
                         eventTime,
                         /*pointerCount*/ 1, pointerProperties, pointerCoords);
    
        const int32_t additionalPolicyFlags = POLICY_FLAG_PASS_TO_USER;
        ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
                  mDispatcher->injectInputEvent(&event, {} /*targetUid*/,
                                                InputEventInjectionSync::WAIT_FOR_RESULT, 10ms,
                                                policyFlags | additionalPolicyFlags));
    
        InputEvent* received = mWindow->consume();
        ASSERT_NE(nullptr, received);
        ASSERT_EQ(resolvedDeviceId, received->getDeviceId());
        ASSERT_EQ(received->getType(), AINPUT_EVENT_TYPE_MOTION);
        MotionEvent& motionEvent = static_cast<MotionEvent&>(*received);
        ASSERT_EQ(flags, motionEvent.getFlags());
    }
    
private:
    sp<FakeWindowHandle> mWindow;
};

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

class InputDispatcherOnPointerDownOutsideFocus : public InputDispatcherTest {
    virtual void SetUp() override {
        InputDispatcherTest::SetUp();
    
        std::shared_ptr<FakeApplicationHandle> application =
                std::make_shared<FakeApplicationHandle>();
        mUnfocusedWindow =
                sp<FakeWindowHandle>::make(application, mDispatcher, "Top", ADISPLAY_ID_DEFAULT);
        mUnfocusedWindow->setFrame(Rect(0, 0, 30, 30));
    
        mFocusedWindow =
                sp<FakeWindowHandle>::make(application, mDispatcher, "Second", ADISPLAY_ID_DEFAULT);
        mFocusedWindow->setFrame(Rect(50, 50, 100, 100));
    
        // Set focused application.
        mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
        mFocusedWindow->setFocusable(true);
    
        // Expect one focus window exist in display.
        mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mUnfocusedWindow, mFocusedWindow}}});
        setFocusedWindow(mFocusedWindow);
        mFocusedWindow->consumeFocusEvent(true);
    }
    
    virtual void TearDown() override {
        InputDispatcherTest::TearDown();
    
        mUnfocusedWindow.clear();
        mFocusedWindow.clear();
    }
    
protected:
    sp<FakeWindowHandle> mUnfocusedWindow;
    sp<FakeWindowHandle> mFocusedWindow;
    static constexpr PointF FOCUSED_WINDOW_TOUCH_POINT = {60, 60};
};

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

// These tests ensures we can send touch events to a single client when there are multiple input
// windows that point to the same client token.
class InputDispatcherMultiWindowSameTokenTests : public InputDispatcherTest {
    virtual void SetUp() override {
        InputDispatcherTest::SetUp();
    
        std::shared_ptr<FakeApplicationHandle> application =
                std::make_shared<FakeApplicationHandle>();
        mWindow1 = sp<FakeWindowHandle>::make(application, mDispatcher, "Fake Window 1",
                                              ADISPLAY_ID_DEFAULT);
        mWindow1->setFrame(Rect(0, 0, 100, 100));
    
        mWindow2 = sp<FakeWindowHandle>::make(application, mDispatcher, "Fake Window 2",
                                              ADISPLAY_ID_DEFAULT, mWindow1->getToken());
        mWindow2->setFrame(Rect(100, 100, 200, 200));
    
        mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mWindow1, mWindow2}}});
    }
    
protected:
    sp<FakeWindowHandle> mWindow1;
    sp<FakeWindowHandle> mWindow2;
    
    // Helper function to convert the point from screen coordinates into the window's space
    static PointF getPointInWindow(const WindowInfo* windowInfo, const PointF& point) {
        vec2 vals = windowInfo->transform.transform(point.x, point.y);
        return {vals.x, vals.y};
    }
    
    void consumeMotionEvent(const sp<FakeWindowHandle>& window, int32_t expectedAction,
                            const std::vector<PointF>& points) {
        const std::string name = window->getName();
        InputEvent* event = window->consume();
    
        ASSERT_NE(nullptr, event) << name.c_str()
                                  << ": consumer should have returned non-NULL event.";
    
        ASSERT_EQ(AINPUT_EVENT_TYPE_MOTION, event->getType())
                << name.c_str() << "expected " << inputEventTypeToString(AINPUT_EVENT_TYPE_MOTION)
                << " event, got " << inputEventTypeToString(event->getType()) << " event";
    
        const MotionEvent& motionEvent = static_cast<const MotionEvent&>(*event);
        assertMotionAction(expectedAction, motionEvent.getAction());
        ASSERT_EQ(points.size(), motionEvent.getPointerCount());
    
        for (size_t i = 0; i < points.size(); i++) {
            float expectedX = points[i].x;
            float expectedY = points[i].y;
    
            EXPECT_EQ(expectedX, motionEvent.getX(i))
                    << "expected " << expectedX << " for x[" << i << "] coord of " << name.c_str()
                    << ", got " << motionEvent.getX(i);
            EXPECT_EQ(expectedY, motionEvent.getY(i))
                    << "expected " << expectedY << " for y[" << i << "] coord of " << name.c_str()
                    << ", got " << motionEvent.getY(i);
        }
    }
    
    void touchAndAssertPositions(int32_t action, const std::vector<PointF>& touchedPoints,
                                 std::vector<PointF> expectedPoints) {
        NotifyMotionArgs motionArgs = generateMotionArgs(action, AINPUT_SOURCE_TOUCHSCREEN,
                                                         ADISPLAY_ID_DEFAULT, touchedPoints);
        mDispatcher->notifyMotion(&motionArgs);
    
        // Always consume from window1 since it's the window that has the InputReceiver
        consumeMotionEvent(mWindow1, action, expectedPoints);
    }
};

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

class InputDispatcherSingleWindowAnr : public InputDispatcherTest {
    virtual void SetUp() override {
        InputDispatcherTest::SetUp();
    
        mApplication = std::make_shared<FakeApplicationHandle>();
        mApplication->setDispatchingTimeout(20ms);
        mWindow = sp<FakeWindowHandle>::make(mApplication, mDispatcher, "TestWindow",
                                             ADISPLAY_ID_DEFAULT);
        mWindow->setFrame(Rect(0, 0, 30, 30));
        mWindow->setDispatchingTimeout(30ms);
        mWindow->setFocusable(true);
    
        // Set focused application.
        mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, mApplication);
    
        mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mWindow}}});
        setFocusedWindow(mWindow);
        mWindow->consumeFocusEvent(true);
    }
    
    virtual void TearDown() override {
        InputDispatcherTest::TearDown();
        mWindow.clear();
    }
    
protected:
    std::shared_ptr<FakeApplicationHandle> mApplication;
    sp<FakeWindowHandle> mWindow;
    static constexpr PointF WINDOW_LOCATION = {20, 20};
    
    void tapOnWindow() {
        ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
                  injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                                   WINDOW_LOCATION));
        ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
                  injectMotionUp(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                                 WINDOW_LOCATION));
    }
    
    sp<FakeWindowHandle> addSpyWindow() {
        sp<FakeWindowHandle> spy =
                sp<FakeWindowHandle>::make(mApplication, mDispatcher, "Spy", ADISPLAY_ID_DEFAULT);
        spy->setTrustedOverlay(true);
        spy->setFocusable(false);
        spy->setSpy(true);
        spy->setDispatchingTimeout(30ms);
        mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {spy, mWindow}}});
        return spy;
    }
};

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

class InputDispatcherMultiWindowAnr : public InputDispatcherTest {
    virtual void SetUp() override {
        InputDispatcherTest::SetUp();
    
        mApplication = std::make_shared<FakeApplicationHandle>();
        mApplication->setDispatchingTimeout(10ms);
        mUnfocusedWindow = sp<FakeWindowHandle>::make(mApplication, mDispatcher, "Unfocused",
                                                      ADISPLAY_ID_DEFAULT);
        mUnfocusedWindow->setFrame(Rect(0, 0, 30, 30));
        // Adding FLAG_WATCH_OUTSIDE_TOUCH to receive ACTION_OUTSIDE when another window is tapped
        mUnfocusedWindow->setWatchOutsideTouch(true);
    
        mFocusedWindow = sp<FakeWindowHandle>::make(mApplication, mDispatcher, "Focused",
                                                    ADISPLAY_ID_DEFAULT);
        mFocusedWindow->setDispatchingTimeout(30ms);
        mFocusedWindow->setFrame(Rect(50, 50, 100, 100));
    
        // Set focused application.
        mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, mApplication);
        mFocusedWindow->setFocusable(true);
    
        // Expect one focus window exist in display.
        mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mUnfocusedWindow, mFocusedWindow}}});
        setFocusedWindow(mFocusedWindow);
        mFocusedWindow->consumeFocusEvent(true);
    }
    
    virtual void TearDown() override {
        InputDispatcherTest::TearDown();
    
        mUnfocusedWindow.clear();
        mFocusedWindow.clear();
    }
    
protected:
    std::shared_ptr<FakeApplicationHandle> mApplication;
    sp<FakeWindowHandle> mUnfocusedWindow;
    sp<FakeWindowHandle> mFocusedWindow;
    static constexpr PointF UNFOCUSED_WINDOW_LOCATION = {20, 20};
    static constexpr PointF FOCUSED_WINDOW_LOCATION = {75, 75};
    static constexpr PointF LOCATION_OUTSIDE_ALL_WINDOWS = {40, 40};
    
    void tapOnFocusedWindow() { tap(FOCUSED_WINDOW_LOCATION); }
    
    void tapOnUnfocusedWindow() { tap(UNFOCUSED_WINDOW_LOCATION); }
    
private:
    void tap(const PointF& location) {
        ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
                  injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                                   location));
        ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
                  injectMotionUp(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                                 location));
    }
};

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

// These tests ensure we cannot send touch events to a window that's positioned behind a window
// that has feature NO_INPUT_CHANNEL.
// Layout:
//   Top (closest to user)
//       mNoInputWindow (above all windows)
//       mBottomWindow
//   Bottom (furthest from user)
class InputDispatcherMultiWindowOcclusionTests : public InputDispatcherTest {
    virtual void SetUp() override {
        InputDispatcherTest::SetUp();
    
        mApplication = std::make_shared<FakeApplicationHandle>();
        mNoInputWindow =
                sp<FakeWindowHandle>::make(mApplication, mDispatcher,
                                           "Window without input channel", ADISPLAY_ID_DEFAULT,
                                           std::make_optional<sp<IBinder>>(nullptr) /*token*/);
        mNoInputWindow->setNoInputChannel(true);
        mNoInputWindow->setFrame(Rect(0, 0, 100, 100));
        // It's perfectly valid for this window to not have an associated input channel
    
        mBottomWindow = sp<FakeWindowHandle>::make(mApplication, mDispatcher, "Bottom window",
                                                   ADISPLAY_ID_DEFAULT);
        mBottomWindow->setFrame(Rect(0, 0, 100, 100));
    
        mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mNoInputWindow, mBottomWindow}}});
    }
    
protected:
    std::shared_ptr<FakeApplicationHandle> mApplication;
    sp<FakeWindowHandle> mNoInputWindow;
    sp<FakeWindowHandle> mBottomWindow;
};

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

class InputDispatcherMirrorWindowFocusTests : public InputDispatcherTest {
protected:
    std::shared_ptr<FakeApplicationHandle> mApp;
    sp<FakeWindowHandle> mWindow;
    sp<FakeWindowHandle> mMirror;
    
    virtual void SetUp() override {
        InputDispatcherTest::SetUp();
        mApp = std::make_shared<FakeApplicationHandle>();
        mWindow = sp<FakeWindowHandle>::make(mApp, mDispatcher, "TestWindow", ADISPLAY_ID_DEFAULT);
        mMirror = sp<FakeWindowHandle>::make(mApp, mDispatcher, "TestWindowMirror",
                                             ADISPLAY_ID_DEFAULT, mWindow->getToken());
        mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, mApp);
        mWindow->setFocusable(true);
        mMirror->setFocusable(true);
        mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mWindow, mMirror}}});
    }
};

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

class InputDispatcherPointerCaptureTests : public InputDispatcherTest {
protected:
    std::shared_ptr<FakeApplicationHandle> mApp;
    sp<FakeWindowHandle> mWindow;
    sp<FakeWindowHandle> mSecondWindow;
    
    void SetUp() override {
        InputDispatcherTest::SetUp();
        mApp = std::make_shared<FakeApplicationHandle>();
        mWindow = sp<FakeWindowHandle>::make(mApp, mDispatcher, "TestWindow", ADISPLAY_ID_DEFAULT);
        mWindow->setFocusable(true);
        mSecondWindow =
                sp<FakeWindowHandle>::make(mApp, mDispatcher, "TestWindow2", ADISPLAY_ID_DEFAULT);
        mSecondWindow->setFocusable(true);
    
        mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, mApp);
        mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mWindow, mSecondWindow}}});
    
        setFocusedWindow(mWindow);
        mWindow->consumeFocusEvent(true);
    }
    
    void notifyPointerCaptureChanged(const PointerCaptureRequest& request) {
        const NotifyPointerCaptureChangedArgs args = generatePointerCaptureChangedArgs(request);
        mDispatcher->notifyPointerCaptureChanged(&args);
    }
    
    PointerCaptureRequest requestAndVerifyPointerCapture(const sp<FakeWindowHandle>& window,
                                                         bool enabled) {
        mDispatcher->requestPointerCapture(window->getToken(), enabled);
        auto request = mFakePolicy->assertSetPointerCaptureCalled(enabled);
        notifyPointerCaptureChanged(request);
        window->consumeCaptureEvent(enabled);
        return request;
    }
};

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

class InputDispatcherUntrustedTouchesTest : public InputDispatcherTest {
protected:
    constexpr static const float MAXIMUM_OBSCURING_OPACITY = 0.8;
    
    constexpr static const float OPACITY_ABOVE_THRESHOLD = 0.9;
    static_assert(OPACITY_ABOVE_THRESHOLD > MAXIMUM_OBSCURING_OPACITY);
    
    constexpr static const float OPACITY_BELOW_THRESHOLD = 0.7;
    static_assert(OPACITY_BELOW_THRESHOLD < MAXIMUM_OBSCURING_OPACITY);
    
    // When combined twice, ie 1 - (1 - 0.5)*(1 - 0.5) = 0.75 < 8, is still below the threshold
    constexpr static const float OPACITY_FAR_BELOW_THRESHOLD = 0.5;
    static_assert(OPACITY_FAR_BELOW_THRESHOLD < MAXIMUM_OBSCURING_OPACITY);
    static_assert(1 - (1 - OPACITY_FAR_BELOW_THRESHOLD) * (1 - OPACITY_FAR_BELOW_THRESHOLD) <
                  MAXIMUM_OBSCURING_OPACITY);
    
    static const int32_t TOUCHED_APP_UID = 10001;
    static const int32_t APP_B_UID = 10002;
    static const int32_t APP_C_UID = 10003;
    
    sp<FakeWindowHandle> mTouchWindow;
    
    virtual void SetUp() override {
        InputDispatcherTest::SetUp();
        mTouchWindow = getWindow(TOUCHED_APP_UID, "Touched");
        mDispatcher->setMaximumObscuringOpacityForTouch(MAXIMUM_OBSCURING_OPACITY);
    }
    
    virtual void TearDown() override {
        InputDispatcherTest::TearDown();
        mTouchWindow.clear();
    }
    
    sp<FakeWindowHandle> getOccludingWindow(int32_t uid, std::string name, TouchOcclusionMode mode,
                                            float alpha = 1.0f) {
        sp<FakeWindowHandle> window = getWindow(uid, name);
        window->setTouchable(false);
        window->setTouchOcclusionMode(mode);
        window->setAlpha(alpha);
        return window;
    }
    
    sp<FakeWindowHandle> getWindow(int32_t uid, std::string name) {
        std::shared_ptr<FakeApplicationHandle> app = std::make_shared<FakeApplicationHandle>();
        sp<FakeWindowHandle> window =
                sp<FakeWindowHandle>::make(app, mDispatcher, name, ADISPLAY_ID_DEFAULT);
        // Generate an arbitrary PID based on the UID
        window->setOwnerInfo(1777 + (uid % 10000), uid);
        return window;
    }
    
    void touch(const std::vector<PointF>& points = {PointF{100, 200}}) {
        NotifyMotionArgs args =
                generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                                   ADISPLAY_ID_DEFAULT, points);
        mDispatcher->notifyMotion(&args);
    }
};

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

class InputDispatcherDragTests : public InputDispatcherTest {
protected:
    std::shared_ptr<FakeApplicationHandle> mApp;
    sp<FakeWindowHandle> mWindow;
    sp<FakeWindowHandle> mSecondWindow;
    sp<FakeWindowHandle> mDragWindow;
    sp<FakeWindowHandle> mSpyWindow;
    // Mouse would force no-split, set the id as non-zero to verify if drag state could track it.
    static constexpr int32_t MOUSE_POINTER_ID = 1;
    
    void SetUp() override {
        InputDispatcherTest::SetUp();
        mApp = std::make_shared<FakeApplicationHandle>();
        mWindow = sp<FakeWindowHandle>::make(mApp, mDispatcher, "TestWindow", ADISPLAY_ID_DEFAULT);
        mWindow->setFrame(Rect(0, 0, 100, 100));
    
        mSecondWindow =
                sp<FakeWindowHandle>::make(mApp, mDispatcher, "TestWindow2", ADISPLAY_ID_DEFAULT);
        mSecondWindow->setFrame(Rect(100, 0, 200, 100));
    
        mSpyWindow =
                sp<FakeWindowHandle>::make(mApp, mDispatcher, "SpyWindow", ADISPLAY_ID_DEFAULT);
        mSpyWindow->setSpy(true);
        mSpyWindow->setTrustedOverlay(true);
        mSpyWindow->setFrame(Rect(0, 0, 200, 100));
    
        mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, mApp);
        mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mSpyWindow, mWindow, mSecondWindow}}});
    }
    
    void injectDown(int fromSource = AINPUT_SOURCE_TOUCHSCREEN) {
        switch (fromSource) {
            case AINPUT_SOURCE_TOUCHSCREEN:
                ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
                          injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN,
                                           ADISPLAY_ID_DEFAULT, {50, 50}))
                        << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
                break;
            case AINPUT_SOURCE_STYLUS:
                ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
                          injectMotionEvent(
                                  mDispatcher,
                                  MotionEventBuilder(AMOTION_EVENT_ACTION_DOWN,
                                                     AINPUT_SOURCE_STYLUS)
                                          .buttonState(AMOTION_EVENT_BUTTON_STYLUS_PRIMARY)
                                          .pointer(PointerBuilder(0, AMOTION_EVENT_TOOL_TYPE_STYLUS)
                                                           .x(50)
                                                           .y(50))
                                          .build()));
                break;
            case AINPUT_SOURCE_MOUSE:
                ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
                          injectMotionEvent(
                                  mDispatcher,
                                  MotionEventBuilder(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_MOUSE)
                                          .buttonState(AMOTION_EVENT_BUTTON_PRIMARY)
                                          .pointer(PointerBuilder(MOUSE_POINTER_ID,
                                                                  AMOTION_EVENT_TOOL_TYPE_MOUSE)
                                                           .x(50)
                                                           .y(50))
                                          .build()));
                break;
            default:
                FAIL() << "Source " << fromSource << " doesn't support drag and drop";
        }
    
        // Window should receive motion event.
        mWindow->consumeMotionDown(ADISPLAY_ID_DEFAULT);
        // Spy window should also receive motion event
        mSpyWindow->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    }
    
    // Start performing drag, we will create a drag window and transfer touch to it.
    // @param sendDown : if true, send a motion down on first window before perform drag and drop.
    // Returns true on success.
    bool startDrag(bool sendDown = true, int fromSource = AINPUT_SOURCE_TOUCHSCREEN) {
        if (sendDown) {
            injectDown(fromSource);
        }
    
        // The drag window covers the entire display
        mDragWindow =
                sp<FakeWindowHandle>::make(mApp, mDispatcher, "DragWindow", ADISPLAY_ID_DEFAULT);
        mDragWindow->setTouchableRegion(Region{{0, 0, 0, 0}});
        mDispatcher->setInputWindows(
                {{ADISPLAY_ID_DEFAULT, {mDragWindow, mSpyWindow, mWindow, mSecondWindow}}});
    
        // Transfer touch focus to the drag window
        bool transferred =
                mDispatcher->transferTouchFocus(mWindow->getToken(), mDragWindow->getToken(),
                                                true /* isDragDrop */);
        if (transferred) {
            mWindow->consumeMotionCancel();
            mDragWindow->consumeMotionDown();
        }
        return transferred;
    }
};

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

class InputDispatcherDropInputFeatureTest : public InputDispatcherTest {};

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

class InputDispatcherTouchModeChangedTests : public InputDispatcherTest {
protected:
    std::shared_ptr<FakeApplicationHandle> mApp;
    std::shared_ptr<FakeApplicationHandle> mSecondaryApp;
    sp<FakeWindowHandle> mWindow;
    sp<FakeWindowHandle> mSecondWindow;
    
    sp<FakeWindowHandle> mThirdWindow;
    
    void SetUp() override {
        InputDispatcherTest::SetUp();
    
        mApp = std::make_shared<FakeApplicationHandle>();
        mSecondaryApp = std::make_shared<FakeApplicationHandle>();
        mWindow = sp<FakeWindowHandle>::make(mApp, mDispatcher, "TestWindow", ADISPLAY_ID_DEFAULT);
        mWindow->setFocusable(true);
        setFocusedWindow(mWindow);
        mSecondWindow =
                sp<FakeWindowHandle>::make(mApp, mDispatcher, "TestWindow2", ADISPLAY_ID_DEFAULT);
        mSecondWindow->setFocusable(true);
        mThirdWindow =
                sp<FakeWindowHandle>::make(mSecondaryApp, mDispatcher,
                                           "TestWindow3_SecondaryDisplay", SECOND_DISPLAY_ID);
        mThirdWindow->setFocusable(true);
    
        mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, mApp);
        mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mWindow, mSecondWindow}},
                                      {SECOND_DISPLAY_ID, {mThirdWindow}}});
        mThirdWindow->setOwnerInfo(SECONDARY_WINDOW_PID, SECONDARY_WINDOW_UID);
        mWindow->consumeFocusEvent(true);
    
        // Set main display initial touch mode to InputDispatcher::kDefaultInTouchMode.
        if (mDispatcher->setInTouchMode(InputDispatcher::kDefaultInTouchMode, WINDOW_PID,
                                        WINDOW_UID, true /* hasPermission */,
                                        ADISPLAY_ID_DEFAULT)) {
            mWindow->consumeTouchModeEvent(InputDispatcher::kDefaultInTouchMode);
            mSecondWindow->consumeTouchModeEvent(InputDispatcher::kDefaultInTouchMode);
            mThirdWindow->assertNoEvents();
        }
    
        // Set secondary display initial touch mode to InputDispatcher::kDefaultInTouchMode.
        if (mDispatcher->setInTouchMode(InputDispatcher::kDefaultInTouchMode, SECONDARY_WINDOW_PID,
                                        SECONDARY_WINDOW_UID, true /* hasPermission */,
                                        SECOND_DISPLAY_ID)) {
            mWindow->assertNoEvents();
            mSecondWindow->assertNoEvents();
            mThirdWindow->consumeTouchModeEvent(InputDispatcher::kDefaultInTouchMode);
        }
    }
    
    void changeAndVerifyTouchModeInMainDisplayOnly(bool inTouchMode, int32_t pid, int32_t uid, bool hasPermission) {
        ASSERT_TRUE(mDispatcher->setInTouchMode(inTouchMode, pid, uid, hasPermission,
                                                ADISPLAY_ID_DEFAULT));
        mWindow->consumeTouchModeEvent(inTouchMode);
        mSecondWindow->consumeTouchModeEvent(inTouchMode);
        mThirdWindow->assertNoEvents();
    }
};

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

class InputDispatcherSpyWindowTest : public InputDispatcherTest {
public:
    sp<FakeWindowHandle> createSpy() {
        std::shared_ptr<FakeApplicationHandle> application =
                std::make_shared<FakeApplicationHandle>();
        std::string name = "Fake Spy ";
        name += std::to_string(mSpyCount++);
        sp<FakeWindowHandle> spy = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                              name.c_str(), ADISPLAY_ID_DEFAULT);
        spy->setSpy(true);
        spy->setTrustedOverlay(true);
        return spy;
    }
    
    sp<FakeWindowHandle> createForeground() {
        std::shared_ptr<FakeApplicationHandle> application =
                std::make_shared<FakeApplicationHandle>();
        sp<FakeWindowHandle> window =
                sp<FakeWindowHandle>::make(application, mDispatcher, "Fake Window",
                                           ADISPLAY_ID_DEFAULT);
        window->setFocusable(true);
        return window;
    }
    
private:
    int mSpyCount{0};
};

using InputDispatcherSpyWindowDeathTest = InputDispatcherSpyWindowTest;
TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

class InputDispatcherStylusInterceptorTest : public InputDispatcherTest {
public:
    std::pair<sp<FakeWindowHandle>, sp<FakeWindowHandle>> setupStylusOverlayScenario() {
        std::shared_ptr<FakeApplicationHandle> overlayApplication =
                std::make_shared<FakeApplicationHandle>();
        sp<FakeWindowHandle> overlay =
                sp<FakeWindowHandle>::make(overlayApplication, mDispatcher,
                                           "Stylus interceptor window", ADISPLAY_ID_DEFAULT);
        overlay->setFocusable(false);
        overlay->setOwnerInfo(111, 111);
        overlay->setTouchable(false);
        overlay->setInterceptsStylus(true);
        overlay->setTrustedOverlay(true);
    
        std::shared_ptr<FakeApplicationHandle> application =
                std::make_shared<FakeApplicationHandle>();
        sp<FakeWindowHandle> window =
                sp<FakeWindowHandle>::make(application, mDispatcher, "Application window",
                                           ADISPLAY_ID_DEFAULT);
        window->setFocusable(true);
        window->setOwnerInfo(222, 222);
    
        mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
        mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {overlay, window}}});
        setFocusedWindow(window);
        window->consumeFocusEvent(true /*hasFocus*/, true /*inTouchMode*/);
        return {std::move(overlay), std::move(window)};
    }
    
    void sendFingerEvent(int32_t action) {
        NotifyMotionArgs motionArgs =
                generateMotionArgs(action, AINPUT_SOURCE_TOUCHSCREEN | AINPUT_SOURCE_STYLUS,
                                   ADISPLAY_ID_DEFAULT, {PointF{20, 20}});
        mDispatcher->notifyMotion(&motionArgs);
    }
    
    void sendStylusEvent(int32_t action) {
        NotifyMotionArgs motionArgs =
                generateMotionArgs(action, AINPUT_SOURCE_TOUCHSCREEN | AINPUT_SOURCE_STYLUS,
                                   ADISPLAY_ID_DEFAULT, {PointF{30, 40}});
        motionArgs.pointerProperties[0].toolType = AMOTION_EVENT_TOOL_TYPE_STYLUS;
        mDispatcher->notifyMotion(&motionArgs);
    }
};

using InputDispatcherStylusInterceptorDeathTest = InputDispatcherStylusInterceptorTest;

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

struct User {
    int32_t mPid;
    int32_t mUid;
    uint32_t mPolicyFlags{DEFAULT_POLICY_FLAGS};
    std::unique_ptr<InputDispatcher>& mDispatcher;
    
    User(std::unique_ptr<InputDispatcher>& dispatcher, int32_t pid, int32_t uid)
          : mPid(pid), mUid(uid), mDispatcher(dispatcher) {}
    
    InputEventInjectionResult injectTargetedMotion(int32_t action) const {
        return injectMotionEvent(mDispatcher, action, AINPUT_SOURCE_TOUCHSCREEN,
                                 ADISPLAY_ID_DEFAULT, {100, 200},
                                 {AMOTION_EVENT_INVALID_CURSOR_POSITION,
                                  AMOTION_EVENT_INVALID_CURSOR_POSITION},
                                 INJECT_EVENT_TIMEOUT, InputEventInjectionSync::WAIT_FOR_RESULT,
                                 systemTime(SYSTEM_TIME_MONOTONIC), {mUid}, mPolicyFlags);
    }
    
    InputEventInjectionResult injectTargetedKey(int32_t action) const {
        return inputdispatcher::injectKey(mDispatcher, action, 0 /* repeatCount*/, ADISPLAY_ID_NONE,
                                          InputEventInjectionSync::WAIT_FOR_RESULT,
                                          INJECT_EVENT_TIMEOUT, false /*allowKeyRepeat*/, {mUid},
                                          mPolicyFlags);
    }
    
    sp<FakeWindowHandle> createWindow() const {
        std::shared_ptr<FakeApplicationHandle> overlayApplication =
                std::make_shared<FakeApplicationHandle>();
        sp<FakeWindowHandle> window =
                sp<FakeWindowHandle>::make(overlayApplication, mDispatcher, "Owned Window",
                                           ADISPLAY_ID_DEFAULT);
        window->setOwnerInfo(mPid, mUid);
        return window;
    }
};

using InputDispatcherTargetedInjectionTest = InputDispatcherTest;

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();

    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});

    // We allow generation of ACTION_OUTSIDE events into windows owned by different uids.
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}

} // namespace android::inputdispatcher

class ShouldSplitTouchFixture : public InputDispatcherTest,
                                public ::testing::WithParamInterface<bool> {};

using InputDispatcherPilferPointersTest = InputDispatcherSpyWindowTest;
