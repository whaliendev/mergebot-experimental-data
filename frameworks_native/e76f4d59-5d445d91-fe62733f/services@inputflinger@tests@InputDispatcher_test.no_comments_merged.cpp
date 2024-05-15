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
static constexpr nsecs_t ARBITRARY_TIME = 1234;
static constexpr int32_t DEVICE_ID = 1;
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
static constexpr int32_t WINDOW_PID = 999;
static constexpr int32_t WINDOW_UID = 1001;
static constexpr int32_t SECONDARY_WINDOW_PID = 1010;
static constexpr int32_t SECONDARY_WINDOW_UID = 1012;
static constexpr uint32_t DEFAULT_POLICY_FLAGS = POLICY_FLAG_FILTERED | POLICY_FLAG_PASS_TO_USER;
static constexpr int32_t MONITOR_PID = 2001;
static constexpr std::chrono::duration STALE_EVENT_TIMEOUT = 1000ms;
static constexpr int expectedWallpaperFlags =
        AMOTION_EVENT_FLAG_WINDOW_IS_OBSCURED | AMOTION_EVENT_FLAG_WINDOW_IS_PARTIALLY_OBSCURED;
struct PointF {
    float x;
    float y;
    auto operator<=>(const PointF&) const = default;
};
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
MATCHER_P(WithMotionAction, action, "MotionEvent with specified action") {
    bool matches = action == arg.getAction();
    if (!matches) {
        *result_listener << "expected action " << MotionEvent::actionToString(action)
                         << ", but got " << MotionEvent::actionToString(arg.getAction());
    }
    if (action == AMOTION_EVENT_ACTION_DOWN) {
        if (!matches) {
            *result_listener << "; ";
        }
        *result_listener << "downTime should match eventTime for ACTION_DOWN events";
        matches &= arg.getDownTime() == arg.getEventTime();
    }
    if (action == AMOTION_EVENT_ACTION_CANCEL) {
        if (!matches) {
            *result_listener << "; ";
        }
        *result_listener << "expected FLAG_CANCELED to be set with ACTION_CANCEL, but was not set";
        matches &= (arg.getFlags() & AMOTION_EVENT_FLAG_CANCELED) != 0;
    }
    return matches;
}
MATCHER_P(WithDownTime, downTime, "InputEvent with specified downTime") {
    return arg.getDownTime() == downTime;
}
MATCHER_P(WithSource, source, "InputEvent with specified source") {
    *result_listener << "expected source " << inputEventSourceToString(source) << ", but got "
                     << inputEventSourceToString(arg.getSource());
    return arg.getSource() == source;
}
MATCHER_P2(WithCoords, x, y, "MotionEvent with specified coordinates") {
    if (arg.getPointerCount() != 1) {
        *result_listener << "Expected 1 pointer, got " << arg.getPointerCount();
        return false;
    }
    return arg.getX(0 ) == x && arg.getY(0 ) == y;
}
MATCHER_P(WithPointers, pointers, "MotionEvent with specified pointers") {
    std::map<int32_t , PointF> actualPointers;
    for (size_t pointerIndex = 0; pointerIndex < arg.getPointerCount(); pointerIndex++) {
        const int32_t pointerId = arg.getPointerId(pointerIndex);
        actualPointers[pointerId] = {arg.getX(pointerIndex), arg.getY(pointerIndex)};
    }
    return pointers == actualPointers;
}
class FakeInputDispatcherPolicy : public InputDispatcherPolicyInterface {
    InputDispatcherConfiguration mConfig;
    using AnrResult = std::pair<sp<IBinder>, int32_t >;
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
    void setInterceptKeyTimeout(std::chrono::milliseconds timeout) {
        mInterceptKeyTimeout = timeout;
    }
private:
    std::mutex mLock;
    std::unique_ptr<InputEvent> mFilteredEvent GUARDED_BY(mLock);
    std::optional<nsecs_t> mConfigurationChangedTime GUARDED_BY(mLock);
    sp<IBinder> mOnPointerDownToken GUARDED_BY(mLock);
    std::optional<NotifySwitchArgs> mLastNotifySwitch GUARDED_BY(mLock);
    std::condition_variable mPointerCaptureChangedCondition;
    std::optional<PointerCaptureRequest> mPointerCaptureRequest GUARDED_BY(mLock);
    std::queue<std::shared_ptr<InputApplicationHandle>> mAnrApplications GUARDED_BY(mLock);
    std::queue<AnrResult> mAnrWindows GUARDED_BY(mLock);
    std::queue<AnrResult> mResponsiveWindows GUARDED_BY(mLock);
    std::condition_variable mNotifyAnr;
    std::queue<sp<IBinder>> mBrokenInputChannels GUARDED_BY(mLock);
    std::condition_variable mNotifyInputChannelBroken;
    sp<IBinder> mDropTargetWindowToken GUARDED_BY(mLock);
    bool mNotifyDropWindowWasCalled GUARDED_BY(mLock) = false;
    std::chrono::milliseconds mInterceptKeyTimeout = 0ms;
    template <class T>
    T getAnrTokenLockedInterruptible(std::chrono::nanoseconds timeout, std::queue<T>& storage,
                                     std::unique_lock<std::mutex>& lock) REQUIRES(mLock) {
        std::chrono::duration timeToWait = timeout + 100ms;
        const std::chrono::time_point start = std::chrono::steady_clock::now();
        std::optional<T> token =
                getItemFromStorageLockedInterruptible(timeToWait, storage, lock, mNotifyAnr);
        if (!token.has_value()) {
            ADD_FAILURE() << "Did not receive the ANR callback";
            return {};
        }
        const std::chrono::duration waited = std::chrono::steady_clock::now() - start;
        if (std::chrono::abs(timeout - waited) > 100ms) {
            ADD_FAILURE() << "ANR was raised too early or too late. Expected "
                          << std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count()
                          << "ms, but waited "
                          << std::chrono::duration_cast<std::chrono::milliseconds>(waited).count()
                          << "ms instead";
        }
        return *token;
    }
    template <class T>
    std::optional<T> getItemFromStorageLockedInterruptible(std::chrono::nanoseconds timeout,
                                                           std::queue<T>& storage,
                                                           std::unique_lock<std::mutex>& lock,
                                                           std::condition_variable& condition)
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
            mInterceptKeyTimeout = 0ms;
        }
    }
    void interceptMotionBeforeQueueing(int32_t, nsecs_t, uint32_t&) override {}
    nsecs_t interceptKeyBeforeDispatching(const sp<IBinder>&, const KeyEvent*, uint32_t) override {
        nsecs_t delay = std::chrono::nanoseconds(mInterceptKeyTimeout).count();
        mInterceptKeyTimeout = 0ms;
        return delay;
    }
    bool dispatchUnhandledKey(const sp<IBinder>&, const KeyEvent*, uint32_t, KeyEvent*) override {
        return false;
    }
    void notifySwitch(nsecs_t when, uint32_t switchValues, uint32_t switchMask,
                      uint32_t policyFlags) override {
        std::scoped_lock lock(mLock);
        mLastNotifySwitch = NotifySwitchArgs(1 , when, policyFlags, switchValues, switchMask);
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
class InputDispatcherTest : public testing::Test {
protected:
    sp<FakeInputDispatcherPolicy> mFakePolicy;
    std::unique_ptr<InputDispatcher> mDispatcher;
    void SetUp() override {
        mFakePolicy = sp<FakeInputDispatcherPolicy>::make();
        mDispatcher = std::make_unique<InputDispatcher>(mFakePolicy, STALE_EVENT_TIMEOUT);
        mDispatcher->setInputDispatchMode( true, false);
        ASSERT_EQ(OK, mDispatcher->start());
    }
    void TearDown() override {
        ASSERT_EQ(OK, mDispatcher->stop());
        mFakePolicy.clear();
        mDispatcher.reset();
    }
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
TEST_F(InputDispatcherTest, InjectInputEvent_ValidatesKeyEvents) {
    KeyEvent event;
    event.initialize(InputEvent::nextId(), DEVICE_ID, AINPUT_SOURCE_KEYBOARD, ADISPLAY_ID_NONE,
                     INVALID_HMAC,
                                -1, 0, AKEYCODE_A, KEY_A, AMETA_NONE, 0, ARBITRARY_TIME,
                     ARBITRARY_TIME);
    ASSERT_EQ(InputEventInjectionResult::FAILED,
              mDispatcher->injectInputEvent(&event, {} , InputEventInjectionSync::NONE,
                                            0ms, 0))
            << "Should reject key events with undefined action.";
    event.initialize(InputEvent::nextId(), DEVICE_ID, AINPUT_SOURCE_KEYBOARD, ADISPLAY_ID_NONE,
                     INVALID_HMAC, AKEY_EVENT_ACTION_MULTIPLE, 0, AKEYCODE_A, KEY_A, AMETA_NONE, 0,
                     ARBITRARY_TIME, ARBITRARY_TIME);
    ASSERT_EQ(InputEventInjectionResult::FAILED,
              mDispatcher->injectInputEvent(&event, {} , InputEventInjectionSync::NONE,
                                            0ms, 0))
            << "Should reject key events with ACTION_MULTIPLE.";
}
TEST_F(InputDispatcherTest, InjectInputEvent_ValidatesMotionEvents) {
    MotionEvent event;
    PointerProperties pointerProperties[MAX_POINTERS + 1];
    PointerCoords pointerCoords[MAX_POINTERS + 1];
    for (size_t i = 0; i <= MAX_POINTERS; i++) {
        pointerProperties[i].clear();
        pointerProperties[i].id = i;
        pointerCoords[i].clear();
    }
    constexpr int32_t source = AINPUT_SOURCE_TOUCHSCREEN;
    constexpr int32_t edgeFlags = AMOTION_EVENT_EDGE_FLAG_NONE;
    constexpr int32_t metaState = AMETA_NONE;
    constexpr MotionClassification classification = MotionClassification::NONE;
    ui::Transform identityTransform;
    event.initialize(InputEvent::nextId(), DEVICE_ID, source, DISPLAY_ID, INVALID_HMAC,
                                -1, 0, 0, edgeFlags, metaState, 0, classification,
                     identityTransform, 0, 0, AMOTION_EVENT_INVALID_CURSOR_POSITION,
                     AMOTION_EVENT_INVALID_CURSOR_POSITION, identityTransform, ARBITRARY_TIME,
                     ARBITRARY_TIME,
                                      1, pointerProperties, pointerCoords);
    ASSERT_EQ(InputEventInjectionResult::FAILED,
              mDispatcher->injectInputEvent(&event, {} , InputEventInjectionSync::NONE,
                                            0ms, 0))
            << "Should reject motion events with undefined action.";
    event.initialize(InputEvent::nextId(), DEVICE_ID, source, DISPLAY_ID, INVALID_HMAC,
                     POINTER_1_DOWN, 0, 0, edgeFlags, metaState, 0, classification,
                     identityTransform, 0, 0, AMOTION_EVENT_INVALID_CURSOR_POSITION,
                     AMOTION_EVENT_INVALID_CURSOR_POSITION, identityTransform, ARBITRARY_TIME,
                     ARBITRARY_TIME,
                                      1, pointerProperties, pointerCoords);
    ASSERT_EQ(InputEventInjectionResult::FAILED,
              mDispatcher->injectInputEvent(&event, {} , InputEventInjectionSync::NONE,
                                            0ms, 0))
            << "Should reject motion events with pointer down index too large.";
    event.initialize(InputEvent::nextId(), DEVICE_ID, source, DISPLAY_ID, INVALID_HMAC,
                     AMOTION_EVENT_ACTION_POINTER_DOWN |
                             (~0U << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                     0, 0, edgeFlags, metaState, 0, classification, identityTransform, 0, 0,
                     AMOTION_EVENT_INVALID_CURSOR_POSITION, AMOTION_EVENT_INVALID_CURSOR_POSITION,
                     identityTransform, ARBITRARY_TIME, ARBITRARY_TIME,
                                      1, pointerProperties, pointerCoords);
    ASSERT_EQ(InputEventInjectionResult::FAILED,
              mDispatcher->injectInputEvent(&event, {} , InputEventInjectionSync::NONE,
                                            0ms, 0))
            << "Should reject motion events with pointer down index too small.";
    event.initialize(InputEvent::nextId(), DEVICE_ID, source, DISPLAY_ID, INVALID_HMAC,
                     POINTER_1_UP, 0, 0, edgeFlags, metaState, 0, classification, identityTransform,
                     0, 0, AMOTION_EVENT_INVALID_CURSOR_POSITION,
                     AMOTION_EVENT_INVALID_CURSOR_POSITION, identityTransform, ARBITRARY_TIME,
                     ARBITRARY_TIME,
                                      1, pointerProperties, pointerCoords);
    ASSERT_EQ(InputEventInjectionResult::FAILED,
              mDispatcher->injectInputEvent(&event, {} , InputEventInjectionSync::NONE,
                                            0ms, 0))
            << "Should reject motion events with pointer up index too large.";
    event.initialize(InputEvent::nextId(), DEVICE_ID, source, DISPLAY_ID, INVALID_HMAC,
                     AMOTION_EVENT_ACTION_POINTER_UP |
                             (~0U << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                     0, 0, edgeFlags, metaState, 0, classification, identityTransform, 0, 0,
                     AMOTION_EVENT_INVALID_CURSOR_POSITION, AMOTION_EVENT_INVALID_CURSOR_POSITION,
                     identityTransform, ARBITRARY_TIME, ARBITRARY_TIME,
                                      1, pointerProperties, pointerCoords);
    ASSERT_EQ(InputEventInjectionResult::FAILED,
              mDispatcher->injectInputEvent(&event, {} , InputEventInjectionSync::NONE,
                                            0ms, 0))
            << "Should reject motion events with pointer up index too small.";
    event.initialize(InputEvent::nextId(), DEVICE_ID, source, DISPLAY_ID, INVALID_HMAC,
                     AMOTION_EVENT_ACTION_DOWN, 0, 0, edgeFlags, metaState, 0, classification,
                     identityTransform, 0, 0, AMOTION_EVENT_INVALID_CURSOR_POSITION,
                     AMOTION_EVENT_INVALID_CURSOR_POSITION, identityTransform, ARBITRARY_TIME,
                     ARBITRARY_TIME,
                                      0, pointerProperties, pointerCoords);
    ASSERT_EQ(InputEventInjectionResult::FAILED,
              mDispatcher->injectInputEvent(&event, {} , InputEventInjectionSync::NONE,
                                            0ms, 0))
            << "Should reject motion events with 0 pointers.";
    event.initialize(InputEvent::nextId(), DEVICE_ID, source, DISPLAY_ID, INVALID_HMAC,
                     AMOTION_EVENT_ACTION_DOWN, 0, 0, edgeFlags, metaState, 0, classification,
                     identityTransform, 0, 0, AMOTION_EVENT_INVALID_CURSOR_POSITION,
                     AMOTION_EVENT_INVALID_CURSOR_POSITION, identityTransform, ARBITRARY_TIME,
                     ARBITRARY_TIME,
                                      MAX_POINTERS + 1, pointerProperties, pointerCoords);
    ASSERT_EQ(InputEventInjectionResult::FAILED,
              mDispatcher->injectInputEvent(&event, {} , InputEventInjectionSync::NONE,
                                            0ms, 0))
            << "Should reject motion events with more than MAX_POINTERS pointers.";
    pointerProperties[0].id = -1;
    event.initialize(InputEvent::nextId(), DEVICE_ID, source, DISPLAY_ID, INVALID_HMAC,
                     AMOTION_EVENT_ACTION_DOWN, 0, 0, edgeFlags, metaState, 0, classification,
                     identityTransform, 0, 0, AMOTION_EVENT_INVALID_CURSOR_POSITION,
                     AMOTION_EVENT_INVALID_CURSOR_POSITION, identityTransform, ARBITRARY_TIME,
                     ARBITRARY_TIME,
                                      1, pointerProperties, pointerCoords);
    ASSERT_EQ(InputEventInjectionResult::FAILED,
              mDispatcher->injectInputEvent(&event, {} , InputEventInjectionSync::NONE,
                                            0ms, 0))
            << "Should reject motion events with pointer ids less than 0.";
    pointerProperties[0].id = MAX_POINTER_ID + 1;
    event.initialize(InputEvent::nextId(), DEVICE_ID, source, DISPLAY_ID, INVALID_HMAC,
                     AMOTION_EVENT_ACTION_DOWN, 0, 0, edgeFlags, metaState, 0, classification,
                     identityTransform, 0, 0, AMOTION_EVENT_INVALID_CURSOR_POSITION,
                     AMOTION_EVENT_INVALID_CURSOR_POSITION, identityTransform, ARBITRARY_TIME,
                     ARBITRARY_TIME,
                                      1, pointerProperties, pointerCoords);
    ASSERT_EQ(InputEventInjectionResult::FAILED,
              mDispatcher->injectInputEvent(&event, {} , InputEventInjectionSync::NONE,
                                            0ms, 0))
            << "Should reject motion events with pointer ids greater than MAX_POINTER_ID.";
    pointerProperties[0].id = 1;
    pointerProperties[1].id = 1;
    event.initialize(InputEvent::nextId(), DEVICE_ID, source, DISPLAY_ID, INVALID_HMAC,
                     AMOTION_EVENT_ACTION_DOWN, 0, 0, edgeFlags, metaState, 0, classification,
                     identityTransform, 0, 0, AMOTION_EVENT_INVALID_CURSOR_POSITION,
                     AMOTION_EVENT_INVALID_CURSOR_POSITION, identityTransform, ARBITRARY_TIME,
                     ARBITRARY_TIME,
                                      2, pointerProperties, pointerCoords);
    ASSERT_EQ(InputEventInjectionResult::FAILED,
              mDispatcher->injectInputEvent(&event, {} , InputEventInjectionSync::NONE,
                                            0ms, 0))
            << "Should reject motion events with duplicate pointer ids.";
}
TEST_F(InputDispatcherTest, NotifyConfigurationChanged_CallsPolicy) {
    constexpr nsecs_t eventTime = 20;
    NotifyConfigurationChangedArgs args(10 , eventTime);
    mDispatcher->notifyConfigurationChanged(&args);
    ASSERT_TRUE(mDispatcher->waitForIdle());
    mFakePolicy->assertNotifyConfigurationChangedWasCalled(eventTime);
}
TEST_F(InputDispatcherTest, NotifySwitch_CallsPolicy) {
    NotifySwitchArgs args(10 , 20 , 0 , 1 ,
                          2 );
    mDispatcher->notifySwitch(&args);
    args.policyFlags |= POLICY_FLAG_TRUSTED;
    mFakePolicy->assertNotifySwitchWasCalled(args);
}
static constexpr std::chrono::duration INJECT_EVENT_TIMEOUT = 500ms;
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
    std::optional<uint32_t> receiveEvent(InputEvent** outEvent = nullptr) {
        uint32_t consumeSeq;
        InputEvent* event;
        std::chrono::time_point start = std::chrono::steady_clock::now();
        status_t status = WOULD_BLOCK;
        while (status == WOULD_BLOCK) {
            status = mConsumer->consume(&mEventFactory, true , -1, &consumeSeq,
                                        &event);
            std::chrono::duration elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > 100ms) {
                break;
            }
        }
        if (status == WOULD_BLOCK) {
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
            return;
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
    int getChannelFd() { return mInputReceiver->getChannelFd(); }
private:
    const std::string mName;
    std::unique_ptr<FakeInputReceiver> mInputReceiver;
    static std::atomic<int32_t> sId;
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
    event.initialize(InputEvent::nextId(), DEVICE_ID, AINPUT_SOURCE_KEYBOARD, displayId,
                     INVALID_HMAC, action, 0, AKEYCODE_A, KEY_A, AMETA_NONE,
                     repeatCount, currentTime, currentTime);
    if (!allowKeyRepeat) {
        policyFlags |= POLICY_FLAG_DISABLE_KEY_REPEAT;
    }
    return dispatcher->injectInputEvent(&event, targetUid, syncMode, injectionTimeout, policyFlags);
}
static InputEventInjectionResult injectKeyDown(const std::unique_ptr<InputDispatcher>& dispatcher,
                                               int32_t displayId = ADISPLAY_ID_NONE) {
    return injectKey(dispatcher, AKEY_EVENT_ACTION_DOWN, 0, displayId);
}
static InputEventInjectionResult injectKeyDownNoRepeat(
        const std::unique_ptr<InputDispatcher>& dispatcher, int32_t displayId = ADISPLAY_ID_NONE) {
    return injectKey(dispatcher, AKEY_EVENT_ACTION_DOWN, 0, displayId,
                     InputEventInjectionSync::WAIT_FOR_RESULT, INJECT_EVENT_TIMEOUT,
                                          false);
}
static InputEventInjectionResult injectKeyUp(const std::unique_ptr<InputDispatcher>& dispatcher,
                                             int32_t displayId = ADISPLAY_ID_NONE) {
    return injectKey(dispatcher, AKEY_EVENT_ACTION_UP, 0, displayId);
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
        if (mSource == AINPUT_SOURCE_MOUSE &&
            !MotionEvent::isValidCursorPosition(mRawXCursorPosition, mRawYCursorPosition) &&
            mPointers.size() == 1) {
            mRawXCursorPosition = pointerCoords[0].getX();
            mRawYCursorPosition = pointerCoords[0].getY();
        }
        MotionEvent event;
        ui::Transform identityTransform;
        event.initialize(InputEvent::nextId(), DEVICE_ID, mSource, mDisplayId, INVALID_HMAC,
                         mAction, mActionButton, mFlags, 0, AMETA_NONE,
                         mButtonState, MotionClassification::NONE, identityTransform,
                                          0, 0, mRawXCursorPosition,
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
        std::optional<int32_t> targetUid = {}, uint32_t policyFlags = DEFAULT_POLICY_FLAGS) {
    MotionEvent event = MotionEventBuilder(action, source)
                                .displayId(displayId)
                                .eventTime(eventTime)
                                .rawXCursorPosition(cursorPosition.x)
                                .rawYCursorPosition(cursorPosition.y)
                                .pointer(PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER)
                                                 .x(position.x)
                                                 .y(position.y))
                                .build();
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
    NotifyKeyArgs args( 0, currentTime, 0 , DEVICE_ID, AINPUT_SOURCE_KEYBOARD,
                       displayId, POLICY_FLAG_PASS_TO_USER, action, 0, AKEYCODE_A,
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
    NotifyMotionArgs args( 0, currentTime, 0 , DEVICE_ID, source, displayId,
                          POLICY_FLAG_PASS_TO_USER, action, 0, 0,
                          AMETA_NONE, 0, MotionClassification::NONE,
                          AMOTION_EVENT_EDGE_FLAG_NONE, pointerCount, pointerProperties,
                          pointerCoords, 0, 0,
                          AMOTION_EVENT_INVALID_CURSOR_POSITION,
                          AMOTION_EVENT_INVALID_CURSOR_POSITION, currentTime, {});
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
    return NotifyPointerCaptureChangedArgs( 0, systemTime(SYSTEM_TIME_MONOTONIC), request);
}
TEST_F(InputDispatcherTest, WhenInputChannelBreaks_PolicyIsNotified) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window =
            sp<FakeWindowHandle>::make(application, mDispatcher,
                                       "Window that breaks its input channel", ADISPLAY_ID_DEFAULT);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    window->destroyReceiver();
    mFakePolicy->assertNotifyInputChannelBrokenWasCalled(window->getInfo()->token);
}
TEST_F(InputDispatcherTest, SetInputWindow_SingleWindowTouch) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Fake Window", ADISPLAY_ID_DEFAULT);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionDown(ADISPLAY_ID_DEFAULT);
}
TEST_F(InputDispatcherTest, WhenDisplayNotSpecified_InjectMotionToDefaultDisplay) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Fake Window", ADISPLAY_ID_DEFAULT);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_NONE))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionDown(ADISPLAY_ID_DEFAULT);
}
TEST_F(InputDispatcherTest, SetInputWindowOnceWithSingleTouchWindow) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Fake Window", ADISPLAY_ID_DEFAULT);
    window->setFrame(Rect(0, 0, 100, 100));
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {50, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionDown(ADISPLAY_ID_DEFAULT);
}
TEST_F(InputDispatcherTest, SetInputWindowTwice_SingleWindowTouch) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Fake Window", ADISPLAY_ID_DEFAULT);
    window->setFrame(Rect(0, 0, 100, 100));
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {50, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionDown(ADISPLAY_ID_DEFAULT);
}
TEST_F(InputDispatcherTest, SetInputWindow_MultiWindowsTouch) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> windowTop =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Top", ADISPLAY_ID_DEFAULT);
    sp<FakeWindowHandle> windowSecond =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Second", ADISPLAY_ID_DEFAULT);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {windowTop, windowSecond}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    windowTop->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    windowSecond->assertNoEvents();
}
TEST_F(InputDispatcherTest, WhenForegroundWindowDisappears_WallpaperTouchIsCanceled) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> foregroundWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Foreground", ADISPLAY_ID_DEFAULT);
    foregroundWindow->setDupTouchToWallpaper(true);
    sp<FakeWindowHandle> wallpaperWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Wallpaper", ADISPLAY_ID_DEFAULT);
    wallpaperWindow->setIsWallpaper(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {foregroundWindow, wallpaperWindow}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {100, 200}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    foregroundWindow->consumeMotionDown();
    wallpaperWindow->consumeMotionDown(ADISPLAY_ID_DEFAULT, expectedWallpaperFlags);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN,
                                ADISPLAY_ID_DEFAULT, {110, 200}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    foregroundWindow->consumeMotionMove();
    wallpaperWindow->consumeMotionMove(ADISPLAY_ID_DEFAULT, expectedWallpaperFlags);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {wallpaperWindow}}});
    foregroundWindow->consumeMotionCancel();
    wallpaperWindow->consumeMotionCancel(ADISPLAY_ID_DEFAULT, expectedWallpaperFlags);
}
TEST_F(InputDispatcherTest, WhenWallpaperDisappears_NoCrash) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> foregroundWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Foreground", ADISPLAY_ID_DEFAULT);
    foregroundWindow->setDupTouchToWallpaper(true);
    sp<FakeWindowHandle> wallpaperWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Wallpaper", ADISPLAY_ID_DEFAULT);
    wallpaperWindow->setIsWallpaper(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {foregroundWindow, wallpaperWindow}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {100, 200}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    foregroundWindow->consumeMotionDown();
    wallpaperWindow->consumeMotionDown(ADISPLAY_ID_DEFAULT, expectedWallpaperFlags);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN,
                                ADISPLAY_ID_DEFAULT, {110, 200}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    foregroundWindow->consumeMotionMove();
    wallpaperWindow->consumeMotionMove(ADISPLAY_ID_DEFAULT, expectedWallpaperFlags);
    wallpaperWindow->destroyReceiver();
    mFakePolicy->assertNotifyInputChannelBrokenWasCalled(wallpaperWindow->getInfo()->token);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {wallpaperWindow}}});
    foregroundWindow->consumeMotionCancel();
}
class ShouldSplitTouchFixture : public InputDispatcherTest,
                                public ::testing::WithParamInterface<bool> {};
INSTANTIATE_TEST_SUITE_P(InputDispatcherTest, ShouldSplitTouchFixture,
                         ::testing::Values(true, false));
TEST_P(ShouldSplitTouchFixture, WallpaperWindowReceivesMultiTouch) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> foregroundWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Foreground", ADISPLAY_ID_DEFAULT);
    foregroundWindow->setDupTouchToWallpaper(true);
    foregroundWindow->setPreventSplitting(GetParam());
    sp<FakeWindowHandle> wallpaperWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Wallpaper", ADISPLAY_ID_DEFAULT);
    wallpaperWindow->setIsWallpaper(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {foregroundWindow, wallpaperWindow}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {100, 100}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    foregroundWindow->consumeMotionDown();
    wallpaperWindow->consumeMotionDown(ADISPLAY_ID_DEFAULT, expectedWallpaperFlags);
    const MotionEvent secondFingerDownEvent =
            MotionEventBuilder(POINTER_1_DOWN, AINPUT_SOURCE_TOUCHSCREEN)
                    .eventTime(systemTime(SYSTEM_TIME_MONOTONIC))
                    .pointer(PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER)
                                     .x(100)
                                     .y(100))
                    .pointer(PointerBuilder( 1, AMOTION_EVENT_TOOL_TYPE_FINGER)
                                     .x(150)
                                     .y(150))
                    .build();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, secondFingerDownEvent, INJECT_EVENT_TIMEOUT,
                                InputEventInjectionSync::WAIT_FOR_RESULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    foregroundWindow->consumeMotionPointerDown(1 );
    wallpaperWindow->consumeMotionPointerDown(1 , ADISPLAY_ID_DEFAULT,
                                              expectedWallpaperFlags);
    const MotionEvent secondFingerUpEvent =
            MotionEventBuilder(POINTER_0_UP, AINPUT_SOURCE_TOUCHSCREEN)
                    .displayId(ADISPLAY_ID_DEFAULT)
                    .eventTime(systemTime(SYSTEM_TIME_MONOTONIC))
                    .pointer(PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER)
                                     .x(100)
                                     .y(100))
                    .pointer(PointerBuilder( 1, AMOTION_EVENT_TOOL_TYPE_FINGER)
                                     .x(150)
                                     .y(150))
                    .build();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, secondFingerUpEvent, INJECT_EVENT_TIMEOUT,
                                InputEventInjectionSync::WAIT_FOR_RESULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    foregroundWindow->consumeMotionPointerUp(0);
    wallpaperWindow->consumeMotionPointerUp(0, ADISPLAY_ID_DEFAULT, expectedWallpaperFlags);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionUp(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                             {100, 100}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    foregroundWindow->consumeMotionUp(ADISPLAY_ID_DEFAULT);
    wallpaperWindow->consumeMotionUp(ADISPLAY_ID_DEFAULT, expectedWallpaperFlags);
}
TEST_F(InputDispatcherTest, TwoWindows_SplitWallpaperTouch) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> leftWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Left", ADISPLAY_ID_DEFAULT);
    leftWindow->setFrame(Rect(0, 0, 200, 200));
    leftWindow->setDupTouchToWallpaper(true);
    sp<FakeWindowHandle> rightWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Right", ADISPLAY_ID_DEFAULT);
    rightWindow->setFrame(Rect(200, 0, 400, 200));
    rightWindow->setDupTouchToWallpaper(true);
    sp<FakeWindowHandle> wallpaperWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Wallpaper", ADISPLAY_ID_DEFAULT);
    wallpaperWindow->setFrame(Rect(0, 0, 400, 200));
    wallpaperWindow->setIsWallpaper(true);
    mDispatcher->setInputWindows(
            {{ADISPLAY_ID_DEFAULT, {leftWindow, rightWindow, wallpaperWindow}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {100, 100}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    leftWindow->consumeMotionDown();
    wallpaperWindow->consumeMotionDown(ADISPLAY_ID_DEFAULT, expectedWallpaperFlags);
    const MotionEvent secondFingerDownEvent =
            MotionEventBuilder(POINTER_1_DOWN, AINPUT_SOURCE_TOUCHSCREEN)
                    .eventTime(systemTime(SYSTEM_TIME_MONOTONIC))
                    .pointer(PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER)
                                     .x(100)
                                     .y(100))
                    .pointer(PointerBuilder( 1, AMOTION_EVENT_TOOL_TYPE_FINGER)
                                     .x(300)
                                     .y(100))
                    .build();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, secondFingerDownEvent, INJECT_EVENT_TIMEOUT,
                                InputEventInjectionSync::WAIT_FOR_RESULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    leftWindow->consumeMotionMove();
    rightWindow->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    wallpaperWindow->consumeMotionPointerDown(1 , ADISPLAY_ID_DEFAULT,
                                              expectedWallpaperFlags);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {rightWindow, wallpaperWindow}}});
    leftWindow->consumeMotionCancel();
    wallpaperWindow->consumeMotionCancel(ADISPLAY_ID_DEFAULT, expectedWallpaperFlags);
    const MotionEvent secondFingerMoveEvent =
            MotionEventBuilder(AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN)
                    .eventTime(systemTime(SYSTEM_TIME_MONOTONIC))
                    .pointer(PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER)
                                     .x(100)
                                     .y(100))
                    .pointer(PointerBuilder( 1, AMOTION_EVENT_TOOL_TYPE_FINGER)
                                     .x(310)
                                     .y(110))
                    .build();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, secondFingerMoveEvent, INJECT_EVENT_TIMEOUT,
                                InputEventInjectionSync::WAIT_FOR_RESULT));
    rightWindow->consumeMotionMove();
    leftWindow->assertNoEvents();
    rightWindow->assertNoEvents();
    wallpaperWindow->assertNoEvents();
}
TEST_F(InputDispatcherTest, WallpaperWindowWhenSlippery) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> leftWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Left", ADISPLAY_ID_DEFAULT);
    leftWindow->setFrame(Rect(0, 0, 200, 200));
    leftWindow->setDupTouchToWallpaper(true);
    leftWindow->setSlippery(true);
    sp<FakeWindowHandle> rightWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Right", ADISPLAY_ID_DEFAULT);
    rightWindow->setFrame(Rect(200, 0, 400, 200));
    sp<FakeWindowHandle> wallpaperWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Wallpaper", ADISPLAY_ID_DEFAULT);
    wallpaperWindow->setIsWallpaper(true);
    mDispatcher->setInputWindows(
            {{ADISPLAY_ID_DEFAULT, {leftWindow, rightWindow, wallpaperWindow}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {100, 100}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    leftWindow->consumeMotionDown();
    wallpaperWindow->consumeMotionDown(ADISPLAY_ID_DEFAULT, expectedWallpaperFlags);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN,
                                ADISPLAY_ID_DEFAULT, {201, 100}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    leftWindow->consumeMotionCancel();
    rightWindow->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    wallpaperWindow->consumeMotionCancel(ADISPLAY_ID_DEFAULT, expectedWallpaperFlags);
}
TEST_F(InputDispatcherTest, SplitWorksWhenEmptyAreaIsTouched) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Window", DISPLAY_ID);
    mDispatcher->setInputWindows({{DISPLAY_ID, {window}}});
    NotifyMotionArgs args;
    mDispatcher->notifyMotion(&(args = generateTouchArgs(AMOTION_EVENT_ACTION_DOWN, {{-1, -1}})));
    mDispatcher->waitForIdle();
    window->assertNoEvents();
    mDispatcher->notifyMotion(&(args = generateTouchArgs(POINTER_1_DOWN, {{-1, -1}, {10, 10}})));
    mDispatcher->waitForIdle();
    window->consumeMotionDown();
}
TEST_F(InputDispatcherTest, SplitWorksWhenNonTouchableWindowIsTouched) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window1 =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Window1", DISPLAY_ID);
    window1->setTouchableRegion(Region{{0, 0, 100, 100}});
    window1->setTouchable(false);
    sp<FakeWindowHandle> window2 =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Window2", DISPLAY_ID);
    window2->setTouchableRegion(Region{{100, 0, 200, 100}});
    mDispatcher->setInputWindows({{DISPLAY_ID, {window1, window2}}});
    NotifyMotionArgs args;
    mDispatcher->notifyMotion(&(args = generateTouchArgs(AMOTION_EVENT_ACTION_DOWN, {{50, 50}})));
    mDispatcher->waitForIdle();
    window1->assertNoEvents();
    window2->assertNoEvents();
    mDispatcher->notifyMotion(&(args = generateTouchArgs(POINTER_1_DOWN, {{50, 50}, {150, 50}})));
    mDispatcher->waitForIdle();
    window2->consumeMotionDown();
}
TEST_F(InputDispatcherTest, SplitTouchesSendCorrectActionDownTime) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window1 =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Window1", DISPLAY_ID);
    window1->setTouchableRegion(Region{{0, 0, 100, 100}});
    sp<FakeWindowHandle> window2 =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Window2", DISPLAY_ID);
    window2->setTouchableRegion(Region{{100, 0, 200, 100}});
    mDispatcher->setInputWindows({{DISPLAY_ID, {window1, window2}}});
    NotifyMotionArgs args;
    mDispatcher->notifyMotion(&(args = generateTouchArgs(AMOTION_EVENT_ACTION_DOWN, {{50, 50}})));
    mDispatcher->waitForIdle();
    InputEvent* inputEvent1 = window1->consume();
    ASSERT_NE(inputEvent1, nullptr);
    window2->assertNoEvents();
    MotionEvent& motionEvent1 = static_cast<MotionEvent&>(*inputEvent1);
    nsecs_t downTimeForWindow1 = motionEvent1.getDownTime();
    ASSERT_EQ(motionEvent1.getDownTime(), motionEvent1.getEventTime());
    mDispatcher->notifyMotion(&(args = generateTouchArgs(POINTER_1_DOWN, {{50, 50}, {150, 50}})));
    mDispatcher->waitForIdle();
    InputEvent* inputEvent2 = window2->consume();
    ASSERT_NE(inputEvent2, nullptr);
    MotionEvent& motionEvent2 = static_cast<MotionEvent&>(*inputEvent2);
    nsecs_t downTimeForWindow2 = motionEvent2.getDownTime();
    ASSERT_NE(downTimeForWindow1, downTimeForWindow2);
    ASSERT_EQ(motionEvent2.getDownTime(), motionEvent2.getEventTime());
    mDispatcher->notifyMotion(
            &(args = generateTouchArgs(AMOTION_EVENT_ACTION_MOVE, {{50, 50}, {151, 51}})));
    mDispatcher->waitForIdle();
    window2->consumeMotionEvent(WithDownTime(downTimeForWindow2));
    mDispatcher->notifyMotion(
            &(args = generateTouchArgs(POINTER_2_DOWN, {{50, 50}, {151, 51}, {150, 50}})));
    mDispatcher->waitForIdle();
    window2->consumeMotionEvent(WithDownTime(downTimeForWindow2));
    window1->consumeMotionMove();
    window1->assertNoEvents();
    mDispatcher->notifyMotion(
            &(args = generateTouchArgs(AMOTION_EVENT_ACTION_MOVE, {{51, 51}, {151, 51}})));
    mDispatcher->waitForIdle();
    window1->consumeMotionEvent(WithDownTime(downTimeForWindow1));
    mDispatcher->notifyMotion(&(
            args = generateTouchArgs(POINTER_3_DOWN, {{51, 51}, {151, 51}, {150, 50}, {50, 50}})));
    mDispatcher->waitForIdle();
    window1->consumeMotionEvent(WithDownTime(downTimeForWindow1));
}
TEST_F(InputDispatcherTest, HoverMoveEnterMouseClickAndHoverMoveExit) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> windowLeft =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Left", ADISPLAY_ID_DEFAULT);
    windowLeft->setFrame(Rect(0, 0, 600, 800));
    sp<FakeWindowHandle> windowRight =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Right", ADISPLAY_ID_DEFAULT);
    windowRight->setFrame(Rect(600, 0, 1200, 800));
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {windowLeft, windowRight}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_HOVER_MOVE,
                                                   AINPUT_SOURCE_MOUSE)
                                        .pointer(PointerBuilder(0, AMOTION_EVENT_TOOL_TYPE_MOUSE)
                                                         .x(900)
                                                         .y(400))
                                        .build()));
    windowRight->consumeMotionEvent(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_ENTER));
    windowRight->consumeMotionEvent(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE));
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_HOVER_MOVE,
                                                   AINPUT_SOURCE_MOUSE)
                                        .pointer(PointerBuilder(0, AMOTION_EVENT_TOOL_TYPE_MOUSE)
                                                         .x(300)
                                                         .y(400))
                                        .build()));
    windowRight->consumeMotionEvent(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_EXIT));
    windowLeft->consumeMotionEvent(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_ENTER));
    windowLeft->consumeMotionEvent(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE));
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_MOUSE)
                                        .buttonState(AMOTION_EVENT_BUTTON_PRIMARY)
                                        .pointer(PointerBuilder(0, AMOTION_EVENT_TOOL_TYPE_MOUSE)
                                                         .x(300)
                                                         .y(400))
                                        .build()));
    windowLeft->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_BUTTON_PRESS,
                                                   AINPUT_SOURCE_MOUSE)
                                        .buttonState(AMOTION_EVENT_BUTTON_PRIMARY)
                                        .actionButton(AMOTION_EVENT_BUTTON_PRIMARY)
                                        .pointer(PointerBuilder(0, AMOTION_EVENT_TOOL_TYPE_MOUSE)
                                                         .x(300)
                                                         .y(400))
                                        .build()));
    windowLeft->consumeMotionEvent(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_PRESS));
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_BUTTON_RELEASE,
                                                   AINPUT_SOURCE_MOUSE)
                                        .buttonState(0)
                                        .actionButton(AMOTION_EVENT_BUTTON_PRIMARY)
                                        .pointer(PointerBuilder(0, AMOTION_EVENT_TOOL_TYPE_MOUSE)
                                                         .x(300)
                                                         .y(400))
                                        .build()));
    windowLeft->consumeMotionEvent(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_RELEASE));
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_UP, AINPUT_SOURCE_MOUSE)
                                        .buttonState(0)
                                        .pointer(PointerBuilder(0, AMOTION_EVENT_TOOL_TYPE_MOUSE)
                                                         .x(300)
                                                         .y(400))
                                        .build()));
    windowLeft->consumeMotionUp(ADISPLAY_ID_DEFAULT);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_HOVER_MOVE,
                                                   AINPUT_SOURCE_MOUSE)
                                        .pointer(PointerBuilder(0, AMOTION_EVENT_TOOL_TYPE_MOUSE)
                                                         .x(900)
                                                         .y(400))
                                        .build()));
    windowLeft->consumeMotionEvent(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_EXIT));
    windowRight->consumeMotionEvent(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_ENTER));
    windowRight->consumeMotionEvent(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE));
    windowLeft->assertNoEvents();
    windowRight->assertNoEvents();
}
TEST_F(InputDispatcherTest, HoverWithSpyWindows) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> spyWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Spy", ADISPLAY_ID_DEFAULT);
    spyWindow->setFrame(Rect(0, 0, 600, 800));
    spyWindow->setTrustedOverlay(true);
    spyWindow->setSpy(true);
    sp<FakeWindowHandle> window =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Window", ADISPLAY_ID_DEFAULT);
    window->setFrame(Rect(0, 0, 600, 800));
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {spyWindow, window}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_HOVER_ENTER,
                                                   AINPUT_SOURCE_MOUSE)
                                        .pointer(PointerBuilder(0, AMOTION_EVENT_TOOL_TYPE_MOUSE)
                                                         .x(100)
                                                         .y(100))
                                        .build()));
    window->consumeMotionEvent(AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_ENTER),
                                     WithSource(AINPUT_SOURCE_MOUSE)));
    spyWindow->consumeMotionEvent(AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_ENTER),
                                        WithSource(AINPUT_SOURCE_MOUSE)));
    window->assertNoEvents();
    spyWindow->assertNoEvents();
}
TEST_F(InputDispatcherTest, HoverEnterMouseClickAndHoverExit) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Window", ADISPLAY_ID_DEFAULT);
    window->setFrame(Rect(0, 0, 1200, 800));
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_HOVER_ENTER,
                                                   AINPUT_SOURCE_MOUSE)
                                        .pointer(PointerBuilder(0, AMOTION_EVENT_TOOL_TYPE_MOUSE)
                                                         .x(300)
                                                         .y(400))
                                        .build()));
    window->consumeMotionEvent(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_ENTER));
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_MOUSE)
                                        .buttonState(AMOTION_EVENT_BUTTON_PRIMARY)
                                        .pointer(PointerBuilder(0, AMOTION_EVENT_TOOL_TYPE_MOUSE)
                                                         .x(300)
                                                         .y(400))
                                        .build()));
    window->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_BUTTON_PRESS,
                                                   AINPUT_SOURCE_MOUSE)
                                        .buttonState(AMOTION_EVENT_BUTTON_PRIMARY)
                                        .actionButton(AMOTION_EVENT_BUTTON_PRIMARY)
                                        .pointer(PointerBuilder(0, AMOTION_EVENT_TOOL_TYPE_MOUSE)
                                                         .x(300)
                                                         .y(400))
                                        .build()));
    window->consumeMotionEvent(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_PRESS));
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_BUTTON_RELEASE,
                                                   AINPUT_SOURCE_MOUSE)
                                        .buttonState(0)
                                        .actionButton(AMOTION_EVENT_BUTTON_PRIMARY)
                                        .pointer(PointerBuilder(0, AMOTION_EVENT_TOOL_TYPE_MOUSE)
                                                         .x(300)
                                                         .y(400))
                                        .build()));
    window->consumeMotionEvent(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_RELEASE));
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_UP, AINPUT_SOURCE_MOUSE)
                                        .buttonState(0)
                                        .pointer(PointerBuilder(0, AMOTION_EVENT_TOOL_TYPE_MOUSE)
                                                         .x(300)
                                                         .y(400))
                                        .build()));
    window->consumeMotionUp(ADISPLAY_ID_DEFAULT);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_HOVER_EXIT,
                                                   AINPUT_SOURCE_MOUSE)
                                        .pointer(PointerBuilder(0, AMOTION_EVENT_TOOL_TYPE_MOUSE)
                                                         .x(300)
                                                         .y(400))
                                        .build()));
    window->consumeMotionEvent(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_EXIT));
}
TEST_F(InputDispatcherTest, MouseHoverAndTouchTap) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Window", ADISPLAY_ID_DEFAULT);
    window->setFrame(Rect(0, 0, 100, 100));
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    NotifyMotionArgs motionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_HOVER_MOVE, AINPUT_SOURCE_MOUSE,
                               ADISPLAY_ID_DEFAULT, {{50, 50}});
    motionArgs.xCursorPosition = 50;
    motionArgs.yCursorPosition = 50;
    mDispatcher->notifyMotion(&motionArgs);
    ASSERT_NO_FATAL_FAILURE(
            window->consumeMotionEvent(AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_ENTER),
                                             WithSource(AINPUT_SOURCE_MOUSE))));
    ASSERT_NO_FATAL_FAILURE(
            window->consumeMotionEvent(AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE),
                                             WithSource(AINPUT_SOURCE_MOUSE))));
    motionArgs = generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                                    ADISPLAY_ID_DEFAULT, {{10, 10}});
    mDispatcher->notifyMotion(&motionArgs);
    ASSERT_NO_FATAL_FAILURE(
            window->consumeMotionEvent(AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN),
                                             WithSource(AINPUT_SOURCE_TOUCHSCREEN))));
    motionArgs = generateMotionArgs(AMOTION_EVENT_ACTION_UP, AINPUT_SOURCE_TOUCHSCREEN,
                                    ADISPLAY_ID_DEFAULT, {{10, 10}});
    mDispatcher->notifyMotion(&motionArgs);
    ASSERT_NO_FATAL_FAILURE(
            window->consumeMotionEvent(AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP),
                                             WithSource(AINPUT_SOURCE_TOUCHSCREEN))));
}
TEST_F(InputDispatcherTest, HoverEnterMoveRemoveWindowsInSecondDisplay) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> windowDefaultDisplay =
            sp<FakeWindowHandle>::make(application, mDispatcher, "DefaultDisplay",
                                       ADISPLAY_ID_DEFAULT);
    windowDefaultDisplay->setFrame(Rect(0, 0, 600, 800));
    sp<FakeWindowHandle> windowSecondDisplay =
            sp<FakeWindowHandle>::make(application, mDispatcher, "SecondDisplay",
                                       SECOND_DISPLAY_ID);
    windowSecondDisplay->setFrame(Rect(0, 0, 600, 800));
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {windowDefaultDisplay}},
                                  {SECOND_DISPLAY_ID, {windowSecondDisplay}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_HOVER_MOVE,
                                                   AINPUT_SOURCE_MOUSE)
                                        .displayId(ADISPLAY_ID_DEFAULT)
                                        .pointer(PointerBuilder(0, AMOTION_EVENT_TOOL_TYPE_MOUSE)
                                                         .x(300)
                                                         .y(600))
                                        .build()));
    windowDefaultDisplay->consumeMotionEvent(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_ENTER));
    windowDefaultDisplay->consumeMotionEvent(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE));
    mDispatcher->setInputWindows(
            {{ADISPLAY_ID_DEFAULT, {windowDefaultDisplay}}, {SECOND_DISPLAY_ID, {}}});
    windowDefaultDisplay->assertNoEvents();
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {windowDefaultDisplay}},
                                  {SECOND_DISPLAY_ID, {windowSecondDisplay}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_HOVER_MOVE,
                                                   AINPUT_SOURCE_MOUSE)
                                        .displayId(ADISPLAY_ID_DEFAULT)
                                        .pointer(PointerBuilder(0, AMOTION_EVENT_TOOL_TYPE_MOUSE)
                                                         .x(400)
                                                         .y(700))
                                        .build()));
    windowDefaultDisplay->consumeMotionEvent(
            AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE),
                  WithSource(AINPUT_SOURCE_MOUSE)));
    windowDefaultDisplay->assertNoEvents();
}
TEST_F(InputDispatcherTest, DispatchMouseEventsUnderCursor) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> windowLeft =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Left", ADISPLAY_ID_DEFAULT);
    windowLeft->setFrame(Rect(0, 0, 600, 800));
    sp<FakeWindowHandle> windowRight =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Right", ADISPLAY_ID_DEFAULT);
    windowRight->setFrame(Rect(600, 0, 1200, 800));
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {windowLeft, windowRight}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_MOUSE,
                                ADISPLAY_ID_DEFAULT, {610, 400}, {599, 400}));
    windowLeft->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    windowRight->assertNoEvents();
}
TEST_F(InputDispatcherTest, NotifyDeviceReset_CancelsKeyStream) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Fake Window", ADISPLAY_ID_DEFAULT);
    window->setFocusable(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    setFocusedWindow(window);
    window->consumeFocusEvent(true);
    NotifyKeyArgs keyArgs = generateKeyArgs(AKEY_EVENT_ACTION_DOWN, ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyKey(&keyArgs);
    window->consumeKeyDown(ADISPLAY_ID_DEFAULT);
    NotifyDeviceResetArgs args(10 , 20 , DEVICE_ID);
    mDispatcher->notifyDeviceReset(&args);
    window->consumeEvent(AINPUT_EVENT_TYPE_KEY, AKEY_EVENT_ACTION_UP, ADISPLAY_ID_DEFAULT,
                         AKEY_EVENT_FLAG_CANCELED);
}
TEST_F(InputDispatcherTest, NotifyDeviceReset_CancelsMotionStream) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Fake Window", ADISPLAY_ID_DEFAULT);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    NotifyMotionArgs motionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyMotion(&motionArgs);
    window->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    NotifyDeviceResetArgs args(10 , 20 , DEVICE_ID);
    mDispatcher->notifyDeviceReset(&args);
    window->consumeEvent(AINPUT_EVENT_TYPE_MOTION, AMOTION_EVENT_ACTION_CANCEL, ADISPLAY_ID_DEFAULT,
                         0 );
}
TEST_F(InputDispatcherTest, InterceptKeyByPolicy) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Fake Window", ADISPLAY_ID_DEFAULT);
    window->setFocusable(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    setFocusedWindow(window);
    window->consumeFocusEvent(true);
    NotifyKeyArgs keyArgs = generateKeyArgs(AKEY_EVENT_ACTION_DOWN, ADISPLAY_ID_DEFAULT);
    const std::chrono::milliseconds interceptKeyTimeout = 50ms;
    const nsecs_t injectTime = keyArgs.eventTime;
    mFakePolicy->setInterceptKeyTimeout(interceptKeyTimeout);
    mDispatcher->notifyKey(&keyArgs);
    window->consumeKeyDown(ADISPLAY_ID_DEFAULT);
    ASSERT_TRUE((systemTime(SYSTEM_TIME_MONOTONIC) - injectTime) >=
                std::chrono::nanoseconds(interceptKeyTimeout).count());
}
TEST_F(InputDispatcherTest, InterceptKeyIfKeyUp) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Fake Window", ADISPLAY_ID_DEFAULT);
    window->setFocusable(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    setFocusedWindow(window);
    window->consumeFocusEvent(true);
    NotifyKeyArgs keyDown = generateKeyArgs(AKEY_EVENT_ACTION_DOWN, ADISPLAY_ID_DEFAULT);
    NotifyKeyArgs keyUp = generateKeyArgs(AKEY_EVENT_ACTION_UP, ADISPLAY_ID_DEFAULT);
    mFakePolicy->setInterceptKeyTimeout(150ms);
    mDispatcher->notifyKey(&keyDown);
    mDispatcher->notifyKey(&keyUp);
    window->consumeKeyDown(ADISPLAY_ID_DEFAULT);
    window->consumeKeyUp(ADISPLAY_ID_DEFAULT);
}
TEST_F(InputDispatcherTest, ActionOutsideForOwnedWindowHasValidCoordinates) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "First Window", ADISPLAY_ID_DEFAULT);
    window->setFrame(Rect{0, 0, 100, 100});
    sp<FakeWindowHandle> outsideWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Second Window",
                                       ADISPLAY_ID_DEFAULT);
    outsideWindow->setFrame(Rect{100, 100, 200, 200});
    outsideWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {outsideWindow, window}}});
    NotifyMotionArgs motionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT, {PointF{50, 50}});
    mDispatcher->notifyMotion(&motionArgs);
    window->consumeMotionDown();
    outsideWindow->consumeMotionEvent(
            AllOf(WithMotionAction(ACTION_OUTSIDE), WithCoords(-50, -50)));
}
TEST_F(InputDispatcherTest, ActionOutsideSentOnlyWhenAWindowIsTouched) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "First Window", ADISPLAY_ID_DEFAULT);
    window->setWatchOutsideTouch(true);
    window->setFrame(Rect{0, 0, 100, 100});
    sp<FakeWindowHandle> secondWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Second Window",
                                       ADISPLAY_ID_DEFAULT);
    secondWindow->setFrame(Rect{100, 100, 200, 200});
    sp<FakeWindowHandle> thirdWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Third Window",
                                       ADISPLAY_ID_DEFAULT);
    thirdWindow->setFrame(Rect{200, 200, 300, 300});
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window, secondWindow, thirdWindow}}});
    NotifyMotionArgs motionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT, {PointF{-10, -10}});
    mDispatcher->notifyMotion(&motionArgs);
    window->assertNoEvents();
    secondWindow->assertNoEvents();
    motionArgs = generateMotionArgs(POINTER_1_DOWN, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                                    {PointF{-10, -10}, PointF{105, 105}});
    mDispatcher->notifyMotion(&motionArgs);
    const std::map<int32_t, PointF> expectedPointers{{0, PointF{-10, -10}}, {1, PointF{105, 105}}};
    window->consumeMotionEvent(
            AllOf(WithMotionAction(ACTION_OUTSIDE), WithPointers(expectedPointers)));
    secondWindow->consumeMotionDown();
    thirdWindow->assertNoEvents();
    motionArgs = generateMotionArgs(POINTER_2_DOWN, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                                    {PointF{-10, -10}, PointF{105, 105}, PointF{205, 205}});
    mDispatcher->notifyMotion(&motionArgs);
    window->assertNoEvents();
    secondWindow->consumeMotionMove();
    thirdWindow->consumeMotionDown();
}
TEST_F(InputDispatcherTest, OnWindowInfosChanged_RemoveAllWindowsOnDisplay) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Fake Window", ADISPLAY_ID_DEFAULT);
    window->setFocusable(true);
    mDispatcher->onWindowInfosChanged({*window->getInfo()}, {});
    setFocusedWindow(window);
    window->consumeFocusEvent(true);
    NotifyKeyArgs keyDown = generateKeyArgs(AKEY_EVENT_ACTION_DOWN, ADISPLAY_ID_DEFAULT);
    NotifyKeyArgs keyUp = generateKeyArgs(AKEY_EVENT_ACTION_UP, ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyKey(&keyDown);
    mDispatcher->notifyKey(&keyUp);
    window->consumeKeyDown(ADISPLAY_ID_DEFAULT);
    window->consumeKeyUp(ADISPLAY_ID_DEFAULT);
    mDispatcher->onWindowInfosChanged({}, {});
    window->consumeFocusEvent(false);
    mDispatcher->notifyKey(&keyDown);
    mDispatcher->notifyKey(&keyUp);
    window->assertNoEvents();
}
TEST_F(InputDispatcherTest, NonSplitTouchableWindowReceivesMultiTouch) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Fake Window", ADISPLAY_ID_DEFAULT);
    window->setPreventSplitting(true);
    window->setWindowOffset(20, 40);
    mDispatcher->onWindowInfosChanged({*window->getInfo()}, {});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {50, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    const MotionEvent secondFingerDownEvent =
            MotionEventBuilder(POINTER_1_DOWN, AINPUT_SOURCE_TOUCHSCREEN)
                    .displayId(ADISPLAY_ID_DEFAULT)
                    .eventTime(systemTime(SYSTEM_TIME_MONOTONIC))
                    .pointer(PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER).x(50).y(50))
                    .pointer(PointerBuilder( 1, AMOTION_EVENT_TOOL_TYPE_FINGER)
                                     .x(-30)
                                     .y(-50))
                    .build();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, secondFingerDownEvent, INJECT_EVENT_TIMEOUT,
                                InputEventInjectionSync::WAIT_FOR_RESULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    const MotionEvent* event = window->consumeMotion();
    EXPECT_EQ(POINTER_1_DOWN, event->getAction());
    EXPECT_EQ(70, event->getX(0));
    EXPECT_EQ(90, event->getY(0));
    EXPECT_EQ(-10, event->getX(1));
    EXPECT_EQ(-10, event->getY(1));
}
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
    std::pair<sp<FakeWindowHandle>, sp<FakeWindowHandle>> setupScaledDisplayScenario() {
        ui::Transform displayTransform;
        displayTransform.set(2, 0, 0, 4);
        addDisplayInfo(ADISPLAY_ID_DEFAULT, displayTransform);
        std::shared_ptr<FakeApplicationHandle> application =
                std::make_shared<FakeApplicationHandle>();
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
TEST_F(InputDispatcherDisplayProjectionTest, HitTestsInDisplaySpace) {
    auto [firstWindow, secondWindow] = setupScaledDisplayScenario();
    NotifyMotionArgs downMotionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT, {PointF{75, 55}});
    mDispatcher->notifyMotion(&downMotionArgs);
    firstWindow->consumeMotionDown();
    secondWindow->assertNoEvents();
}
TEST_F(InputDispatcherDisplayProjectionTest, InjectionInLogicalDisplaySpace) {
    auto [firstWindow, secondWindow] = setupScaledDisplayScenario();
    injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                     PointF{75 * 2, 55 * 4});
    firstWindow->consumeMotionDown();
    secondWindow->assertNoEvents();
}
TEST_F(InputDispatcherDisplayProjectionTest, InjectionWithTransformInLogicalDisplaySpace) {
    auto [firstWindow, secondWindow] = setupScaledDisplayScenario();
    const std::array<float, 9> matrix = {1.1, 2.2, 3.3, 4.4, 5.5, 6.6, 0.0, 0.0, 1.0};
    ui::Transform injectedEventTransform;
    injectedEventTransform.set(matrix);
    const vec2 expectedPoint{75, 55};
    const vec2 untransformedPoint = injectedEventTransform.inverse().transform(expectedPoint);
    MotionEvent event = MotionEventBuilder(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN)
                                .displayId(ADISPLAY_ID_DEFAULT)
                                .eventTime(systemTime(SYSTEM_TIME_MONOTONIC))
                                .pointer(PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER)
                                                 .x(untransformedPoint.x)
                                                 .y(untransformedPoint.y))
                                .build();
    event.transform(matrix);
    injectMotionEvent(mDispatcher, event, INJECT_EVENT_TIMEOUT,
                      InputEventInjectionSync::WAIT_FOR_RESULT);
    firstWindow->consumeMotionDown();
    secondWindow->assertNoEvents();
}
TEST_F(InputDispatcherDisplayProjectionTest, WindowGetsEventsInCorrectCoordinateSpace) {
    auto [firstWindow, secondWindow] = setupScaledDisplayScenario();
    NotifyMotionArgs downMotionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT, {PointF{150, 220}});
    mDispatcher->notifyMotion(&downMotionArgs);
    firstWindow->assertNoEvents();
    const MotionEvent* event = secondWindow->consumeMotion();
    EXPECT_EQ(AMOTION_EVENT_ACTION_DOWN, event->getAction());
    EXPECT_EQ(300, event->getRawX(0));
    EXPECT_EQ(880, event->getRawY(0));
    EXPECT_EQ(100, event->getX(0));
    EXPECT_EQ(80, event->getY(0));
}
using TransferFunction = std::function<bool(const std::unique_ptr<InputDispatcher>& dispatcher,
                                            sp<IBinder>, sp<IBinder>)>;
class TransferTouchFixture : public InputDispatcherTest,
                             public ::testing::WithParamInterface<TransferFunction> {};
TEST_P(TransferTouchFixture, TransferTouch_OnePointer) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> firstWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "First Window",
                                       ADISPLAY_ID_DEFAULT);
    firstWindow->setDupTouchToWallpaper(true);
    sp<FakeWindowHandle> secondWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Second Window",
                                       ADISPLAY_ID_DEFAULT);
    sp<FakeWindowHandle> wallpaper =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Wallpaper", ADISPLAY_ID_DEFAULT);
    wallpaper->setIsWallpaper(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {firstWindow, secondWindow, wallpaper}}});
    NotifyMotionArgs downMotionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyMotion(&downMotionArgs);
    firstWindow->consumeMotionDown();
    secondWindow->assertNoEvents();
    wallpaper->consumeMotionDown(ADISPLAY_ID_DEFAULT, expectedWallpaperFlags);
    TransferFunction f = GetParam();
    const bool success = f(mDispatcher, firstWindow->getToken(), secondWindow->getToken());
    ASSERT_TRUE(success);
    firstWindow->consumeMotionCancel();
    secondWindow->consumeMotionDown();
    wallpaper->consumeMotionCancel(ADISPLAY_ID_DEFAULT, expectedWallpaperFlags);
    NotifyMotionArgs upMotionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_UP, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyMotion(&upMotionArgs);
    firstWindow->assertNoEvents();
    secondWindow->consumeMotionUp();
    wallpaper->assertNoEvents();
}
TEST_P(TransferTouchFixture, TransferTouch_MultipleWindowsWithSpy) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> spyWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Spy", ADISPLAY_ID_DEFAULT);
    spyWindow->setTrustedOverlay(true);
    spyWindow->setSpy(true);
    sp<FakeWindowHandle> firstWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "First", ADISPLAY_ID_DEFAULT);
    sp<FakeWindowHandle> secondWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Second", ADISPLAY_ID_DEFAULT);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {spyWindow, firstWindow, secondWindow}}});
    NotifyMotionArgs downMotionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyMotion(&downMotionArgs);
    spyWindow->consumeMotionDown();
    firstWindow->consumeMotionDown();
    TransferFunction f = GetParam();
    const bool success = f(mDispatcher, firstWindow->getToken(), secondWindow->getToken());
    ASSERT_TRUE(success);
    firstWindow->consumeMotionCancel();
    secondWindow->consumeMotionDown();
    NotifyMotionArgs upMotionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_UP, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyMotion(&upMotionArgs);
    firstWindow->assertNoEvents();
    spyWindow->consumeMotionUp();
    secondWindow->consumeMotionUp();
}
TEST_P(TransferTouchFixture, TransferTouch_TwoPointersNonSplitTouch) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    PointF touchPoint = {10, 10};
    sp<FakeWindowHandle> firstWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "First Window",
                                       ADISPLAY_ID_DEFAULT);
    firstWindow->setPreventSplitting(true);
    sp<FakeWindowHandle> secondWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Second Window",
                                       ADISPLAY_ID_DEFAULT);
    secondWindow->setPreventSplitting(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {firstWindow, secondWindow}}});
    NotifyMotionArgs downMotionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT, {touchPoint});
    mDispatcher->notifyMotion(&downMotionArgs);
    firstWindow->consumeMotionDown();
    secondWindow->assertNoEvents();
    NotifyMotionArgs pointerDownMotionArgs =
            generateMotionArgs(POINTER_1_DOWN, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {touchPoint, touchPoint});
    mDispatcher->notifyMotion(&pointerDownMotionArgs);
    firstWindow->consumeMotionPointerDown(1);
    secondWindow->assertNoEvents();
    TransferFunction f = GetParam();
    bool success = f(mDispatcher, firstWindow->getToken(), secondWindow->getToken());
    ASSERT_TRUE(success);
    firstWindow->consumeMotionCancel();
    secondWindow->consumeMotionDown();
    secondWindow->consumeMotionPointerDown(1);
    NotifyMotionArgs pointerUpMotionArgs =
            generateMotionArgs(POINTER_1_UP, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {touchPoint, touchPoint});
    mDispatcher->notifyMotion(&pointerUpMotionArgs);
    firstWindow->assertNoEvents();
    secondWindow->consumeMotionPointerUp(1);
    NotifyMotionArgs upMotionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_UP, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyMotion(&upMotionArgs);
    firstWindow->assertNoEvents();
    secondWindow->consumeMotionUp();
}
TEST_P(TransferTouchFixture, TransferTouch_MultipleWallpapers) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> firstWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "First Window",
                                       ADISPLAY_ID_DEFAULT);
    firstWindow->setDupTouchToWallpaper(true);
    sp<FakeWindowHandle> secondWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Second Window",
                                       ADISPLAY_ID_DEFAULT);
    secondWindow->setDupTouchToWallpaper(true);
    sp<FakeWindowHandle> wallpaper1 =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Wallpaper1", ADISPLAY_ID_DEFAULT);
    wallpaper1->setIsWallpaper(true);
    sp<FakeWindowHandle> wallpaper2 =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Wallpaper2", ADISPLAY_ID_DEFAULT);
    wallpaper2->setIsWallpaper(true);
    mDispatcher->setInputWindows(
            {{ADISPLAY_ID_DEFAULT, {firstWindow, wallpaper1, secondWindow, wallpaper2}}});
    NotifyMotionArgs downMotionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyMotion(&downMotionArgs);
    firstWindow->consumeMotionDown();
    secondWindow->assertNoEvents();
    wallpaper1->consumeMotionDown(ADISPLAY_ID_DEFAULT, expectedWallpaperFlags);
    wallpaper2->assertNoEvents();
    TransferFunction f = GetParam();
    bool success = f(mDispatcher, firstWindow->getToken(), secondWindow->getToken());
    ASSERT_TRUE(success);
    firstWindow->consumeMotionCancel();
    secondWindow->consumeMotionDown();
    wallpaper1->consumeMotionCancel(ADISPLAY_ID_DEFAULT, expectedWallpaperFlags);
    wallpaper2->consumeMotionDown(ADISPLAY_ID_DEFAULT, expectedWallpaperFlags);
    NotifyMotionArgs upMotionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_UP, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyMotion(&upMotionArgs);
    firstWindow->assertNoEvents();
    secondWindow->consumeMotionUp();
    wallpaper1->assertNoEvents();
    wallpaper2->consumeMotionUp(ADISPLAY_ID_DEFAULT, expectedWallpaperFlags);
}
INSTANTIATE_TEST_SUITE_P(TransferFunctionTests, TransferTouchFixture,
                         ::testing::Values(
                                 [&](const std::unique_ptr<InputDispatcher>& dispatcher,
                                     sp<IBinder> , sp<IBinder> destChannelToken) {
                                     return dispatcher->transferTouch(destChannelToken,
                                                                      ADISPLAY_ID_DEFAULT);
                                 },
                                 [&](const std::unique_ptr<InputDispatcher>& dispatcher,
                                     sp<IBinder> from, sp<IBinder> to) {
                                     return dispatcher->transferTouchFocus(from, to,
                                                                           false );
                                 }));
TEST_F(InputDispatcherTest, TransferTouchFocus_TwoPointersSplitTouch) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> firstWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "First Window",
                                       ADISPLAY_ID_DEFAULT);
    firstWindow->setFrame(Rect(0, 0, 600, 400));
    sp<FakeWindowHandle> secondWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Second Window",
                                       ADISPLAY_ID_DEFAULT);
    secondWindow->setFrame(Rect(0, 400, 600, 800));
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {firstWindow, secondWindow}}});
    PointF pointInFirst = {300, 200};
    PointF pointInSecond = {300, 600};
    NotifyMotionArgs firstDownMotionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT, {pointInFirst});
    mDispatcher->notifyMotion(&firstDownMotionArgs);
    firstWindow->consumeMotionDown();
    secondWindow->assertNoEvents();
    NotifyMotionArgs secondDownMotionArgs =
            generateMotionArgs(POINTER_1_DOWN, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {pointInFirst, pointInSecond});
    mDispatcher->notifyMotion(&secondDownMotionArgs);
    firstWindow->consumeMotionMove();
    secondWindow->consumeMotionDown();
    mDispatcher->transferTouchFocus(firstWindow->getToken(), secondWindow->getToken());
    firstWindow->consumeMotionCancel();
    secondWindow->consumeMotionPointerDown(1);
    NotifyMotionArgs pointerUpMotionArgs =
            generateMotionArgs(POINTER_1_UP, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {pointInFirst, pointInSecond});
    mDispatcher->notifyMotion(&pointerUpMotionArgs);
    firstWindow->assertNoEvents();
    secondWindow->consumeMotionPointerUp(1);
    NotifyMotionArgs upMotionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_UP, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyMotion(&upMotionArgs);
    firstWindow->assertNoEvents();
    secondWindow->consumeMotionUp();
}
TEST_F(InputDispatcherTest, TransferTouch_TwoPointersSplitTouch) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> firstWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "First Window",
                                       ADISPLAY_ID_DEFAULT);
    firstWindow->setFrame(Rect(0, 0, 600, 400));
    sp<FakeWindowHandle> secondWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Second Window",
                                       ADISPLAY_ID_DEFAULT);
    secondWindow->setFrame(Rect(0, 400, 600, 800));
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {firstWindow, secondWindow}}});
    PointF pointInFirst = {300, 200};
    PointF pointInSecond = {300, 600};
    NotifyMotionArgs firstDownMotionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT, {pointInFirst});
    mDispatcher->notifyMotion(&firstDownMotionArgs);
    firstWindow->consumeMotionDown();
    secondWindow->assertNoEvents();
    NotifyMotionArgs secondDownMotionArgs =
            generateMotionArgs(POINTER_1_DOWN, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {pointInFirst, pointInSecond});
    mDispatcher->notifyMotion(&secondDownMotionArgs);
    firstWindow->consumeMotionMove();
    secondWindow->consumeMotionDown();
    const bool transferred =
            mDispatcher->transferTouch(secondWindow->getToken(), ADISPLAY_ID_DEFAULT);
    ASSERT_FALSE(transferred);
    firstWindow->assertNoEvents();
    secondWindow->assertNoEvents();
    NotifyMotionArgs pointerUpMotionArgs =
            generateMotionArgs(POINTER_1_UP, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {pointInFirst, pointInSecond});
    mDispatcher->notifyMotion(&pointerUpMotionArgs);
    firstWindow->consumeMotionMove();
    secondWindow->consumeMotionUp();
    NotifyMotionArgs upMotionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_UP, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyMotion(&upMotionArgs);
    firstWindow->consumeMotionUp();
    secondWindow->assertNoEvents();
}
TEST_F(InputDispatcherTest, TransferTouchFocus_CloneSurface) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> firstWindowInPrimary =
            sp<FakeWindowHandle>::make(application, mDispatcher, "D_1_W1", ADISPLAY_ID_DEFAULT);
    firstWindowInPrimary->setFrame(Rect(0, 0, 100, 100));
    sp<FakeWindowHandle> secondWindowInPrimary =
            sp<FakeWindowHandle>::make(application, mDispatcher, "D_1_W2", ADISPLAY_ID_DEFAULT);
    secondWindowInPrimary->setFrame(Rect(100, 0, 200, 100));
    sp<FakeWindowHandle> mirrorWindowInPrimary =
            firstWindowInPrimary->clone(application, mDispatcher, ADISPLAY_ID_DEFAULT);
    mirrorWindowInPrimary->setFrame(Rect(0, 100, 100, 200));
    sp<FakeWindowHandle> firstWindowInSecondary =
            firstWindowInPrimary->clone(application, mDispatcher, SECOND_DISPLAY_ID);
    firstWindowInSecondary->setFrame(Rect(0, 0, 100, 100));
    sp<FakeWindowHandle> secondWindowInSecondary =
            secondWindowInPrimary->clone(application, mDispatcher, SECOND_DISPLAY_ID);
    secondWindowInPrimary->setFrame(Rect(100, 0, 200, 100));
    mDispatcher->setInputWindows(
            {{SECOND_DISPLAY_ID, {firstWindowInSecondary, secondWindowInSecondary}},
             {ADISPLAY_ID_DEFAULT,
              {mirrorWindowInPrimary, firstWindowInPrimary, secondWindowInPrimary}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {50, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    firstWindowInPrimary->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    ASSERT_TRUE(mDispatcher->transferTouchFocus(firstWindowInPrimary->getToken(),
                                                secondWindowInPrimary->getToken()));
    firstWindowInPrimary->consumeMotionCancel();
    secondWindowInPrimary->consumeMotionDown();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN,
                                ADISPLAY_ID_DEFAULT, {150, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    firstWindowInPrimary->assertNoEvents();
    secondWindowInPrimary->consumeMotionMove();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionUp(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                             {150, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    firstWindowInPrimary->assertNoEvents();
    secondWindowInPrimary->consumeMotionUp();
}
TEST_F(InputDispatcherTest, TransferTouch_CloneSurface) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> firstWindowInPrimary =
            sp<FakeWindowHandle>::make(application, mDispatcher, "D_1_W1", ADISPLAY_ID_DEFAULT);
    firstWindowInPrimary->setFrame(Rect(0, 0, 100, 100));
    sp<FakeWindowHandle> secondWindowInPrimary =
            sp<FakeWindowHandle>::make(application, mDispatcher, "D_1_W2", ADISPLAY_ID_DEFAULT);
    secondWindowInPrimary->setFrame(Rect(100, 0, 200, 100));
    sp<FakeWindowHandle> mirrorWindowInPrimary =
            firstWindowInPrimary->clone(application, mDispatcher, ADISPLAY_ID_DEFAULT);
    mirrorWindowInPrimary->setFrame(Rect(0, 100, 100, 200));
    sp<FakeWindowHandle> firstWindowInSecondary =
            firstWindowInPrimary->clone(application, mDispatcher, SECOND_DISPLAY_ID);
    firstWindowInSecondary->setFrame(Rect(0, 0, 100, 100));
    sp<FakeWindowHandle> secondWindowInSecondary =
            secondWindowInPrimary->clone(application, mDispatcher, SECOND_DISPLAY_ID);
    secondWindowInPrimary->setFrame(Rect(100, 0, 200, 100));
    mDispatcher->setInputWindows(
            {{SECOND_DISPLAY_ID, {firstWindowInSecondary, secondWindowInSecondary}},
             {ADISPLAY_ID_DEFAULT,
              {mirrorWindowInPrimary, firstWindowInPrimary, secondWindowInPrimary}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, SECOND_DISPLAY_ID, {50, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    firstWindowInPrimary->consumeMotionDown(SECOND_DISPLAY_ID);
    ASSERT_TRUE(mDispatcher->transferTouch(secondWindowInSecondary->getToken(), SECOND_DISPLAY_ID));
    firstWindowInPrimary->consumeMotionCancel(SECOND_DISPLAY_ID);
    secondWindowInPrimary->consumeMotionDown(SECOND_DISPLAY_ID);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN,
                                SECOND_DISPLAY_ID, {150, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    firstWindowInPrimary->assertNoEvents();
    secondWindowInPrimary->consumeMotionMove(SECOND_DISPLAY_ID);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionUp(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, SECOND_DISPLAY_ID, {150, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    firstWindowInPrimary->assertNoEvents();
    secondWindowInPrimary->consumeMotionUp(SECOND_DISPLAY_ID);
}
TEST_F(InputDispatcherTest, FocusedWindow_ReceivesFocusEventAndKeyEvent) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Fake Window", ADISPLAY_ID_DEFAULT);
    window->setFocusable(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    setFocusedWindow(window);
    window->consumeFocusEvent(true);
    NotifyKeyArgs keyArgs = generateKeyArgs(AKEY_EVENT_ACTION_DOWN, ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyKey(&keyArgs);
    window->consumeKeyDown(ADISPLAY_ID_DEFAULT);
}
TEST_F(InputDispatcherTest, UnfocusedWindow_DoesNotReceiveFocusEventOrKeyEvent) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Fake Window", ADISPLAY_ID_DEFAULT);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    NotifyKeyArgs keyArgs = generateKeyArgs(AKEY_EVENT_ACTION_DOWN, ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyKey(&keyArgs);
    mDispatcher->waitForIdle();
    window->assertNoEvents();
}
TEST_F(InputDispatcherTest, UnfocusedWindow_ReceivesMotionsButNotKeys) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Fake Window", ADISPLAY_ID_DEFAULT);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    NotifyKeyArgs keyArgs = generateKeyArgs(AKEY_EVENT_ACTION_DOWN, ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyKey(&keyArgs);
    NotifyMotionArgs motionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyMotion(&motionArgs);
    window->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    window->assertNoEvents();
}
TEST_F(InputDispatcherTest, PointerCancel_SendCancelWhenSplitTouch) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> firstWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "First Window",
                                       ADISPLAY_ID_DEFAULT);
    firstWindow->setFrame(Rect(0, 0, 600, 400));
    sp<FakeWindowHandle> secondWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Second Window",
                                       ADISPLAY_ID_DEFAULT);
    secondWindow->setFrame(Rect(0, 400, 600, 800));
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {firstWindow, secondWindow}}});
    PointF pointInFirst = {300, 200};
    PointF pointInSecond = {300, 600};
    NotifyMotionArgs firstDownMotionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT, {pointInFirst});
    mDispatcher->notifyMotion(&firstDownMotionArgs);
    firstWindow->consumeMotionDown();
    secondWindow->assertNoEvents();
    NotifyMotionArgs secondDownMotionArgs =
            generateMotionArgs(POINTER_1_DOWN, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {pointInFirst, pointInSecond});
    mDispatcher->notifyMotion(&secondDownMotionArgs);
    firstWindow->consumeMotionMove();
    secondWindow->consumeMotionDown();
    NotifyMotionArgs pointerUpMotionArgs =
            generateMotionArgs(POINTER_1_UP, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {pointInFirst, pointInSecond});
    pointerUpMotionArgs.flags |= AMOTION_EVENT_FLAG_CANCELED;
    mDispatcher->notifyMotion(&pointerUpMotionArgs);
    firstWindow->consumeMotionMove(ADISPLAY_ID_DEFAULT, AMOTION_EVENT_FLAG_CANCELED);
    secondWindow->consumeMotionCancel(ADISPLAY_ID_DEFAULT, AMOTION_EVENT_FLAG_CANCELED);
    NotifyMotionArgs upMotionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_UP, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyMotion(&upMotionArgs);
    firstWindow->consumeMotionUp();
    secondWindow->assertNoEvents();
}
TEST_F(InputDispatcherTest, SendTimeline_DoesNotCrashDispatcher) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Window", ADISPLAY_ID_DEFAULT);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    std::array<nsecs_t, GraphicsTimeline::SIZE> graphicsTimeline;
    graphicsTimeline[GraphicsTimeline::GPU_COMPLETED_TIME] = 2;
    graphicsTimeline[GraphicsTimeline::PRESENT_TIME] = 3;
    window->sendTimeline(1 , graphicsTimeline);
    window->assertNoEvents();
    mDispatcher->waitForIdle();
}
class FakeMonitorReceiver {
public:
    FakeMonitorReceiver(const std::unique_ptr<InputDispatcher>& dispatcher, const std::string name,
                        int32_t displayId) {
        base::Result<std::unique_ptr<InputChannel>> channel =
                dispatcher->createInputMonitor(displayId, name, MONITOR_PID);
        mInputReceiver = std::make_unique<FakeInputReceiver>(std::move(*channel), name);
    }
    sp<IBinder> getToken() { return mInputReceiver->getToken(); }
    void consumeKeyDown(int32_t expectedDisplayId, int32_t expectedFlags = 0) {
        mInputReceiver->consumeEvent(AINPUT_EVENT_TYPE_KEY, AKEY_EVENT_ACTION_DOWN,
                                     expectedDisplayId, expectedFlags);
    }
    std::optional<int32_t> receiveEvent() { return mInputReceiver->receiveEvent(); }
    void finishEvent(uint32_t consumeSeq) { return mInputReceiver->finishEvent(consumeSeq); }
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
                                     0 );
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
    void assertNoEvents() { mInputReceiver->assertNoEvents(); }
private:
    std::unique_ptr<FakeInputReceiver> mInputReceiver;
};
using InputDispatcherMonitorTest = InputDispatcherTest;
TEST_F(InputDispatcherMonitorTest, MonitorTouchIsCanceledWhenForegroundWindowDisappears) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Foreground", ADISPLAY_ID_DEFAULT);
    FakeMonitorReceiver monitor = FakeMonitorReceiver(mDispatcher, "M_1", ADISPLAY_ID_DEFAULT);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {100, 200}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionDown();
    monitor.consumeMotionDown(ADISPLAY_ID_DEFAULT);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN,
                                ADISPLAY_ID_DEFAULT, {110, 200}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionMove();
    monitor.consumeMotionMove(ADISPLAY_ID_DEFAULT);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {}}});
    window->consumeMotionCancel();
    monitor.assertNoEvents();
    ASSERT_EQ(InputEventInjectionResult::FAILED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN,
                                ADISPLAY_ID_DEFAULT, {120, 200}))
            << "Injection should fail because the window was removed";
    window->assertNoEvents();
    monitor.consumeMotionCancel(ADISPLAY_ID_DEFAULT);
}
TEST_F(InputDispatcherMonitorTest, ReceivesMotionEvents) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Fake Window", ADISPLAY_ID_DEFAULT);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    FakeMonitorReceiver monitor = FakeMonitorReceiver(mDispatcher, "M_1", ADISPLAY_ID_DEFAULT);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    monitor.consumeMotionDown(ADISPLAY_ID_DEFAULT);
}
TEST_F(InputDispatcherMonitorTest, MonitorCannotPilferPointers) {
    FakeMonitorReceiver monitor = FakeMonitorReceiver(mDispatcher, "M_1", ADISPLAY_ID_DEFAULT);
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Fake Window", ADISPLAY_ID_DEFAULT);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    monitor.consumeMotionDown(ADISPLAY_ID_DEFAULT);
    window->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    EXPECT_NE(OK, mDispatcher->pilferPointers(monitor.getToken()));
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN,
                                ADISPLAY_ID_DEFAULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    monitor.consumeMotionMove(ADISPLAY_ID_DEFAULT);
    window->consumeMotionMove(ADISPLAY_ID_DEFAULT);
}
TEST_F(InputDispatcherMonitorTest, NoWindowTransform) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Fake Window", ADISPLAY_ID_DEFAULT);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    window->setWindowOffset(20, 40);
    window->setWindowTransform(0, 1, -1, 0);
    FakeMonitorReceiver monitor = FakeMonitorReceiver(mDispatcher, "M_1", ADISPLAY_ID_DEFAULT);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    MotionEvent* event = monitor.consumeMotion();
    ASSERT_EQ(ui::Transform(), event->getTransform());
}
TEST_F(InputDispatcherMonitorTest, InjectionFailsWithNoWindow) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    FakeMonitorReceiver monitor = FakeMonitorReceiver(mDispatcher, "M_1", ADISPLAY_ID_DEFAULT);
    ASSERT_EQ(InputEventInjectionResult::FAILED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT))
            << "Injection should fail if there is a monitor, but no touchable window";
    monitor.assertNoEvents();
}
TEST_F(InputDispatcherTest, TestMoveEvent) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Fake Window", ADISPLAY_ID_DEFAULT);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    NotifyMotionArgs motionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyMotion(&motionArgs);
    window->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    motionArgs.action = AMOTION_EVENT_ACTION_MOVE;
    motionArgs.id += 1;
    motionArgs.eventTime = systemTime(SYSTEM_TIME_MONOTONIC);
    motionArgs.pointerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_X,
                                             motionArgs.pointerCoords[0].getX() - 10);
    mDispatcher->notifyMotion(&motionArgs);
    window->consumeEvent(AINPUT_EVENT_TYPE_MOTION, AMOTION_EVENT_ACTION_MOVE, ADISPLAY_ID_DEFAULT,
                         0 );
}
TEST_F(InputDispatcherTest, TouchModeState_IsSentToApps) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Test window", ADISPLAY_ID_DEFAULT);
    const WindowInfo& windowInfo = *window->getInfo();
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
    window->setFocusable(true);
    SCOPED_TRACE("Check default value of touch mode");
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    setFocusedWindow(window);
    window->consumeFocusEvent(true , true );
    SCOPED_TRACE("Remove the window to trigger focus loss");
    window->setFocusable(false);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    window->consumeFocusEvent(false , true );
    SCOPED_TRACE("Disable touch mode");
    mDispatcher->setInTouchMode(false, windowInfo.ownerPid, windowInfo.ownerUid,
                                true , ADISPLAY_ID_DEFAULT);
    window->consumeTouchModeEvent(false);
    window->setFocusable(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    setFocusedWindow(window);
    window->consumeFocusEvent(true , false );
    SCOPED_TRACE("Remove the window to trigger focus loss");
    window->setFocusable(false);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    window->consumeFocusEvent(false , false );
    SCOPED_TRACE("Enable touch mode again");
    mDispatcher->setInTouchMode(true, windowInfo.ownerPid, windowInfo.ownerUid,
                                true , ADISPLAY_ID_DEFAULT);
    window->consumeTouchModeEvent(true);
    window->setFocusable(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    setFocusedWindow(window);
    window->consumeFocusEvent(true , true );
    window->assertNoEvents();
}
TEST_F(InputDispatcherTest, VerifyInputEvent_KeyEvent) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Test window", ADISPLAY_ID_DEFAULT);
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
    window->setFocusable(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    setFocusedWindow(window);
    window->consumeFocusEvent(true , true );
    NotifyKeyArgs keyArgs = generateKeyArgs(AKEY_EVENT_ACTION_DOWN);
    mDispatcher->notifyKey(&keyArgs);
    InputEvent* event = window->consume();
    ASSERT_NE(event, nullptr);
    std::unique_ptr<VerifiedInputEvent> verified = mDispatcher->verifyInputEvent(*event);
    ASSERT_NE(verified, nullptr);
    ASSERT_EQ(verified->type, VerifiedInputEvent::Type::KEY);
    ASSERT_EQ(keyArgs.eventTime, verified->eventTimeNanos);
    ASSERT_EQ(keyArgs.deviceId, verified->deviceId);
    ASSERT_EQ(keyArgs.source, verified->source);
    ASSERT_EQ(keyArgs.displayId, verified->displayId);
    const VerifiedKeyEvent& verifiedKey = static_cast<const VerifiedKeyEvent&>(*verified);
    ASSERT_EQ(keyArgs.action, verifiedKey.action);
    ASSERT_EQ(keyArgs.flags & VERIFIED_KEY_EVENT_FLAGS, verifiedKey.flags);
    ASSERT_EQ(keyArgs.downTime, verifiedKey.downTimeNanos);
    ASSERT_EQ(keyArgs.keyCode, verifiedKey.keyCode);
    ASSERT_EQ(keyArgs.scanCode, verifiedKey.scanCode);
    ASSERT_EQ(keyArgs.metaState, verifiedKey.metaState);
    ASSERT_EQ(0, verifiedKey.repeatCount);
}
TEST_F(InputDispatcherTest, VerifyInputEvent_MotionEvent) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Test window", ADISPLAY_ID_DEFAULT);
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
    ui::Transform transform;
    transform.set({1.1, 2.2, 3.3, 4.4, 5.5, 6.6, 0, 0, 1});
    gui::DisplayInfo displayInfo;
    displayInfo.displayId = ADISPLAY_ID_DEFAULT;
    displayInfo.transform = transform;
    mDispatcher->onWindowInfosChanged({*window->getInfo()}, {displayInfo});
    NotifyMotionArgs motionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyMotion(&motionArgs);
    InputEvent* event = window->consume();
    ASSERT_NE(event, nullptr);
    std::unique_ptr<VerifiedInputEvent> verified = mDispatcher->verifyInputEvent(*event);
    ASSERT_NE(verified, nullptr);
    ASSERT_EQ(verified->type, VerifiedInputEvent::Type::MOTION);
    EXPECT_EQ(motionArgs.eventTime, verified->eventTimeNanos);
    EXPECT_EQ(motionArgs.deviceId, verified->deviceId);
    EXPECT_EQ(motionArgs.source, verified->source);
    EXPECT_EQ(motionArgs.displayId, verified->displayId);
    const VerifiedMotionEvent& verifiedMotion = static_cast<const VerifiedMotionEvent&>(*verified);
    const vec2 rawXY =
            MotionEvent::calculateTransformedXY(motionArgs.source, transform,
                                                motionArgs.pointerCoords[0].getXYValue());
    EXPECT_EQ(rawXY.x, verifiedMotion.rawX);
    EXPECT_EQ(rawXY.y, verifiedMotion.rawY);
    EXPECT_EQ(motionArgs.action & AMOTION_EVENT_ACTION_MASK, verifiedMotion.actionMasked);
    EXPECT_EQ(motionArgs.flags & VERIFIED_MOTION_EVENT_FLAGS, verifiedMotion.flags);
    EXPECT_EQ(motionArgs.downTime, verifiedMotion.downTimeNanos);
    EXPECT_EQ(motionArgs.metaState, verifiedMotion.metaState);
    EXPECT_EQ(motionArgs.buttonState, verifiedMotion.buttonState);
}
TEST_F(InputDispatcherTest, GeneratedHmac_IsConsistent) {
    KeyEvent event = getTestKeyEvent();
    VerifiedKeyEvent verifiedEvent = verifiedKeyEventFromKeyEvent(event);
    std::array<uint8_t, 32> hmac1 = mDispatcher->sign(verifiedEvent);
    std::array<uint8_t, 32> hmac2 = mDispatcher->sign(verifiedEvent);
    ASSERT_EQ(hmac1, hmac2);
}
TEST_F(InputDispatcherTest, GeneratedHmac_ChangesWhenFieldsChange) {
    KeyEvent event = getTestKeyEvent();
    VerifiedKeyEvent verifiedEvent = verifiedKeyEventFromKeyEvent(event);
    std::array<uint8_t, 32> initialHmac = mDispatcher->sign(verifiedEvent);
    verifiedEvent.deviceId += 1;
    ASSERT_NE(initialHmac, mDispatcher->sign(verifiedEvent));
    verifiedEvent.source += 1;
    ASSERT_NE(initialHmac, mDispatcher->sign(verifiedEvent));
    verifiedEvent.eventTimeNanos += 1;
    ASSERT_NE(initialHmac, mDispatcher->sign(verifiedEvent));
    verifiedEvent.displayId += 1;
    ASSERT_NE(initialHmac, mDispatcher->sign(verifiedEvent));
    verifiedEvent.action += 1;
    ASSERT_NE(initialHmac, mDispatcher->sign(verifiedEvent));
    verifiedEvent.downTimeNanos += 1;
    ASSERT_NE(initialHmac, mDispatcher->sign(verifiedEvent));
    verifiedEvent.flags += 1;
    ASSERT_NE(initialHmac, mDispatcher->sign(verifiedEvent));
    verifiedEvent.keyCode += 1;
    ASSERT_NE(initialHmac, mDispatcher->sign(verifiedEvent));
    verifiedEvent.scanCode += 1;
    ASSERT_NE(initialHmac, mDispatcher->sign(verifiedEvent));
    verifiedEvent.metaState += 1;
    ASSERT_NE(initialHmac, mDispatcher->sign(verifiedEvent));
    verifiedEvent.repeatCount += 1;
    ASSERT_NE(initialHmac, mDispatcher->sign(verifiedEvent));
}
TEST_F(InputDispatcherTest, SetFocusedWindow) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> windowTop =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Top", ADISPLAY_ID_DEFAULT);
    sp<FakeWindowHandle> windowSecond =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Second", ADISPLAY_ID_DEFAULT);
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
    windowTop->setFocusable(true);
    windowSecond->setFocusable(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {windowTop, windowSecond}}});
    setFocusedWindow(windowSecond);
    windowSecond->consumeFocusEvent(true);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyDown(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::SUCCEEDED";
    windowSecond->consumeKeyDown(ADISPLAY_ID_NONE);
    windowTop->assertNoEvents();
}
TEST_F(InputDispatcherTest, SetFocusedWindow_DropRequestInvalidChannel) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window =
            sp<FakeWindowHandle>::make(application, mDispatcher, "TestWindow", ADISPLAY_ID_DEFAULT);
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
    window->setFocusable(true);
    window->releaseChannel();
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    setFocusedWindow(window);
    ASSERT_EQ(InputEventInjectionResult::TIMED_OUT, injectKeyDown(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::TIMED_OUT";
    window->assertNoEvents();
}
TEST_F(InputDispatcherTest, SetFocusedWindow_DropRequestNoFocusableWindow) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window =
            sp<FakeWindowHandle>::make(application, mDispatcher, "TestWindow", ADISPLAY_ID_DEFAULT);
    window->setFocusable(false);
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    setFocusedWindow(window);
    ASSERT_EQ(InputEventInjectionResult::TIMED_OUT, injectKeyDown(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::TIMED_OUT";
    window->assertNoEvents();
}
TEST_F(InputDispatcherTest, SetFocusedWindow_CheckFocusedToken) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> windowTop =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Top", ADISPLAY_ID_DEFAULT);
    sp<FakeWindowHandle> windowSecond =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Second", ADISPLAY_ID_DEFAULT);
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
    windowTop->setFocusable(true);
    windowSecond->setFocusable(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {windowTop, windowSecond}}});
    setFocusedWindow(windowTop);
    windowTop->consumeFocusEvent(true);
    setFocusedWindow(windowSecond, windowTop);
    windowSecond->consumeFocusEvent(true);
    windowTop->consumeFocusEvent(false);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyDown(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::SUCCEEDED";
    windowSecond->consumeKeyDown(ADISPLAY_ID_NONE);
}
TEST_F(InputDispatcherTest, SetFocusedWindow_DropRequestFocusTokenNotFocused) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> windowTop =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Top", ADISPLAY_ID_DEFAULT);
    sp<FakeWindowHandle> windowSecond =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Second", ADISPLAY_ID_DEFAULT);
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
    windowTop->setFocusable(true);
    windowSecond->setFocusable(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {windowTop, windowSecond}}});
    setFocusedWindow(windowSecond, windowTop);
    ASSERT_EQ(InputEventInjectionResult::TIMED_OUT, injectKeyDown(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::TIMED_OUT";
    windowTop->assertNoEvents();
    windowSecond->assertNoEvents();
}
TEST_F(InputDispatcherTest, SetFocusedWindow_DeferInvisibleWindow) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window =
            sp<FakeWindowHandle>::make(application, mDispatcher, "TestWindow", ADISPLAY_ID_DEFAULT);
    sp<FakeWindowHandle> previousFocusedWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "previousFocusedWindow",
                                       ADISPLAY_ID_DEFAULT);
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
    window->setFocusable(true);
    previousFocusedWindow->setFocusable(true);
    window->setVisible(false);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window, previousFocusedWindow}}});
    setFocusedWindow(previousFocusedWindow);
    previousFocusedWindow->consumeFocusEvent(true);
    setFocusedWindow(window);
    previousFocusedWindow->consumeFocusEvent(false);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectKey(mDispatcher, AKEY_EVENT_ACTION_DOWN, 0 ,
                        ADISPLAY_ID_DEFAULT, InputEventInjectionSync::NONE));
    window->assertNoEvents();
    window->setVisible(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    window->consumeFocusEvent(true);
    window->consumeKeyDown(ADISPLAY_ID_DEFAULT);
}
TEST_F(InputDispatcherTest, DisplayRemoved) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window =
            sp<FakeWindowHandle>::make(application, mDispatcher, "window", ADISPLAY_ID_DEFAULT);
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
    window->setFocusable(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    setFocusedWindow(window);
    window->consumeFocusEvent(true);
    mDispatcher->displayRemoved(ADISPLAY_ID_DEFAULT);
    window->consumeFocusEvent(false);
}
TEST_F(InputDispatcherTest, SlipperyWindow_SetsFlagPartiallyObscured) {
    constexpr int32_t SLIPPERY_PID = WINDOW_PID + 1;
    constexpr int32_t SLIPPERY_UID = WINDOW_UID + 1;
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
    sp<FakeWindowHandle> slipperyExitWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Top", ADISPLAY_ID_DEFAULT);
    slipperyExitWindow->setSlippery(true);
    slipperyExitWindow->setFrame(Rect(25, 25, 75, 75));
    slipperyExitWindow->setOwnerInfo(SLIPPERY_PID, SLIPPERY_UID);
    sp<FakeWindowHandle> slipperyEnterWindow =
            sp<FakeWindowHandle>::make(application, mDispatcher, "Second", ADISPLAY_ID_DEFAULT);
    slipperyExitWindow->setFrame(Rect(0, 0, 100, 100));
    mDispatcher->setInputWindows(
            {{ADISPLAY_ID_DEFAULT, {slipperyExitWindow, slipperyEnterWindow}}});
    NotifyMotionArgs args = generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                                               ADISPLAY_ID_DEFAULT, {{50, 50}});
    mDispatcher->notifyMotion(&args);
    slipperyExitWindow->consumeMotionDown();
    slipperyExitWindow->setFrame(Rect(70, 70, 100, 100));
    mDispatcher->setInputWindows(
            {{ADISPLAY_ID_DEFAULT, {slipperyExitWindow, slipperyEnterWindow}}});
    args = generateMotionArgs(AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN,
                              ADISPLAY_ID_DEFAULT, {{51, 51}});
    mDispatcher->notifyMotion(&args);
    slipperyExitWindow->consumeMotionCancel();
    slipperyEnterWindow->consumeMotionDown(ADISPLAY_ID_DEFAULT,
                                           AMOTION_EVENT_FLAG_WINDOW_IS_PARTIALLY_OBSCURED);
}
class InputDispatcherKeyRepeatTest : public InputDispatcherTest {
protected:
    static constexpr nsecs_t KEY_REPEAT_TIMEOUT = 40 * 1000000;
    static constexpr nsecs_t KEY_REPEAT_DELAY = 40 * 1000000;
    std::shared_ptr<FakeApplicationHandle> mApp;
    sp<FakeWindowHandle> mWindow;
    virtual void SetUp() override {
        mFakePolicy = sp<FakeInputDispatcherPolicy>::make();
        mFakePolicy->setKeyRepeatConfiguration(KEY_REPEAT_TIMEOUT, KEY_REPEAT_DELAY);
        mDispatcher = std::make_unique<InputDispatcher>(mFakePolicy);
        mDispatcher->setInputDispatchMode( true, false);
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
        keyArgs.policyFlags |= POLICY_FLAG_TRUSTED;
        mDispatcher->notifyKey(&keyArgs);
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
        keyArgs.policyFlags |= POLICY_FLAG_TRUSTED;
        mDispatcher->notifyKey(&keyArgs);
        mWindow->consumeEvent(AINPUT_EVENT_TYPE_KEY, AKEY_EVENT_ACTION_UP, ADISPLAY_ID_DEFAULT,
                              0 );
    }
};
TEST_F(InputDispatcherKeyRepeatTest, FocusedWindow_ReceivesKeyRepeat) {
    sendAndConsumeKeyDown(1 );
    for (int32_t repeatCount = 1; repeatCount <= 10; ++repeatCount) {
        expectKeyRepeatOnce(repeatCount);
    }
}
TEST_F(InputDispatcherKeyRepeatTest, FocusedWindow_ReceivesKeyRepeatFromTwoDevices) {
    sendAndConsumeKeyDown(1 );
    for (int32_t repeatCount = 1; repeatCount <= 10; ++repeatCount) {
        expectKeyRepeatOnce(repeatCount);
    }
    sendAndConsumeKeyDown(2 );
    for (int32_t repeatCount = 1; repeatCount <= 10; ++repeatCount) {
        expectKeyRepeatOnce(repeatCount);
    }
}
TEST_F(InputDispatcherKeyRepeatTest, FocusedWindow_StopsKeyRepeatAfterUp) {
    sendAndConsumeKeyDown(1 );
    expectKeyRepeatOnce(1 );
    sendAndConsumeKeyUp(1 );
    mWindow->assertNoEvents();
}
TEST_F(InputDispatcherKeyRepeatTest, FocusedWindow_KeyRepeatAfterStaleDeviceKeyUp) {
    sendAndConsumeKeyDown(1 );
    expectKeyRepeatOnce(1 );
    sendAndConsumeKeyDown(2 );
    expectKeyRepeatOnce(1 );
    sendAndConsumeKeyUp(1 );
    expectKeyRepeatOnce(2 );
    expectKeyRepeatOnce(3 );
    sendAndConsumeKeyUp(2 );
    mWindow->assertNoEvents();
}
TEST_F(InputDispatcherKeyRepeatTest, FocusedWindow_KeyRepeatStopsAfterRepeatingKeyUp) {
    sendAndConsumeKeyDown(1 );
    expectKeyRepeatOnce(1 );
    sendAndConsumeKeyDown(2 );
    expectKeyRepeatOnce(1 );
    sendAndConsumeKeyUp(2 );
    mWindow->assertNoEvents();
}
TEST_F(InputDispatcherKeyRepeatTest, FocusedWindow_StopsKeyRepeatAfterDisableInputDevice) {
    sendAndConsumeKeyDown(DEVICE_ID);
    expectKeyRepeatOnce(1 );
    NotifyDeviceResetArgs args(10 , 20 , DEVICE_ID);
    mDispatcher->notifyDeviceReset(&args);
    mWindow->consumeKeyUp(ADISPLAY_ID_DEFAULT,
                          AKEY_EVENT_FLAG_CANCELED | AKEY_EVENT_FLAG_LONG_PRESS);
    mWindow->assertNoEvents();
}
TEST_F(InputDispatcherKeyRepeatTest, FocusedWindow_RepeatKeyEventsUseEventIdFromInputDispatcher) {
    sendAndConsumeKeyDown(1 );
    for (int32_t repeatCount = 1; repeatCount <= 10; ++repeatCount) {
        InputEvent* repeatEvent = mWindow->consume();
        ASSERT_NE(nullptr, repeatEvent) << "Didn't receive event with repeat count " << repeatCount;
        EXPECT_EQ(IdGenerator::Source::INPUT_DISPATCHER,
                  IdGenerator::getSource(repeatEvent->getId()));
    }
}
TEST_F(InputDispatcherKeyRepeatTest, FocusedWindow_RepeatKeyEventsUseUniqueEventId) {
    sendAndConsumeKeyDown(1 );
    std::unordered_set<int32_t> idSet;
    for (int32_t repeatCount = 1; repeatCount <= 10; ++repeatCount) {
        InputEvent* repeatEvent = mWindow->consume();
        ASSERT_NE(nullptr, repeatEvent) << "Didn't receive event with repeat count " << repeatCount;
        int32_t id = repeatEvent->getId();
        EXPECT_EQ(idSet.end(), idSet.find(id));
        idSet.insert(id);
    }
}
class InputDispatcherFocusOnTwoDisplaysTest : public InputDispatcherTest {
public:
    virtual void SetUp() override {
        InputDispatcherTest::SetUp();
        application1 = std::make_shared<FakeApplicationHandle>();
        windowInPrimary =
                sp<FakeWindowHandle>::make(application1, mDispatcher, "D_1", ADISPLAY_ID_DEFAULT);
        mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application1);
        windowInPrimary->setFocusable(true);
        mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {windowInPrimary}}});
        setFocusedWindow(windowInPrimary);
        windowInPrimary->consumeFocusEvent(true);
        application2 = std::make_shared<FakeApplicationHandle>();
        windowInSecondary =
                sp<FakeWindowHandle>::make(application2, mDispatcher, "D_2", SECOND_DISPLAY_ID);
        mDispatcher->setFocusedDisplay(SECOND_DISPLAY_ID);
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
TEST_F(InputDispatcherFocusOnTwoDisplaysTest, SetInputWindow_MultiDisplayTouch) {
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    windowInPrimary->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    windowInSecondary->assertNoEvents();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, SECOND_DISPLAY_ID))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    windowInPrimary->assertNoEvents();
    windowInSecondary->consumeMotionDown(SECOND_DISPLAY_ID);
}
TEST_F(InputDispatcherFocusOnTwoDisplaysTest, SetInputWindow_MultiDisplayFocus) {
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectKeyDownNoRepeat(mDispatcher, ADISPLAY_ID_DEFAULT))
            << "Inject key event should return InputEventInjectionResult::SUCCEEDED";
    windowInPrimary->consumeKeyDown(ADISPLAY_ID_DEFAULT);
    windowInSecondary->assertNoEvents();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyDownNoRepeat(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::SUCCEEDED";
    windowInPrimary->assertNoEvents();
    windowInSecondary->consumeKeyDown(ADISPLAY_ID_NONE);
    mDispatcher->setInputWindows({{SECOND_DISPLAY_ID, {}}});
    windowInSecondary->consumeEvent(AINPUT_EVENT_TYPE_KEY, AKEY_EVENT_ACTION_UP, ADISPLAY_ID_NONE,
                                    AKEY_EVENT_FLAG_CANCELED);
    ASSERT_EQ(InputEventInjectionResult::TIMED_OUT, injectKeyDownNoRepeat(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::TIMED_OUT";
    windowInPrimary->assertNoEvents();
    windowInSecondary->consumeFocusEvent(false);
    windowInSecondary->assertNoEvents();
}
TEST_F(InputDispatcherFocusOnTwoDisplaysTest, MonitorMotionEvent_MultiDisplay) {
    FakeMonitorReceiver monitorInPrimary =
            FakeMonitorReceiver(mDispatcher, "M_1", ADISPLAY_ID_DEFAULT);
    FakeMonitorReceiver monitorInSecondary =
            FakeMonitorReceiver(mDispatcher, "M_2", SECOND_DISPLAY_ID);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    windowInPrimary->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    monitorInPrimary.consumeMotionDown(ADISPLAY_ID_DEFAULT);
    windowInSecondary->assertNoEvents();
    monitorInSecondary.assertNoEvents();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, SECOND_DISPLAY_ID))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    windowInPrimary->assertNoEvents();
    monitorInPrimary.assertNoEvents();
    windowInSecondary->consumeMotionDown(SECOND_DISPLAY_ID);
    monitorInSecondary.consumeMotionDown(SECOND_DISPLAY_ID);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TRACKBALL, ADISPLAY_ID_NONE))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    windowInPrimary->assertNoEvents();
    monitorInPrimary.assertNoEvents();
    windowInSecondary->consumeMotionDown(ADISPLAY_ID_NONE);
    monitorInSecondary.consumeMotionDown(ADISPLAY_ID_NONE);
}
TEST_F(InputDispatcherFocusOnTwoDisplaysTest, MonitorKeyEvent_MultiDisplay) {
    FakeMonitorReceiver monitorInPrimary =
            FakeMonitorReceiver(mDispatcher, "M_1", ADISPLAY_ID_DEFAULT);
    FakeMonitorReceiver monitorInSecondary =
            FakeMonitorReceiver(mDispatcher, "M_2", SECOND_DISPLAY_ID);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyDown(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::SUCCEEDED";
    windowInPrimary->assertNoEvents();
    monitorInPrimary.assertNoEvents();
    windowInSecondary->consumeKeyDown(ADISPLAY_ID_NONE);
    monitorInSecondary.consumeKeyDown(ADISPLAY_ID_NONE);
}
TEST_F(InputDispatcherFocusOnTwoDisplaysTest, CanFocusWindowOnUnfocusedDisplay) {
    sp<FakeWindowHandle> secondWindowInPrimary =
            sp<FakeWindowHandle>::make(application1, mDispatcher, "D_1_W2", ADISPLAY_ID_DEFAULT);
    secondWindowInPrimary->setFocusable(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {windowInPrimary, secondWindowInPrimary}}});
    setFocusedWindow(secondWindowInPrimary);
    windowInPrimary->consumeFocusEvent(false);
    secondWindowInPrimary->consumeFocusEvent(true);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyDown(mDispatcher, ADISPLAY_ID_DEFAULT))
            << "Inject key event should return InputEventInjectionResult::SUCCEEDED";
    windowInPrimary->assertNoEvents();
    windowInSecondary->assertNoEvents();
    secondWindowInPrimary->consumeKeyDown(ADISPLAY_ID_DEFAULT);
}
TEST_F(InputDispatcherFocusOnTwoDisplaysTest, CancelTouch_MultiDisplay) {
    FakeMonitorReceiver monitorInPrimary =
            FakeMonitorReceiver(mDispatcher, "M_1", ADISPLAY_ID_DEFAULT);
    FakeMonitorReceiver monitorInSecondary =
            FakeMonitorReceiver(mDispatcher, "M_2", SECOND_DISPLAY_ID);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    windowInPrimary->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    monitorInPrimary.consumeMotionDown(ADISPLAY_ID_DEFAULT);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, SECOND_DISPLAY_ID))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    windowInSecondary->consumeMotionDown(SECOND_DISPLAY_ID);
    monitorInSecondary.consumeMotionDown(SECOND_DISPLAY_ID);
    mDispatcher->cancelCurrentTouch();
    windowInPrimary->consumeMotionCancel(ADISPLAY_ID_DEFAULT);
    monitorInPrimary.consumeMotionCancel(ADISPLAY_ID_DEFAULT);
    windowInSecondary->consumeMotionCancel(SECOND_DISPLAY_ID);
    monitorInSecondary.consumeMotionCancel(SECOND_DISPLAY_ID);
    ASSERT_EQ(InputEventInjectionResult::FAILED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN,
                                ADISPLAY_ID_DEFAULT, {110, 200}))
            << "Inject motion event should return InputEventInjectionResult::FAILED";
    windowInPrimary->assertNoEvents();
    monitorInPrimary.assertNoEvents();
    ASSERT_EQ(InputEventInjectionResult::FAILED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN,
                                SECOND_DISPLAY_ID, {110, 200}))
            << "Inject motion event should return InputEventInjectionResult::FAILED";
    windowInSecondary->assertNoEvents();
    monitorInSecondary.assertNoEvents();
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
TEST_F(InputFilterTest, MotionEvent_InputFilter) {
    testNotifyMotion(ADISPLAY_ID_DEFAULT, false);
    testNotifyMotion(SECOND_DISPLAY_ID, false);
    mDispatcher->setInputFilterEnabled(true);
    testNotifyMotion(ADISPLAY_ID_DEFAULT, true);
    testNotifyMotion(SECOND_DISPLAY_ID, true);
    mDispatcher->setInputFilterEnabled(false);
    testNotifyMotion(ADISPLAY_ID_DEFAULT, false);
    testNotifyMotion(SECOND_DISPLAY_ID, false);
}
TEST_F(InputFilterTest, KeyEvent_InputFilter) {
    testNotifyKey( false);
    mDispatcher->setInputFilterEnabled(true);
    testNotifyKey( true);
    mDispatcher->setInputFilterEnabled(false);
    testNotifyKey( false);
}
TEST_F(InputFilterTest, MotionEvent_UsesLogicalDisplayCoordinates_notifyMotion) {
    ui::Transform firstDisplayTransform;
    firstDisplayTransform.set({1.1, 2.2, 3.3, 4.4, 5.5, 6.6, 0, 0, 1});
    ui::Transform secondDisplayTransform;
    secondDisplayTransform.set({-6.6, -5.5, -4.4, -3.3, -2.2, -1.1, 0, 0, 1});
    std::vector<gui::DisplayInfo> displayInfos(2);
    displayInfos[0].displayId = ADISPLAY_ID_DEFAULT;
    displayInfos[0].transform = firstDisplayTransform;
    displayInfos[1].displayId = SECOND_DISPLAY_ID;
    displayInfos[1].transform = secondDisplayTransform;
    mDispatcher->onWindowInfosChanged({}, displayInfos);
    mDispatcher->setInputFilterEnabled(true);
    testNotifyMotion(ADISPLAY_ID_DEFAULT, true, firstDisplayTransform);
    testNotifyMotion(SECOND_DISPLAY_ID, true, secondDisplayTransform);
}
class InputFilterInjectionPolicyTest : public InputDispatcherTest {
protected:
    virtual void SetUp() override {
        InputDispatcherTest::SetUp();
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
                         KEY_A, AMETA_NONE, 0 , eventTime, eventTime);
        const int32_t additionalPolicyFlags =
                POLICY_FLAG_PASS_TO_USER | POLICY_FLAG_DISABLE_KEY_REPEAT;
        ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
                  mDispatcher->injectInputEvent(&event, {} ,
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
                                          1, pointerProperties, pointerCoords);
        const int32_t additionalPolicyFlags = POLICY_FLAG_PASS_TO_USER;
        ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
                  mDispatcher->injectInputEvent(&event, {} ,
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
TEST_F(InputFilterInjectionPolicyTest, TrustedFilteredEvents_KeepOriginalDeviceId) {
    testInjectedKey(POLICY_FLAG_FILTERED, 3 , 3 ,
                    0 );
}
TEST_F(InputFilterInjectionPolicyTest, KeyEventsInjectedFromAccessibility_HaveAccessibilityFlag) {
    testInjectedKey(POLICY_FLAG_FILTERED | POLICY_FLAG_INJECTED_FROM_ACCESSIBILITY,
                    3 , 3 ,
                    AKEY_EVENT_FLAG_IS_ACCESSIBILITY_EVENT);
}
TEST_F(InputFilterInjectionPolicyTest,
       MotionEventsInjectedFromAccessibility_HaveAccessibilityFlag) {
    testInjectedMotion(POLICY_FLAG_FILTERED | POLICY_FLAG_INJECTED_FROM_ACCESSIBILITY,
                       3 , 3 ,
                       AMOTION_EVENT_FLAG_IS_ACCESSIBILITY_EVENT);
}
TEST_F(InputFilterInjectionPolicyTest, RegularInjectedEvents_ReceiveVirtualDeviceId) {
    testInjectedKey(0 , 3 ,
                    VIRTUAL_KEYBOARD_ID , 0 );
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
        mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
        mFocusedWindow->setFocusable(true);
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
TEST_F(InputDispatcherOnPointerDownOutsideFocus, OnPointerDownOutsideFocus_Success) {
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {20, 20}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mUnfocusedWindow->consumeMotionDown();
    ASSERT_TRUE(mDispatcher->waitForIdle());
    mFakePolicy->assertOnPointerDownEquals(mUnfocusedWindow->getToken());
}
TEST_F(InputDispatcherOnPointerDownOutsideFocus, OnPointerDownOutsideFocus_NonPointerSource) {
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TRACKBALL, ADISPLAY_ID_DEFAULT, {20, 20}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mFocusedWindow->consumeMotionDown();
    ASSERT_TRUE(mDispatcher->waitForIdle());
    mFakePolicy->assertOnPointerDownWasNotCalled();
}
TEST_F(InputDispatcherOnPointerDownOutsideFocus, OnPointerDownOutsideFocus_NonMotionFailure) {
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectKeyDownNoRepeat(mDispatcher, ADISPLAY_ID_DEFAULT))
            << "Inject key event should return InputEventInjectionResult::SUCCEEDED";
    mFocusedWindow->consumeKeyDown(ADISPLAY_ID_DEFAULT);
    ASSERT_TRUE(mDispatcher->waitForIdle());
    mFakePolicy->assertOnPointerDownWasNotCalled();
}
TEST_F(InputDispatcherOnPointerDownOutsideFocus, OnPointerDownOutsideFocus_OnAlreadyFocusedWindow) {
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               FOCUSED_WINDOW_TOUCH_POINT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mFocusedWindow->consumeMotionDown();
    ASSERT_TRUE(mDispatcher->waitForIdle());
    mFakePolicy->assertOnPointerDownWasNotCalled();
}
TEST_F(InputDispatcherOnPointerDownOutsideFocus, NoFocusChangeFlag) {
    const MotionEvent event =
            MotionEventBuilder(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_MOUSE)
                    .eventTime(systemTime(SYSTEM_TIME_MONOTONIC))
                    .pointer(PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER).x(20).y(20))
                    .addFlag(AMOTION_EVENT_FLAG_NO_FOCUS_CHANGE)
                    .build();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectMotionEvent(mDispatcher, event))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mUnfocusedWindow->consumeAnyMotionDown(ADISPLAY_ID_DEFAULT, AMOTION_EVENT_FLAG_NO_FOCUS_CHANGE);
    ASSERT_TRUE(mDispatcher->waitForIdle());
    mFakePolicy->assertOnPointerDownWasNotCalled();
    mUnfocusedWindow->assertNoEvents();
}
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
        consumeMotionEvent(mWindow1, action, expectedPoints);
    }
};
TEST_F(InputDispatcherMultiWindowSameTokenTests, SingleTouchSameScale) {
    PointF touchedPoint = {10, 10};
    PointF expectedPoint = getPointInWindow(mWindow1->getInfo(), touchedPoint);
    touchAndAssertPositions(AMOTION_EVENT_ACTION_DOWN, {touchedPoint}, {expectedPoint});
    touchAndAssertPositions(AMOTION_EVENT_ACTION_UP, {touchedPoint}, {expectedPoint});
    touchedPoint = {150, 150};
    expectedPoint = getPointInWindow(mWindow2->getInfo(), touchedPoint);
    touchAndAssertPositions(AMOTION_EVENT_ACTION_DOWN, {touchedPoint}, {expectedPoint});
}
TEST_F(InputDispatcherMultiWindowSameTokenTests, SingleTouchDifferentTransform) {
    mWindow2->setWindowScale(0.5f, 0.5f);
    PointF touchedPoint = {10, 10};
    PointF expectedPoint = getPointInWindow(mWindow1->getInfo(), touchedPoint);
    touchAndAssertPositions(AMOTION_EVENT_ACTION_DOWN, {touchedPoint}, {expectedPoint});
    touchAndAssertPositions(AMOTION_EVENT_ACTION_UP, {touchedPoint}, {expectedPoint});
    touchedPoint = {150, 150};
    expectedPoint = getPointInWindow(mWindow2->getInfo(), touchedPoint);
    touchAndAssertPositions(AMOTION_EVENT_ACTION_DOWN, {touchedPoint}, {expectedPoint});
    touchAndAssertPositions(AMOTION_EVENT_ACTION_UP, {touchedPoint}, {expectedPoint});
    mWindow2->setWindowTransform(0, -1, 1, 0);
    expectedPoint = getPointInWindow(mWindow2->getInfo(), touchedPoint);
    touchAndAssertPositions(AMOTION_EVENT_ACTION_DOWN, {touchedPoint}, {expectedPoint});
}
TEST_F(InputDispatcherMultiWindowSameTokenTests, MultipleTouchDifferentTransform) {
    mWindow2->setWindowScale(0.5f, 0.5f);
    std::vector<PointF> touchedPoints = {PointF{10, 10}};
    std::vector<PointF> expectedPoints = {getPointInWindow(mWindow1->getInfo(), touchedPoints[0])};
    touchAndAssertPositions(AMOTION_EVENT_ACTION_DOWN, touchedPoints, expectedPoints);
    touchedPoints.push_back(PointF{150, 150});
    expectedPoints.push_back(getPointInWindow(mWindow2->getInfo(), touchedPoints[1]));
    touchAndAssertPositions(POINTER_1_DOWN, touchedPoints, expectedPoints);
    touchAndAssertPositions(POINTER_1_UP, touchedPoints, expectedPoints);
    expectedPoints.pop_back();
    mWindow2->setWindowTransform(0, -1, 1, 0);
    expectedPoints.push_back(getPointInWindow(mWindow2->getInfo(), touchedPoints[1]));
    touchAndAssertPositions(POINTER_1_DOWN, touchedPoints, expectedPoints);
}
TEST_F(InputDispatcherMultiWindowSameTokenTests, MultipleTouchMoveDifferentTransform) {
    mWindow2->setWindowScale(0.5f, 0.5f);
    std::vector<PointF> touchedPoints = {PointF{10, 10}};
    std::vector<PointF> expectedPoints = {getPointInWindow(mWindow1->getInfo(), touchedPoints[0])};
    touchAndAssertPositions(AMOTION_EVENT_ACTION_DOWN, touchedPoints, expectedPoints);
    touchedPoints.push_back(PointF{150, 150});
    expectedPoints.push_back(getPointInWindow(mWindow2->getInfo(), touchedPoints[1]));
    touchAndAssertPositions(POINTER_1_DOWN, touchedPoints, expectedPoints);
    touchedPoints = {{20, 20}, {175, 175}};
    expectedPoints = {getPointInWindow(mWindow1->getInfo(), touchedPoints[0]),
                      getPointInWindow(mWindow2->getInfo(), touchedPoints[1])};
    touchAndAssertPositions(AMOTION_EVENT_ACTION_MOVE, touchedPoints, expectedPoints);
    touchAndAssertPositions(POINTER_1_UP, touchedPoints, expectedPoints);
    expectedPoints.pop_back();
    mWindow2->setWindowTransform(0, -1, 1, 0);
    expectedPoints.push_back(getPointInWindow(mWindow2->getInfo(), touchedPoints[1]));
    touchAndAssertPositions(POINTER_1_DOWN, touchedPoints, expectedPoints);
    touchedPoints = {{20, 20}, {175, 175}};
    expectedPoints = {getPointInWindow(mWindow1->getInfo(), touchedPoints[0]),
                      getPointInWindow(mWindow2->getInfo(), touchedPoints[1])};
    touchAndAssertPositions(AMOTION_EVENT_ACTION_MOVE, touchedPoints, expectedPoints);
}
TEST_F(InputDispatcherMultiWindowSameTokenTests, MultipleWindowsFirstTouchWithScale) {
    mWindow1->setWindowScale(0.5f, 0.5f);
    std::vector<PointF> touchedPoints = {PointF{10, 10}};
    std::vector<PointF> expectedPoints = {getPointInWindow(mWindow1->getInfo(), touchedPoints[0])};
    touchAndAssertPositions(AMOTION_EVENT_ACTION_DOWN, touchedPoints, expectedPoints);
    touchedPoints.push_back(PointF{150, 150});
    expectedPoints.push_back(getPointInWindow(mWindow2->getInfo(), touchedPoints[1]));
    touchAndAssertPositions(POINTER_1_DOWN, touchedPoints, expectedPoints);
    touchedPoints = {{20, 20}, {175, 175}};
    expectedPoints = {getPointInWindow(mWindow1->getInfo(), touchedPoints[0]),
                      getPointInWindow(mWindow2->getInfo(), touchedPoints[1])};
    touchAndAssertPositions(AMOTION_EVENT_ACTION_MOVE, touchedPoints, expectedPoints);
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
TEST_F(InputDispatcherSingleWindowAnr, WhenTouchIsConsumed_NoAnr) {
    tapOnWindow();
    mWindow->consumeMotionDown();
    mWindow->consumeMotionUp();
    ASSERT_TRUE(mDispatcher->waitForIdle());
    mFakePolicy->assertNotifyAnrWasNotCalled();
}
TEST_F(InputDispatcherSingleWindowAnr, WhenKeyIsConsumed_NoAnr) {
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyDownNoRepeat(mDispatcher));
    mWindow->consumeKeyDown(ADISPLAY_ID_NONE);
    ASSERT_TRUE(mDispatcher->waitForIdle());
    mFakePolicy->assertNotifyAnrWasNotCalled();
}
TEST_F(InputDispatcherSingleWindowAnr, WhenFocusedApplicationChanges_NoAnr) {
    mWindow->setFocusable(false);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mWindow}}});
    mWindow->consumeFocusEvent(false);
    InputEventInjectionResult result =
            injectKey(mDispatcher, AKEY_EVENT_ACTION_DOWN, 0 , ADISPLAY_ID_DEFAULT,
                      InputEventInjectionSync::NONE, 10ms ,
                      false );
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, result);
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, nullptr);
    ASSERT_TRUE(mDispatcher->waitForIdle());
    mFakePolicy->assertNotifyAnrWasNotCalled();
}
TEST_F(InputDispatcherSingleWindowAnr, OnPointerDown_BasicAnr) {
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               WINDOW_LOCATION));
    std::optional<uint32_t> sequenceNum = mWindow->receiveEvent();
    ASSERT_TRUE(sequenceNum);
    const std::chrono::duration timeout = mWindow->getDispatchingTimeout(DISPATCHING_TIMEOUT);
    mFakePolicy->assertNotifyWindowUnresponsiveWasCalled(timeout, mWindow);
    mWindow->finishEvent(*sequenceNum);
    mWindow->consumeEvent(AINPUT_EVENT_TYPE_MOTION, AMOTION_EVENT_ACTION_CANCEL,
                          ADISPLAY_ID_DEFAULT, 0 );
    ASSERT_TRUE(mDispatcher->waitForIdle());
    mFakePolicy->assertNotifyWindowResponsiveWasCalled(mWindow->getToken(), mWindow->getPid());
}
TEST_F(InputDispatcherSingleWindowAnr, OnKeyDown_BasicAnr) {
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyDownNoRepeat(mDispatcher));
    std::optional<uint32_t> sequenceNum = mWindow->receiveEvent();
    ASSERT_TRUE(sequenceNum);
    const std::chrono::duration timeout = mWindow->getDispatchingTimeout(DISPATCHING_TIMEOUT);
    mFakePolicy->assertNotifyWindowUnresponsiveWasCalled(timeout, mWindow);
    ASSERT_TRUE(mDispatcher->waitForIdle());
}
TEST_F(InputDispatcherSingleWindowAnr, FocusedApplication_NoFocusedWindow) {
    mWindow->setFocusable(false);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mWindow}}});
    mWindow->consumeFocusEvent(false);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               WINDOW_LOCATION));
    ASSERT_NO_FATAL_FAILURE(mWindow->consumeMotionDown());
    mDispatcher->waitForIdle();
    mFakePolicy->assertNotifyAnrWasNotCalled();
    const InputEventInjectionResult result =
            injectKey(mDispatcher, AKEY_EVENT_ACTION_DOWN, 0 , ADISPLAY_ID_DEFAULT,
                      InputEventInjectionSync::WAIT_FOR_RESULT, 10ms, false );
    ASSERT_EQ(InputEventInjectionResult::TIMED_OUT, result);
    const std::chrono::duration timeout = mApplication->getDispatchingTimeout(DISPATCHING_TIMEOUT);
    mFakePolicy->assertNotifyNoFocusedWindowAnrWasCalled(timeout, mApplication);
    ASSERT_TRUE(mDispatcher->waitForIdle());
}
TEST_F(InputDispatcherSingleWindowAnr, StaleKeyEventDoesNotAnr) {
    mWindow->setFocusable(false);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mWindow}}});
    mWindow->consumeFocusEvent(false);
    KeyEvent event;
    const nsecs_t eventTime = systemTime(SYSTEM_TIME_MONOTONIC) -
            std::chrono::nanoseconds(STALE_EVENT_TIMEOUT).count();
    event.initialize(InputEvent::nextId(), DEVICE_ID, AINPUT_SOURCE_KEYBOARD, ADISPLAY_ID_NONE,
                     INVALID_HMAC, AKEY_EVENT_ACTION_DOWN, 0, AKEYCODE_A, KEY_A,
                     AMETA_NONE, 1 , eventTime, eventTime);
    const int32_t policyFlags = POLICY_FLAG_FILTERED | POLICY_FLAG_PASS_TO_USER;
    InputEventInjectionResult result =
            mDispatcher->injectInputEvent(&event, {} ,
                                          InputEventInjectionSync::WAIT_FOR_RESULT,
                                          INJECT_EVENT_TIMEOUT, policyFlags);
    ASSERT_EQ(InputEventInjectionResult::FAILED, result)
            << "Injection should fail because the event is stale";
    ASSERT_TRUE(mDispatcher->waitForIdle());
    mFakePolicy->assertNotifyAnrWasNotCalled();
    mWindow->assertNoEvents();
}
TEST_F(InputDispatcherSingleWindowAnr, NoFocusedWindow_DoesNotSendDuplicateAnr) {
    mWindow->setFocusable(false);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mWindow}}});
    mWindow->consumeFocusEvent(false);
    const InputEventInjectionResult result =
            injectKey(mDispatcher, AKEY_EVENT_ACTION_DOWN, 0 , ADISPLAY_ID_DEFAULT,
                      InputEventInjectionSync::WAIT_FOR_RESULT, 10ms, false );
    ASSERT_EQ(InputEventInjectionResult::TIMED_OUT, result);
    const std::chrono::duration appTimeout =
            mApplication->getDispatchingTimeout(DISPATCHING_TIMEOUT);
    mFakePolicy->assertNotifyNoFocusedWindowAnrWasCalled(appTimeout, mApplication);
    std::this_thread::sleep_for(appTimeout);
    mFakePolicy->assertNotifyAnrWasNotCalled();
    ASSERT_TRUE(mDispatcher->waitForIdle());
}
TEST_F(InputDispatcherSingleWindowAnr, NoFocusedWindow_DropsFocusedEvents) {
    mWindow->setFocusable(false);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mWindow}}});
    mWindow->consumeFocusEvent(false);
    const InputEventInjectionResult result =
            injectKey(mDispatcher, AKEY_EVENT_ACTION_DOWN, 0 , ADISPLAY_ID_DEFAULT,
                      InputEventInjectionSync::WAIT_FOR_RESULT, 10ms);
    ASSERT_EQ(InputEventInjectionResult::TIMED_OUT, result);
    const std::chrono::duration timeout = mApplication->getDispatchingTimeout(DISPATCHING_TIMEOUT);
    mFakePolicy->assertNotifyNoFocusedWindowAnrWasCalled(timeout, mApplication);
    ASSERT_EQ(InputEventInjectionResult::FAILED, injectKeyDown(mDispatcher));
    ASSERT_TRUE(mDispatcher->waitForIdle());
    mWindow->assertNoEvents();
}
TEST_F(InputDispatcherSingleWindowAnr, Anr_HandlesEventsWithIdenticalTimestamps) {
    nsecs_t currentTime = systemTime(SYSTEM_TIME_MONOTONIC);
    injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                      ADISPLAY_ID_DEFAULT, WINDOW_LOCATION,
                      {AMOTION_EVENT_INVALID_CURSOR_POSITION,
                       AMOTION_EVENT_INVALID_CURSOR_POSITION},
                      500ms, InputEventInjectionSync::WAIT_FOR_RESULT, currentTime);
    injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_UP, AINPUT_SOURCE_TOUCHSCREEN,
                      ADISPLAY_ID_DEFAULT, WINDOW_LOCATION,
                      {AMOTION_EVENT_INVALID_CURSOR_POSITION,
                       AMOTION_EVENT_INVALID_CURSOR_POSITION},
                      500ms, InputEventInjectionSync::WAIT_FOR_RESULT, currentTime);
    mWindow->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    const std::chrono::duration timeout = mWindow->getDispatchingTimeout(DISPATCHING_TIMEOUT);
    mFakePolicy->assertNotifyWindowUnresponsiveWasCalled(timeout, mWindow);
}
TEST_F(InputDispatcherSingleWindowAnr, SpyWindowAnr) {
    sp<FakeWindowHandle> spy = addSpyWindow();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               WINDOW_LOCATION));
    mWindow->consumeMotionDown();
    std::optional<uint32_t> sequenceNum = spy->receiveEvent();
    ASSERT_TRUE(sequenceNum);
    const std::chrono::duration timeout = spy->getDispatchingTimeout(DISPATCHING_TIMEOUT);
    mFakePolicy->assertNotifyWindowUnresponsiveWasCalled(timeout, spy);
    spy->finishEvent(*sequenceNum);
    spy->consumeEvent(AINPUT_EVENT_TYPE_MOTION, AMOTION_EVENT_ACTION_CANCEL, ADISPLAY_ID_DEFAULT,
                      0 );
    ASSERT_TRUE(mDispatcher->waitForIdle());
    mFakePolicy->assertNotifyWindowResponsiveWasCalled(spy->getToken(), mWindow->getPid());
}
TEST_F(InputDispatcherSingleWindowAnr, SpyWindowReceivesEventsDuringAppAnrOnKey) {
    sp<FakeWindowHandle> spy = addSpyWindow();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectKeyDown(mDispatcher, ADISPLAY_ID_DEFAULT));
    mWindow->consumeKeyDown(ADISPLAY_ID_DEFAULT);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyUp(mDispatcher, ADISPLAY_ID_DEFAULT));
    const std::chrono::duration timeout = mWindow->getDispatchingTimeout(DISPATCHING_TIMEOUT);
    mFakePolicy->assertNotifyWindowUnresponsiveWasCalled(timeout, mWindow);
    tapOnWindow();
    spy->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    spy->consumeMotionUp(ADISPLAY_ID_DEFAULT);
    mWindow->consumeKeyUp(ADISPLAY_ID_DEFAULT);
    mDispatcher->waitForIdle();
    mFakePolicy->assertNotifyWindowResponsiveWasCalled(mWindow->getToken(), mWindow->getPid());
    mWindow->assertNoEvents();
    spy->assertNoEvents();
}
TEST_F(InputDispatcherSingleWindowAnr, SpyWindowReceivesEventsDuringAppAnrOnMotion) {
    sp<FakeWindowHandle> spy = addSpyWindow();
    tapOnWindow();
    spy->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    spy->consumeMotionUp(ADISPLAY_ID_DEFAULT);
    mWindow->consumeMotionDown();
    const std::chrono::duration timeout = mWindow->getDispatchingTimeout(DISPATCHING_TIMEOUT);
    mFakePolicy->assertNotifyWindowUnresponsiveWasCalled(timeout, mWindow);
    tapOnWindow();
    spy->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    spy->consumeMotionUp(ADISPLAY_ID_DEFAULT);
    mWindow->consumeMotionUp(ADISPLAY_ID_DEFAULT);
    mDispatcher->waitForIdle();
    mFakePolicy->assertNotifyWindowResponsiveWasCalled(mWindow->getToken(), mWindow->getPid());
    mWindow->assertNoEvents();
    spy->assertNoEvents();
}
TEST_F(InputDispatcherSingleWindowAnr, UnresponsiveMonitorAnr) {
    mDispatcher->setMonitorDispatchingTimeoutForTest(30ms);
    FakeMonitorReceiver monitor = FakeMonitorReceiver(mDispatcher, "M_1", ADISPLAY_ID_DEFAULT);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               WINDOW_LOCATION));
    mWindow->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    const std::optional<uint32_t> consumeSeq = monitor.receiveEvent();
    ASSERT_TRUE(consumeSeq);
    mFakePolicy->assertNotifyWindowUnresponsiveWasCalled(30ms, monitor.getToken(), MONITOR_PID);
    monitor.finishEvent(*consumeSeq);
    monitor.consumeMotionCancel(ADISPLAY_ID_DEFAULT);
    ASSERT_TRUE(mDispatcher->waitForIdle());
    mFakePolicy->assertNotifyWindowResponsiveWasCalled(monitor.getToken(), MONITOR_PID);
}
TEST_F(InputDispatcherSingleWindowAnr, SameWindow_CanReceiveAnrTwice) {
    tapOnWindow();
    mWindow->consumeMotionDown();
    const std::chrono::duration timeout = mWindow->getDispatchingTimeout(DISPATCHING_TIMEOUT);
    mFakePolicy->assertNotifyWindowUnresponsiveWasCalled(timeout, mWindow);
    mWindow->consumeMotionUp();
    mDispatcher->waitForIdle();
    mFakePolicy->assertNotifyWindowResponsiveWasCalled(mWindow->getToken(), mWindow->getPid());
    mWindow->assertNoEvents();
    tapOnWindow();
    mWindow->consumeMotionDown();
    mFakePolicy->assertNotifyWindowUnresponsiveWasCalled(timeout, mWindow);
    mWindow->consumeMotionUp();
    mDispatcher->waitForIdle();
    mFakePolicy->assertNotifyWindowResponsiveWasCalled(mWindow->getToken(), mWindow->getPid());
    mFakePolicy->assertNotifyAnrWasNotCalled();
    mWindow->assertNoEvents();
}
TEST_F(InputDispatcherSingleWindowAnr, Policy_DoesNotGetDuplicateAnr) {
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               WINDOW_LOCATION));
    const std::chrono::duration windowTimeout = mWindow->getDispatchingTimeout(DISPATCHING_TIMEOUT);
    mFakePolicy->assertNotifyWindowUnresponsiveWasCalled(windowTimeout, mWindow);
    std::this_thread::sleep_for(windowTimeout);
    mFakePolicy->assertNotifyAnrWasNotCalled();
    mWindow->consumeMotionDown();
    mWindow->consumeEvent(AINPUT_EVENT_TYPE_MOTION, AMOTION_EVENT_ACTION_CANCEL,
                          ADISPLAY_ID_DEFAULT, 0 );
    mWindow->assertNoEvents();
    mDispatcher->waitForIdle();
    mFakePolicy->assertNotifyWindowResponsiveWasCalled(mWindow->getToken(), mWindow->getPid());
    mFakePolicy->assertNotifyAnrWasNotCalled();
}
TEST_F(InputDispatcherSingleWindowAnr, Key_StaysPendingWhileMotionIsProcessed) {
    mWindow->setDispatchingTimeout(2s);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mWindow}}});
    tapOnWindow();
    std::optional<uint32_t> downSequenceNum = mWindow->receiveEvent();
    ASSERT_TRUE(downSequenceNum);
    std::optional<uint32_t> upSequenceNum = mWindow->receiveEvent();
    ASSERT_TRUE(upSequenceNum);
    InputEventInjectionResult result =
            injectKey(mDispatcher, AKEY_EVENT_ACTION_DOWN, 0 , ADISPLAY_ID_DEFAULT,
                      InputEventInjectionSync::WAIT_FOR_RESULT, 10ms);
    ASSERT_EQ(InputEventInjectionResult::TIMED_OUT, result);
    std::optional<uint32_t> keySequenceNum = mWindow->receiveEvent();
    ASSERT_FALSE(keySequenceNum);
    std::this_thread::sleep_for(500ms);
    mWindow->consumeKeyDown(ADISPLAY_ID_DEFAULT);
    mWindow->finishEvent(*downSequenceNum);
    mWindow->finishEvent(*upSequenceNum);
}
TEST_F(InputDispatcherSingleWindowAnr,
       PendingKey_IsDroppedWhileMotionIsProcessedAndNewTouchComesIn) {
    mWindow->setDispatchingTimeout(2s);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mWindow}}});
    tapOnWindow();
    std::optional<uint32_t> downSequenceNum = mWindow->receiveEvent();
    ASSERT_TRUE(downSequenceNum);
    std::optional<uint32_t> upSequenceNum = mWindow->receiveEvent();
    ASSERT_TRUE(upSequenceNum);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectKey(mDispatcher, AKEY_EVENT_ACTION_DOWN, 0 ,
                        ADISPLAY_ID_DEFAULT, InputEventInjectionSync::NONE));
    std::optional<uint32_t> keySequenceNum = mWindow->receiveEvent();
    ASSERT_FALSE(keySequenceNum);
    tapOnWindow();
    mWindow->consumeKeyDown(ADISPLAY_ID_DEFAULT);
    mWindow->finishEvent(*downSequenceNum);
    mWindow->finishEvent(*upSequenceNum);
    mWindow->consumeMotionDown();
    mWindow->consumeMotionUp();
    mWindow->assertNoEvents();
}
class InputDispatcherMultiWindowAnr : public InputDispatcherTest {
    virtual void SetUp() override {
        InputDispatcherTest::SetUp();
        mApplication = std::make_shared<FakeApplicationHandle>();
        mApplication->setDispatchingTimeout(10ms);
        mUnfocusedWindow = sp<FakeWindowHandle>::make(mApplication, mDispatcher, "Unfocused",
                                                      ADISPLAY_ID_DEFAULT);
        mUnfocusedWindow->setFrame(Rect(0, 0, 30, 30));
        mUnfocusedWindow->setWatchOutsideTouch(true);
        mFocusedWindow = sp<FakeWindowHandle>::make(mApplication, mDispatcher, "Focused",
                                                    ADISPLAY_ID_DEFAULT);
        mFocusedWindow->setDispatchingTimeout(30ms);
        mFocusedWindow->setFrame(Rect(50, 50, 100, 100));
        mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, mApplication);
        mFocusedWindow->setFocusable(true);
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
TEST_F(InputDispatcherMultiWindowAnr, TwoWindows_BothUnresponsive) {
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               FOCUSED_WINDOW_LOCATION))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mFocusedWindow->consumeMotionDown();
    mUnfocusedWindow->consumeEvent(AINPUT_EVENT_TYPE_MOTION, AMOTION_EVENT_ACTION_OUTSIDE,
                                   ADISPLAY_ID_DEFAULT, 0 );
    ASSERT_TRUE(mDispatcher->waitForIdle());
    mFakePolicy->assertNotifyAnrWasNotCalled();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               FOCUSED_WINDOW_LOCATION));
    std::optional<uint32_t> unfocusedSequenceNum = mUnfocusedWindow->receiveEvent();
    ASSERT_TRUE(unfocusedSequenceNum);
    const std::chrono::duration timeout =
            mFocusedWindow->getDispatchingTimeout(DISPATCHING_TIMEOUT);
    mFakePolicy->assertNotifyWindowUnresponsiveWasCalled(timeout, mFocusedWindow);
    mFocusedWindow->consumeMotionCancel();
    mUnfocusedWindow->finishEvent(*unfocusedSequenceNum);
    mFocusedWindow->consumeMotionDown();
    mFocusedWindow->consumeMotionCancel();
    mFocusedWindow->assertNoEvents();
    mUnfocusedWindow->assertNoEvents();
    ASSERT_TRUE(mDispatcher->waitForIdle());
    mFakePolicy->assertNotifyWindowResponsiveWasCalled(mFocusedWindow->getToken(),
                                                       mFocusedWindow->getPid());
    mFakePolicy->assertNotifyAnrWasNotCalled();
}
TEST_F(InputDispatcherMultiWindowAnr, TwoWindows_BothUnresponsiveWithSameTimeout) {
    mUnfocusedWindow->setDispatchingTimeout(10ms);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mUnfocusedWindow, mFocusedWindow}}});
    tapOnFocusedWindow();
    sp<IBinder> anrConnectionToken1, anrConnectionToken2;
    ASSERT_NO_FATAL_FAILURE(anrConnectionToken1 = mFakePolicy->getUnresponsiveWindowToken(10ms));
    ASSERT_NO_FATAL_FAILURE(anrConnectionToken2 = mFakePolicy->getUnresponsiveWindowToken(0ms));
    ASSERT_TRUE(mFocusedWindow->getToken() == anrConnectionToken1 ||
                mFocusedWindow->getToken() == anrConnectionToken2);
    ASSERT_TRUE(mUnfocusedWindow->getToken() == anrConnectionToken1 ||
                mUnfocusedWindow->getToken() == anrConnectionToken2);
    ASSERT_TRUE(mDispatcher->waitForIdle());
    mFakePolicy->assertNotifyAnrWasNotCalled();
    mFocusedWindow->consumeMotionDown();
    mFocusedWindow->consumeMotionUp();
    mUnfocusedWindow->consumeMotionOutside();
    sp<IBinder> responsiveToken1, responsiveToken2;
    ASSERT_NO_FATAL_FAILURE(responsiveToken1 = mFakePolicy->getResponsiveWindowToken());
    ASSERT_NO_FATAL_FAILURE(responsiveToken2 = mFakePolicy->getResponsiveWindowToken());
    ASSERT_TRUE(mFocusedWindow->getToken() == responsiveToken1 ||
                mFocusedWindow->getToken() == responsiveToken2);
    ASSERT_TRUE(mUnfocusedWindow->getToken() == responsiveToken1 ||
                mUnfocusedWindow->getToken() == responsiveToken2);
    mFakePolicy->assertNotifyAnrWasNotCalled();
}
TEST_F(InputDispatcherMultiWindowAnr, DuringAnr_SecondTapIsIgnored) {
    tapOnFocusedWindow();
    mUnfocusedWindow->consumeEvent(AINPUT_EVENT_TYPE_MOTION, AMOTION_EVENT_ACTION_OUTSIDE,
                                   ADISPLAY_ID_DEFAULT, 0 );
    std::optional<uint32_t> downEventSequenceNum = mFocusedWindow->receiveEvent();
    ASSERT_TRUE(downEventSequenceNum);
    std::optional<uint32_t> upEventSequenceNum = mFocusedWindow->receiveEvent();
    ASSERT_TRUE(upEventSequenceNum);
    const std::chrono::duration timeout =
            mFocusedWindow->getDispatchingTimeout(DISPATCHING_TIMEOUT);
    mFakePolicy->assertNotifyWindowUnresponsiveWasCalled(timeout, mFocusedWindow);
    ASSERT_EQ(InputEventInjectionResult::FAILED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               FOCUSED_WINDOW_LOCATION));
    ASSERT_EQ(InputEventInjectionResult::FAILED,
              injectMotionUp(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                             FOCUSED_WINDOW_LOCATION));
    mUnfocusedWindow->assertNoEvents();
    mFocusedWindow->finishEvent(*downEventSequenceNum);
    mFocusedWindow->finishEvent(*upEventSequenceNum);
    ASSERT_TRUE(mDispatcher->waitForIdle());
    mFocusedWindow->assertNoEvents();
    mFakePolicy->assertNotifyWindowResponsiveWasCalled(mFocusedWindow->getToken(),
                                                       mFocusedWindow->getPid());
    mFakePolicy->assertNotifyAnrWasNotCalled();
}
TEST_F(InputDispatcherMultiWindowAnr, TapOutsideAllWindows_DoesNotAnr) {
    ASSERT_EQ(InputEventInjectionResult::FAILED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               LOCATION_OUTSIDE_ALL_WINDOWS));
    ASSERT_TRUE(mDispatcher->waitForIdle());
    mFakePolicy->assertNotifyAnrWasNotCalled();
}
TEST_F(InputDispatcherMultiWindowAnr, Window_CanBePaused) {
    mFocusedWindow->setPaused(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mUnfocusedWindow, mFocusedWindow}}});
    ASSERT_EQ(InputEventInjectionResult::FAILED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               FOCUSED_WINDOW_LOCATION));
    std::this_thread::sleep_for(mFocusedWindow->getDispatchingTimeout(DISPATCHING_TIMEOUT));
    ASSERT_TRUE(mDispatcher->waitForIdle());
    mFakePolicy->assertNotifyAnrWasNotCalled();
    mFocusedWindow->assertNoEvents();
    mUnfocusedWindow->assertNoEvents();
}
TEST_F(InputDispatcherMultiWindowAnr, PendingKey_GoesToNewlyFocusedWindow) {
    mFocusedWindow->setDispatchingTimeout(2s);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mFocusedWindow, mUnfocusedWindow}}});
    tapOnUnfocusedWindow();
    std::optional<uint32_t> downSequenceNum = mUnfocusedWindow->receiveEvent();
    ASSERT_TRUE(downSequenceNum);
    std::optional<uint32_t> upSequenceNum = mUnfocusedWindow->receiveEvent();
    ASSERT_TRUE(upSequenceNum);
    InputEventInjectionResult result =
            injectKey(mDispatcher, AKEY_EVENT_ACTION_DOWN, 0 , ADISPLAY_ID_DEFAULT,
                      InputEventInjectionSync::NONE, 10ms );
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, result);
    std::optional<uint32_t> keySequenceNum = mFocusedWindow->receiveEvent();
    ASSERT_FALSE(keySequenceNum);
    mFocusedWindow->setFocusable(false);
    mUnfocusedWindow->setFocusable(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mFocusedWindow, mUnfocusedWindow}}});
    setFocusedWindow(mUnfocusedWindow);
    mUnfocusedWindow->consumeFocusEvent(true);
    mFocusedWindow->consumeFocusEvent(false);
    mUnfocusedWindow->finishEvent(*downSequenceNum);
    mUnfocusedWindow->finishEvent(*upSequenceNum);
    mUnfocusedWindow->consumeKeyDown(ADISPLAY_ID_DEFAULT);
    mFocusedWindow->assertNoEvents();
    mUnfocusedWindow->assertNoEvents();
    mFakePolicy->assertNotifyAnrWasNotCalled();
}
TEST_F(InputDispatcherMultiWindowAnr, SplitTouch_SingleWindowAnr) {
    NotifyMotionArgs motionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT, {FOCUSED_WINDOW_LOCATION});
    mDispatcher->notifyMotion(&motionArgs);
    mUnfocusedWindow->consumeEvent(AINPUT_EVENT_TYPE_MOTION, AMOTION_EVENT_ACTION_OUTSIDE,
                                   ADISPLAY_ID_DEFAULT, 0 );
    motionArgs = generateMotionArgs(POINTER_1_DOWN, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                                    {FOCUSED_WINDOW_LOCATION, UNFOCUSED_WINDOW_LOCATION});
    mDispatcher->notifyMotion(&motionArgs);
    const std::chrono::duration timeout =
            mFocusedWindow->getDispatchingTimeout(DISPATCHING_TIMEOUT);
    mFakePolicy->assertNotifyWindowUnresponsiveWasCalled(timeout, mFocusedWindow);
    mUnfocusedWindow->consumeMotionDown();
    mFocusedWindow->consumeMotionDown();
    InputEvent* event;
    std::optional<int32_t> moveOrCancelSequenceNum = mFocusedWindow->receiveEvent(&event);
    ASSERT_TRUE(moveOrCancelSequenceNum);
    mFocusedWindow->finishEvent(*moveOrCancelSequenceNum);
    ASSERT_NE(nullptr, event);
    ASSERT_EQ(event->getType(), AINPUT_EVENT_TYPE_MOTION);
    MotionEvent& motionEvent = static_cast<MotionEvent&>(*event);
    if (motionEvent.getAction() == AMOTION_EVENT_ACTION_MOVE) {
        mFocusedWindow->consumeMotionCancel();
    } else {
        ASSERT_EQ(AMOTION_EVENT_ACTION_CANCEL, motionEvent.getAction());
    }
    ASSERT_TRUE(mDispatcher->waitForIdle());
    mFakePolicy->assertNotifyWindowResponsiveWasCalled(mFocusedWindow->getToken(),
                                                       mFocusedWindow->getPid());
    mUnfocusedWindow->assertNoEvents();
    mFocusedWindow->assertNoEvents();
    mFakePolicy->assertNotifyAnrWasNotCalled();
}
TEST_F(InputDispatcherMultiWindowAnr, FocusedWindowWithoutSetFocusedApplication_NoAnr) {
    std::shared_ptr<FakeApplicationHandle> focusedApplication =
            std::make_shared<FakeApplicationHandle>();
    focusedApplication->setDispatchingTimeout(60ms);
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, focusedApplication);
    mFocusedWindow->setFocusable(false);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mFocusedWindow, mUnfocusedWindow}}});
    mFocusedWindow->consumeFocusEvent(false);
    InputEventInjectionResult result =
            injectKey(mDispatcher, AKEY_EVENT_ACTION_DOWN, 0 , ADISPLAY_ID_DEFAULT,
                      InputEventInjectionSync::NONE, 10ms ,
                      false );
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, result);
    std::this_thread::sleep_for(10ms);
    NotifyMotionArgs motionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT, {UNFOCUSED_WINDOW_LOCATION});
    mDispatcher->notifyMotion(&motionArgs);
    mFocusedWindow->setFocusable(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mFocusedWindow, mUnfocusedWindow}}});
    setFocusedWindow(mFocusedWindow);
    mFocusedWindow->consumeFocusEvent(true);
    mUnfocusedWindow->consumeMotionDown();
    ASSERT_TRUE(mDispatcher->waitForIdle());
    ASSERT_NO_FATAL_FAILURE(mFakePolicy->assertNotifyAnrWasNotCalled());
}
class InputDispatcherMultiWindowOcclusionTests : public InputDispatcherTest {
    virtual void SetUp() override {
        InputDispatcherTest::SetUp();
        mApplication = std::make_shared<FakeApplicationHandle>();
        mNoInputWindow =
                sp<FakeWindowHandle>::make(mApplication, mDispatcher,
                                           "Window without input channel", ADISPLAY_ID_DEFAULT,
                                           std::make_optional<sp<IBinder>>(nullptr) );
        mNoInputWindow->setNoInputChannel(true);
        mNoInputWindow->setFrame(Rect(0, 0, 100, 100));
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
TEST_F(InputDispatcherMultiWindowOcclusionTests, NoInputChannelFeature_DropsTouches) {
    PointF touchedPoint = {10, 10};
    NotifyMotionArgs motionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT, {touchedPoint});
    mDispatcher->notifyMotion(&motionArgs);
    mNoInputWindow->assertNoEvents();
    mBottomWindow->assertNoEvents();
}
TEST_F(InputDispatcherMultiWindowOcclusionTests,
       NoInputChannelFeature_DropsTouchesWithValidChannel) {
    mNoInputWindow = sp<FakeWindowHandle>::make(mApplication, mDispatcher,
                                                "Window with input channel and NO_INPUT_CHANNEL",
                                                ADISPLAY_ID_DEFAULT);
    mNoInputWindow->setNoInputChannel(true);
    mNoInputWindow->setFrame(Rect(0, 0, 100, 100));
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mNoInputWindow, mBottomWindow}}});
    PointF touchedPoint = {10, 10};
    NotifyMotionArgs motionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT, {touchedPoint});
    mDispatcher->notifyMotion(&motionArgs);
    mNoInputWindow->assertNoEvents();
    mBottomWindow->assertNoEvents();
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
TEST_F(InputDispatcherMirrorWindowFocusTests, CanGetFocus) {
    setFocusedWindow(mMirror);
    mWindow->consumeFocusEvent(true);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyDown(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::SUCCEEDED";
    mWindow->consumeKeyDown(ADISPLAY_ID_NONE);
}
TEST_F(InputDispatcherMirrorWindowFocusTests, FocusedIfAllWindowsFocusable) {
    setFocusedWindow(mMirror);
    mWindow->consumeFocusEvent(true);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyDown(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::SUCCEEDED";
    mWindow->consumeKeyDown(ADISPLAY_ID_NONE);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyUp(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::SUCCEEDED";
    mWindow->consumeKeyUp(ADISPLAY_ID_NONE);
    mMirror->setFocusable(false);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mWindow, mMirror}}});
    mWindow->consumeFocusEvent(false);
    ASSERT_EQ(InputEventInjectionResult::TIMED_OUT, injectKeyDown(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::TIMED_OUT";
    mWindow->assertNoEvents();
}
TEST_F(InputDispatcherMirrorWindowFocusTests, FocusedIfAnyWindowVisible) {
    setFocusedWindow(mMirror);
    mWindow->consumeFocusEvent(true);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyDown(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::SUCCEEDED";
    mWindow->consumeKeyDown(ADISPLAY_ID_NONE);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyUp(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::SUCCEEDED";
    mWindow->consumeKeyUp(ADISPLAY_ID_NONE);
    mMirror->setVisible(false);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mWindow, mMirror}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyDown(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::SUCCEEDED";
    mWindow->consumeKeyDown(ADISPLAY_ID_NONE);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyUp(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::SUCCEEDED";
    mWindow->consumeKeyUp(ADISPLAY_ID_NONE);
    mWindow->setVisible(false);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mWindow, mMirror}}});
    mWindow->consumeFocusEvent(false);
    ASSERT_EQ(InputEventInjectionResult::TIMED_OUT, injectKeyDown(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::TIMED_OUT";
    mWindow->assertNoEvents();
}
TEST_F(InputDispatcherMirrorWindowFocusTests, FocusedWhileWindowsAlive) {
    setFocusedWindow(mMirror);
    mWindow->consumeFocusEvent(true);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyDown(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::SUCCEEDED";
    mWindow->consumeKeyDown(ADISPLAY_ID_NONE);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyUp(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::SUCCEEDED";
    mWindow->consumeKeyUp(ADISPLAY_ID_NONE);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mMirror}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyDown(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::SUCCEEDED";
    mWindow->consumeKeyDown(ADISPLAY_ID_NONE);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyUp(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::SUCCEEDED";
    mWindow->consumeKeyUp(ADISPLAY_ID_NONE);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {}}});
    mWindow->consumeFocusEvent(false);
    ASSERT_EQ(InputEventInjectionResult::TIMED_OUT, injectKeyDown(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::TIMED_OUT";
    mWindow->assertNoEvents();
}
TEST_F(InputDispatcherMirrorWindowFocusTests, DeferFocusWhenInvisible) {
    mWindow->setVisible(false);
    mMirror->setVisible(false);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mWindow, mMirror}}});
    setFocusedWindow(mMirror);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectKey(mDispatcher, AKEY_EVENT_ACTION_DOWN, 0 ,
                        ADISPLAY_ID_DEFAULT, InputEventInjectionSync::NONE));
    mMirror->setVisible(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mWindow, mMirror}}});
    mWindow->consumeFocusEvent(true);
    mWindow->consumeKeyDown(ADISPLAY_ID_DEFAULT);
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
TEST_F(InputDispatcherPointerCaptureTests, EnablePointerCaptureWhenFocused) {
    mDispatcher->requestPointerCapture(mSecondWindow->getToken(), true);
    mFakePolicy->assertSetPointerCaptureNotCalled();
    mSecondWindow->assertNoEvents();
    requestAndVerifyPointerCapture(mWindow, true);
    mDispatcher->requestPointerCapture(mSecondWindow->getToken(), false);
    mFakePolicy->assertSetPointerCaptureNotCalled();
    requestAndVerifyPointerCapture(mWindow, false);
}
TEST_F(InputDispatcherPointerCaptureTests, DisablesPointerCaptureAfterWindowLosesFocus) {
    auto request = requestAndVerifyPointerCapture(mWindow, true);
    setFocusedWindow(mSecondWindow);
    mWindow->consumeCaptureEvent(false);
    mWindow->consumeFocusEvent(false);
    mSecondWindow->consumeFocusEvent(true);
    mFakePolicy->assertSetPointerCaptureCalled(false);
    notifyPointerCaptureChanged({});
    notifyPointerCaptureChanged(request);
    notifyPointerCaptureChanged({});
    mWindow->assertNoEvents();
    mSecondWindow->assertNoEvents();
    mFakePolicy->assertSetPointerCaptureNotCalled();
}
TEST_F(InputDispatcherPointerCaptureTests, UnexpectedStateChangeDisablesPointerCapture) {
    auto request = requestAndVerifyPointerCapture(mWindow, true);
    notifyPointerCaptureChanged({});
    notifyPointerCaptureChanged(request);
    mFakePolicy->assertSetPointerCaptureCalled(false);
    mWindow->consumeCaptureEvent(false);
    mWindow->assertNoEvents();
}
TEST_F(InputDispatcherPointerCaptureTests, OutOfOrderRequests) {
    requestAndVerifyPointerCapture(mWindow, true);
    setFocusedWindow(mSecondWindow);
    mFakePolicy->assertSetPointerCaptureCalled(false);
    mWindow->consumeCaptureEvent(false);
    mDispatcher->requestPointerCapture(mSecondWindow->getToken(), true);
    auto request = mFakePolicy->assertSetPointerCaptureCalled(true);
    notifyPointerCaptureChanged({});
    notifyPointerCaptureChanged(request);
    mSecondWindow->consumeFocusEvent(true);
    mSecondWindow->consumeCaptureEvent(true);
}
TEST_F(InputDispatcherPointerCaptureTests, EnableRequestFollowsSequenceNumbers) {
    mDispatcher->requestPointerCapture(mWindow->getToken(), true);
    auto firstRequest = mFakePolicy->assertSetPointerCaptureCalled(true);
    mDispatcher->requestPointerCapture(mWindow->getToken(), false);
    mFakePolicy->assertSetPointerCaptureCalled(false);
    mDispatcher->requestPointerCapture(mWindow->getToken(), true);
    auto secondRequest = mFakePolicy->assertSetPointerCaptureCalled(true);
    notifyPointerCaptureChanged(firstRequest);
    mWindow->assertNoEvents();
    notifyPointerCaptureChanged(secondRequest);
    mWindow->consumeCaptureEvent(true);
}
TEST_F(InputDispatcherPointerCaptureTests, RapidToggleRequests) {
    requestAndVerifyPointerCapture(mWindow, true);
    mDispatcher->requestPointerCapture(mWindow->getToken(), false);
    mFakePolicy->assertSetPointerCaptureCalled(false);
    mDispatcher->requestPointerCapture(mWindow->getToken(), true);
    auto enableRequest = mFakePolicy->assertSetPointerCaptureCalled(true);
    notifyPointerCaptureChanged(enableRequest);
    mWindow->assertNoEvents();
}
class InputDispatcherUntrustedTouchesTest : public InputDispatcherTest {
protected:
    constexpr static const float MAXIMUM_OBSCURING_OPACITY = 0.8;
    constexpr static const float OPACITY_ABOVE_THRESHOLD = 0.9;
    static_assert(OPACITY_ABOVE_THRESHOLD > MAXIMUM_OBSCURING_OPACITY);
    constexpr static const float OPACITY_BELOW_THRESHOLD = 0.7;
    static_assert(OPACITY_BELOW_THRESHOLD < MAXIMUM_OBSCURING_OPACITY);
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
TEST_F(InputDispatcherUntrustedTouchesTest, WindowWithBlockUntrustedOcclusionMode_BlocksTouch) {
    const sp<FakeWindowHandle>& w =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::BLOCK_UNTRUSTED);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w, mTouchWindow}}});
    touch();
    mTouchWindow->assertNoEvents();
}
TEST_F(InputDispatcherUntrustedTouchesTest,
       WindowWithBlockUntrustedOcclusionModeWithOpacityBelowThreshold_BlocksTouch) {
    const sp<FakeWindowHandle>& w =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::BLOCK_UNTRUSTED, 0.7f);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w, mTouchWindow}}});
    touch();
    mTouchWindow->assertNoEvents();
}
TEST_F(InputDispatcherUntrustedTouchesTest,
       WindowWithBlockUntrustedOcclusionMode_DoesNotReceiveTouch) {
    const sp<FakeWindowHandle>& w =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::BLOCK_UNTRUSTED);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w, mTouchWindow}}});
    touch();
    w->assertNoEvents();
}
TEST_F(InputDispatcherUntrustedTouchesTest, WindowWithAllowOcclusionMode_AllowsTouch) {
    const sp<FakeWindowHandle>& w = getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::ALLOW);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w, mTouchWindow}}});
    touch();
    mTouchWindow->consumeAnyMotionDown();
}
TEST_F(InputDispatcherUntrustedTouchesTest, TouchOutsideOccludingWindow_AllowsTouch) {
    const sp<FakeWindowHandle>& w =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::BLOCK_UNTRUSTED);
    w->setFrame(Rect(0, 0, 50, 50));
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w, mTouchWindow}}});
    touch({PointF{100, 100}});
    mTouchWindow->consumeAnyMotionDown();
}
TEST_F(InputDispatcherUntrustedTouchesTest, WindowFromSameUid_AllowsTouch) {
    const sp<FakeWindowHandle>& w =
            getOccludingWindow(TOUCHED_APP_UID, "A", TouchOcclusionMode::BLOCK_UNTRUSTED);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w, mTouchWindow}}});
    touch();
    mTouchWindow->consumeAnyMotionDown();
}
TEST_F(InputDispatcherUntrustedTouchesTest, WindowWithZeroOpacity_AllowsTouch) {
    const sp<FakeWindowHandle>& w =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::BLOCK_UNTRUSTED, 0.0f);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w, mTouchWindow}}});
    touch();
    mTouchWindow->consumeAnyMotionDown();
}
TEST_F(InputDispatcherUntrustedTouchesTest, WindowWithZeroOpacity_DoesNotReceiveTouch) {
    const sp<FakeWindowHandle>& w =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::BLOCK_UNTRUSTED, 0.0f);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w, mTouchWindow}}});
    touch();
    w->assertNoEvents();
}
TEST_F(InputDispatcherUntrustedTouchesTest,
       WindowWithZeroOpacityAndWatchOutside_ReceivesOutsideEvent) {
    const sp<FakeWindowHandle>& w =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::BLOCK_UNTRUSTED, 0.0f);
    w->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w, mTouchWindow}}});
    touch();
    w->consumeMotionOutside();
}
TEST_F(InputDispatcherUntrustedTouchesTest, OutsideEvent_HasZeroCoordinates) {
    const sp<FakeWindowHandle>& w =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::BLOCK_UNTRUSTED, 0.0f);
    w->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w, mTouchWindow}}});
    touch();
    w->consumeMotionOutsideWithZeroedCoords();
}
TEST_F(InputDispatcherUntrustedTouchesTest, WindowWithOpacityBelowThreshold_AllowsTouch) {
    const sp<FakeWindowHandle>& w =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::USE_OPACITY,
                               OPACITY_BELOW_THRESHOLD);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w, mTouchWindow}}});
    touch();
    mTouchWindow->consumeAnyMotionDown();
}
TEST_F(InputDispatcherUntrustedTouchesTest, WindowWithOpacityAtThreshold_AllowsTouch) {
    const sp<FakeWindowHandle>& w =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::USE_OPACITY,
                               MAXIMUM_OBSCURING_OPACITY);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w, mTouchWindow}}});
    touch();
    mTouchWindow->consumeAnyMotionDown();
}
TEST_F(InputDispatcherUntrustedTouchesTest, WindowWithOpacityAboveThreshold_BlocksTouch) {
    const sp<FakeWindowHandle>& w =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::USE_OPACITY,
                               OPACITY_ABOVE_THRESHOLD);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w, mTouchWindow}}});
    touch();
    mTouchWindow->assertNoEvents();
}
TEST_F(InputDispatcherUntrustedTouchesTest, WindowsWithCombinedOpacityAboveThreshold_BlocksTouch) {
    const sp<FakeWindowHandle>& w1 =
            getOccludingWindow(APP_B_UID, "B1", TouchOcclusionMode::USE_OPACITY,
                               OPACITY_BELOW_THRESHOLD);
    const sp<FakeWindowHandle>& w2 =
            getOccludingWindow(APP_B_UID, "B2", TouchOcclusionMode::USE_OPACITY,
                               OPACITY_BELOW_THRESHOLD);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w1, w2, mTouchWindow}}});
    touch();
    mTouchWindow->assertNoEvents();
}
TEST_F(InputDispatcherUntrustedTouchesTest, WindowsWithCombinedOpacityBelowThreshold_AllowsTouch) {
    const sp<FakeWindowHandle>& w1 =
            getOccludingWindow(APP_B_UID, "B1", TouchOcclusionMode::USE_OPACITY,
                               OPACITY_FAR_BELOW_THRESHOLD);
    const sp<FakeWindowHandle>& w2 =
            getOccludingWindow(APP_B_UID, "B2", TouchOcclusionMode::USE_OPACITY,
                               OPACITY_FAR_BELOW_THRESHOLD);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w1, w2, mTouchWindow}}});
    touch();
    mTouchWindow->consumeAnyMotionDown();
}
TEST_F(InputDispatcherUntrustedTouchesTest,
       WindowsFromDifferentAppsEachBelowThreshold_AllowsTouch) {
    const sp<FakeWindowHandle>& wB =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::USE_OPACITY,
                               OPACITY_BELOW_THRESHOLD);
    const sp<FakeWindowHandle>& wC =
            getOccludingWindow(APP_C_UID, "C", TouchOcclusionMode::USE_OPACITY,
                               OPACITY_BELOW_THRESHOLD);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {wB, wC, mTouchWindow}}});
    touch();
    mTouchWindow->consumeAnyMotionDown();
}
TEST_F(InputDispatcherUntrustedTouchesTest, WindowsFromDifferentAppsOneAboveThreshold_BlocksTouch) {
    const sp<FakeWindowHandle>& wB =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::USE_OPACITY,
                               OPACITY_BELOW_THRESHOLD);
    const sp<FakeWindowHandle>& wC =
            getOccludingWindow(APP_C_UID, "C", TouchOcclusionMode::USE_OPACITY,
                               OPACITY_ABOVE_THRESHOLD);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {wB, wC, mTouchWindow}}});
    touch();
    mTouchWindow->assertNoEvents();
}
TEST_F(InputDispatcherUntrustedTouchesTest,
       WindowWithOpacityAboveThresholdAndSelfWindow_BlocksTouch) {
    const sp<FakeWindowHandle>& wA =
            getOccludingWindow(TOUCHED_APP_UID, "T", TouchOcclusionMode::USE_OPACITY,
                               OPACITY_BELOW_THRESHOLD);
    const sp<FakeWindowHandle>& wB =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::USE_OPACITY,
                               OPACITY_ABOVE_THRESHOLD);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {wA, wB, mTouchWindow}}});
    touch();
    mTouchWindow->assertNoEvents();
}
TEST_F(InputDispatcherUntrustedTouchesTest,
       WindowWithOpacityBelowThresholdAndSelfWindow_AllowsTouch) {
    const sp<FakeWindowHandle>& wA =
            getOccludingWindow(TOUCHED_APP_UID, "T", TouchOcclusionMode::USE_OPACITY,
                               OPACITY_ABOVE_THRESHOLD);
    const sp<FakeWindowHandle>& wB =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::USE_OPACITY,
                               OPACITY_BELOW_THRESHOLD);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {wA, wB, mTouchWindow}}});
    touch();
    mTouchWindow->consumeAnyMotionDown();
}
TEST_F(InputDispatcherUntrustedTouchesTest, SelfWindowWithOpacityAboveThreshold_AllowsTouch) {
    const sp<FakeWindowHandle>& w =
            getOccludingWindow(TOUCHED_APP_UID, "T", TouchOcclusionMode::USE_OPACITY,
                               OPACITY_ABOVE_THRESHOLD);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w, mTouchWindow}}});
    touch();
    mTouchWindow->consumeAnyMotionDown();
}
TEST_F(InputDispatcherUntrustedTouchesTest, SelfWindowWithBlockUntrustedMode_AllowsTouch) {
    const sp<FakeWindowHandle>& w =
            getOccludingWindow(TOUCHED_APP_UID, "T", TouchOcclusionMode::BLOCK_UNTRUSTED);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w, mTouchWindow}}});
    touch();
    mTouchWindow->consumeAnyMotionDown();
}
TEST_F(InputDispatcherUntrustedTouchesTest,
       OpacityThresholdIs0AndWindowAboveThreshold_BlocksTouch) {
    mDispatcher->setMaximumObscuringOpacityForTouch(0.0f);
    const sp<FakeWindowHandle>& w =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::USE_OPACITY, 0.1f);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w, mTouchWindow}}});
    touch();
    mTouchWindow->assertNoEvents();
}
TEST_F(InputDispatcherUntrustedTouchesTest, OpacityThresholdIs0AndWindowAtThreshold_AllowsTouch) {
    mDispatcher->setMaximumObscuringOpacityForTouch(0.0f);
    const sp<FakeWindowHandle>& w =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::USE_OPACITY, 0.0f);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w, mTouchWindow}}});
    touch();
    mTouchWindow->consumeAnyMotionDown();
}
TEST_F(InputDispatcherUntrustedTouchesTest,
       OpacityThresholdIs1AndWindowBelowThreshold_AllowsTouch) {
    mDispatcher->setMaximumObscuringOpacityForTouch(1.0f);
    const sp<FakeWindowHandle>& w =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::USE_OPACITY,
                               OPACITY_ABOVE_THRESHOLD);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w, mTouchWindow}}});
    touch();
    mTouchWindow->consumeAnyMotionDown();
}
TEST_F(InputDispatcherUntrustedTouchesTest,
       WindowWithBlockUntrustedModeAndWindowWithOpacityBelowFromSameApp_BlocksTouch) {
    const sp<FakeWindowHandle>& w1 =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::BLOCK_UNTRUSTED,
                               OPACITY_BELOW_THRESHOLD);
    const sp<FakeWindowHandle>& w2 =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::USE_OPACITY,
                               OPACITY_BELOW_THRESHOLD);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w1, w2, mTouchWindow}}});
    touch();
    mTouchWindow->assertNoEvents();
}
TEST_F(InputDispatcherUntrustedTouchesTest,
       WindowWithBlockUntrustedModeAndWindowWithOpacityBelowFromDifferentApps_BlocksTouch) {
    const sp<FakeWindowHandle>& wB =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::BLOCK_UNTRUSTED,
                               OPACITY_BELOW_THRESHOLD);
    const sp<FakeWindowHandle>& wC =
            getOccludingWindow(APP_C_UID, "C", TouchOcclusionMode::USE_OPACITY,
                               OPACITY_BELOW_THRESHOLD);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {wB, wC, mTouchWindow}}});
    touch();
    mTouchWindow->assertNoEvents();
}
TEST_F(InputDispatcherUntrustedTouchesTest,
       WindowWithSameApplicationTokenFromDifferentApp_AllowsTouch) {
    const sp<FakeWindowHandle>& w =
            getOccludingWindow(APP_B_UID, "B", TouchOcclusionMode::BLOCK_UNTRUSTED);
    w->setApplicationToken(mTouchWindow->getApplicationToken());
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {w, mTouchWindow}}});
    touch();
    mTouchWindow->consumeAnyMotionDown();
}
class InputDispatcherDragTests : public InputDispatcherTest {
protected:
    std::shared_ptr<FakeApplicationHandle> mApp;
    sp<FakeWindowHandle> mWindow;
    sp<FakeWindowHandle> mSecondWindow;
    sp<FakeWindowHandle> mDragWindow;
    sp<FakeWindowHandle> mSpyWindow;
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
        mWindow->consumeMotionDown(ADISPLAY_ID_DEFAULT);
        mSpyWindow->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    }
    bool startDrag(bool sendDown = true, int fromSource = AINPUT_SOURCE_TOUCHSCREEN) {
        if (sendDown) {
            injectDown(fromSource);
        }
        mDragWindow =
                sp<FakeWindowHandle>::make(mApp, mDispatcher, "DragWindow", ADISPLAY_ID_DEFAULT);
        mDragWindow->setTouchableRegion(Region{{0, 0, 0, 0}});
        mDispatcher->setInputWindows(
                {{ADISPLAY_ID_DEFAULT, {mDragWindow, mSpyWindow, mWindow, mSecondWindow}}});
        bool transferred =
                mDispatcher->transferTouchFocus(mWindow->getToken(), mDragWindow->getToken(),
                                                true );
        if (transferred) {
            mWindow->consumeMotionCancel();
            mDragWindow->consumeMotionDown();
        }
        return transferred;
    }
};
TEST_F(InputDispatcherDragTests, DragEnterAndDragExit) {
    startDrag();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN,
                                ADISPLAY_ID_DEFAULT, {50, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mDragWindow->consumeMotionMove(ADISPLAY_ID_DEFAULT);
    mWindow->consumeDragEvent(false, 50, 50);
    mSecondWindow->assertNoEvents();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN,
                                ADISPLAY_ID_DEFAULT, {150, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mDragWindow->consumeMotionMove(ADISPLAY_ID_DEFAULT);
    mWindow->consumeDragEvent(true, 150, 50);
    mSecondWindow->consumeDragEvent(false, 50, 50);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN,
                                ADISPLAY_ID_DEFAULT, {50, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mDragWindow->consumeMotionMove(ADISPLAY_ID_DEFAULT);
    mWindow->consumeDragEvent(false, 50, 50);
    mSecondWindow->consumeDragEvent(true, -50, 50);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionUp(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT, {50, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mDragWindow->consumeMotionUp(ADISPLAY_ID_DEFAULT);
    mWindow->assertNoEvents();
    mSecondWindow->assertNoEvents();
}
TEST_F(InputDispatcherDragTests, DragEnterAndPointerDownPilfersPointers) {
    startDrag();
    mSpyWindow->assertNoEvents();
    const MotionEvent secondFingerDownEvent =
            MotionEventBuilder(POINTER_1_DOWN, AINPUT_SOURCE_TOUCHSCREEN)
                    .eventTime(systemTime(SYSTEM_TIME_MONOTONIC))
                    .pointer(PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER).x(50).y(50))
                    .pointer(PointerBuilder( 1, AMOTION_EVENT_TOOL_TYPE_FINGER).x(60).y(60))
                    .build();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, secondFingerDownEvent, INJECT_EVENT_TIMEOUT,
                                InputEventInjectionSync::WAIT_FOR_RESULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mSpyWindow->consumeMotionCancel();
    mSpyWindow->consumeMotionDown();
    mSpyWindow->assertNoEvents();
}
TEST_F(InputDispatcherDragTests, DragAndDrop) {
    startDrag();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN,
                                ADISPLAY_ID_DEFAULT, {50, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mDragWindow->consumeMotionMove(ADISPLAY_ID_DEFAULT);
    mWindow->consumeDragEvent(false, 50, 50);
    mSecondWindow->assertNoEvents();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN,
                                ADISPLAY_ID_DEFAULT, {150, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mDragWindow->consumeMotionMove(ADISPLAY_ID_DEFAULT);
    mWindow->consumeDragEvent(true, 150, 50);
    mSecondWindow->consumeDragEvent(false, 50, 50);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionUp(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                             {150, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mDragWindow->consumeMotionUp(ADISPLAY_ID_DEFAULT);
    mFakePolicy->assertDropTargetEquals(mSecondWindow->getToken());
    mWindow->assertNoEvents();
    mSecondWindow->assertNoEvents();
}
TEST_F(InputDispatcherDragTests, StylusDragAndDrop) {
    startDrag(true, AINPUT_SOURCE_STYLUS);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_STYLUS)
                                        .buttonState(AMOTION_EVENT_BUTTON_STYLUS_PRIMARY)
                                        .pointer(PointerBuilder(0, AMOTION_EVENT_TOOL_TYPE_STYLUS)
                                                         .x(50)
                                                         .y(50))
                                        .build()))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mDragWindow->consumeMotionMove(ADISPLAY_ID_DEFAULT);
    mWindow->consumeDragEvent(false, 50, 50);
    mSecondWindow->assertNoEvents();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_STYLUS)
                                        .buttonState(0)
                                        .pointer(PointerBuilder(0, AMOTION_EVENT_TOOL_TYPE_STYLUS)
                                                         .x(150)
                                                         .y(50))
                                        .build()))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mDragWindow->consumeMotionMove(ADISPLAY_ID_DEFAULT);
    mWindow->assertNoEvents();
    mSecondWindow->assertNoEvents();
    mFakePolicy->assertDropTargetEquals(mSecondWindow->getToken());
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_UP, AINPUT_SOURCE_STYLUS)
                                        .buttonState(0)
                                        .pointer(PointerBuilder(0, AMOTION_EVENT_TOOL_TYPE_STYLUS)
                                                         .x(150)
                                                         .y(50))
                                        .build()))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mDragWindow->consumeMotionUp(ADISPLAY_ID_DEFAULT);
    mWindow->assertNoEvents();
    mSecondWindow->assertNoEvents();
}
TEST_F(InputDispatcherDragTests, DragAndDropOnInvalidWindow) {
    startDrag();
    mSecondWindow->setVisible(false);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mDragWindow, mWindow, mSecondWindow}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN,
                                ADISPLAY_ID_DEFAULT, {50, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mDragWindow->consumeMotionMove(ADISPLAY_ID_DEFAULT);
    mWindow->consumeDragEvent(false, 50, 50);
    mSecondWindow->assertNoEvents();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN,
                                ADISPLAY_ID_DEFAULT, {150, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mDragWindow->consumeMotionMove(ADISPLAY_ID_DEFAULT);
    mWindow->consumeDragEvent(true, 150, 50);
    mSecondWindow->assertNoEvents();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionUp(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                             {150, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mDragWindow->consumeMotionUp(ADISPLAY_ID_DEFAULT);
    mFakePolicy->assertDropTargetEquals(nullptr);
    mWindow->assertNoEvents();
    mSecondWindow->assertNoEvents();
}
TEST_F(InputDispatcherDragTests, NoDragAndDropWhenMultiFingers) {
    mWindow->setPreventSplitting(true);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {50, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mWindow->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    const MotionEvent secondFingerDownEvent =
            MotionEventBuilder(POINTER_1_DOWN, AINPUT_SOURCE_TOUCHSCREEN)
                    .displayId(ADISPLAY_ID_DEFAULT)
                    .eventTime(systemTime(SYSTEM_TIME_MONOTONIC))
                    .pointer(PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER).x(50).y(50))
                    .pointer(PointerBuilder( 1, AMOTION_EVENT_TOOL_TYPE_FINGER).x(75).y(50))
                    .build();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, secondFingerDownEvent, INJECT_EVENT_TIMEOUT,
                                InputEventInjectionSync::WAIT_FOR_RESULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mWindow->consumeMotionPointerDown(1 );
    ASSERT_FALSE(startDrag(false));
}
TEST_F(InputDispatcherDragTests, DragAndDropWhenSplitTouch) {
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {150, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mSecondWindow->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    const MotionEvent secondFingerDownEvent =
            MotionEventBuilder(POINTER_1_DOWN, AINPUT_SOURCE_TOUCHSCREEN)
                    .displayId(ADISPLAY_ID_DEFAULT)
                    .eventTime(systemTime(SYSTEM_TIME_MONOTONIC))
                    .pointer(
                            PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER).x(150).y(50))
                    .pointer(PointerBuilder( 1, AMOTION_EVENT_TOOL_TYPE_FINGER).x(50).y(50))
                    .build();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, secondFingerDownEvent, INJECT_EVENT_TIMEOUT,
                                InputEventInjectionSync::WAIT_FOR_RESULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mWindow->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    ASSERT_TRUE(startDrag(false));
    const MotionEvent secondFingerMoveEvent =
            MotionEventBuilder(AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN)
                    .eventTime(systemTime(SYSTEM_TIME_MONOTONIC))
                    .pointer(
                            PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER).x(150).y(50))
                    .pointer(PointerBuilder( 1, AMOTION_EVENT_TOOL_TYPE_FINGER).x(50).y(50))
                    .build();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, secondFingerMoveEvent, INJECT_EVENT_TIMEOUT,
                                InputEventInjectionSync::WAIT_FOR_RESULT));
    mDragWindow->consumeMotionMove(ADISPLAY_ID_DEFAULT);
    mWindow->consumeDragEvent(false, 50, 50);
    mSecondWindow->consumeMotionMove();
    const MotionEvent secondFingerUpEvent =
            MotionEventBuilder(POINTER_1_UP, AINPUT_SOURCE_TOUCHSCREEN)
                    .eventTime(systemTime(SYSTEM_TIME_MONOTONIC))
                    .pointer(
                            PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER).x(150).y(50))
                    .pointer(PointerBuilder( 1, AMOTION_EVENT_TOOL_TYPE_FINGER).x(50).y(50))
                    .build();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, secondFingerUpEvent, INJECT_EVENT_TIMEOUT,
                                InputEventInjectionSync::WAIT_FOR_RESULT));
    mDragWindow->consumeMotionUp(ADISPLAY_ID_DEFAULT);
    mFakePolicy->assertDropTargetEquals(mWindow->getToken());
    mWindow->assertNoEvents();
    mSecondWindow->consumeMotionMove();
}
TEST_F(InputDispatcherDragTests, DragAndDropWhenMultiDisplays) {
    startDrag();
    sp<FakeWindowHandle> windowInSecondary =
            sp<FakeWindowHandle>::make(mApp, mDispatcher, "D_2", SECOND_DISPLAY_ID);
    mDispatcher->setInputWindows({{SECOND_DISPLAY_ID, {windowInSecondary}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_DOWN,
                                                   AINPUT_SOURCE_TOUCHSCREEN)
                                        .displayId(SECOND_DISPLAY_ID)
                                        .pointer(PointerBuilder(0, AMOTION_EVENT_TOOL_TYPE_FINGER)
                                                         .x(100)
                                                         .y(100))
                                        .build()));
    windowInSecondary->consumeEvent(AINPUT_EVENT_TYPE_MOTION, AMOTION_EVENT_ACTION_DOWN,
                                    SECOND_DISPLAY_ID, 0 );
    mDispatcher->setInputWindows({{SECOND_DISPLAY_ID, {windowInSecondary}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN,
                                ADISPLAY_ID_DEFAULT, {50, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mDragWindow->consumeMotionMove(ADISPLAY_ID_DEFAULT);
    mWindow->consumeDragEvent(false, 50, 50);
    mSecondWindow->assertNoEvents();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN,
                                ADISPLAY_ID_DEFAULT, {150, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mDragWindow->consumeMotionMove(ADISPLAY_ID_DEFAULT);
    mWindow->consumeDragEvent(true, 150, 50);
    mSecondWindow->consumeDragEvent(false, 50, 50);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionUp(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                             {150, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mDragWindow->consumeMotionUp(ADISPLAY_ID_DEFAULT);
    mFakePolicy->assertDropTargetEquals(mSecondWindow->getToken());
    mWindow->assertNoEvents();
    mSecondWindow->assertNoEvents();
}
TEST_F(InputDispatcherDragTests, MouseDragAndDrop) {
    startDrag(true, AINPUT_SOURCE_MOUSE);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_MOUSE)
                                        .buttonState(AMOTION_EVENT_BUTTON_PRIMARY)
                                        .pointer(PointerBuilder(MOUSE_POINTER_ID,
                                                                AMOTION_EVENT_TOOL_TYPE_MOUSE)
                                                         .x(50)
                                                         .y(50))
                                        .build()))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mDragWindow->consumeMotionMove(ADISPLAY_ID_DEFAULT);
    mWindow->consumeDragEvent(false, 50, 50);
    mSecondWindow->assertNoEvents();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_MOUSE)
                                        .buttonState(AMOTION_EVENT_BUTTON_PRIMARY)
                                        .pointer(PointerBuilder(MOUSE_POINTER_ID,
                                                                AMOTION_EVENT_TOOL_TYPE_MOUSE)
                                                         .x(150)
                                                         .y(50))
                                        .build()))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mDragWindow->consumeMotionMove(ADISPLAY_ID_DEFAULT);
    mWindow->consumeDragEvent(true, 150, 50);
    mSecondWindow->consumeDragEvent(false, 50, 50);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher,
                                MotionEventBuilder(AMOTION_EVENT_ACTION_UP, AINPUT_SOURCE_MOUSE)
                                        .buttonState(0)
                                        .pointer(PointerBuilder(MOUSE_POINTER_ID,
                                                                AMOTION_EVENT_TOOL_TYPE_MOUSE)
                                                         .x(150)
                                                         .y(50))
                                        .build()))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    mDragWindow->consumeMotionUp(ADISPLAY_ID_DEFAULT);
    mFakePolicy->assertDropTargetEquals(mSecondWindow->getToken());
    mWindow->assertNoEvents();
    mSecondWindow->assertNoEvents();
}
class InputDispatcherDropInputFeatureTest : public InputDispatcherTest {};
TEST_F(InputDispatcherDropInputFeatureTest, WindowDropsInput) {
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Test window", ADISPLAY_ID_DEFAULT);
    window->setDropInput(true);
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
    window->setFocusable(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    setFocusedWindow(window);
    window->consumeFocusEvent(true , true );
    NotifyKeyArgs keyArgs = generateKeyArgs(AKEY_EVENT_ACTION_DOWN, ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyKey(&keyArgs);
    window->assertNoEvents();
    NotifyMotionArgs motionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyMotion(&motionArgs);
    window->assertNoEvents();
    window->setDropInput(false);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    keyArgs = generateKeyArgs(AKEY_EVENT_ACTION_UP, ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyKey(&keyArgs);
    window->consumeKeyUp(ADISPLAY_ID_DEFAULT);
    motionArgs = generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                                    ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyMotion(&motionArgs);
    window->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    window->assertNoEvents();
}
TEST_F(InputDispatcherDropInputFeatureTest, ObscuredWindowDropsInput) {
    std::shared_ptr<FakeApplicationHandle> obscuringApplication =
            std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> obscuringWindow =
            sp<FakeWindowHandle>::make(obscuringApplication, mDispatcher, "obscuringWindow",
                                       ADISPLAY_ID_DEFAULT);
    obscuringWindow->setFrame(Rect(0, 0, 50, 50));
    obscuringWindow->setOwnerInfo(111, 111);
    obscuringWindow->setTouchable(false);
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Test window", ADISPLAY_ID_DEFAULT);
    window->setDropInputIfObscured(true);
    window->setOwnerInfo(222, 222);
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
    window->setFocusable(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {obscuringWindow, window}}});
    setFocusedWindow(window);
    window->consumeFocusEvent(true , true );
    NotifyKeyArgs keyArgs = generateKeyArgs(AKEY_EVENT_ACTION_DOWN, ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyKey(&keyArgs);
    window->assertNoEvents();
    NotifyMotionArgs motionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyMotion(&motionArgs);
    window->assertNoEvents();
    window->setDropInputIfObscured(false);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {obscuringWindow, window}}});
    keyArgs = generateKeyArgs(AKEY_EVENT_ACTION_UP, ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyKey(&keyArgs);
    window->consumeKeyUp(ADISPLAY_ID_DEFAULT);
    motionArgs = generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                                    ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyMotion(&motionArgs);
    window->consumeMotionDown(ADISPLAY_ID_DEFAULT, AMOTION_EVENT_FLAG_WINDOW_IS_PARTIALLY_OBSCURED);
    window->assertNoEvents();
}
TEST_F(InputDispatcherDropInputFeatureTest, UnobscuredWindowGetsInput) {
    std::shared_ptr<FakeApplicationHandle> obscuringApplication =
            std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> obscuringWindow =
            sp<FakeWindowHandle>::make(obscuringApplication, mDispatcher, "obscuringWindow",
                                       ADISPLAY_ID_DEFAULT);
    obscuringWindow->setFrame(Rect(0, 0, 50, 50));
    obscuringWindow->setOwnerInfo(111, 111);
    obscuringWindow->setTouchable(false);
    std::shared_ptr<FakeApplicationHandle> application = std::make_shared<FakeApplicationHandle>();
    sp<FakeWindowHandle> window = sp<FakeWindowHandle>::make(application, mDispatcher,
                                                             "Test window", ADISPLAY_ID_DEFAULT);
    window->setDropInputIfObscured(true);
    window->setOwnerInfo(222, 222);
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
    window->setFocusable(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {obscuringWindow, window}}});
    setFocusedWindow(window);
    window->consumeFocusEvent(true , true );
    NotifyKeyArgs keyArgs = generateKeyArgs(AKEY_EVENT_ACTION_DOWN, ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyKey(&keyArgs);
    window->assertNoEvents();
    NotifyMotionArgs motionArgs =
            generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                               ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyMotion(&motionArgs);
    window->assertNoEvents();
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window, obscuringWindow}}});
    keyArgs = generateKeyArgs(AKEY_EVENT_ACTION_UP, ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyKey(&keyArgs);
    window->consumeKeyUp(ADISPLAY_ID_DEFAULT);
    motionArgs = generateMotionArgs(AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                                    ADISPLAY_ID_DEFAULT);
    mDispatcher->notifyMotion(&motionArgs);
    window->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    window->assertNoEvents();
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
        if (mDispatcher->setInTouchMode(InputDispatcher::kDefaultInTouchMode, WINDOW_PID,
                                        WINDOW_UID, true ,
                                        ADISPLAY_ID_DEFAULT)) {
            mWindow->consumeTouchModeEvent(InputDispatcher::kDefaultInTouchMode);
            mSecondWindow->consumeTouchModeEvent(InputDispatcher::kDefaultInTouchMode);
            mThirdWindow->assertNoEvents();
        }
        if (mDispatcher->setInTouchMode(InputDispatcher::kDefaultInTouchMode, SECONDARY_WINDOW_PID,
                                        SECONDARY_WINDOW_UID, true ,
                                        SECOND_DISPLAY_ID)) {
            mWindow->assertNoEvents();
            mSecondWindow->assertNoEvents();
            mThirdWindow->consumeTouchModeEvent(InputDispatcher::kDefaultInTouchMode);
        }
    }
    void changeAndVerifyTouchModeInMainDisplayOnly(bool inTouchMode, int32_t pid, int32_t uid,
                                                   bool hasPermission) {
        ASSERT_TRUE(mDispatcher->setInTouchMode(inTouchMode, pid, uid, hasPermission,
                                                ADISPLAY_ID_DEFAULT));
        mWindow->consumeTouchModeEvent(inTouchMode);
        mSecondWindow->consumeTouchModeEvent(inTouchMode);
        mThirdWindow->assertNoEvents();
    }
};
TEST_F(InputDispatcherTouchModeChangedTests, FocusedWindowCanChangeTouchMode) {
    const WindowInfo& windowInfo = *mWindow->getInfo();
    changeAndVerifyTouchModeInMainDisplayOnly(!InputDispatcher::kDefaultInTouchMode,
                                              windowInfo.ownerPid, windowInfo.ownerUid,
                                              false );
}
TEST_F(InputDispatcherTouchModeChangedTests, NonFocusedWindowOwnerCannotChangeTouchMode) {
    const WindowInfo& windowInfo = *mWindow->getInfo();
    int32_t ownerPid = windowInfo.ownerPid;
    int32_t ownerUid = windowInfo.ownerUid;
    mWindow->setOwnerInfo( -1, -1);
    ASSERT_FALSE(mDispatcher->setInTouchMode(InputDispatcher::kDefaultInTouchMode, ownerPid,
                                             ownerUid, false ,
                                             ADISPLAY_ID_DEFAULT));
    mWindow->assertNoEvents();
    mSecondWindow->assertNoEvents();
}
TEST_F(InputDispatcherTouchModeChangedTests, NonWindowOwnerMayChangeTouchModeOnPermissionGranted) {
    const WindowInfo& windowInfo = *mWindow->getInfo();
    int32_t ownerPid = windowInfo.ownerPid;
    int32_t ownerUid = windowInfo.ownerUid;
    mWindow->setOwnerInfo( -1, -1);
    changeAndVerifyTouchModeInMainDisplayOnly(!InputDispatcher::kDefaultInTouchMode, ownerPid,
                                              ownerUid, true );
}
TEST_F(InputDispatcherTouchModeChangedTests, EventIsNotGeneratedIfNotChangingTouchMode) {
    const WindowInfo& windowInfo = *mWindow->getInfo();
    ASSERT_FALSE(mDispatcher->setInTouchMode(InputDispatcher::kDefaultInTouchMode,
                                             windowInfo.ownerPid, windowInfo.ownerUid,
                                             true , ADISPLAY_ID_DEFAULT));
    mWindow->assertNoEvents();
    mSecondWindow->assertNoEvents();
}
TEST_F(InputDispatcherTouchModeChangedTests, ChangeTouchOnSecondaryDisplayOnly) {
    const WindowInfo& windowInfo = *mThirdWindow->getInfo();
    ASSERT_TRUE(mDispatcher->setInTouchMode(!InputDispatcher::kDefaultInTouchMode,
                                            windowInfo.ownerPid, windowInfo.ownerUid,
                                            true , SECOND_DISPLAY_ID));
    mWindow->assertNoEvents();
    mSecondWindow->assertNoEvents();
    mThirdWindow->consumeTouchModeEvent(!InputDispatcher::kDefaultInTouchMode);
}
TEST_F(InputDispatcherTouchModeChangedTests, CanChangeTouchModeWhenOwningLastInteractedWindow) {
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyDown(mDispatcher, ADISPLAY_ID_DEFAULT))
            << "Inject key event should return InputEventInjectionResult::SUCCEEDED";
    mWindow->consumeKeyDown(ADISPLAY_ID_DEFAULT);
    mWindow->setFocusable(false);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {mWindow}}});
    const WindowInfo& windowInfo = *mWindow->getInfo();
    ASSERT_TRUE(mDispatcher->setInTouchMode(!InputDispatcher::kDefaultInTouchMode,
                                            windowInfo.ownerPid, windowInfo.ownerUid,
                                            false , ADISPLAY_ID_DEFAULT));
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
TEST_F(InputDispatcherSpyWindowDeathTest, UntrustedSpy_AbortsDispatcher) {
    ScopedSilentDeath _silentDeath;
    auto spy = createSpy();
    spy->setTrustedOverlay(false);
    ASSERT_DEATH(mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {spy}}}),
                 ".* not a trusted overlay");
}
TEST_F(InputDispatcherSpyWindowTest, NoForegroundWindow) {
    auto spy = createSpy();
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {spy}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    spy->consumeMotionDown(ADISPLAY_ID_DEFAULT);
}
TEST_F(InputDispatcherSpyWindowTest, ReceivesInputInOrder) {
    auto window = createForeground();
    auto spy1 = createSpy();
    auto spy2 = createSpy();
    auto spy3 = createSpy();
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {spy1, spy2, window, spy3}}});
    const std::vector<sp<FakeWindowHandle>> channels{spy1, spy2, window, spy3};
    const size_t numChannels = channels.size();
    base::unique_fd epollFd(epoll_create1(EPOLL_CLOEXEC));
    if (!epollFd.ok()) {
        FAIL() << "Failed to create epoll fd";
    }
    for (size_t i = 0; i < numChannels; i++) {
        struct epoll_event event = {.events = EPOLLIN, .data.u64 = i};
        if (epoll_ctl(epollFd.get(), EPOLL_CTL_ADD, channels[i]->getChannelFd(), &event) < 0) {
            FAIL() << "Failed to add fd to epoll";
        }
    }
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    std::vector<size_t> eventOrder;
    std::vector<struct epoll_event> events(numChannels);
    for (;;) {
        const int nFds = epoll_wait(epollFd.get(), events.data(), static_cast<int>(numChannels),
                                    (100ms).count());
        if (nFds < 0) {
            FAIL() << "Failed to call epoll_wait";
        }
        if (nFds == 0) {
            break;
        }
        for (int i = 0; i < nFds; i++) {
            ASSERT_EQ(static_cast<uint32_t>(EPOLLIN), events[i].events);
            eventOrder.push_back(static_cast<size_t>(events[i].data.u64));
            channels[i]->consumeMotionDown();
        }
    }
    EXPECT_EQ(3u, eventOrder.size());
    EXPECT_EQ(2u, eventOrder[0]);
    EXPECT_EQ(0u, eventOrder[1]);
    EXPECT_EQ(1u, eventOrder[2]);
}
TEST_F(InputDispatcherSpyWindowTest, NotTouchable) {
    auto window = createForeground();
    auto spy = createSpy();
    spy->setTouchable(false);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {spy, window}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    spy->assertNoEvents();
}
TEST_F(InputDispatcherSpyWindowTest, TouchableRegion) {
    auto window = createForeground();
    auto spy = createSpy();
    spy->setTouchableRegion(Region{{0, 0, 20, 20}});
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {spy, window}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionDown();
    spy->assertNoEvents();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionUp(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionUp();
    spy->assertNoEvents();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {5, 10}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionDown();
    spy->consumeMotionDown();
}
TEST_F(InputDispatcherSpyWindowTest, WatchOutsideTouches) {
    auto window = createForeground();
    window->setOwnerInfo(12, 34);
    auto spy = createSpy();
    spy->setWatchOutsideTouch(true);
    spy->setOwnerInfo(56, 78);
    spy->setFrame(Rect{0, 0, 20, 20});
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {spy, window}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {100, 200}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionDown();
    spy->consumeMotionOutsideWithZeroedCoords();
}
TEST_F(InputDispatcherSpyWindowTest, ReceivesMultiplePointers) {
    auto windowLeft = createForeground();
    windowLeft->setFrame({0, 0, 100, 200});
    auto windowRight = createForeground();
    windowRight->setFrame({100, 0, 200, 200});
    auto spy = createSpy();
    spy->setFrame({0, 0, 200, 200});
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {spy, windowLeft, windowRight}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {50, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    windowLeft->consumeMotionDown();
    spy->consumeMotionDown();
    const MotionEvent secondFingerDownEvent =
            MotionEventBuilder(POINTER_1_DOWN, AINPUT_SOURCE_TOUCHSCREEN)
                    .eventTime(systemTime(SYSTEM_TIME_MONOTONIC))
                    .pointer(PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER).x(50).y(50))
                    .pointer(
                            PointerBuilder( 1, AMOTION_EVENT_TOOL_TYPE_FINGER).x(150).y(50))
                    .build();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, secondFingerDownEvent, INJECT_EVENT_TIMEOUT,
                                InputEventInjectionSync::WAIT_FOR_RESULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    windowRight->consumeMotionDown();
    spy->consumeMotionPointerDown(1 );
}
TEST_F(InputDispatcherSpyWindowTest, ReceivesSecondPointerAsDown) {
    auto window = createForeground();
    window->setFrame({0, 0, 200, 200});
    auto spyRight = createSpy();
    spyRight->setFrame({100, 0, 200, 200});
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {spyRight, window}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {50, 50}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionDown();
    spyRight->assertNoEvents();
    const MotionEvent secondFingerDownEvent =
            MotionEventBuilder(POINTER_1_DOWN, AINPUT_SOURCE_TOUCHSCREEN)
                    .eventTime(systemTime(SYSTEM_TIME_MONOTONIC))
                    .pointer(PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER).x(50).y(50))
                    .pointer(
                            PointerBuilder( 1, AMOTION_EVENT_TOOL_TYPE_FINGER).x(150).y(50))
                    .build();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, secondFingerDownEvent, INJECT_EVENT_TIMEOUT,
                                InputEventInjectionSync::WAIT_FOR_RESULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionPointerDown(1 );
    spyRight->consumeMotionDown();
}
TEST_F(InputDispatcherSpyWindowTest, SplitIfNoForegroundWindowTouched) {
    auto spy = createSpy();
    spy->setPreventSplitting(true);
    auto window = createForeground();
    window->setFrame(Rect(0, 0, 100, 100));
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {spy, window}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {100, 200}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    spy->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    window->assertNoEvents();
    const MotionEvent secondFingerDownEvent =
            MotionEventBuilder(POINTER_1_DOWN, AINPUT_SOURCE_TOUCHSCREEN)
                    .displayId(ADISPLAY_ID_DEFAULT)
                    .eventTime(systemTime(SYSTEM_TIME_MONOTONIC))
                    .pointer(PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER)
                                     .x(100)
                                     .y(200))
                    .pointer(PointerBuilder( 1, AMOTION_EVENT_TOOL_TYPE_FINGER).x(50).y(50))
                    .build();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, secondFingerDownEvent, INJECT_EVENT_TIMEOUT,
                                InputEventInjectionSync::WAIT_FOR_RESULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    spy->consumeMotionPointerDown(1 );
}
TEST_F(InputDispatcherSpyWindowTest, UnfocusableSpyDoesNotReceiveKeyEvents) {
    auto spy = createSpy();
    spy->setFocusable(false);
    auto window = createForeground();
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {spy, window}}});
    setFocusedWindow(window);
    window->consumeFocusEvent(true);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyDown(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeKeyDown(ADISPLAY_ID_NONE);
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyUp(mDispatcher))
            << "Inject key event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeKeyUp(ADISPLAY_ID_NONE);
    spy->assertNoEvents();
}
using InputDispatcherPilferPointersTest = InputDispatcherSpyWindowTest;
TEST_F(InputDispatcherPilferPointersTest, PilferPointers) {
    auto window = createForeground();
    auto spy1 = createSpy();
    auto spy2 = createSpy();
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {spy1, spy2, window}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionDown();
    spy1->consumeMotionDown();
    spy2->consumeMotionDown();
    EXPECT_EQ(OK, mDispatcher->pilferPointers(spy2->getToken()));
    spy2->assertNoEvents();
    spy1->consumeMotionCancel();
    window->consumeMotionCancel();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_MOVE, AINPUT_SOURCE_TOUCHSCREEN,
                                ADISPLAY_ID_DEFAULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    spy2->consumeMotionMove();
    spy1->assertNoEvents();
    window->assertNoEvents();
}
TEST_F(InputDispatcherPilferPointersTest, CanPilferAfterWindowIsRemovedMidStream) {
    auto window = createForeground();
    auto spy = createSpy();
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {spy, window}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    spy->consumeMotionDown(ADISPLAY_ID_DEFAULT);
    window->releaseChannel();
    EXPECT_EQ(OK, mDispatcher->pilferPointers(spy->getToken()));
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionUp(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    spy->consumeMotionUp(ADISPLAY_ID_DEFAULT);
}
TEST_F(InputDispatcherPilferPointersTest, ContinuesToReceiveGestureAfterPilfer) {
    auto spy = createSpy();
    auto window = createForeground();
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {spy, window}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {100, 200}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    spy->consumeMotionDown();
    window->consumeMotionDown();
    EXPECT_EQ(OK, mDispatcher->pilferPointers(spy->getToken()));
    window->consumeMotionCancel();
    const MotionEvent secondFingerDownEvent =
            MotionEventBuilder(POINTER_1_DOWN, AINPUT_SOURCE_TOUCHSCREEN)
                    .displayId(ADISPLAY_ID_DEFAULT)
                    .eventTime(systemTime(SYSTEM_TIME_MONOTONIC))
                    .pointer(PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER)
                                     .x(100)
                                     .y(200))
                    .pointer(PointerBuilder( 1, AMOTION_EVENT_TOOL_TYPE_FINGER).x(50).y(50))
                    .build();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, secondFingerDownEvent, INJECT_EVENT_TIMEOUT,
                                InputEventInjectionSync::WAIT_FOR_RESULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    spy->consumeMotionPointerDown(1 );
    const MotionEvent thirdFingerDownEvent =
            MotionEventBuilder(POINTER_2_DOWN, AINPUT_SOURCE_TOUCHSCREEN)
                    .displayId(ADISPLAY_ID_DEFAULT)
                    .eventTime(systemTime(SYSTEM_TIME_MONOTONIC))
                    .pointer(PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER)
                                     .x(100)
                                     .y(200))
                    .pointer(PointerBuilder( 1, AMOTION_EVENT_TOOL_TYPE_FINGER).x(50).y(50))
                    .pointer(PointerBuilder( 2, AMOTION_EVENT_TOOL_TYPE_FINGER).x(-5).y(-5))
                    .build();
    ASSERT_EQ(InputEventInjectionResult::FAILED,
              injectMotionEvent(mDispatcher, thirdFingerDownEvent, INJECT_EVENT_TIMEOUT,
                                InputEventInjectionSync::WAIT_FOR_RESULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    spy->assertNoEvents();
    window->assertNoEvents();
}
TEST_F(InputDispatcherPilferPointersTest, PartiallyPilferRequiredPointers) {
    auto spy = createSpy();
    spy->setFrame(Rect(0, 0, 100, 100));
    auto window = createForeground();
    window->setFrame(Rect(0, 0, 200, 200));
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {spy, window}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {150, 150}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionDown();
    const MotionEvent secondFingerDownEvent =
            MotionEventBuilder(POINTER_1_DOWN, AINPUT_SOURCE_TOUCHSCREEN)
                    .displayId(ADISPLAY_ID_DEFAULT)
                    .eventTime(systemTime(SYSTEM_TIME_MONOTONIC))
                    .pointer(PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER)
                                     .x(150)
                                     .y(150))
                    .pointer(PointerBuilder( 1, AMOTION_EVENT_TOOL_TYPE_FINGER).x(10).y(10))
                    .build();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, secondFingerDownEvent, INJECT_EVENT_TIMEOUT,
                                InputEventInjectionSync::WAIT_FOR_RESULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    spy->consumeMotionDown();
    window->consumeMotionPointerDown(1);
    const MotionEvent thirdFingerDownEvent =
            MotionEventBuilder(POINTER_2_DOWN, AINPUT_SOURCE_TOUCHSCREEN)
                    .displayId(ADISPLAY_ID_DEFAULT)
                    .eventTime(systemTime(SYSTEM_TIME_MONOTONIC))
                    .pointer(PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER)
                                     .x(150)
                                     .y(150))
                    .pointer(PointerBuilder( 1, AMOTION_EVENT_TOOL_TYPE_FINGER).x(10).y(10))
                    .pointer(PointerBuilder( 2, AMOTION_EVENT_TOOL_TYPE_FINGER).x(50).y(50))
                    .build();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, thirdFingerDownEvent, INJECT_EVENT_TIMEOUT,
                                InputEventInjectionSync::WAIT_FOR_RESULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    spy->consumeMotionPointerDown(1);
    window->consumeMotionPointerDown(2);
    EXPECT_EQ(OK, mDispatcher->pilferPointers(spy->getToken()));
    window->consumeMotionPointerUp( 2, ADISPLAY_ID_DEFAULT, AMOTION_EVENT_FLAG_CANCELED);
    window->consumeMotionPointerUp( 1, ADISPLAY_ID_DEFAULT, AMOTION_EVENT_FLAG_CANCELED);
    spy->assertNoEvents();
    window->assertNoEvents();
}
TEST_F(InputDispatcherPilferPointersTest, PilferAllRequiredPointers) {
    auto spy = createSpy();
    spy->setFrame(Rect(0, 0, 100, 100));
    auto window = createForeground();
    window->setFrame(Rect(0, 0, 200, 200));
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {spy, window}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {10, 10}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionDown();
    spy->consumeMotionDown();
    const MotionEvent secondFingerDownEvent =
            MotionEventBuilder(POINTER_1_DOWN, AINPUT_SOURCE_TOUCHSCREEN)
                    .displayId(ADISPLAY_ID_DEFAULT)
                    .eventTime(systemTime(SYSTEM_TIME_MONOTONIC))
                    .pointer(PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER).x(10).y(10))
                    .pointer(PointerBuilder( 1, AMOTION_EVENT_TOOL_TYPE_FINGER).x(50).y(50))
                    .build();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, secondFingerDownEvent, INJECT_EVENT_TIMEOUT,
                                InputEventInjectionSync::WAIT_FOR_RESULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    spy->consumeMotionPointerDown(1);
    window->consumeMotionPointerDown(1);
    EXPECT_EQ(OK, mDispatcher->pilferPointers(spy->getToken()));
    window->consumeMotionCancel();
    spy->assertNoEvents();
    window->assertNoEvents();
}
TEST_F(InputDispatcherPilferPointersTest, CanReceivePointersAfterPilfer) {
    auto spy = createSpy();
    spy->setFrame(Rect(0, 0, 100, 100));
    auto window = createForeground();
    window->setFrame(Rect(0, 0, 200, 200));
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {spy, window}}});
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionDown(mDispatcher, AINPUT_SOURCE_TOUCHSCREEN, ADISPLAY_ID_DEFAULT,
                               {10, 10}))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionDown();
    spy->consumeMotionDown();
    EXPECT_EQ(OK, mDispatcher->pilferPointers(spy->getToken()));
    window->consumeMotionCancel();
    const MotionEvent secondFingerDownEvent =
            MotionEventBuilder(POINTER_1_DOWN, AINPUT_SOURCE_TOUCHSCREEN)
                    .displayId(ADISPLAY_ID_DEFAULT)
                    .eventTime(systemTime(SYSTEM_TIME_MONOTONIC))
                    .pointer(PointerBuilder( 0, AMOTION_EVENT_TOOL_TYPE_FINGER).x(10).y(10))
                    .pointer(PointerBuilder( 1, AMOTION_EVENT_TOOL_TYPE_FINGER)
                                     .x(150)
                                     .y(150))
                    .build();
    ASSERT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, secondFingerDownEvent, INJECT_EVENT_TIMEOUT,
                                InputEventInjectionSync::WAIT_FOR_RESULT))
            << "Inject motion event should return InputEventInjectionResult::SUCCEEDED";
    window->consumeMotionDown();
    window->assertNoEvents();
    spy->consumeMotionMove();
    spy->assertNoEvents();
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
        window->consumeFocusEvent(true , true );
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
TEST_F(InputDispatcherStylusInterceptorDeathTest, UntrustedOverlay_AbortsDispatcher) {
    ScopedSilentDeath _silentDeath;
    auto [overlay, window] = setupStylusOverlayScenario();
    overlay->setTrustedOverlay(false);
    ASSERT_DEATH(mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {overlay, window}}}),
                 ".* not a trusted overlay");
}
TEST_F(InputDispatcherStylusInterceptorTest, ConsmesOnlyStylusEvents) {
    auto [overlay, window] = setupStylusOverlayScenario();
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {overlay, window}}});
    sendStylusEvent(AMOTION_EVENT_ACTION_DOWN);
    overlay->consumeMotionDown();
    sendStylusEvent(AMOTION_EVENT_ACTION_UP);
    overlay->consumeMotionUp();
    sendFingerEvent(AMOTION_EVENT_ACTION_DOWN);
    window->consumeMotionDown();
    sendFingerEvent(AMOTION_EVENT_ACTION_UP);
    window->consumeMotionUp();
    overlay->assertNoEvents();
    window->assertNoEvents();
}
TEST_F(InputDispatcherStylusInterceptorTest, SpyWindowStylusInterceptor) {
    auto [overlay, window] = setupStylusOverlayScenario();
    overlay->setSpy(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {overlay, window}}});
    sendStylusEvent(AMOTION_EVENT_ACTION_DOWN);
    overlay->consumeMotionDown();
    window->consumeMotionDown();
    sendStylusEvent(AMOTION_EVENT_ACTION_UP);
    overlay->consumeMotionUp();
    window->consumeMotionUp();
    sendFingerEvent(AMOTION_EVENT_ACTION_DOWN);
    window->consumeMotionDown();
    sendFingerEvent(AMOTION_EVENT_ACTION_UP);
    window->consumeMotionUp();
    overlay->assertNoEvents();
    window->assertNoEvents();
}
TEST_F(InputDispatcherStylusInterceptorTest, StylusHandwritingScenario) {
    auto [overlay, window] = setupStylusOverlayScenario();
    overlay->setSpy(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {overlay, window}}});
    sendStylusEvent(AMOTION_EVENT_ACTION_DOWN);
    overlay->consumeMotionDown();
    window->consumeMotionDown();
    EXPECT_EQ(OK, mDispatcher->pilferPointers(overlay->getToken()));
    window->consumeMotionCancel();
    overlay->setSpy(false);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {overlay, window}}});
    sendStylusEvent(AMOTION_EVENT_ACTION_MOVE);
    overlay->consumeMotionMove();
    sendStylusEvent(AMOTION_EVENT_ACTION_UP);
    overlay->consumeMotionUp();
    window->assertNoEvents();
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
        return inputdispatcher::injectKey(mDispatcher, action, 0 , ADISPLAY_ID_NONE,
                                          InputEventInjectionSync::WAIT_FOR_RESULT,
                                          INJECT_EVENT_TIMEOUT, false , {mUid},
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
TEST_F(InputDispatcherTargetedInjectionTest, CanInjectIntoOwnedWindow) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    setFocusedWindow(window);
    window->consumeFocusEvent(true);
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedKey(AKEY_EVENT_ACTION_DOWN));
    window->consumeKeyDown(ADISPLAY_ID_NONE);
}
TEST_F(InputDispatcherTargetedInjectionTest, CannotInjectIntoUnownedWindow) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {window}}});
    auto rando = User(mDispatcher, 20, 21);
    EXPECT_EQ(InputEventInjectionResult::TARGET_MISMATCH,
              rando.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    setFocusedWindow(window);
    window->consumeFocusEvent(true);
    EXPECT_EQ(InputEventInjectionResult::TARGET_MISMATCH,
              rando.injectTargetedKey(AKEY_EVENT_ACTION_DOWN));
    window->assertNoEvents();
}
TEST_F(InputDispatcherTargetedInjectionTest, CanInjectIntoOwnedSpyWindow) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();
    auto spy = owner.createWindow();
    spy->setSpy(true);
    spy->setTrustedOverlay(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {spy, window}}});
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    spy->consumeMotionDown();
    window->consumeMotionDown();
}
TEST_F(InputDispatcherTargetedInjectionTest, CannotInjectIntoUnownedSpyWindow) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();
    auto rando = User(mDispatcher, 20, 21);
    auto randosSpy = rando.createWindow();
    randosSpy->setSpy(true);
    randosSpy->setTrustedOverlay(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosSpy, window}}});
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    randosSpy->assertNoEvents();
    window->consumeMotionDown();
}
TEST_F(InputDispatcherTargetedInjectionTest, CanInjectIntoAnyWindowWhenNotTargeting) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();
    auto rando = User(mDispatcher, 20, 21);
    auto randosSpy = rando.createWindow();
    randosSpy->setSpy(true);
    randosSpy->setTrustedOverlay(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosSpy, window}}});
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              injectMotionEvent(mDispatcher, AMOTION_EVENT_ACTION_DOWN, AINPUT_SOURCE_TOUCHSCREEN,
                                ADISPLAY_ID_DEFAULT));
    randosSpy->consumeMotionDown();
    window->consumeMotionDown();
    setFocusedWindow(randosSpy);
    randosSpy->consumeFocusEvent(true);
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED, injectKeyDown(mDispatcher));
    randosSpy->consumeKeyDown(ADISPLAY_ID_NONE);
    window->assertNoEvents();
}
TEST_F(InputDispatcherTargetedInjectionTest, CanGenerateActionOutsideToOtherUids) {
    auto owner = User(mDispatcher, 10, 11);
    auto window = owner.createWindow();
    auto rando = User(mDispatcher, 20, 21);
    auto randosWindow = rando.createWindow();
    randosWindow->setFrame(Rect{-10, -10, -5, -5});
    randosWindow->setWatchOutsideTouch(true);
    mDispatcher->setInputWindows({{ADISPLAY_ID_DEFAULT, {randosWindow, window}}});
    EXPECT_EQ(InputEventInjectionResult::SUCCEEDED,
              owner.injectTargetedMotion(AMOTION_EVENT_ACTION_DOWN));
    window->consumeMotionDown();
    randosWindow->consumeMotionOutside();
}
}
